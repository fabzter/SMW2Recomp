/* Task-3 sprite policy tests (src/smw2_sprites.c).
 *
 * Fixtures are ORIGINAL synthetic programs written against the core's
 * verified instruction semantics, placed at the production hook PCs so
 * the module's armed pc-hooks fire. Only the narrow supported-ROM
 * signature bytes required by the shape guards coincide with retail;
 * no ROM stream bytes are reproduced (see each fixture's comment). */
#include "../src/smw2_sprites.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/superfx.h"
#include "types.h"

/* ── Host scaffolding the linked framework TUs need ───────────────────── */
uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
Snes *g_snes;
Ppu *g_ppu;
bool g_ws_active;
int g_ws_extra;
bool g_fail;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
static uint64_t s_test_generation = 1;
uint64_t RtlStateGeneration(void) { return s_test_generation; }

/* Test-controlled AOT coverage table: nonzero pc24 marks compiled code. */
static uint32_t s_aot_pc[8];
int cpu_dispatch_has_entry(CpuState *cpu, uint32_t pc24) {
  (void)cpu;
  for (unsigned i = 0; i < 8; i++)
    if (s_aot_pc[i] && (s_aot_pc[i] & 0x7FFFFFu) == (pc24 & 0x7FFFFFu))
      return 1;
  return 0;
}

/* ppu.c's WsShadow / misc externs (same stub set the ppu tests link). */
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t scroll,
                      uint16_t address, uint16_t tile) {
  (void)layer;
  (void)x;
  (void)y;
  (void)scroll;
  (void)address;
  return tile;
}
bool WsShadowLayerActive(int layer) {
  (void)layer;
  return false;
}
uint32_t WsShadowWorldX(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowWorldY(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowScrollX(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowScrollY(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowPresentWorldY(int layer, int x) {
  (void)layer;
  (void)x;
  return 0;
}
void WsShadowOnVramWrite(uint16_t address, uint16_t value) {
  (void)address;
  (void)value;
}

/* ── Synthetic machine ────────────────────────────────────────────────── */
#define ROM_SIZE 0x100000u
static uint8_t t_rom[ROM_SIZE];
static uint8_t t_gsu_ram[0x10000];
static Snes t_snes;
static Cart t_cart;
static Ppu t_ppu;
static CpuState t_cpu;
static SuperFx *s_fx;
static uint64_t s_master;

/* LoROM map shared by the CPU bus stub, the GSU core and the module's
 * shape-verification byte reads. */
static uint32_t lorom(uint32_t a24) {
  return (((a24 >> 16) & 0x3f) << 15) | (a24 & 0x7fff);
}
static uint8_t rom8(uint32_t a24) { return t_rom[lorom(a24) & (ROM_SIZE - 1)]; }

uint8 *RomPtr(uint32_t addr) { return &t_rom[lorom(addr) & (ROM_SIZE - 1)]; }
uint8_t *cart_getRomPtr(Cart *cart, uint8_t bank, uint16_t adr) {
  (void)cart;
  return RomPtr(((uint32_t)bank << 16) | adr);
}
uint8_t ReadRegOpenBus(uint16_t adr, uint8_t open_bus) {
  (void)adr;
  return open_bus;
}
uint8_t cart_read(Cart *cart, uint8_t bank, uint16_t adr) {
  if (bank == 0x70 || bank == 0x71)
    return cart->ram[adr & (cart->ramSize - 1)];
  if (adr >= 0x6000 && adr < 0x8000 &&
      (bank < 0x40 || (bank >= 0x80 && bank < 0xc0)))
    return cart->ram[(adr - 0x6000) & (cart->ramSize - 1)];
  return rom8(((uint32_t)bank << 16) | adr);
}
void cart_write(Cart *cart, uint8_t bank, uint16_t adr, uint8_t val) {
  if (bank == 0x70 || bank == 0x71) {
    cart->ram[adr & (cart->ramSize - 1)] = val;
    return;
  }
  if (adr >= 0x6000 && adr < 0x8000 &&
      (bank < 0x40 || (bank >= 0x80 && bank < 0xc0))) {
    cart->ram[(adr - 0x6000) & (cart->ramSize - 1)] = val;
    return;
  }
}
uint8_t cpu_read8(CpuState *cpu, uint8_t bank, uint16_t addr) {
  (void)cpu;
  if (addr < 0x2000 && (bank < 0x40 || bank >= 0x80))
    return g_ram[addr];
  if (bank == 0x7e || bank == 0x7f)
    return g_ram[((bank & 1) << 16) | addr];
  return cart_read(g_snes->cart, bank, addr);
}
uint16_t cpu_read16(CpuState *cpu, uint8_t bank, uint16_t addr) {
  return (uint16_t)(cpu_read8(cpu, bank, addr) |
                    (cpu_read8(cpu, bank, (uint16_t)(addr + 1)) << 8));
}
void cpu_write8(CpuState *cpu, uint8_t bank, uint16_t addr, uint8_t v) {
  (void)cpu;
  if (addr < 0x2000 && (bank < 0x40 || bank >= 0x80)) {
    g_ram[addr] = v;
    return;
  }
  if (bank == 0x7e || bank == 0x7f) {
    g_ram[((bank & 1) << 16) | addr] = v;
    return;
  }
  cart_write(g_snes->cart, bank, addr, v);
}
void cpu_write16(CpuState *cpu, uint8_t bank, uint16_t addr, uint16_t v) {
  cpu_write8(cpu, bank, addr, (uint8_t)v);
  cpu_write8(cpu, bank, (uint16_t)(addr + 1), (uint8_t)(v >> 8));
}
uint8_t cpu_read8_paced(CpuState *cpu, uint8_t bank, uint16_t addr) {
  cpu->master_cycles += 8;
  return cpu_read8(cpu, bank, addr);
}
uint16_t cpu_read16_paced(CpuState *cpu, uint8_t bank, uint16_t addr) {
  cpu->master_cycles += 8;
  return cpu_read16(cpu, bank, addr);
}
void cpu_write8_paced(CpuState *cpu, uint8_t bank, uint16_t addr, uint8_t v) {
  cpu->master_cycles += 8;
  cpu_write8(cpu, bank, addr, v);
}
void cpu_write16_paced(CpuState *cpu, uint8_t bank, uint16_t addr, uint16_t v) {
  cpu->master_cycles += 8;
  cpu_write16(cpu, bank, addr, v);
}
uint32_t cpu_region_speed(uint32_t adr) {
  (void)adr;
  return 8;
}
void cpu_state_init(CpuState *cpu, uint8 *ram) {
  memset(cpu, 0, sizeof(*cpu));
  cpu->ram = ram;
  cpu->emulation = 1;
  cpu->P = 0x34;
  cpu->S = 0x01FF;
  cpu_p_to_mirrors(cpu);
}

/* interp_bridge / interp816 scaffolding: where the real runner has
 * heavyweight subsystems (APU lockstep, tier-2 capture, dispatch
 * manifest) the test installs faithful no-ops; nothing in the despawn
 * routines touches them. */
const char *g_last_recomp_func;
void RecompStackPush(const char *name) { (void)name; }
void RecompStackPushInterpreter(const char *name) { (void)name; }
void RecompStackPop(void) {}
void RecompStackPopYield(void) {}
void boundary_audit_record_entry(const char *name) { (void)name; }
void boundary_audit_record_exit(void) {}
void ppudma_note_interrupt(int is_nmi) { (void)is_nmi; }
void cpu_interrupt_context_enter(void) {}
void cpu_interrupt_context_leave(void) {}
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void rtl_sync_apu_to_cpu_locked(void) {}
bool rtl_apu_extended_frame_timing(void) { return false; }
bool rtl_apu_frame_timeline_active(void) { return false; }
void RtlGuestScopeCheckpoint(void) {}
int g_interp_apu_driving;
uint64_t g_apu_last_sync_master;
uint64_t g_main_cpu_cycles_estimate;
uint64_t g_apu_pace_cycles_estimate;
uint8_t g_memsel;
double kInterpApuPerMaster = 0.0;
uint32_t cpu_interrupt_masked_master(void) { return UINT32_MAX; }
uint32_t g_lle_master_deadline;
void tier2_record(uint32_t site, uint32_t target, uint8_t mx, int kind,
                  int outcome) {
  (void)site;
  (void)target;
  (void)mx;
  (void)kind;
  (void)outcome;
}
void tier2_discover(uint32_t site, uint32_t target, uint8_t mx, int kind,
                    int32_t ret) {
  (void)site;
  (void)target;
  (void)mx;
  (void)kind;
  (void)ret;
}
uint8_t tier2_entry_mx(const CpuState *cpu) {
  return (uint8_t)((cpu->m_flag << 1) | cpu->x_flag);
}
void interp_tier_note(uint32_t pc24) { (void)pc24; }
void interp_edge_note(uint32_t f, uint32_t t, int k, uint16_t s) {
  (void)f;
  (void)t;
  (void)k;
  (void)s;
}
int lle_bounce_target_excluded(uint32_t pc) {
  (void)pc;
  return 0;
}
RecompReturn interp_bridge_lle_yield_unwind(CpuState *cpu, uint32_t pc) {
  (void)cpu;
  (void)pc;
  return 0;
}
void lle_resume_set(uint32_t pc, int site, uint16_t sp) {
  (void)pc;
  (void)site;
  (void)sp;
}
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site,
                                             uint16_t entry_s, uint8_t hrv) {
  (void)cpu;
  (void)site;
  (void)entry_s;
  (void)hrv;
  return 0;
}
RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc,
                                  uint8 frame_size) {
  (void)cpu;
  (void)pc;
  (void)frame_size;
  return 0;
}
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc,
                                  uint16 entry_s_for_miss_restore,
                                  uint32 source_pc24) {
  (void)cpu;
  (void)pc;
  (void)entry_s_for_miss_restore;
  (void)source_pc24;
  return 0;
}
uint8_t cpu_dispatch_inline_arg_bytes(uint32_t pc24) {
  (void)pc24;
  return 0;
}
uint8_t sa1_cpu_irq_pending(Sa1 *sa1) {
  (void)sa1;
  return 0;
}
void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) {
  (void)cart;
  (void)master_clock;
}
void cart_note_cpu_bus(Cart *cart, uint8_t bank, uint16_t address) {
  (void)cart;
  (void)bank;
  (void)address;
}
void snes_sync_master_clock(Snes *snes, uint64_t master) {
  (void)snes;
  (void)master;
}
void snes_refresh_exempt(void) {}
void snes_refresh_charge(void) {}
void cpu_trace_offrails(const char *kind, uint32_t addr) {
  (void)kind;
  (void)addr;
}
void interp_bus_timing_note(uint32_t a, uint32_t b) {
  (void)a;
  (void)b;
}
void interp_port_probe_write(uint16_t a, uint8_t v) {
  (void)a;
  (void)v;
}
void interp_port_probe_read(uint16_t a) { (void)a; }
int wlog_scope_available(void) { return 0; }
void wlog_scope_enter(const char *tag) { (void)tag; }
void wlog_scope_exit(void) {}
void wlog_addr_note_direct(uint32_t wa, uint8_t v, const char *via) {
  (void)wa;
  (void)v;
  (void)via;
}
uint32_t g_interp_wlog_pc24;
int g_wlog_active;
int g_wlog_configured;
void (*g_interp_recent_dump_hook)(int n, FILE *out);
void (*g_interp_bridge_pc_hook)(uint32_t pc24, int m_flag, int x_flag);
void (*g_interp_bridge_bounce_hook)(uint32_t pc24, int m_flag, int x_flag);
uint32_t g_interp816_cur_pc;
void (*g_stage_window_store_hook)(void);
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y) {
  (void)pc;
  (void)a;
  (void)x;
  (void)y;
}
const char *rtl_game_title(void) { return "smw2_sprite_test"; }
CpuState g_cpu;
int g_recomp_stack_top;
uint16_t g_cpu_entry_s[64];
uint8_t sdd1_read(Sdd1 *sdd1, uint16_t addr) {
  (void)sdd1;
  (void)addr;
  return 0;
}
void snes_catchupApu(Snes *snes) { (void)snes; }

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/* ── Fixture emitter ──────────────────────────────────────────────────── */
static void emit(uint32_t pc24, const uint8_t *bytes, size_t n) {
  memcpy(&t_rom[lorom(pc24)], bytes, n);
}
static void emit_str(uint32_t pc24, const char *s) {
  /* Whitespace-separated hex-byte listing into the ROM image. */
  uint32_t off = lorom(pc24);
  while (*s) {
    while (*s == ' ' || *s == '\n')
      s++;
    if (!*s)
      break;
    t_rom[off++] = (uint8_t)strtol(s, (char **)&s, 16);
  }
}

