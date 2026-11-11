/* Synthetic address regressions; no ROM-derived data is checked in.
 * Run: cc -std=c11 -I snesrecomp/runner/src tests/smw2_ws_test.c \
 *         snesrecomp/runner/src/snes/ws_shadow.c -o /tmp/smw2_ws_test
 * Optional real-state audit: /tmp/smw2_ws_test DUMP_DIR ROM level0 level1 ...
 * Include the title implementation to exercise its private decoder without
 * exporting test-only functions. The world store is the real framework one.
 * CTest links no renderer, so the few host setters the title policy calls are
 * scaffolded below with the exact field effects of the real implementations;
 * the decoder, proof walk and scene gates under test stay real. */
#include "../src/smw2_rtl.c"

uint8 g_ram[0x20000];
Snes *g_snes;
Ppu *g_ppu;
bool g_ws_active;
int g_ws_extra;
int snes_frame_counter;

static Snes snes;
static Cart cart;
static Ppu ppu;
static uint8_t sram[0x8000], rom[0x200000];
static unsigned checks, failures;

/* Model RomPtr's actual non-ROM fallback on this SuperFX cartridge. The
 * GSU data reader must not confuse a $4C GSU address with CPU bank $18. */
uint8 *RomPtr(uint32 addr) {
  (void)addr;
  return rom;
}

/* Test-owned timeline clock in place of common_rtl.c's RtlStateGeneration,
 * same contract: bump it to model a reset/load, leave it for rollback or an
 * ordinary frame. */
static uint64_t s_test_generation = 1;
uint64_t RtlStateGeneration(void) { return s_test_generation; }
/* Compatibility adapter: lets this regression compile and run against a
 * pre-fix ws_shadow.c that lacks WsShadowSetNegWorldTile. When the real
 * implementation is linked this weak definition is discarded; against the
 * old object it is a no-op, giving genuine behavioral RED (the title
 * policy cannot arm the outside tile) rather than a link failure. */
void WsShadowSetNegWorldTile(int layerIndex, int entry);
__attribute__((weak)) void WsShadowSetNegWorldTile(int layerIndex, int entry) {
  (void)layerIndex;
  (void)entry;
}

/* Minimal host-API scaffolding: ppu.c is not linked into this runner, so the
 * three setters the background policy calls are reproduced with the exact
 * field effects of their real ppu.c bodies. PpuResetLayerPolicies' clearing
 * of unrelated ws_* policy arrays is omitted: this binary never sets those
 * fields, and the BG3-widen byte is not part of that reset set anyway. */
void PpuSetExtraSpace(Ppu *p, uint16_t extra) {
  if (extra > kPpuExtraLeftRight)
    extra = kPpuExtraLeftRight;
  p->extraLeftRight = extra;
  p->extraLeftCur = extra;
  p->extraRightCur = extra;
}

void PpuSetExtraSpaceCentered(Ppu *p, uint16_t budget) {
  if (budget > kPpuExtraLeftRight)
    budget = kPpuExtraLeftRight;
  p->extraLeftRight = budget;
  p->extraLeftCur = 0;
  p->extraRightCur = 0;
}

void PpuSetWidescreenBg3Widen(Ppu *p, uint8_t from_y) {
  p->wsBg3WidenY = from_y;
}

#define CHECK(expr)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(expr)) {                                                             \
      fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #expr);         \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void put16(uint8_t *b, unsigned off, uint16_t value) {
  b[off] = (uint8_t)value;
  b[off + 1] = (uint8_t)(value >> 8);
}

static void fixture(void) {
  memset(&snes, 0, sizeof(snes));
  memset(&cart, 0, sizeof(cart));
  memset(&ppu, 0, sizeof(ppu));
  memset(sram, 0, sizeof(sram));
  memset(rom, 0, sizeof(rom));
  memset(g_ram, 0, sizeof(g_ram));
  snes.cart = &cart;
  cart.ram = sram;
  cart.ramSize = sizeof(sram);
  cart.rom = rom;
  cart.romSize = sizeof(rom);
  g_snes = &snes;
  g_ppu = &ppu;
  g_ws_active = true;
  g_ws_extra = 99;
  ppu.bgmode = 0x69; /* Mode 1, BG2 and BG3 big; BG1 small. */
  ppu.bgXsc[0] = 0x69;
  ppu.screenEnabled[0] = 1;
  s_test_generation = 1;
  Smw2ResetWsBackgrounds();
}

/* A fully consistent streamed BG1 view: MAP16 RAM/ROM and the VRAM window
 * agree on every tile of the 64x32 buffer, so both the tolerant and the
 * strict 12/12 proof pass while camera phase `phase` is applied. */
