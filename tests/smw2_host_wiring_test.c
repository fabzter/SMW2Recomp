/* Host-wiring contract for src/main.c, included directly (#define main
 * trick). Proves, against the REAL hook functions and the REAL geometry
 * helper:
 *   - kGameHost registers on_rom_loaded / on_reset / before_run_frame /
 *     prepare_frame / begin_sim_frame / on_shutdown;
 *   - before_run_frame derives margins from the OWNED window's fresh
 *     drawable PIXELS (scripted SDL_GetWindowSizeInPixels), not the stale
 *     g_ws_extra/g_ws_active fields — a live resize must change the armed
 *     margins on the very next frame;
 *   - prepare_frame returns helper geometry (aspect-aware, not the old
 *     8/7 formula);
 *   - begin_sim_frame runs BG margins THEN the sprite begin (order
 *     recorded), on_reset/on_rom_loaded/on_shutdown fire the teardown
 *     pairs.
 * Module entry points are recording stubs — the wiring is what is under
 * test, the modules are proven by their own suites. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- scripted environment ------------------------------------------ */
static int s_draw_w = 854, s_draw_h = 480;
static int s_extra_override = -1;
static int s_sprites_before_l, s_sprites_before_r, s_sprites_before_n;
static int s_sprites_begin_n, s_sprites_reset_n, s_sprites_shutdown_n;
static int s_bg_margins_n, s_bg_reset_n;
static char s_order[16];
static int s_order_n;

int PpuWsExtraOverride(void) { return s_extra_override; }
void Smw2SpritesBeforeFrame(int l, int r) {
  s_sprites_before_l = l;
  s_sprites_before_r = r;
  s_sprites_before_n++;
  s_order[s_order_n++] = 'B';
}
void Smw2SpritesBeginFrame(void) {
  s_sprites_begin_n++;
  s_order[s_order_n++] = 'b';
}
void Smw2SpritesReset(void) {
  s_sprites_reset_n++;
  s_order[s_order_n++] = 'R';
}
void Smw2SpritesShutdown(void) {
  s_sprites_shutdown_n++;
  s_order[s_order_n++] = 'S';
}
void Smw2ConfigureWsBgMargins(void) {
  s_bg_margins_n++;
  s_order[s_order_n++] = 'M';
}
void Smw2ResetWsBackgrounds(void) {
  s_bg_reset_n++;
  s_order[s_order_n++] = 'W';
}

/* beam frame driver entry points game_rtl.c binds */
void snes_beam_frame_driver_run_frame(void) {}
void snes_beam_frame_driver_draw_ppu_frame(void) {}
void snes_beam_frame_driver_reset(void) {}

/* beam driver snapshot hooks game_rtl.c binds (identity is what is tested) */
#include <stdbool.h>
#include <stddef.h>
struct SaveLoadInfo;
void snes_beam_frame_driver_state_save(struct SaveLoadInfo *sli) { (void)sli; }
void snes_beam_frame_driver_state_load(struct SaveLoadInfo *sli,
                                       uint32_t version) {
  (void)sli; (void)version;
}
void snes_beam_frame_driver_state_loaded(uint32_t version) { (void)version; }
const char *snes_beam_frame_driver_state_identity(void) { return "test"; }
bool snes_beam_frame_driver_state_preflight(const void *data, size_t size) {
  (void)data; (void)size; return true;
}

void snesrecomp_opengl_set_screenshot_sidecar(void (*fn)(const char *)) {
  (void)fn;
}

/* telemetry observers main.c calls (proven by their own run, not here) */
void Smw2TelemetryFrame(unsigned frame) { (void)frame; }
void Smw2TelemetrySidecar(const char *path) { (void)path; }

/* SDL window + scripted PIXEL-size drawable. The real SDL3 headers come in
 * through main.c's sdl_compat.h; SDL_Window is opaque, so complete it as a
 * dummy to own an instance, and override the size getter at LINK level —
 * SDL declares it extern, a definition here shadows it for this TU test. */
#include <stdbool.h>
struct SDL_Window { int unused; };
typedef struct SDL_Window SDL_Window;
SDL_Window *g_window;
static SDL_Window s_fake_window;

int snesrecomp_exe_dir_path(const char *rel, char *out, size_t size) {
  (void)rel;
  (void)out;
  (void)size;
  return 0;
}
void Smw2UpdateCheckOnStart(const char *v) { (void)v; }

/* The game's real units, verbatim. Their main() stays the process entry:
 * it calls snesrecomp_desktop_main(&kGameHost, ...), which the stub at the
 * bottom of this file redirects into run_wiring_tests. */
#include "../src/game_rtl.c"
#include "../src/main.c"

Config g_config;

