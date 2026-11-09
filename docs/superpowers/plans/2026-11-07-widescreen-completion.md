# Adaptive Widescreen Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Finish adaptive widescreen backgrounds/UI, sprites/activation, and scene lifecycle with verified OpenGL output.

**Architecture:** Retain exact BG1 MAP16 margins, natural per-line BG2/BG3 rendering, and the existing desktop host. Capture GSU sprite staging, widen authorized activation/pack bounds, park non-native packed entries, and feed validated signed X/Y coordinates to the PPU. Use timeline/scene invalidation plus pointer-free rollback state.

**Tech Stack:** C11/C++ desktop runner, SNES/GSU interpreters, SDL3 host, OpenGL presenter, CMake/CTest, Python regression runners.

**Spec:** `docs/superpowers/specs/2026-11-07-widescreen-completion-design.md`

## Global Constraints

- Final presenter is OpenGL; no SDL software release fallback.
- Full adaptive scope is owner-approved, including widescreen-gated earlier activation/later despawn. No silent width cap.
- Widescreen disabled remains the faithful path on a matched fresh initial state.
- Preserve existing dirty/untracked work on `local/macos`; do not reset, clean, switch worktrees, commit unrelated work, push, tag, publish, or change versions.
- Do not edit generated C or copyrighted ROM-derived source. Read the authorized `/tmp/yi-disasm` as evidence, but commit only original policy code and synthetic tests.
- Use real renderer/interpreter behavior in tests; do not weaken expectations to obtain green results.
- No private SuperFX texture replay or state-zero sprite resurrection.
- No BG2 shadow cache, fixed guessed HUD band, raw-INIDISP gate, screen-ID hash, or guessed VRAM-write threshold.
- Metadata must match slot index and packed content; extended raw packed output must remain safe with metadata absent.
- Capture all simulated frames, including speculation; default mode has no new behavior.

## Interfaces and file ownership

Task 1 owns the PPU coordinate API and its rollback residue. Task 2 owns background policy and title background tests. Task 3 owns new sprite policy files/tests. Task 4 owns host wiring/geometry/CMake and integration renderer tests. Task 5 owns native qualification and user-facing documentation. Task 6 is the independent final review/fix gate.

### Shared PPU contract (Task 1 produces, Task 3 consumes)

```c
typedef struct PpuWsOamCoordinates {
  int16_t x[128], y[128];
  uint16_t expectedOam[256];
  uint8_t expectedHighOam[32];
  uint8_t valid[16];
} PpuWsOamCoordinates;
void PpuWsSetOamCoordinates(Ppu *ppu, const PpuWsOamCoordinates *coordinates);
```

Setter copies value-owned storage; NULL clears. Coordinates are signed X and already-packed Y (emitted Y minus one, modulo 256). The same slot's two actual OAM words and two high bits must match expected bytes every time it is decoded. Override is inert without live side margins. State lives outside ordinary snapshot spans and is included in `PpuRollbackResidue` get/set. Valid overrides bypass the left-admission/edge-clipping/HUD-adjust paths, not the normal tile/priority/window/color paths.

### Title contracts

```c
/* smw2_rtl.h: Task 2 */
void Smw2ConfigureWsBgMargins(void); /* retained entry */
void Smw2ResetWsBackgrounds(void);

/* smw2_sprites.h: Task 3 */
void Smw2SpritesBeforeFrame(int left_extra, int right_extra);
void Smw2SpritesBeginFrame(void);
void Smw2SpritesReset(void);
void Smw2SpritesShutdown(void);
```

Sprite reset invalidates pending publication and PPU coordinates. Shutdown disarms only owned GSU hooks and forgets a live core before teardown. A persistent composed CPU callback must be inert when disabled; do not clear all other hook owners. BeforeFrame verifies the current cart/core and enables only supported gameplay/layout/ROM/tier combinations. BeginFrame consumes a newly captured pack once; it must not overwrite restored PPU coordinates with already-consumed future metadata.

---

### Task 1: Safe PPU signed-coordinate override

