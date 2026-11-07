# macOS build

This builds a native Apple Silicon (`arm64`) executable from a clean checkout.
No `.app` bundle or code signing is produced; the runnable binary is written to
`build-macos/SuperMarioWorld2YoshisIslandSNESRecomp`.

## Prerequisites

- macOS on Apple Silicon with Command Line Tools/Xcode clang available
- `git`, `cmake`, `ninja`, and `python3` on `PATH`
- A legally owned USA `.sfc` ROM matching `rom_identity.txt`
- Network access for CMake FetchContent on a fresh build directory

Homebrew is **not** required for the normal build. The framework supplies SDL3
through `SNESRECOMP_SDL3_FETCH`; Homebrew SDL3, FreeType, and HarfBuzz are
explicitly excluded so the executable links only macOS `/usr/lib` and `/System`
frameworks. Color emoji fall back to launcher outline glyphs on macOS.

## One-command build

```sh
SNESRECOMP_ROM="/path/to/Super Mario World 2 - Yoshi's Island (USA).sfc" \
    scripts/macos/build-macos.sh
```

or pass the ROM explicitly:

```sh
scripts/macos/build-macos.sh --rom "/path/to/Super Mario World 2 - Yoshi's Island (USA).sfc"
```

The script:

1. initializes all pinned submodules recursively;
2. applies every patch under `patches/` to the listed submodule trees and
   fails loudly if a patch neither applies nor is already applied;
3. runs `tools/regen.sh` to generate the uncommitted `src/gen/*.c` files;
4. configures `build-macos` with the framework's static SDL3 fetch path and
   builds it with Ninja.

## Run

```sh
./build-macos/SuperMarioWorld2YoshisIslandSNESRecomp \
    "/path/to/Super Mario World 2 - Yoshi's Island (USA).sfc"
```

Launching with no ROM argument opens the launcher ROM picker. Runtime files
(`config.ini`, `keybinds.ini`, `rom.cfg`, saves, staged assets) are anchored to
the executable directory, `build-macos/`.

## Optional: double-clickable `.app`

The build pipeline deliberately produces a bare executable. If you want a
Finder-launchable bundle instead, `scripts/macos/package-app.sh` wraps the
build tree's exe and its staged sidecars (assets, mods, shader presets) into
`dist/Yoshi's Island Recomp.app` with an `Info.plist` and an ad-hoc code
signature:

```sh
scripts/macos/package-app.sh                  # build-macos -> dist/
scripts/macos/package-app.sh build-macos out  # custom dirs
scripts/macos/package-app.sh build-macos dist --install   # copy into ~/Applications
open "dist/Yoshi's Island Recomp.app"         # or: open ~/Applications/"Yoshi's Island Recomp.app"
```

`--install` copies the bundle to `~/Applications/<APP_NAME>.app`, replacing any
older copy, so it shows up in Launchpad/Finder like any other app. The bundle
starts clean: on first run use **Browse For ROM** once (or hand it your
`rom.cfg` line) — the picker caches the path in `rom.cfg` beside the exe inside
the bundle, so later launches open straight onto the verified ROM.

If you move the app, the `rom.cfg` inside it keeps working as long as the ROM
file itself doesn't move — it stores an absolute path.

Per-machine state (`rom.cfg`, `config.ini`, `keybinds.ini`, saves, logs, ROMs)
is never bundled; the app writes its own beside the exe on first run. It is a
packaging convenience only — everything above about the plain binary still
applies, and bundling is unsigned/ad-hoc.

## Performance notes (measured on Apple M2 Pro)

- The build is **~100% interpreted**: this port's recompilation manifest
  covers only the IRQ vector, so nearly every guest opcode goes through the
  LakeSnes interpreter tier (`build-macos/last_run_report.json` →
  `execution_share` shows the interp/AOT split). Normal for this port's
  bring-up state, not a misconfiguration.
- `build-macos.sh` therefore configures: `INTERPROCEDURAL_OPTIMIZATION`
  (thin LTO — the interpreter loop + bus helpers live in separate TUs),
  `SNESRECOMP_INTERP_READ_IMPL=FUSED` (one ROM-map decode per fetch, not two),
  `SNESRECOMP_SUPERFX_IDLE_IMPL=NATIVE` (bulk idle-credit retire for Yoshi's
  parked GSU), and `-mcpu=native`. Measured: guest frame cost ~4.7 → ~4.0 ms
  median. Suspect a regression? Launch with `SNESRECOMP_FORCE_FLOOR=1` to get
  the faithful reference paths back at runtime.
- **Input latency**: `RunAhead = 1` in `config.ini` `[General]` (set in the
  installed .app). Speculates one extra frame and shows it, hiding ~16.6 ms
  of the game's own input latency at ~2 ms/frame cost — inside budget.
- FPS readout: press `f` (config `DisplayPerf`). Expect 60–61 on a 60 Hz panel.
- Frame pacing: SDL_Delay deadline loop + GL vsync (interval 1). A missed
  present costs one vsync; a `SNESRECOMP_FRAME_TIMING=<csv>` capture shows
  the ~11 ms vsync block inside `present_ms`.


## macOS quirks

- `tools/regen.sh` worked with the verified CloudStorage ROM path; no macOS-only
  source workaround was needed.
- SDL3 is built from the pinned upstream release as a static FetchContent
  dependency. The first configure can take a few minutes while it downloads
  and configures SDL.
- FreeType/HarfBuzz color-emoji support is intentionally disabled on macOS. If
  you intentionally want the optional color emoji path, drop the two
  `CMAKE_DISABLE_FIND_PACKAGE_*` settings and install both libraries, but expect
  Homebrew dylib links in the resulting binary.
- Unlike the Windows host path, the executable writes diagnostics to stdout/stderr;
  `SuperMarioWorld2YoshisIslandSNESRecomp.log` is not created on macOS.
- Runtime initialization was verified with SDL `video=cocoa`,
  `audio=coreaudio`, a 960×720 OpenGL window, `SnesInit: ok`, first frame/audio
  callbacks, and normal `SDL_QUIT` shutdown.
- Gatekeeper quarantine can still apply if the binary is moved into a different
  signed/unsigned packaging flow; this native checkout build is unsigned.
