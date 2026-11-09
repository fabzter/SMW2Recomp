/* Super Mario World 2: Yoshi's Island — title-side sprite policy.
 *
 * Full-adaptive sprite activation and exact pack metadata for the
 * widescreen side margins (SDD task 3). Address evidence (yi-disasm,
 * all verified against the loaded ROM's opcode shapes before arming):
 *
 *   bank08.asm $08B1EF copy_oam_buffer — packs the 256-entry OAM source
 *     ($0200..$09FF, 8B entries: signed-16 X, signed-16 Y, attr word,
 *     high/flags word) into the low mirror $0A00 (forward) / $0BFC
 *     (reverse), the 128-byte per-slot high buffer $0C20 and the
 *     compressed high mirror $0C00..$0C1F. Native clip X[-15,256),
 *     Y[-15,240); source bit $4000 selects reverse routing; the loop
 *     ends when the two output pointers cross (112 slots max); unpacked
 *     region Y is cleared to $F0; stored packed Y = emit Y - 1. The
 *     CPU-side mirrors are $6A00/$6C00 (physical cart RAM $0A00/$0C00).
 *
 *   bank09.asm $098000 gsu_check_new_sprites — submits unspawned stage
 *     sprite entries whose world-tile coordinates land on the newly
 *     crossed column/row (native 22 columns x 20 rows) into the
 *     31-entry queue at $7027CE (4B per entry, flag table at $7028CA).
 *
 *   bank09.asm $098A18 — GSU edge-despawn class X-threshold readout;
 *     classes 1..3 only (class 0 already bypassed the despawn entirely).
 *
 *   bank03.asm $03A2E8/$03A302 — CPU manual despawn carry decisions
 *     after CMP #$0200 on (x+$80) and CMP #$0180 on (x+$40).
 *
 * Widescreen off (both live margins zero) is the faithful path: no GSU
 * hook is armed, no expanded decision runs, and the published
 * PpuWsOamCoordinates table is left alone for the PPU to keep/clear.
 */

#include "smw2_sprites.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common_cpu_infra.h" /* g_snes, g_snes_cpu */
#include "common_rtl.h"       /* g_ppu, g_ram, RtlStateGeneration */
#include "cpu_state.h"        /* CpuState, cpu_dispatch_has_entry */
#include "snes/cart.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/superfx.h"
#include "types.h"

extern bool g_ws_active;

/* ── Binding geometry (verified: bank08.asm 8788..8943) ──────────────── */
enum {
  kSrcBase = 0x0200, /* OAM source entries, 256 x 8 bytes */
  kSrcCount = 256,
  kSrcStride = 8,
  kCapBytes = 0x0800,     /* capture bound: $0200..$09FF = 2048 bytes */
  kLowFwd = 0x0A40,       /* forward low-mirror write start (slot 16) */
  kLowRev = 0x0BFC,       /* reverse low-mirror write start (slot 127) */
  kHiBase = 0x0C20,       /* per-slot high buffer base (slot index) */
  kHiMirrorBase = 0x0C00, /* compressed high mirror, 32 bytes */
  kLowMirrorBase = 0x0A00,
  kLowMirrorSize = 0x200,
  kFirstSlot = 16,
  kMaxSlot = 128,
  kParkY = 0xF0,
  kNativeXMin = -15,
  kNativeXMax = 256,
  kNativeYMin = -15,
  kNativeYMax = 240,
  kSpawnQueueBase = 0x27CE,
  kSpawnQueueEnd = 0x28CA, /* also the flag table base */
  kSpawnEntrySize = 8,
  kSpawnEntriesMax = 31,
  kSpawnCols = 22,
  kSpawnRows = 20,
};

