#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
The BACKTRACE ORACLE (#38 repro B) - RULE 2 for an uncaught error.

The reference for a rendered error is the SAME program run with inlining
OFF in the tree-walker (`-ni -tw`): no inlined-at chain exists there, so
every frame is a physical one and nothing can be dropped. Every other
configuration - inlining on in each engine, the JIT's levers, the
bytecode splice on and off, a stored `.myv` image, every AST transform
off - must print a BYTE-IDENTICAL stdout, stderr (the header, the caret,
the whole backtrace) and exit code.

Usage: tests/bt_oracle.py BINARY [PROGRAM.my ...]
       (default corpus: tests/bt_oracle/*.my plus the generated
        recursion-unroll programs, one per depth 1..8)

NOT VACUOUS: the whole point is inlined code, so the run fails unless
the inliner actually changed the program somewhere - the corpus must
contain programs whose `-s` optimized tree holds an inlined splice.
"""

import difflib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# (name, extra argv, extra env). The reference is first.
REF = ("-ni -tw", ["-ni", "-tw"], {})
CONFIGS = [
    ("--no-opt all -ni -tw", ["--no-opt", "all", "-ni", "-tw"], {}),
    ("-tw", ["-tw"], {}),
    ("-ni (vm+jit)", ["-ni"], {}),
    ("-ni -nj", ["-ni", "-nj"], {}),
    ("-nj", ["-nj"], {}),
    ("default (jit)", [], {}),
    ("-nbi", ["-nbi"], {}),
    ("-nbi -nj", ["-nbi", "-nj"], {}),
    ("OFF=norec", [], {"MYLANG_JIT_OFF": "norec"}),
    ("OFF=frameless", [], {"MYLANG_JIT_OFF": "frameless"}),
    ("OFF=all", [], {"MYLANG_JIT_OFF": "all"}),
    # the sync depth cap drops to 32 without the native stack (what a
    # sanitized build runs), so a deep recursion takes the SWITCH path
    ("NATIVE_STACK=0", [], {"MYLANG_NATIVE_STACK": "0"}),
]

# The fib-unroll shape: a pure tree recursion the AST inliner unrolls in
# place, throwing from the base case on a WARMED call (iteration 3 of a
# loop - a first descent never takes the emitted push).
REC_TEMPLATE = """\
func u(n, k) {
    if (n < 2) return 10 / (n - k);
    return u(n - 1, k) + u(n - 2, k);
}
func drive(int d) {
    var s = 0;
    for (var i = 0; i < 4; i++) {
        var k = -1;
        if (i == 3) k = runtime(1);
        s = s + u(runtime(d), k);
    }
    return s;
}
print(drive(%d));
"""


def run(binary, prog, argv, env):
    # run from the program's directory with a bare file name, so the
    # header names the file the same way a .myv image's source reference
    # resolves it
    e = dict(os.environ)
    e.update(env)
    p = subprocess.run([binary] + argv + [os.path.basename(prog)],
                       capture_output=True, env=e, timeout=60,
                       cwd=os.path.dirname(os.path.abspath(prog)))
    return (p.returncode, p.stdout.decode(errors="replace"),
            p.stderr.decode(errors="replace"))


def inlines(binary, prog):
    """True when the inliner changed this program's tree."""
    a = subprocess.run([binary, "-s", prog], capture_output=True,
                       timeout=60).stdout
    b = subprocess.run([binary, "-s", "-ni", prog], capture_output=True,
                       timeout=60).stdout
    # -s dumps the tree then RUNS; only the dumps matter, and they differ
    # iff inlining did something (the run's output is the same either way
    # once the oracle is green)
    return a != b


def show(tag, r):
    rc, out, err = r
    return "  [%s] rc=%d\n  stdout:\n%s  stderr:\n%s" % (
        tag, rc, out, err)


def diff(ref, r, tag):
    a = ("rc=%d\n" % ref[0] + ref[1] + ref[2]).splitlines()
    b = ("rc=%d\n" % r[0] + r[1] + r[2]).splitlines()
    return "\n".join(difflib.unified_diff(a, b, REF[0], tag, lineterm="",
                                          n=1))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="bt_oracle.")
    progs = sys.argv[2:]
    if not progs:
        d = os.path.join(HERE, "bt_oracle")
        progs = sorted(os.path.join(d, f) for f in os.listdir(d)
                       if f.endswith(".my"))
        for depth in range(1, 9):
            p = os.path.join(tmp, "rec_unroll_%d.my" % depth)
            with open(p, "w") as f:
                f.write(REC_TEMPLATE % depth)
            progs.append(p)

    fails = 0
    inlined = 0
    for prog in progs:
        ref = run(binary, prog, REF[1], REF[2])
        name = os.path.basename(prog)
        bad = False
        if ref[0] == 0 or "at line" not in ref[2]:
            print("VACUOUS %s: the reference did not end in an uncaught "
                  "error with a backtrace" % name)
            print(show(REF[0], ref))
            fails += 1
            continue
        if inlines(binary, prog):
            inlined += 1
        results = [(c[0], run(binary, prog, c[1], c[2])) for c in CONFIGS]
        img = os.path.join(tmp, name + ".myv")
        c = subprocess.run([binary, "-c", os.path.basename(prog), "-o",
                            img], capture_output=True, timeout=60,
                           cwd=os.path.dirname(os.path.abspath(prog)))
        if c.returncode != 0:
            results.append((".myv compile", (c.returncode, "",
                                             c.stderr.decode())))
        else:
            results.append((".myv", run(binary, img, [], {})))
            os.unlink(img)
        for tag, r in results:
            if r != ref:
                if not bad:
                    print("FAIL %s" % name)
                    print(show(REF[0], ref))
                bad = True
                print(diff(ref, r, tag))
        if bad:
            fails += 1
    shutil.rmtree(tmp, ignore_errors=True)
    print("bt_oracle: %d/%d programs render identically to `-ni -tw` "
          "in %d configurations; %d inline" % (
              len(progs) - fails, len(progs), len(CONFIGS) + 1, inlined))
    if inlined * 2 < len(progs):
        print("bt_oracle: VACUOUS - fewer than half the programs are "
              "changed by the inliner")
        return 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
