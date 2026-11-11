/* SMW2 widescreen telemetry — observer-only per-frame state log.
 * See smw2_telemetry.h. No guest or policy state is modified. */
#include "smw2_telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common_cpu_infra.h" /* g_ram */
#include "common_rtl.h"       /* g_ppu */
#include "config.h"           /* g_config */
#include "host_paths.h"
#include "ppu.h"
#include "smw2_rtl.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/ws_shadow.h"

extern bool g_ws_active;
extern int g_ws_extra;

static FILE *s_csv;
static int s_csv_failed;

static unsigned long long WallMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (unsigned long long)ts.tv_sec * 1000ull +
         (unsigned long long)(ts.tv_nsec / 1000000);
}

static unsigned CountBits(const uint8_t *bytes, unsigned n) {
  unsigned c = 0;
  for (unsigned i = 0; i < n; i++)
    for (unsigned b = 0; b < 8; b++)
      c += (bytes[i] >> b) & 1u;
  return c;
}

typedef struct {
  unsigned long long wall_ms;
  unsigned frame;
  unsigned mode, stage, load_type, exit_rec;
  int cam_x, cam_y, hscroll, vscroll;
  int extra, left_cur, right_cur;
  int armed, bg1_active, bg3w;
  WsShadowMarginStat ms;
  unsigned published, grace, mode7;
} TelemetryRow;

static void ReadRow(TelemetryRow *r) {
  memset(r, 0, sizeof(*r));
  r->wall_ms = WallMs();
  r->frame = (unsigned)snes_frame_counter;
  r->mode = g_ram[0x0118];
  r->stage = (unsigned)(g_ram[0x021a] | (g_ram[0x021b] << 8));
  r->load_type = g_ram[0x038c];
  r->exit_rec = (unsigned)(g_ram[0x038e] | (g_ram[0x038f] << 8));
  r->cam_x = (int)(int16_t)(g_ram[0x39] | (g_ram[0x3a] << 8));
  r->cam_y = (int)(int16_t)(g_ram[0x3b] | (g_ram[0x3c] << 8));
  r->extra = g_ws_extra;
  if (g_ppu) {
    r->hscroll = g_ppu->hScroll[0];
    r->vscroll = g_ppu->vScroll[0];
    r->left_cur = g_ppu->extraLeftCur;
    r->right_cur = g_ppu->extraRightCur;
    r->bg3w = g_ppu->wsBg3WidenY;
    r->published = CountBits(g_ppu->wsOamCoordinates.valid, 16);
    for (unsigned i = 0; i < 128; i++)
      if (g_ppu->wsOamMotionGrace[i])
        r->grace++;
  }
  r->armed = Smw2WsBg1Holding() ? 2 : Smw2WsBg1Armed();
  r->bg1_active = WsShadowLayerActive(0) ? 1 : 0;
  WsShadowGetMarginStats(0, &r->ms);
  r->mode7 = g_config.mode7.enabled ? 1 : 0;
}

#define ROW_FMT                                                                \
  "%llu,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"                         \
  "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"               \
  "%u,%u,%u\n"
#define ROW_ARGS(r)                                                            \
  (r)->wall_ms, (r)->frame, (r)->mode, (r)->stage, (r)->load_type,             \
      (r)->exit_rec, (r)->cam_x, (r)->cam_y, (r)->hscroll, (r)->vscroll,       \
      (r)->extra, (r)->left_cur, (r)->right_cur, (r)->armed, (r)->bg1_active,  \
      (r)->bg3w, (r)->ms.westHit, (r)->ms.westMiss, (r)->ms.eastHit,           \
      (r)->ms.eastMiss, (r)->ms.prefillSeed, (r)->ms.prefillRefresh,           \
      (r)->ms.westFold, (r)->ms.eastFold, (r)->ms.westBlank,                   \
      (r)->ms.eastBlank, (r)->ms.westRawFallback, (r)->ms.eastRawFallback,     \
      (r)->published, (r)->grace, (r)->mode7

void Smw2TelemetryFrame(unsigned frame) {
  (void)frame;
  if (s_csv_failed)
    return;
  if (!s_csv) {
    char path[1024];
    if (!snesrecomp_exe_dir_path("ws_telemetry.csv", path, sizeof(path)))
      snprintf(path, sizeof(path), "ws_telemetry.csv");
    s_csv = fopen(path, "a");
    if (!s_csv) {
      s_csv_failed = 1;
      return;
    }
    long s_pos = ftell(s_csv);
    fprintf(s_csv, "# session wall_ms=%llu build=%s\n", WallMs(),
            SNES_GAME_VERSION);
    if (s_pos == 0)
      fprintf(s_csv,
            "wall_ms,frame,mode,stage,load_type,exit_rec,cam_x,cam_y,"
            "hscroll,vscroll,extra,left_cur,right_cur,armed,bg1_active,bg3w,"
            "westHit,westMiss,eastHit,eastMiss,prefillSeed,prefillRefresh,"
            "westFold,eastFold,westBlank,eastBlank,westRawFallback,"
            "eastRawFallback,published,grace,mode7\n");
  }
  TelemetryRow r;
  ReadRow(&r);
  fprintf(s_csv, ROW_FMT, ROW_ARGS(&r));
  if ((r.frame & 31u) == 0)
    fflush(s_csv);
}

void Smw2TelemetrySidecar(const char *path) {
  char jp[1200];
  snprintf(jp, sizeof(jp), "%s.json", path);
  FILE *f = fopen(jp, "w");
  if (!f)
    return;
  TelemetryRow r;
  ReadRow(&r);
  fprintf(
      f,
      "{\n"
      "  \"wall_ms\": %llu, \"frame\": %u,\n"
      "  \"mode\": %u, \"stage\": %u, \"load_type\": %u, \"exit_rec\": %u,\n"
      "  \"cam\": [%d, %d], \"scroll\": [%d, %d],\n"
      "  \"extra\": %d, \"left_cur\": %d, \"right_cur\": %d,\n"
      "  \"armed\": %d, \"bg1_active\": %d, \"bg3w\": %d,\n"
      "  \"westHit\": %llu, \"westMiss\": %llu, \"eastHit\": %llu, "
      "\"eastMiss\": %llu,\n"
      "  \"westFold\": %llu, \"eastFold\": %llu, \"westBlank\": %llu, "
      "\"eastBlank\": %llu,\n"
      "  \"westRawFallback\": %llu, \"eastRawFallback\": %llu,\n"
      "  \"published\": %u, \"grace\": %u, \"mode7\": %u\n}\n",
      r.wall_ms, r.frame, r.mode, r.stage, r.load_type, r.exit_rec, r.cam_x,
      r.cam_y, r.hscroll, r.vscroll, r.extra, r.left_cur, r.right_cur, r.armed,
      r.bg1_active, r.bg3w, r.ms.westHit, r.ms.westMiss, r.ms.eastHit,
      r.ms.eastMiss, r.ms.westFold, r.ms.eastFold, r.ms.westBlank,
      r.ms.eastBlank, r.ms.westRawFallback, r.ms.eastRawFallback, r.published,
      r.grace, r.mode7);
  fclose(f);
  if (s_csv)
    fflush(s_csv);
}
