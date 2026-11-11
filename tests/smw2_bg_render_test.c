/* BG renderer contract over the widescreen field: real ppu.c raster paths
 * (old + new dispatchers), register state staged directly like
 * tests/snesrecomp/ppu/ppu_oam_coordinates_test.c.
 *
 * Covers what Task 4 must prove pixel-accurate in the widened field:
 *   - BG1 32x32 / 8x8 tiles: margin columns, scroll wrap, page boundary
 *   - BG2 32x64 / 16x16 tiles (the YI composition): quadrant + vertical
 *     page crossing
 *   - BG3 32x32 / 2bpp: clamped-256 policy and PpuSetWidescreenBg3Widen band
 *   - per-line hscroll/vscroll variation (raster splits)
 *   - hardware windows (screenWindowed + windowsel + window1left/right)
 *   - brightness scaling (inidisp)
 *   - color math vs a subscreen layer (cgwsel/cgadsub)
 *   - mosaic (the ppu.c:1839 path)
 *   - upper-plane VRAM reads (the unsigned-shift fix sites)
 * Everything runs under BOTH renderers (flags 0 and
 * kPpuRenderFlags_NewRenderer) — the fixed 4bpp fast path is the old
 * renderer's. */

#include "snes/ppu.h"
#include "snes/ppu_brightness.h"
#include "snes/snes.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* externs ppu.c needs from the rest of the runner */
Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
#ifndef SMW2_LINK_REAL_WS_SHADOW
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
#endif /* SMW2_LINK_REAL_WS_SHADOW */

/* Compatibility adapter: pre-fix ws_shadow.c has no WsShadowSetNegWorldTile.
 * The weak no-op links then and produces behavioral RED (the outside tile
 * is never served); with the real symbol this definition is discarded. */
void WsShadowSetNegWorldTile(int layerIndex, int entry);
__attribute__((weak)) void WsShadowSetNegWorldTile(int layerIndex,
                                                   int entry) {
  (void)layerIndex;
  (void)entry;
}
void WsShadowReset(void);
void WsShadowSetWorld(int layer, uint32_t worldX, uint32_t worldY);
void WsShadowSetScroll(int layer, uint32_t scrollX, uint32_t scrollY);
void WsShadowForceTile(int layer, uint32_t worldTileX, uint32_t worldTileY,
                       uint16_t entry);
void WsShadowFrame(const struct Ppu *ppu);
bool WsShadowLayerActive(int layer);

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++cases;                                                                   \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: %s FAILED\n", __FILE__, __LINE__, #cond);        \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define COL(x) ((x) + kPpuExtraLeftRight)

static Ppu p;
static uint32_t frame[kPpuBufWidth * 240];
static unsigned cases;
static const char *cur_case = "?";

static uint32_t px(int x, int row) {
  return frame[(size_t)row * kPpuBufWidth + COL(x)];
}
static unsigned emit5(unsigned v) { return (v << 3) | (v >> 2); }
static uint32_t emit_rgb(unsigned r, unsigned g, unsigned b) {
  return emit5(b) | (emit5(g) << 8) | (emit5(r) << 16);
}
static uint32_t emit_color(uint16_t c) {
  return emit_rgb(c & 31, (c >> 5) & 31, (c >> 10) & 31);
}

/* ---- staging helpers ---- */

/* map entry at tile-cell (x,y); y beyond a page selects vertical page. */
static void map_entry(int layer, int x, int y, uint16_t entry) {
  unsigned base = (unsigned)p.bgXsc[layer] >> 2 << 10; /* (bgXsc&0xfc)<<8 */
  unsigned page =
      (unsigned)((y >> 5) & 1) * ((p.bgXsc[layer] & 1) ? 0x800 : 0x400);
  p.vram[(base + page + ((unsigned)y & 31u) * 32u + ((unsigned)x & 31u)) &
         0x7fff] = entry;
}
/* same, for x in a second horizontal page (32x64 / 64x32 maps) */
static void map_entry_hp(int layer, int x, int y, uint16_t entry) {
  unsigned base = (unsigned)p.bgXsc[layer] >> 2 << 10;
  unsigned page =
      ((unsigned)(x >> 5) & 1u) * 0x400u +
      (unsigned)((y >> 5) & 1) * ((p.bgXsc[layer] & 1) ? 0x800 : 0x400);
  p.vram[(base + page + ((unsigned)y & 31u) * 32u + ((unsigned)x & 31u)) &
         0x7fff] = entry;
}

/* 4bpp 8x8 tile at the fixture's char base (word 0x2000, bgTileAdr
 * nibble 2 — the map lives at 0x0000, so chars can never start at 0). */
static void tile8_4bpp(int t, int pix) {
  int a = 0x2000 + t * 16;
  for (int r = 0; r < 8; r++) {
    p.vram[a + r] =
        (uint16_t)(((pix & 1) ? 0xff : 0) | ((pix & 2) ? 0xff00 : 0));
    p.vram[a + 8 + r] =
        (uint16_t)(((pix & 4) ? 0xff : 0) | ((pix & 8) ? 0xff00 : 0));
  }
}
/* 2bpp 8x8 tile: uniform colour pix (0..3). */
static void tile8_2bpp(int t, int pix) {
  int a = 0x2000 + t * 8;
  for (int r = 0; r < 8; r++)
    p.vram[a + r] =
        (uint16_t)(((pix & 1) ? 0xff : 0) | ((pix & 2) ? 0xff00 : 0));
}
/* 16x16 tile = chars n, n+1, n+16, n+17 (4bpp). */
static void tile16_4bpp(int t, int pix) {
  tile8_4bpp(t, pix);
  tile8_4bpp(t + 1, pix);
  tile8_4bpp(t + 16, pix);
  tile8_4bpp(t + 17, pix);
}

