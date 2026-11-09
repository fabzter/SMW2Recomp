# Adaptive widescreen completion design

## Owner decision and scope

The owner requested completion of HUD/BG3, BG2 parallax, sprite/GSU margins, and scene boundaries. In the interactive `ultrawide-sprite-scope` approval, the owner selected **Yes — finish full adaptive widescreen**: widen spawn/despawn ranges only while widescreen is enabled; original behavior remains when disabled. A silent 80-pixel cap is not authorized.

Keep the existing OpenGL release presenter. Do not introduce SDL software fallback into release configuration. SNES PPU rasterization remains CPU-side; this is not a new GPU PPU implementation. No dependency installation, release, push, tag, or version change is part of this work.

## Existing verified work

BG1 MAP16 reconstruction, corrected SRAM mirror offset $0CAA, GSU ROMB $4C mapping, PPU-phase camera lift, and filling the column beyond the 32-column live capture are retained. Tests and the existing dirty workspace must be preserved. HEAD is `6ce3e93`; no clean-tree reset or worktree move may discard existing untracked source/tests.

Before new work, build/CTest, packaging, PPU extra-object, detached rendering, and SuperFX opt-in tests passed. Existing dumps labelled map/level contain stage $0B, the intro/tutorial, not proof of normal 1-1 gameplay. Final verification must reach an ordinary stage and exercise an actual transition.

## Invariants

- Widescreen disabled is the faithful path: no extended sprite positions, spawn/despawn changes, BG3 widening, presentation replay, or guest-state differences.
- Widescreen enabled changes only the authorized activation/despawn decisions and presentation hooks. Once an activation differs, downstream sprite AI, occupancy, RNG, and later guest state may legitimately diverge, as implied by the owner's approval of earlier enemy activation. Do not modify unrelated gameplay routines.
- Unknown scenes retain native UI and safely bounded rendering. No stretched text, edge smearing, invented sprites, or resurrected state-zero entities.
- Authored periodic background wrap is distinct from accidental rolling-buffer terrain wrap. Do not add a frame-latched shadow cache to fully authored BG2/BG3 maps.
- Per-scanline scroll, windows, priority, color math, and brightness remain in the ordinary PPU paths.
- All host metadata is invalidated on scene or timeline discontinuities, and regenerated after load. Do not serialize host pointers into guest snapshots.
- Every new behavior gets a red-first regression, its full module is rerun, and native OpenGL exercises the integrated result.

## Architecture

### 1. Geometry and frame lifecycle

`src/main.c::Smw2PrepareFrame` will use `SnesDisplayAspect_ComputeAdaptiveFrame` rather than the current hard-coded `8/7` multiplication. The configured display pixel aspect is independent of the drawable aspect. Even width, native minimum 256, height 224, and the existing `kPpuExtraLeftRight` capacity remain. Honor the existing deterministic `PpuWsExtraOverride()` when set.
Use `g_config.display_aspect` as already parsed from configuration; do not add an unrelated launcher row as part of this fix.

The lifecycle stays in the existing desktop host hooks. `before_run_frame` installs/enables the title's sprite hooks for the selected width. `begin_sim_frame` publishes background policy and captured sprite metadata after OAM staging. `on_reset` clears both modules; `RtlStateGeneration()` additionally detects timeline jumps. `on_rom_loaded` invalidates verification and old ownership before SnesInit; the first pre-frame hook verifies/arms the new `Cart::superfx`. `on_shutdown` disarms only title-owned GSU hooks before teardown. Never clear other owners' interpreter hook registrations; persistent callbacks are inert when disabled.

### 2. BG1 lifetime and scene boundaries

`src/smw2_rtl.c` continues to own BG1 and the background policy. Track an exact scene tuple, not a lossy packed hash: game mode ($0118), world ($0218), stage ($021A), load type ($038C), and screen exit ($038E). A tuple change, timeline generation change, cartridge replacement, or explicit reset drops retained world tiles and sprite metadata before reuse.

Call `WsShadowInvalidateTile` for each unresolved margin MAP16 cell; a previous value must not survive. On a known scene/timeline change, drop history and keep the transition frame centered/native rather than display unproven map wrap. Re-arm on the first subsequent complete 12/12 native-view proof (not on resolved margin cells), then use the existing tolerant proof in settled frames. A first cold observation may arm immediately on that strict proof. Ordinary streaming-table updates do not trigger re-arming.

Do not gate on raw INIDISP: YI normally leaves it at $8F at the pre-raster hook and uses scanline blanking at the top/bottom. Do not fingerprint the screen-ID table: normal streaming mutates it. Do not invent a bulk-VRAM threshold.

### 3. BG2 parallax

Keep BG2 in the normal per-line renderer, on the subscreen with native color math. Its authored 32x64-cell, 16x16-tile map supplies 512x1024 pixels. Do not register BG2 in `ws_shadow`; a frame-latched world origin would break raster scrolling without adding content.

