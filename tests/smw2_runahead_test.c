/* Per-simulated-frame Before coverage of the REAL run-ahead speculation
 * loop (snesrecomp/runner/src/snes_runahead.c, compiled verbatim).
 *
 * Contract: with a registered SnesRunaheadBeforeFrame, every RtlRunFrame
 * inside the speculation (frames 1..N) is preceded by exactly one Before,
 * in order; frame 0 keeps the outer-loop placement (the module never
 * calls the hook for it — no double-fire); the speculated frame's capture
 * lands after the last speculative run and before the rollback load;
 * rollback ordering is preserved; the audio producer rewinds; the frame
 * counter and disableRender restore; default-NULL keeps legacy order;
 * disabled/fallback paths don't double-advance and never invoke the hook.
 *
 * Neighbours are recording stubs — the runahead module is the unit under
 * test; RtlRunFrame, the snapshot walkers and the audio cursor live in
 * common_rtl.c and are already proven by the state_load suite. */

#include "snes_runahead.h"

/* Always forward-declared so the test compiles against the PRE-change
 * snes_runahead.h as well (identical function type). Under
 * -DSMW2_RA_NO_API a recording no-op satisfies the link: the preimage
 * module has no before-frame API, the hook can then never fire, and the
 * order assertions below fail behaviorally — the RED for this contract. */
void snes_runahead_set_before_frame(SnesRunaheadBeforeFrame fn,
                                    void *context);
#ifdef SMW2_RA_NO_API
void snes_runahead_set_before_frame(SnesRunaheadBeforeFrame fn,
                                    void *context) {
  (void)fn;
  (void)context;
}
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "snes/snes.h"

/* ── Machine the module touches ───────────────────────────────────────── */
static Snes t_snes;
Snes *g_snes = &t_snes;
int snes_frame_counter;
const RtlGameInfo *g_rtl_game_info;

/* ── Event log ────────────────────────────────────────────────────────── */
static char s_ev[256];
static unsigned s_ev_n;
static void note(char c) {
  assert(s_ev_n + 1 < sizeof s_ev);
  s_ev[s_ev_n++] = c;
  s_ev[s_ev_n] = 0;
}
static void reset_events(void) {
  s_ev_n = 0;
  s_ev[0] = 0;
}

/* ── Recording neighbours ─────────────────────────────────────────────── */
static uint32_t s_run_inputs[16];
static unsigned s_runs;
static int s_render_at_run[16]; /* g_snes->disableRender sampled per run */
static bool s_spec_at_run[16];  /* speculative flag sampled per run      */

static bool s_spec_flag;
static uint32_t s_bound_ret;
static size_t s_save_ret;
static bool s_load_ret = true;
static int s_exec_bound_ret = 1; /* >0: bound check passes */

static void before(void *ctx) {
  (void)ctx;
  note('B');
}
static void capture(void *ctx, int for_picture) {
  (void)ctx;
  note(for_picture ? 'C' : 'c');
}

bool RtlRunFrame(uint32_t inputs) {
  s_run_inputs[s_runs] = inputs;
  s_render_at_run[s_runs] = g_snes->disableRender;
  s_spec_at_run[s_runs] = s_spec_flag;
  s_runs++;
  note('R');
  ++snes_frame_counter; /* real RtlRunFrame owns the counter */
  return true;
}
void RtlSetSpeculativeFrame(bool on) {
  s_spec_flag = on;
  note(on ? 'T' : 'f');
}
bool RtlSpeculativeFrame(void) { return s_spec_flag; }
size_t RtlRollbackSnapshotBound(void) { return s_bound_ret; }
size_t RtlRollbackSaveToMemory(void *d, size_t cap) {
  (void)d;
  (void)cap;
  note('S');
  return s_save_ret;
}
bool RtlRollbackLoadFromMemory(const void *d, size_t n) {
  (void)d;
  (void)n;
  note('L');
  return s_load_ret;
}
uint32_t RtlAudioProducerCursor(void) { return 7; }
void RtlAudioRewindProducer(uint32_t c) {
  assert(c == 7);
  note('W');
}
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
size_t t_exec_bound(void) { return (size_t)s_exec_bound_ret; }
static const RtlGameInfo t_info = {.exec_state_bound = t_exec_bound};

static void setup(int frames, int with_before) {
  memset(&t_snes, 0, sizeof t_snes);
  s_runs = 0;
  s_save_ret = 64;
  s_bound_ret = 4096;
  s_load_ret = true;
  s_exec_bound_ret = 1;
  snes_frame_counter = 0;
  snes_runahead_set_capture(&capture, NULL);
  snes_runahead_set_before_frame(with_before ? &before : NULL, NULL);
  snes_runahead_set_frames(frames);
  g_rtl_game_info = &t_info;
  reset_events();
}