/* GSU hook PCs (PBR<<16 | offset, superfx.h convention). */
enum {
  kPcPackEntry = 0x08B1EFu,   /* ibt r12 — once per pack run */
  kPcPackXLo = 0x08B219u,     /* cmp r7 (0x3F prefix) — per part */
  kPcPackXHi = 0x08B221u,     /* cmp r8 (0x3F prefix) — per part */
  kPcPackYTest = 0x08B230u,   /* cmp r7 (0x3F prefix) — per part */
  kPcPackFwdDec = 0x08B251u,  /* dec r14 — forward write path */
  kPcPackRevDec = 0x08B263u,  /* dec r14 — reverse write path */
  kPcSpawnDecide = 0x098055u, /* sub r3 — unspawned candidate check */
  kPcDespawnX = 0x098A18u,    /* to r5 — class X threshold latch */
  kPcSpawnSubmit = 0x09806Bu, /* .spawn_sprite redirect target */
  kPcSpawnSkip = 0x09807Cu,   /* .spawn_continue redirect target */
  kPcDespawnOff = 0x098A2Fu,  /* .despawn redirect target */
  kPcDespawnNext = 0x098A1Fu, /* onscreen redirect target (sub r0) */
};

/* 65816 hook PCs (bank03.asm 3680+). */
enum {
  kPcCpuDespawnA = 0x03A2E8u, /* BCS after CMP #$0200 on (x+$80) */
  kPcCpuDespawnB = 0x03A302u, /* BCS after CMP #$0180 on (x+$40) */
  kCpuRoutineA = 0x03A2DEu,   /* containing routine entries for the */
  kCpuRoutineB = 0x03A2F8u,   /* AOT-tier guard */
};

/* ── Part metadata recorded while the pack runs ───────────────────────── */
typedef struct {
  uint8_t admitted; /* X passed the live bounds */
  uint8_t x_vis;    /* admitted AND within the native X clip */
  uint8_t y_vis;    /* source Y within the native clip */
  uint8_t written;  /* reached a low-mirror write before termination */
  uint8_t dir;      /* 0 forward, 1 reverse (word3 bit $4000) */
  int16_t x;        /* true signed source X */
  int16_t y;        /* true signed source Y */
  uint8_t slot;     /* OAM slot written (16..127) */
  uint8_t hib;      /* packed high-buffer byte (low(word3) | x9) */
  uint16_t attr;    /* bytes 3 & 4 */
} Smw2PartMeta;

typedef struct {
  bool valid;
  uint8_t source[kCapBytes];
  Smw2PartMeta part[kSrcCount];
  uint8_t pred_lo[kLowMirrorSize]; /* model of physical $0A00..$0BFF */
  uint8_t pred_hi[32];             /* model of physical $0C00..$0C1F */
  uint8_t covered[kMaxSlot];       /* slot outcome determined this pack */
} Smw2PackCapture;

static Smw2PackCapture s_pending;

/* ── Live state ───────────────────────────────────────────────────────── */
static const Cart *s_cart;
static SuperFx *s_fx;
static int s_left, s_right; /* live margins, px */
static int s_enabled;       /* all gates pass and margins nonzero */
static int s_gsu_armed;
static int s_cpu_registered;
static int s_cpu_inert; /* callbacks exist but must not act */
static uint64_t s_generation;
static uint32_t s_hook_fires; /* diagnostic: expanded decisions taken */
static uint32_t s_redirects;

static void smw2_note(const char *why) {
  /* Rate-limited diagnostic: once per rejection reason per boot. */
  static char seen[256];
  const size_t n = strlen(why);
  if (n >= sizeof(seen) - 2)
    return;
  if (strstr(seen, why))
    return;
  strcat(seen, "|");
  strcat(seen, why);
  fprintf(stderr, "[smw2-sprites] %s\n", why);
}

/* ── Loaded-ROM opcode shape checks ───────────────────────────────────── */
/* Both the CPU's LoROM view and the GSU's low-bank view map
 * (bank & $3F)<<15 | (adr & $7FFF) into the cart image. */
static int smw2_rom8(uint32_t addr24) {
  const Cart *c = s_cart;
  if (!c || !c->rom)
    return -1;
  uint32_t off = (((addr24 >> 16) & 0x3f) << 15) | (addr24 & 0x7fff);
  if (off >= (uint32_t)c->romSize)
    return -1;
  return c->rom[off];
}

static int smw2_rom_ok(uint32_t pc, int opcode) {
  return smw2_rom8(pc) == opcode;
}