/* GSU source entry: signed-16 X, signed-16 Y, attr word, high word. */
static void src_entry(unsigned part, int16_t x, int16_t y, uint16_t attr,
                      uint16_t word3) {
  uint8_t *e = &t_gsu_ram[0x200 + part * 8];
  e[0] = (uint8_t)x;
  e[1] = (uint8_t)(x >> 8);
  e[2] = (uint8_t)y;
  e[3] = (uint8_t)(y >> 8);
  e[4] = (uint8_t)attr;
  e[5] = (uint8_t)(attr >> 8);
  e[6] = (uint8_t)word3;
  e[7] = (uint8_t)(word3 >> 8);
}

static void fixture_pack(void) {
  /* ORIGINAL synthetic GSU pack program at the production hook PCs. Not a
   * ROM copy: structure, register roles and sequencing were authored for
   * this suite. Only the 6 supported-ROM signature bytes below coincide
   * with the retail stream (required by smw2_gsu_shapes_ok): AC @B1EF,
   * 3F 67 @B219, 3F 68 @B221, 3F 67 @B230, EE @B251, EE @B263.
   * Registers: r1 src cursor, r2 fwd ptr ($0A40), r3 rev ptr ($0BFC),
   * r6 hib byte, r11 X, r12 loop count, r13 loop top, r14 packed Y+1.
   * The B251 dec is shared by both paths (park hook covers both); B263
   * is an idempotent second dec on the forward stream (shape site). */
  emit_str(0x08B1EF,
                     "AC 00 FC 00 01 F1 00 02 " /* B1EF entry hook r12=256 r1=src */
                     "F2 40 0A F3 FC 0B FA A0 " /* B1F7 r2=fwd r3=rev r10=hibbias */
                     "09 F5 FF 00 F4 20 0C A8 " /* B1FF r5=255 r4=scratch r8=64 zero n */
                     "40 B0 34 D4 D4 E8 08 F9 " /* B207 stw(r4)=0 r4+=2 dec r8 bne disp->zero=$B208 */
                     "01 A4 08 FD 16 B2 02 2B " /* B20F bne delay r4=8 stride (hook-safe) r13=LOOPTOP cache with r11;ldw(r1) X */
                     "41 41 3F 67 0D 0D 01 01 " /* B217 ldw(r1)->r0 X XLO site cmp r7 bcs admit disp->WRITE=$B22A bcs delay p */
                     "01 01 3F 68 0C 05 01 01 " /* B21F p p XHI site cmp r8 bcc admit disp->WRITE=$B22A bcc delay p */
                     "05 74 01 B1 3E 52 40 3E " /* B227 bra clip disp->CLIP=$B29D bra delay &Y r0=Y Y+15 */
                     "5F 3F 67 0D 69 01 3F 65 " /* B22F YTEST site bcs clip disp->CLIP=$B29D bcs delay cmp255 */
                     "0D 64 B3 3F 62 0C 63 B1 " /* B237 bcs clip disp->CLIP=$B29D cross pre r3-r2 bcc done disp->PACKDONE=$B2A1 &Y */
                     "3E 52 2E 40 3E 54 40 16 " /* B23F with r14;ldw(r0) Y r0=&w3 r0=w3 to r6;lob w3lo */
                     "9E BB C0 0A 05 26 3E C1 " /* B247 r0=X>>8, S=sign bpl skip or disp->DEC1=$B251 with r6 delay r6|=1 sign */
                     "01 01 EE B1 3E 56 40 04 " /* B24F p p DEC site shared &w3->r0 r0=w3 rol rol CY=w3bit14 */
                     "04 0D 1E 01 BB 3D 32 D2 " /* B257 bcs rev disp->REV=$B278 bcs delay stb X r2++ */
                     "BE 3D 32 D2 EE B1 3E 54 " /* B25F stb Y r2++ DEC site fwd attr->r9 */
                     "29 40 29 32 D2 D2 B2 03 " /* B267 with r9;stw(r2) r2+=2 hibaddr */
                     "03 E0 5A 26 3D 30 05 20 " /* B26F with r6;stb(r0) bra disp->TAIL=$B297 */
                     "01 BB 3D 33 D3 BE 3D 33 " /* B277 bra delay stb X r3++ stb Y */
                     "D3 B1 3E 54 29 40 29 33 " /* B27F r3++ attr->r9 with r9;stw(r3) */
                     "B3 03 03 5A 26 3D 30 E3 " /* B287 hibaddr with r6;stb(r0) r3-=6 */
                     "E3 E3 E3 E3 E3 05 01 01 " /* B28F bra disp->TAIL=$B297 bra delay */
                     "B3 3F 62 0C 05 01 21 54 " /* B297 cross post r3-r2 bcc done disp->PACKDONE=$B2A1 bcc delay with r1;add r4 +=8 */
                     "3C 01 A0 F0 D2 3D 32 D2 " /* B29F loop loop delay r0=F0 r2++ ->Y stb F0@Y r2+=3 next slot */
                     "D2 D2 B2 3F 63 0D 04 01 " /* B2A7 cmp r2-r3 bcs done r2>=r3 disp->GAPDONE=$B2B2 bcs delay */
                     "05 F2 01 F2 00 0A F3 40 " /* B2AF bra disp->GAP=$B2A3 bra delay r2=low base r3=fwd base */
                     "0A D2 3D 32 D2 D2 D2 B2 " /* B2B7 r2++ ->Y stb F0@Y r2+=3 next slot cmp r2-r3 */
                     "3F 63 0D 04 01 05 F2 01 " /* B2BF bcs done r2>=r3 disp->GAPLODONE=$B2C7 bcs delay bra disp->GAPLO=$B2B8 bra delay */
                     "F1 30 0C F2 04 0C A3 1C " /* B2C7 r1=scratch+16 r2=out+4 r3=28 */
                     "A4 00 3D 41 D1 3E 73 24 " /* B2CF r4=0 ldb p0 r1++ and#3 r4+=r0 */
                     "50 3D 41 D1 3E 73 50 50 " /* B2D7 ldb p1 r1++ and#3 <<1 <<1 */
                     "24 50 3D 41 D1 3E 73 50 " /* B2DF r4+=r0 ldb p2 r1++ and#3 <<1 */
                     "50 50 50 24 50 3D 41 D1 " /* B2E7 <<1 <<1 <<1 r4+=r0 ldb p3 r1++ */
                     "3E 73 50 50 50 50 50 50 " /* B2EF and#3 <<1 <<1 <<1 <<1 <<1 <<1 */
                     "24 50 B4 3D 32 D2 A4 00 " /* B2F7 r4+=r0 stb(r2) r2++ r4=0 */
                     "E3 08 CF 01 00 " /* B2FF dec r3 bne disp->CBYTE=$B2D1 bne delay stop */);



}