static void stage_fixture(unsigned phase) {
  const uint16_t camera_x = (uint16_t)(216 + phase);
  put16(g_ram, 0x39, camera_x);
  put16(g_ram, 0x3b, 1780);
  ppu.hScroll[0] = camera_x;
  ppu.vScroll[0] = 756;
  put16(rom, 0xc32a6, 0x8000); /* page one, above the 32K split */
  for (unsigned obj = 0; obj < 256; obj++) {
    put16(g_ram, 0x18000 + obj * 2, (uint16_t)(0x0100 + obj));
    for (unsigned q = 0; q < 4; q++)
      put16(rom, 0xcb3f2 + obj * 8 + q * 2, (uint16_t)(0x1000 + obj * 4 + q));
  }
  for (unsigned r = 0; r < 32; r++) {
    for (unsigned c = 0; c < 64; c++) {
      unsigned obj = (r / 2) * 16 + (c / 2) % 16;
      unsigned addr = 0x6800 + r * 32 + (c % 32) + (c >= 32 ? 1024 : 0);
      ppu.vram[addr] = (uint16_t)(0x1000 + obj * 4 + (r % 2) * 2 + c % 2);
    }
  }
}

static void test_screen_table(void) {
  fixture();
  bool ok;
  put16(g_ram, 0x18000, 0x1234);
  CHECK(smw2_map16_index(0, 0, &ok) == 0x1234 && ok); /* slot zero is valid */
  sram[0xcaa + 0x61] = 3; /* screen (1,6), not the framebuffer at $706D0B */
  sram[0x6caa + 0x61] = 5;
  put16(g_ram, 0x18600 + 2 * 0x20 + 4 * 2, 0x5678);
  CHECK(smw2_map16_index(20, 98, &ok) == 0x5678 && ok);
  sram[0xcaa + 0x61] = 0x83; /* unloaded flag must survive ID masking */
  (void)smw2_map16_index(20, 98, &ok);
  CHECK(!ok);
  (void)smw2_map16_index(256, 0, &ok);
  CHECK(!ok);
  (void)smw2_map16_index(0, 128, &ok);
  CHECK(!ok);
  cart.ramSize = 0xcaa;
  (void)smw2_map16_index(0, 0, &ok);
  CHECK(!ok);
}

static void test_gsu_rom(void) {
  fixture();
  uint16_t entry[4];
  put16(rom, 0xc32a4 + 9 * 2, 0x7000);
  for (unsigned i = 0; i < 4; i++)
    put16(rom, 0xca40a + i * 2, (uint16_t)(0x1200 + i));
  CHECK(smw2_map16_entry(0x0903, entry));
  CHECK(entry[0] == 0x1200 && entry[1] == 0x1201 && entry[2] == 0x1202 &&
        entry[3] == 0x1203);
  /* GSU r14 wraps within its 64K ROM bank, not at the LoROM 32K split. */
  put16(rom, 0xc32a4 + 2 * 2, 0xcc0a);
  put16(rom, 0xcfffc, 0x2100);
  put16(rom, 0xcfffe, 0x2101);
  put16(rom, 0xc0000, 0x2102);
  put16(rom, 0xc0002, 0x2103);
  CHECK(smw2_map16_entry(0x0200, entry));
  CHECK(entry[0] == 0x2100 && entry[1] == 0x2101 && entry[2] == 0x2102 &&
        entry[3] == 0x2103);
  cart.romSize = 0xc0000;
  CHECK(!smw2_map16_entry(0x0903, entry));
}

static void test_world_origin(void) {
  fixture();
  int32_t x, y;
  put16(g_ram, 0x39, 220);
  put16(g_ram, 0x3b, 1780);
  ppu.hScroll[0] = 220;
  ppu.vScroll[0] = 756;
  smw2_world_origin(0, &x, &y);
  CHECK(x == 220 && y == 1780);
  ppu.hScroll[0] = 222;
  ppu.vScroll[0] = 758;
  smw2_world_origin(0, &x, &y);
  CHECK(x == 222 && y == 1782);
  put16(g_ram, 0x39, 512);
  put16(g_ram, 0x3b, 2048);
  ppu.hScroll[0] = 510;
  ppu.vScroll[0] = 1022;
  smw2_world_origin(0, &x, &y);
  CHECK(x == 510 && y == 2046);
}

static void test_near_uniform_view_stands_down(void) {
  fixture();
  for (unsigned i = 0; i < 4; i++)
    put16(rom, 0xc33f2 + i * 2, 0x18ff);
  for (unsigned i = 0; i < 0x8000; i++)
    ppu.vram[i] = 0x18ff;
  /* Ten identical samples and two mismatches do not prove a world map. */
  ppu.vram[0x6800] = 0x1234; /* sample (0,0): the first sample is an outlier */
  ppu.vram[0x6863] = 0x2345; /* sample (3,3) */
  CHECK(!smw2_bg1_stream_valid(0, 0, 0, 0));
}

/* A fully consistent but NEAR-UNIFORM view: every sampled tile is the same
 * authored empty tile (0x18ff), exactly like the stairs section whose sky
 * fills all twelve diagonal samples. The world↔buffer mapping is still
 * proven 12/12 and both gutters decode, so the margins must arm — uniformity
 * alone is not evidence of a wrong mapping (P3). A single mismatch must still
 * stand down: that is the partial-proof case the uniform heuristic guards. */
