/* Fixture for the OAM-coordinates lifecycle gates: the production
 * RtlSaveSnapshotToMemory / RtlLoadSnapshotFromMemory /
 * RtlRollbackSaveToMemory / RtlRollbackLoadFromMemory path (spliced by
 * tests/run_smw2_task4.py the same way snapshot_guard/test_transaction.py
 * builds its translation unit), plus the REAL PpuWsSetOamCoordinates and
 * ppu_rb_residue_get/set so the coordinate table's load/rollback behavior
 * is the shipped code, not a mock. Everything else the spliced bodies
 * need is stubbed in the transaction_fixture.c style. */

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snapshot_identity.h"
#include "apu_frame_clock.h"
#include "snes/apu.h"
#include "snes/dma.h"
#include "snes/dsp.h"
#include "snes/joypad.h"
#include "snes/snes.h"
#include "snes/spc.h"
#include "snes/superfx.h"
#include "snes/interp_bridge.h"
#include "src/smw2_sprites.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <stddef.h>

static Snes machine;
static Cpu cpu;
static Apu apu;
static Dsp dsp;
static Spc spc;
static Dma dma;
static Ppu ppu;
static Cart cart;
/* WRAM: the real smw2_sprites.c gates on g_ram[0x0118] (gameplay mode),
 * so the machine's WRAM IS the global g_ram rather than a private buffer. */
uint8_t g_ram[0x20000];
static unsigned char cart_ram[0x10000]; /* full GSU RAM (OAM mirrors live here) */
static unsigned char cart_rom[0x100000];
static SuperFx t_fx;
Snes *g_snes = &machine;
CpuState g_cpu;
int snes_frame_counter;

/* counters the test asserts on */
static unsigned reconcile_calls;
static unsigned table_live_at_reconcile; /* wsOamCoordinates nonzero seen
                                          * inside on_state_loaded — proves
                                          * the clear is a POST-callback
                                          * semantic boundary */
static unsigned presentation_resets;
static unsigned locks;
static unsigned audio_resets;

static void rtl_reset_audio_delivery(void) { ++audio_resets; }
void snes_mod_audio_stop_all(void) {}
const RtlGameInfo *g_rtl_game_info;

void RtlApuLock(void) { ++locks; }
void RtlApuUnlock(void) {
  assert(locks);
  --locks;
}
void PpuResetWidescreenOamHistory(Ppu *p) {
  (void)p;
  ++presentation_resets;
}
void cart_saveload(Cart *c, SaveLoadInfo *s) { s->func(s, c->ram, c->ramSize); }

static bool table_nonempty(void) {
  for (unsigned i = 0; i < sizeof(ppu.wsOamCoordinates.valid); ++i)
    if (ppu.wsOamCoordinates.valid[i])
      return true;
  return false;
}
static void reconcile(uint32_t version) {
  (void)version;
  ++reconcile_calls;
  if (table_nonempty())
    ++table_live_at_reconcile;
  /* The title semantic of on_state_loaded is the host's on_reset
   * (Smw2OnReset → Smw2SpritesReset): pending pack-capture ownership
   * belongs to the abandoned timeline. The REAL module reset is invoked
   * so gate 4 exercises the actual callback path, not a proxy. */
  Smw2SpritesReset();
}
static const RtlGameInfo game = {
    .on_state_loaded = reconcile,
};

/* rollback-side externs the spliced residue code reads/writes. The types
 * match common_rtl.c's own extern declarations. */
uint8_t g_memsel;
uint8_t g_snesrecomp_last_hdmaen;
int g_interp_apu_driving;
uint64_t g_apu_pace_cycles_estimate;
uint64_t g_apu_last_sync_cycles;
uint64_t g_apu_last_sync_master;
static uint64_t g_apu_frame_start_master;
uint64_t g_main_cpu_cycles_estimate;
static int g_apu_frame_time_valid;
static RtlApuFrameClock g_apu_frame_clock;

/* The subsystems whose state rides the residue get recording stubs: capture
 * writes IN, apply reads OUT; the test round-trips tagged values through
 * them where it matters. */
