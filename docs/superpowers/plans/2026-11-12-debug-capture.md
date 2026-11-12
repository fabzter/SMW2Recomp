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

Contrarian review applied: `.superpowers/sdd/2026-11-12-debug-capture/
contrarian-review.md` (CRIT-01, HIGH-01..04, MED-01..07, LOW-01..11 all
folded in below; finding ids cited at the affected constraint/task).

## Global Constraints (binding)

- GC1: Release behavior is byte-unchanged when the new env vars are unset:
  `SNESRECOMP_ARTIFACT_DIR` unset ⇒ every existing path resolves exactly as
  today (exe-dir anchoring); `SNESRECOMP_ENABLE_VIDEO_CAPTURE` off ⇒ recorder
  code compiles to no-ops (no ffmpeg spawn, no per-present work beyond one
  predictable branch). Accepted carve-outs (LOW-05, LOW-06): release builds
  parse/store `[Controller] RecordGesture` inertly (Config +64 bytes) and may
  persist a user-set key; host_paths gains the artifact helpers in every
  build. The guarantee is BEHAVIORAL identity, not binary-size identity.
- GC2: The recorder is compile-gated by `SNESRECOMP_ENABLE_VIDEO_CAPTURE`
  (CMake option, default OFF), following the `SNESRECOMP_ENABLE_TRACE` pattern
  in `snesrecomp/runner/runner.cmake` (option → `add_compile_definitions` →
  conditional sources). Release cache `build-macos` is never reconfigured with
  the option ON; the debug build lives in `build-debug/`.
- GC3: Video captures the **pre-shader composited wide field** — the same
  `pixel_buffer`/`pitch`/`g_snes_width`×`g_snes_height` that `WritePpm`
  consumes in `DrawPpuFrameWithPerf` (host_main.c ~1662-1925), OSD included,
  post-blend ("what the player sees"). Pixel order is the one WritePpm uses:
  R=(p>>16)&255, G=(p>>8)&255, B=p&255 (host memory 0x??RRGGBB little-endian
  ⇒ byte order B,G,R,X ⇒ ffmpeg `-pix_fmt bgr0`).
- GC4: ffmpeg is a **child process fed rawvideo over a pipe** (popen), never
  linked. Binary resolved from `SNESRECOMP_FFMPEG` env (quoted in the shell
  command, LOW-04), else `ffmpeg` on PATH. **CRIT-01:** `signal(SIGPIPE,
  SIG_IGN)` once at configure (POSIX block) so a dead pipe yields EPIPE from
  fwrite, not process death; preflight the binary before popen (contains '/'
  ⇒ `access(X_OK)`; else spawn `<bin> -version` and check exit status) so
  Available()/Toggle() refuse with a toast instead of spawning a dead pipe.
  Missing ffmpeg ⇒ toast + breadcrumb, recording stays off, game unaffected.
- GC5: Gesture uses the existing `PadGesture` machinery (host_main.c
  ~2104-2210): `[Controller] RecordGesture`, default `L3+R3`, "none" disables,
  <2 buttons refused (existing PadGestureConfigure rule). Edge-triggered via
  PadGesturePressed. Must not fire while rewind/save-browser overlays are
  open (same guards as the rewind gesture site ~5168). Seat 0 only, like the
  existing gestures (LOW-02, document).
- GC6: Artifact redirection is ONE helper pair in `host_paths.{c,h}`:
  `int snesrecomp_artifact_path(const char *leaf, char *out, size_t max_len)`
  and `int snesrecomp_artifact_dir(char *out, size_t max_len)` (MED-05:
  required API, returns the ensured-to-exist dir). Env
  `SNESRECOMP_ARTIFACT_DIR` set ⇒ `<dir>/<leaf>`; unset ⇒ delegates to
  `snesrecomp_exe_dir_path` unchanged. **MED-01:** the helper mkdir -p's ALL
  parent components of the leaf (walk creating each, ignore EEXIST), so
  `screenshots/x.bmp` creates `<dir>/screenshots/`; callers' literal
  `MkDir("screenshots")` calls are removed/routed. Env read ONCE (static
  cache); test-only `snesrecomp_artifact_path_reset(void)` clears it.
  Only ARTIFACT writers use it: ws_input_log.csv, ws_telemetry.csv,
  screenshots/ (present ppm + bmp + sidecar), video files, stop-frame dumps.
  Config/saves/rom.cfg/mods stay exe-anchored (game state, not artifacts).