Completion here means regression coverage and qualification: widened sampling follows that line's scroll through both map pages, 16x16 quadrants, wrap, windows, and subscreen composition. Verify off-native map currency against scrolling captures. Fix an actual demonstrated renderer defect if found, rather than introducing an unnecessary cache.

### 4. BG3 artwork and bounded UI

Use natural per-line BG3 rendering for proven level artwork. Candidate discriminator, verified in YI disassembly: game mode $0F, message-box state $0D0F == 0, stage-intro flag $0121 == 0, camera-event flag at GSU RAM $1E2A == 0, and the expected Mode-1 BG3-priority/large-tile map shape. Additional inactive flags may be used only with semantic evidence, not as guessed HUD gates.

Message cards, opening/closing cards, intros, menus, pause/status, and unsupported mode/map layouts retain the native composition. Do not impose a conventional top-HUD band: the observed BG3 is scrolling art and a dialogue carrier, not a fixed status strip. Reapply or clear BG3 policy each frame. Native HDMA scroll remains live; no frame-level scroll cache is added.

A feature-specific diagnostic disable switch may be added, but the completed validated feature must be enabled when widescreen is on; leaving it default-off is not completion.

### 5. Sprites and GSU

Add a focused `src/smw2_sprites.c/.h` module for signed emitted coordinates, hook lifecycle, and widescreen activation bounds.

YI emits 256 eight-byte staging entries at GSU RAM $0200..$09FF, with signed 16-bit X and $8000 unused sentinel. `copy_oam_buffer` starts at $08:B1EF (`bank08.asm:8788+`). It clears the first 16 OAM Y bytes; those slots remain native/CPU-owned. It scans entries sequentially, routing by word-4 bit $4000: forward from $0A40 (slot 16), backward from $0BFC (slot 127), stopping when cursors cross. The intervening gap gets Y=$F0; packed Y is emitted Y minus one. Preserve the 112-slot capacity. High flags are staged at $0C30/$0C9F and compressed into $0C00..$0C1F by the 32-iteration loop. CPU $6A00/$6C00 mirror physical GSU RAM $0A00/$0C00.

Observe the emitted table immediately before packing, with `superfx_ram_peek` for pending-store visibility. Reproduce its slot mapping exactly; excluded slots 0..15 are never overridden or included in pack validation. Compare predicted packed data with post-pack mirrors at `begin_sim_frame`, not with pre-pack output.

The native pack clips X to `[-15,256)` at $08:B219/$08:B221, and Y to `[-15,240)`. Widen only X while enabled: `[-leftExtra-(partWidth-1),256+rightExtra)`, using each entry's 8/16-pixel size. The upper bound is reloaded every loop; use per-comparison hooks, and restore the shared lower-bound register before the Y comparison. Keep the unused sentinel rejected. Normal YI OBJ size mode 0 is required; unsupported layouts use bounded/native presentation.

**Fail-closed output:** the packer's original ninth bit reflects X's sign, not the real bit 8 of positive X. Any accepted part outside the original native pack interval must therefore be parked at Y=$F0 in the packed mirror. Generation-valid host coordinates supply both original signed X and emitted-Y-minus-one for rendering; without metadata, normal 8/16-pixel parts remain hidden, never aliased onto the center. Do not rely on raw ninth-bit fallback for extended objects.

Add an opt-in **X+Y coordinate override** to the shared PPU sprite evaluator. Match each slot index and its actual two packed OAM words plus high-table pair at decode time, including raster OAM snapshots. The expected bytes must have been sampled post-pack and verified, not predicted only. Valid coordinates bypass left-hint/motion-grace admission, the per-pixel `ws_clip_left`, and HUD shifts; native reserved slots retain normal policy. Zero live margins or explicit disable gates the override off. Preserve tile fetch, ordering, transparency, windows, brightness, and color math. The single sprite evaluator feeds both pixel renderers.

Define metadata generation as an observed pack invocation: a monotonically increasing title sequence with a one-shot pending snapshot and verified post-pack output, not `RtlStateGeneration` or frame number alone. `begin_sim_frame` consumes pending metadata once. No fresh pack means do not republish a previously consumed capture over current PPU coordinates. Store the PPU's value-owned coordinate/signature state in `PpuRollbackResidue` so rollback restores the image metadata with the guest. Ordinary save-state loads/reset clear it and the title's pending capture; a subsequent pack regenerates it. Hooks also execute during speculative simulation; never gate them on live-frame presentation.
The raw PPU `SaveLoadInfo` visitor has no direction flag, so byte-identical serialization alone cannot distinguish saving from loading. Host integration must explicitly clear coordinates at the successful semantic snapshot-load boundary, after title execution-state restoration; serialization saves must not clear them. Rollback then restores its separately captured PPU residue. This is load wiring, not a relaxation of ordinary-load invalidation.

