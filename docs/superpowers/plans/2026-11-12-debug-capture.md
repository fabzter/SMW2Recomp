# Plan: debug-only video capture + centralized artifacts (SMW2Recomp)

Spec: the owner-approved design in the 2026-11-12 session (no separate spec
file; the design below IS the authority). Owner requirements, verbatim intent:

- A **debug-only build** carries the capture machinery; the release build must
  not pay for it ("I don't want the release version to be loaded with such
  consuming stuff").
- **All artifacts land in the repo working directory** (`<repo>/artifacts/`),
  not scattered across the machine (app bundle, /tmp, Desktop).
- Input recording stays (already exists); add **video recording toggled by a
  controller combination, default L3+R3** (press = start, press again = stop).
- The agent must get as much information as possible: video + frame index +
  telemetry + a full state dump at the stop frame.

## Global Constraints (binding)

- GC1: Release behavior is byte-unchanged when the new env vars are unset:
  `SNESRECOMP_ARTIFACT_DIR` unset ⇒ every existing path resolves exactly as
  today (exe-dir anchoring); `SNESRECOMP_ENABLE_VIDEO_CAPTURE` off ⇒ recorder
  code compiles to no-ops (no ffmpeg spawn, no per-present work beyond one
  predictable branch).
- GC2: The recorder is compile-gated by `SNESRECOMP_ENABLE_VIDEO_CAPTURE`
  (CMake option, default OFF), following the `SNESRECOMP_ENABLE_TRACE` pattern
  in `snesrecomp/runner/runner.cmake` (option → `add_compile_definitions` →
  conditional sources). Release cache `build-macos` is never reconfigured with
  the option ON; the debug build lives in `build-debug/`.
- GC3: Video captures the **pre-shader composited wide field** — the same
  `pixel_buffer`/`pitch`/`g_snes_width`×`g_snes_height` that `WritePpm`
  consumes in `DrawPpuFrameWithPerf` (host_main.c ~1662-1925). Pixel order is
  the one WritePpm uses: R=(p>>16)&255, G=(p>>8)&255, B=p&255 (host memory
  0xAARRGGRR little-endian ⇒ byte order B,G,R,X ⇒ ffmpeg `-pix_fmt bgr0`).
- GC4: ffmpeg is a **child process fed rawvideo over a pipe** (popen), never
  linked. Binary resolved from `SNESRECOMP_FFMPEG` env, else `ffmpeg` on PATH.
  Missing ffmpeg ⇒ toast + breadcrumb, recording stays off, game unaffected.
- GC5: Gesture uses the existing `PadGesture` machinery (host_main.c
  ~2104-2210): `[Controller] RecordGesture`, default `L3+R3`, "none" disables,
  <2 buttons refused (existing PadGestureConfigure rule). Edge-triggered via
  PadGesturePressed. Must not fire while rewind/save-browser/launcher overlays
  are open (same guards as the rewind gesture site ~5168).
- GC6: Artifact redirection is ONE helper in `host_paths.{c,h}`:
  `int snesrecomp_artifact_path(const char *leaf, char *out, size_t max_len)`
  — env `SNESRECOMP_ARTIFACT_DIR` set ⇒ `<dir>/<leaf>` (dir created once,
  recursively-safe via existing MkDir-style logic or mkdir loop); unset ⇒
  delegates to `snesrecomp_exe_dir_path` unchanged. Only ARTIFACT writers use
  it: ws_input_log.csv, ws_telemetry.csv, screenshots/ (present ppm + bmp +
  sidecar), video files, stop-frame dumps. Config/saves/rom.cfg/mods stay
  exe-anchored (they are game state, not artifacts).
- GC7: Every recorded video gets a sidecar `<name>.json` (start wall_ms, start
  guest frame, width, height, fps, extra/margins at start, build version) and
  a frame index `<name>.frames.csv` (`present_index,guest_frame,wall_ms`, one
  row per written frame) so video time maps to exact guest frames.
- GC8: On stop (gesture or shutdown while recording): close pipe, wait for
  ffmpeg exit, write a full state dump tagged `rec_stop_<timestamp>` via the
  existing `snes_state_dump` (VRAM/WRAM/SRAM/regs/OAM/fb) into the artifact
  dir, toast the file names.
- GC9: Submodule commit order: snesrecomp first (remote `fork`), then title
  (remote `origin`), branch `local/macos` in all repos. recomp-ui is NOT
  touched by this plan.
- GC10: No new dependencies. C11 + POSIX (popen, mkdir) only; macOS arm64 is
  the target; keep the code Windows-tolerant where trivial (`#ifndef _WIN32`
  around popen paths is acceptable — debug capture may be POSIX-only, but the
  RELEASE build must still compile on all platforms with the option OFF).
- GC11: Tests: extend the existing CTest wiring (CMakeLists.txt ~168+). New
  pure-logic unit tests must run in BOTH option states. Verification of the
  real recorder uses the headless replay harness pattern
  (tests/run_smw2_widescreen_native.py GameRun: private dir, copied binary,
  rom.cfg from the user's existing config, SDL dummy video NOT required —
  the present path runs with the normal cocoa window in a private dir).

## Architecture

```
[Controller L3+R3] ── PadGesturePressed ──> VideoRecToggle()
                                              │ start: popen(ffmpeg rawvideo pipe)
                                              │ stop:  pclose + snes_state_dump
[DrawPpuFrameWithPerf present path] ──> VideoRecPresent(pixel_buffer,pitch,w,h,frame)
                                              │ writes bgr0 row bytes + index row
[host_paths] snesrecomp_artifact_path(leaf) ── env dir or exe-dir (GC6)
[InputLogOpen / Smw2TelemetryFrame / screenshot writers] ── use artifact_path
[scripts/macos/build-debug.sh] ── build-debug cache, option ON
[Debug Play.command] ── env ARTIFACT_DIR=<repo>/artifacts, INPUT_RECORD=1,
                        exec build-debug binary (repo-root anchored)
```

## Tasks

### Task 1 — artifact path helper + redirect existing writers (snesrecomp + title)

Files:
- `snesrecomp/runner/src/host_paths.h` / `host_paths.c`: add
  `snesrecomp_artifact_path` per GC6. Read the env ONCE (static cache like
  other host knobs); create the directory on first use (mkdir -p semantics:
  walk the path creating components; ignore EEXIST). Return 0 only when
  exe-dir resolution also fails; fall back to bare leaf like existing callers.
- `snesrecomp/runner/src/desktop/host_main.c`:
  - `InputLogOpen` (~5692): default record path via artifact helper
    (explicit `SNESRECOMP_INPUT_RECORD=<path>` still wins verbatim).
  - Screenshot writers: `RequestScreenshot` path construction (the
    `screenshots/` relative paths around ~1836-1850 and the GL bmp path in
    opengl.c `SaveScreenshotBmp` — route BOTH through the helper; the GL side
    gets the dir via a small setter or by calling the helper directly since
    opengl.c already includes host_paths.h — verify include, add if missing).
  - `SNESRECOMP_SCREENSHOT_DIR` explicit env still wins verbatim.
- `src/smw2_telemetry.c`: `Smw2TelemetryFrame` (~100) default path via helper.
- Unit test `snesrecomp/runner/tests/host/artifact_path_test.c` (new; wire
  into the runner's test cmake the way neighboring host tests are wired —
  inspect `snesrecomp/runner/tests/host/video_settings/` for the pattern):
  env unset ⇒ equals exe_dir_path result; env set ⇒ joined path + dir
  created; env set to relative path ⇒ resolved against cwd (abspath helper).
  Use setenv/unsetenv in the test; no ROM needed.

Acceptance: ctest new test green in build-macos; a headless run with
SNESRECOMP_ARTIFACT_DIR=/tmp/artprobe + SNESRECOMP_INPUT_RECORD=1 +
SNESRECOMP_SCREENSHOT=/tmp/... writes ws_input_log.csv under /tmp/artprobe;
without the env, paths are byte-identical to today (verify by running the
existing probe script once and diffing log path lines).

### Task 2 — gated video recorder module (snesrecomp)

Files (new): `snesrecomp/runner/src/desktop/video_rec.h`, `video_rec.c`.
API (host-facing, all no-ops when the compile flag is off):

```c
void VideoRecConfigure(void);          /* read env knobs once at startup */
bool VideoRecAvailable(void);          /* compile flag && ffmpeg found */
void VideoRecToggle(void);             /* gesture entry: start or stop */
bool VideoRecActive(void);
void VideoRecPresent(const uint8_t *pixels, int pitch, int w, int h,
                     unsigned guest_frame);   /* per present, cheap when off */
void VideoRecShutdown(void);           /* quit path: stop if active */
```

Behavior:
- Compile gate `SNESRECOMP_VIDEO_CAPTURE` (define set by the CMake option in
  Task 3). With the flag off, the header provides static-inline no-ops and
  video_rec.c is NOT in the source list (GC2) — mirror how debug_server.h
  stubs at SNESRECOMP_TRACE=0.
- Start: resolve artifact dir via `snesrecomp_artifact_path("")`-style query
  (add `snesrecomp_artifact_dir(char*,size_t)` accessor in host_paths if
  cleaner); names `video_YYYYmmdd_HHMMSS.mp4` (+ `.json`, `.frames.csv` per
  GC7). Spawn:
  `ffmpeg -hide_banner -loglevel error -f rawvideo -pix_fmt bgr0 -s WxH -r 60 -i pipe:0 -c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p -movflags +faststart <path>`
  via popen("w"). W/H from the FIRST presented frame (window can resize
  mid-session: if w/h change while active, STOP the current file cleanly and
  start a new segment with the new size — index/sidecar per segment; simplest
  correct rule, no scaler).
- Per present: convert rows to bgr0 byte order is NOT needed — host memory
  already is B,G,R,X (GC3); fwrite rows directly (pitch may exceed w*4: write
  row-by-row). Append index row. If fwrite fails (ffmpeg died): stop cleanly,
  toast "recording stopped (encoder error)", breadcrumb.
- fps: fixed 60 (guest NTSC ~60.0988; document the 0.16% drift in the sidecar
  `fps_nominal: 60`); index maps presents→guest frames exactly, so drift is
  recoverable. Do NOT build a variable-fps muxer (YAGNI).
- Stop: fflush+pclose (waits), write stop-frame state dump (GC8) — the dump
  call needs guest state access: video_rec.c lives in the runner and may call
  `snes_state_dump(dir, tag, NULL, 0, 0, 0, frame)` (pixels NULL ⇒ no fb
  files; the present path already wrote the video) using g_snes/g_ppu externs
  like state_dump.c does. Toast: "REC stopped: <name> (<N> frames)".
- Shutdown: if active, stop (same path) so quitting mid-recording finalizes
  the mp4 (faststart) and the dump.
- Safety: popen/pclose POSIX-only; `#ifdef _WIN32` stubs return unavailable
  (GC10). No threads. All file writes through artifact helper.
- Unit test `snesrecomp/runner/tests/host/video_rec_test.c`: compile the
  module TWICE in the test target — once with the flag off (assert no-ops:
  VideoRecActive false, Toggle does nothing, Present tolerates NULL) and once
  with the flag on but ffmpeg forced missing via SNESRECOMP_FFMPEG=/nonexistent
  (assert Available false, Toggle leaves Active false, no crash). The real
  encode path is verified in Task 4's integration run, not here.

Acceptance: both flag states build; unit tests green; release binary size
delta ≈ 0 (no video_rec.c linked when OFF).

### Task 3 — CMake option + host wiring + gesture (snesrecomp + title)

Files:
- `snesrecomp/runner/runner.cmake`: `option(SNESRECOMP_ENABLE_VIDEO_CAPTURE
  "Debug-only gameplay video recorder (ffmpeg child process)" OFF)`; when ON:
  `add_compile_definitions(SNESRECOMP_VIDEO_CAPTURE=1)` + append video_rec.c
  to SNESRECOMP_RUNNER_SOURCES (place next to the TRACE option block ~425).
- `snesrecomp/runner/src/desktop/config.h` + `mmx_config.c`:
  `char record_gesture[64]` in Config; parse `[Controller] RecordGesture`
  (section 7, next to RewindGesture ~612); persist in WriteConfigFile kvs
  ONLY when non-empty (follow how RewindGesture is written — inspect and
  mirror; do not add a default line to fresh configs).
- `snesrecomp/runner/src/desktop/host_main.c`:
  - `static PadGesture g_record_gesture;` configure at ~4056 next to the
    rewind/launcher gestures: fallback "L3+R3", tag "record", key
    "RecordGesture". Gate configuration on VideoRecAvailable() so a release
    build never parses it (GC1).
  - Main-loop gesture site next to the rewind check (~5168):
    `if (VideoRecAvailable() && PadGesturePressed(&g_record_gesture) &&
    !snes_rewind_is_open() && !snes_savestate_menu_is_open()) VideoRecToggle();`
    (no `continue` — recording does not freeze the guest).
  - Present path: call `VideoRecPresent(pixel_buffer, pitch, w, h,
    g_present_frame)` immediately after the existing SCREENSHOT block
    (~1850), BEFORE the staged-copy/EndDraw — same buffer WritePpm reads.
  - Startup: `VideoRecConfigure()` near InputLogOpen (~4764). Shutdown:
    `VideoRecShutdown()` on every exit path after the main loop (find the
    single cleanup site; if multiple, the one that runs for normal quit).
  - OSD toasts come from video_rec.c via snes_osd_push (already linked).
- Title `CMakeLists.txt`: nothing required if runner.cmake appends sources
  before add_executable (verify order; the option block must precede the
  include at line 37 — it does, options live in runner.cmake which is
  included at 37 BEFORE add_executable at 39).

Acceptance: build-macos reconfigure+build green with option OFF (default);
`cmake -B build-debug -DSNESRECOMP_ENABLE_VIDEO_CAPTURE=ON` (same generator
and SDL flags as build-macos — copy the configure line from
scripts/macos/build-macos.sh minus the perf flags that are irrelevant)
builds green; ctest green in both caches.

### Task 4 — debug launcher, build script, integration verification (title)

Files:
- `scripts/macos/build-debug.sh` (new, executable): mirror build-macos.sh
  structure but: build dir `build-debug`, add
  `-DSNESRECOMP_ENABLE_VIDEO_CAPTURE=ON`, keep Release+LTO flags identical
  (debug refers to the capture feature, not -O0; the recorder must not
  perturb timing more than necessary). Skip regen if sources unchanged?
  NO — keep it simple: run the same submodule/patch/regen steps as
  build-macos.sh (idempotent), then configure+build build-debug.
- `Debug Play.command` (new, repo root, executable, bash): resolve repo root
  from the script location; export
  `SNESRECOMP_ARTIFACT_DIR="$repo/artifacts"`, `SNESRECOMP_INPUT_RECORD=1`,
  `SNESRECOMP_WS_BG_MARGINS_DEBUG=1`; exec
  `"$repo/build-debug/SuperMarioWorld2YoshisIslandSNESRecomp"`. If the binary
  is missing, print "run scripts/macos/build-debug.sh first" and exit 1.
  NOTE: running from repo root means config/saves anchor to repo root
  (exe-dir anchoring) — the script must copy the user's app config.ini,
  keybinds.ini, rom.cfg and saves/ into the repo root IF ABSENT (never
  overwrite), so the debug session plays the user's real settings/save.
  Document this in the script header.
- `.gitignore`: add `/artifacts/`, `/build-debug/` is already covered by
  `/build-*/`; add repo-root `ws_input_log.csv`, `ws_telemetry.csv`,
  `screenshots/`, `last_run_report.json` (runtime state when run from repo).
- Integration verification (manual, scripted in the report): launch the
  build-debug binary headless-style via the replay harness pattern with a
  recorded input session (reuse /tmp/wsfast/ws_input_log.csv if present,
  else record a short scripted session), SNESRECOMP_ARTIFACT_DIR=/tmp/vidprobe,
  and drive VideoRecToggle via a test-only env knob
  `SNESRECOMP_VIDEO_AUTOTEST=<start_frame>,<stop_frame>` (compiled ONLY with
  the capture flag; the main loop checks it once per frame and calls Toggle
  at those frames — this is the testable seam for the gesture path, which
  needs a physical pad). Verify: mp4 exists and ffprobe reports
  h264/yuv420p/WxH; frames.csv row count == stop-start presents ±1; sidecar
  fields present; stop-frame dump files exist; telemetry+input log landed in
  the artifact dir. Then decode 3 frames with ffmpeg and eyeball via the
  report (pixel sanity: not black, correct width).
- Docs: `docs/DEBUG_CAPTURE.md` — how to build, launch, gesture, where
  artifacts land, how the agent consumes them (frame index → guest frame →
  telemetry/dump correlation), the autotest knob, and the release-safety
  guarantees.

Acceptance: full integration run green with evidence in the report; release
build-macos binary unchanged in behavior (probe run path-diff from Task 1
still clean); Debug Play.command launches and records via autotest knob.

## Out of scope (documented, not built)

- Post-shader/GL-readback capture (CRT washout lies; pre-shader is ground
  truth).
- Audio in the video (guest audio capture is a separate subsystem; YAGNI).
- Variable-fps muxing, scaling, overlay burn-in (index + sidecar suffice).
- recomp-ui changes (no launcher UI row; gesture + env is the debug surface).
- Windows recorder implementation (stubs keep it compiling; POSIX-only).