/* Seven-byte CPU decision window ending at the BCS: ADC abs16, CMP abs16,
 * BCS (the CLC between ADC and CMP is not shape-checked). The carry the
 * hook rewrites belongs to the CMP; the ADC bias defines the margin math.
 * Verified against the loaded ROM: 03:A2E2 ADC #$0080 / A2E5 CMP #$0200 /
 * A2E8 BCS and 03:A2FC ADC #$0040 / A2FF CMP #$0180 / A302 BCS. */
static int smw2_cpu_shape_ok(uint32_t bcs_pc, uint16_t adc_imm,
                             uint16_t cmp_imm) {
  uint32_t cmp_pc = bcs_pc - 3, adc_pc = bcs_pc - 6;
  return smw2_rom8(adc_pc) == 0x69 &&
         smw2_rom8(adc_pc + 1) == (adc_imm & 0xff) &&
         smw2_rom8(adc_pc + 2) == (adc_imm >> 8) && smw2_rom8(cmp_pc) == 0xc9 &&
         smw2_rom8(cmp_pc + 1) == (cmp_imm & 0xff) &&
         smw2_rom8(cmp_pc + 2) == (cmp_imm >> 8) && smw2_rom8(bcs_pc) == 0xb0;
}

static int smw2_gsu_shapes_ok(void) {
  return smw2_rom_ok(kPcPackEntry, 0xac) && /* ibt r12 */
         smw2_rom_ok(kPcPackXLo, 0x3f) &&   /* cmp r7 prefix */
         smw2_rom_ok(kPcPackXLo + 1, 0x67) &&
         smw2_rom_ok(kPcPackXHi, 0x3f) && /* cmp r8 prefix */
         smw2_rom_ok(kPcPackXHi + 1, 0x68) &&
         smw2_rom_ok(kPcPackYTest, 0x3f) && /* cmp r7 prefix */
         smw2_rom_ok(kPcPackYTest + 1, 0x67) &&
         smw2_rom_ok(kPcPackFwdDec, 0xee) &&  /* dec r14 */
         smw2_rom_ok(kPcPackRevDec, 0xee) &&  /* dec r14 */
         smw2_rom_ok(kPcSpawnDecide, 0x63) && /* sub r3 */
         smw2_rom_ok(kPcDespawnX, 0x15);      /* to r5 */
}

static int smw2_cpu_shapes_ok(void) {
  return smw2_cpu_shape_ok(kPcCpuDespawnA, 0x0080, 0x0200) &&
         smw2_cpu_shape_ok(kPcCpuDespawnB, 0x0040, 0x0180);
}

/* ── Activation gates ─────────────────────────────────────────────────── */
/* AOT bodies bypass interpreter pre-opcode hooks. The two CPU hook PCs
 * must have no compiled entry — and neither may their containing routines,
 * or an interpreter JSR/branch could still bounce into compiled extents
 * that never raise the callback. GSU PC hooks are core-local and always
 * available regardless of the 65816 tier. */
static int smw2_cpu_tier_ok(void) {
  static const uint32_t pcs[] = {kPcCpuDespawnA, kPcCpuDespawnB, kCpuRoutineA,
                                 kCpuRoutineB};
  static const struct {
    uint8_t m, x;
  } mx[] = {{0, 1}, {1, 1}, {0, 0}};
  CpuState probe;
  memset(&probe, 0, sizeof(probe));
  for (unsigned p = 0; p < sizeof(pcs) / sizeof(pcs[0]); p++)
    for (unsigned v = 0; v < sizeof(mx) / sizeof(mx[0]); v++) {
      probe.m_flag = mx[v].m;
      probe.x_flag = mx[v].x;
      if (cpu_dispatch_has_entry(&probe, pcs[p]))
        return 0;
    }
  return 1;
}

/* Gameplay/layout gate: level gameplay game mode $0F (WRAM $0118, the
 * same word the background policy gates on) plus the only OBJ layout this
 * module publishes metadata for — OBSEL size-select mode 0 (8/16px parts
 * where high bit 1 is the size flag). Unknown layouts stay bounded/native. */
static int smw2_gameplay_ok(void) {
  return g_ram[0x0118] == 0x0f && g_ram[0x0119] == 0;
}

static int smw2_layout_ok(void) { return g_ppu && PPU_objSize(g_ppu) == 0; }