int main(void) {
  /* ── N=2 speculation, hook registered ──────────────────────────────────
   * The module owns R0 (real) + R1..R2 (speculative): the host's Before
   * fired before calling in, so expected sequence is R, c, T, S, then
   * B,R twice (hook immediately before EACH speculative run), C, W, f, L.
   * Wait: RtlSetSpeculativeFrame(true) runs BEFORE the loop. Actual order
   * per source: R0 → bound-check → c0 → S → save render/counter/cursor →
   * T (speculative on) → [B,R] ×2 → C → W → L → restore counter/render. */
  setup(2, 1);
  assert(snes_runahead_run_frame(0x1234) == 1);
  /* exact order: the speculative loop was added AFTER T */
  {
    const char *p = s_ev;
    /* R0, bound-probe pass, side-effect capture, save */
    assert(p[0] == 'R');
    p = strchr(p, 'c');
    assert(p && p[1] == 'S');
    /* speculative window: T then B,R B,R then C then W then L */
    p = strchr(p, 'T');
    assert(p && p[1] == 'B' && p[2] == 'R');
    assert(p[3] == 'B' && p[4] == 'R');
    assert(p[5] == 'C');
    assert(strchr(p, 'W') && strchr(p, 'L'));
    /* load happens after rewind (W before L) */
    assert(strchr(p, 'W') < strchr(p, 'L'));
  }
  assert(s_runs == 3);             /* real + 2 speculative */
  assert(s_render_at_run[0] == 0); /* real frame renders */
  assert(s_render_at_run[1] == 1); /* non-last speculative: no raster */
  assert(s_render_at_run[2] == 0); /* last speculative IS the picture */
  assert(!s_spec_at_run[0] && s_spec_at_run[1] && s_spec_at_run[2]);
  /* every speculative run carried the same inputs word */
  assert(s_run_inputs[0] == 0x1234 && s_run_inputs[2] == 0x1234);
  /* frame counter restored: guest ran 3 frames (counter hit 3) but the
   * module saves it before speculation and restores it after the rollback
   * load — the host's frame numbering must not advance N+1 per displayed
   * frame. */
  assert(snes_frame_counter == 1); /* restored to the post-R0 value */

  /* ── N=1: one B between S and R ─────────────────────────────────────── */
  setup(1, 1);
  assert(snes_runahead_run_frame(0) == 1);
  {
    const char *p = strchr(s_ev, 'T');
    assert(p && p[1] == 'B' && p[2] == 'R' && p[3] == 'C');
  }
  assert(s_runs == 2);

  /* ── N=4 bound: B count == speculative run count == 4 ───────────────── */
  setup(4, 1);
  assert(snes_runahead_run_frame(0) == 1);
  assert(s_runs == 5);
  {
    unsigned bs = 0;
    for (const char *p = s_ev; *p; p++)
      bs += (*p == 'B');
    assert(bs == 4);
  }

  /* ── default NULL: legacy schedule preserved (no B at all) ─────────── */
  setup(2, 0);
  assert(snes_runahead_run_frame(0) == 1);
  assert(s_runs == 3);
  assert(!strchr(s_ev, 'B'));

  /* ── frames=0: inactive → returns 0, caller runs the frame itself ──── */
  setup(0, 1);
  assert(snes_runahead_run_frame(0) == 0);
  assert(s_runs == 0 && s_ev[0] == 0);

  /* ── fallback: snapshot unavailable → real frame only, capture, no B ──
   * ensure_blob() fails → capture(1) + return 1 after R0 */
  setup(2, 1);
  s_bound_ret = 0; /* RtlRollbackSnapshotBound()==0 → no blob */
  assert(snes_runahead_run_frame(0) == 1);
  assert(s_runs == 1);
  assert(s_ev[0] == 'R' && strchr(s_ev, 'C') && !strchr(s_ev, 'B'));

  /* ── save failure mid-way → capture + return 1, no speculation, no B ── */
  setup(2, 1);
  s_save_ret = 0;
  assert(snes_runahead_run_frame(0) == 1);
  assert(s_runs == 1);
  assert(!strchr(s_ev, 'B'));

  /* ── unrewindable exec (bound==0 for 120 frames) → C1 early return ────
   * while probing (<120) the module captures for-picture and returns 1:
   * no speculation, and — crucially — no B (frame 0 stays the caller's). */
  setup(2, 1);
  s_exec_bound_ret = 0;
  assert(snes_runahead_run_frame(0) == 1);
  assert(s_runs == 1);
  assert(!strchr(s_ev, 'B'));
  assert(strchr(s_ev, 'C'));

  /* ── rollback load failure: speculation already ran (B's consumed),
   * module disables itself loudly rather than corrupting further ────── */
  setup(2, 1);
  s_load_ret = false;
  assert(snes_runahead_run_frame(0) == 1);
  assert(s_runs == 3);
  assert(snes_runahead_frames() == 0); /* self-disabled */
  /* … and the next call declines cleanly (host runs the frame itself) */
  assert(snes_runahead_run_frame(0) == 0);

  snes_runahead_shutdown();
  printf("smw2_runahead: PASS\n");
  return 0;
}