static void fixture(unsigned flags) {
  memset(&p, 0, sizeof p);
  ppu_reset(&p);
  p.inidisp = 15;
  p.bgmode = 1;
  p.cgram[0] = 0x0000; /* black backdrop so "nothing drawn" is provable */
  PpuSetExtraSpace(&p, kPpuExtraLeftRight);
  memset(frame, 0x5A, sizeof frame);
  PpuBeginDrawing(&p, (uint8_t *)frame, kPpuBufWidth * 4, flags);
  ppu_runLine(&p, 0); /* prep line: mosaic table, sprite eval, no row out */
}

/* Render output row `row`. ppu_runLine(n) samples BG source line n and
 * writes framebuffer row n-1, so row r comes from runLine(r+1) — the BG
 * source y is r+1, not r (map row = (r+1+vScroll)>>3). */
static void draw_row(int row) {
  ++snes_frame_counter;
  ppu_runLine(&p, row + 1);
}

static void both_renderers(const char *name, void (*fn)(unsigned)) {
  cur_case = name;
  fn(0);
  fn(kPpuRenderFlags_NewRenderer);
}

/* ===================================================================== */

/* BG1 32x32 map @0, 4bpp tiles @0. Cell (x,y) → tile T=(x+y*32)&63 with
 * uniform colour = (T & 15) | 1 so adjacent cells differ in the low bits.
 * Proves margin columns render real map content and scroll math wraps the
 * map correctly across the 256 seam. */
static void bg1_margins_and_scroll(unsigned flags) {
  fixture(flags);
  p.screenEnabled[0] = 1;
  p.bgXsc[0] = 0x00; /* 32x32 @ word 0 */
  p.bgTileAdr = 0x2;   /* BG1 chars @ word 0x2000 */
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      int t = (x + y * 32) & 63;
      map_entry(0, x, y, (uint16_t)t);
      tile8_4bpp(t, (t & 15) | 1);
    }
  for (int c = 1; c < 16; c++)
    p.cgram[c] = (uint16_t)(c * 0x0421); /* distinct rgb15 per colour */

  /* no scroll: world x = screen x (mod 512 wrap) */
  p.hScroll[0] = 0;
  p.vScroll[0] = 0;
  draw_row(0);
  /* screen x=-272 → world -272 → x&0x1ff=240 → map cell 30,row0 → T=30 */
  CHECK(px(-272, 0) == emit_color(p.cgram[(30 & 15) | 1]));
  /* x=0 → cell 32 → T=32 */
  CHECK(px(0, 0) == emit_color(p.cgram[(32 & 15) | 1]));
  /* x=255 → cell 63 → T=63 */
  CHECK(px(255, 0) == emit_color(p.cgram[(63 & 15) | 1]));
  /* x=271 → cell 65 → wraps to map x 33 (32x32: no second page) → T=33 */
  CHECK(px(271, 0) == emit_color(p.cgram[(33 & 15) | 1]));

  /* hScroll=200 shifts the window; page-1 cells are unreachable in a 32x32
   * map so the row is a pure modulo-32 wrap of page 0. */
  p.hScroll[0] = 200;
  draw_row(0);
  /* x=0 → world 200 → cell 25 → T=25 */
  CHECK(px(0, 0) == emit_color(p.cgram[(25 & 15) | 1]));
  /* x=-272 → world -72 → &0x1ff = 440 → cell 55? 440>>3 = 55 &31 → 23 →
   * T=23. Wrap math: -72 mod 512 = 440; (440>>3)&0x1f = 55&31 = 23. */
  CHECK(px(-272, 0) == emit_color(p.cgram[(23 & 15) | 1]));
  /* x=271 → world 471 → cell 58&31=26 → T=26 */
  CHECK(px(271, 0) == emit_color(p.cgram[(26 & 15) | 1]));

  /* vScroll: line renders row (y+vScroll). row0 y=0, vScroll=80 → map row
   * 10 → T = cell+320&63 = cell+0 (320&63=0)... use vScroll=88 → row 11 →
   * +352&63 = +32. x=0: world x0 → cell 0 → T = (0+352)&63 = 32. */
  p.hScroll[0] = 0;
  p.vScroll[0] = 88;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(p.cgram[(32 & 15) | 1]));
}

/* BG2 32x64 map @word 0x1000 (bgXsc=0x12), 16x16 tiles (bgmode bit5), 4bpp
 * chars @ word 0x2000 (bgTileAdr nibble1=2). Cells carry tile index
 * (cx + cy*16), uniform colour (idx&15)|1, palette 0. Proves the big-tile
 * path over margins, horizontal page crossing, and the vertical page
 * (y&0x100 → +0x800) boundary. */