**Files:** modify `snesrecomp/runner/src/snes/ppu.h`, `ppu.c`; create `snesrecomp/tests/ppu/ppu_oam_coordinates_test.c` and `run_oam_coordinates.py`.

**Consumes:** native OAM arrays, `PpuDecodeOamX`, sprite evaluator, reset/saveload and rollback residue.
**Produces:** shared API above with pixel-tested behavior.

- [ ] Write a ROM-free failing pixel regression for a parked 16x16 object whose actual signed position is outside hardware coordinates. Initial unavailable-feature path must fail an assertion/explicit FAIL, not be mistaken for a passing skipped test. Add the opt-in setup when implementing the API, retaining the original expected pixels.

```c
/* Required outcomes after binding the real coordinates:
 * x=300 -> pixel at 300, never at raw alias44;
 * x=520 -> pixel at520, never at8;
 * x=-270 -> pixel at-270, never its packed alias;
 * NULL/slot-signature mismatch -> parked object emits no pixels;
 * extra=0 -> ordinary native OAM decode only.
 */
```

- [ ] Run and record RED against the unmodified PPU.
- [ ] Add the value-owned struct/setter. Integrate X and Y decoding into both sprite-evaluation passes and all other decode users found by consistency search. A valid slot bypasses `PpuWidescreenOamLeftHintAllows`, per-pixel `ws_clip_left`, and HUD relocation; ordinary slots retain existing behavior.
- [ ] Copy the coordinate struct in `ppu_rb_residue_get/set`; clear on reset and data-changing raw visitor loads, without altering stable guest snapshot spans or mutating publication on saves. The raw visitor has no direction flag; Task4 supplies unconditional successful ordinary-load invalidation at the semantic loader boundary, including byte-identical loads. Do not clear restored state just because no new pack ran in a frame.
- [ ] Cover mismatched tile/attribute/Y/high pair, recycled slot, windows, color math, priority, both pixel renderers, full 272px capacity, reset/load, and rollback restoring coordinates from before a speculative frame. Residue-restored coordinates must persist across frames without an explicit setter/reset/load. Test live margins (`extraLeftCur/extraRightCur`), including centered256 with a nonzero allocation budget, plus ordinary native fallback after signature mismatch. Title-reserved slots and stale one-shot capture republishing are Task3/4 assertions, not Task1 dependencies.
- [ ] Run the full new module and existing suites:

```sh
python3 snesrecomp/tests/ppu/run_oam_coordinates.py
python3 snesrecomp/tests/ppu/test_extra_objects.py
python3 snesrecomp/tests/ppu/run_detached_line.py
```

Report RED/GREEN evidence, full test commands, touched files, snapshot/residue compatibility and concerns. Do not commit.

### Task 2: Background artwork/UI and scene lifetime

**Files:** modify `src/smw2_rtl.c`, `src/smw2_rtl.h`, `tests/smw2_ws_test.c`.

**Consumes:** current BG1 decoder, `RtlStateGeneration`, PPU background policy, exact YI scene/UI bytes.
**Produces:** background reset API and validated BG3 policy; BG2 remains natural.

- [ ] Add failing regressions through `Smw2ConfigureWsBgMargins`: scene change with identical sampled tilemap; timeline change; unloaded cell previously stored; strict re-arm; BG3 message/intro/event/mode gates. Preserve the existing eight fine-scroll and address assertions.
- [ ] Record RED on the current implementation.
- [ ] Track the exact tuple `{mode0118,world0218,stage021A,load038C,exit038E}` and a uint64 timeline generation. A known change resets WsShadow and yields one centered/native transition frame; the next full 12/12 native proof re-arms. Cold first observation may arm immediately on a full proof. Restore original margin geometry after the transient through normal next-frame setup.

```c
if (!smw2_bg1_tile(tx, ty, &entry)) {
  WsShadowInvalidateTile(kBg1, tx, ty);
} else {
  WsShadowForceTile(kBg1, tx, ty, entry);
}
```