/* gsu_check_new_sprites — ORIGINAL synthetic scanner at production PCs.
 * Record stream: 3 bytes {IDlo, (Y<<1)|IDbit8, Xtile} in bank09 at the
 * pointer in ram[$2600..2602], $FFFF terminated; flag table $28CA
 * (0=submit, else skip, rewritten $FF on submit); queue $27CE, 8B
 * {ID,X,Y,stage}. Only byte 63 (sub r3) @8055 is a retail-identical
 * signature byte; the hook redirects to 806B (submit)/807C (skip). */
static void fixture_spawn(void) {
  emit_str(0x098000,
                     "02 A5 FF F6 FF 01 AD 16 " /* 8000 cache r5=-1 r6=mask r13=22 */
                     "A7 14 A8 00 F9 CA 28 FA " /* 8008 r7=20 r8=stage r9=flags r10=queue */
                     "CE 27 3D F0 02 26 3F DF " /* 8010 lm r0,2602 romb */
                     "3D FE 00 26 01 01 01 01 " /* 8018 lm r14,2600 pad: r4 already tiles */
                     "01 01 01 01 01 01 A3 00 " /* 8020 pad: r2 already tiles r3=0 */
                     "3D 49 E0 0B 09 01 DE DE " /* 8028 flag dec bmi disp->BODY=$8036 nop r14+=3 */
                     "DE D8 D9 05 F3 01 EF DE " /* 8030 r8++ r9++ bra disp->REC=$8028 nop b0 r14++ */
                     "1C 3D EF DE BC 3E 51 09 " /* 8038 to r12 getbh r14++ w+1 beq */
                     "40 01 BC 76 3A 1B EF DE " /* 8040 disp->DONE=$8081 nop ID&mask stw ID to r11 b2=X r14++ */
                     "BC C0 1C 03 01 01 01 01 " /* 8048 r12=Y pad-to-site pad-to-site pad-to-site pad-to-site */
                     "01 01 01 01 01 63 BB 64 " /* 8050 pad-to-site pad-to-site pad-to-site pad-to-site pad-to-site HOOK site X-left */
                     "0B 22 01 BC 62 0B 1D 01 " /* 8058 bmi disp->SKIP=$807C nop Y-top bmi disp->SKIP=$807C nop */
                     "BC 67 0A 18 01 BB 6D 0A " /* 8060 Y-20 bpl disp->SKIP=$807C nop X-22 bpl */
                     "13 01 01 DA DA BB 3A DA " /* 8068 disp->SKIP=$807C nop pad-to-site q+=2 stw X q+=2 */
                     "DA BC 3A DA DA B8 3A DA " /* 8070 stw Y q+=2 stw stage q+=2 */
                     "DA B5 3D 39 D8 D9 05 A8 " /* 8078 flag=FF r8++ r9++ bra disp->REC=$8028 */
                     "01 B5 3A 00 01 " /* 8080 nop term FFFF stop nop */);        /* term; stop @8083 */
}

/* Sprite-class edge despawn — ORIGINAL synthetic program at production
 * PCs. Table $8C83+class {LE X-thr, LE Y-thr}. Byte 15 (to r5) @8A18 is
 * the only retail-identical signature byte; hook redirects to 8A1F
 * (onscreen Y recheck) or 8A2F (despawn: stw 0). Native chain after the
 * site is original-but-equivalent so a disabled hook still decides. */
static void fixture_despawn(void) {
  emit_str(0x0989F5,
                     "F9 00 17 A0 09 B0 3F DF " /* 89F5 r9=class r0=9 romb */
                     "49 3E 7C 09 2F 01 FE 83 " /* 89FD ldw(r9) and#12 beq disp->BYPASS=$8A31 nop r14=tbl */
                     "8C 2E 50 EF FB F0 00 01 " /* 8A05 with r14;add r0 -> r14+=class T r11=F0 pad */
                     "FA C8 00 01 01 01 01 01 " /* 8A0D iwt r10,#$00C8 pad-to-site pad-to-site pad-to-site pad-to-site pad-to-site */
                     "01 01 01 15 57 50 5B 65 " /* 8A15 pad-to-site pad-to-site pad-to-site HOOK site r5=T+x 2T 2T+F0 -r5 */
                     "0C 10 01 DE DE EF 15 58 " /* 8A1D bcc disp->OFF=$8A2F nop/on Yt addr Yt r5=Yt+y */
                     "EF 5A 3F 68 0C 04 01 00 " /* 8A25 Yt Yt+C8 cmp relY bcc disp->OFF=$8A2F nop stop pad-to-site */
                     "01 01 60 36 00 01 " /* 8A2D pad-to-site pad-to-site r0=0 stw(r6) stop nop */);        /* 8A2F: r0=0; stw(r6); stop */
  /* Test-authored threshold table at $8C83+class: {Xthr, Ythr} LE. */
  uint8_t tbl[] = {0x60, 0x00, 0x60, 0x00, /* class 4: T=0x60 Yt=0x60 */
                   0x90, 0x00, 0x60, 0x00, /* class 8: T=0x90 Yt=0x60 */
                   0x90, 0x00, 0xA0, 0x00}; /* class 12: T=0x90 Yt=0xA0 */
  emit(0x098C87, tbl, sizeof(tbl));
}

/* 65816 manual despawn (bank03.asm 3686+). Two routines, each:
 * x-branch carry decision (hooked) then an untouched Y check; the
 * branch-taken paths reach the shared camera-event marker STA $1700,X
 * at $A311, the survive paths RTL. Only the carry on the X CMP is the
 * hook's domain — the Y CMPs stay native to prove they still decide. */