static void bg2_bigtile_pages(unsigned flags) {
  fixture(flags);
  p.screenEnabled[0] = 2;
  p.bgmode = 0x21;   /* mode 1, BG2 big tiles */
  p.bgXsc[1] = 0x12; /* 32x64 map @ 0x1000 — the YI composition */
  p.bgTileAdr = 0x0020;
  /* The 16x16 path (PpuReadTilemapEntry) addresses the map in 16px cells:
   * cell (mx,my) = vram[map + ((my&31)<<5) + (mx&31)], +0x800 on the second
   * vertical page (my 32..63), +0x400 on a second horizontal page (mx
   * 32..63, wider maps only). Cell → entry e = (mx*3 + my*5) & 0x3f; the
   * entry renders as 16x16 tile 2e (chars {2e,2e+1,2e+16,2e+17} — spaced so
   * neighbouring tiles never share a character) with palette (e>>3) and
   * uniform pixel (e&7)+1, giving a unique cgram slot per e. */
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 32; x++) {
      unsigned e = ((unsigned)x * 3 + (unsigned)y * 5) & 0x3f;
      tile16_4bpp(2 * (int)e, (int)(e & 7) + 1);
      p.cgram[(e >> 3) * 16 + (e & 7) + 1] = (uint16_t)e;
      map_entry_hp(1, x, y, (uint16_t)((e << 1) | ((e >> 3) << 10)));
    }

  /* world x = screen x; cell = (x>>4)&31, row = (y>>4)&31 (coord_mask
   * 0x3ff: 1024px world wrap before the map's own page select). */
  p.hScroll[1] = 0; p.vScroll[1] = 0;
  draw_row(0);
  /* x=-272 → sx -272&0x3ff = 752 → cell 47&31 = 15, row 0 → e = 45 */
  CHECK(px(-272, 0) == emit_color(45));
  /* x=0 → cell 0 → e=0 */
  CHECK(px(0, 0) == emit_color(0));
  /* x=255 → cell 15 → e=45 */
  CHECK(px(255, 0) == emit_color(45));
  /* x=271 → cell 16 → e=48 */
  CHECK(px(271, 0) == emit_color(48));

  /* horizontal scroll: hScroll=300 → x=0 → sx 300 → cell 18 → e = 54 */
  p.hScroll[1] = 300;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(54));

  /* vertical scroll crossing the 512px page boundary: vScroll=512 →
   * sy=512 → row 32 → (ty>>5)&1 && higher → +0x800 → page-1 row 0.
   * x=0 → cell 0 → e for (0,32) = 160&0x3f = 32. */
  p.hScroll[1] = 0;
  p.vScroll[1] = 512;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(32));
  /* x=271 → cell 16 → e for (16,32) = 208&0x3f = 16 */
  CHECK(px(271, 0) == emit_color(16));

  /* inside the page, nonzero row: vScroll=304 → sy=304 → row 19 → e for
   * (0,19) = 95&0x3f = 31 */
  p.vScroll[1] = 304;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(31));
}

/* BG2 64x32 WIDER map (bgXsc bit 0 set, bit 1 clear): the horizontal
 * second-page select — PpuReadTilemapEntry adds +0x400 when tx>=32
 * (ppu.c:1460-1461; legacy: x&0x200 && wider → +0x400 at ppu_legacy.c:314).
 * Positive-pixel proof that the second horizontal page is sourced, in
 * addition to bg2_bigtile_pages' vertical +0x800 transition. */
static void bg2_horizontal_page(unsigned flags) {
  fixture(flags);
  p.screenEnabled[0] = 2;
  p.bgmode = 0x21;   /* mode 1, BG2 big tiles */
  p.bgXsc[1] = 0x11; /* 64x32 map @ 0x0400 — wider, NOT higher */
  p.bgTileAdr = 0x0020;
  /* Same cell scheme as bg2_bigtile_pages but across the full 64-cell
   * width: cells 32..63 land at map+0x400 via map_entry_hp. */
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 64; x++) {
      unsigned e = ((unsigned)x * 3 + (unsigned)y * 5) & 0x3f;
      tile16_4bpp(2 * (int)e, (int)(e & 7) + 1);
      p.cgram[(e >> 3) * 16 + (e & 7) + 1] = (uint16_t)e;
      map_entry_hp(1, x, y, (uint16_t)((e << 1) | ((e >> 3) << 10)));
    }

  p.hScroll[1] = 0; p.vScroll[1] = 0;
  draw_row(0);
  /* x=0 → cell 0 → e=0 (first page) */
  CHECK(px(0, 0) == emit_color(0));
  /* x=527 → cell 32 → (tx>>5)&1 && wider → +0x400 → e = 96&0x3f = 32.
   * This is the discriminating pixel: a narrower map would wrap to
   * cell 0 and emit e=0. */
  CHECK(px(527, 0) == emit_color(32));
  /* x=255 → cell 15 → e=45, still first page */
  CHECK(px(255, 0) == emit_color(45));
  /* margin x=-272 → sx 752 → cell 47 → tx>=32 → +0x400 → e = 141&0x3f=13 */
  CHECK(px(-272, 0) == emit_color(13));

  /* hscroll places the page seam inside the visible field: hScroll=300 →
   * x=0 → sx 300 → cell 18 → e=54; x=227 → sx 527 → cell 32 → e=32. */
  p.hScroll[1] = 300;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(54));
  CHECK(px(227, 0) == emit_color(32));
}

/* BG3 (layer 2, 2bpp) margin policy: default → clamped to the authentic
 * 256 columns; PpuSetWidescreenBg3Widen(y0) → widens on lines ≥ y0 only. */
