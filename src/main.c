/*
 * Super Mario World 2: Yoshi's Island — desktop host shim.
 *
 * Identity and hooks, nothing else. The host itself is the framework's
 * (snesrecomp/runner/src/desktop/host_main.h): the pre-boot launcher, ROM
 * resolution and digest checks, config.ini and keybinds.ini, the window and
 * the SDL / OpenGL presenters, audio, gamepads, the in-game save-state
 * browser and rewind filmstrip, the OSD, the pacing clock, crash handlers and
 * the post-mortem report, mod packages, Generate & rebuild, and the netplay
 * barrier when the project is built with it. A fix there reaches this
 * project on a submodule pull; nothing in this file needs to change for it.
 *
 * This file used to BE the host -- a ~700-line copy made at scaffold time --
 * so every project scaffolded before a fix kept the bug. Two ports of the
 * same game, a week apart, had different hosts.
 *
 * Nothing here ever ships a ROM. The player supplies one; the host makes
 * that easy (launcher, then positional argument, then a copy beside the
 * executable, then the rom.cfg cache, then a file picker) and checks what it
 * is handed against the digests in rom_identity.txt.
 *
 * Grow this file only with what is specific to THIS title: a custom
 * presenter (prepare_frame / draw_frame), an SPC player, a pacing rule
 * (keep_pacing_debt), a Mods provider. See SnesDesktopHostGame for the
 * complete list of hooks and what each is for.
 */

#include "game_rtl.h"
#include "host_main.h"
#include "smw2_rtl.h"
#include "snesrecomp_rom_identity.h" /* generated from rom_identity.txt */

#ifndef __ANDROID__
#define SDL_MAIN_HANDLED 1
#endif
#include "desktop/sdl_compat.h"
#ifdef __ANDROID__
/* SDLActivity loads libmain.so and dlsyms its SDL_main. */
#define main SDL_main
#endif

#ifndef SNES_GAME_VERSION
#define SNES_GAME_VERSION "dev"
#endif

#include "config.h"
#include "host_paths.h"
#include "smw2_telemetry.h"
#include "smw2_update.h"
#ifndef __ANDROID__
#include "opengl.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef RECOMP_LAUNCHER
#include "recomp_launcher.h"
/* ROM patches (.ips/.ips32/.bps): the launcher applies them to the verified
 * stock image and boots the patched copy. This build runs ~100% interpreted,
 * so patched guest code actually executes (recompiled banks would bypass it).
 * The cache dir holds the prepared image per (source sha1, patch sha1); it
 * must exist and live beside the exe with the rest of the mod data. */
static void Smw2ConfigureLauncher(RecompLauncherCGameInfo *info) {
  info->renderer_note =
      "Shaders need OpenGL: choosing one switches the renderer to OpenGL.";
  static char s_patch_cache[1024];
  if (!s_patch_cache[0]) {
    if (!snesrecomp_exe_dir_path("mods/rom-patches", s_patch_cache,
                                 sizeof(s_patch_cache)))
      snprintf(s_patch_cache, sizeof(s_patch_cache), "mods/rom-patches");
    mkdir(s_patch_cache, 0777);
  }
  info->rom_patch_supported = 1;
  info->has_window_ratio = 1;
  info->rom_patch_cache_dir = s_patch_cache;
#ifdef _WIN32
  info->host_toggle_label = "Check for updates on start";
  info->host_toggle_help =
      "Look for a newer release on GitHub when the game starts and offer to "
      "install it. Saves, save states and settings are kept.";
  info->host_toggle_get = Smw2UpdateGetEnabled;
  info->host_toggle_set = Smw2UpdateSetEnabled;
#endif
}

#endif

/* Window shape for an Auto-sized window: the configured ratio applied to the
 * 240-line base height. 4:3 keeps the historical 320x240; wider ratios give
 * the adaptive widescreen margins more room. An explicit WindowSize=WxH in
 * config.ini still overrides (the host checks custom_size first). */
static int Smw2WindowBaseWidth(int frame_w) {
  (void)frame_w;
  static const int kNum[4] = {4, 16, 21, 32};
  static const int kDen[4] = {3, 9, 9, 9};
  const unsigned idx = g_config.window_ratio < 4 ? g_config.window_ratio : 0;
  return 240 * kNum[idx] / kDen[idx];
}

#if defined(_WIN32) && !defined(SMW2_CONSOLE)
#include <io.h>
#include <stdio.h>
#undef HIBYTE /* the framework's types.h has its own; this file does not use   \
                 it */
#undef LOBYTE
#include <windows.h>

/* The Windows build is a window app (no console, see CMakeLists.txt). Keep the
 * host's stdout/stderr diagnostics in <exe>.log beside the executable unless
 * the caller already redirected them. */