typedef struct {
  int saves, restores;
} StubCount;
static StubCount apu_port_ct, interp_ct, dma_ct, refresh_ct;
void apu_port_sched_save(const Apu *a, ApuPortSched *out) {
  (void)a;
  ++apu_port_ct.saves;
  memset(out, 0xAB, sizeof *out);
}
void apu_port_sched_restore(Apu *a, const ApuPortSched *in) {
  (void)a;
  (void)in;
  ++apu_port_ct.restores;
}
void snes_refresh_state_get(uint64_t *phase, uint64_t *charged_upto) {
  ++refresh_ct.saves;
  *phase = 0x1111;
  *charged_upto = 0x2222;
}
void snes_refresh_state_set(uint64_t phase, uint64_t charged_upto) {
  ++refresh_ct.restores;
  (void)phase;
  (void)charged_upto;
}
size_t interp_bridge_rb_state_size(void) { return 16; }
void interp_bridge_rb_state_save(void *out) {
  ++interp_ct.saves;
  memset(out, 0xCD, 16);
}
void interp_bridge_rb_state_load(const void *in) {
  ++interp_ct.restores;
  (void)in;
}
uint8_t dma_hdma_pending_init_get(const Dma *d) {
  (void)d;
  ++dma_ct.saves;
  return 0x5A;
}
void dma_hdma_pending_init_set(Dma *d, uint8_t mask) {
  (void)d;
  (void)mask;
  ++dma_ct.restores;
}

/* Host-side statics that live OUTSIDE the spliced common_rtl.c windows:
 * the [RTL_SAV_MAGIC .. host-pacing) splice ends just before these. */
static uint64_t s_state_generation;
static bool s_rollback_load;
static RtlStateExtraBoundCallback *s_state_extra_bound;

uint64_t RtlStateGeneration(void) { return s_state_generation; }

/* ── Real src/smw2_sprites.c is linked into this TU (gate-4 real Before/
 * Begin). Its neighbours are recording stubs; the hook PCs the module
 * arms land in s_hooks so the test can fire the real capture callbacks. */
Ppu *g_ppu = &ppu;
bool g_ws_active;
static uint8_t s_fx_ram[0x10000];
static uint16_t s_fx_regs[16];

int cpu_dispatch_has_entry(CpuState *cpu, uint32_t pc24) {
  (void)cpu;
  (void)pc24;
  return 0; /* no AOT coverage: CPU hooks stay eligible */
}
bool interp_bridge_add_pre_opcode_hook(uint32_t pc24,
                                       InterpPreOpcodeHook hook) {
  (void)pc24;
  (void)hook;
  return true; /* composed callback registered */
}

typedef struct {
  uint32_t pc;
  SuperFxPcHook *hook;
  void *ctx;
} RecHook;
static RecHook s_hooks[16];
static int s_nhooks;

bool superfx_set_pc_hook(SuperFx *f, uint32_t pc24, SuperFxPcHook *hook,
                         void *context) {
  (void)f;
  for (int i = 0; i < s_nhooks; i++)
    if (s_hooks[i].pc == (pc24 & 0xffffffu)) {
      s_hooks[i].hook = hook;
      s_hooks[i].ctx = context;
      return true;
    }
  if (!hook)
    return true; /* disarm of an unarmed slot: fine */
  s_hooks[s_nhooks].pc = pc24 & 0xffffffu;
  s_hooks[s_nhooks].hook = hook;
  s_hooks[s_nhooks].ctx = context;
  s_nhooks++;
  return true;
}
uint8_t superfx_ram_peek(const SuperFx *f, uint16_t address) {
  (void)f;
  return s_fx_ram[address];
}
uint16_t superfx_reg(const SuperFx *f, unsigned n) {
  (void)f;
  return s_fx_regs[n & 15];
}
void superfx_set_reg(SuperFx *f, unsigned n, uint16_t value) {
  (void)f;
  s_fx_regs[n & 15] = value;
}
void superfx_hook_redirect(SuperFx *f, uint16_t address) {
  (void)f;
  (void)address;
}
