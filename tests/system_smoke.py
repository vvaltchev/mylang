#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Basic functional smoke test for a RELEASE binary that has NO -rt suite.

The -rt unit tests are compiled in only under TESTS=1. A default release
(TESTS=0, ASSERTS=1) and a bare release (TESTS=0, ASSERTS=0) therefore ship
with NO in-process test coverage at all - so nothing in CI proved they even
run correctly until this. It is deliberately shallow: build + run actual
scripts at the minimum size, confirm they don't crash and produce the right
answers. Coverage is the TESTS lanes' job; this is "does the shipped binary
work at all".

WHAT IT RUNS (all at scale 1 - the minimum, exactly the bench suite's
smallest point):
  - every bench/my/NN.my with a bench/py/NN.py twin, compared TOKEN-WISE
    against CPython (ints exact, floats within a relative tolerance - the
    same normalization bench/run.py uses, which absorbs MyLang's cosmetic
    print differences: a trailing space, floats as 3.500000 vs 3.5). Run
    through BOTH engines - the JIT default and the tree-walker (-tw) - since
    the release has no differential harness to catch an engine divergence.
  - every tests/functional/NN.my - self-asserting programs that throw on
    failure, so exit 0 IS the assertion.

Non-deterministic pairs (rand*) are skipped; samples/ is skipped (its
scripts are the interactive human showcase - they read stdin and take argv
in their own way, not a fixed scale). Stdlib only.

    tests/system_smoke.py ./mylang

Exit 0 if everything passed, 1 otherwise.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIMEOUT = 60


def tokens(s):
    return s.split()


def same(a, b):
    """Token-wise: ints exact, floats within 1e-6 relative - bench/run.py's
    comparison, so a cosmetic print difference is not a failure but a real
    logic divergence is."""
    ta, tb = tokens(a), tokens(b)
    if len(ta) != len(tb):
        return False
    for x, y in zip(ta, tb):
        if x == y:
            continue
        try:
            fx, fy = float(x), float(y)
        except ValueError:
            return False
        scale = max(abs(fx), abs(fy), 1.0)
        if abs(fx - fy) / scale > 1e-6:
            return False
    return True


def run(argv):
    try:
        r = subprocess.run(argv, timeout=TIMEOUT, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return r.returncode, r.stdout.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        return None, '(timeout)'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    mylang = sys.argv[1]
    if not os.access(mylang, os.X_OK):
        print("usage: %s <path-to-mylang>" % sys.argv[0])
        return 2

    fails = 0
    ran = 0

    # 1. bench pairs vs CPython, both engines
    mydir = os.path.join(ROOT, 'bench', 'my')
    for name in sorted(os.listdir(mydir)):
        if not name.endswith('.my') or 'rand' in name:
            continue
        stem = name[:-3]
        py = os.path.join(ROOT, 'bench', 'py', stem + '.py')
        if not os.path.isfile(py):
            continue
        myf = os.path.join(mydir, name)
        prc, pout = run(['python3', py, '1'])
        if prc != 0:
            print('  SKIP  %s (CPython exited %s)' % (stem, prc))
            continue
        for engine in ([], ['-tw']):
            tag = stem + (' -tw' if engine else '')
            ran += 1
            rc, out = run([mylang] + engine + [myf, '1'])
            if rc != 0:
                print('  FAIL  %s: mylang exited %s\n%s'
                      % (tag, rc, out[:400]))
                fails += 1
            elif not same(out, pout):
                print('  FAIL  %s: output differs from CPython' % tag)
                print('    my: %r' % out[:200])
                print('    py: %r' % pout[:200])
                fails += 1

    # 2. functional tests - self-asserting, exit 0 is the pass
    fdir = os.path.join(ROOT, 'tests', 'functional')
    for name in sorted(os.listdir(fdir)):
        if not name.endswith('.my'):
            continue
        ran += 1
        rc, out = run([mylang, os.path.join(fdir, name)])
        if rc != 0:
            print('  FAIL  functional/%s: exited %s\n%s'
                  % (name, rc, out[:400]))
            fails += 1

    print('system smoke: %d runs, %d failures' % (ran, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