static void Smw2RedirectLogsToFile(void) {
  if (GetStdHandle(STD_ERROR_HANDLE) != NULL)
    return;
  wchar_t path[MAX_PATH];
  DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH - 4)
    return;
  wchar_t *dot = wcsrchr(path, L'.');
  wcscpy_s(dot ? dot : path + len,
           MAX_PATH - (size_t)((dot ? dot : path + len) - path), L".log");
  FILE *log;
  if (_wfreopen_s(&log, path, L"w", stderr) != 0)
    return;
  setvbuf(stderr, NULL, _IONBF, 0);
  if (_dup2(_fileno(stderr), 1) == 0)
    setvbuf(stdout, NULL, _IONBF, 0);
}
#endif

/* GLSL presets (assets/shaders) render only on the OpenGL presenter, so make
 * it this title's default. A fresh install starts with no shader: the plain
 * picture costs the GPU almost nothing, while a full-screen CRT preset made
 * weaker laptops drop frames. Whatever the player picks in the launcher is
 * saved to config.ini and left alone. */
static void Smw2AfterConfig(void) {
  if (g_config.renderer[0] == '\0') {
    strcpy(g_config.renderer, "opengl");
    g_config.output_method = kOutputMethod_OpenGL;
  }
  /* MSU-1: arm it automatically when a pack lives in <exe>/msu1/. The env
   * wins when the user set one explicitly (explicit base > auto folder). */
  if (!getenv("SNESRECOMP_MSU1")) {
    static char s_msu_dir[1024];
    struct stat st;
    if (snesrecomp_exe_dir_path("msu1", s_msu_dir, sizeof(s_msu_dir)) &&
        stat(s_msu_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
      setenv("SNESRECOMP_MSU1", s_msu_dir, 0);
    }
  }
  Smw2UpdateCheckOnStart(SNES_GAME_VERSION);
#ifndef __ANDROID__
  snesrecomp_opengl_set_screenshot_sidecar(Smw2TelemetrySidecar);
#endif
}

/* ── Widescreen (full adaptive scope) ──────────────────────────────────────
 * The PPU line renderer fills kPpuBufWidth-wide fields; this title opts into
 * the generic widened path the host shares with MMX rather than the SuperFX
 * projection replay (YI's GSU tasks are sprite/OAM compositors — there is no
 * center_x/max_x RAM pair for WidescreenLinearProjection to widen).
 *
 * Frame width follows the window's drawable aspect under the configured
 * pixel aspect (display_aspect.h), clamped to the PPU's 272-per-side
 * budget. Two hooks need that same geometry per simulated frame and share
 * it through smw2_ws_geometry.h so they can never diverge:
 *
 *   - prepare_frame runs INSIDE PreparePpuFrame, which then publishes the
 *     result as the live margins (PpuSetExtraSpace) and g_ws_extra;
 *   - before_run_frame runs BEFORE PreparePpuFrame, where g_ws_extra /
 *     g_ws_active still hold LAST frame's values — the sprite policy must
 *     arm against fresh drawable PIXELS, never the stale host fields.
 *
 * The sprite lifecycle is Task 3's (smw2_sprites.c): BeforeFrame arms the
 * title's own pack/spawn/despawn hooks against the live margins, BeginFrame
 * consumes one completed pack capture and publishes its signed OAM
 * coordinates (a packed-content binding — SnesRenderIndexOam stays the raw
 * identity key; the table never decodes positions by itself). The
 * background policy is Task 2's (smw2_rtl.c): Configure runs at its
 * existing post-Prepare point where g_ws_* are fresh. */
#include "host_api.h" /* g_window — the owned window; never a guessed id */
#include "ppu.h"
#include "smw2_sprites.h"
#include "smw2_telemetry.h"
#include "smw2_ws_geometry.h"

/* Per-frame margins from the owned window's drawable, in PIXELS (the
 * SDL3 call is SDL_GetWindowSizeInPixels — never the Retina-doubled-mismatch
 * logical size). before_run_frame runs on the host's main thread, so the
 * main-thread-only query is legal there. */
static Smw2WsGeometry Smw2WsFrameGeometry(void) {
  int dw = 0, dh = 0;
  if (g_window)
    snesrecomp_sdl_get_drawable_size(g_window, &dw, &dh);
  return Smw2WsComputeGeometry(dw, dh, g_config.display_aspect,
                               g_config.widescreen, PpuWsExtraOverride());
}

static void Smw2PrepareFrame(int drawable_w, int drawable_h, int *w, int *h) {
  const Smw2WsGeometry geo =
      Smw2WsComputeGeometry(drawable_w, drawable_h, g_config.display_aspect,
                            g_config.widescreen, PpuWsExtraOverride());
  *w = geo.width;
  *h = 224;
}

/* Simulated frame, before guest execution (and before PreparePpuFrame —
 * g_ws_extra is stale here): arm the sprite pack/spawn/despawn widening
 * against THIS frame's fresh margins. */
static void Smw2BeforeRunFrame(void) {
  const Smw2WsGeometry geo = Smw2WsFrameGeometry();
  Smw2SpritesBeforeFrame(geo.left_extra, geo.right_extra);
}

/* After guest simulation, around the raster (PreparePpuFrame has just
 * republished g_ws_extra/extraLeftRight): the background margin engine
 * consumes them at its existing point, then the sprite module consumes
 * this frame's completed pack capture once and publishes the validated
 * signed-coordinate table. No capture leaves the last publication (or a
 * rollback-restored one) untouched — republishing a cached title capture
 * after a rewind is exactly what BeginFrame refuses to do. */
static void Smw2BeginFrame(unsigned number) {
  (void)number;
  Smw2ConfigureWsBgMargins();
  Smw2SpritesBeginFrame();
  Smw2TelemetryFrame(number);
}

/* Timeline jump (reset / savestate load / overlay close — GameReset's only
 * callers): pending capture ownership and the published coordinate table
 * belong to the abandoned timeline, and the world-keyed background store
 * re-proves cold on the next frame. */
static void Smw2OnReset(void) {
  Smw2SpritesReset();
  Smw2ResetWsBackgrounds();
}

/* The ROM bytes are in hand but the machine is not yet built (the host
 * calls this before SnesInit, including on a ROM swap): drop any module
 * ownership bound to the previous cart/core. Both calls are machine-free
 * safe. */
static void Smw2OnRomLoaded(const uint8_t *rom, size_t size) {
  (void)rom;
  (void)size;
  Smw2SpritesShutdown();
  Smw2ResetWsBackgrounds();
}

/* The frame loop is over and the host is about to tear the machine down:
 * disarm this title's GSU hooks while their core still exists. */
static void Smw2OnShutdown(void) {
  Smw2SpritesShutdown();
  Smw2ResetWsBackgrounds();
}

static const SnesDesktopHostGame kGameHost = {
    .display_name = "Super Mario World 2: Yoshi's Island",
    .window_title = "Super Mario World 2: Yoshi's Island",
    .region = SNESRECOMP_ROM_REGION,
    .rom_file = SNESRECOMP_ROM_FILE,
    .expected_sha256_hex = SNESRECOMP_ROM_EXPECTED_SHA256,
    .expected_crc32_hex = SNESRECOMP_ROM_EXPECTED_CRC32,
    .game_id = SNESRECOMP_ROM_GAME_ID,
    .build_version = SNES_GAME_VERSION,
    .game_info = &kGameInfo,
    .num_players = 1,
    .shader_supported = 1,
    .window_base_width = Smw2WindowBaseWidth,
    .after_config = Smw2AfterConfig,
#ifdef RECOMP_LAUNCHER
    .configure_launcher = Smw2ConfigureLauncher,
#endif
    /* Full adaptive widescreen: launcher toggle + adaptive PPU margins,
     * title-proven BG margins, and pack-level sprite widening. */
    .widescreen_supported = 1,
    .native_widescreen = 1,
    .on_rom_loaded = Smw2OnRomLoaded,
    .on_reset = Smw2OnReset,
    .before_run_frame = Smw2BeforeRunFrame,
    .prepare_frame = Smw2PrepareFrame,
    .begin_sim_frame = Smw2BeginFrame,
    .on_shutdown = Smw2OnShutdown,
    /* Battery-backed SRAM shows the launcher's SAVES panel. Leave NULL for a
     * title without one. The path is exe-relative. */
    .sram_path = "saves/save.srm",
    /* Ctrl+L (or Select+L3) reopens the launcher mid-game; shader, scale,
     * volume and bindings apply on RESUME without a restart. */
    .in_game_launcher = 1,
};

#ifdef _WIN32
/* Laptops with switchable graphics (NVIDIA Optimus, AMD PowerXpress) run an
 * unknown program on the integrated GPU, which cannot keep a full-screen
 * shader at 60 fps. The drivers read these exports from the executable and
 * pick the discrete GPU instead. */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

#ifndef __ANDROID__
#undef main /* desktop: keep plain main() even if SDL_main.h remapped it */
#endif
int main(int argc, char **argv) {
#if defined(_WIN32) && !defined(SMW2_CONSOLE)
  Smw2RedirectLogsToFile();
#endif
  return snesrecomp_desktop_main(&kGameHost, argc, argv);
}
