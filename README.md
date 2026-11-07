# Super Mario World 2: Yoshi's Island Recomp

A native recompilation of **Super Mario World 2: Yoshi's Island** (USA), built on
[snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp).

Yoshi's Island is Nintendo's 1995 platformer for the Super NES: Yoshi carries
baby Mario across the island to rescue his brother Luigi from Kamek and Baby
Bowser. Its hand-drawn look, with sprites stretched, rotated and morphed on the
cartridge's Super FX 2 chip, made it one of the most distinctive games of the
console.

> **You must legally own a copy of the game.** No ROM data is distributed with
> this project, in the repository or in any release. The recompiled C is
> generated locally from your own copy and is never committed.

## ☕ Keep the ports coming

<p align="center">
  <a href="https://buymeacoffee.com/mpolizel"><img src="https://img.shields.io/badge/Buy%20me%20a%20coffee-support%20more%20ports-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=black" alt="Buy me a coffee"></a>
</p>

**This project is free and always will be.** Every port in this series,
Yoshi's Island included, is built with many hours of AI-assisted reverse engineering,
and that AI time is paid. If you enjoy playing it, a coffee at
**[buymeacoffee.com/mpolizel](https://buymeacoffee.com/mpolizel)** keeps the AI running so **more SNES
games can be ported to native Recomp**. Any amount helps, and thank you!

## Download (ready-to-run Windows build)

Get the latest `yoshisisland-<version>-windows-x64.zip` from the
[Releases page](https://github.com/mpolizel/SMW2Recomp/releases/latest).

The zip is a **setup pack**: it contains the launcher, the recompiler and this
source tree (with the framework patches already applied), but no game data.
On first run:

1. Unzip anywhere and start `SuperMarioWorld2YoshisIslandSNESRecomp.exe`.
2. Let it download the portable build tools (one time per machine), or point
   it at a `cmake-clang-v1` zip you already have.
3. Pick your own `Super Mario World 2 - Yoshi's Island (USA).sfc` (CRC32 `d138f224`;
   the file name does not matter, the contents are checked).
4. Press **Generate & rebuild**. The playable game is built into `build/` and
   started; run it from there afterwards.

In the launcher, **Settings → Shader** lists the scanline, CRT and upscaler
presets; choosing one switches the renderer to OpenGL automatically. A fresh
install starts with no shader (the plain picture), the lightest option for
any graphics card; pick a CRT or upscaler preset there if yours has room.
Your choice is saved in `config.ini` and kept. The upscalers include
**ScaleFX** and the neural **NNEDI3** presets.

During play, **Ctrl+L** (or **Select+L3** on a controller) reopens the
launcher: shader, window scale, volume and controls apply when you press
**Resume**, without restarting the game. **I** cycles the audio interpolation
between **Gaussian** (the console's own, the default), **Cubic** (crisper
highs) and **Sinc**; it only changes what you hear, never the game, and the
choice is saved in `audio_interpolation.txt`.

On laptops with two graphics cards (NVIDIA Optimus, AMD switchable graphics)
the game asks the driver for the dedicated one. If it still runs slowly,
set the game's executable to **High performance** in Windows Settings →
System → Display → Graphics.

### Saves and updates

The game's own save (the three save files on the cartridge's battery) is kept
in `build\saves\save.srm`.

With **Check for updates on start** ticked (the default; on the launcher's main
screen, next to **Skip launcher on boot**), the game asks GitHub for the latest
release when it starts and offers to install a newer one. Accepting closes the
game, downloads the new setup pack, copies it over the install folder and
reopens the setup window: press **Generate & rebuild**. Nothing is deleted;
`build\` (the rebuilt game, `saves\`, save states, `config.ini`,
`keybinds.ini`) and `rom.cfg` are kept. Untick the box to turn the check off
(saved in `update.ini`).

**Your saves are never lost or broken by an update.** In-game saves and save
states made by any release keep working in every later one. Before installing,
the updater copies `build\saves\` to `save-backups\<date>_before_<version>\`
and never deletes those copies; save files are left out of the copy, and if
they changed anyway the backup is put back. Every release after the first is
checked against the one before it with `tools/check_update_compat.ps1`, which
builds the previous release, makes an in-game save and a save state with it,
installs the new pack with the previous release's own updater and rebuilds; it
fails unless every save file is byte-identical and the new build continues the
old save state.

See [CHANGELOG.md](CHANGELOG.md) for what changed in each release.

## Status

Boots through the Nintendo logo, the opening story and the title screen into
the file select, the intro on Yoshi's Island and the first level, at full speed
with the Super FX graphics drawn. The in-game save is written to
`save.srm`. Most code still runs through the snesrecomp interpreter; every
world and boss is not yet verified. See [Porting from here](#porting-from-here).

### Framework patches

`patches/` holds changes to the `snesrecomp` and `recomp-ui` submodules.
Reapply them after updating the submodules:

```sh
git -C snesrecomp apply ../patches/snesrecomp-audio-interpolation.patch
git -C snesrecomp apply ../patches/snesrecomp-glsl-passprev-and-vao.patch
git -C snesrecomp apply ../patches/snesrecomp-mosaic-frame-model.patch
git -C snesrecomp apply ../patches/snesrecomp-nmi-idle-loop-wai.patch
git -C snesrecomp apply ../patches/snesrecomp-nmi-wai-in-handler.patch
git -C snesrecomp apply ../patches/snesrecomp-raster-oam-and-hdma.patch
git -C snesrecomp apply ../patches/snesrecomp-raster-vram-journal.patch
git -C snesrecomp apply ../patches/snesrecomp-resume-state-restart.patch
git -C snesrecomp apply ../patches/snesrecomp-shader-forces-opengl.patch
git -C recomp-ui apply ../patches/recomp-ui-host-toggle.patch
```

`snesrecomp-raster-oam-and-hdma.patch` is the renderer fix shared with the
other ports: per-line sprite tables, HDMA set up in vblank, and vblank writes
kept in their own field. It also counts HDMA's first transfer, made in V=0's
H-blank, as a table line, as the hardware does. Yoshi's Island uploads
Yoshi's tiles and his sprite table together at line 221; in 0.1.0 the
renderer drew every line with the newest tiles, so the per-line sprite table
was limited to screen splits (with the older table under the new tiles, Yoshi
showed extra heads). Since 0.1.1 the tiles are journaled too (below), and the
table and tiles of each line come from the same moment.

`snesrecomp-raster-vram-journal.patch` makes the renderer draw each line with
the video memory, palettes and sprite table it had on hardware. The renderer
runs after the CPU half of a frame, so a write made while the picture is
drawn (in forced blank at the bottom of the screen) used to reach every line
of that frame; it is now journaled and replayed from its own line, and writes
made by the next frame's NMI before the render are kept out. It also reads
BG mode 2's offset-per-tile rows at BG3VOFS and BG3VOFS+8 (the code read
BG3VOFS-1 and +7, a row off when BG3VOFS is a multiple of 8) and drops a +1
on the vertical offset, as bsnes does. Both showed in World 1-7: the stage
was assembled from the wrong rows, then shook 8 pixels back and forth while
Yoshi walked, because the next frame's offset table, uploaded at line 217,
was drawn under this frame's fine scroll. With video memory journaled, the
per-line sprite table of `snesrecomp-raster-oam-and-hdma.patch` applies to
every frame that rewrites it again.

`snesrecomp-resume-state-restart.patch` makes a setting that needs a restart
come back to the same moment. The in-game launcher saves `saves\resume.sav`,
restarts and loads it before the first frame, when no session exists yet. A
save state holds the machine but not the CPU half's registers or resume
point (a load during play keeps the session's own, the game's idle loop), so
the frame driver booted from the reset vector over it. The restart file is
now written in the rollback format (machine plus execution residue), and the
frame driver takes the interpreter's resume point at the start of a frame.
Save-state slots keep their format.

`snesrecomp-mosaic-frame-model.patch` builds the mosaic table on every visible
line. The optimized renderer built it only on line 0, which frame-model hosts
never render, so every line of a mosaic-enabled layer sampled the layer's
first row.

`snesrecomp-nmi-idle-loop-wai.patch` and `snesrecomp-nmi-wai-in-handler.patch`
make a `WAI` halt until the next interrupt, as on hardware, in the main loop
and inside an interrupt handler; without them the interpreter could spin to
its step cap and freeze the screen.

`snesrecomp-audio-interpolation.patch` adds the Cubic and Sinc audio
interpolation (I key). They resample the dry output only; the echo buffer,
OUTX, pitch modulation and save states stay those of the console's Gaussian
filter, so the game runs identically in every mode.

`snesrecomp-glsl-passprev-and-vao.patch` fixes two shader bugs: a pass that
reads `PassPrev<n>` back to the game frame got nothing (ScaleFX drew black),
and switching to no shader mid-game drew a black screen because the shader
path left the plain path's vertex attributes disabled.

`snesrecomp-shader-forces-opengl.patch` switches the renderer to OpenGL when a
shader is picked, since the presets only run there.

`recomp-ui-host-toggle.patch` lets a title add one checkbox of its own beside
"Skip launcher on boot"; this project uses it for the update check.

## ROM identity

| | |
|---|---|
| File | `Super Mario World 2 - Yoshi's Island (USA).sfc` |
| Publisher | Nintendo |
| Developer | Nintendo EAD |
| Year | 1995 |
| Mapping | lorom |
| Region | USA (North America) |
| Coprocessor | Super FX 2 (GSU-2) |
| CRC32 | `d138f224` |
| SHA-256 | `9b4957466798bbdb5b43a450bbb60b2591ae81d95b891430f62d53ca62e8bc7b` |

`tools/regen.sh` refuses to run against anything else, so a mismatched
revision fails immediately instead of producing subtly wrong output.

## Build

```sh
git submodule update --init --recursive
bash tools/regen.sh --rom "/path/to/Super Mario World 2 - Yoshi's Island (USA).sfc"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
**macOS:** `scripts/macos/build-macos.sh --rom "<rom path>"` reproduces the whole
pipeline (submodules, patches, regen, configure, build) into `build-macos/`.
See [scripts/macos/MACOS.md](scripts/macos/MACOS.md) for prerequisites and
quirks.

The ROM does not have to live in the repository — keeping it on your own
drive is the better habit, and `SNESRECOMP_ROM` sets the path once for a
shell. Without either, `tools/regen.sh` finds
`Super Mario World 2 - Yoshis Island (USA).sfc` at the repo root, then the path
the setup wizard recorded in `rom.cfg`; `.gitignore` blocks both from ever
being committed.

`tools/regen.sh` verifies the ROM, generates `src/gen/*.c`, and re-syncs
`recomp/funcs.h`. Re-run it whenever you change anything under `recomp/`.

The Windows build is a window app: diagnostics go to
`SuperMarioWorld2YoshisIslandSNESRecomp.log` beside the executable.
`-DSMW2_CONSOLE=ON` brings the console back for debugging.

## Run

```sh
./build/SuperMarioWorld2YoshisIslandSNESRecomp                    # launcher picks the ROM
./build/SuperMarioWorld2YoshisIslandSNESRecomp "/path/to/Super Mario World 2 - Yoshi's Island (USA).sfc"
```

With no ROM on the command line the build opens the recomp-ui launcher: a ROM
picker with a verification badge, plus display / audio / input settings. Your
choice is cached in `rom.cfg` beside the executable, so the next launch opens
on it and "Skip launcher on boot" makes it immediate.

Either way the ROM is checked against the digests above before anything boots
— a dump that could not have produced this build is refused at the door rather
than mis-executing ten frames in.

## Layout

| Path | What lives there |
|---|---|
| `recomp/` | Analysis input: `bank*.cfg`, `symbols.toml`, generated `funcs.h` |
| `rom_identity.txt` | ROM digests — read by the build, `tools/regen.sh` and CI |
| `src/` | Host code you own: `main.c`, `game_rtl.c`, `smw2_update.c` |
| `src/gen/` | Generated C. Never committed — regenerate locally |
| `assets/shaders/` | Scanline, CRT and upscaler presets for the launcher |
| `patches/` | Changes to the framework submodules (see above) |
| `snesrecomp/` | Framework submodule (owns `lib/recomp-net`, `lib/retcomm-rbengine`) |
| `tools/` | `regen.sh` — the ROM → C pipeline; `check_update_compat.ps1`; `import_libretro_shaders.py` |
| `scripts/` | `package_release.sh` — player-facing zip |
| `framework_pins.txt` | Exact framework commits this project was scaffolded against |

## Porting from here

The scaffold stops where the game-specific work starts. In rough order:

1. **Make it boot.** `src/game_rtl.c` holds the frame driver. It starts on
   the framework's beam-aligned driver (`snesrecomp/runner/src/beam_frame_driver.h`):
   one frame per PPU field, interrupts taken where the beam latches them,
   the field rasterized from the raster journal.
2. **Name things.** Add entries to `recomp/symbols.toml` as you identify
   routines, then re-run `tools/regen.sh`. Set `emit = true` to promote one
   into ahead-of-time analysis; leave it false to keep it interpreted.
   See `recomp/README.md`.
3. **Resolve dispatch misses.** After every run, deal with unresolved
   indirect targets before anything else — they are the reason a port
   diverges, and they are cheap to fix early.
4. **Never synthesise a result** to get past uncovered code, and never edit
   `src/gen/` by hand. Fix the config or the framework and regenerate.

## Multiplayer

Single player.

## License

This project's own source is under the MIT license in `LICENSE`. The
framework carries its own terms (PolyForm Noncommercial 1.0.0) — see
`snesrecomp/LICENSE` and `snesrecomp/THIRD_PARTY_ATTRIBUTION.md`; the changes
in `patches/` apply to that framework and fall under its terms. The shaders
under `assets/shaders/_libretro/` come from libretro/glsl-shaders and keep each
author's license in their file headers. None of these cover the game data,
which is not distributed here.

---

<p align="center">
  Enjoying Yoshi's Island Recomp? <a href="https://buymeacoffee.com/mpolizel"><b>☕ Buy me a coffee</b></a> to help
  bring the next SNES game to native Recomp.
</p>
