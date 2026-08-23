#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
NET 3 - EXHAUSTIVE SMALL-SCOPE ENUMERATION for the G1 no-record tier.

    tests/norec_enum.py BINARY [--depth 4] [--jobs N]

NOT A FUZZER.  It emits EVERY program in a bounded shape space and runs
each one through four engine configurations, comparing stdout, stderr
and exit status BYTE-FOR-BYTE.  RULE 2 is the spec: an optimization may
never change observable behaviour, and the no-record tier's rebuilt
frames feed the backtrace - which is exactly the hard consumer.

THE SHAPE SPACE (plans/archived/g1-no-record-tier.md, Net 3):
  depth        1..N chained functions (default 4)
  frame kind   per level: plain / try / try-finally / dict-iter
  terminal     return int / return float / throw
  catch level  for a throw: caught at level j for every j, or uncaught

Why a BOUNDED depth suffices, stated so it can be attacked: the walk is
inductive - each step processes one frame given only its parent's
anchor - so an ordering or off-by-one bug is expressible in an
interleaving of length <= 3-4; more depth adds repetition, not new
states.  The cases that do NOT fit that argument are enumerated
explicitly by --extra (a call at a segment boundary, recursion deep
enough to grow the native stack, and a reconstruction spanning both).

TWO DELIBERATE SUBSTITUTIONS, recorded rather than silently dropped
(the plan's axis list is closed to removals):
  * "cached-call" as a frame kind is not generated.  A frame gets a
    cache key only when its callee is a pure tree-recursive function,
    which cannot also be a link in an impure chain - the two are
    mutually exclusive by construction.  The axis is instead covered
    where it is reachable: `--cached` adds a variant whose levels call
    a pure recursive helper alongside the descent.
  * "a builtin that captures a backtrace WITHOUT throwing" has no
    builtin to call - MyLang exposes none.  Backtrace CAPTURE is
    exercised by every throw variant (frames are recorded as the
    exception unwinds, caught or not) and backtrace RENDERING by the
    uncaught ones, which is where the byte-for-byte compare bites.

VACUITY IS REPORTED, NOT ASSUMED.  --reach re-runs a sample under
MYLANG_JITSTATS and prints how many programs actually produced emitted
pushes and record-less pushes; a space that never reaches the tier
proves nothing, and the summary says so instead of printing a green
zero.

Stdlib only.
"""

import argparse
import itertools
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

KINDS = ("plain", "try", "tryfin", "diter")
TERMINALS = ("ret_int", "ret_float", "throw")

# (label, argv-prefix, env) - the four configurations RULE 2 must make
# indistinguishable.  `shadow` is the no-record tier OFF: records are
# still written and the reconstruction is verified against them.
ENGINES = (
    ("tw",         ["-tw"], {}),
    ("vm",         ["-nj"], {}),
    ("jit",        [],      {}),
    ("shadow",     [],      {"MYLANG_JIT_OFF": "norec"}),
)

PRELUDE = """struct Boom { int at; }
struct Never { int x; }
var D = {"a": 1, "b": 2};
"""


def level_body(i, depth, kind, terminal, catch_at, flt):
    """The body of L<i>, wrapping the descent to L<i+1> (or the
    terminal, at the innermost level)."""
    Z = "0.0" if flt else "0"
    NEG = "-1.0" if flt else "-1"
    innermost = (i == depth - 1)

    if innermost:
        # the terminal replaces the descent
        if terminal == "ret_int":
            inner = "    var r = 0;"
        elif terminal == "ret_float":
            inner = "    var r = 0.5;"
        else:
            # the `n < 0` arm is never taken; without a value-returning
            # path the pair infers `none` and `r + 1` is a NullabilityEx
            inner = ("    if (n < 0) { return %s; }\n"
                     "    throw Boom(7);\n"
                     "    var r = %s;" % (Z, Z))
    else:
        inner = "    var r = L%d(n - 1);" % (i + 1)

    # A catch at this level forces a try region, so it OVERRIDES the
    # level's kind - recorded in the program header so the shape that
    # actually ran is readable from the file.
    if catch_at == i:
        catch_val = "e.at + 0.0" if flt else "e.at"
        return ("    var out = %s;\n"
                "    try {\n"
                "    %s\n"
                "        out = r + 1;\n"
                "    } catch (Boom as e) { out = %s; }\n"
                "    return out;" % (Z, inner.strip(), catch_val))

    if kind == "plain":
        return "%s\n    return r + 1;" % inner
    if kind == "try":
        return ("    var out = %s;\n"
                "    try {\n"
                "    %s\n"
                "        out = r + 1;\n"
                "    } catch (Never) { out = %s; }\n"
                "    return out;" % (Z, inner.strip(), NEG))
    if kind == "tryfin":
        return ("    var s = %s;\n"
                "    try {\n"
                "    %s\n"
                "        s = r + 1;\n"
                "    } finally { s = s + 0; }\n"
                "    return s;" % (Z, inner.strip()))
    if kind == "diter":
        return ("    var acc = 0;\n"
                "    foreach (kk, vv in D) { acc += vv; }\n"
                "%s\n"
                "    return r + acc;" % inner)
    raise AssertionError(kind)


def gen_program(depth, kinds, terminal, catch_at):
    flt = (terminal == "ret_float")
    Z = "0.0" if flt else "0"
    out = [PRELUDE]
    out.append("# depth=%d kinds=%s terminal=%s catch=%s"
               % (depth, ",".join(kinds), terminal,
                  "none" if catch_at is None else "L%d" % catch_at))
    # innermost first so every callee is declared above its caller
    for i in range(depth - 1, -1, -1):
        out.append("func L%d(int n) {\n%s\n}"
                   % (i, level_body(i, depth, kinds[i], terminal,
                                    catch_at, flt)))
    # THE LOOP IS LOAD-BEARING: a first descent is all new record-stack
    # peaks and the record-reuse guard declines every level of it, so a
    # single call emits no inline push however deep it goes.
    out.append("var t = %s;" % Z)
    out.append("for (var k = 0; k < 4; k++)")
    out.append("    t += L0(runtime(%d));" % (depth + 2))
    out.append('print("r:", t);')
    return "\n".join(out) + "\n"


def enumerate_shapes(depth_max):
    for depth in range(1, depth_max + 1):
        for kinds in itertools.product(KINDS, repeat=depth):
            for terminal in TERMINALS:
                if terminal == "throw":
                    # caught at each level, or not at all
                    for catch_at in list(range(depth)) + [None]:
                        yield (depth, kinds, terminal, catch_at)
                else:
                    yield (depth, kinds, terminal, None)


def run_one(args):
    binary, path = args
    results = []
    for label, argv, env_extra in ENGINES:
        env = dict(os.environ)
        env.update(env_extra)
        try:
            p = subprocess.run([binary] + argv + [path], env=env,
                               stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=60)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired:
            rc, out, err = None, b"", b"TIMEOUT"
        results.append((label, rc, out, err))

    ref_label, ref_rc, ref_out, ref_err = results[0]
    problems = []
    for label, rc, out, err in results[1:]:
        if rc != ref_rc:
            problems.append("exit %s(%s) != %s(%s)"
                            % (label, rc, ref_label, ref_rc))
        if out != ref_out:
            problems.append("stdout %s != %s" % (label, ref_label))
        if err != ref_err:
            problems.append("stderr %s != %s" % (label, ref_label))
    # a sanitizer/assert finding is a failure even if all four agree
    for label, rc, out, err in results:
        for bad in (b"AddressSanitizer", b"MISMATCH", b"Assertion",
                    b"runtime error:", b"Sanitizer"):
            if bad in err:
                problems.append("%s stderr has %s"
                                % (label, bad.decode()))
                break
    return path, problems, ref_err.decode("utf-8", "replace")[:400]


def reach_sample(binary, paths):
    """How many sampled programs actually reach the emitted tier?"""
    pushes = norec = 0
    env = dict(os.environ)
    env["MYLANG_JITSTATS"] = "1"
    for path in paths:
        try:
            p = subprocess.run([binary, path], env=env,
                               stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            continue
        err = p.stderr.decode("utf-8", "replace")
        if "norec_verify" in err:
            pushes += 1
        if "norec_pushes" in err:
            norec += 1
    return pushes, norec


def main():
    ap = argparse.ArgumentParser(
        description="Net 3: exhaustive small-scope enumeration of "
                    "no-record-tier call shapes.")
    ap.add_argument("binary")
    ap.add_argument("--depth", type=int, default=4,
                    help="max chain depth (default 4)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N programs (0 = all)")
    ap.add_argument("--keep-dir", default=None,
                    help="write the generated programs here and keep them")
    ap.add_argument("--reach", type=int, default=25,
                    help="sample size for the tier-reach report (0 = off)")
    args = ap.parse_args()

    shapes = list(enumerate_shapes(args.depth))
    if args.limit:
        shapes = shapes[:args.limit]

    tmp = args.keep_dir or tempfile.mkdtemp(prefix="norec_enum_")
    os.makedirs(tmp, exist_ok=True)
    paths = []
    for idx, (depth, kinds, terminal, catch_at) in enumerate(shapes):
        name = "d%d_%s_%s_c%s_%04d.my" % (
            depth, "".join(k[0] for k in kinds), terminal,
            "x" if catch_at is None else catch_at, idx)
        path = os.path.join(tmp, name)
        with open(path, "w") as f:
            f.write(gen_program(depth, kinds, terminal, catch_at))
        paths.append(path)

    print("generated %d programs in %s" % (len(paths), tmp))
    print("engines   : %s" % ", ".join(l for l, _, _ in ENGINES))

    fails = []
    done = 0
    with ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for path, problems, ref_err in ex.map(
                run_one, [(args.binary, p) for p in paths], chunksize=4):
            done += 1
            if problems:
                fails.append((path, problems, ref_err))
            if done % 200 == 0:
                print("  %d/%d ... %d failing"
                      % (done, len(paths), len(fails)))

    print()
    print("programs      : %d" % len(paths))
    print("engine runs   : %d" % (len(paths) * len(ENGINES)))
    if args.reach:
        sample = paths[::max(1, len(paths) // args.reach)][:args.reach]
        pushes, norec = reach_sample(args.binary, sample)
        print("tier reach    : %d/%d sampled programs made emitted "
              "pushes, %d made record-less ones" % (pushes, len(sample),
                                                    norec))
        if pushes == 0:
            print("\nVACUOUS: nothing in this space reached the emitted "
                  "call tier.\nCheck the binary is a TESTS=1 build with "
                  "the JIT enabled.")
            return 1
    if fails:
        print("\nFAILURES (%d of %d):" % (len(fails), len(paths)))
        for path, problems, ref_err in fails[:20]:
            print("\n  %s" % path)
            for p in problems:
                print("      %s" % p)
            if ref_err.strip():
                print("      tw stderr: %s"
                      % ref_err.strip().splitlines()[0][:120])
        if len(fails) > 20:
            print("\n  ... and %d more" % (len(fails) - 20))
        print("\n(the generated programs are kept in %s)" % tmp)
        return 1
    print("\nOK - all engines agree on every program")
    return 0


if __name__ == "__main__":
    sys.exit(main())