static void fixture_cpu(void) {
  /* ORIGINAL synthetic 65816 despawn pair. Signature bytes required by
   * smw2_cpu_shape_ok: 69 80 00 @A2E2 / C9 00 02 @A2E5 / B0 @A2E8 and
   * 69 40 00 @A2FC / C9 80 01 @A2FF / B0 @A302. X/Y operand tables
   * $7680/$7682 (s_spr_cam_x/y) are the real binding. Both X and Y CMPs
   * branch to a shared event tail STA $1700,X at $A311; survives RTL at
   * $A2F6 (routine A) and $A310 (routine B). */
  emit_str(0x03A2DE,
                     "BD 80 76 18 69 80 00 C9 " /* A2DE LDA7680 CLC ADC80 CMP0200 */
                     "00 02 B0 27 BD 82 76 18 " /* A2E6 BCS@A2E8 disp->EVENT=$3A311 LDA7682 CLC */
                     "69 80 00 C9 E0 01 B0 21 " /* A2EE ADC80 CMP01E0 BCS disp->EVENT=$3A311 */
                     "6B 01 BD 80 76 18 69 40 " /* A2F6 RTL pad LDA7680 CLC ADC40 */
                     "00 C9 80 01 B0 0D BD 82 " /* A2FE CMP0180 BCS@A302 disp->EVENT=$3A311 LDA7682 */
                     "76 18 69 40 00 C9 60 01 " /* A306 CLC ADC40 CMP0160 */
                     "B0 01 6B 9D 00 17 6B " /* A30E BCS disp->EVENT=$3A311 RTL@A310 STA@A311 RTL */);  /* BCS->A311; RTL @A310; STA $1700,X; RTL */
}


/* ── Machine helpers ──────────────────────────────────────────────────── */
static void machine_reset(void) {
  memset(t_rom, 0, sizeof(t_rom));
  memset(t_gsu_ram, 0, sizeof(t_gsu_ram));
  memset(g_ram, 0, sizeof(g_ram));
  memset(&t_snes, 0, sizeof(t_snes));
  memset(&t_cart, 0, sizeof(t_cart));
  memset(&t_ppu, 0, sizeof(t_ppu));
  memset(&t_cpu, 0, sizeof(t_cpu));
  memset(s_aot_pc, 0, sizeof(s_aot_pc));
  t_cart.type = CART_SUPERFX;
  t_cart.rom = t_rom;
  t_cart.romSize = ROM_SIZE;
  t_cart.romImageSize = ROM_SIZE;
  t_cart.ram = t_gsu_ram;
  t_cart.ramSize = sizeof(t_gsu_ram);
  t_cart.superfx =
      superfx_create(t_rom, ROM_SIZE, t_gsu_ram, sizeof(t_gsu_ram));
  t_snes.cart = &t_cart;
  t_snes.ppu = &t_ppu;
  g_snes = &t_snes;
  g_ppu = &t_ppu;
  s_fx = t_cart.superfx;
  s_master = 0;
  fixture_pack();
  fixture_spawn();
  fixture_despawn();
  fixture_cpu();
  /* Gameplay + supported OBJ layout gates. */
  g_ram[0x0118] = 0x0f;
  g_ram[0x0119] = 0;
  t_ppu.obsel = 0;
  g_ws_active = true;
  cpu_state_init(&t_cpu, g_ram);
}

static void gsu_go(uint8_t pbr, uint16_t pc) {
  s_master += 60000000;
  superfx_cpu_write_io(s_fx, 0x3034, pbr);
  superfx_cpu_write_io(s_fx, 0x301E, pc & 0xff);
  superfx_cpu_write_io(s_fx, 0x301F, pc >> 8);
  superfx_sync(s_fx, s_master);
}

static void mirror_to_ppu(void) {
  /* The SNES's OAM DMA, reduced to the byte equivalence the module and
   * the PPU signature binding both verify. */
  for (unsigned i = 0; i < 256; i++)
    t_ppu.oam[i] = (uint16_t)(t_gsu_ram[0x0A00 + 2 * i] |
                              (t_gsu_ram[0x0A01 + 2 * i] << 8));
  for (unsigned i = 0; i < 32; i++)
    t_ppu.highOam[i] = t_gsu_ram[0x0C00 + i];
}

static int slot_valid(unsigned slot) {
  return (t_ppu.wsOamCoordinates.valid[slot >> 3] >> (slot & 7)) & 1;
}

/* Reference (never-hooked) core over the identical images. */
static void ref_pack_run(uint8_t *ram_out, uint16_t *regs_out,
                         uint64_t *count_out) {
  static uint8_t ref_ram[0x10000];
  memcpy(ref_ram, t_gsu_ram, sizeof(ref_ram));
  SuperFx *ref = superfx_create(t_rom, ROM_SIZE, ref_ram, sizeof(ref_ram));
  superfx_cpu_write_io(ref, 0x3034, 8);
  superfx_cpu_write_io(ref, 0x301E, 0xEF);
  superfx_cpu_write_io(ref, 0x301F, 0xB1);
  superfx_sync(ref, 60000000);
  memcpy(ram_out, ref_ram, 0x10000);
  for (unsigned i = 0; i < 16; i++)
    regs_out[i] = superfx_reg(ref, i);
  regs_out[16] = ref->sfr;
  *count_out = ref->instruction_count;
  superfx_destroy(ref);
}

/* ═══ TESTS ═══════════════════════════════════════════════════════════ */

/* 1. Native-off: margins zero -> no GSU hook armed -> byte-identical
 * RAM, registers and instruction count vs a pristine reference core. */
static void test_native_off_parity(void) {
  machine_reset();
  for (unsigned i = 0; i < 256; i++)
    src_entry(i, (int16_t)(i * 2 - 64), (int16_t)(i % 200), 0x3000 + i,
              (i & 1) ? 0x4002 : 0x0002);
  src_entry(0, 0x8000, 50, 0x1111, 0); /* sentinel ends the scan */
  Smw2SpritesBeforeFrame(0, 0);
  gsu_go(8, 0xB1EF);
  static uint8_t ref_ram[0x10000];
  uint16_t ref_regs[17];
  uint64_t ref_count;
  ref_pack_run(ref_ram, ref_regs, &ref_count);
  CHECK(memcmp(t_gsu_ram, ref_ram, sizeof(ref_ram)) == 0);
  for (unsigned i = 0; i < 16; i++)
    CHECK(superfx_reg(s_fx, i) == ref_regs[i]);
  CHECK(s_fx->sfr == ref_regs[16]);
  CHECK(s_fx->instruction_count == ref_count);
  /* No capture armed => BeginFrame publishes nothing. */
  Smw2SpritesBeginFrame();
  for (unsigned i = 0; i < 16; i++)
    CHECK(t_ppu.wsOamCoordinates.valid[i] == 0);
  printf("  native-off parity: ram+regs+instr count identical\n");
}

