/* Pure widescreen geometry contract (src/smw2_ws_geometry.h).
 *
 * Every CHECK is fatal on failure (never assert()-gated, so Release keeps
 * the gate). The suite is the behavioral RED against the phase-1 formula:
 * at 854x480 CRT 4:3 the old 224*aspect*(8/7) math produced 454/99, while
 * the pixel-aspect-aware helper must produce 342/43.
 *
 * Cases: every display aspect (4:3 / 8:7 / 1:1), 16:9 / 21:9 / 32:9
 * drawables, invalid dimensions, widescreen off, the 800px capacity
 * ceiling, the 256px native minimum, and explicit 0 / 272 env-pin
 * overrides. */

#include "src/smw2_ws_geometry.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned cases;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++cases;                                                                   \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

enum {
  ASP_43 = kSnesDisplayAspect_Crt4x3,
  ASP_87 = kSnesDisplayAspect_SquarePixels8x7,
  ASP_11 = kSnesDisplayAspect_SquareFrame1x1,
};

static Smw2WsGeometry geo(int dw, int dh, int aspect) {
  return Smw2WsComputeGeometry(dw, dh, aspect, 1, -1);
}

static void expect(int dw, int dh, int aspect, int width, int extra) {
  Smw2WsGeometry g = geo(dw, dh, aspect);
  if (g.width != width || g.extra != extra) {
    fprintf(stderr, "%s:%d: %dx%d aspect=%d: got %d/%d want %d/%d\n", __FILE__,
            __LINE__, dw, dh, aspect, g.width, g.extra, width, extra);
    exit(1);
  }
  cases += 2;
  CHECK(g.width == 256 + 2 * g.extra);
  CHECK(g.left_extra == g.extra && g.right_extra == g.extra);
  CHECK(!(g.width & 1));
  CHECK(g.extra >= 0 && g.extra <= 272);
}