static void stage_fixture_uniform(void) {
  put16(g_ram, 0x39, 216);
  put16(g_ram, 0x3b, 1780);
  ppu.hScroll[0] = 216;
  ppu.vScroll[0] = 756;
  put16(rom, 0xc32a6, 0x8000); /* page one, above the 32K split */
  for (unsigned obj = 0; obj < 256; obj++) {
    put16(g_ram, 0x18000 + obj * 2, (uint16_t)(0x0100 + obj));
    for (unsigned q = 0; q < 4; q++)
      put16(rom, 0xcb3f2 + obj * 8 + q * 2, 0x18ff);
  }
  for (unsigned i = 0; i < 0x8000; i++)
    ppu.vram[i] = 0x18ff;
}

static void test_uniform_proven_view_arms(void) {
  fixture();
  stage_fixture_uniform();
  const unsigned wtx0 = 216 >> 3, wty0 = 1780 >> 3;
  const unsigned btx0 = ppu.hScroll[0] >> 3, bty0 = ppu.vScroll[0] >> 3;
  bool nu = false;
  CHECK(smw2_bg1_view_hits(wtx0, wty0, btx0, bty0, &nu) == 12 && nu);
  /* Perfect proof on a uniform view arms both proof levels. */
  CHECK(smw2_bg1_stream_proven(wtx0, wty0, btx0, bty0));
  CHECK(smw2_bg1_stream_valid(wtx0, wty0, btx0, bty0));
  PpuSetExtraSpace(&ppu, 16);
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(ppu.extraLeftCur == 16 && ppu.extraRightCur == 16);
  /* One corrupted sample breaks the perfect proof: a uniform 11/12 view is
   * exactly the unproven case that must stay native. */
  const unsigned addr = smw2_vram_word(btx0 + 9, bty0 + 9) & 0x7fff;
  ppu.vram[addr] = 0x1234;
  CHECK(smw2_bg1_view_hits(wtx0, wty0, btx0, bty0, &nu) == 11 && nu);
  CHECK(!smw2_bg1_stream_proven(wtx0, wty0, btx0, bty0));
  CHECK(!smw2_bg1_stream_valid(wtx0, wty0, btx0, bty0));
}

static void test_margin_store(unsigned phase) {
  fixture();
  stage_fixture(phase);
  const uint16_t camera_x = (uint16_t)(216 + phase);
  static uint8_t ram_before[sizeof(g_ram)];
  static uint8_t sram_before[sizeof(sram)];
  static uint16_t vram_before[sizeof(ppu.vram) / sizeof(uint16_t)];
  memcpy(ram_before, g_ram, sizeof(ram_before));
  memcpy(sram_before, sram, sizeof(sram_before));
  memcpy(vram_before, ppu.vram, sizeof(vram_before));
  uint16_t native = ppu.vram[0x6800];
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(WsShadowWorldX(0) == camera_x && WsShadowWorldY(0) == 1780);
  uint16_t tile = 0;
  CHECK(WsShadowLookupWorldTile(0, 20, 224, &tile) && tile == 0x1028);
  CHECK(WsShadowLookupWorldTile(0, 60, 224, &tile) && tile == 0x1038);
  CHECK(WsShadowLookupWorldTile(0, 59, 224, &tile) && tile == 0x1035);
  /* Background policy is presentation-only: WRAM, cart SRAM and VRAM must
   * come back byte-identical. */
  CHECK(memcmp(ram_before, g_ram, sizeof(ram_before)) == 0);
  CHECK(memcmp(sram_before, sram, sizeof(sram_before)) == 0);
  CHECK(memcmp(vram_before, ppu.vram, sizeof(vram_before)) == 0);
  CHECK(ppu.vram[0x6800] == native);
  g_ws_active = false;
  Smw2ConfigureWsBgMargins();
  CHECK(!WsShadowLayerActive(0));
}

/* A scene-tuple change with an identical sampled tilemap: the store is
 * dropped, one centered/native frame is shown, and re-arm needs the full
 * 12/12 proof on a later frame — while scrolling. The same cycle must fire
 * for every tuple field, the timeline generation, and a cart swap. */