- GC7: Every recorded video gets a sidecar `<name>.json` (start wall_ms, start
  guest frame, width, height, fps_nominal, g_ws_extra, display aspect,
  run_ahead, frame_blend, build version — LOW-10) and a frame index
  `<name>.frames.csv` (`present_index,guest_frame,wall_ms`, one row per
  written frame). **MED-03:** `guest_frame` is the just-completed simulated
  frame, `snes_frame_counter ? snes_frame_counter - 1 : 0` (the FrameDump
  convention, host_main.c:1813) — NOT g_present_frame (post-increment host
  counter, diverges under run-ahead). Same convention for the stop-dump tag.
  wall_ms gaps during overlays are the alignment mechanism (LOW-09,
  document).
- GC8: On stop (gesture or shutdown while recording): fflush+pclose (waits;
  +faststart moov rewrite can stall a few hundred ms on large captures —
  LOW-08, document), write a full state dump tagged `rec_stop_<timestamp>`
  via `snes_state_dump(dir, tag, NULL, 0, 0, 0, frame)` into the artifact dir
  (dir ensured by GC6), toast the file names. Crash exits bypass the dump but
  the closed pipe still finalizes a playable mp4 (LOW-11, document).
- GC9: Submodule commit order: snesrecomp first (remote `fork`), then title
  (remote `origin`), branch `local/macos` in all repos. recomp-ui is NOT
  touched by this plan.
- GC10: No new dependencies. C11 + POSIX (popen, mkdir, signal) only; macOS
  arm64 is the target; `#ifdef _WIN32` stubs keep every build compiling with
  the option OFF (debug capture itself is POSIX-only).
- GC11 (HIGH-02): Unit tests are wired into the TITLE `CMakeLists.txt`
  `if(BUILD_TESTING)` block (~168+, precedent: `smw2_ws_test` compiling
  runner sources directly into a test exe) so `ctest` in build-macos AND
  build-debug discovers them. The standalone-project pattern under
  snesrecomp/tests/host/ is NOT used (it is invisible to the title ctest).
  video_rec tests are TWO executables (LOW-01): one without the define
  (exercises header no-op stubs), one with `SNESRECOMP_VIDEO_CAPTURE=1` +
  video_rec.c. Verification of the real recorder uses the headless replay
  harness pattern (tests/run_smw2_widescreen_native.py GameRun).
- GC12 (MED-04): video_rec.c is appended to SNESRECOMP_RUNNER_SOURCES and
  must therefore depend ONLY on `runner/src/` headers (host_paths.h,
  state_dump.h, snes_osd.h) + extern decls (g_snes, g_ppu,
  snes_frame_counter, g_ws_extra) + SNES_GAME_VERSION. No src/desktop/
  includes (config.h, host_main.h, opengl.h) — those are per-target.

## Architecture

```
[Controller L3+R3] ── PadGesturePressed ──> VideoRecToggle()
                                              │ start: preflight + popen(ffmpeg)
                                              │ stop:  pclose + snes_state_dump
[DrawPpuFrameWithPerf present path] ──> VideoRecPresent(pixel_buffer,pitch,w,h,
                                              snes_frame_counter-1)
                                              │ writes bgr0 rows + index row
[host_paths] snesrecomp_artifact_path(leaf) ── env dir or exe-dir (GC6)
[InputLogOpen / Smw2TelemetryFrame / screenshot writers] ── use artifact_path
[scripts/macos/build-debug.sh] ── build-debug cache, option ON
[Debug Play.command] ── env ARTIFACT_DIR=<repo>/artifacts, INPUT_RECORD=1,
                        exec build-debug binary; user config/saves copied
                        into build-debug/ (the exe-dir anchor), if absent
```

## Tasks

### Task 1 — artifact path helper + redirect existing writers (snesrecomp + title)

Files:
- `snesrecomp/runner/src/host_paths.h` / `host_paths.c`: add
  `snesrecomp_artifact_path` + `snesrecomp_artifact_dir` + test-only
  `snesrecomp_artifact_path_reset` per GC6 (mkdir -p ALL leaf parent
  components; env cached once; dir accessor returns ensured-to-exist dir).