Widen activation while enabled, preserving never-despawn classes, native vertical limits, camera-bound warp/death, autoscroll, and non-target registers/flags. Inventory all four sources: camera-column/new-row spawn task $09:8000; GSU class despawn X math $09:8A18; CPU manual X decisions $03:A2E8/$03:A302; and pack X clipping above. Ambient entities share the despawn pass but are spawned by game events; do not invent new ambient events. Regular sprite activation must include all horizontal columns exposed by the wider view, not just shift a single leading-column test. Preserve native slot limits and bound the new-sprite queue ($27CE..$28C9) to avoid overwriting spawn flags at $28CA.

Verify hook-site instruction assumptions against the supported loaded ROM before arming. The GSU always executes through its own interpreter, so its hooks are independent of 65816 AOT. CPU pre-opcode hooks can be bypassed by compiled bodies: supply a supported counterpart or a concrete configuration/tier guard. Do not rewrite generated C. No private texture replay or state-zero sprite resurrection is used.

## Component contracts

- `smw2_rtl.c/.h`: per-frame background policy and an explicit background reset entry point.
- `smw2_sprites.c/.h`: pre-frame hook configuration, post-guest OAM metadata publication, explicit reset/disarm.
- `main.c`: geometry, invokes each module at its appropriate host lifecycle point; owns no sprite packing or map decoding.
- `ppu.h/.c` (and other decoder users found by consistency search): additive signed OAM X+Y coordinates, per-slot expected packed signatures, explicit inactive/clear behavior, reset safety, and value-owned rollback residue.
- Existing packaging exclusions and OpenGL defaults remain intact.

## Verification and acceptance

### Deterministic tests

1. Geometry across all display-pixel aspects, native/16:9/21:9/32:9, zero/invalid drawable, capacity bounds, and deterministic margin override.
2. BG1: all eight fine phases; failed-cell invalidation; scene change with identical sampled VRAM; reset/load generation; strict re-arm; no unintended RAM/SRAM/VRAM mutation by background policy.
3. BG3: artwork on; every nonzero message state off (include 1..$0D and $FF); stage intro/event/mode mismatch off; off-to-on and on-to-off in consecutive frames; no leaked policy when widescreen is disabled.
4. BG2/BG3 pixels: live per-line scroll changes, big-tile quadrants, both map dimensions and wrap, subscreen/windows/color math, native-width parity.
5. OAM: real pack-order parity, signed negative versus positive alias, unused sentinel, recycled slot, priority rotation, generation mismatch, reset and disable. Exercise X below -256 and above 511 where the widened buffer permits them.
6. Activation bounds: native unchanged when disabled; full adaptive bounds when on; class-specific and never-despawn behavior; CPU and GSU gates; left/right spawn coverage; vertical rules unchanged.
7. Complete existing title module, related PPU/SuperFX modules, sanitizers, packaging, and whitespace checks. Do not weaken existing assertions for a green result.

### Integrated native run

Use copied saves and explicit OpenGL configuration in an isolated temporary directory. Reach ordinary gameplay (stage not $0B), capture dialogue open/close, motion and reversal, a door/pipe or stage boundary, save/load or rewind, and a wider-than-native object-activation test. Compare matched fresh widescreen-off state and framebuffer to a baseline executable. Under widescreen-on, activation and its downstream state divergence are expected; compare unrelated behavior before that divergence, not whole-state hashes afterward. Cross-flag loads must be benign, but a state originally created with earlier activation is not a fresh native-equivalence oracle. Rewinding across a toggle uses the current activation rules. Record actual renderer, completed scenarios, images, and measured performance; frame counts alone are not gameplay coverage.

## Alternatives rejected

- Private GSU replay cannot keep entities alive after native despawn; repeated replay would either flicker or invent/resurrect absent entities.
- A silent 80-pixel cap contradicts the owner's selected full-adaptive scope.
- Raw 9-bit OAM hints cannot distinguish both signed interpretations at wide margins.
- Clamping BG3 permanently would leave the requested artwork work undone.
- BG2 world-history capture would add stale/per-frame phase errors to an already authored map.

## Review disposition

The owner approved the written design and execution without routine check-ins. Three independent design reviews were consumed. Required mechanical corrections incorporated: fail-closed parking plus X+Y coordinates, reserved slots and exact pack routing, post-pack slot-scoped signatures, rollback residue and one-shot publication, hint/edge/HUD precedence, explicit lifecycle ownership, queue capacity and all activation gates, nonzero UI-state gating, and off-only baseline scope.

Primary-source corrections to reviewers: the pack scans 256 entries (`iwt r12,#$0100`), not 32; the compressed high mirror really is $0C00..$0C1F (32 stores); OAM low base $0A00 is correct for the whole table, while GSU output begins at $0A40. References are `bank08.asm:8788-8943`. No new product decision was needed for these fixes.