static void test_scene_transitions(void) {
  fixture();
  stage_fixture(0);
  PpuSetExtraSpace(&ppu, 16); /* the host's per-frame margin setup */
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(ppu.extraLeftCur == 16 && ppu.extraRightCur == 16);
  uint16_t tile = 0;
  CHECK(WsShadowLookupWorldTile(0, 20, 224, &tile));

  /* Stage bump only: tilemap proof would still pass, but the frame must be
   * native — a tuple change is an epoch, not a margin proof. */
  put16(g_ram, 0x021a, 0x000b);
  Smw2ConfigureWsBgMargins();
  CHECK(!WsShadowLayerActive(0));
  CHECK(ppu.extraLeftCur == 0 && ppu.extraRightCur == 0); /* centered window */
  CHECK(ppu.extraLeftRight == 16); /* budget preserved for next frame */
  CHECK(!WsShadowLookupWorldTile(0, 20, 224, &tile)); /* store dropped */

  /* Next frame: host margins restored through the normal setup path, camera
   * scrolled; the complete native proof re-arms. */
  PpuSetExtraSpace(&ppu, 16);
  stage_fixture(1);
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(ppu.extraLeftCur == 16);
  CHECK(WsShadowWorldX(0) == 217);

  /* Every remaining tuple word is an epoch on its own. */
  static const unsigned kTupleOff[4] = {0x0118, 0x0218, 0x038c, 0x038e};
  for (unsigned i = 0; i < 4; i++) {
    const uint16_t keep =
        (uint16_t)(g_ram[kTupleOff[i]] | (g_ram[kTupleOff[i] + 1] << 8));
    put16(g_ram, kTupleOff[i], (uint16_t)(keep + 1));
    Smw2ConfigureWsBgMargins();
    CHECK(!WsShadowLayerActive(0));
    CHECK(ppu.extraLeftCur == 0);
    PpuSetExtraSpace(&ppu, 16);
    stage_fixture(2);
    Smw2ConfigureWsBgMargins();
    CHECK(WsShadowLayerActive(0));
  }

  /* A timeline jump (reset/save-state load) is an epoch without any tuple
   * field moving. */
  s_test_generation = 2;
  Smw2ConfigureWsBgMargins();
  CHECK(!WsShadowLayerActive(0));
  CHECK(ppu.extraLeftCur == 0);
  PpuSetExtraSpace(&ppu, 16);
  stage_fixture(3);
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));

  /* A cartridge replacement drops the store but is a cold observation: it
   * may re-arm on the same frame's complete proof and is owed no centered
   * frame. */
  {
    static Cart other_cart;
    memcpy(&other_cart, &cart, sizeof(cart));
    CHECK(WsShadowLookupWorldTile(0, 20, 224, &tile));
    snes.cart = &other_cart;
    Smw2ConfigureWsBgMargins();
    CHECK(WsShadowLayerActive(0)); /* cold arm on full proof */
    CHECK(ppu.extraLeftCur == 16); /* no transition frame */
    snes.cart = &cart;
    Smw2ConfigureWsBgMargins();
    CHECK(WsShadowLayerActive(0));
  }
}

/* Until a complete native proof is seen — cold start or after an epoch —
 * the tolerant 10/12 walk must NOT arm the margins. */
static void test_strict_rearm(void) {
  fixture();
  stage_fixture(0);
  /* Corrupt one of the twelve sample cells: 11/12 hits passes the settled
   * tolerance but must fail the cold strict proof. */
  const unsigned wtx0 = 216 >> 3, wty0 = 1780 >> 3;
  const unsigned btx0 = ppu.hScroll[0] >> 3, bty0 = ppu.vScroll[0] >> 3;
  const unsigned addr =
      smw2_vram_word(btx0 + 9, bty0 + 9) & 0x7fff; /* 4th sample cell */
  const uint16_t keep = ppu.vram[addr];
  ppu.vram[addr] = (uint16_t)(keep + 1);
  bool nu;
  CHECK(smw2_bg1_view_hits(wtx0, wty0, btx0, bty0, &nu) == 11 && !nu);
  CHECK(smw2_bg1_stream_valid(wtx0, wty0, btx0, bty0)); /* tolerant passes */
  Smw2ConfigureWsBgMargins();
  CHECK(!WsShadowLayerActive(0)); /* cold start needs the full proof */

  /* Restore and arm for real, then cut a tuple field: an epoch re-arms only
   * on a complete proof too — the same 11/12 must still stand down. */
  ppu.vram[addr] = keep;
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  put16(g_ram, 0x038e, 1);
  Smw2ConfigureWsBgMargins(); /* transition frame: native */
  CHECK(!WsShadowLayerActive(0));
  ppu.vram[addr] = (uint16_t)(keep + 1);
  Smw2ConfigureWsBgMargins(); /* 11/12 is not enough to re-arm */
  CHECK(!WsShadowLayerActive(0));
  ppu.vram[addr] = keep;
  Smw2ConfigureWsBgMargins(); /* next complete proof re-arms */
  CHECK(WsShadowLayerActive(0));
}

/* A margin cell whose MAP16 cell stops resolving must be invalidated: the
 * previously stored tile may not survive in the world-keyed store. */
static void test_unloaded_cell_invalidated(void) {
  fixture();
  stage_fixture(0);
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  uint16_t want;
  CHECK(smw2_bg1_tile(70, 230, &want));
  uint16_t tile = 0;
  CHECK(WsShadowLookupWorldTile(0, 70, 230, &tile) && tile == want);
  /* Screen (2,7) covers the east margin below the sample rows only; marking
   * it unloaded keeps the 12/12 proof passing while that cell fails. */
  sram[0xcaa + 0x72] = 0x80;
  CHECK(!smw2_bg1_tile(70, 230, &want));
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0)); /* samples still all resolve */
  CHECK(!WsShadowLookupWorldTile(0, 70, 230, &tile));
}

