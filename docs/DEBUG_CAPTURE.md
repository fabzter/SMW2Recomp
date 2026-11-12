# Debug Video Capture

This is a debug-build diagnostic surface; it does not add a launcher UI row. Captures are pre-shader composited wide frames (OSD included), without audio.

## Build

Build into the separate `build-debug/` tree. The script mirrors `scripts/macos/build-macos.sh`'s Release optimization and LTO configure flags, then enables capture; this is not an `-O0` debug build.

```sh
./scripts/macos/build-debug.sh --rom "/path/to/Yoshi's Island (USA).sfc"
# Or set SNESRECOMP_ROM to that path and omit --rom.
```

The ROM is required for regeneration. `build-debug.sh` initializes pinned submodules, applies or verifies the normal framework patch stacks, regenerates sources, configures the capture option ON, then builds. This script assumes the integrated `local/macos` baseline. If no patch is forward-applicable and full reverse-stack verification succeeds, the stack is skipped. If no patch is forward-applicable and verification fails, committed local history is treated as having superseded the stack; the script warns and continues. If any patch applies forward, the script attempts those forward-applicable patches in stack order; an actual `git apply` failure aborts. Reverse-applicability of other patches is irrelevant. Fresh upstream checkouts must run `scripts/macos/build-macos.sh` first to establish the supported baseline. The release cache remains separate and capture-OFF.

## Launch and local state

Double-click `Debug Play.command` in Finder, or run it from a terminal. If the debug binary is missing, the command tells you to run `scripts/macos/build-debug.sh` first. The host anchors its working directory to the executable directory (`build-debug/`), not the repository root. On first launch only, the command copies `config.ini`, `keybinds.ini`, `rom.cfg`, and `saves/` from:

```text
$HOME/Applications/Yoshi's Island Recomp.app/Contents/MacOS/
```

It copies each item only when the destination is absent; it never refreshes or overwrites existing debug-local state. Game changes to the debug copy do not flow back to the installed app. The command exports `SNESRECOMP_ARTIFACT_DIR=<repo>/artifacts`, `SNESRECOMP_INPUT_RECORD=1`, and `SNESRECOMP_WS_BG_MARGINS_DEBUG=1` before executing the binary.

The default capture gesture is **L3 + R3 on seat 0 only**. Press once to start, then press again to stop. Set the debug copy's `[Graphics] WindowSize` to the target size when reproducing a particular layout; for the owner's ultrawide run, use `3440x1378` (the observed `g_ws_extra` is 112).

## Artifact layout and agent workflow

The artifact directory contains append/session-marked `ws_input_log.csv` and `ws_telemetry.csv`, plus video/index/metadata and stop-state files:

```text
artifacts/
  video_<timestamp>.mp4
  video_<timestamp>.frames.csv       # present_index,guest_frame,wall_ms
  video_<timestamp>.json             # start_wall_ms, start_guest_frame, width, height,
                                    # fps_nominal, g_ws_extra, display_aspect,
                                    # run_ahead, frame_blend, build_version
  rec_stop_<timestamp>.*             # full state dump at stop
```

To investigate a frame:

1. Select `present_index` in the video `.frames.csv`; its `guest_frame` is the just-completed simulated frame, `snes_frame_counter ? snes_frame_counter - 1 : 0`. It is **not** `g_present_frame` (which can diverge under run-ahead).
2. Use that guest-frame number to locate the corresponding input-log row, respecting the `# session` markers. Correlate by `wall_ms` with the nearby telemetry row; wall-clock gaps between video rows are expected when overlays skip presents and preserve timing across the gap.
3. Inspect the `rec_stop_<timestamp>.info.json` / `.regs.json` frame and the associated `.sram.bin`, `.wram.bin`, `.vram.bin`, `.cgram.bin`, `.oam.bin`, `.apu.tsv`, `.gsu.tsv`, `.dma.tsv`, and `.ppuw.tsv` files. The dump is written at the recorder stop frame (the next stop-threshold frame can be one past the last video row).

The `.json` sidecar records the video start state and output dimensions; use it with the per-present index rather than inferring game frames from video timestamps alone. `ws_input_log.csv` and `ws_telemetry.csv` are append-mode files; use their session markers when matching a run.

## Headless/autotest capture

`SNESRECOMP_VIDEO_AUTOTEST=<start>,<stop>` starts and stops capture at guest-frame thresholds. The `.frames.csv` records the exact guest frames captured (the first row can be the just-completed frame immediately before the start threshold). The supported script interface reads commands from a file; piping `wait`/`quit` to stdin does not drive the headless runner.

```sh
printf 'wait 3200\nquit\n' > /tmp/video-capture-script.txt
env SNESRECOMP_ARTIFACT_DIR=/tmp/vidprobe \
    SNESRECOMP_INPUT_REPLAY=/tmp/wsfast/ws_input_log.csv \
    SNESRECOMP_REPLAY_SESSION=1 \
    SNESRECOMP_VIDEO_AUTOTEST=950,1050 \
    ./build-debug/SuperMarioWorld2YoshisIslandSNESRecomp \
      --no-launcher --config build-debug/config.ini \
      --script /tmp/video-capture-script.txt
```

The input replay selector is zero-based; leave it unset to use the last session by default. `SNESRECOMP_ARTIFACT_DIR` can point a direct binary run at a scratch directory instead of the repository `artifacts/` directory. `Debug Play.command` intentionally fixes its artifact directory to `<repo>/artifacts`.

The pad filter is insurance, not an odd-dimension test: capture width is `256 + 2*extra` (symmetric margins), so it is always even by construction. The odd-width acceptance step cannot exercise odd dimensions; do not rerun it expecting odd input.
Stopping normally flushes the raw-video pipe with `fflush`/`pclose`; `pclose` waits for ffmpeg and its `+faststart` moov rewrite can take a few hundred milliseconds for a large capture. Wait for normal shutdown to finish before inspecting the MP4. A crash bypasses the `rec_stop_` state dump, but closing the pipe can still let ffmpeg finalize a playable MP4.

## Release safety and known baseline

`build-macos/` keeps `SNESRECOMP_ENABLE_VIDEO_CAPTURE=OFF`. With `SNESRECOMP_ARTIFACT_DIR` unset, existing artifact paths continue to resolve beside the executable. With capture compiled off, no recorder/ffmpeg process is created and the recorder path adds no per-present work beyond its predictable gate. GC1's accepted carve-outs are behavioral, not binary-size identity: release builds parse/store `[Controller] RecordGesture` inertly (Config +64 bytes) and may persist a user-set key; the artifact helper pair is present in `host_paths` in all builds.

Expected baseline in both CTest caches is **4/5**: only `smw2_ws_addresses` fails at `tests/smw2_ws_test.c:410` and `:438` (the two pre-existing checks). Do not treat those failures as a video-capture regression or change them in this task.
