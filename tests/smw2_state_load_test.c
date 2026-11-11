/* Lifecycle gates for the published OAM coordinate table, run through the
 * REAL production snapshot walkers (spliced common_rtl.c) + REAL
 * PpuWsSetOamCoordinates / ppu_rb_residue get/set:
 *
 *   1. ORDINARY load clears the table — unconditionally, including a
 *      byte-identical reload (content-delta detection cannot see the
 *      table: it lives outside ppu_saveload's serialized region).
 *   2. The clear is the semantic post-callback boundary: on_state_loaded
 *      still sees the stale table live.
 *   3. ROLLBACK restores the residue's coordinates AFTER that same
 *      unconditional clear — so a rewind keeps exactly the publication
 *      the restored frame drew (PpuRollbackResidue.wsOamCoordinates).
 *   4. ORDINARY save with the table live retains it (serialization
 *      cannot mutate host state), incl. a byte-identical second save.
 *   5. The clear sits BEFORE the ++s_state_generation bump and rollback
 *      restores the generation, so the next REAL Before/Begin with no
 *      fresh pack keeps the restored table — exercised end-to-end with
 *      real pack-driven publications and a consumed capture that must
 *      not republish.
 *
 * Machine shape mirrors snapshot_guard/transaction_test.c (same walkers,
 * same stubs). */

/* Fire one armed GSU pc-hook through the REAL module callback the hook
 * table recorded (superfx_set_pc_hook stub), as Task3's synthetic-GSU
 * method does: the module's own hook body is what captures the pack. */
static void fire_pc(uint32_t pc24) {
  for (int i = 0; i < s_nhooks; i++)
    if (s_hooks[i].pc == (pc24 & 0xffffffu) && s_hooks[i].hook) {
      s_hooks[i].hook(&t_fx, pc24, s_hooks[i].ctx);
      return;
    }
  assert(0 && "pc hook not armed");
}

/* Drive the REAL capture→model→publish chain for a one-part pack:
 * Before arms the module, the pack-entry hook captures the staged GSU
 * RAM source, the cart mirror + live OAM are staged to the bytes the
 * module's own model predicts, Begin consumes the capture and publishes.
 * Single forward part at x,y,attr → slot 16 (kLowFwd 0xA40 → slot
 * (0xA40-0xA00)/4 = 16), hib = word3&0xff (x>=0) in mirror byte 4. */
static void publish_pack(int16_t x, int16_t y, uint16_t attr,
                         uint16_t word3, PpuWsOamCoordinates *out) {
  /* dead markers for every source entry, then one live forward part */
  for (unsigned i = 0; i < 256; i++) {
    s_fx_ram[0x0200 + i * 8 + 0] = 0x00;
    s_fx_ram[0x0200 + i * 8 + 1] = 0x80; /* $8000 inactive X */
  }
  uint8_t *e = s_fx_ram + 0x0200;
  e[0] = (uint8_t)x; e[1] = (uint8_t)(x >> 8);
  e[2] = (uint8_t)y; e[3] = (uint8_t)(y >> 8);
  e[4] = (uint8_t)attr; e[5] = (uint8_t)(attr >> 8);
  e[6] = (uint8_t)word3; e[7] = (uint8_t)(word3 >> 8);

  memset(cart_ram, 0, sizeof(cart_ram));
  memset(ppu.oam, 0, sizeof ppu.oam);
  memset(ppu.highOam, 0, sizeof ppu.highOam);

  Smw2SpritesBeforeFrame(64, 64); /* real arm: gates + margins */
  fire_pc(0x08B1EFu);             /* real capture: s_pending owns pack */

  /* mirror + OAM = the model's predicted bytes (hand-computed for one
   * admitted forward part, verified in the module's own suite) */
  cart_ram[0x0A40] = (uint8_t)x;
  cart_ram[0x0A41] = (uint8_t)(x >= -15 && x < 256 ? (y - 1) & 0xff : 0xF0);
  cart_ram[0x0A42] = (uint8_t)attr;
  cart_ram[0x0A43] = (uint8_t)(attr >> 8);
  cart_ram[0x0C04] = (uint8_t)(word3 & 0xff); /* hi mirror byte for slot 16 */
  for (unsigned a = 0x0A44; a <= 0x0BFC; a += 4)
    cart_ram[a + 1] = 0xF0; /* gap clearing */
  ppu.oam[32] = (uint16_t)(cart_ram[0x0A40] | (cart_ram[0x0A41] << 8));
  ppu.oam[33] = (uint16_t)(cart_ram[0x0A42] | (cart_ram[0x0A43] << 8));
  ppu.highOam[4] = cart_ram[0x0C04];

  Smw2SpritesBeginFrame(); /* real consume-once + publish */
  if (out)
    *out = ppu.wsOamCoordinates;
}

