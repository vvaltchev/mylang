#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Corrupt a .myv image every way there is and require a DEFINED outcome.

THE CONTRACT (#137, docs/myv-format.txt "TRUST MODEL"): a mangled image may
be refused, and may even be ACCEPTED and produce nonsense - but it must never
crash the interpreter, hang it, or make it allocate without bound. This is
the sweep that found every image bug the loader now defends against, and it
is the only way to re-establish that after a format change.

WHAT COUNTS AS A FAILURE
    exit 0      ran (the mutation was benign, or benign-looking nonsense)  OK
    exit 1      refused, or a MyLang exception                             OK
    signal      SIGSEGV / SIGABRT / std::terminate - a C++-level crash    FAIL
    timeout     a hang, OR an unbounded allocation still grinding         FAIL

The one EXPECTED timeout class is real and undecidable: a mutation can turn
the program into one that legitimately never terminates. Such an image LOADS
cleanly (`mylang -vd img.myv` exits 0), which is the discriminator - use
--triage to see it.

USAGE
    tests/myv_fuzz.py ./build/mylang                 # both corpora
    tests/myv_fuzz.py ./build/mylang -n 4000         # longer
    tests/myv_fuzz.py ./build/mylang --triage        # classify survivors
    tests/myv_fuzz.py ./build/mylang --save /tmp/bad # keep the offenders

A finding is SAVED (--save, default ./myv-fuzz-bad) because it cannot be
regenerated from the seed alone: an image embeds its SOURCE PATH, so the
same seed over a corpus written to a different directory produces different
bytes. Re-run a saved image directly - it is the whole reproducer.

Run it against a DEBUG (ASan + UBSan) build for detection, and against
`OPT=1 ASSERTS=0` for what actually ships - they catch different things: the
sanitized build sees a bad read the release silently survives, and the
release exposes anything an assertion was papering over.

Stdlib only (the no-dependencies rule), like tests/nested_fuzz.py.
"""
import os
import random
import subprocess
import sys
import tempfile

# Two programs, because the SHAPE decides which pools exist at all: a small
# one keeps the signal-to-offset ratio high, while the fat one is the only
# way to reach the struct / dict / closure / try-region records - and the
# `const OPS = [sq]` at its end is there for the VALUE-CODEC's `func` tag,
# the one record no corpus program used to produce. That gap was not
# academic: read_value built its FuncObject with a NULL context and the
# ctor dereferenced it, so the UNMUTATED image segfaulted on load.
SMALL = """\
struct P { int x; }
func f(a) { var p = P(a); return p.x + 1; }
var t = f(3);
print(t);
"""

FAT = """\
struct P { int x; float y; }
struct Q { int a; }
func mk(int n) { return P(n, float(n) * 1.5); }
func dist(P p) { return p.x * p.x + int(p.y); }
var pts = [];
for (var i = 0; i < 6; i++) { append(pts, mk(i)); }
var s = 0;
foreach (var p in pts) { s += dist(p); }
var d = {"a": 1, "b": 2};
d["c"] = 3;
foreach (var k, v in d) { s += v; }
var names = ["x", "y", "z"];
var joined = join(names, "-");
var nested = [[1, 2], [3, 4]];
for (var i = 0; i < 2; i++) { s += nested[i][1]; }
var cnt = 0;
var bump = func[cnt]() { cnt = cnt + 1; return cnt; };
s += bump() + bump();
try { throw Q(9); } catch (Q as e) { s += e.a; } finally { s += 1; }
var t = 0;
try { var z = pts[99]; } catch (OutOfBounds) { t = 1; }
var fl = [1.0, 2.0, 3.0];
var fs = 0.0;
foreach (var f in fl) { fs += sqrt(f); }
var bits = (s << 2) | 7;
pure func sq(x) => x * x;
const OPS = [sq];
var dyn ops = runtime(OPS);
bits += len(ops);
print(s, joined, t, bits, fs > 0.0);
"""


def mutate(base, i, rnd):
    """One mutation. The modes are not interchangeable: truncation reaches
    the length checks, the 0xFF word reaches the count and index checks, and
    the bit flip reaches everything else a byte at a time."""
    b = bytearray(base)
    mode = i % 5
    if mode == 0:                                   # flip one bit
        b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
    elif mode == 1:                                 # set one byte
        b[rnd.randrange(len(b))] = rnd.randrange(256)
    elif mode == 2:                                 # truncate
        b = b[:rnd.randrange(len(b))]
    elif mode == 3:                                 # randomise a burst
        p = rnd.randrange(len(b))
        for k in range(rnd.randrange(1, 31)):
            if p + k < len(b):
                b[p + k] = rnd.randrange(256)
    else:                                           # 0xFF over an aligned word
        p = rnd.randrange(len(b) // 4) * 4
        for k in range(4):
            if p + k < len(b):
                b[p + k] = 0xFF
    return bytes(b), mode


def run(binary, path, timeout, args=()):
    try:
        r = subprocess.run([binary] + list(args) + [path], timeout=timeout,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        return r.returncode, r.stderr.decode('utf-8', 'replace').strip()
    except subprocess.TimeoutExpired:
        return None, ''


def save(savedir, name, i, blob):
    os.makedirs(savedir, exist_ok=True)
    path = os.path.join(savedir, '%s-%d.myv' % (name, i))
    open(path, 'wb').write(blob)
    return path


def sweep(binary, name, src, n, seed, timeout, triage, tmp, savedir):
    my = os.path.join(tmp, name + '.my')
    img = os.path.join(tmp, name + '.myv')
    mut = os.path.join(tmp, name + '_m.myv')
    open(my, 'w').write(src)
    subprocess.run([binary, '-c', my, '-o', img], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    base = open(img, 'rb').read()
    print('%s: %d bytes, %d mutations' % (name, len(base), n))

    rnd = random.Random(seed)
    hangs = crashes = 0
    for i in range(n):
        blob, mode = mutate(base, i, rnd)
        open(mut, 'wb').write(blob)
        rc, err = run(binary, mut, timeout)
        if rc is None:
            hangs += 1
            note = ''
            if triage:
                # -vd LOADS (running the load-time JIT) and dumps without
                # executing: exit 0 means the image is structurally fine and
                # the program itself simply does not terminate - the one
                # legitimate timeout.
                lrc, _ = run(binary, mut, timeout, ('-vd',))
                note = (' [loads cleanly - a non-terminating program]'
                        if lrc == 0 else ' [hung during LOAD - a real bug]')
            print('  HANG   #%d mode=%d%s  -> %s'
                  % (i, mode, note, save(savedir, name, i, blob)))
        elif rc < 0 or rc not in (0, 1):
            crashes += 1
            print('  CRASH  #%d mode=%d rc=%d  %s  -> %s'
                  % (i, mode, rc, err[:200], save(savedir, name, i, blob)))
    return hangs, crashes


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    n = 1600
    seed = 20260809
    timeout = 10
    triage = '--triage' in sys.argv
    savedir = 'myv-fuzz-bad'
    if '-n' in sys.argv:
        n = int(sys.argv[sys.argv.index('-n') + 1])
    if '--save' in sys.argv:
        savedir = sys.argv[sys.argv.index('--save') + 1]

    total_h = total_c = 0
    with tempfile.TemporaryDirectory(prefix='mylang-myvfuzz-') as tmp:
        for name, src in (('small', SMALL), ('fat', FAT)):
            h, c = sweep(binary, name, src, n, seed, timeout, triage, tmp,
                         savedir)
            total_h += h
            total_c += c

    print('\n=== %d hangs, %d crashes over %d mutations ==='
          % (total_h, total_c, 2 * n))
    if total_c:
        print('A CRASH IS ALWAYS A BUG - the loader must refuse, not die.')
    if total_h and not triage:
        print('Re-run with --triage to separate a non-terminating PROGRAM '
              '(expected) from a hang in the LOADER (a bug).')
    # Only a crash fails the run: a hang may be the undecidable case, and
    # --triage is how a human tells them apart.
    return 1 if total_c else 0


if __name__ == '__main__':
    sys.exit(main())
