#ifndef SMW2_SPRITES_H_
#define SMW2_SPRITES_H_

/* Per-frame sprite lifecycle policy for Yoshi's Island widescreen margins.
 *
 * Arms title hooks on the live GSU core and the composed 65816 pre-opcode
 * chain so the game's own sprite systems serve the widened side margins:
 *
 *   - the GSU OAM pack (bank08 copy_oam_buffer, $08B1EF) admits parts whose
 *     X lands inside the widened rectangle and parks packed Y=$F0 for parts
 *     only visible outside the native clip, while recording per-part
 *     metadata for the signed-coordinate publication below;
 *   - the GSU new-sprite scan (bank09 gsu_check_new_sprites, $098055)
 *     replaces the native single-new-column/row trigger with the full
 *     widened window while the 31-entry queue bound is enforced in host
 *     integer space;
 *   - the GSU edge despawn (bank09 $098A18) widens the class X-threshold
 *     math without touching class-0 never-despawn or the Y path;
 *   - the two CPU manual despawn carry decisions (bank03 $03A2E8/$03A302)
 *     are re-derived against the live margins, carry bit only.
 *
 * At BeginFrame the module predicts the whole pack (forward/reverse
 * routing, crossing termination, gap clearing, high-table compression),
 * compares it slot-by-slot against the actual post-pack cart mirrors and
 * the live PPU OAM, and publishes the matching non-reserved slots as a
 * PpuWsOamCoordinates table (Task1) so widened-only positions render at
 * their true signed coordinates instead of their parked or wrapped
 * aliases. Slots 0-15 are reserved/CPU-owned and are never published.
 *
 * Widescreen off (both margins zero) is the faithful path: no GSU hook is
 * armed and no expanded decision runs. The persistent composed CPU
 * callbacks stay registered but inert. Smw2SpritesReset drops pending
 * ownership and the published table; Smw2SpritesShutdown additionally
 * disarms this module's GSU hooks and forgets the cart/core before
 * teardown. Neither touches other hook owners' registrations. */

/* Frame boundary, before guest simulation: verify the live cart/core and
 * the supported gameplay/OBJ-layout/ROM-shape/CPU-tier gates, and enable
 * mutation while at least one side margin is nonzero. left_extra and
 * right_extra are the freshly derived per-side pixel margins. */
void Smw2SpritesBeforeFrame(int left_extra, int right_extra);
/* After guest simulation: if this frame produced a newly completed pack
 * capture, consume it once and publish the validated coordinate table.
 * No fresh capture leaves the PPU's last validated/restored publication
 * untouched. */
void Smw2SpritesBeginFrame(void);
/* Scene/timeline reset: clear pending capture ownership and the published
 * coordinate table; other hook owners' registrations are preserved. */
void Smw2SpritesReset(void);
/* Module teardown: disarm only this module's GSU hooks on the live core
 * and forget the cart/core reference. */
void Smw2SpritesShutdown(void);

#endif /* SMW2_SPRITES_H_ */