- `snesrecomp/runner/src/desktop/host_main.c`:
  - `InputLogOpen` (~5692): default record path via artifact helper.
    **MED-06 boundary (preserve existing semantics exactly):** values
    containing '/' ⇒ verbatim (unchanged); `=1`/empty-but-set ⇒ artifact
    default path; bare names without '/' keep TODAY's behavior (exe-dir
    leaf) — do not relocate them.
  - Screenshot writers: the `screenshots/` relative paths ~1836-1850 and
    opengl.c `SaveScreenshotBmp` (~322-384) — route BOTH through the helper;
    remove the literal `MkDir("screenshots")` calls (helper creates parents,
    MED-01). opengl.c: add host_paths.h include if missing.
  - `SNESRECOMP_SCREENSHOT_DIR` explicit env still wins verbatim.
- `src/smw2_telemetry.c`: `Smw2TelemetryFrame` (~100) default path via helper.
- Unit test (GC11/HIGH-02): `tests/artifact_path_test.c` (title repo) +
  `add_executable(artifact_path_test tests/artifact_path_test.c
  ${SNESRECOMP_ROOT}/runner/src/host_paths.c)` + `add_test` inside the
  existing `if(BUILD_TESTING)` block. Cases: env unset ⇒ equals
  exe_dir_path result; env set ⇒ joined path + dir created; leaf with
  subdir ⇒ parents created; env relative ⇒ resolved against cwd.
  setenv/unsetenv + reset between cases; no ROM needed.

Acceptance: `ctest -R artifact_path` green in build-macos; a headless run
with SNESRECOMP_ARTIFACT_DIR=/tmp/artprobe + SNESRECOMP_INPUT_RECORD=1
writes ws_input_log.csv under /tmp/artprobe; without the env, paths are
byte-identical to today — verify with
`python3 tests/run_smw2_widescreen_native.py --scenario binaries` (LOW-07)
plus one probe run, diffing the log path lines.

### Task 2 — gated video recorder module (snesrecomp)

Files (new): `snesrecomp/runner/src/desktop/video_rec.h`, `video_rec.c`
(GC12: only runner/src/ headers + externs). API (host-facing, all no-ops
when the compile flag is off):

```c
void VideoRecConfigure(void);          /* env knobs + SIGPIPE ignore + probe */
bool VideoRecAvailable(void);          /* compile flag && ffmpeg preflight ok */
void VideoRecToggle(void);             /* gesture entry: start or stop */
bool VideoRecActive(void);
void VideoRecPresent(const uint8_t *pixels, int pitch, int w, int h,
                     unsigned guest_frame);   /* per present, cheap when off */
void VideoRecShutdown(void);           /* quit path: stop if active */
```

Behavior:
- Compile gate `SNESRECOMP_VIDEO_CAPTURE` (define set by the CMake option in
  Task 3). Flag off ⇒ header static-inline no-ops, video_rec.c not in the
  source list (GC2) — mirror debug_server.h stubs at SNESRECOMP_TRACE=0.
- Configure (CRIT-01): `signal(SIGPIPE, SIG_IGN)` (POSIX); resolve ffmpeg
  path (SNESRECOMP_FFMPEG quoted, else "ffmpeg"); preflight per GC4; cache
  availability. Idempotent; safe to call before anything else.
- Start: `snesrecomp_artifact_dir` (ensured to exist); names
  `video_YYYYmmdd_HHMMSS.mp4` (+ `.json`, `.frames.csv` per GC7). Spawn:
  `"<ffmpeg>" -hide_banner -loglevel error -f rawvideo -pix_fmt bgr0 -s WxH -r 60 -i pipe:0 -vf pad=ceil(iw/2)*2:ceil(ih/2)*2 -c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p -movflags +faststart "<path>"`
  via popen("w"). **HIGH-03:** the pad filter makes odd W/H legal (adaptive
  widescreen widths can be odd); sidecar records the raw W/H. W/H from the
  FIRST presented frame; on size change STOP the segment cleanly and start
  a new one (index/sidecar per segment; no scaler).
