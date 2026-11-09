#!/usr/bin/env python3
"""Task-4 widescreen-completion suite runner.

Phases per suite: normal build+run, then ASan+UBSan (halt_on_error,
detect_leaks=0). Suites:
  smw2_ws_geometry_test.c  — pure geometry contract (self-contained)
  smw2_bg_render_test.c    — real PPU raster paths, both renderers
  smw2_state_load_test.c   — spliced production snapshot/rollback walkers
  smw2_host_wiring_test.c  — real src/main.c hook wiring
  smw2_runahead_test.c     — REAL snes_runahead.c speculation loop:
                            optional per-speculative-frame Before hook

The state-load translation unit is spliced from production sources the
same way snesrecomp/tests/snapshot_guard/test_transaction.py builds its
transaction fixture: real Rtl* walkers + real PpuWsSetOamCoordinates +
real ppu_rb_residue_get/set, stubbed neighbours.

Usage: python3 tests/run_smw2_task4.py [--cc cc] [--skip-sanitizers]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'snesrecomp' / 'runner' / 'src'

INC = ['-I' + str(ROOT), '-I' + str(RUNNER), '-I' + str(RUNNER / 'snes')]
HW_INC = INC + ['-I' + str(ROOT / 'tests' / 'shim'), '-I' + str(ROOT / 'tests'),
                '-I' + str(ROOT / 'src'), '-I' + str(RUNNER / 'desktop'),
                '-I/opt/homebrew/include', '-DSDL_MAIN_HANDLED',
                '-DSNESRECOMP_SDL3=1']


def run(cmd, env=None, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, env=env, **kw)


def function(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def build_state_load_tu(tmp):
    """Assemble the spliced translation unit (transaction_fixture.py style)."""
    rtl = (RUNNER / 'common_rtl.c').read_text(encoding='utf-8')
    snes = (RUNNER / 'snes/snes.c').read_text(encoding='utf-8')
    parts = [(ROOT / 'tests/smw2_state_load_fixture.c').read_text(encoding='utf-8')]
    for f, n in [('cpu', 'cpu'), ('apu', 'apu'), ('dsp', 'dsp'), ('spc', 'spc'),
                 ('dma', 'dma'), ('ppu', 'ppu')]:
        parts.append(function((RUNNER / f'snes/{f}.c').read_text(encoding='utf-8'),
                              f'{n}_saveload('))
    dsp = (RUNNER / 'snes/dsp.c').read_text(encoding='utf-8')
    for name in ('dsp_output_ring_save(', 'dsp_output_ring_restore('):
        parts.append(function(dsp, name))
    parts.append(snes[snes.index('static uint32_t s_saveload_version'):snes.index('void snes_reset(')])
    parts.append(rtl[rtl.index('#define RTL_SAV_MAGIC'):rtl.index('/* Host-side state that paces')])
    parts.append(rtl[rtl.index('bool RtlSaveSnapshot('):rtl.rfind('/*', 0, rtl.index('See common_rtl.h for why the guest'))])
    # rollback residue typedef + resync pairs + capture/apply + save/load
    parts.append(rtl[rtl.index('#define RTL_RB_RESIDUE_MAGIC'):rtl.index('static RtlRollbackResidue s_loaded_execution;')])
    parts.append(function(rtl, 'size_t RtlRollbackSaveToMemory('))
    parts.append(function(rtl, 'bool RtlRollbackLoadFromMemory('))
    # real coordinate-table functions under test
    ppu = (RUNNER / 'snes/ppu.c').read_text(encoding='utf-8')
    for name in ('void PpuWsSetOamCoordinates(', 'void ppu_rb_residue_get(',
                 'void ppu_rb_residue_set('):
        parts.append(function(ppu, name))
    parts.append((ROOT / 'tests/smw2_state_load_test.c').read_text(encoding='utf-8'))
    src = tmp / 'smw2_state_load.c'
    src.write_text('\n'.join(parts), encoding='utf-8')
    return src


def build_and_run(cc, name, sources, extra_inc=None, san=False, tmp=None):
    out = tmp / (name + ('_san' if san else ''))
    cmd = [cc, '-std=c11', '-O1', '-w'] + (extra_inc or INC)
    if san:
        cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    cmd += [str(s) for s in sources] + ['-o', str(out)]
    b = run(cmd, cwd=ROOT)
    if b.returncode != 0:
        print(f'{name}: build FAILED (san={san})')
        print(b.stderr, file=sys.stderr)
        sys.exit(1)
    env = dict(os.environ)
    if san:
        env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    r = run([str(out)], env=env, cwd=tmp)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0:
        print(f'{name}: run FAILED (san={san})')
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    ap.add_argument('--skip-sanitizers', action='store_true')
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix='smw2-task4-') as t:
        tmp = Path(t)

        build_and_run(args.cc, 'smw2_ws_geometry',
                      [ROOT / 'tests/smw2_ws_geometry_test.c'], tmp=tmp)
        build_and_run(args.cc, 'smw2_bg_render',
                      [ROOT / 'tests/smw2_bg_render_test.c',
                       RUNNER / 'snes/ppu.c', RUNNER / 'snes/ppu_legacy.c',
                       RUNNER / 'snes/ws_shadow.c'],
                      extra_inc=INC + ['-DSMW2_LINK_REAL_WS_SHADOW'], tmp=tmp)
        sl = build_state_load_tu(tmp)
        state_srcs = [sl, RUNNER / 'snes/joypad.c', RUNNER / 'snapshot_guard.c',
                      RUNNER / 'crc32.c', ROOT / 'src/smw2_sprites.c']
        state_inc = INC + ['-include', str(RUNNER / 'snapshot_guard.h')]
        build_and_run(args.cc, 'smw2_state_load', state_srcs,
                      extra_inc=state_inc, tmp=tmp)
        build_and_run(args.cc, 'smw2_host_wiring',
                      [ROOT / 'tests/smw2_host_wiring_test.c'],
                      extra_inc=HW_INC, tmp=tmp)
        build_and_run(args.cc, 'smw2_runahead',
                      [ROOT / 'tests/smw2_runahead_test.c',
                       RUNNER / 'snes_runahead.c'], tmp=tmp)

        if args.skip_sanitizers:
            print('sanitizers: skipped')
            return
        for name, srcs, inc in [
                ('smw2_ws_geometry', [ROOT / 'tests/smw2_ws_geometry_test.c'], INC),
                ('smw2_bg_render', [ROOT / 'tests/smw2_bg_render_test.c',
                                    RUNNER / 'snes/ppu.c',
                                    RUNNER / 'snes/ppu_legacy.c',
                                    RUNNER / 'snes/ws_shadow.c'],
                 INC + ['-DSMW2_LINK_REAL_WS_SHADOW']),
                ('smw2_state_load', state_srcs, state_inc),
                ('smw2_host_wiring', [ROOT / 'tests/smw2_host_wiring_test.c'], HW_INC),
                ('smw2_runahead', [ROOT / 'tests/smw2_runahead_test.c',
                                   RUNNER / 'snes_runahead.c'], INC)]:
            build_and_run(args.cc, name, srcs, extra_inc=inc, san=True, tmp=tmp)
        print('task4: all suites PASS (normal + ASan/UBSan)')


if __name__ == '__main__':
    main()
