/* Super Mario World 2: Yoshi's Island — title-side runtime policy.
 *
 * Exact background source for the widescreen side margins. YI authors its
 * BG1 terrain as MAP16 objects (16x16 tiles): a screen-grid map of MAP16
 * index words lives in WRAM, the screen ids in GSU (Game Pak) RAM, and the
 * four 8x8 tilemap words of each MAP16 object in ROM. The GSU streams the
 * current screen + the approach screen into two 256-px VRAM tilemap pages;
 * hardware scroll then reads them as one 64x32 map. The same decode is done
 * here host-side to key WsShadow's margin store to true world tiles.
 *
 * Address evidence (yi-disasm, all verified):
 *   cameras            DP $0039-$0047 (g_ram): NMI writes $0039..$0044 raw to
 *                      bg1..bg3 hofs/vofs (bank00.asm $00C0C1+) — the PPU
 *                      scroll IS the world camera modulo the 512-px map wrap.
 *   screen_num_to_id   CPU $006CAA mirrors GSU RAM $700CAA: byte table, index
 *                      x_scr | (y_scr << 4)  (bank10.asm $1090E2 builds the
 *                      word index as 00000000_0yyy_xxxx and reads the upper
 *                      byte through $6CA9,x; bank03.asm $03D442 and
 *                      bank06.asm $06C2EE read T[sn]&$3F directly).
 *                      Bit 7 = unloaded; slot 0 is a valid screen id.
 *   MAP16 RAM          WRAM $7F8000 + screen_id*$200 + row*$20 + col*2
 *                      (bank10.asm $1090F1 "0ssssssrrrrcccc0"; row/col = 16x16
 *                      tile inside the 16x16-tile screen). Mutable in play —
 *                      change_map16 (bank10.asm $109295) writes it, so
 *                      destroyed terrain resolves exactly.
 *   MAP16 ROM          GSU bank $4C (file $0C0000): page offsets u16 at
 *                      $32A4+page*2, entries at $33F2 + page_off + index*8.
 *                      GSU offsets wrap at 64K; not CPU LoROM addresses.
 *                      8 bytes = four tilemap words in TL, TR, BL, BR order
 *                      (gsu_map16_partial_column, bank09.asm $09FA28).
 *
 * Fail-safe like X2's: the walk must reproduce the live native view or the
 * layer stands down to authentic map wrap for that frame. Registered layers
 * are served from the world-keyed store; unregistered frames fall back to
 * the renderer's wrap.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h" /* g_snes, g_snes_cpu */
#include "common_rtl.h"       /* g_ppu, g_ram */
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/superfx.h"
#include "snes/ws_shadow.h"
#include "types.h"

extern bool g_ws_active;
extern int g_ws_extra;

/* BG layer indices in the Ppu/ws_shadow APIs. BG2 (index 1) is deliberately
 * absent: it stays natural per-line rendering and is never registered. */
enum { kBg1 = 0, kBg3 = 2 };

/* Camera mirrors (direct page, bank $7E). */
static uint16_t smw2_dp_u16(unsigned addr) {
  return (uint16_t)(g_ram[addr] | (g_ram[addr + 1] << 8));
}
#define SMW2_CAM_X(n) smw2_dp_u16(0x39 + (n) * 4)
#define SMW2_CAM_Y(n) smw2_dp_u16(0x3b + (n) * 4)

/* GSU Game Pak RAM ($70:xxxx view = cart->ram + addr). */
static const uint8_t *smw2_gsu_ram(void) {
  return (g_snes && g_snes->cart && g_snes->cart->ram) ? g_snes->cart->ram
                                                       : NULL;
}
/* GSU Game Pak RAM word. Physical cart SRAM is its own address space here —
 * the identically numbered WRAM cell is unrelated, so GSU-space variables
 * like the camera-event flag $1E2A (bank09.asm $0994D7, sram_vars.asm
 * !s_cam_event_flag_l) must be read through the cart, never g_ram. */
static uint16_t smw2_gsu_u16(unsigned addr) {
  const uint8_t *gsu = smw2_gsu_ram();
  if (!gsu || addr + 1 >= (unsigned)g_snes->cart->ramSize)
    return 0xffff; /* absent RAM reads as "active", the fail-closed answer */
  return (uint16_t)(gsu[addr] | (gsu[addr + 1] << 8));
}