- Per present: host memory already B,G,R,X (GC3); fwrite row-by-row (pitch
  may exceed w*4). Append index row with guest_frame per GC7 (MED-03: the
  caller passes snes_frame_counter-1). fwrite failure (EPIPE — SIGPIPE is
  ignored) ⇒ stop cleanly, toast "recording stopped (encoder error)",
  breadcrumb.
- fps: fixed 60 (guest NTSC ~60.0988; sidecar fps_nominal: 60 documents the
  0.16% drift); index maps presents→guest frames exactly. No variable-fps
  muxer (YAGNI).
- Stop: fflush+pclose, stop-frame state dump (GC8) with the same guest-frame
  convention, toast "REC stopped: <name> (<N> frames)".
- Shutdown: if active, stop (same path).
- Safety: POSIX-only popen/pclose/signal; `#ifdef _WIN32` stubs return
  unavailable (GC10). No threads. All file writes through artifact helpers.
- Unit tests (GC11, LOW-01): in the title `if(BUILD_TESTING)` block, TWO
  executables from `tests/video_rec_test.c`: `video_rec_off_test` (no
  define; asserts header no-ops: Active false, Toggle inert, Present
  tolerates NULL) and `video_rec_on_test` (define=1 + video_rec.c +
  host_paths.c + state_dump.c deps as needed; SNESRECOMP_FFMPEG=/nonexistent
  ⇒ Available false, Toggle leaves Active false, no crash). Real encode
  path verified in Task 4.

Acceptance: both flag states build; both test exes green via ctest in
build-macos; release link contains no video_rec.o when OFF.

### Task 3 — CMake option + host wiring + gesture (snesrecomp + title)

Files:
- `snesrecomp/runner/runner.cmake`: `option(SNESRECOMP_ENABLE_VIDEO_CAPTURE
  "Debug-only gameplay video recorder (ffmpeg child process)" OFF)`; when ON:
  `add_compile_definitions(SNESRECOMP_VIDEO_CAPTURE=1)` + append video_rec.c
  to SNESRECOMP_RUNNER_SOURCES (next to the TRACE option block ~425).
- `snesrecomp/runner/src/desktop/config.h` + `mmx_config.c`:
  `char record_gesture[64]` in Config; parse `[Controller] RecordGesture`
  (section 7, next to RewindGesture ~612). **MED-02:** WriteConfigFile has
  NO gesture-persistence precedent (RewindGesture is never written; CfgSet
  Dies on a missing kvs row). Mechanism: add the kvs row AND emit it only
  when `record_gesture[0]` is non-empty (conditional CfgSet after the kvs
  loop, or a done-flagged row — inspect CfgSet/CfgFind and pick the safe
  one). Release builds may persist a USER-SET key (GC1 carve-out); never
  write a default/empty line.
- `snesrecomp/runner/src/desktop/host_main.c`:
  - **HIGH-04 ordering:** call `VideoRecConfigure()` BEFORE the gesture
    configure block (~4056), not near InputLogOpen. Then
    `static PadGesture g_record_gesture;` configured there: fallback
    "L3+R3", tag "record", key "RecordGesture", gated on
    VideoRecAvailable() (release never parses it).
  - Main-loop gesture site next to the rewind check (~5168):
    `if (VideoRecAvailable() && PadGesturePressed(&g_record_gesture) &&
    !snes_rewind_is_open() && !snes_savestate_menu_is_open()) VideoRecToggle();`
    (no `continue` — recording does not freeze the guest).
  - Present path: `VideoRecPresent(pixel_buffer, pitch, w, h,
    snes_frame_counter ? snes_frame_counter - 1 : 0)` immediately after the
    existing SCREENSHOT block (~1850), BEFORE the staged-copy/EndDraw —
    same buffer WritePpm reads (MED-03 convention).
  - Shutdown: `VideoRecShutdown()` at the normal-quit cleanup site after the
    main loop (~5305-5413, near snes_rewind_shutdown).
  - Test seam: `SNESRECOMP_VIDEO_AUTOTEST=<start_frame>,<stop_frame>` —
    main-loop check INSIDE `#if SNESRECOMP_VIDEO_CAPTURE` (LOW-05: compiled
    out of release, not merely gated at runtime), calls VideoRecToggle at
    those frames (headless-testable seam; the gesture needs a physical pad).
  - OSD toasts come from video_rec.c via snes_osd_push (already linked).
