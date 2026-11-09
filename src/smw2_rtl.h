#ifndef SMW2_RTL_H_
#define SMW2_RTL_H_

/* Per-simulated-frame widescreen policy for Yoshi's Island.
 *
 * Modelled on MegaManX2Recomp's src/x2_rtl.c (the other snesrecomp title with
 * an exact, self-validated background source for the widescreen margins).
 * This pin's ws_shadow engine has no lazy world-provider callback, so the
 * title fills the gutter cells each frame with WsShadowForceTile from the
 * game's own MAP16 RAM/ROM tables — the same walk the GSU map16 routines
 * (gsu_map16_to_tmap_column, bank09 $09F9E8) do natively.
 *
 * Kill switch: SNESRECOMP_WS_BG_MARGINS=0 (env, read once). */
void Smw2ConfigureWsBgMargins(void);
/* Drop every retained piece of background state: the ws_shadow world store
 * and the scene/timeline tracking used for transition frames. The host's
 * reset/ROM-load lifecycle wires this (Task4); the next Configure call is a
 * cold first observation that can arm only on a complete native proof. The
 * BG3 widen policy is also reapplied to its inactive value. */
void Smw2ResetWsBackgrounds(void);

#endif /* SMW2_RTL_H_ */