- [ ] Widen BG3 naturally only for mode0F, message0D0F==0, intro0121==0, GSU event1E2A==0, expected Mode1/large-BG3/priority/TM/map shape. Treat every nonzero message value as active UI. Reapply zero policy every ineligible frame and when off; feature is enabled by default when wide and eligible.
- [ ] Leave BG2 unregistered in WsShadow. Do not force BG1 scene gates onto every parallax pixel.
- [ ] Provide `Smw2ResetWsBackgrounds`. Test RAM/SRAM/VRAM non-mutation for background operations, exact invalidation, screen-table updates without false epoch changes, transition re-arm while scrolling, and all old assertions.
- [ ] Run full title test and real intro-state audits, distinguishing audit decoding from live scene eligibility. If CTest links no real PPU, use the minimal host API scaffolding needed for policy fields and a test-owned `RtlStateGeneration()` clock alongside the existing host globals; test actual rendering later in Task4. Never mock the decoder behavior.

```sh
cmake --build build-macos --parallel 8
ctest --test-dir build-macos --output-on-failure
```

Report RED/GREEN and exact scene behavior. Do not edit main.c/CMake/other owners' files or commit.

### Task 3: Full-adaptive sprite activation and exact pack metadata

**Files:** create `src/smw2_sprites.c/.h`, `tests/smw2_sprite_test.c`, and a cross-platform runner `tests/run_smw2_sprites.py`.

**Consumes:** Task1 coordinate API, current Cart::superfx, composed CPU pre-opcode hooks, SuperFX PC hooks.
**Produces:** four title sprite lifecycle functions above.

- [ ] Write synthetic tests exercising the actual new policy against a minimal supported GSU ROM fixture and the real SuperFX core. Include a retained native reference instance. No game ROM bytes are checked into fixtures. A missing-feature behavioral RED is recorded before implementation.
- [ ] Factor bounded packing/limits into small deterministic helpers. Reproduce the actual 256-entry scan, slots16..127 forward/reverse, bit4000 routing, crossing termination, Y-1, gap clearing, and high pair packing. Exclude slots0..15. The 112-slot and 31-entry new-sprite queue limits remain.

```c
static bool visible_part(int x, int width, int left, int right) {
  return x >= -left - (width - 1) && x < 256 + right;
}
/* Native-off path never invokes expanded decisions.
 * No activation uses stale state-zero actor coordinates.
 */
```

- [ ] Observe pack at08B1EF with `superfx_ram_peek`; copy at most2048B. Widen X comparisons per iteration at08B219/B221; restore native -15 before Y test B230. Determine each part's8/16 width from its high flags. Widened-only parts get parked packedY=F0 in the forward/reverse write path, while their original coordinates remain in pending metadata. Verify the exact register/write hook points in disassembly and against the loaded ROM before arming; do not use guessed addresses. No post-simulation authoritative RAM pokes.
- [ ] At BeginFrame, compare predicted pack with actual post-pack cart mirrors and current PPU OAM, restricted to nonreserved slots; publish per-slot actual expected signatures and signed X/Y. Consume pending once. No pending capture leaves the PPU's prior validated/restored coordinates alone. Scene/timeline reset clears all pending ownership; cart replacement/Shutdown disarms only this module's hooks.
- [ ] Expand ordinary sprite activation throughout the visible horizontal rectangle, including all columns already visible on enable/resize; do not merely offset the leading-column equality. The task09:8000 checks unspawned ROM entries; a hook at09:8055 may decide using r11/r12 worldtile coordinates, r4 left tile, r2 top tile and native20-row height, redirecting to806B or807C. Bound the queue before submission in widened C integer arithmetic: `(int)r10 + 8 + 2 <= 0x28CA`; do not allow uint16 wrap. Preserve the normal consumer, flags, instance limits and initialization; ambient events stay event-driven.
- [ ] Expand GSU class X threshold math at09:8A18 only after the never-despawn-class bypass. With original threshold T, use `T+leftExtra` and horizontal span constant `240+rightExtra-leftExtra`; preserve Y threshold processing and the earlier autoscroll/edge-warp/death code.
- [ ] Cover both CPU manual X decisions03:A2E8 and03:A302. Only replace the carry decision of the X branch, using native ranges[-128,384) and[-64,320) expanded by the live sides; preserve A/X/Y/stack, all nontarget flags, Y checks and camera-event logic. Use composed callbacks and inert disabled state. Provide a concrete AOT/configuration guard instead of assuming callbacks see compiled instructions; GSU hooks are always available independently of 65816 tier.
- [ ] Reject unsupported/mutated hook instruction shapes and unsupported OBJ layouts. Never globally clear another callback owner's hooks.
- [ ] Tests: native-off exact state/clock/RAM parity; wide activation left/right/new row; 31-entry queue boundary; native instance capacity; never-despawn/ambient classes; vertical unchanged; pack X=-300,300,520 and sentinel8000; forward/reverse parity and112 crossing; no-metadata parked fallback; per-slot signature mismatch; rollback one-shot publication; disable/load/core replacement; instruction mutation and AOT guard.