- Title `CMakeLists.txt`: nothing required (runner.cmake include at 37
  precedes add_executable at 39; verified).

Acceptance: build-macos reconfigure+build green with option OFF (default);
build-debug configured with the SAME flags as build-macos.sh plus only
`-B build-debug -DSNESRECOMP_ENABLE_VIDEO_CAPTURE=ON` (MED-07: verbatim
configure copy — keep LTO/FUSED/NATIVE/-mcpu=native so the debug binary
reproduces release code paths); ctest green in both caches.

### Task 4 — debug launcher, build script, integration verification (title)

Files:
- `scripts/macos/build-debug.sh` (new, executable): mirror build-macos.sh
  (same submodule/patch/regen steps, idempotent), configure line copied
  VERBATIM plus `-B build-debug -DSNESRECOMP_ENABLE_VIDEO_CAPTURE=ON`
  (MED-07). Release+LTO flags identical (debug = the capture feature, not
  -O0).
- `Debug Play.command` (new, repo root, executable, bash): resolve repo root
  from script location; export `SNESRECOMP_ARTIFACT_DIR="$repo/artifacts"`,
  `SNESRECOMP_INPUT_RECORD=1`, `SNESRECOMP_WS_BG_MARGINS_DEBUG=1`; exec
  `"$repo/build-debug/SuperMarioWorld2YoshisIslandSNESRecomp"`. Missing
  binary ⇒ print "run scripts/macos/build-debug.sh first", exit 1.
  **HIGH-01:** the host chdirs to the EXE dir (build-debug/), NOT repo root
  (snesrecomp_anchor_to_exe_dir). Copy the user's app config.ini,
  keybinds.ini, rom.cfg and saves/ into `$repo/build-debug/` IF ABSENT
  (never overwrite). Script header documents this.
- `.gitignore`: add `/artifacts/` ONLY (HIGH-01: build-debug/ is already
  covered by `/build-*/`; runtime state lands there, so no root-level
  ws-log/screenshots entries are needed).
- Integration verification (scripted, evidence in the report): launch the
  build-debug binary via the replay harness pattern with a recorded input
  session (reuse /tmp/wsfast/ws_input_log.csv if present, else record a
  short scripted session), SNESRECOMP_ARTIFACT_DIR=/tmp/vidprobe, drive
  recording via SNESRECOMP_VIDEO_AUTOTEST. Verify: mp4 exists, ffprobe
  reports h264/yuv420p/even-dims; frames.csv rows == stop-start presents ±1
  and guest_frame values match the input-log convention (MED-03); sidecar
  fields present; stop-frame dump files exist; telemetry+input log in
  artifact dir. Decode 3 frames with ffmpeg; pixel sanity (not black,
  correct width). Also verify an ODD-width run (pin SNESRECOMP_WS_EXTRA to
  an odd value) encodes successfully (HIGH-03).
- Docs: `docs/DEBUG_CAPTURE.md` — build, launch, gesture (seat 0 only,
  LOW-02), artifact locations, agent consumption (frame index → guest frame
  → telemetry/dump, the snes_frame_counter-1 convention, wall_ms gaps during
  overlays LOW-09), autotest knob, pclose/faststart stall (LOW-08), crash
  captures (LOW-11), release-safety guarantees + GC1 carve-outs.

Acceptance: full integration run green with evidence; release build-macos
behavior unchanged (Task 1 path-diff still clean); Debug Play.command
launches and records via autotest knob.

## Out of scope (documented, not built)

- Post-shader/GL-readback capture (CRT washout lies; pre-shader is ground
  truth).
- Audio in the video (guest audio capture is a separate subsystem; YAGNI).
- Variable-fps muxing, scaling, overlay burn-in (index + sidecar suffice).
- recomp-ui changes (no launcher UI row; gesture + env is the debug surface).
- Windows recorder implementation (stubs keep it compiling; POSIX-only).
- was_held reset across overlays (LOW-03: acceptable edge; holding L3+R3
  through an overlay fires once after close — documented, not fixed).