static void bg3_widen_policy(unsigned flags) {
  /* The BG3 margin policy lives in the new compositor
   * (PpuWidescreenLayerExtra / PpuWindows_Clear): the legacy renderer
   * draws every layer to its own width and never clamps BG3. Assert each
   * engine's real contract. */
  const int new_renderer = (flags & kPpuRenderFlags_NewRenderer) != 0;
  fixture(flags);
  p.screenEnabled[0] = 4;
  p.bgmode = 0x01; /* mode 1: BG3 2bpp */
  p.bgXsc[2] = 0x00;
  p.bgTileAdr = 0x200;
  tile8_2bpp(1, 3);
  p.cgram[3] = 0x03e0;
  for (int x = 0; x < 32; x++)
    map_entry(2, x, 0, 1);
  draw_row(0);
  CHECK(px(0, 0) == emit_color(0x03e0));
  CHECK(px(255, 0) == emit_color(0x03e0));
  if (!new_renderer) {
    /* legacy: natural layer render in margins (world wrap) */
    CHECK(px(-200, 0) == emit_color(0x03e0));
    CHECK(px(300, 0) == emit_color(0x03e0));
  } else {
    /* default policy: BG3 clamped to the authentic 256 columns */
    CHECK(px(-200, 0) == emit_color(p.cgram[0]));
    CHECK(px(300, 0) == emit_color(p.cgram[0]));
    CHECK(px(-272, 0) == emit_color(p.cgram[0]));
    CHECK(px(527, 0) == emit_color(p.cgram[0]));
  }

  /* widen band: PpuSetWidescreenBg3Widen(y0) lets lines >= y0 extend. */
  fixture(flags);
  p.screenEnabled[0] = 4;
  p.bgmode = 0x01;
  p.bgXsc[2] = 0x00;
  p.bgTileAdr = 0x200;
  tile8_2bpp(1, 3);
  p.cgram[3] = 0x03e0;
  for (int x = 0; x < 32; x++)
    map_entry(2, x, 0, 1);
  PpuSetWidescreenBg3Widen(&p, 3);
  draw_row(0); /* line 1 < 3 */
  draw_row(1); /* line 2 < 3 */
  draw_row(2); /* line 3 >= 3: widened */
  if (!new_renderer) {
    CHECK(px(-200, 0) == emit_color(0x03e0));
    CHECK(px(-200, 1) == emit_color(0x03e0));
    CHECK(px(-200, 2) == emit_color(0x03e0));
  } else {
    CHECK(px(-200, 0) == emit_color(p.cgram[0]));
    CHECK(px(-200, 1) == emit_color(p.cgram[0]));
    CHECK(px(0, 2) == emit_color(0x03e0));
    /* margin cell at screen -200 → world -200&0x1ff = 312 → cell 7 →
     * tile 1 → drawn on the widened line */
    CHECK(px(-200, 2) == emit_color(0x03e0));
    CHECK(px(300, 2) == emit_color(0x03e0));
  }
}

/* Raster splits: hscroll/vscroll change between consecutive runLine calls
 * on the same frame — each row samples its own scroll. */
static void perline_scroll(unsigned flags) {
  fixture(flags);
  p.screenEnabled[0] = 1;
  p.bgXsc[0] = 0;
  p.bgTileAdr = 0x2;
  tile8_4bpp(10, 5);
  tile8_4bpp(11, 6);
  p.cgram[5] = 0x1234;
  p.cgram[6] = 0x5678;
  /* row band A: map row 0 all tile 10; row band B: map row 2 all tile 11 */
  for (int x = 0; x < 32; x++) {
    map_entry(0, x, 0, 10);
    map_entry(0, x, 1, 10);
    map_entry(0, x, 2, 11);
    map_entry(0, x, 3, 11);
  }
  /* rows 0..3 use vScroll=0 → map rows 0,1 → tile 10; rows 4..5 flip to
   * vScroll=16 → map rows 2,3 → tile 11 (mid-frame scroll write). */
  p.vScroll[0] = 0;
  for (int r = 0; r < 4; r++) {
    draw_row(r);
    CHECK(px(100, r) == emit_color(0x1234));
  }
  p.vScroll[0] = 16;
  for (int r = 4; r < 6; r++) {
    draw_row(r);
    CHECK(px(100, r) == emit_color(0x5678));
  }
  /* hscroll mid-frame: rows 6,7 differ horizontally. */
  p.vScroll[0] = 0;
  p.hScroll[0] = 0;
  draw_row(6); /* y=7 → map row 0 */
  /* x=0 → world 0 → cell 0 → tile 10 → 0x1234 */
  CHECK(px(0, 6) == emit_color(0x1234));
  /* row 7 samples source y=8 → map row 1: restage row 1 with an
   * alternating pattern so the mid-frame hscroll write is observable */
  p.hScroll[0] = 64;
  for (int x = 0; x < 32; x++)
    map_entry(0, x, 1, (x & 1) ? 10 : 11);
  draw_row(7);
  /* x=0 → world 64 → cell 8 → even → tile 11 → 0x5678 */
  CHECK(px(0, 7) == emit_color(0x5678));
  /* x=8 → world 72 → cell 9 → odd → tile 10 → 0x1234 */
  CHECK(px(8, 7) == emit_color(0x1234));
  /* margin x=-272 → world -208 → &0x1ff = 304 → cell 38&31=6 → tile 11 */
  CHECK(px(-272, 7) == emit_color(0x5678));
}