/* real SDL3 signature (from the real SDL3 headers via sdl_compat.h) */
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *dw, int *dh) {
  assert(w == &s_fake_window);
  *dw = s_draw_w;
  *dh = s_draw_h;
  return true;
}

static void reset_counters(void) {
  s_sprites_before_n = s_sprites_begin_n = s_sprites_reset_n = 0;
  s_sprites_shutdown_n = s_bg_margins_n = s_bg_reset_n = 0;
  s_order_n = 0;
  memset(s_order, 0, sizeof s_order);
}

static int run_wiring_tests(void) {
  g_window = &s_fake_window;
  g_config.widescreen = 1;
  g_config.display_aspect = 0; /* CRT 4:3 */

  /* hook registration: every widescreen hook must be wired */
  assert(kGameHost.widescreen_supported && kGameHost.native_widescreen);
  assert(kGameHost.on_rom_loaded && kGameHost.on_reset &&
         kGameHost.before_run_frame && kGameHost.prepare_frame &&
         kGameHost.begin_sim_frame && kGameHost.on_shutdown);
  assert(kGameHost.game_info == &kGameInfo);

  /* fresh PIXELS geometry on every simulated frame: 854x480 CRT4:3 →
   * 342/43 (the helper, not the old 454/99 formula) */
  reset_counters();
  s_draw_w = 854;
  s_draw_h = 480;
  kGameHost.before_run_frame();
  assert(s_sprites_before_n == 1 && s_sprites_before_l == 43 &&
         s_sprites_before_r == 43);
  int w = -1, h = -1;
  kGameHost.prepare_frame(854, 480, &w, &h);
  assert(w == 342 && h == 224);

  /* LIVE RESIZE mid-sequence: drawable shrinks to 640x480 → the next
   * before_run_frame arms 0 margins, proving no stale g_ws_extra reuse */
  s_draw_w = 640;
  s_draw_h = 480;
  kGameHost.before_run_frame();
  assert(s_sprites_before_n == 2 && s_sprites_before_l == 0 &&
         s_sprites_before_r == 0);
  kGameHost.prepare_frame(640, 480, &w, &h);
  assert(w == 256 && h == 224);

  /* and back — margins recover on the very next simulated frame */
  s_draw_w = 3840;
  s_draw_h = 1080;
  kGameHost.before_run_frame();
  assert(s_sprites_before_n == 3 && s_sprites_before_l == 213 &&
         s_sprites_before_r == 213);
  kGameHost.prepare_frame(3840, 1080, &w, &h);
  assert(w == 682 && h == 224);

  /* begin_sim_frame order: BG margins (post-Prepare point) then sprite
   * begin — the pack lands before the published table is consumed */
  reset_counters();
  kGameHost.begin_sim_frame(60);
  assert(s_bg_margins_n == 1 && s_sprites_begin_n == 1);
  assert(s_order_n == 2 && s_order[0] == 'M' && s_order[1] == 'b');

  /* on_reset: sprite reset + world-store invalidation, order recorded */
  reset_counters();
  kGameHost.on_reset();
  assert(s_sprites_reset_n == 1 && s_bg_reset_n == 1);
  assert(s_order_n == 2 && s_order[0] == 'R' && s_order[1] == 'W');

  /* on_rom_loaded (pre-SnesInit / ROM swap) + on_shutdown: shutdown +
   * bg reset pairs */
  reset_counters();
  kGameHost.on_rom_loaded((const uint8_t *)"", 0);
  assert(s_sprites_shutdown_n == 1 && s_bg_reset_n == 1);
  reset_counters();
  kGameHost.on_shutdown();
  assert(s_sprites_shutdown_n == 1 && s_bg_reset_n == 1);

  /* widescreen off → faithful 256 even on a wide drawable */
  g_config.widescreen = 0;
  kGameHost.before_run_frame();
  assert(s_sprites_before_l == 0 && s_sprites_before_r == 0);
  kGameHost.prepare_frame(3840, 1080, &w, &h);
  assert(w == 256 && h == 224);
  g_config.widescreen = 1;

  /* env pin rides through the hook path (scripted override = 120) */
  s_extra_override = 120;
  s_draw_w = 854;
  s_draw_h = 480;
  kGameHost.before_run_frame();
  assert(s_sprites_before_l == 120 && s_sprites_before_r == 120);
  kGameHost.prepare_frame(854, 480, &w, &h);
  assert(w == 496 && h == 224);

  printf("smw2_host_wiring: PASS\n");
  return 0;
}

/* the game's own main() is the process entry: it calls
 * snesrecomp_desktop_main(&kGameHost, ...), which this stub redirects to
 * the wiring suite. */
int snesrecomp_desktop_main(const SnesDesktopHostGame *game, int argc,
                            char **argv) {
  assert(game == &kGameHost);
  (void)argc; (void)argv;
  return run_wiring_tests();
}