/* 2. Wide pack: margin admission, parked Y, signed publication. */
static void test_wide_pack_publish(void) {
  machine_reset();
  /* part0 native (x=40, 8px); part1 right-margin (x=260, 16px word3
   * bit1); part2 native emitted with x=0 (a REAL used slot whose X
   * byte is 0 — pins the contract that X==0 must not be treated as an
   * unused-slot marker); part3 left-margin reverse (x=-24, 16px, bit
   * $4000). L=32,R=16 admission window: x in [-47, 272). */
  src_entry(0, 40, 60, 0x2211, 0x0000);
  src_entry(1, 260, 70, 0x3322, 0x0002);
  src_entry(2, 0, 80, 0x4433, 0x0000);
  src_entry(3, -24, 90, 0x5544, 0x4002);
  for (unsigned i = 4; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16);
  gsu_go(8, 0xB1EF);
  const unsigned s16 = 16 * 4, s17 = 17 * 4, s18 = 18 * 4, s127 = 127 * 4;
  /* part0 forward at slot 16, onscreen packed (y-1). */
  CHECK(t_gsu_ram[0x0A00 + s16] == 40);
  CHECK(t_gsu_ram[0x0A00 + s16 + 1] == 59);
  CHECK(t_gsu_ram[0x0A00 + s16 + 2] == 0x11);
  CHECK(t_gsu_ram[0x0A00 + s16 + 3] == 0x22);
  /* part1 admitted at slot 17 but parked: Y byte F0. */
  CHECK(t_gsu_ram[0x0A00 + s17] == (uint8_t)(260 & 0xff));
  CHECK(t_gsu_ram[0x0A00 + s17 + 1] == 0xF0);
  CHECK(t_gsu_ram[0x0A00 + s17 + 2] == 0x22);
  CHECK(t_gsu_ram[0x0A00 + s17 + 3] == 0x33);
  /* part2 emitted at slot 18 with X byte 0, native-packed Y. */
  CHECK(t_gsu_ram[0x0A00 + s18] == 0);
  CHECK(t_gsu_ram[0x0A00 + s18 + 1] == 79);
  CHECK(t_gsu_ram[0x0A00 + s18 + 2] == 0x33);
  CHECK(t_gsu_ram[0x0A00 + s18 + 3] == 0x44);
  CHECK(t_gsu_ram[0x0C20 + 18] == 0x00);
  /* part3 reverse-routed at slot 127, also parked. */
  CHECK(t_gsu_ram[0x0A00 + s127] == (uint8_t)(-24 & 0xff));
  CHECK(t_gsu_ram[0x0A00 + s127 + 1] == 0xF0);
  CHECK(t_gsu_ram[0x0A00 + s127 + 2] == 0x44);
  CHECK(t_gsu_ram[0x0A00 + s127 + 3] == 0x55);
  /* Per-slot high buffer: hib = low(word3) | sign-of-x (the moves
   * flag). Positive margin X (260) does NOT set it — that is exactly
   * why the part is parked and its true coordinate lives in the
   * published metadata instead. */
  CHECK(t_gsu_ram[0x0C20 + 16] == 0x00);
  CHECK(t_gsu_ram[0x0C20 + 17] == 0x02);
  CHECK(t_gsu_ram[0x0C20 + 127] == 0x03); /* -24: sign | 0x02 */
  /* Compressed mirror packs slots 4m..4m+3 at pair (slot&3). */
  CHECK(t_gsu_ram[0x0C00 + 4] == 0x08);  /* slot17 pair = 2 */
  CHECK(t_gsu_ram[0x0C00 + 31] == 0xC0); /* slot127 pair = 3 */
  /* Slots 0..15 prologue-cleared to y=$F0. */
  for (unsigned i = 0; i < 16; i++)
    CHECK(t_gsu_ram[0x0A01 + i * 4] == 0xF0);

  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(!slot_valid(0));
  CHECK(slot_valid(16) && slot_valid(17) && slot_valid(127));
  CHECK(slot_valid(18));
  CHECK(t_ppu.wsOamCoordinates.x[18] == 0);
  CHECK(t_ppu.wsOamCoordinates.y[18] == 79);
  CHECK(t_ppu.wsOamCoordinates.x[16] == 40);
  CHECK(t_ppu.wsOamCoordinates.y[16] == 59);
  CHECK(t_ppu.wsOamCoordinates.x[17] == 260);
  CHECK(t_ppu.wsOamCoordinates.y[17] == 69);
  CHECK(t_ppu.wsOamCoordinates.x[127] == -24);
  CHECK(t_ppu.wsOamCoordinates.y[127] == 89);
  printf("  wide pack: margins admitted, parked, signed publish ok\n");
}

/* 3. Signature mismatch: post-pack mirror corruption -> that slot fails
 * closed (its valid bit never sets). */
static void test_mismatch_failclosed(void) {
  machine_reset();
  src_entry(0, 40, 60, 0x2211, 0x0000);
  src_entry(1, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 2; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16);
  gsu_go(8, 0xB1EF);
  t_gsu_ram[0x0A00 + 17 * 4] ^= 0xFF; /* corrupt slot 17 post-pack */
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(slot_valid(16));
  CHECK(!slot_valid(17));
  printf("  mirror mismatch: slot fails closed\n");
}

/* 4. Pending lifecycle: consume-once; a second BeginFrame without a new
 * capture leaves the publication alone; Reset clears it. */
static void test_pending_lifecycle(void) {
  machine_reset();
  src_entry(0, 40, 60, 0x2211, 0x0000);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16);
  gsu_go(8, 0xB1EF);
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(slot_valid(16));
  /* The consumed capture must not republish over restored residue. */
  Smw2SpritesBeginFrame();
  CHECK(slot_valid(16));
  Smw2SpritesReset();
  for (unsigned i = 0; i < 16; i++)
    CHECK(t_ppu.wsOamCoordinates.valid[i] == 0);
  printf("  pending lifecycle: consume-once + reset clear ok\n");
}

/* 5. Spawn window: margin columns submit; out-of-window and
 * already-spawned records skipped. */
static void fixture_spawn_records(const uint8_t rec[][3], unsigned n,
                                  const uint8_t *flags) {
  t_gsu_ram[0x2600] = 0x00;
  t_gsu_ram[0x2601] = 0x20;
  t_gsu_ram[0x2602] = 0x09; /* record stream pointer $2000, bank 9 */
  memset(&t_gsu_ram[0x27CE], 0, 0x200);
  memset(&t_gsu_ram[0x28CA], 0, 0x200);
  if (flags)
    memcpy(&t_gsu_ram[0x28CA], flags, n < 64 ? n : 64);
  uint32_t base = lorom(0x092000);
  for (unsigned i = 0; i < n; i++) {
    t_rom[base + i * 3 + 0] = rec[i][0]; /* word low = ID lo byte */
    t_rom[base + i * 3 + 1] = rec[i][1]; /* word hi: (Y<<1)|ID bit8 */
    t_rom[base + i * 3 + 2] = rec[i][2]; /* X tile */
  }
  t_rom[base + n * 3 + 0] = 0xFF; /* $FFFF terminator word */
  t_rom[base + n * 3 + 1] = 0xFF;
}

static void spawn_regs(void) {
  /* Tile-space camera registers: r2/r4 bound the spawn window in tiles
   * (top row 8, left col 40); r1/r3 carry the pixel camera edges used by
   * the retail new-column logic (unused by this fixture stream). */
  superfx_set_reg(s_fx, 1, 62 * 16);
  superfx_set_reg(s_fx, 2, 8);
  superfx_set_reg(s_fx, 3, 30 * 16);
  superfx_set_reg(s_fx, 4, 40);
}

static void test_spawn_window(void) {
  machine_reset();
  /* Camera rect in tiles: top=8,left=40; window x in [40-2,40+22+2) =
   * [38,64) after +2-tile margins, y in [8,28) — every rec below probes
   * a distinct edge of that rectangle. */
  const uint8_t recs[8][3] = {
      {42, 10 << 1, 42}, /* inside the native window          */
      {62, 12 << 1, 62}, /* widened-only column 62 (right)    */
      {64, 14 << 1, 64}, /* outside even widened (right bound)*/
      {50, 16 << 1, 50}, /* inside, already spawned           */
      {38, 11 << 1, 38}, /* left margin edge: x==left-lt, in  */
      {55, 7 << 1, 55},  /* new-row y==top-1: above, skip     */
      {55, 28 << 1, 55}, /* new-row y==top+20: below, skip    */
      {39, 9 << 1, 39},  /* left margin interior (x<40), in   */
  };
  const uint8_t flags[8] = {0, 0, 0, 1, 0, 0, 0, 0};
  fixture_spawn_records(recs, 8, flags);
  spawn_regs();
  Smw2SpritesBeforeFrame(32, 32); /* +2 tiles each side */
  gsu_go(9, 0x8000);
  uint16_t *q = (uint16_t *)&t_gsu_ram[0x27CE];
  /* Entries {ID, X, Y, stageID} in record order; stageID counts every
   * PROCESSED record, not just submits. */
  CHECK(q[0] == 0x02A && q[1] == 42 && q[2] == 10 && q[3] == 0);
  CHECK(q[4] == 0x03E && q[5] == 62 && q[6] == 12 && q[7] == 1);
  CHECK(q[8] == 0x026 && q[9] == 38 && q[10] == 11 && q[11] == 4);
  CHECK(q[12] == 0x027 && q[13] == 39 && q[14] == 9 && q[15] == 7);
  CHECK(q[16] == 0xFFFF); /* terminator after four submits */
  CHECK(t_gsu_ram[0x28CA + 0] == 0xFF);
  CHECK(t_gsu_ram[0x28CA + 1] == 0xFF);
  CHECK(t_gsu_ram[0x28CA + 2] == 0x00); /* right bound skip   */
  CHECK(t_gsu_ram[0x28CA + 3] == 0x01); /* already-spawned flag kept */
  CHECK(t_gsu_ram[0x28CA + 4] == 0xFF); /* left-edge submit   */
  CHECK(t_gsu_ram[0x28CA + 5] == 0x00); /* above-row skip     */
  CHECK(t_gsu_ram[0x28CA + 6] == 0x00); /* below-row skip     */
  CHECK(t_gsu_ram[0x28CA + 7] == 0xFF); /* interior-left submit */
  printf("  spawn window: left/right/new-row edges, skips kept\n");
}