/* Hardware window on BG1: window1 masks the layer inside [l,r]; margins
 * outside the window show backdrop. windowsel layer0 nibble: enable+inv
 * bits kWindow1Enabled=2 in nibble 0. */
static void bg1_window_mask(unsigned flags) {
  fixture(flags);
  p.screenEnabled[0] = 1;
  p.screenWindowed[0] = 1;       /* BG1 windowed on main screen */
  p.windowsel = kWindow1Enabled; /* layer0 nibble = 0b0010 = window1 on */
  p.window1left = 100;
  p.window1right = 200;
  p.bgXsc[0] = 0;
  p.bgTileAdr = 0x2;
  tile8_4bpp(10, 5);
  p.cgram[5] = 0x1234;
  for (int x = 0; x < 32; x++)
    map_entry(0, x, 0, 10);
  draw_row(0);
  /* win.bits marks DISABLED regions; an enabled (non-inversed) window
   * masks its interior: inside [100,200] → backdrop, outside → drawn. */
  CHECK(px(0, 0) == emit_color(0x1234));        /* outside → drawn */
  CHECK(px(99, 0) == emit_color(0x1234));
  CHECK(px(100, 0) == emit_color(p.cgram[0])); /* inside → masked */
  CHECK(px(200, 0) == emit_color(p.cgram[0]));
  CHECK(px(201, 0) == emit_color(0x1234));     /* outside → drawn */
  CHECK(px(-272, 0) == emit_color(0x1234));    /* margin drawn */
  CHECK(px(527, 0) == emit_color(0x1234));

  /* inversed window: inside drawn, outside (incl. margins) masked */
  p.windowsel = kWindow1Enabled | kWindow1Inversed;
  draw_row(0);
  CHECK(px(150, 0) == emit_color(0x1234));      /* inside → drawn */
  CHECK(px(0, 0) == emit_color(p.cgram[0]));   /* outside → masked */
  CHECK(px(-200, 0) == emit_color(p.cgram[0]));
  CHECK(px(300, 0) == emit_color(p.cgram[0]));
}

/* Brightness: inidisp scales every emitted channel through
 * brightnessMult[v] = emit5(v)*b/15. Prove at inidisp 15 vs 8. */
static void bg1_brightness(unsigned flags) {
  fixture(flags);
  p.screenEnabled[0] = 1;
  p.bgXsc[0] = 0;
  p.bgTileAdr = 0x2;
  tile8_4bpp(10, 5);
  p.cgram[5] = 0x7fff; /* white rgb15 */
  for (int x = 0; x < 32; x++)
    map_entry(0, x, 0, 10);
  p.inidisp = 15;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(0x7fff)); /* 255,255,255 */
  p.inidisp = 8;
  draw_row(0);
  {
    /* Hardware scales in the 5-bit domain with rounding, then expands
     * (ppu_brightness.h, bsnes lightTable): round(31*8/15)=17 -> 140. */
    unsigned v = ppu_brightness_level(31, 8);
    CHECK(px(0, 0) == (v | (v << 8) | (v << 16)));
    /* and margins too */
    CHECK(px(-272, 0) == (v | (v << 8) | (v << 16)));
  }
}

/* Color math: BG1 on mainscreen, BG2 on subscreen; cgwsel addSubscreen +
 * cgadsub math on layer0. main red + sub green → additive clamp. */
static void bg1_color_math(unsigned flags) {
  fixture(flags);
  p.screenEnabled[0] = 1; /* main: BG1 */
  p.screenEnabled[1] = 2; /* sub: BG2 */
  p.bgmode = 1;
  p.bgXsc[0] = 0;    /* BG1 map @ word 0 */
  p.bgXsc[1] = 0x04; /* BG2 map @ word 0x400 — separate from BG1's */
  p.bgTileAdr = 0x22; /* both layers' chars @ word 0x2000 */
  tile8_4bpp(10, 5);
  tile8_4bpp(11, 6);
  p.cgram[5] = 0x001f; /* r=31 */
  p.cgram[6] = 0x03e0; /* g=31 */
  for (int x = 0; x < 32; x++) {
    map_entry(0, x, 0, 0x2000 | 10); /* BG1 high-priority → wins the merge */
    map_entry(1, x, 0, 11);
  }
  /* math: cgwsel bit1 addSubscreen; cgadsub bit0 math on BG1 */
  p.cgwsel = 0x02;
  p.cgadsub = 0x01; /* mathEnabled layer0, no half, add */
  draw_row(0);
  {
    /* (31,0,0)+(0,31,0) = (31,31,0) → emit */
    uint32_t want = emit_rgb(31, 31, 0);
    CHECK(px(0, 0) == want);
    CHECK(px(-272, 0) == want); /* margins math too */
  }
  /* subtract: r-g clamps g to 0 → pure red */
  p.cgadsub = 0x81;
  draw_row(0);
  CHECK(px(0, 0) == emit_rgb(31, 0, 0));
}

/* Mosaic on BG1: PPU_mosaicSize-px blocks share one source pixel. Drives
 * the second small-tile READ_BITS site (ppu.c:1839) plus mosaicModulo. */
