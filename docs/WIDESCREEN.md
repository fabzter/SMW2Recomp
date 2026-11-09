# Widescreen support — scope, evidence, and limitations

This document records what the adaptive-widescreen implementation actually
does in `SuperMarioWorld2YoshisIslandSNESRecomp`, what was measured on real
hardware runs, and what it deliberately does not claim.

## What it is

`Widescreen = 1` in `config.ini` widens the simulated SNES field beyond the
native 256 columns. The left/right margins are *adaptive*: they are computed
each frame from the drawable's pixel aspect, then clamped so the frame width
stays at or under the PPU's 800-pixel cap (272 extra pixels per side, hard
limit). `SNESRECOMP_WS_EXTRA` may pin a fixed margin per side for diagnostics;
`SNESRECOMP_WS_BG_MARGINS_DEBUG=1` logs `bg1_active/extra/armed/bg3w` every
120 shadow calls.

Widescreen **off** is the faithful path: no GSU hook is armed, the
coordinate publication is disabled (and cleared on reset), and a fresh
`Widescreen=0` run is byte-identical to the retained gold captures
(see Qualification below).

## Measured margins (presented frame, OpenGL/cocoa)

| Window | Presented raster | Extra/side | Spawn-window tiles (rt) |
|--------|------------------|-----------:|------------------------:|
| 854×480 (16:9) | 342×224 | 43 px | 3 |
| 1920×540 (32:9) | 682×224 | 213 px | 14 |

Raster width is `g_snes_width` measured off the captured presented frame —
not scaled framebuffer/PNG geometry.

## Effects on game logic (the widening is real, not a cosmetic frame)

- **Sprite spawn window.** `gsu_check_new_sprites` admits records whose
  world-tile X lands in `[left−lt, left+22+rt)`, where `left` is the
  SRAM `$00A4` pixel seam >>4 and `lt/rt = ceil(margin_px/16)`. At 32:9 a
  stage-7 record at world-tile X 31 was flagged while the camera seam sat at
  tile 0 — spawn bound 22 — i.e. activation happened *earlier* inside the
  widened band. This is intended and disclosed.
- **Edge despawn** uses the widened window `relX ∈ [-(T+L), $F0+T+R)`; sprites
  in the margins stay alive, matching what the player sees.
- **OAM packing.** Parts outside the native X clip `[-15, 256)` are admitted
  and their low-mirror packed Y is parked at `$F0` (invisible); true signed
  coordinates are published through `PpuWsOamCoordinates` so the PPU draws
  them at their real position. Parts after the first `$8000` hole render at
  true coordinates (the packer scans all 256 source entries). On a real 32:9
  presented-frame capture (`posthole-trace`), a part that is unreachable in
  the native field (packed `y==$F0`) was verified to paint sprite-palette
  pixels at its true extended position (43 palette hits at screen x=525 with
  a 213px margin) — the widened field renders new content, not a wrapped
  ghost.
- **BG1** is widened via a ws_shadow margin layer fed from the level's own
  MAP16 stream — tile addresses in the margin read the same map path as the
  native area (VRAM-word audit: 12/12 hits, mode-7 offset modal 7/512).
- **BG3** widens only when the eligibility chain holds (mode `0F`, no
  message/intro/camera-event, BG mode 1, 16px tiles, `bgsc3=$3400`, TM bit 2);
  it arms/disarms on real scene boundaries and re-seeds on scroll.
- **BG2** is rendered by the live tilemap/HDMA path across the whole
  expanded output — it has no ws_shadow margin layer (it is never registered
  for shadow widening), but its margins are not blank: its tiles are drawn
  from the same live tilemap across the wider field, and its scroll/HSCROLL
  registers remain current between dumps (measured hscroll change).

## Sprite instance capacity

The regular sprite table keeps its native shape: 24 slots, of which slots
6–23 are allocatable (18 live regular sprites max). The widened producer
submits to the real consumer (CPU `03:9596` after the GSU `09:8000` queue);
when the pool is full, a failed allocation clears the candidate's stage flag
for retry — widened submissions cannot exceed the bound or hijack the
separate ambient pool. Measured on the trace build: with 17 disclosed-staged
slots leaving slot 23 free, the first widened record allocates slot 23 and its
`s_spr_id` equals the queued record's id (real producer→consumer→init, not
just flag traffic); once all 18 allocatable slots are live, further widened
records drain from the `0x27CE` queue with their flag cleared, the queue
terminator stays within `[$27CE,$28C8]`, and the ambient pool keeps its own
16-slot bound (no staged regulars leak).

## Ordinary save/load, cross-flag, and run-ahead