static void test_spawn_queue_bound(void) {
  machine_reset();
  /* 33 unflagged in-window records: the 31-entry bound must stop
   * submissions before the flag table at $28CA. */
  uint8_t recs[33][3];
  for (unsigned i = 0; i < 33; i++) {
    recs[i][0] = (uint8_t)i;            /* ID low byte        */
    recs[i][1] = (10 << 1) | (i >> 8);  /* (Y<<1)|ID bit8     */
    recs[i][2] = 55;                    /* X tile             */
  }
  fixture_spawn_records(recs, 33, NULL);
  spawn_regs();
  Smw2SpritesBeforeFrame(48, 48);
  gsu_go(9, 0x8000);
  uint16_t *q = (uint16_t *)&t_gsu_ram[0x27CE];
  for (unsigned i = 0; i < 31; i++) {
    CHECK(q[i * 4] == i && q[i * 4 + 1] == 55 && q[i * 4 + 2] == 10 &&
          q[i * 4 + 3] == i);
  }
  CHECK(q[4 * 31] == 0xFFFF); /* terminator at 0x28C6 */
  for (unsigned i = 0; i < 31; i++)
    CHECK(t_gsu_ram[0x28CA + i] == 0xFF);
  CHECK(t_gsu_ram[0x28CA + 31] == 0x00); /* bounded out, untouched */
  CHECK(t_gsu_ram[0x28CA + 32] == 0x00);
  printf("  spawn queue: 31-entry bound enforced\n");
}

/* 6. GSU despawn: native-window sprite survives widened margin, far
 * sprite despawns, Y path unchanged, class 0 bypassed. */
static void despawn_run(int16_t relx, int16_t rely, uint16_t class_word,
                        uint16_t *state) {
  memset(&t_gsu_ram[0x1700], 0, 4);
  t_gsu_ram[0x1700] = (uint8_t)class_word;
  t_gsu_ram[0x1701] = (uint8_t)(class_word >> 8);
  t_gsu_ram[0x1750] = 0xCD; /* alive state word */
  t_gsu_ram[0x1751] = 0xAB;
  superfx_set_reg(s_fx, 6, 0x1750); /* stw (r6) state target */
  superfx_set_reg(s_fx, 7, (uint16_t)relx);
  superfx_set_reg(s_fx, 8, (uint16_t)rely);
  gsu_go(9, 0x89F5);
  *state = (uint16_t)(t_gsu_ram[0x1750] | (t_gsu_ram[0x1751] << 8));
}

static void test_despawn(void) {
  machine_reset();
  Smw2SpritesBeforeFrame(64, 64);
  uint16_t st;
  /* class-8, T=0x90: native X window [-0x90, 0x180], widened
   * [-0xD0, 0x1C0]. relX=0x1A0 is widened-only onscreen. */
  despawn_run(0x1A0, 0x10, 8, &st);
  CHECK(st == 0xABCD);
  /* One pixel past the widened right edge despawns. */
  despawn_run(0x1C1, 0x10, 8, &st);
  CHECK(st == 0x0000);
  /* Left edge: -0xD0 exactly on the bound, -0xD1 one past. */
  despawn_run(-0xD0, 0x10, 8, &st);
  CHECK(st == 0xABCD);
  despawn_run(-0xD1, 0x10, 8, &st);
  CHECK(st == 0x0000);
  /* Y offscreen (Yt=0x60): native path still despawns even when X is
   * comfortably inside. */
  despawn_run(0x10, 0x200, 8, &st);
  CHECK(st == 0x0000);
  /* class-0 (class&12 == 0) bypasses the despawn entirely: survives at
   * any relX, including beyond even the widened bound. */
  despawn_run(0x7FFF, 0x10, 0, &st);
  CHECK(st == 0xABCD);
  /* Native Y-threshold edge equality (class 4: Yt=0x60; X held inside
   * so the Y path alone decides): the real chain bounds onscreen at
   * u16(Yt+relY) <= 2Yt+$C8, i.e. relY <= Yt+$C8 = $0128 — $0128
   * survives, $0129 despawns. */
  despawn_run(0x10, 0x0128, 4, &st);
  CHECK(st == 0xABCD);
  despawn_run(0x10, 0x0129, 4, &st);
  CHECK(st == 0x0000);
  printf("  GSU despawn: margin X, Y edges, class-0 bypass ok\n");
}

/* 7. CPU despawn carry: hooked carry honors live margins, native when
 * off, Y checks and state untouched. */
static void cpu_despawn_case(uint16_t x, uint16_t *mark, uint32_t routine) {
  memset(&t_cpu, 0, sizeof(t_cpu));
  cpu_state_init(&t_cpu, g_ram);
  t_cpu.emulation = 0;
  t_cpu.P = 0x04; /* native mode, 16-bit A/X, I set */
  cpu_p_to_mirrors(&t_cpu); /* m_flag=x_flag=0, _flag_I=1 */
  t_cpu.PB = 0x03;
  t_cpu.DB = 0x7E;
  t_cpu.S = 0x1FE0;
  t_cpu.X = 0;
  t_cpu.host_return_valid = 0;
  g_ram[0x7680] = (uint8_t)x;
  g_ram[0x7681] = (uint8_t)(x >> 8);
  g_ram[0x7682] = 0x10; /* y pos: onscreen for both Y CMPs */
  g_ram[0x7683] = 0;
  g_ram[0x1700] = 0;
  g_ram[0x1701] = 0;
  cpu_push_jsl_return_frame(&t_cpu);
  int ok = interp_bridge_run(&t_cpu, routine);
  CHECK(ok == 1);
  *mark = (uint16_t)(g_ram[0x1700] | (g_ram[0x1701] << 8));
}

static void test_cpu_despawn(void) {
  machine_reset();
  uint16_t mark;
  /* Native A: (x+0x80) >= 0x200 -> x >= 0x180 events. */
  Smw2SpritesBeforeFrame(0, 0);
  cpu_despawn_case(0x180, &mark, 0x03A2DE);
  CHECK(mark != 0);
  cpu_despawn_case(0x17F, &mark, 0x03A2DE);
  CHECK(mark == 0);
  /* Widened A (L=R=64): inside iff u16(x+0xC0) < 0x280 ->
   * x in [-0xC0, 0x1C0). */
  Smw2SpritesBeforeFrame(64, 64);
  cpu_despawn_case(0x180, &mark, 0x03A2DE);
  CHECK(mark == 0); /* native edge now onscreen */
  cpu_despawn_case(0x1BF, &mark, 0x03A2DE);
  CHECK(mark == 0);
  cpu_despawn_case(0x1C0, &mark, 0x03A2DE);
  CHECK(mark != 0);
  /* Left wrap: x=-0xC0 (0xFF40) onscreen, -0xC1 (0xFF3F) off. */
  cpu_despawn_case(0xFF40, &mark, 0x03A2DE);
  CHECK(mark == 0);
  cpu_despawn_case(0xFF3F, &mark, 0x03A2DE);
  CHECK(mark != 0);
  /* Routine B (CMP #$0180 on x+$40): widened inside iff
   * u16(x+0x80) < 0x200 -> x in [-0x80, 0x180). */
  cpu_despawn_case(0x140, &mark, 0x03A2F8);
  CHECK(mark == 0); /* native edge now onscreen */
  cpu_despawn_case(0x17F, &mark, 0x03A2F8);
  CHECK(mark == 0);
  cpu_despawn_case(0x180, &mark, 0x03A2F8);
  CHECK(mark != 0);
  printf("  CPU despawn: widened carry both thresholds, wrap left\n");
}

/* 8. AOT coverage guard: cpu_dispatch_has_entry coverage on a hook PC
 * (or its containing routine) marks the CPU hooks inert — the native
 * carry stands — while GSU widening is untouched. */
static void test_aot_guard(void) {
  machine_reset();
  uint16_t mark;
  s_aot_pc[0] = 0x03A2E8; /* hook PC itself compiled */
  Smw2SpritesBeforeFrame(64, 64);
  cpu_despawn_case(0x180, &mark, 0x03A2DE);
  CHECK(mark != 0); /* native decision: widened carry NOT applied */
  memset(s_aot_pc, 0, sizeof(s_aot_pc));
  s_aot_pc[0] = 0x03A2F8; /* containing routine of the B hook */
  Smw2SpritesBeforeFrame(64, 64);
  cpu_despawn_case(0x140, &mark, 0x03A2F8);
  CHECK(mark != 0); /* native decision stands again */
  printf("  AOT guard: CPU hooks inert under coverage\n");
}

/* 9. Disable/load/core replacement: registered callbacks must be inert
 * when disabled, and disarming stops the GSU widening. */