static void bg1_mosaic(unsigned flags) {
  /* Reference row without mosaic: every 8px cell its own tile/colour. */
  uint32_t ref[8];
  fixture(flags);
  p.screenEnabled[0] = 1;
  p.bgXsc[0] = 0;
  p.bgTileAdr = 0x2;
  for (int x = 0; x < 32; x++)
    map_entry(0, x, 0, (uint16_t)(20 + x));
  for (int t = 0; t < 32; t++) {
    tile8_4bpp(20 + t, (t & 15) | 1);
    p.cgram[(t & 15) | 1] = (uint16_t)(0x6000 + t);
  }
  draw_row(0);
  ref[0] = px(0, 0);
  ref[1] = px(8, 0);
  ref[2] = px(16, 0);
  ref[3] = px(24, 0);
  ref[4] = px(-16, 0); /* margin reference */
  ref[5] = px(-272, 0);
  ref[6] = px(271, 0);
  /* mosaic creates size-pixel blocks that share one source pixel; exact
   * block contents depend on the engine's own modulo table (indexed by
   * the screen x itself), which we verify behaviourally:
   * every block of `size` px is uniform, adjacent blocks differ, and the
   * margin columns (mosaic table only spans 0..255) keep their reference
   * values. */
  const int sizes[3] = {2, 4, 8};
  for (int si = 0; si < 3; si++) {
    int size = sizes[si];
    fixture(flags);
    p.screenEnabled[0] = 1;
    p.bgXsc[0] = 0;
    p.bgTileAdr = 0x2;
    p.mosaic = (uint8_t)(((size - 1) << 4) | 1);
    for (int x = 0; x < 32; x++)
      map_entry(0, x, 0, (uint16_t)(20 + x));
    for (int t = 0; t < 32; t++) {
      tile8_4bpp(20 + t, (t & 15) | 1);
      p.cgram[(t & 15) | 1] = (uint16_t)(0x6000 + t);
    }
    draw_row(0);
    /* block uniformity: within each size-block the colour is constant */
    for (int b = 0; b + size <= 64; b += size) {
      uint32_t c0 = px(b, 0);
      for (int x = b + 1; x < b + size; x++)
        CHECK(px(x, 0) == c0);
    }
    /* every emitted colour comes from the staged tile palette — the
     * mosaic path sampled real cells, not garbage */
    for (int x = 0; x < 64; x++) {
      uint32_t c = px(x, 0);
      int ok = 0;
      for (int t = 0; t < 32; t++)
        ok |= (c == emit_color((uint16_t)(0x6000 + t)));
      CHECK(ok);
    }
    /* snapped runs are at least `size` px long (mono runs can't be
     * shorter than the snap granularity) */
    for (int x = 1, run = 1; x <= 64; x++) {
      if (x < 64 && px(x, 0) == px(x - 1, 0)) {
        run++;
        continue;
      }
      CHECK(run >= size);
      run = 1;
    }
    /* margins (mosaic table only spans 0..255): identical to reference */
    CHECK(px(-16, 0) == ref[4]);
    CHECK(px(-272, 0) == ref[5]);
    CHECK(px(271, 0) == ref[6]);
    (void)ref;
  }
}

/* The unsigned-shift fix sites: a tile index whose (ta + tile*16) lands
 * the second plane pair high in VRAM so addr[8] >= 0x8000 patterns are
 * exercised — the pre-fix expression shifted the promoted uint16 left by
 * 16 (UB). Behaviour-level proof: correct colour + no sanitizer abort on
 * the small 8x8 path AND the mosaic path AND the 16x16 generic path. */
static void bg_upper_plane(unsigned flags) {
  /* small 8x8 path (site 1, ~line 1252): tile 511 → char words 8176..8191,
   * plane23 at 8184.. → addr[8] = vram[8184+r] = 0xffff → old code UB. */
  fixture(flags);
  p.screenEnabled[0] = 1;
  p.bgXsc[0] = 0;
  p.bgTileAdr = 0x2;
  for (int x = 0; x < 32; x++)
    map_entry(0, x, 0, 511);
  tile8_4bpp(511, 0xf); /* all planes 0xff → pixel 15 */
  p.cgram[15] = 0x4321;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(0x4321));
  CHECK(px(-272, 0) == emit_color(0x4321));

  /* mosaic path (site 2, ~line 1839): same tile through the size-4 path */
  fixture(flags);
  p.screenEnabled[0] = 1;
  p.bgXsc[0] = 0;
  p.bgTileAdr = 0x2;
  p.mosaic = 0x31;
  for (int x = 0; x < 32; x++)
    map_entry(0, x, 0, 511);
  tile8_4bpp(511, 0xf);
  p.cgram[15] = 0x4321;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(0x4321));

  /* 16x16 big-tile path on BG2 (reads plane23 at +8 through the generic
   * path — evidence case 'b' stayed clean, keep it green). */
  fixture(flags);
  p.screenEnabled[0] = 2;
  p.bgmode = 0x21;
  p.bgXsc[1] = 0x12;
  p.bgTileAdr = 0x0020;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 32; x++)
      map_entry_hp(1, x, y, 300);
  tile16_4bpp(300, 0xf);
  p.cgram[15] = 0x4321;
  draw_row(0);
  CHECK(px(0, 0) == emit_color(0x4321));
  CHECK(px(271, 0) == emit_color(0x4321));
}


