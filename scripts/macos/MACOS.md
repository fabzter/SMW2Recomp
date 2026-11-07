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