static int smw2_active(void) { return s_enabled && !s_cpu_inert; }

/* ── Pack: bounded helpers ────────────────────────────────────────────── */
/* Exact binding rule from the brief. */
static bool visible_part(int x, int width, int left, int right) {
  return x >= -left - (width - 1) && x < 256 + right;
}

/* Per-part horizontal size from its high-table flags (OBSEL mode 0:
 * 8x8 / 16x16 square sizes; high bit 1 selects the large size). */
static int smw2_part_width(const uint8_t *entry) {
  return (entry[6] & 2) ? 16 : 8;
}

static int16_t smw2_src16(const uint8_t *src, unsigned part, unsigned off) {
  const uint8_t *e = src + part * kSrcStride + off;
  return (int16_t)(uint16_t)(e[0] | (e[1] << 8));
}

/* Walk the armed pack with the widened bounds. Mirrors the GSU scan:
 * per part: X admit (per-part width), Y native test, high x9 OR, the
 * $4000 routing bit, forward/reverse writes, post-write crossing, then
 * gap clearing and the four-into-one high-table compression. */
static void smw2_model_pack(void) {
  Smw2PackCapture *p = &s_pending;
  int fwd = kLowFwd, rev = kLowRev;
  memset(p->pred_lo, 0, sizeof(p->pred_lo));
  memset(p->pred_hi, 0, sizeof(p->pred_hi));
  memset(p->covered, 0, sizeof(p->covered));

  for (unsigned i = 0; i < kSrcCount; i++) {
    const uint8_t *e = p->source + i * kSrcStride;
    Smw2PartMeta *m = &p->part[i];
    int x = smw2_src16(p->source, i, 0);
    int y = smw2_src16(p->source, i, 2);
    uint16_t attr = (uint16_t)(e[4] | (e[5] << 8));
    uint16_t word3 = (uint16_t)(e[6] | (e[7] << 8));
    int w = smw2_part_width(e);
    m->x = (int16_t)x;
    m->y = (int16_t)y;
    m->attr = attr;
    m->dir = (word3 & 0x4000) ? 1 : 0;
    m->admitted = visible_part(x, w, s_left, s_right);
    m->x_vis = (x >= kNativeXMin && x < kNativeXMax);
    m->y_vis = (y >= kNativeYMin && y < kNativeYMax);
    m->written = 0;
    m->hib = (uint8_t)((word3 & 0xff) | (x < 0 ? 1 : 0));
    m->slot = 0xff;
    /* $8000 X entries are gsu_init_oam_buffer's inactive markers —
     * ordinary clipped records; live and dead entries interleave and
     * the fixed-256 scan never early-outs on them. */
    if (!m->admitted || !m->y_vis)
      continue;
    if (rev < fwd)
      break; /* crossing after a write ended the scan */
    int slot_addr;
    if (!m->dir) {
      slot_addr = fwd;
      fwd += 4;
    } else {
      slot_addr = rev;
      rev -= 4;
    }
    m->slot = (uint8_t)((slot_addr - kLowMirrorBase) >> 2);
    uint8_t packed_y = m->x_vis ? (uint8_t)((y - 1) & 0xff) : (uint8_t)kParkY;
    unsigned b = (unsigned)(slot_addr - kLowMirrorBase);
    p->pred_lo[b] = (uint8_t)x;
    p->pred_lo[b + 1] = packed_y;
    p->pred_lo[b + 2] = (uint8_t)attr;
    p->pred_lo[b + 3] = (uint8_t)(attr >> 8);
    m->written = 1;
    p->covered[m->slot] = 1;
    if (rev < fwd)
      break; /* post-write crossing: scan terminates */
  }

  /* Gap clearing: unwritten region between the two low-mirror pointers
   * gets Y=$F0 (the X/attr bytes keep stale content — never covered). */
  for (int a = fwd; a <= rev; a += 4)
    p->pred_lo[a - kLowMirrorBase + 1] = (uint8_t)kParkY;

  /* High-buffer compression: mirror byte h packs buffer slots
   * 4h..4h+3, lowest index in the lowest pair. Buffer bytes for slots
   * 0..15 are CPU-owned; only written slots' bytes are modeled. */
  for (unsigned slot = 0; slot < kMaxSlot; slot++) {
    if (!p->covered[slot])
      continue;
    for (unsigned i = 0; i < kSrcCount; i++) {
      const Smw2PartMeta *m = &p->part[i];
      if (m->written && m->slot == slot)
        p->pred_hi[slot >> 2] |= (uint8_t)((m->hib & 3) << ((slot & 3) * 2));
    }
  }
}