- `loadstate`/save-state slots work under widescreen; no stale OAM ghosts
  appear on the published table after a load. The check is discriminating,
  not self-passing: the receiver is walked 120 frames to `physX=8`, 243px
  from the saved `physX=251` (>>48px gate), so a skipped load cannot pass —
  the post-load `physX` (219 vs 251±64) lands on the save's trajectory only
  if the state was actually applied.
- A state saved under `Widescreen=1` cold-loads under `Widescreen=0` and vice
  versa (autosave path): both load into gameplay mode `0F` without a ghost,
  each side's receiver starting from that same discriminating position.
- `RunAhead = 1` speculates during non-turbo gameplay only (the host skips
  runahead while turbo is held); reversal and an ordinary load across the
  rollback boundary leave the published OAM table ghost-free.

## Diagnostic switches

| Switch | Effect |
|---|---|
| `SNESRECOMP_WS_EXTRA=<px>` | pin margin per side (release build included) |
| `SNESRECOMP_WS_BG_MARGINS_DEBUG=1` | log `bg1_active/extra/armed/bg3w` |
| `SNESRECOMP_SCREENSHOT=<file.ppm>` + `SNESRECOMP_SCREENSHOT_FRAME=<n>` | dump presented frame n |
| `SNESRECOMP_DUMP_DIR=<dir>` + `dump <tag>` script cmd | scene state dump (wram/sram/vram/oam/cgram/fb/regs/info) |
| `SNESRECOMP_DEBUG_PORT=<p>` (TRACE build) | TCP debug server: `step`, `read/write_sram`, `screenshot`, `get_ppu_state`, `ws_shadow_stats`, `get_superfx_state`, `loadstate/savestate` |

## Limitations — honest scope

- **No campaign certification.** Qualified scenes are the probe4 route
  (1-1 intro → message box → reversal → bonus cave → return pipe) plus the
  trace/capacity scenarios. Other scenes/effects are best-effort.
- **CPU rasterization retained.** The PPU stays the faithful CPU renderer;
  only the frame width and the margins change. There is no GPU raster path.
- **Execution tier measured, not assumed.** `interp_cycle_pct` ≈ 99.997%
  across parity/16:9/32:9 runs; the recomp manifest for this port carries no
  compiled AOT bodies, so the 65816 CPU hook PCs run under the interpreter
  where their pre-opcode callbacks are always seen. The GSU hooks are
  SuperFx PC hooks, unaffected by any CPU-tier question; the trace build's
  `get_superfx_state` shows the pack routine last_task `0xb1ef` executed.
  A compiled-coverage dispatch path does not exist in this build to check.
- **Widened spawning is a behavioral change by design.** Games that gate on
  sprite counts per screen can behave slightly differently; the allocator
  bound above is the containment.
- **West-of-map (worldX<0) BG1 margin is a bounded, intentional
  sign-scoped policy.** For worldX<0 margin fetches only, the ws_shadow
  layer serves an explicit outside tile (`WsShadowSetNegWorldTile`, the
  title arms it from MAP16 index 0 through the real ROM/tileset decoder
  with a revalidated fully-transparent fallback; default off and
  reset-cleared). Positive-worldX fetches keep the unchanged
  hit/miss/raw-fallback behavior, and the BG2/BG3 margins keep their
  authored wrap. The legacy renderer is not modified: the host forces
  the optimized renderer whenever widescreen is active (`NewRenderer=1`
  when `g_ws_active`), so the margin policy applies where the wider
  picture is actually drawn; on the legacy fallback the west margin
  keeps the SNES-authentic map wrap.
- **Qualified live-host paths** include a real drawable resize in the
  running process and a real rewind filmstrip round-trip (F12 → Left
  scrub → Return commit → real post-commit guest advancement, no
  re-open) driven by the injected test-only helper (own-process AppKit
  `setContentSize` + `dlsym(SDL_PushEvent)` real keyboard events); see
  the `live-host` scenario. The save-state browser (F11) itself was not
  exercised — the ordinary save/load path (`RtlSaveLoad`) it wraps was.
  A once-measured stale-`g_rewind_hotkey` re-open defect is fixed (see
  the task-5 report's fix1 section).

## Reproducing

```sh
python3 tests/run_smw2_widescreen_native.py --scenario all
# individual: binaries parity-off wide-16x9 ultra-32x9 runahead-16x9
#             crossflag resize-adapt trace-16x9 capacity-trace
#             posthole-trace live-host
```

Each scenario launches a private copy of the binary into an isolated temp
dir (own `config.ini`, `saves/`, `dumps/`). ROM location is read from the
user's existing `rom.cfg`; nothing is packaged or written outside `/tmp`.