```sh
python3 tests/run_smw2_sprites.py
```

Run normal plus AddressSanitizer/UndefinedBehaviorSanitizer variants of the full module. Return hook inventory, red-first proof, state parity evidence, and unresolved concerns. Do not edit main.c/CMake or commit.

### Task 4: Host wiring, geometry, and full background pixel gates

**Files:** modify `src/main.c`, `CMakeLists.txt`, and the successful semantic-load boundary in `snesrecomp/runner/src/common_rtl.c`; only if necessary, add a narrow drawable accessor in desktop host public API/implementation or extend `scripts/macos/package-app.sh` and its existing packaging regression for new test binaries. Create `src/smw2_ws_geometry.h`, `tests/smw2_ws_geometry_test.c`, `tests/smw2_bg_render_test.c`, test runner(s) needed to exercise the real PPU.

**Consumes:** Tasks1-3 interfaces. **Produces:** linked application and CTest coverage.

- [ ] Write geometry tests RED against the old formula: CRT4:3 at854x480 must produce width342/extra43, not454/99. Cover every display aspect, 16:9/21:9/32:9, invalid dimensions, max800width, native minimum256 and explicit0/272 overrides.
- [ ] Extract only the pure geometry calculation, then call the existing helper:

```c
SnesDisplayFrame frame = SnesDisplayAspect_ComputeAdaptiveFrame(
    256, 224, 256 + 2 * kPpuExtraLeftRight,
    (double)drawable_w / drawable_h,
    SnesDisplayAspect_Clamp(g_config.display_aspect));
/* PpuWsExtraOverride >=0 replaces extra within capacity; feature-off stays256. */
```

- [ ] Wire BeforeFrame/BeginFrame/reset/ROM-loaded/Shutdown as specified. `before_run_frame` precedes `PreparePpuFrame` in every normal/replay loop, so `g_ws_extra/g_ws_active` are stale there and must not be used as desired geometry. Obtain fresh drawable dimensions through the supported desktop/SDL interface (a narrow host accessor if necessary), then run the same extracted geometry helper honoring current `g_config.widescreen`; use that result for BeforeFrame. Do not use file-static `g_last_drawable_*` or guess a window ID. Test first-frame enable and live resize. Main owns orchestration only.
- [ ] At every successful semantic ordinary snapshot-load boundary, explicitly clear PPU coordinates with the existing setter after title execution-state restoration, including byte-identical loads. `RtlLoadSnapshotFromMemory` currently invokes `on_state_loaded` before advancing generation; clear after that callback. Do not infer direction solely from changed serialized bytes or a lookahead callback (preflight also has lookahead). Rollback must then restore its PPU residue; serialization saves must keep current coordinates. Cover this integration ordering and actual load behavior.
- [ ] Add source/tests to CMake with BUILD_TESTING guards. Keep all verification executables/generated test artifacts out of application packaging; preserve the prior exclusions and extend only for the new named test binaries.
- [ ] Create renderer tests using real PPU memory and deterministic tile patterns: BG2 mode1 large16 cells on a32x64 map, BG3 on a32x32 map, per-line hscroll/vscroll change, both page transitions, quadrants/wrap, main/subscreen windows/brightness/color math, BG3 UI policy and wide-off parity. No frame-latched shadow for these layers. If a renderer defect appears, write its failing pixel case first and fix minimally in a separately reviewed patch.
- [ ] Run full CTest, all new test runners, existing PPU coordinate/extra-object/detached tests and packaging; include sanitizer builds for title and sprite tests. Check every changed API/callsite and duplicate semantics with grep. Build the full binary.