/* ── GSU hook bodies ──────────────────────────────────────────────────── */
static void smw2_fx_set(SuperFx *fx, unsigned n, uint16_t v) {
  superfx_set_reg(fx, n, v);
}

static void smw2_hook_pack_entry(SuperFx *fx, uint32_t pc24, void *ctx) {
  (void)pc24;
  (void)ctx;
  /* Fresh capture: the pack owns pending for this run. A pack that dies
   * before producing its writes publishes nothing. */
  memset(&s_pending, 0, sizeof(s_pending));
  for (unsigned a = 0; a < kCapBytes; a++)
    s_pending.source[a] = superfx_ram_peek(fx, (uint16_t)(kSrcBase + a));
  s_pending.valid = true;
}

static void smw2_hook_pack_xlo(SuperFx *fx, uint32_t pc24, void *ctx) {
  (void)pc24;
  (void)ctx;
  if (!s_enabled)
    return;
  s_hook_fires++;
  /* r6 still points at the entry base (r6 += 2 happens after this cmp).
   * Per-part width lives in entry byte 6 bit 1 (OBSEL mode 0 gate). */
  unsigned part = (unsigned)((superfx_reg(fx, 1) - kSrcBase) / kSrcStride);
  int w = 16;
  if (part < kSrcCount)
    w = (superfx_ram_peek(fx, (uint16_t)(kSrcBase + part * kSrcStride + 6)) & 2)
            ? 16
            : 8;
  smw2_fx_set(fx, 7, (uint16_t)(-(s_left + (w - 1)) & 0xffff));
}

static void smw2_hook_pack_xhi(SuperFx *fx, uint32_t pc24, void *ctx) {
  (void)pc24;
  (void)ctx;
  if (!s_enabled)
    return;
  smw2_fx_set(fx, 8, (uint16_t)((256 + s_right) & 0xffff));
}

static void smw2_hook_pack_ytest(SuperFx *fx, uint32_t pc24, void *ctx) {
  (void)pc24;
  (void)ctx;
  if (!s_enabled)
    return;
  /* The Y test shares r7 with the X low bound; restore the native -15. */
  smw2_fx_set(fx, 7, (uint16_t)0xfff1);
}

/* Widened-only parts (outside the native X clip but admitted) must not
 * show a wrapped-center ghost when their metadata is absent or later
 * mismatched: park packed Y at $F0 in the write path. The true
 * coordinates stay in pending metadata for the signed publication. */
static void smw2_hook_pack_dec(SuperFx *fx, uint32_t pc24, void *ctx) {
  (void)pc24;
  (void)ctx;
  if (!s_enabled)
    return;
  int x = (int16_t)superfx_reg(fx, 11);
  if (x < kNativeXMin || x >= kNativeXMax)
    smw2_fx_set(fx, 14, (uint16_t)((kParkY + 1) & 0xffff));
}

static void smw2_hook_spawn(SuperFx *fx, uint32_t pc24, void *ctx) {
  (void)pc24;
  (void)ctx;
  if (!s_enabled)
    return;
  /* Widened rectangle: the native new-column X bound r4 covers the left
   * edge and 22 columns; every widened column is a "new" column, including
   * columns already visible on enable/resize. Pixel margins convert to
   * tiles conservatively (ceil(px/16)) so no visible column is missed. */
  int lt = (s_left + 15) >> 4;
  int rt = (s_right + 15) >> 4;
  int x = (int16_t)superfx_reg(fx, 11); /* world-tile X */
  int y = (int16_t)superfx_reg(fx, 12); /* world-tile Y */
  int left = (int16_t)superfx_reg(fx, 4);
  int top = (int16_t)superfx_reg(fx, 2);
  int in_range = (y >= top && y < top + kSpawnRows && x >= left - lt &&
                  x < left + kSpawnCols + rt);
  if (!in_range) {
    superfx_hook_redirect(fx, (uint16_t)kPcSpawnSkip);
    s_redirects++;
    return;
  }
  /* Queue bound, decided in widened C integer arithmetic BEFORE submit:
   * one more 8-byte entry plus the $FFFF terminator must not reach the
   * flag table at $28CA. uint16 wrap is not allowed. */
  if ((int)superfx_reg(fx, 10) + kSpawnEntrySize + 2 > kSpawnQueueEnd) {
    superfx_hook_redirect(fx, (uint16_t)kPcSpawnSkip);
    s_redirects++;
    return;
  }
  superfx_hook_redirect(fx, (uint16_t)kPcSpawnSubmit);
  s_redirects++;
}