/* Task5: a known negative-worldX west margin must render the armed outside
 * tile in the shadow-capable (optimized) renderer — never the rolling
 * tilemap's far-page wrap. The east margin (positive worldX miss) and
 * native pixels keep authentic map wrap/store content. The compatibility
 * renderer has no ws_shadow integration at all (not one WsShadow* call in
 * ppu_legacy.c), so its margin output stays the hardware-authentic map
 * wrap — the same non-regression contract bg3_widen_policy uses above.
 * Real ws_shadow.c is linked (SMW2_LINK_REAL_WS_SHADOW); the layer is
 * armed through the same SetWorld/ForceTile calls the title uses.
 *
 * Coverage: all 8 fine-scroll phases (worldX == hScroll, so the neg-world
 * bound lands on every tile phase), the full 272px west gutter, the
 * worldX=0 seam inside the first native chunk, camX=256 positive-west hit
 * AND miss, and disarm/reset/native-off controls. */
/* Expected margin pixel colour for an unmodified map-wrap fetch at screen
 * x (map at word 0, row 0, uniform tiles so colour index == tile index). */
static uint32_t wrap_color(int x) {
  unsigned col = ((unsigned)x >> 3) & 31u;
  return emit_color(p.cgram[p.vram[col] & 0x3ff]);
}

static void negworld_setup(uint32_t wx) {
  p.screenEnabled[0] = 1;
  p.bgXsc[0] = 0x00; /* 32x32 map @ word 0 */
  p.bgTileAdr = 0x2; /* BG1 chars @ word 0x2000 */
  p.cgram[1] = 0x03e0; /* outside tile palette idx 1: green */
  p.cgram[2] = 0x001f; /* wrapped strip tile idx 2: red */
  p.cgram[3] = 0x7c00; /* native col0 tile idx 3: blue */
  p.cgram[4] = 0x0020; /* world tile 34 idx4: brown */
  p.cgram[5] = 0x4210; /* world tile 63 idx5 (west store hit) */
  p.cgram[6] = 0x2100; /* wrap marker B idx6: light gray */
  tile8_4bpp(1, 1);    /* tile 1 = uniform idx1 */
  tile8_4bpp(2, 2);    /* tile 2 = uniform idx2 */
  tile8_4bpp(3, 3);
  tile8_4bpp(4, 4);
  tile8_4bpp(5, 5);
  tile8_4bpp(6, 6);
  map_entry(0, 0, 0, 0x0003); /* worldX 0 native col0 -> idx3 (seam) */
  map_entry(0, 1, 0, 0x0002); /* east wrap of sx=264 -> col1 -> idx2 */
  map_entry(0, 31, 0, 0x0002); /* west wrap of sx=-8 -> col31 -> idx2 */
  map_entry(0, 2, 0, 0x0006); /* wrap marker B (col2) */
  map_entry(0, 4, 0, 0x0006); /* wrap marker B (col4) */
  WsShadowReset();
  WsShadowSetWorld(0, wx, 0);
  WsShadowSetScroll(0, wx, 0);
  WsShadowForceTile(0, 33, 0, 0x0004); /* worldX 264..271 -> tile4 */
  /* camX256 store proofs OUTSIDE the frame-captured key range (the
   * capture writes worldX>>3 = 32..63 at camX256): wT0 covers positive
   * west worldX 0..7 (sx -256..-249); wT64 covers east wX 512..519
   * (sx 256..263). */
  WsShadowForceTile(0, 0, 0, 0x0005);
  WsShadowForceTile(0, 64, 0, 0x0004);
  WsShadowSetNegWorldTile(0, 0x0001); /* outside = tile1, pal0 */
  WsShadowFrame(&p);
  CHECK(WsShadowLayerActive(0));
}