/* Screen id for a 16x16-tile screen at (x_scr, y_scr).
 * Grid: screen_num = x_scr | (y_scr << 4). Bit 7 marks unloaded screens;
 * zero is a valid slot. CPU $6CAA is the $6000 mirror of GSU $70:0CAA. */
static unsigned smw2_screen_id(uint32_t x_scr, uint32_t y_scr) {
  const uint8_t *gsu = smw2_gsu_ram();
  if (!gsu || y_scr >= 8 || x_scr >= 16)
    return 0x80;
  const uint32_t off = 0x0caa + (y_scr << 4) + x_scr;
  if (off >= (uint32_t)g_snes->cart->ramSize)
    return 0x80;
  return gsu[off] & 0xbf; /* preserve the unloaded flag */
}

/* MAP16 index word (page<<8 | object offset) for a 16x16 world tile. */
static uint16_t smw2_map16_index(uint32_t m16x, uint32_t m16y, bool *ok) {
  *ok = false;
  const unsigned id = smw2_screen_id(m16x >> 4, m16y >> 4);
  if (id & 0x80)
    return 0;
  const uint32_t off =
      0x18000 + (uint32_t)id * 0x200 + (m16y & 15) * 0x20 + (m16x & 15) * 2;
  if (off + 1 >= 0x20000)
    return 0;
  *ok = true;
  return (uint16_t)(g_ram[off] | (g_ram[off + 1] << 8));
}

/* The four BG1 tilemap words of a MAP16 object, in TL, TR, BL, BR order.
 * Returns false when the index or the ROM tables can't resolve. */
static bool smw2_map16_entry(uint16_t index_word, uint16_t out[4]) {
  const Cart *cart = g_snes ? g_snes->cart : NULL;
  if (!cart || !cart->rom || cart->romSize < 0xd0000)
    return false;
  const uint8_t *bank = cart->rom + 0xc0000; /* GSU ROMB $4C */
  const unsigned page = index_word >> 8;
  const unsigned obj = index_word & 0xff;
  const unsigned p = 0x32a4 + page * 2;
  const uint16_t eaddr =
      (uint16_t)(0x33f2 + bank[p] + (bank[p + 1] << 8) + obj * 8);
  for (unsigned i = 0; i < 4; i++) {
    const uint16_t lo = (uint16_t)(eaddr + i * 2);
    const uint16_t hi = (uint16_t)(lo + 1);
    out[i] = (uint16_t)(bank[lo] | (bank[hi] << 8));
  }
  return true;
}

/* Exact BG1 tilemap word for world tile (tx, ty) in 8x8 units. */
static bool smw2_bg1_tile(uint32_t tx, uint32_t ty, uint16_t *word) {
  bool ok;
  const uint16_t idx = smw2_map16_index(tx >> 1, ty >> 1, &ok);
  if (!ok)
    return false;
  uint16_t e[4];
  if (!smw2_map16_entry(idx, e))
    return false;
  *word = e[(ty & 1) * 2 + (tx & 1)];
  return true;
}

/* VRAM word address holding the BG1 tile for buffer column/row (wide 64x32
 * map: second 32 columns live at +0x400 words — the right-hand screen of the
 * two-screen-wide GSU window; bank10.asm $1090A7/$1091B5 flag odd-screen
 * m16 columns via the $0400 page bit). */
static uint16_t smw2_vram_word(uint32_t map_col, uint32_t map_row) {
  const uint32_t off = (uint32_t)PPU_bgTilemapAdr(g_ppu, kBg1) +
                       ((map_row & 31) << 5) + (map_col & 31) +
                       ((map_col & 32) ? 0x400 : 0);
  return (uint16_t)(off & 0x7fff);
}

/* The walk must reproduce the live native view to earn the gutters: mode-1
 * menus, mid-transition states and any stage where BG1 isn't the streamed
 * map all fail validation and keep the authentic wrap. Mirrors X2's
 * stream_valid: 12 spread samples and not a near-uniform view (nothing
 * proven by sampling one repeated tile).
 *
 * The VRAM window is addressed by BUFFER tiles (hScroll/vScroll derived);
 * the MAP16 tables are addressed by WORLD tiles (lifted camera derived).
 * The two differ whenever the camera leaves the first 512 px, so the
 * samples cross both spaces.
 *
 * Two proof levels share the walk: settled frames keep the tolerant 10/12
 * threshold (the game stages moving BG objects into the map mid-frame, so a
 * late DMA can legitimately beat one sample), while a cold start or a known
 * scene/timeline change must reproduce ALL 12 samples to re-arm — anything
 * less keeps the view native until a later complete proof. */