static void smw2_hook_despawn(SuperFx *fx, uint32_t pc24, void *ctx) {
  (void)pc24;
  (void)ctx;
  if (!s_enabled)
    return;
  /* r0 = class X threshold T (getbh result), r11 = $00F0, r7 = relX.
   * Verified latch semantics: to r5; add r7 -> r5 = u16(T + relX);
   * add r0; add r11 -> r0 = 2T + $F0; sub r5 -> z = r0 - r5, carry iff
   * z >= 0. Onscreen iff u16(relX + T) <= 2T + $F0 — the u16 wrap of the
   * T + relX sum supplies the symmetric -T left edge. The widened form
   * shifts that window by the live margins: onscreen iff
   * u16(relX + T + L) <= 2T + $F0 + L + R, i.e. relX stays inside
   * [-(T + L), $F0 + T + R]. */
  int t = (int16_t)superfx_reg(fx, 0);
  int relx = (int16_t)superfx_reg(fx, 7);
  uint32_t span = (uint32_t)(0xf0 + 2 * t + s_left + s_right);
  int onscreen = (uint16_t)(relx + t + s_left) <= span;
  if (onscreen) {
    superfx_hook_redirect(fx, (uint16_t)kPcDespawnNext);
  } else {
    /* .despawn stores $0000 to the sprite state word via stw (r6). */
    smw2_fx_set(fx, 0, 0);
    superfx_hook_redirect(fx, (uint16_t)kPcDespawnOff);
  }
  s_redirects++;
}

/* ── Composed 65816 pre-opcode callbacks (persistent, inert when off) ── */
/* Only the carry decision of the X branch is replaced; A/X/Y/stack, all
 * non-target flags, the Y checks and the camera-event logic run natively.
 * A = live operand (x + bias); the native decision is
 * (u16)(A) >= cmp_imm, and the widened range replaces the native
 * [bias-cmp, bias+cmp) window with the margin-shifted one:
 * (u16)(A + L) >= cmp_imm + L + R. The u16 wrap of A + L keeps the left
 * margin checked like the native bias carries it. */
static void smw2_cpu_carry(CpuState *cpu, uint16_t cmp_imm) {
  uint32_t bound = (uint32_t)cmp_imm + (uint32_t)s_left + (uint32_t)s_right;
  int c = (uint16_t)(cpu->A + s_left) >= bound;
  cpu->_flag_C = (uint8)c;
  if (c)
    cpu->P |= 0x01;
  else
    cpu->P = (uint8)(cpu->P & ~0x01);
}

static void smw2_cpu_despawn_a(CpuState *cpu, uint32_t pc24) {
  (void)pc24;
  if (!smw2_active())
    return;
  s_hook_fires++;
  smw2_cpu_carry(cpu, 0x0200);
}

static void smw2_cpu_despawn_b(CpuState *cpu, uint32_t pc24) {
  (void)pc24;
  if (!smw2_active())
    return;
  s_hook_fires++;
  smw2_cpu_carry(cpu, 0x0180);
}

static void smw2_register_cpu_hooks(void) {
  if (s_cpu_registered)
    return;
  if (!interp_bridge_add_pre_opcode_hook(kPcCpuDespawnA, smw2_cpu_despawn_a) ||
      !interp_bridge_add_pre_opcode_hook(kPcCpuDespawnB, smw2_cpu_despawn_b)) {
    smw2_note("cpu hook composition unavailable; cpu widening off");
    s_cpu_inert = 1;
    return;
  }
  s_cpu_registered = 1;
}

