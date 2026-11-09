#!/usr/bin/env python3
"""Cross-platform runner for the Task-3 sprite policy suite
(tests/smw2_sprite_test.c against src/smw2_sprites.c on the real
SuperFX core, 65816 interpreter bridge and Ppu).

Three phases, in order:
  1. RED probe — a translation unit that consumes the module's public
     API must fail to compile/link when the implementation is absent.
     Recorded as the missing-feature behavioral baseline.
  2. Normal build+run.
  3. ASan+UBSan build+run (macOS: detect_leaks=0, LSan unsupported;
     UBSAN_OPTIONS=halt_on_error=1 — a recovery/print is a failure).

Usage: python3 tests/run_smw2_sprites.py [--cc cc] [--skip-sanitizers]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'snesrecomp' / 'runner' / 'src'

SOURCES = [
    'tests/smw2_sprite_test.c',
    'src/smw2_sprites.c',
    'snesrecomp/runner/src/snes/superfx.c',
    'snesrecomp/runner/src/snes/interp816.c',
    'snesrecomp/runner/src/snes/interp_bridge.c',
    'snesrecomp/runner/src/snes/ppu.c',
    'snesrecomp/runner/src/snes/ppu_legacy.c',
    'snesrecomp/runner/src/snes/cx4.c',
    'snesrecomp/runner/src/sha256.c',
    'snesrecomp/runner/src/snes/tier2_capture.c',
]

INCLUDES = [str(RUNNER), str(RUNNER / 'snes')]

PROBE = '''\
/* RED: consumes the Task-3 public surface. Compiles only when the
 * header exists, links only when the implementation is present. */
#include "src/smw2_sprites.h"
int main(void) {
  Smw2SpritesBeforeFrame(32, 16);
  Smw2SpritesBeginFrame();
  Smw2SpritesReset();
  Smw2SpritesShutdown();
  return 0;
}
'''


def run(cmd, env=None, **kw):
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          env=env, **kw)
    return proc


def link_flags():
    if sys.platform == 'darwin':
        return ['-Wl,-dead_strip']
    return ['-Wl,--gc-sections']


def build(cc, out, extra=None):
    cmd = [cc, '-std=c11', '-O1', '-Wall', '-Wextra',
           '-ffunction-sections', '-fdata-sections']
    for inc in INCLUDES:
        cmd += ['-I', inc]
    cmd += [str(ROOT / s) for s in SOURCES]
    cmd += link_flags()
    if extra:
        cmd += extra
    cmd += ['-o', str(out)]
    return run(cmd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--skip-sanitizers', action='store_true')
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix='smw2-sprites-') as tmp:
        tmp = Path(tmp)

        # ---- Phase 1: RED probe -------------------------------------
        probe_src = tmp / 'probe.c'
        probe_src.write_text(PROBE)
        probe = run([args.cc, '-std=c11', '-I', '.',
                     '-fsyntax-only', str(probe_src)], cwd=ROOT)
        if probe.returncode != 0:
            print('RED probe: FAIL — smw2_sprites.h API missing '
                  '(expected pre-implementation)')
            print(probe.stderr, file=sys.stderr)
            sys.exit(1)
        # Header exists: a link WITHOUT the implementation must fail.
        nolink = run([args.cc, '-std=c11', str(probe_src), '-I', '.',
                      '-o', str(tmp / 'probe.exe')], cwd=ROOT)
        if nolink.returncode == 0:
            print('RED probe: FAIL — API linked without implementation')
            sys.exit(1)
        print('RED probe: header present; link without implementation '
              'fails as required (unresolved Smw2Sprites*).')

        # ---- Phase 2: normal build + run -----------------------------
        normal = tmp / 'smw2_sprite_test'
        b = build(args.cc, normal)
        if b.returncode != 0:
            print('normal build FAILED')
            print(b.stderr, file=sys.stderr)
            sys.exit(1)
        r = run([str(normal)])
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0:
            print('normal run FAILED')
            sys.exit(1)

        # ---- Phase 3: ASan + UBSan -----------------------------------
        if args.skip_sanitizers:
            print('sanitizers: skipped')
            return
        san = tmp / 'smw2_sprite_test_san'
        b = build(args.cc, san, [
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])
        if b.returncode != 0:
            print('sanitizer build FAILED')
            print(b.stderr, file=sys.stderr)
            sys.exit(1)
        env = dict(os.environ)
        env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        r = run([str(san)], env=env)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode != 0:
            print('sanitizer run FAILED')
            sys.exit(1)
        print('sanitizers: ASan+UBSan clean (detect_leaks=0, '
              'halt_on_error=1)')


if __name__ == '__main__':
    main()