static unsigned smw2_bg1_view_hits(uint32_t world_tx0, uint32_t world_ty0,
                                   uint32_t buf_tx0, uint32_t buf_ty0,
                                   bool *near_uniform) {
  *near_uniform = false;
  const uint16_t *vram = PpuRenderVram(g_ppu);
  if (!vram)
    return 0;
  static const int kSampleCol[12] = {0,  3,  6,  9,  12, 15,
                                     18, 21, 24, 27, 30, 31};
  static const int kSampleRow[12] = {0,  3,  6,  9,  12, 15,
                                     18, 21, 24, 26, 27, 28};
  uint16_t samples[12];
  unsigned modal_n = 0, hits = 0;
  for (int i = 0; i < 12; i++) {
    const uint32_t wtx = (world_tx0 + (uint32_t)kSampleCol[i]) & 0x3ff;
    const uint32_t wty = (world_ty0 + (uint32_t)kSampleRow[i]) & 0x1ff;
    const uint16_t got =
        vram[smw2_vram_word(buf_tx0 + (uint32_t)kSampleCol[i],
                            buf_ty0 + (uint32_t)kSampleRow[i]) &
             0x7fff];
    samples[i] = got;
    unsigned count = 0;
    for (int j = 0; j <= i; j++)
      count += samples[j] == got;
    if (count > modal_n)
      modal_n = count;
    uint16_t want;
    if (smw2_bg1_tile(wtx, wty, &want) && want == got)
      hits++;
  }
  *near_uniform = modal_n > 9; /* a near-uniform view proves nothing */
  return hits;
}

static bool smw2_bg1_stream_valid(uint32_t world_tx0, uint32_t world_ty0,
                                  uint32_t buf_tx0, uint32_t buf_ty0) {
  bool nu;
  return smw2_bg1_view_hits(world_tx0, world_ty0, buf_tx0, buf_ty0, &nu) >=
             10 &&
         !nu;
}

/* Strict cold/re-arm proof: every sample must match and the view must be
 * informative. Settled frames use the tolerant walk above instead. */
static bool smw2_bg1_stream_proven(uint32_t world_tx0, uint32_t world_ty0,
                                   uint32_t buf_tx0, uint32_t buf_ty0) {
  bool nu;
  return smw2_bg1_view_hits(world_tx0, world_ty0, buf_tx0, buf_ty0, &nu) ==
             12 &&
         !nu;
}

/* Outside-world tilemap entry for margin pixels whose worldX is negative
 * (the camera can sit at the level's west edge; nothing exists there, so
 * without a bound the ws_shadow fetch falls through to the rolling
 * tilemap's far page — the staged east-ahead strip — and paints repeated
 * terrain garbage). Prefer the game's own MAP16 empty object, index 0,
 * resolved through the current ROM tileset — the same decoder the live
 * proof uses — but only when all four quadrants resolve to ONE uniform
 * entry; a decorated "empty" object is not a safe single tile. Never call
 * smw2_bg1_tile with a negative tx: the screen-table index masks to slot
 * 15, which is a populated far-east screen in other rooms — that would
 * serve real level art as "empty". When no uniform entry exists, serve a
 * tile proven fully transparent in the current BG1 tile page so BG2/BG3's
 * authored wrap and color math show through; the zero check runs every
 * frame, so a streamed-over tile can never be served stale. If neither
 * exists, the option is left disabled and the established wrap stands. */
static int smw2_bg1_outside_entry(void) {
  uint16_t e[4];
  if (smw2_map16_entry(0x0000, e) && e[0] == e[1] && e[0] == e[2] &&
      e[0] == e[3])
    return (int)e[0];
  const uint16_t *v = PpuRenderVram(g_ppu);
  if (!v)
    return -1;
  const uint32_t base = (uint32_t)PPU_bgTileAdr(g_ppu, kBg1) & 0x7fff;
  for (unsigned t = 0; t < 0x400; t++) {
    const uint32_t a = (base + t * 16) & 0x7fff;
    bool zero = true;
    for (unsigned w = 0; w < 16; w++)
      if (v[(a + w) & 0x7fff] != 0) {
        zero = false;
        break;
      }
    if (zero)
      return (int)t; /* palette 0, no flips: a fully transparent cell */
  }
  return -1;
}

