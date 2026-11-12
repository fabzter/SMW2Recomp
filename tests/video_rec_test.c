#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video_rec.h"

#if defined(SNESRECOMP_VIDEO_CAPTURE) && SNESRECOMP_VIDEO_CAPTURE
#include "snes_osd.h"
#include "state_dump.h"

int snes_frame_counter;
int g_ws_extra;

static unsigned s_osd_calls;
static unsigned s_dump_calls;
static char s_last_toast[160];

void snes_osd_push(const char *message, int duration_ms) {
  (void)duration_ms;
  s_osd_calls++;
  snprintf(s_last_toast, sizeof(s_last_toast), "%s", message ? message : "");
}

int snes_state_dump(const char *dir, const char *tag, const uint8_t *pixels,
                    int pitch_bytes, int x0, int height, uint32_t frame) {
  (void)dir;
  (void)tag;
  (void)pixels;
  (void)pitch_bytes;
  (void)x0;
  (void)height;
  (void)frame;
  s_dump_calls++;
  return 0;
}
#endif

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #condition);    \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static unsigned failures;

int main(void) {
  VideoRecConfigure();
  CHECK(!VideoRecAvailable());
  CHECK(!VideoRecActive());

  VideoRecToggle();
  CHECK(!VideoRecActive());
  VideoRecPresent(NULL, 0, 0, 0, 0);
  VideoRecShutdown();
  CHECK(!VideoRecActive());

#if defined(SNESRECOMP_VIDEO_CAPTURE) && SNESRECOMP_VIDEO_CAPTURE
  CHECK(s_osd_calls > 0);
  CHECK(strstr(s_last_toast, "ffmpeg") != NULL);
  CHECK(s_dump_calls == 0);
#endif

  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