static void bg1_negworld_margin(unsigned flags) {
  const int new_renderer = (flags & kPpuRenderFlags_NewRenderer) != 0;
  fixture(flags);
  negworld_setup(0);

  /* Phase A — worldX == hScroll == 0: full 272px west gutter, every west
   * phase position. Chunks fetch at sx -272..-9 step 8, all worldX<0. */
  p.hScroll[0] = 0;
  draw_row(0);
  if (new_renderer) {
    for (int x = -kPpuExtraLeftRight; x < 0; x++)
      CHECK(px(x, 0) == emit_color(p.cgram[1]));
    /* worldX 0 seam + native untouched. */
    CHECK(px(0, 0) == emit_color(p.cgram[3]));
    /* East margin positive worldX: store hit at wT33 (sx 264..271),
     * unproven miss at sx 280..287 keeps the wrapped col3 (empty). */
    CHECK(px(264, 0) == emit_color(p.cgram[4]));
    CHECK(px(280, 0) == emit_color(p.cgram[0]));
  } else {
    /* legacy: authentic map wrap — cols 28..31 red? col31/col1 red,
     * col0 blue, rest empty->backdrop, regardless of the option. */
    CHECK(px(-8, 0) == emit_color(p.cgram[2]));
    CHECK(px(-16, 0) == emit_color(p.cgram[0]));
    CHECK(px(-43, 0) == emit_color(p.cgram[0]));
    CHECK(px(264, 0) == emit_color(p.cgram[2]));
  }

  /* Phase B — the other 7 fine-scroll phases. worldX = hScroll makes the
   * neg-world bound hit every in-tile phase: for phase hs, sx -hs..-1
   * remains worldX 0..hs-1 (native chunk content), everything west is
   * worldX<0. */
  for (unsigned hs = 1; hs < 8; hs++) {
    fixture(flags);
    negworld_setup(hs); /* worldX == hScroll: neg bound at every phase */
    p.hScroll[0] = (uint16_t)hs;
    draw_row(0);
    if (new_renderer) {
      /* Full neg-world west gutter for this phase: sx < -hs. */
      for (int x = -kPpuExtraLeftRight; x < -(int)hs; x++)
        CHECK(px(x, 0) == emit_color(p.cgram[1]));
      /* Seam: sx -hs..-1 are worldX 0..hs-1 — the first native chunk's
       * trailing pixels of world tile 0 (idx3), NOT the outside tile. */
      for (int x = -(int)hs; x < 0; x++)
        CHECK(px(x, 0) == emit_color(p.cgram[3]));
    } else {
      /* legacy wrap, scroll-shifted: x = -hs..-1 samples lx 0..hs-1,
       * the native col0 tile (idx3); x = -(hs+8)..-(hs+1) wraps to the
       * col31 strip (red); one chunk deeper lands in an empty column. */
      for (int x = -(int)hs; x < 0; x++)
        CHECK(px(x, 0) == emit_color(p.cgram[3]));
      for (int x = -(int)hs - 8; x < -(int)hs; x++)
        CHECK(px(x, 0) == emit_color(x < -(int)hs ? p.cgram[2]
                                     : p.cgram[3]));
      CHECK(px(-(int)hs - 9, 0) == emit_color(p.cgram[0]));
    }
  }

  /* camX 256: west margin worldX is positive — outside tile must NOT fire;
   * a real store hit serves its entry, unproven misses keep the wrap. */
  fixture(flags);
  negworld_setup(256);
  p.hScroll[0] = 256;
  draw_row(0);
  if (new_renderer) {
    /* worldX 240..255 (sx -16..-1) is positive: outside tile must NOT
     * fire; unproven misses keep the wrap — cols 30 (empty) and 31
     * (red strip). */
    for (int x = -16; x < -8; x++)
      CHECK(px(x, 0) == emit_color(p.cgram[0]));
    for (int x = -8; x < 0; x++)
      CHECK(px(x, 0) == emit_color(p.cgram[2]));
    /* Legit negative worldX deep west (sx -272..-257): outside tile. */
    for (int x = -kPpuExtraLeftRight; x < -256; x++)
      CHECK(px(x, 0) == emit_color(p.cgram[1]));
    /* Positive-west STORE HIT: worldX 0..7 (sx -256..-249) is west of
     * the view yet positive, uncaptured, and serves tile5 — distinct
     * from both outside tile and wrap. */
    for (int x = -256; x < -248; x++)
      CHECK(px(x, 0) == emit_color(p.cgram[5]));
    /* Adjacent positive misses keep the wrapped tilemap content
     * (cols 1/2/4 are populated, the rest empty). */
    for (int x = -248; x < 0; x++)
      CHECK(px(x, 0) == wrap_color(x));
    /* Positive misses inside the view-window wrap, same rule. */
    for (int x = 0; x < 256; x++)
      CHECK(px(x, 0) == wrap_color(x));
    /* East store hit beyond the captured keys: wT64 (sx 256..263). */
    for (int x = 256; x < 264; x++)
      CHECK(px(x, 0) == emit_color(p.cgram[4]));
    /* East positive misses keep the wrap. */
    for (int x = 264; x < 272; x++)
      CHECK(px(x, 0) == wrap_color(x));
  } else {
    CHECK(px(-8, 0) == emit_color(p.cgram[2]));
    CHECK(px(-200, 0) == emit_color(p.cgram[0]));
    CHECK(px(264, 0) == emit_color(p.cgram[2]));
  }

  /* Option disarmed: same worldstart geometry falls back to the wrap
   * (old behavior). With the weak pre-fix adapter the new-renderer
   * gutter CHECKs above already failed (red tile everywhere west);
   * these asserts are GREEN-only proof that the flag, not the layer,
   * carries the new semantic. */
  fixture(flags);
  negworld_setup(0);
  WsShadowSetNegWorldTile(0, -1);
  draw_row(0);
  CHECK(px(-8, 0) == emit_color(p.cgram[2]));
  /* WsShadowReset clears the option (and the layer registration). */
  fixture(flags);
  negworld_setup(0);
  WsShadowSetNegWorldTile(0, 0x0001);
  WsShadowReset();
  draw_row(0);
  CHECK(px(-8, 0) == emit_color(p.cgram[2]));
}

int main(void) {
  both_renderers("bg1_margins_and_scroll", bg1_margins_and_scroll);
  both_renderers("bg2_bigtile_pages", bg2_bigtile_pages);
  both_renderers("bg2_horizontal_page", bg2_horizontal_page);
  both_renderers("bg3_widen_policy", bg3_widen_policy);
  both_renderers("perline_scroll", perline_scroll);
  both_renderers("bg1_window_mask", bg1_window_mask);
  both_renderers("bg1_brightness", bg1_brightness);
  both_renderers("bg1_color_math", bg1_color_math);
  both_renderers("bg1_mosaic", bg1_mosaic);
  both_renderers("bg_upper_plane", bg_upper_plane);
  both_renderers("bg1_negworld_margin", bg1_negworld_margin);
  printf("smw2_bg_render: PASS %u checks\n", cases);
  return 0;
}