/* Streaming mutates the screen-id table and VRAM columns constantly; those
 * writes are not a scene epoch and must not drop retained world tiles. */
static void test_screen_table_no_epoch(void) {
  fixture();
  stage_fixture(0);
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  uint16_t tile = 0;
  CHECK(WsShadowLookupWorldTile(0, 20, 224, &tile));
  /* A streaming table update that reroutes an unsampled margin screen is
   * not an epoch: the layer stays armed, the cell is re-resolved through
   * its new screen slot, and retained tiles are not dropped. */
  put16(g_ram, 0x183e6, 0x0105); /* populate screen-id slot 1 at (m16x3,
                                    m16y15) → cell (70,222) resolves 0x1014 */
  sram[0xcaa + 0x62] = 1;        /* screen (2,6) now points at slot 1 */
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(WsShadowLookupWorldTile(0, 20, 224, &tile) && tile == 0x1028);
  CHECK(WsShadowLookupWorldTile(0, 70, 222, &tile) && tile == 0x1014);
  /* Stream-out: only the unresolved margin cell is invalidated — the layer
   * stays active and the retained store is untouched (no epoch). */
  sram[0xcaa + 0x62] = 0x80;
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(WsShadowLookupWorldTile(0, 20, 224, &tile));  /* history preserved */
  CHECK(!WsShadowLookupWorldTile(0, 70, 222, &tile)); /* exact invalidation */
  /* Stream it back: the margin cell is re-seeded on the same armed frame —
   * an epoch would have cost a centered frame and a strict re-arm first. */
  sram[0xcaa + 0x62] = 0;
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(WsShadowLookupWorldTile(0, 70, 222, &tile));
  CHECK(ppu.extraLeftCur == 0); /* fixture never granted margins */
}

/* BG3 widens naturally only in proven level play: game mode $0F, no message
 * box, no stage intro, no camera event, and the native Mode-1 large-BG3
 * priority layout (bgXsc $34, 32x32 at VRAM word $3400, TM bit 2). Every
 * other state reapplies the zero policy each frame. */
static void test_bg3_gates(void) {
  fixture();
  put16(g_ram, 0x0118, 0x000f); /* level gameplay */
  ppu.screenEnabled[0] = 0x15;  /* TM: BG1|BG3 on the main screen */
  ppu.bgXsc[2] = 0x34;          /* documented 32x32 BG3 map base */
  ppu.wsBg3WidenY = 7;          /* stale policy must be reapplied, not kept */
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 1);

  /* Every nonzero message-box state is active UI. */
  for (unsigned v = 1; v <= 0x0d; v++) {
    g_ram[0x0d0f] = (uint8_t)v;
    Smw2ConfigureWsBgMargins();
    CHECK(ppu.wsBg3WidenY == 0);
  }
  for (unsigned v = 0x80; v <= 0xff; v += 0x7f) {
    g_ram[0x0d0f] = (uint8_t)v;
    Smw2ConfigureWsBgMargins();
    CHECK(ppu.wsBg3WidenY == 0);
  }
  g_ram[0x0d0f] = 0;
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 1); /* off → on across consecutive frames */

  g_ram[0x0121] = 1; /* stage intro flag */
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 0);
  g_ram[0x0121] = 0;

  /* Camera events live in physical cart SRAM ($701E2A), not WRAM $1E2A. */
  put16(sram, 0x1e2a, 3);
  put16(g_ram, 0x1e2a, 0);
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 0);
  put16(sram, 0x1e2a, 0);
  put16(g_ram, 0x1e2a, 7); /* the identically numbered WRAM cell is inert */
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 1);
  put16(g_ram, 0x1e2a, 0);

  static const struct {
    uint8_t bgmode, bg3sc, tm;
  } kShape[7] = {{0x07, 0x34, 0x15},  /* mode 0, no BG3 priority/big  */
                 {0x61, 0x34, 0x15},  /* BG3 priority off             */
                 {0x29, 0x34, 0x15},  /* BG3 8px tiles                */
                 {0x69, 0x34, 0x11},  /* BG3 off the main screen      */
                 {0x69, 0x35, 0x15},  /* 64-wide BG3 map              */
                 {0x69, 0x36, 0x15},  /* 64-TALL BG3 map (!higher)    */
                 {0x69, 0x1a, 0x15}}; /* different map base           */
  for (unsigned i = 0; i < 7; i++) {
    ppu.bgmode = kShape[i].bgmode;
    ppu.bgXsc[2] = kShape[i].bg3sc;
    ppu.screenEnabled[0] = kShape[i].tm;
    Smw2ConfigureWsBgMargins();
    CHECK(ppu.wsBg3WidenY == 0);
  }
  ppu.bgmode = 0x69;
  ppu.bgXsc[2] = 0x34;
  ppu.screenEnabled[0] = 0x15;
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 1); /* on → off → on again */

  /* Widescreen off forces the zero policy; nothing may leak through. */
  g_ws_active = false;
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 0);
  g_ws_active = true;
  Smw2ConfigureWsBgMargins();
  CHECK(ppu.wsBg3WidenY == 1);
}