int main(void) {
  /* The binding RED case: drawable 854x480 (aspect ~1.7792), CRT 4:3.
   * pixels_per_aspect = 224*6/7 = 192; extra = (1.7792*192-256)/2 +0.5
   * = (341.6-256)/2 +0.5 = 43 -> width 342.  The phase-1 formula gave
   * 454/99. */
  expect(854, 480, ASP_43, 342, 43);

  /* 16:9-class drawables. */
  expect(1280, 720, ASP_43, 342, 43); /* 16:9 = 341.33 -> 342/43 */
  expect(1920, 1080, ASP_43, 342, 43);
  expect(2560, 1440, ASP_43, 342, 43);
  /* square-pixel PAR 1: pixels_per_aspect = 224 -> 16:9 -> 398/71 */
  expect(1280, 720, ASP_87, 398, 71);
  /* 1:1 frame PAR 7/8: pixels_per_aspect = 256 -> 455 -> even 456/100 */
  expect(1280, 720, ASP_11, 456, 100);

  /* 21:9-class drawables (aspect ~2.333). */
  expect(2560, 1080, ASP_43, 456, 100); /* 2.3704*192=455.1 -> 456/100 */
  expect(3440, 1440, ASP_43, 458, 101); /* 2.3889*192=458.7 -> 458/101 */

  /* 32:9-class and beyond: capacity ceiling 800/272. */
  expect(3840, 1080, ASP_43, 682, 213); /* 3.5556*192=682.7 -> 682/213 */
  expect(5120, 1440, ASP_43, 682, 213); /* 32:9 same aspect */
  expect(7680, 2160, ASP_43, 682, 213); /* 32:9 at 2x */
  expect(7680, 1440, ASP_43, 800, 272); /* 5.333 -> clamped to 800 */
  expect(8192, 1152, ASP_43, 800, 272); /* 7.11 -> clamped */

  /* 8:7 / 1:1 at the extremes hit capacity earlier. */
  expect(5120, 1440, ASP_87, 796, 270); /* 32:9: 3.5556*224=796.4 -> 796/270 */
  expect(7680, 1440, ASP_87, 800, 272); /* 5.333*224=1195 -> clamped */
  expect(5120, 1440, ASP_11, 800, 272); /* 3.5556*256=910 -> clamped */

  /* Narrow drawables: native minimum 256. */
  expect(640, 480, ASP_43, 256, 0); /* 1.333*192=256.0 -> exactly native */
  expect(512, 480, ASP_43, 256, 0); /* narrower than native: clamp up */
  expect(256, 224, ASP_43, 256, 0);
  expect(480, 854, ASP_43, 256, 0); /* portrait */
  expect(100, 1000, ASP_43, 256, 0);

  /* Just over native: 4:3 drawable at 8:7 PAR gives a little margin. */
  expect(640, 480, ASP_87, 298, 21); /* 1.333*224=298.7 -> 298/21 */
  expect(640, 480, ASP_11, 342, 43); /* 1.333*256=341.3 -> 342/43 */

  /* Invalid dimensions stay native. */
  {
    Smw2WsGeometry g = geo(0, 480, ASP_43);
    CHECK(g.width == 256 && g.extra == 0);
    g = geo(854, 0, ASP_43);
    CHECK(g.width == 256 && g.extra == 0);
    g = geo(-100, 480, ASP_43);
    CHECK(g.width == 256 && g.extra == 0);
    g = geo(854, -480, ASP_43);
    CHECK(g.width == 256 && g.extra == 0);
  }

  /* Widescreen off is the faithful path at any drawable/override. */
  {
    Smw2WsGeometry g = Smw2WsComputeGeometry(3840, 1080, ASP_43, 0, -1);
    CHECK(g.width == 256 && g.extra == 0 && g.left_extra == 0 &&
          g.right_extra == 0);
    g = Smw2WsComputeGeometry(3840, 1080, ASP_43, 0, 272);
    CHECK(g.width == 256 && g.extra == 0);
  }

  /* Out-of-range aspect config values clamp to CRT 4:3. */
  {
    Smw2WsGeometry a = geo(854, 480, ASP_43);
    Smw2WsGeometry b = Smw2WsComputeGeometry(854, 480, 99, 1, -1);
    CHECK(b.width == a.width && b.extra == a.extra);
    b = Smw2WsComputeGeometry(854, 480, -3, 1, -1);
    CHECK(b.width == a.width && b.extra == a.extra);
  }

  /* Explicit margin pins (SNESRECOMP_WS_EXTRA): override replaces the
   * drawable-derived extra, inside the 272 capacity. */
  {
    Smw2WsGeometry g = Smw2WsComputeGeometry(854, 480, ASP_43, 1, 0);
    CHECK(g.width == 256 && g.extra == 0 && g.left_extra == 0 &&
          g.right_extra == 0);
    g = Smw2WsComputeGeometry(640, 480, ASP_43, 1, 272);
    CHECK(g.width == 800 && g.extra == 272);
    g = Smw2WsComputeGeometry(854, 480, ASP_43, 1, 43);
    CHECK(g.width == 342 && g.extra == 43);
    /* Above capacity clamps rather than overrunning the buffers. */
    g = Smw2WsComputeGeometry(854, 480, ASP_43, 1, 500);
    CHECK(g.width == 800 && g.extra == 272);
    /* Negative still means "follow the drawable". */
    g = Smw2WsComputeGeometry(854, 480, ASP_43, 1, -5);
    CHECK(g.width == 342 && g.extra == 43);
  }

  /* Live resize: successive calls track fresh dims, no state carried. */
  {
    Smw2WsGeometry a = geo(854, 480, ASP_43);
    Smw2WsGeometry b = geo(640, 480, ASP_43);
    Smw2WsGeometry c = geo(854, 480, ASP_43);
    CHECK(a.width == 342 && a.extra == 43);
    CHECK(b.width == 256 && b.extra == 0);
    CHECK(c.width == a.width && c.extra == a.extra);
  }

  printf("smw2_ws_geometry: PASS %u checks\n", cases);
  return 0;
}