int main(void) {
  machine.cpu = &cpu;
  machine.apu = &apu;
  machine.dma = &dma;
  machine.ppu = &ppu;
  machine.cart = &cart;
  machine.ram = g_ram;
  apu.dsp = &dsp;
  apu.spc = &spc;
  cart.type = CART_SUPERFX;
  cart.ram = cart_ram;
  cart.ramSize = sizeof(cart_ram);
  cart.rom = cart_rom;
  cart.romSize = sizeof(cart_rom);
  cart.superfx = &t_fx;
  machine.cart = &cart;
  /* ROM shape bytes the module's armed-PC guards require (LoROM map:
   * (bank&0x3f)<<15 | (adr&0x7fff)). Synthetic fixture bytes placed at
   * the production hook PCs — same method as tests/smw2_sprite_test.c. */
  {
    unsigned char *R = cart_rom;
#define ROM8(a24) R[(((a24) >> 16 & 0x3f) << 15) | ((a24) & 0x7fff)]
    ROM8(0x08B1EF) = 0xac; /* ibt r12  — pack entry      */
    ROM8(0x08B219) = 0x3f; ROM8(0x08B21A) = 0x67; /* cmp r7 — X lo */
    ROM8(0x08B221) = 0x3f; ROM8(0x08B222) = 0x68; /* cmp r8 — X hi */
    ROM8(0x08B230) = 0x3f; ROM8(0x08B231) = 0x67; /* cmp r7 — Y    */
    ROM8(0x08B251) = 0xee; /* dec r14 — forward write  */
    ROM8(0x08B263) = 0xee; /* dec r14 — reverse write  */
    ROM8(0x098055) = 0x63; /* sub r3  — spawn decide   */
    ROM8(0x098A18) = 0x15; /* to r5   — despawn latch  */
    /* CPU despawn shapes: ADC abs16 / CMP abs16 / BCS windows */
    ROM8(0x03A2E2) = 0x69; ROM8(0x03A2E3) = 0x80; ROM8(0x03A2E4) = 0x00;
    ROM8(0x03A2E5) = 0xc9; ROM8(0x03A2E6) = 0x00; ROM8(0x03A2E7) = 0x02;
    ROM8(0x03A2E8) = 0xb0;
    ROM8(0x03A2FC) = 0x69; ROM8(0x03A2FD) = 0x40; ROM8(0x03A2FE) = 0x00;
    ROM8(0x03A2FF) = 0xc9; ROM8(0x03A300) = 0x80; ROM8(0x03A301) = 0x01;
    ROM8(0x03A302) = 0xb0;
#undef ROM8
  }
  g_ram[0x0118] = 0x0f; /* level gameplay mode gate */
  g_rtl_game_info = &game;
  g_cpu.ram = g_ram;
  cpu.a = 0x1234;
  g_ram[70] = 8;

  const size_t CAP = 3u * 1024 * 1024;
  unsigned char *snapshot = malloc(CAP);
  unsigned char *snapshot2 = malloc(CAP);
  unsigned char *snapshot3 = malloc(CAP);
  unsigned char *rollback_snap = malloc(CAP);
  assert(snapshot && snapshot2 && snapshot3 && rollback_snap);
  size_t size = RtlSaveSnapshotToMemory(snapshot, CAP);
  assert(size > 0x20000);

  /* publish a coordinate table — the "live publication" for every gate */
  PpuWsOamCoordinates pub;
  memset(&pub, 0, sizeof pub);
  memcpy(pub.expectedOam, ppu.oam, sizeof pub.expectedOam);
  memcpy(pub.expectedHighOam, ppu.highOam, sizeof pub.expectedHighOam);
  pub.valid[0] |= 1u;
  pub.x[0] = 300;
  pub.y[0] = 0x40;
  PpuWsSetOamCoordinates(&ppu, &pub);
  assert(table_nonempty());

  /* ── gate 1+2: ordinary load clears the table AFTER on_state_loaded ── */
  {
    unsigned gen0 = s_state_generation;
    assert(RtlLoadSnapshotFromMemory(snapshot, size));
    assert(!table_nonempty()); /* cleared unconditionally */
    assert(reconcile_calls == 1);
    assert(table_live_at_reconcile == 1); /* title callback saw it first */
    assert(s_state_generation == gen0 + 1);
    assert(presentation_resets == 1);
    assert(!locks);
  }

  /* republish + byte-identical reload: still cleared (the bug shape is a
   * stale table surviving a same-state load — no content delta sees it) */
  PpuWsSetOamCoordinates(&ppu, &pub);
  {
    unsigned gen0 = s_state_generation;
    assert(RtlLoadSnapshotFromMemory(snapshot, size));
    assert(!table_nonempty());
    assert(s_state_generation == gen0 + 1);
    assert(reconcile_calls == 2);
    assert(table_live_at_reconcile == 2);
  }

  /* ── ordinary SAVE retains the live table ─────────────────────────────
   * The save walker serializes machine state only; the host-published
   * table must still be live afterwards — including when the save runs
   * over a byte-identical machine (no serialized-content delta to clear
   * on: that clearing is a LOAD-side semantic). */
  PpuWsSetOamCoordinates(&ppu, &pub);
  size_t size2 = RtlSaveSnapshotToMemory(snapshot2, CAP);
  assert(size2 > 0x20000);
  assert(table_nonempty());
  assert(!memcmp(&ppu.wsOamCoordinates, &pub, sizeof pub));
  /* and a second save over unchanged machine state keeps it too */
  size_t size3 = RtlSaveSnapshotToMemory(snapshot3, CAP);
  assert(size3 == size2);
  assert(!memcmp(&ppu.wsOamCoordinates, &pub, sizeof pub));

  /* ── gate 3+4: REAL capture→publish → rollback restore → REAL
   * next-frame Before/Begin with no fresh pack ──────────────────────────
   * T1 is published by the module's own consume path (not hand-written),
   * so the residue carries exactly what a real BeginFrame produced. */
  PpuWsOamCoordinates t1, t2;
  {
    /* Policy: a native-visible part keeps hardware decode — publishing the
     * raw GSU source over it would shift moving sprites by the game's own
     * pack rounding. Only widened-only positions are published. */
    PpuWsOamCoordinates t0;
    publish_pack(40, 0x60, 0x3333, 0x0000, &t0);
    assert(!(t0.valid[2] & 1u));
  }
  publish_pack(300, 0x41, 0x2244, 0x0002, &t1); /* x_vis: packed_y = y-1 */
  assert(t1.valid[2] & 1u); /* slot 16 published for real */
  assert(!memcmp(&ppu.wsOamCoordinates, &t1, sizeof t1));

  size_t rb_size = RtlRollbackSaveToMemory(rollback_snap, CAP);
  assert(rb_size > size);
  {
    /* A consumed SPECULATIVE capture must not republish after rewind:
     * stage a different pack, let the REAL Begin consume it (pending
     * cleared by the consume itself), mutating the live table to T2. */
    /* Serialized OAM staged for T1 must be RESTORED before the load:
     * ppu_saveload (ppu.c:149-157) clears wsOamCoordinates itself on any
     * serialized-content diff. Returning the machine to the saved OAM
     * bytes makes this a same-content load — the T2 table then lives
     * through ppu_saveload into the callback, so the test exercises the
     * POST-callback unconditional clear rather than the diff clear. */
    uint16_t oam_keep[256];
    uint8_t hi_keep[32];
    memcpy(oam_keep, ppu.oam, sizeof oam_keep);
    memcpy(hi_keep, ppu.highOam, sizeof hi_keep);
    publish_pack(-40, 0x60, 0x3333, 0x0003, &t2);
    assert(t2.valid[2] & 1u);
    assert(memcmp(&t1, &t2, sizeof t1) != 0);
    assert(!memcmp(&ppu.wsOamCoordinates, &t2, sizeof t2));
    memcpy(ppu.oam, oam_keep, sizeof oam_keep);
    memcpy(ppu.highOam, hi_keep, sizeof hi_keep);

    unsigned gen0 = s_state_generation;
    unsigned rec0 = reconcile_calls;
    unsigned pres0 = presentation_resets;
    assert(RtlRollbackLoadFromMemory(rollback_snap, rb_size));
    /* The inner load cleared unconditionally — reconcile saw T2 live
     * (same-content load: no ppu_saveload diff-clear), then the title
     * callback's Smw2SpritesReset ran — its pending-clear is exercised
     * below — then residue_apply restored T1. */
    assert(!memcmp(&ppu.wsOamCoordinates, &t1, sizeof t1));
    assert(reconcile_calls == rec0 + 1);
    assert(table_live_at_reconcile == 3); /* callback saw the T2 table */
    assert(presentation_resets == pres0 + 1);
    assert(s_state_generation == gen0); /* rollback keeps the generation */
    assert(!locks);
    /* residue-side stubs all round-tripped */
    assert(apu_port_ct.saves >= 1 && apu_port_ct.restores >= 1);
    assert(interp_ct.saves >= 1 && interp_ct.restores >= 1);
    assert(dma_ct.saves >= 1 && dma_ct.restores >= 1);
    assert(refresh_ct.saves >= 1 && refresh_ct.restores >= 1);
  }

  /* ── gate 4, REAL module calls: the restored publication survives the
   * actual next-frame Before/Begin with NO fresh pack — BeforeFrame re-arms
   * against live gates, BeginFrame finds pending invalid (the inner-load
   * Smw2SpritesReset cleared it; the T2 consume had also consumed it) and
   * returns early, so neither the restored T1 nor the abandoned T2
   * changes. A republished T2 here would be the republish defect. */
  Smw2SpritesBeforeFrame(64, 64);
  Smw2SpritesBeginFrame();
  assert(!memcmp(&ppu.wsOamCoordinates, &t1, sizeof t1));
  /* And a SECOND no-pack frame keeps it (the nothing-clears-outside-
   * loads property is per-frame, not one-shot). */
  Smw2SpritesBeforeFrame(64, 64);
  Smw2SpritesBeginFrame();
  assert(!memcmp(&ppu.wsOamCoordinates, &t1, sizeof t1));

  /* rollback blob without the residue tail is rejected (not a rollback
   * snapshot) — the residue is what carries the coordinates */
  assert(!RtlRollbackLoadFromMemory(snapshot, size));

  /* truncated rollback blob rejected, live table intact */
  PpuWsSetOamCoordinates(&ppu, &pub);
  assert(!RtlRollbackLoadFromMemory(rollback_snap, rb_size - 1));
  assert(!memcmp(&ppu.wsOamCoordinates, &pub, sizeof pub));

  /* Smw2SpritesReset for the truncation probe above cleared the module's
   * pending but the table is pub (hand-set); end state consistent. */

  printf("smw2_state_load: PASS\n");
  return 0;
}