/* The exported reset drops retained background state and the scene epoch:
 * the next frame is a cold observation that may arm on a complete proof. */
static void test_reset_api(void) {
  fixture();
  stage_fixture(0);
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  uint16_t tile = 0;
  CHECK(WsShadowLookupWorldTile(0, 20, 224, &tile));
  ppu.wsBg3WidenY = 1; /* reset reapplies the zero BG3 policy */
  static uint8_t rb[sizeof(g_ram)], sb[sizeof(sram)];
  static uint16_t vb[sizeof(ppu.vram) / sizeof(uint16_t)];
  memcpy(rb, g_ram, sizeof(rb));
  memcpy(sb, sram, sizeof(sb));
  memcpy(vb, ppu.vram, sizeof(vb));
  Smw2ResetWsBackgrounds();
  CHECK(memcmp(rb, g_ram, sizeof(rb)) == 0 &&
        memcmp(sb, sram, sizeof(sb)) == 0 &&
        memcmp(vb, ppu.vram, sizeof(vb)) == 0);
  CHECK(!WsShadowLayerActive(0));
  CHECK(!WsShadowLookupWorldTile(0, 20, 224, &tile));
  CHECK(ppu.wsBg3WidenY == 0);
  /* Cold re-arm on the next complete proof — no centered transition frame
   * is owed to an explicit reset. */
  ppu.extraLeftCur = 16;
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(ppu.extraLeftCur == 16); /* reset did not force a centered frame */
}

static void read_blob(const char *path, void *out, size_t size) {
  FILE *f = fopen(path, "rb");
  if (!f || fread(out, 1, size, f) != size) {
    fprintf(stderr, "cannot read fixture %s\n", path);
    exit(2);
  }
  fclose(f);
}

static long json_scalar(const char *buf, const char *key, long fallback) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(buf, needle);
  if (!p)
    return fallback;
  p += strlen(needle);
  while (*p == ' ' || *p == ':' || *p == '\t')
    p++;
  return strtol(p, NULL, 0);
}

/* Load the frame's real PPU registers from <tag>.regs.json when the dump
 * captured them, so BG3 eligibility is audited against LIVE state rather
 * than the fabricated fixture shape. Returns nonzero on success. The BG1
 * scroll pair is deliberately NOT loaded: the audit's decode/proof walks
 * key on the camera mirrors and must stay consistent with them. */
static int audit_live_regs(const char *dir, const char *tag) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/%s.regs.json", dir, tag);
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  static char buf[1 << 16];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  if (!strstr(buf, "\"bgmode\"") || !strstr(buf, "\"bgsc\""))
    return 0;
  ppu.bgmode = (uint8_t)json_scalar(buf, "bgmode", 0);
  ppu.screenEnabled[0] = (uint8_t)json_scalar(buf, "tm", 0);
  ppu.screenEnabled[1] = (uint8_t)json_scalar(buf, "ts", 0);
  const char *a = strchr(strstr(buf, "\"bgsc\""), '[');
  for (int i = 0; i < 4 && a; i++) {
    ppu.bgXsc[i] = (uint8_t)strtol(a + 1, (char **)&a, 0);
    a = strchr(a, ',');
  }
  return 1;
}