static void test_disable_rearm(void) {
  machine_reset();
  uint16_t mark;
  Smw2SpritesBeforeFrame(64, 64); /* arm once */
  Smw2SpritesBeforeFrame(0, 0);   /* disable */
  cpu_despawn_case(0x180, &mark, 0x03A2DE);
  CHECK(mark != 0); /* inert: native offscreen decision */
  /* GSU hooks disarmed: the pack reverts to native clipping. */
  src_entry(0, 260, 70, 0x3322, 0x0002); /* right-margin part */
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  gsu_go(8, 0xB1EF);
  CHECK(t_gsu_ram[0x0A00 + 16 * 4 + 1] == 0xF0); /* gap-cleared Y */
  CHECK(t_gsu_ram[0x0A00 + 16 * 4] == 0x00);     /* never written */
  printf("  disable: CPU inert + GSU disarmed\n");
}

/* 10. Cart/core replacement mid-scene: the retired core's armed hooks
 * die with it and the pending capture is dropped, not leaked into the
 * new core's first pack. */
static void test_core_swap(void) {
  machine_reset();
  src_entry(0, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16);
  /* Replace the core (load ROM / reset path) before its first pack. */
  superfx_destroy(s_fx);
  memset(t_gsu_ram + 0xA00, 0, 0x300);
  s_fx = superfx_create(t_rom, ROM_SIZE, t_gsu_ram, sizeof(t_gsu_ram));
  t_cart.superfx = s_fx;
  /* src_entry wrote into t_gsu_ram which the new core shares. */
  src_entry(0, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16); /* re-verifies, re-arms on the new core */
  gsu_go(8, 0xB1EF);
  CHECK(t_gsu_ram[0x0A00 + 16 * 4] == (260 & 0xff));
  CHECK(t_gsu_ram[0x0A00 + 16 * 4 + 1] == 0xF0);
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(slot_valid(16));
  CHECK(t_ppu.wsOamCoordinates.x[16] == 260);
  printf("  core swap: hooks re-armed, capture rebuilt\n");
}

/* 11. Instruction mutation: a shape byte that no longer matches must
 * keep the module native (no arm, no publish). */
