#ifndef SMW2_WS_GEOMETRY_H_
#define SMW2_WS_GEOMETRY_H_

/* Pure widescreen geometry for Yoshi's Island, shared by the two host
 * points that need the same answer on every simulated frame:
 *
 *   - prepare_frame (during PreparePpuFrame): decides this raster's frame
 *     width, which PreparePpuFrame then turns into live PPU margins via
 *     PpuSetExtraSpace;
 *   - before_run_frame (BEFORE PreparePpuFrame, where g_ws_extra is still
 *     last frame's value): derives the fresh per-side margins the sprite
 *     policy arms against, from the owned window's drawable PIXELS.
 *
 * The math itself is the framework's: SnesDisplayAspect_ComputeAdaptiveFrame
 * honours the configured pixel aspect (CRT 4:3 = 7/6 PAR, square-pixels 8:7
 * = 1, square-frame 1:1 = 7/8), clamps the target aspect to [native, max],
 * and returns a centered even width. It replaces the scaffold's hardcoded
 * 8/7 PAR formula, which at e.g. 854x480 CRT 4:3 produced a 454-wide field
 * (99 px/side) instead of the aspect-correct 342 (43 px/side).
 *
 * Inputs are plain values so the helper is pure and testable: callers pass
 * g_config.widescreen, g_config.display_aspect (unclamped int — Clamp is
 * applied inside) and PpuWsExtraOverride() (the SNESRECOMP_WS_EXTRA env
 * pin: -1 follows the drawable, 0..272 pins the margin). */

#include "desktop/display_aspect.h"
#include "snes/ppu.h"

typedef struct Smw2WsGeometry {
  int width;      /* frame width in source pixels: 256 + 2*extra */
  int extra;      /* symmetric per-side margin, 0..kPpuExtraLeftRight */
  int left_extra; /* margins handed to the sprite policy (symmetric) */
  int right_extra;
} Smw2WsGeometry;

static inline Smw2WsGeometry
Smw2WsComputeGeometry(int drawable_w, int drawable_h, int display_aspect,
                      int widescreen_on, int extra_override) {
  Smw2WsGeometry g = {256, 0, 0, 0};
  if (!widescreen_on || drawable_w <= 0 || drawable_h <= 0)
    return g; /* feature off or no drawable: faithful 256-wide field */
  if (extra_override >= 0) {
    /* Deterministic pin (SNESRECOMP_WS_EXTRA). PpuWsExtraOverride already
     * clamps to kPpuExtraLeftRight; clamp here too so the helper's contract
     * does not depend on the caller's source. */
    if (extra_override > kPpuExtraLeftRight)
      extra_override = kPpuExtraLeftRight;
    g.extra = extra_override;
  } else {
    const SnesDisplayFrame frame = SnesDisplayAspect_ComputeAdaptiveFrame(
        256, 224, 256 + 2 * kPpuExtraLeftRight,
        (double)drawable_w / (double)drawable_h,
        SnesDisplayAspect_Clamp(display_aspect));
    g.extra = frame.extra;
  }
  g.width = 256 + 2 * g.extra;
  g.left_extra = g.extra;
  g.right_extra = g.extra;
  return g;
}

#endif /* SMW2_WS_GEOMETRY_H_ */