static void audit_dump(const char *dir, const char *rom_path, const char *tag) {
  fixture();
  char path[4096];
  snprintf(path, sizeof(path), "%s/%s.wram.bin", dir, tag);
  read_blob(path, g_ram, sizeof(g_ram));
  snprintf(path, sizeof(path), "%s/%s.sram.bin", dir, tag);
  read_blob(path, sram, sizeof(sram));
  snprintf(path, sizeof(path), "%s/%s.vram.bin", dir, tag);
  read_blob(path, ppu.vram, sizeof(ppu.vram));
  read_blob(rom_path, rom, sizeof(rom));
  unsigned cx = g_ram[0x39] | g_ram[0x3a] << 8;
  unsigned cy = g_ram[0x3b] | g_ram[0x3c] << 8;
  ppu.hScroll[0] = cx & 1023;
  ppu.vScroll[0] = cy & 1023;
  unsigned hits = 0, total = 0;
  for (unsigned y = 1; y < 27; y++) {
    for (unsigned x = 1; x < 31; x++) {
      unsigned tx = cx / 8 + x, ty = cy / 8 + y;
      unsigned col = tx % 64, row = ty % 32;
      unsigned addr = 0x6800 + row * 32 + col % 32 + (col >= 32 ? 1024 : 0);
      uint16_t tile;
      hits += smw2_bg1_tile(tx, ty, &tile) && tile == ppu.vram[addr];
      total++;
    }
  }
  printf("%s: native tile matches %u/%u\n", tag, hits, total);

  /* Every sampled dump either fully decodes (level state → the strict proof
   * must arm) or does not (menu/non-level state → the layer must stand
   * down). A partial decode must stand down too: unproven views never arm. */
  /* Audit decoding is not live scene eligibility: regs.json supplies the
   * frame's real PPU registers for the BG3 shape gates, while the WRAM
   * gates read the dump itself. A stage-$0B dump can only ever prove the
   * intro/tutorial path — that is reported, not passed off as ordinary
   * stage gameplay. */
  const int live = audit_live_regs(dir, tag);
  if (!live) {
    ppu.bgmode = 0x69;
    ppu.bgXsc[0] = 0x69;
    ppu.bgXsc[1] = 0x3a;
    ppu.bgXsc[2] = 0x34;
    ppu.screenEnabled[0] = 0x15;
    ppu.screenEnabled[1] = 0x02;
  }
  uint16_t sc[kSceneWords];
  smw2_scene_tuple(sc);
  const bool wram_ok = smw2_dp_u16(0x0118) == 0x000f && !g_ram[0x0d0f] &&
                       !g_ram[0x0121] && smw2_gsu_u16(0x1e2a) == 0;
  Smw2ConfigureWsBgMargins();
  printf("%s: scene m%04x w%04x s%04x l%04x e%04x intro=%02x msg=%02x "
         "cam_evt=%04x regs=%s wram_gates=%s bg3_eligible=%d widenY=%u "
         "bg1_active=%d%s\n",
         tag, sc[0], sc[1], sc[2], sc[3], sc[4], g_ram[0x0121], g_ram[0x0d0f],
         smw2_gsu_u16(0x1e2a), live ? "live" : "fabricated",
         wram_ok ? "pass" : "fail", smw2_bg3_eligible(), ppu.wsBg3WidenY,
         WsShadowLayerActive(0),
         sc[2] == 0x000b
             ? " (intro/tutorial stage $0B — NOT normal-stage proof)"
             : "");
  if (hits == total && total == 780) {
    CHECK(WsShadowLayerActive(0));
    CHECK(WsShadowWorldY(0) == cy);
  } else {
    CHECK(!WsShadowLayerActive(0));
  }
}

/* ===== Task5: known negative-worldX west margin =====
 *
 * At a world origin of 0 the west gutter addresses worldX < 0: nothing can
 * legitimately answer for it. Pre-fix it fell through to the renderer's
 * plain map wrap — the far page of the rolling tilemap (the east-ahead
 * staged strip), which parent-verified captures showed as repeated
 * tile/glyph garbage. The fix arms an opt-in sign-scoped "outside" tile
 * from the game's own MAP16 empty object (index 0) — never the slot-15
 * alias that bg1_tile(-1) would read in rooms where slot 15 is populated.
 * The fallback is a validated fully transparent 4bpp tile, revalidated
 * per frame so streaming cannot turn it into stale visible art. */

/* cam_x=0 variant of stage_fixture: hScroll/vScroll == camera so the
 * strict proof still passes with every buffer cell decoded. */
static void stage_fixture_cam0(void) {
  put16(g_ram, 0x39, 0);
  put16(g_ram, 0x3b, 1780);
  ppu.hScroll[0] = 0;
  ppu.vScroll[0] = 756;
  put16(rom, 0xc32a6, 0x8000);
  for (unsigned obj = 0; obj < 256; obj++) {
    put16(g_ram, 0x18000 + obj * 2, (uint16_t)(0x0100 + obj));
    for (unsigned q = 0; q < 4; q++)
      put16(rom, 0xcb3f2 + obj * 8 + q * 2, (uint16_t)(0x1000 + obj * 4 + q));
  }
  for (unsigned r = 0; r < 32; r++) {
    for (unsigned c = 0; c < 64; c++) {
      unsigned obj = (r / 2) * 16 + (c / 2) % 16;
      unsigned addr = 0x6800 + r * 32 + (c % 32) + (c >= 32 ? 1024 : 0);
      ppu.vram[addr] = (uint16_t)(0x1000 + obj * 4 + (r % 2) * 2 + c % 2);
    }
  }
}