/* Hardware scroll supplies the pixel phase; the camera supplies the high
 * world bits. BG1's two-page X window wraps at 512px, its Y window at 256px.
 * A full 1024px difference between cam Y and PPU VOFS is normal, not lag. */
static void smw2_world_origin(int layer, int32_t *wx, int32_t *wy) {
  const int32_t cam_x = (int16_t)SMW2_CAM_X(layer);
  int32_t dx = ((int32_t)g_ppu->hScroll[layer] - cam_x) & 511;
  if (dx >= 256)
    dx -= 512;
  *wx = cam_x + dx;
  const int32_t cam_y = (int16_t)SMW2_CAM_Y(layer);
  int32_t dy = ((int32_t)g_ppu->vScroll[layer] - cam_y) & 255;
  if (dy >= 128)
    dy -= 256;
  *wy = cam_y + dy;
}

/* Scene/lifetime epoch, in the game's own words. Any field changing —
 * plus the host timeline generation or a cart pointer swap — means the
 * world-keyed store could hold tiles from a scene the player can no longer
 * see. Ordinary streaming mutates the screen-id table and VRAM columns
 * without touching these, so table writes are NOT an epoch source. */
enum { kSceneWords = 5 };
static void smw2_scene_tuple(uint16_t out[kSceneWords]) {
  out[0] = smw2_dp_u16(0x0118); /* r_game_mode ($0F = level play)        */
  out[1] = smw2_dp_u16(0x0218); /* r_cur_world                         */
  out[2] = smw2_dp_u16(0x021a); /* r_cur_stage / map icon              */
  out[3] = smw2_dp_u16(0x038c); /* r_level_load_type                   */
  out[4] = smw2_dp_u16(0x038e); /* r_cur_screen_exit                   */
}

static uint16_t s_scene[kSceneWords];
static uint64_t s_scene_gen;
static const void *s_cart_seen;
static bool s_scene_known, s_bg1_armed, s_pending_clear;

int Smw2WsBg1Armed(void) { return s_bg1_armed ? 1 : 0; }

/* BG3 is level artwork and a dialogue carrier, not a fixed status strip —
 * never a frame-latched shadow and never a guessed top-band rule. It may
 * render naturally into the margins only while ALL of these hold (bank01
 * $01E2A7 ff. set the level layout): game mode $0F, no message box ($0D0F;
 * EVERY nonzero state is active UI — open, closing, cursor waits), no stage
 * intro ($0121), no camera event (GSU RAM $1E2A, bank09 $0994D7), and the
 * live PPU shape mode-1/BG3-priority/16px tiles, TM bit 2, 32x32 map at
 * bgXsc $34 (VRAM word $3400 — the only value this game ever writes to
 * r_reg_bg3sc_mirror, bank01 $01D021/$01E46F, bank02 $02DBC9). Anything
 * else keeps the native composition. */
static bool smw2_bg3_eligible(void) {
  if (smw2_dp_u16(0x0118) != 0x000f || g_ram[0x0d0f] || g_ram[0x0121] ||
      smw2_gsu_u16(0x1e2a))
    return false;
  const Ppu *p = g_ppu;
  return (p->bgmode & 7) == 1 && (p->bgmode & 0x08) && (p->bgmode & 0x40) &&
         (p->screenEnabled[0] & 0x04) && !PPU_bgTilemapWider(p, kBg3) &&
         !PPU_bgTilemapHigher(p, kBg3) && PPU_bgTilemapAdr(p, kBg3) == 0x3400;
}

void Smw2ResetWsBackgrounds(void) {
  WsShadowReset();
  s_scene_known = false;
  s_bg1_armed = false;
  s_scene_gen = RtlStateGeneration();
  s_cart_seen = g_snes ? g_snes->cart : NULL;
  if (g_ppu)
    PpuSetWidescreenBg3Widen(g_ppu, 0);
}

