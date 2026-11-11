# Widescreen glitch catalog — Yoshi's Island (SMW2Recomp)

Living catalog of every reported or suspected widescreen artifact, the
evidence that classified it, the layer responsible, and its status.
Reproduce any frame with the probe route in
`tests/run_smw2_widescreen_native.py` (`PROBE_SCRIPT`, deterministic frame
numbers) or `/tmp`-style ad-hoc scripts; presented-frame captures use
`SNESRECOMP_SCREENSHOT[_FRAME]` (pre-shader wide PPM) or the in-game
`Ctrl+F9` hotkey (BMP + `.json` sidecar + paired `present_*.ppm`).
Per-frame state lives in `ws_telemetry.csv` beside the executable
(append-mode, `# session` markers; `armed` reads 2 while the gutter freeze
is active).

Frame numbers below are from the canonical fresh-SRAM probe route at
16:9 (extra = 43 px/side).

## Fixed

### G1 — Repeated-terrain strip in the east gutter (chronic)
- Symptom: east gutter shows wrapped terrain copies while playing; steady
  rate, every frame, worse the wider the window.
- Evidence: `eastRawFallback`/`eastMiss` grew +210 px/frame from the first
  armed frame (658,038 over the route). `SNESRECOMP_WS_MISS_DEBUG` pinned
  every miss to screen x = 297 — one pixel column, tile dx = 38: the MAP16
  prefill loop stopped at `dx < 32 + extra_tiles`, one tile short of the
  phase-overhang column.
- Layer: title `src/smw2_rtl.c` prefill bounds (framework fallback chain
  miss → raw VRAM wrap in `ws_shadow.c` is the amplifier, by design).
- Fix: bounds `±(extra_tiles + 1)`; post-fix route raw-wrap 0/0, misses
  0/0, every margin fetch a hit (16:9 and 32:9).
- Status: fixed, telemetry-proven (`51c45a5`).

### G2 — Far-east level art in the west gutter at the level's west edge
- Symptom: left gutter shows terrain that belongs to the far side of the
  level while the camera sits at the west edge.
- Evidence: mechanism — the prefill cast negative `tx` to `uint32`;
  `smw2_bg1_tile`'s screen index masks into a populated far-east screen
  (the exact case its own comment forbids) and `ForceTile` keyed that art
  at negative world coordinates, where west-margin fetches then hit it.
- Layer: title prefill loop.
- Fix: skip negative `tx`/`ty`; west-of-world cells fall to the validated
  neg-world empty tile (`westBlank` serves, `westRawFallback` 0).
- Status: fixed (`51c45a5`).

### G3 — East prefill deleted before the raster read it
- Symptom: same visual family as G1 (east gutter wrap) on frames where the
  phase column was not the culprit.
- Evidence: `ws_shadow` legacy `eastKeep = 0` prunes every east-of-view
  cell each frame, including the cells the title prefilled that same
  frame; misses persisted after the G1 bound fix until keeps were set.
- Layer: framework prune policy + title not setting keeps.
- Fix: `WsShadowSetEastKeep/WestKeep(extra_tiles + 2)` per armed frame.
- Status: fixed (`51c45a5`).

### G4 — One-frame gutter strobe at scene/layout transitions
- Symptom: both gutters flash wrong content for a frame or two at message
  boxes, ring touches, stage-intro boundaries, cross-section BGMODE flips.
- Evidence: telemetry armed flips 1→0→1 on consecutive frames (8632/8633,
  11322/23, 11617/18, 13102/03, 13405/06 …): a single failed proof or a
  scene-word change ran `WsShadowReset()`, exposing raw map wrap until the
  cold 12/12 re-proof.
- Layer: title transition handling + framework (no freeze primitive).
- Fix: `WsShadowSetFrameHold` (framework): hold keeps the layer active
  with its store and skips the capture; title holds on transient proof
  miss, on scene-word change (deferred `WsShadowReset` on the next full
  proof), and centers the native window when BGMODE leaves the provable
  family. Hold is bounded at 30 consecutive frames, after which the
  original stand-down semantics apply; telemetry `armed=2` marks holds.
- Status: fixed (`51c45a5`, `d812e14`; framework `0c76c83d`, `be4a4d1b`).

## Ruled out — genuine game content, not widescreen artifacts

### N1 — White mask + yellow sliver cluster
- Frames 11925–12655 (both gutters and native field depending on camera).
- Identical at the same route frame with `Widescreen = 0`
  (`off2_11925`): the hint-arrow sign mid pulse-animation plus its mask
  object. Native-faithful.
### N2 — Black oval with glyph / white ring with red mark / coins
- Message-ring and hint objects; present in native-off references
  (`off_12744`). Native-faithful.
### N3 — Dark west gutter at the level's west edge
- Honest neg-world empty tile (nothing exists west of the level start);
  not a miss into wrap (`westRawFallback` 0).
### N4 — Enemies/coins visible in the gutters
- Widened spawn window working as designed (documented behavior change
  under widescreen, see `docs/WIDESCREEN.md`).

## Not reproduced

### P1 — Sprite ghosts / missing margin sprites (user-reported)
- Consecutive-frame shift-correlation of both margin bands mid-scroll
  (11986→11998, camera +10..12 px/step): bands match at the world shift
  (1.5–3.2k px residual) far better than screen-fixed (3.8–5.7k) — content
  moves with the world; residuals are walking enemies and entering
  columns. Exit-scroll segment (12680–12770) visually clean post-fix.
- Discriminators for future reports: telemetry `published`/`grace` vs the
  paired `present_*.ppm` (margin sprite absent from the native band while
  its slot is parked = ghost); screen-fixed margin pixels across
  consecutive scrolling frames.
- Status: open, awaiting user captures from campaign scenes.

### P2 — Mode7 / vehicle / vertical-scroll scenes
- Not reachable from the probe route without an exit-record table; the
  Apple GL affine gate (`db8cb07b`) already keeps mode7 frames on the
  native renderer there. Status: open, awaiting user captures.

## Capture-path artifacts (tooling, not the game)

- `dump <tag>` framebuffers crop to the native 256 columns **by design**
  (`state_dump.h`); margins are only visible in presented captures
  (`SNESRECOMP_SCREENSHOT*`, hotkey BMP/PPM, window pixels).
- Hotkey BMP under a GLSL preset can read back washed out on some
  presenters; the paired `present_<time>.ppm` (pre-shader wide field) is
  the ground truth (`be4a4d1b`).
- `SkipLauncher = 1` runs never activate the app: their windows are
  invisible to AX/CG enumeration until something fronts the process;
  turbo boots make them vanish from capture tools entirely. Use
  `loadstate` + paced config (`VSync = 1`, `DisableFrameDelay = 0`) for
  live-window work.
- Unpaced configs (`VSync = 0` + `DisableFrameDelay = 1`) simulate at
  ~200 fps: "real-time" assumptions in scripts break; turbo only changes
  wall time, never guest frames.
- Parallel launches of the same workspace race mod-state staging
  ("cannot publish mod state"); serialize or give each run its own dir.