/* ── GSU hook arming ──────────────────────────────────────────────────── */
static void smw2_disarm_gsu(void) {
  if (!s_gsu_armed || !s_fx)
    return;
  static const uint32_t pcs[] = {kPcPackEntry,   kPcPackXLo,    kPcPackXHi,
                                 kPcPackYTest,   kPcPackFwdDec, kPcPackRevDec,
                                 kPcSpawnDecide, kPcDespawnX};
  for (unsigned i = 0; i < sizeof(pcs) / sizeof(pcs[0]); i++)
    superfx_set_pc_hook(s_fx, pcs[i], NULL, NULL); /* own slots only */
  s_gsu_armed = 0;
}

static int smw2_arm_gsu(SuperFx *fx) {
  return superfx_set_pc_hook(fx, kPcPackEntry, smw2_hook_pack_entry, NULL) &&
         superfx_set_pc_hook(fx, kPcPackXLo, smw2_hook_pack_xlo, NULL) &&
         superfx_set_pc_hook(fx, kPcPackXHi, smw2_hook_pack_xhi, NULL) &&
         superfx_set_pc_hook(fx, kPcPackYTest, smw2_hook_pack_ytest, NULL) &&
         superfx_set_pc_hook(fx, kPcPackFwdDec, smw2_hook_pack_dec, NULL) &&
         superfx_set_pc_hook(fx, kPcPackRevDec, smw2_hook_pack_dec, NULL) &&
         superfx_set_pc_hook(fx, kPcSpawnDecide, smw2_hook_spawn, NULL) &&
         superfx_set_pc_hook(fx, kPcDespawnX, smw2_hook_despawn, NULL);
}

/* ── BeginFrame publication ───────────────────────────────────────────── */
static int smw2_high_pair(const uint8_t *hi, unsigned slot) {
  return (hi[slot >> 2] >> ((slot & 3) * 2)) & 3;
}

void Smw2SpritesBeforeFrame(int left_extra, int right_extra) {
  s_left = left_extra < 0 ? 0 : left_extra;
  s_right = right_extra < 0 ? 0 : right_extra;
  smw2_register_cpu_hooks(); /* persistent, inert until armed */

  const Cart *cart = (g_snes && g_snes->cart) ? g_snes->cart : NULL;
  SuperFx *fx = (cart && cart->type == CART_SUPERFX) ? cart->superfx : NULL;

  /* Cart or core replacement: the old core's hooks die with it; nothing
   * may be disarmed on a freed core and pending is not a new capture. */
  if (cart != s_cart || fx != s_fx) {
    s_gsu_armed = 0; /* hooks (if any) belonged to the retired core */
    s_cart = cart;
    s_fx = fx;
    s_pending.valid = false;
  }

  int enabled = (s_left | s_right) && s_cart && s_fx;
  if (enabled && s_cart->type != CART_SUPERFX)
    enabled = 0;
  if (enabled && !smw2_gameplay_ok())
    enabled = 0;
  if (enabled && !smw2_layout_ok()) {
    smw2_note("unsupported OBJ layout; staying native");
    enabled = 0;
  }
  if (enabled && !smw2_gsu_shapes_ok()) {
    smw2_note("GSU hook shapes unsupported; staying native");
    enabled = 0;
  }
  if (enabled && !smw2_cpu_shapes_ok()) {
    smw2_note("CPU hook shapes unsupported; cpu widening off");
    s_cpu_inert = 1;
  } else if (enabled && !smw2_cpu_tier_ok()) {
    /* Concrete AOT guard: a compiled extent covering a hook PC would
     * never raise the callback. Report it; don't assume it fired. */
    smw2_note("CPU hook PC under AOT coverage; cpu widening off");
    s_cpu_inert = 1;
  }

  if (!enabled) {
    smw2_disarm_gsu();
    s_enabled = 0;
    return;
  }
  s_enabled = 1;
  /* Re-arm unconditionally each frame. set_pc_hook is idempotent on a
   * pc24 that already carries our callback (it updates in place, no
   * growth), so this is cheap on the steady path — and it is the only
   * reliable resurrection guard: a destroy+create at the SAME address
   * presents an identical (cart, fx) pair yet holds no hooks. Pointer
   * identity cannot detect that; re-arming can. */
  if (s_fx) {
    if (!smw2_arm_gsu(s_fx)) {
      smw2_note("GSU hook arming failed; staying native");
      s_enabled = 0;
      s_gsu_armed = 0;
      return;
    }
    s_gsu_armed = 1;
  }
}