static void test_shape_mutation(void) {
  machine_reset();
  src_entry(0, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  t_rom[lorom(0x08B219)] = 0x01; /* nop where `cmp r7` prefix was */
  Smw2SpritesBeforeFrame(32, 16);
  gsu_go(8, 0xB1EF);
  CHECK(t_gsu_ram[0x0A00 + 16 * 4] == 0x00); /* part never packed */
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(!slot_valid(16));
  printf("  shape mutation: stays native\n");
}


/* 12. g_ws_active is PREVIOUS prepared-frame state and must not gate
 * enablement: first enable/resize from native (ws=false, nonzero
 * supplied margins) must arm and widen identically. */
static void test_ws_inactive_enable(void) {
  machine_reset();
  g_ws_active = false; /* previous frame was native */
  src_entry(0, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16);
  gsu_go(8, 0xB1EF);
  CHECK(t_gsu_ram[0x0A00 + 16 * 4] == (260 & 0xff));
  CHECK(t_gsu_ram[0x0A00 + 16 * 4 + 1] == 0xF0);
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(slot_valid(16));
  CHECK(t_ppu.wsOamCoordinates.x[16] == 260);
  /* And with ws=true but zero margins, every mutation stays inert. */
  machine_reset();
  g_ws_active = true;
  src_entry(0, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(0, 0);
  gsu_go(8, 0xB1EF);
  CHECK(t_gsu_ram[0x0A00 + 16 * 4 + 1] == 0xF0); /* native gap fill, not
                                                * the margin part's X */
  CHECK(t_gsu_ram[0x0A00 + 16 * 4] == 0x00);
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(!slot_valid(16));
  printf("  ws-inactive enable: supplied margins arm widening\n");
}


/* 13. Pack edge coordinates and inactive ($8000) source entries.
 * $8000 is gsu_init_oam_buffer's per-entry inactive marker, NOT a
 * terminator: the real fixed-256 scan just clips it like any other
 * out-of-bounds X and continues, so eligible parts after a hole must
 * still emit, park, and publish. */
static void test_pack_edges_inactive(void) {
  machine_reset();
  src_entry(0, 0x8000, 50, 0x1111, 0x0000); /* leading inactive hole */
  src_entry(1, 40, 60, 0x2211, 0x0000);    /* fwd slot 16 (native) */
  src_entry(2, -39, 61, 0x3311, 0x0002);   /* left bound, slot 17 */
  src_entry(3, 300, 62, 0x4411, 0x0002);   /* widened-only, slot 18 */
  src_entry(4, -30, 63, 0x5511, 0x4002);   /* rev slot 127 */
  src_entry(5, -300, 64, 0x6611, 0x0002);  /* clipped left */
  src_entry(6, 520, 65, 0x7711, 0x0002);   /* clipped right */
  src_entry(7, 0x8000, 66, 0x8811, 0x0002);/* interior inactive hole */
  src_entry(8, -34, 67, 0x9911, 0x0002);   /* widened-only, slot 19 */
  src_entry(9, 40, 68, 0xAA11, 0x0000);    /* native, slot 20 */
  src_entry(10, 45, 239, 0xBB11, 0x0000);  /* Y upper bound-1, slot 21 */
  src_entry(11, 45, 240, 0xCC11, 0x0000);  /* Y==240: clipped */
  src_entry(12, 45, -15, 0xDD11, 0x0000);  /* Y lower bound, slot 22 */
  src_entry(13, 45, -16, 0xEE11, 0x0000);  /* Y<-15: clipped */
  for (unsigned i = 14; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 64);
  gsu_go(8, 0xB1EF);
  const unsigned b = 0x0A00;
  /* Both inactive entries consumed NO slots: emission is dense. */
  CHECK(t_gsu_ram[b + 16 * 4] == 40);
  CHECK(t_gsu_ram[b + 16 * 4 + 1] == 59);  /* native part: packed Y-1 */
  CHECK(t_gsu_ram[b + 17 * 4] == (uint8_t)(-39 & 0xff));
  CHECK(t_gsu_ram[b + 17 * 4 + 1] == 0xF0); /* widened-only parked */
  CHECK(t_gsu_ram[b + 18 * 4] == (uint8_t)(300 & 0xff));
  CHECK(t_gsu_ram[b + 18 * 4 + 1] == 0xF0);
  CHECK(t_gsu_ram[0x0C20 + 17] == 0x03);   /* sign bit | w3lo 0x02 */
  CHECK(t_gsu_ram[0x0C20 + 18] == 0x02);
  CHECK(t_gsu_ram[b + 127 * 4] == (uint8_t)(-30 & 0xff));
  CHECK(t_gsu_ram[b + 127 * 4 + 2] == 0x11);
  CHECK(t_gsu_ram[b + 127 * 4 + 3] == 0x55);
  CHECK(t_gsu_ram[0x0C20 + 127] == 0x03);
  /* The interior hole did not terminate the scan: later parts emit. */
  CHECK(t_gsu_ram[b + 19 * 4] == (uint8_t)(-34 & 0xff));
  CHECK(t_gsu_ram[b + 19 * 4 + 1] == 0xF0);
  CHECK(t_gsu_ram[b + 20 * 4] == 40);
  CHECK(t_gsu_ram[b + 20 * 4 + 1] == 67);
  CHECK(t_gsu_ram[b + 21 * 4] == 45);
  CHECK(t_gsu_ram[b + 21 * 4 + 1] == 238); /* y=239 admitted, packed 238 */
  CHECK(t_gsu_ram[b + 22 * 4] == 45);
  CHECK(t_gsu_ram[b + 22 * 4 + 1] == (uint8_t)(-16 & 0xff)); /* -15 packed */
  /* After slot 22 the gap-clear owns the Y bytes to the rev write. */
  for (unsigned a = b + 23 * 4; a < b + 127 * 4 - 4; a += 4) {
    CHECK(t_gsu_ram[a] == 0x00);
    CHECK(t_gsu_ram[a + 1] == 0xF0);
  }
  /* Publication agrees: the widened-only parts after the holes get
   * their true signed coordinates. */
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(slot_valid(16) && t_ppu.wsOamCoordinates.x[16] == 40);
  CHECK(slot_valid(17) && t_ppu.wsOamCoordinates.x[17] == -39);
  CHECK(slot_valid(18) && t_ppu.wsOamCoordinates.x[18] == 300);
  CHECK(slot_valid(19) && t_ppu.wsOamCoordinates.x[19] == -34);
  CHECK(slot_valid(20) && t_ppu.wsOamCoordinates.x[20] == 40);
  CHECK(slot_valid(127) && t_ppu.wsOamCoordinates.x[127] == -30);
  /* Clipped and Y-rejected records own no slots: nothing may publish
   * at 23..126 or below 16. */
  for (unsigned s = 0; s < 16; s++) CHECK(!slot_valid(s));
  for (unsigned s = 23; s < 127; s++) CHECK(!slot_valid(s));
  printf("  pack edges: -300/300/520/-39, Y239/-15 bounds, holes ok\n");
}

/* 14. Forward/reverse parity and the 112-crossing termination. */
static void test_pack_crossing(void) {
  machine_reset();
  /* 70 forward parts (x=60) interleaved with 70 reverse parts
   * (x=-30,w3 $4000): enough writes to collide mid-table; the crossing
   * check must stop emission before the two streams overlap, and every
   * later part clips. */
  for (unsigned i = 0; i < 140; i++) {
    if (i & 1)
      src_entry(i, -30, (int16_t)(20 + i), 0x5000 + i, 0x4002);
    else
      src_entry(i, 60, (int16_t)(20 + i), 0x6000 + i, 0x0002);
  }
  for (unsigned i = 140; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 64);
  gsu_go(8, 0xB1EF);
  /* The forward stream advances r2 by 4 per write; the reverse stream
   * retreats r3 by 6 (writes at +0/+1, stw at +2, then 6-byte step).
   * Emission continues while r3 >= r2; the first write that would
   * collide is clipped and everything after it is. Count written slots
   * from the mirror directly and require total == native bound. */
  unsigned fwd_written = 0, rev_written = 0;
  for (unsigned s = 16; s < 128; s++) {
    const uint8_t *sl = &t_gsu_ram[0x0A00 + s * 4];
    if (sl[0] == 60 && sl[3] == 0x60)
      fwd_written++;
    else if (sl[0] == (uint8_t)(-30 & 0xff) && sl[3] == 0x50)
      rev_written++;
  }
  /* The two streams met exactly: forward ends at slot 71, reverse owns
   * slot 72 — 112 writes and then the crossing check ended the scan,
   * even though parts 112..139 were all eligible. Any collision would
   * have interleaved attr_lo bytes inside a slot; verify each slot is
   * one clean part. */
  unsigned total = fwd_written + rev_written;
  CHECK(total == 112);
  CHECK(fwd_written == 56 && rev_written == 56);
  /* Boundary slot contents: s71 last fwd (part 110 attr_lo=0x6E),
   * s72 top rev (part 111 attr_lo=0x6F). */
  CHECK(t_gsu_ram[0x0A00 + 71 * 4 + 2] == 0x6E &&
        t_gsu_ram[0x0A00 + 72 * 4 + 2] == 0x6F);
  /* Every fwd slot's attr_lo is even-in-order, every rev slot's is
   * odd-in-order: no part was overwritten by the other stream. */
  for (unsigned s = 16; s <= 71; s++) {
    CHECK(t_gsu_ram[0x0A00 + s * 4 + 2] == (uint8_t)(2 * (s - 16)));
    CHECK(t_gsu_ram[0x0A00 + s * 4 + 3] == 0x60);
  }
  for (unsigned s = 72; s <= 127; s++) {
    CHECK(t_gsu_ram[0x0A00 + s * 4 + 2] ==
          (uint8_t)(1 + 2 * (127 - s)));
    CHECK(t_gsu_ram[0x0A00 + s * 4 + 3] == 0x50);
  }
  printf("  pack crossing: %u fwd + %u rev, exact-fill boundary\n",
         fwd_written, rev_written);
}

/* 15. Instance-area noninterference (preservation coverage). */
static void test_spawn_area_preserved(void) {
  machine_reset();
  const uint8_t recs[3][3] = {
      {42, 10 << 1, 42},
      {38, 11 << 1, 38},
      {64, 12 << 1, 64},
  };
  fixture_spawn_records(recs, 3, NULL);
  /* Canary regions the spawn routine must never touch: the pointer
   * itself was consumed at arming; guard everything outside the queue
   * ($27CE..$28C9) and flag table ($28CA..$28C9+64) this module owns. */
  for (unsigned a = 0x2700; a < 0x27CE; a++) t_gsu_ram[a] = 0xA5;
  for (unsigned a = 0x2900; a < 0x2A00; a++) t_gsu_ram[a] = 0x5A;
  /* Primary-evidenced instance-state span ($1462/$1640/$1700/$1750/
   * $16E2/$1ECC): snapshot the region and require byte-identity after
   * the run. Preservation-only binding; actual allocator capacity is a
   * Task5 qualification, not proven here. */
  uint8_t inst[0x1ED0 - 0x1460];
  memcpy(inst, &t_gsu_ram[0x1460], sizeof(inst));
  spawn_regs();
  Smw2SpritesBeforeFrame(32, 32);
  gsu_go(9, 0x8000);
  for (unsigned a = 0x2700; a < 0x27CE; a++)
    CHECK(t_gsu_ram[a] == 0xA5);
  for (unsigned a = 0x2900; a < 0x2A00; a++)
    CHECK(t_gsu_ram[a] == 0x5A);
  CHECK(memcmp(inst, &t_gsu_ram[0x1460], sizeof(inst)) == 0);
  /* Only the owned queue/flag bytes may differ: two submits, one
   * window-skip; flag[0]=FF flag[1]=FF flag[2] stays 0. */
  CHECK(t_gsu_ram[0x28CA] == 0xFF && t_gsu_ram[0x28CB] == 0xFF &&
        t_gsu_ram[0x28CC] == 0x00);
  printf("  spawn preservation: untracked areas byte-identical\n");
}

/* 16. Title-level stale-capture-over-restored-residue regression. */
static void test_stale_capture_over_residue(void) {
  machine_reset();
  src_entry(0, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16);
  gsu_go(8, 0xB1EF);
  mirror_to_ppu();
  Smw2SpritesBeginFrame(); /* consume once: slot16 widened coords */
  CHECK(slot_valid(16));
  /* Roll back: restore a residue captured BEFORE the pack — the saved
   * frame had no published coordinates. */
  PpuRollbackResidue res;
  ppu_rb_residue_get(&t_ppu, &res);
  memset(&res.wsOamCoordinates, 0, sizeof(res.wsOamCoordinates));
  ppu_rb_residue_set(&t_ppu, &res);
  CHECK(!slot_valid(16)); /* residue reproduces the pre-publish picture */
  /* The next frame boundary has no fresh capture: the already-consumed
   * pending capture must NOT republish over the restored state. */
  Smw2SpritesBeforeFrame(32, 16);
  Smw2SpritesBeginFrame();
  CHECK(!slot_valid(16));
  CHECK(t_ppu.wsOamCoordinates.x[16] == 0);
  printf("  residue: consumed capture stays dead over restore\n");
}

/* 17. Unsupported OBJ layout stays native. */
static void test_layout_gate(void) {
  machine_reset();
  t_ppu.obsel = 0x20; /* OBSEL size-select mode 1: unsupported */
  src_entry(0, 260, 70, 0x3322, 0x0002);
  for (unsigned i = 1; i < 256; i++)
    src_entry(i, -100, -100, 0, 0);
  Smw2SpritesBeforeFrame(32, 16);
  gsu_go(8, 0xB1EF);
  /* Native clip: 260 not packed; gap-fill F0 at slot 16 Y. */
  CHECK(t_gsu_ram[0x0A00 + 16 * 4] == 0x00);
  CHECK(t_gsu_ram[0x0A00 + 16 * 4 + 1] == 0xF0);
  mirror_to_ppu();
  Smw2SpritesBeginFrame();
  CHECK(!slot_valid(16));
  printf("  layout gate: unsupported OBSEL stays native\n");
}

int main(void) {
  test_native_off_parity();
  test_wide_pack_publish();
  test_mismatch_failclosed();
  test_pending_lifecycle();
  test_spawn_window();
  test_spawn_queue_bound();
  test_despawn();
  test_cpu_despawn();
  test_aot_guard();
  test_disable_rearm();
  test_core_swap();
  test_shape_mutation();
  test_ws_inactive_enable();
  test_pack_edges_inactive();
  test_pack_crossing();
  test_spawn_area_preserved();
  test_stale_capture_over_residue();
  test_layout_gate();
  Smw2SpritesShutdown();
  printf("smw2_sprite_test: PASS\n");
  return 0;
}