void Smw2ConfigureWsBgMargins(void) {
  static int on = -1, debug = -1;
  if (on < 0) {
    const char *e = getenv("SNESRECOMP_WS_BG_MARGINS");
    on = !e || e[0] != '0';
    e = getenv("SNESRECOMP_WS_BG_MARGINS_DEBUG");
    debug = e && e[0] != '0';
  }

  /* Epoch detection runs even while widescreen is off, so toggling wide back
   * on never treats a mid-disable scene switch as a fresh transition. */
  const uint64_t gen = RtlStateGeneration();
  const void *cart = g_snes ? g_snes->cart : NULL;
  uint16_t scene[kSceneWords];
  smw2_scene_tuple(scene);
  bool transition = false;
  bool any = false;
  static bool was_active;
  if (!s_scene_known || cart != s_cart_seen) {
    /* Cold observation (or a swapped cart): drop any inherited store, then
     * arm only on a complete proof — no centered frame is owed. */
    WsShadowReset();
    s_bg1_armed = false;
  } else if (gen != s_scene_gen || memcmp(scene, s_scene, sizeof(scene)) != 0) {
    /* Known scene/timeline change: the stored world belongs to another
     * scene. Freeze the gutters for the change frame (an instant reset
     * would flash raw tilemap wrap across both sides until the cold proof
     * lands) and clear the store on the next complete 12/12 proof, the
     * first frame the new scene's mapping is trusted. */
    WsShadowSetFrameHold(kBg1, true);
    s_bg1_armed = false;
    s_pending_clear = true;
    transition = true;
    any = true;
  }
  memcpy(s_scene, scene, sizeof(s_scene));
  s_scene_gen = gen;
  s_cart_seen = cart;
  s_scene_known = true;
  if (transition && g_ppu)
    PpuSetExtraSpaceCentered(g_ppu, g_ppu->extraLeftRight);

  /* BG3 policy is reapplied every frame: eligible → widen naturally from
   * line 1, anything else → zero, including widescreen-off, disabled and
   * transition frames. */
  if (g_ppu) {
    PpuSetWidescreenBg3Widen(
        g_ppu,
        (on && g_ws_active && !transition && smw2_bg3_eligible()) ? 1 : 0);
  }

  if (g_ppu && g_snes && g_snes->cart && on && g_ws_active && !transition) {
    /* Mode = bits 0-2 only: YI levels run BG2 with 16x16 tiles (bit 5), so
     * the X2 mask 0x37 would fold that size bit into the mode test and gate
     * the feature out on exactly the screens it exists for. */
    const unsigned mode = g_ppu->bgmode & 7;
    /* Mode 1/2 only, BG1 small tiles, BG1 wide map (the streamed dual-page
     * window). Cross-section transitions reuse the same pages; validation
     * stands the layer down on any frame the walk can't prove. */
    if ((mode == 1 || mode == 2) && !(g_ppu->bgmode & 0x10) &&
        PPU_bgTilemapWider(g_ppu, kBg1) && (g_ppu->screenEnabled[0] & 1)) {
      int32_t wx, wy;
      smw2_world_origin(kBg1, &wx, &wy);
      const uint32_t buf_tx0 = (uint32_t)g_ppu->hScroll[kBg1] >> 3;
      const uint32_t buf_ty0 = (uint32_t)g_ppu->vScroll[kBg1] >> 3;
      const uint32_t wtx0 = (uint32_t)wx >> 3, wty0 = (uint32_t)wy >> 3;
      /* Until the store is armed, only a complete 12/12 proof qualifies;
       * armed frames keep the existing tolerant 10/12 walk. */
      const bool proven =
          s_bg1_armed ? smw2_bg1_stream_valid(wtx0, wty0, buf_tx0, buf_ty0)
                      : smw2_bg1_stream_proven(wtx0, wty0, buf_tx0, buf_ty0);
      if (!proven && s_bg1_armed) {
        /* Settled store, same scene, eligible layout, but this frame's view
         * walk failed (a staging DMA beat the samples). Freeze: the gutters
         * keep serving last-good history for one frame instead of resetting
         * into raw tilemap wrap and re-proving from cold. */
        WsShadowSetFrameHold(kBg1, true);
        any = true;
      } else if (!proven) {
        WsShadowSetFrameHold(kBg1, false);
      }
      if (proven) {
        WsShadowSetFrameHold(kBg1, false);
        if (s_pending_clear) {
          WsShadowReset();
          s_pending_clear = false;
        }
        s_bg1_armed = true;
        WsShadowSetWorld(kBg1, (uint32_t)wx, (uint32_t)wy);
        WsShadowSetScroll(kBg1, g_ppu->hScroll[kBg1], g_ppu->vScroll[kBg1]);
        WsShadowSetBlankTile(kBg1, -1);
        WsShadowSetNegWorldTile(kBg1, smw2_bg1_outside_entry());
        WsShadowSetRespectGameWrites(kBg1, 60);
        /* Exact gutter fill from the game's own MAP16 tables. ForceTile
         * rather than PrefillTile: history-free, so a stale guess can never
         * outlive one frame; RespectGameWrites still yields to real VRAM
         * staging the game did itself (moving BG objects). A cell the
         * tables can no longer resolve (unloaded screen, streamed-out
         * page) is explicitly invalidated — the previously stored tile
         * must not survive behind the live capture's back. */
        const int extra_tiles = (g_ws_extra + 7) >> 3;
        /* The legacy prune drops every east-of-view cell each frame, which
         * deleted this frame's MAP16 prefill before the raster ever read it
         * (steady east-margin raw-wrap). Retain both gutters' worth. */
        WsShadowSetEastKeep(kBg1, extra_tiles + 2);
        WsShadowSetWestKeep(kBg1, extra_tiles + 2);
        const uint32_t tx0 = (uint32_t)wx >> 3;
        const uint32_t ty0 = (uint32_t)wy >> 3;
        /* Cells the MAP16 tables cannot resolve (screens the GSU has not
         * streamed, rows past the level edge) must never fall through to
         * the rolling tilemap's wrap: that paints far-away terrain into the
         * gutter (the "repeated strip" artifact). Serve the validated empty
         * entry instead; with none available, invalidate as before. */
        const int empty_entry = smw2_bg1_outside_entry();
        /* Matches WsShadowFrame's live capture: cols is clamped to
         * kWsLiveMaxCols (32), so the fine-scroll overhang column tx0+32 is
         * NEVER captured from VRAM and must be force-filled in every phase. */
        /* +1 column each side: with a nonzero world-tile phase the margin's
         * outer pixel column falls one tile beyond extra/8 (measured: the
         * entire east raw-wrap rate was exactly that one column). */
        for (int dx = -extra_tiles - 1; dx < 32 + extra_tiles + 1; dx++) {
          if (dx >= 0 && dx < 32)
            continue; /* native viewport — captured from real VRAM */
          const int32_t txi = (int32_t)tx0 + dx;
          /* Negative world tiles wrap to uint32 and would key the store at
           * the far side of the world; smw2_bg1_tile's screen index then
           * masks into a populated far-east screen and its art lands in the
           * west gutter. The west-of-world bound tile owns those cells. */
          if (txi < 0)
            continue;
          const uint32_t tx = (uint32_t)txi;
          for (int row = -1; row < 30; row++) {
            const int32_t tyi = (int32_t)ty0 + row;
            if (tyi < 0)
              continue;
            const uint32_t ty = (uint32_t)tyi;
            uint16_t entry;
            if (!smw2_bg1_tile(tx, ty, &entry)) {
              if (empty_entry >= 0)
                WsShadowForceTile(kBg1, tx, ty, (uint16_t)empty_entry);
              else
                WsShadowInvalidateTile(kBg1, tx, ty);
            } else {
              WsShadowForceTile(kBg1, tx, ty, entry);
            }
          }
        }
        any = true;
      }
    } else if (on && g_ws_active && !transition && was_active) {
      /* The layout left the provable family this frame (cross-section
       * BGMODE flip, menu): center the native window (backdrop bars in the
       * gutters) and freeze the store; the next eligible proven frame
       * re-arms. Never drop into raw wrap for these frames. */
      PpuSetExtraSpaceCentered(g_ppu, g_ppu->extraLeftRight);
      WsShadowSetFrameHold(kBg1, true);
      any = true;
    }
  }
  if (!any && was_active)
    WsShadowReset();
  was_active = any;
  /* Per-frame latch: moves registered→active and captures the native strip
   * into the world-keyed store. Runs even when nothing registered so a
   * late scene change can never leave an active flag stale. */
  if (g_ppu)
    WsShadowFrame(g_ppu);
  if (debug) {
    static unsigned calls;
    if (!(calls++ % 120))
      fprintf(stderr, "[smw2_ws] bg1_active=%d extra=%d armed=%d bg3w=%u\n",
              WsShadowLayerActive(kBg1), g_ws_extra, s_bg1_armed,
              g_ppu ? g_ppu->wsBg3WidenY : 0);
  }
}