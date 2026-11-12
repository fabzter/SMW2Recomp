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

### G6 — Seam squares around moving sprites in the native band
- Symptom: jarring rectangle seams around Yoshi/carried sprites and other
  moving objects; inside the rect the picture looks squashed/shifted.
  Owner capture at frame 11372 (climbed 1-1, extra=112); reproduced in
  harness at frame ~8080 of the climb route.
- Evidence: native-band diff widescreen-on vs widescreen-off = one 34x32
  rect around the sprite cluster; per-sprite centroids shifted ~+3,+2 px
  under widescreen. `SNESRECOMP_WS_SPRITES=0` (publication kill switch)
  made the native band pixel-identical to widescreen-off, isolating the
  signed-coordinate publication as the sole delta.
- Mechanism: publication overrode every matched slot's decode with the
  raw GSU source value; for native-visible parts the packed OAM byte is
  the game's own rounded integer, so moving sprites shifted by the
  rounding delta (static sprites round identically, hence "only when
  running/moving").
- Layer: title sprite policy (`src/smw2_sprites.c` publication gate).
- Fix: publish coordinates only for widened-only parts (admitted,
  x_vis false); native-visible and parked parts keep hardware decode
  (`ff32628`). Post-fix native band diff = None; margins still show
  widened-only sprites at true signed positions.
- Status: fixed, A/B-proven.

### G7 — Margins collapse at the 4:3 seam in the stairs/climbed section (was P3)
- Symptom: in the climbed/stairs section of 1-1 (cam ~1001,1577, extra=112)
  the wide margins collapse to centered bars and objects straddling the 4:3
  edge clip jarringly. Owner capture frame 3014 of a recorded play session.
- Evidence: frame-exact input replay (`SNESRECOMP_INPUT_REPLAY`, session 0)
  reproduced the owner's `present_*.ppm` with **0/107520 pixel diff**. The
  state dump shows the proof is **12/12**, not 0/12 — but all twelve diagonal
  samples are the same authored sky tile `0x18ff` (modal=12), so
  `near_uniform` fired and stood the layer down (`armed=0`). Offline MAP16
  audit: native window 780/780 and **both gutters 465/465 tiles decode**, 24
  distinct tiles incl. platform/stair art that continues seamlessly into the
  margins. The view was fully provable and fully decodable.
- Mechanism: `smw2_bg1_stream_proven`/`_valid` vetoed ANY near-uniform view
  (`modal_n > 9`), even a perfect 12/12 proof. The heuristic exists to reject
  a repeated-tile coincidence on a PARTIAL proof; applied to a complete proof
  it wrongly stood down correct views whose samples happen to land on sky.
- Layer: title proof gate (`src/smw2_rtl.c`).
- Fix: a perfect 12/12 proof arms regardless of uniformity; the near-uniform
  guard now vetoes only the tolerant partial-proof path (`hits >= 10 && !nu`).
  Regression `test_uniform_proven_view_arms` (uniform-but-proven arms; one
  corrupted sample still stands down). Post-fix replay: stairs `armed=1`,
  native 4:3 band **0 pixels changed**, all 14,625 changed pixels in the
  margins (real terrain replaces the collapse).
- Status: fixed, replay + offline-audit proven. Corrects the earlier P3
  hypothesis (stream-window offset / 0/12): the origin model was never wrong.

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
### N5 — West margin reveals outdoors at the cave edge during fast west scroll
- Symptom (owner report, recorded session quit at frame 3121, cam x~1973,
  stage 1 cave, extra=112): scrolling west fast, the west margin shows sky /
  clouds / outdoor props against the native cave, with a hard vertical edge
  at the 4:3 boundary; reads as a seam "at the limits of the original box".
- Evidence (frame-exact replay, dumps 3050-3120): the MAP16 world model
  decodes the west-margin cells (world tx ~230-243) to the empty tile
  `0x18ff` at EVERY frame — including frames where the camera was east of
  them and they were on-screen (cam_x 2169, wtx0=271). The screen-id table
  (GSU $0CAA) has those screens loaded, and the whole-level MAP16 table
  (WRAM $7F8000) is populated. So the west is GENUINELY outdoors in the
  level data; BG1 (cave) is empty there and BG2/BG3 outdoors shows through.
  The hard edge is the game's authored cave-wall boundary, not a fetch
  miss, not stale data, not a streaming gap.
- Classification: native-faithful content. Widescreen reveals the real
  cave→outdoors transition earlier than 4:3 would; during a fast scroll the
  reveal slides in quickly, which is what reads as "glitchy". Same family
  as N4 (a disclosed widescreen behavior change, not an artifact).
- Status: CLOSED — owner accepted the faithful reveal (2026-11-11). No code
  change; the margin shows the real cave→outdoors transition. Two fix
  attempts were made and REVERTED before this classification (see the
  analysis traps below); the installed build is the faithful one.

### G5 — Phantom terrain after a chronic stand-down (user session, real play)
- Symptom: both gutters fill with wrong flat/repeated terrain ("phantom
  tiles") and stay that way for minutes; reported from normal-speed play
  in the campaign (climbed section of 1-1, cam ~1090,1601, extra=112).
  A related flash appeared in the intro cutscenes (foreign art or wrap for
  a frame per cut) once the transition hold existed.
- Evidence: the session's own `ws_telemetry.csv` — proof hits drop to 0/12
  in that section (world-origin vs streamed VRAM window disagree), hold
  (`armed=2`) repeats, the 30-frame budget expires at frame 5001-5073,
  and from frame 5073 to quit (12798) `armed=0` with the layer inactive:
  inactive layers render the rolling tilemap wrap into the gutters, and
  nothing ever re-armed. Misses/raw stopped counting only because the
  ws_shadow fetch path is skipped entirely when inactive.
- Layer: title stand-down policy (`smw2_rtl.c`) + framework inactive-layer
  margin behavior (raw wrap by design for unregistered layers).
- Fix: centered-native fallback for any unarmed wide frame (`641e348`);
  hold became move-aware — a proof failure while the camera moves centers
  instead of freezing a store that cannot answer new rows (`a279a4f`);
  scene cuts reset+center immediately instead of holding foreign art for
  a frame, which was the intro/cutscene flash (`fe87e77`). Reproduction at
  the exact camera via disclosed `forcepoke` of the camera mirrors (note:
  forcepoke desyncs the GSU scroll link, so it proves the fallback, not
  the proof failure itself).
- Status: phantom tiles fixed. The underlying stairs-section stand-down was
  later root-caused and fixed as **G7** (the `near_uniform` veto, not a proof
  failure or stream offset) — those sections now render full margins.

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

### P3 — RESOLVED → G7 (margins collapse in the stairs/climbed section)
- The earlier hypothesis here (chronic `proof hits=0/12` from a stream-window
  offset the origin model misses) was **disproven** by a frame-exact input
  replay of the owner's recorded session: the proof is 12/12 and the origin
  model is correct. The real cause was the `near_uniform` veto standing down
  a fully-provable, fully-decodable view whose twelve samples all land on the
  same sky tile. Fixed and replay-proven — see **G7** under Fixed.
- Method that cracked it (keep for future reports): record a real play
  session (`Recorded Play.command` / `SNESRECOMP_INPUT_RECORD`), replay it
  headless at the owner's WindowSize (`SNESRECOMP_INPUT_REPLAY` +
  `SNESRECOMP_REPLAY_SESSION`), `dump` the glitch frame, and audit the walk
  and gutter decode offline against the dump. Pixel-diff the replay present
  against the owner's capture to prove the repro before theorizing.
- Pacing note for future visual runs: observation runs must use VSync=1 +
  DisableFrameDelay=0 (config_paced); unpaced configs simulate at ~170-280
  fps and hide present-timing classes. The owner's config has FrameBlend=0,
  so interpolated presents are not a factor in their reports.

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
- Analysis traps that produced two wrong P4/N5 fix attempts (both reverted):
  - **No west VRAM headroom.** The BG1 rolling window is 64 columns = 32
    native + 32 EAST-ahead. Reading "live VRAM" for a west-gutter column
    (buffer col `btx0 + dx`, `dx < 0`) wraps into the east-ahead staged
    strip and paints a MIRROR of the right side into the left margin. The
    west margin can only be sourced from the world-keyed store / MAP16,
    never from live VRAM. An offline audit that "found cave art in the west
    VRAM" was this wrap, not real data.
  - **MAP16-empty ≠ missing data.** The whole-level MAP16 table lives in
    WRAM `$7F8000` (generated at level load); the GSU only converts the
    visible window to VRAM. A west cell decoding to the empty tile means
    the level is genuinely empty (outdoors) there — verify across frames
    where the cell was on-screen before concluding "streaming lag". The
    screen-id table (GSU `$0CAA`) being loaded does not imply non-empty
    content.
  - **Headless screenshot filename collisions.** At ~200 fps, multiple
    `screenshot` script commands in one run land in the same wall-clock
    second and overwrite one `present_<time>.ppm`. Capture one wide frame
    per run via `SNESRECOMP_SCREENSHOT` + `SNESRECOMP_SCREENSHOT_FRAME`
    with distinct names.