### Task 5: Native OpenGL qualification and documentation

**Files:** create focused reproducible qualification runner/script(s) under `tests/`; update `scripts/macos/MACOS.md` and a concise `docs/WIDESCREEN.md` with measured scope/evidence.

**Consumes:** integrated application; preserved baseline `/tmp/smw2-native-baseline.Q6YL2k/SuperMarioWorld2YoshisIslandSNESRecomp` (SHA256 fc18e24a2292796728fe6785ea60e335fe075425e833f553969fd2abc9292ff4).

- [ ] Use isolated temporary dirs and copied saves. Existing ROM location is in `/tmp/smw2-ws/opengl-run2/rom.cfg`; never package ROM or mutate normal saves/configs. Native config uses OpenGL and no dummy video env.
- [ ] Establish a deterministic normal-stage fixture using the game's own loader (stage0 is1-1; stage0B isintro), not fabricated pixel/OAM data. A private test setup may request a legitimate level load through guest state, but subsequent evidence must be the actual loader/gameplay/transition and the setup must be disclosed. Capture dialogue open/close and a door/pipe/stage load. The prior script's repeated A presses and intro-only state are not sufficient.
- [ ] Compare matched fresh native-off baseline/candidate: completed frames, framebuffer and guest WRAM/SRAM/VRAM/OAM equivalence. Use the same initial SRAM and input route, and verify the executed candidate binary hash matches the latest build. Do not compare post-activation wide-on whole-state hashes as if they should be identical.
- [ ] Qualify16:9 and a genuine ultrawide setting (21:9 or32:9) with left/right movement/reversal, earlier activation in the expanded region, parked-ghost absence, dialog/status policy, scene-boundary/history invalidation, load/rewind, and benign cross-flag load.
- [ ] Verify BG1 tile addresses with the existing dump audit; verify BG2 offnative currency using actual scrolling memory/captures. Inspect screenshots. Record renderer=opengl/video=cocoa, scenario identities, counts/timings and no crash. A clean frame count is not a substitute for reaching the required scenes.
- [ ] Document exact hot-path effects and limitations, diagnostic switches, native-faithful off mode, earlier enemy activation while wide, and any preserved hardware capacities. Do not claim campaign certification or GPU PPU rasterization.

### Task 6: Independent review and closure

- [ ] Generate a diff/review package that includes the original dirty/untracked title files and all new code, not just `git diff HEAD` tracked output. Review spec compliance and code quality independently of implementer self-review.
- [ ] Fix all critical/high correctness findings, rerun each full affected module, and run one scoped re-review of the fixes. No drive-by refactor or weakened assertions.
- [ ] Rebuild current candidate after the last production edit, run full CTest/packaging/sanitizers and any changed native scenario again. Run `git diff --check` and inspect scope/callsite consistency.
- [ ] Complete todos only with recorded evidence. Report actual implementation/scenarios and any remaining blockers honestly; do not silently relabel a partial renderer as full completion.

## Plan preflight

Task1->Task3 share only the declared PPU API; Task2 owns background files; Task4 alone owns main/CMake/package integration. Tasks1/3 both touch rollback behavior by contract, but only Task1 edits PpuRollbackResidue. Task2 strict re-arm does not gate BG2's per-line scroll. Task3 parking requires X+Y, not X-only. Task4 geometry uses the framework pixel-aspect helper; sprite bounds receive actual margins. Task5 compares native off only from matched fresh state. Existing feature branch and baseline tests are retained in place to avoid discarding the in-flight untracked BG1 implementation. No workspace move or commits are required.