static void test_negworld_margin(void) {
  fixture();
  stage_fixture_cam0();
  /* MAP16 empty object idx0 = uniform 0x18ff (the game's authored "empty"
   * fill; mirrors the real stage dump's 18ff 18ff 18ff 18ff). */
  for (unsigned q = 0; q < 4; q++)
    put16(rom, 0xc33f2 + q * 2, 0x18ff);
  /* Screen slot 15 NONEMPTY with different content: if the title resolved
   * the outside tile through bg1_tile(-1) it would read the far-east
   * aliased slot, not idx0. Guard proves it does not. */
  sram[0x6caa + (15 | (6 << 4))] = 1;
  sram[0xcaa + (15 | (6 << 4))] = 1;
  put16(g_ram, 0x18200 + 5 * 0x20 + 3 * 2, 0x01aa); /* slot15 cell content */
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  CHECK(WsShadowWorldX(0) == 0 && WsShadowWorldY(0) == 1780);
  const uint32_t wrappedY = (uint32_t)(756 + 16); /* one line into the view */
  /* Every west pixel phase (the margin budget is 99px wide here):
   * worldX < 0 returns the outside tile, never the wrapped page-B strip
   * entry realTile would have supplied. */
  for (int sx = -99; sx < 0; sx++) {
    const uint16_t real = (uint16_t)(0x4000u | (unsigned)(-sx));
    const uint16_t got = WsShadowTile(0, sx, wrappedY, 0, 0x6800, real);
    CHECK(got == 0x18ff);
  }
  /* Seam: native column boundary x=0 keeps realTile. */
  CHECK(WsShadowTile(0, 0, wrappedY, 0, 0x6800, 0x4000) == 0x4000);
  /* Positive unproven miss: east margin beyond the filled span still gets
   * the established realTile wrap — not blanked, not the outside tile. */
  const uint16_t east = WsShadowTile(0, 400, wrappedY, 0, 0x6800, 0x2222);
  CHECK(east == 0x2222);
  /* cam_x=256: west margin is positive worldX and behaves exactly as a
   * normal miss — store hit when filled, wrap when not. */
  put16(g_ram, 0x39, 256);
  ppu.hScroll[0] = 256;
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowWorldX(0) == 256);
  const uint16_t west = WsShadowTile(0, -16, wrappedY, 256, 0x6800, 0x3333);
  uint16_t want;
  CHECK(smw2_bg1_tile(30, 96, &want));
  CHECK(west == want); /* filled gutter cell wins */
  /* Unfilled positive cell misses to realTile, never to 0x18ff. */
  CHECK(WsShadowTile(0, -16, (uint32_t)(756 - 512), 256, 0x6800, 0x3333) ==
        0x3333);
}

static void test_negworld_transparent_fallback(void) {
  fixture();
  stage_fixture_cam0();
  /* The empty object exists but is NOT a single uniform tile (a decorated
   * "outside" object with differing quadrants): unsuitable as one outside
   * entry, so the title must serve the first validated fully-transparent
   * BG1 4bpp tile — not a guessed constant, not the wrap. */
  put16(rom, 0xc33f2 + 0, 0x18ff);
  put16(rom, 0xc33f2 + 2, 0x1234); /* BL quadrant differs */
  put16(rom, 0xc33f2 + 4, 0x18ff);
  put16(rom, 0xc33f2 + 6, 0x18ff);
  /* BG1 tile page (bgTileAdr=0): tile0 nonzero, tile1 all-zero. */
  ppu.vram[0] = 0xffff;
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  const uint32_t wrappedY = (uint32_t)(756 + 16);
  const uint16_t got = WsShadowTile(0, -8, wrappedY, 0, 0x6800, 0x5555);
  CHECK(got == 0x0001); /* tile 1, palette 0, no flips */
  /* Revalidation: overwrite tile1 graphics; the next frame must re-scan
   * and serve tile2, not the now-stale tile1. */
  for (unsigned w = 0; w < 16; w++)
    ppu.vram[16 + w] = 0x00ff;
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowTile(0, -8, wrappedY, 0, 0x6800, 0x5555) == 0x0002);
}

static void test_negworld_reset_and_off(void) {
  fixture();
  stage_fixture_cam0();
  for (unsigned q = 0; q < 4; q++)
    put16(rom, 0xc33f2 + q * 2, 0x18ff);
  Smw2ConfigureWsBgMargins();
  CHECK(WsShadowLayerActive(0));
  const uint32_t wrappedY = (uint32_t)(756 + 16);
  CHECK(WsShadowTile(0, -8, wrappedY, 0, 0x6800, 0x5555) == 0x18ff);
  /* Scene/lifetime reset (or WsShadowReset) disarms the option: the same
   * fetch returns to the realTile wrap. */
  WsShadowReset();
  CHECK(WsShadowTile(0, -8, wrappedY, 0, 0x6800, 0x5555) == 0x5555);
  /* Native-off: g_ws_active=0 never registers the layer. */
  g_ws_active = false;
  Smw2ConfigureWsBgMargins();
  CHECK(!WsShadowLayerActive(0));
  CHECK(WsShadowTile(0, -8, wrappedY, 0, 0x6800, 0x5555) == 0x5555);
}

int main(int argc, char **argv) {
  test_screen_table();
  test_gsu_rom();
  test_world_origin();
  test_near_uniform_view_stands_down();
  test_uniform_proven_view_arms();
  for (unsigned phase = 0; phase < 8; phase++)
    test_margin_store(phase);
  test_scene_transitions();
  test_strict_rearm();
  test_unloaded_cell_invalidated();
  test_screen_table_no_epoch();
  test_bg3_gates();
  test_reset_api();
  test_negworld_margin();
  test_negworld_transparent_fallback();
  test_negworld_reset_and_off();
  for (int i = 3; i < argc; i++)
    audit_dump(argv[1], argv[2], argv[i]);
  WsShadowReset();
  printf("smw2_ws: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