void Smw2SpritesBeginFrame(void) {
  /* A completed capture is consumed exactly once; absence of a fresh
   * capture leaves the last validated (or residue-restored) publication
   * alone. */
  if (!s_pending.valid)
    return;
  s_pending.valid = false;
  if (!s_enabled || !g_ppu || !s_cart || !s_cart->ram)
    return;

  /* Rebuild the armed pack's byte-precise output from the captured
   * source; publication trusts a slot only where model and both
   * authoritative post-pack images agree. */
  smw2_model_pack();

  const uint8_t *lo = s_cart->ram + kLowMirrorBase;
  const uint8_t *hi = s_cart->ram + kHiMirrorBase;
  PpuWsOamCoordinates out;
  memset(&out, 0, sizeof(out));
  for (unsigned i = 0; i < 32; i++)
    out.expectedHighOam[i] = hi[i];
  for (unsigned i = 0; i < 256; i++)
    out.expectedOam[i] = (uint16_t)(lo[2 * i] | (lo[2 * i + 1] << 8));

  unsigned published = 0;
  for (unsigned slot = kFirstSlot; slot < kMaxSlot; slot++) {
    const Smw2PartMeta *m = NULL;
    for (unsigned i = 0; i < kSrcCount; i++)
      if (s_pending.part[i].written && s_pending.part[i].slot == slot) {
        m = &s_pending.part[i];
        break;
      }
    if (!m)
      continue; /* stale/never-written slot: native decode stands */
    unsigned b = slot * 4;
    /* The model must have produced identical bytes for this slot in both
     * the cart mirror and the live OAM before its position is trusted. */
    uint8_t model_hi =
        (uint8_t)((s_pending.pred_hi[slot >> 2] >> ((slot & 3) * 2)) & 3);
    int match =
        lo[b] == s_pending.pred_lo[b] &&
        lo[b + 1] == s_pending.pred_lo[b + 1] &&
        lo[b + 2] == s_pending.pred_lo[b + 2] &&
        lo[b + 3] == s_pending.pred_lo[b + 3] &&
        smw2_high_pair(hi, slot) == model_hi &&
        g_ppu->oam[slot * 2] == out.expectedOam[slot * 2] &&
        g_ppu->oam[slot * 2 + 1] == out.expectedOam[slot * 2 + 1] &&
        smw2_high_pair(g_ppu->highOam, slot) == smw2_high_pair(hi, slot);
    if (!match)
      continue; /* fail-closed: this slot decodes natively */
    /* Signed position: true world X and the true packed Y (real emitted
     * Y minus one). A parked part's true coords are published too — the
     * parked mirror byte is only the fail-closed native fallback. */
    out.x[slot] = m->x;
    out.y[slot] = (int16_t)((m->y - 1) & 0xff);
    out.valid[slot >> 3] |= (uint8_t)(1u << (slot & 7));
    published++;
  }
  PpuWsSetOamCoordinates(g_ppu, &out);
  s_generation = RtlStateGeneration();
  (void)published;
}

void Smw2SpritesReset(void) {
  /* Pending capture ownership belongs to the abandoned scene. Published
   * coordinates likewise end here: the new timeline re-derives them.
   * Other hook owners' registrations are untouched. */
  s_pending.valid = false;
  memset(&s_pending, 0, sizeof(s_pending));
  if (g_ppu)
    PpuWsSetOamCoordinates(g_ppu, NULL);
  s_generation = 0;
}

void Smw2SpritesShutdown(void) {
  smw2_disarm_gsu();
  s_cart = NULL;
  s_fx = NULL;
  s_enabled = 0;
  s_pending.valid = false;
}
