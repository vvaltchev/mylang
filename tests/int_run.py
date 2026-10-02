#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# THE INTRUSIVE-TEST RUNNER (#107, plans/intrusive-tests.md).
#
#   tests/int_run.py BINARY [--no-rt]
#
# 1. REFUSES a binary whose `mylang -v` does not say `int_tests 1`: every
#    check below would be vacuous on an ordinary build (the int_* builtins
#    would not exist, no site would record anything).
# 2. Runs `BINARY -rt` - the C++ half of the INT suite lives in the -rt
#    table, compiled in only under INT_TESTS.
# 3. Runs every tests/int/*.my under the default engine AND the tree-walker:
#    each must exit 0 (the programs self-assert) and the two must print the
#    same stdout.
# 4. THE SITE CENSUS: every process above appends its per-site event counts
#    to one file (MYLANG_INT_OUT, see src/inttest.cpp). A site of
#    src/intsites.h that NO run reached fails the run - "a hook nobody calls
#    is a hook that lies". Add a site together with the test that reaches it.
#
# Everything is deterministic: no seeds, no sampling, a fixed program order.
# The random-program fuzzers never run an INT binary (plan section 7).

import argparse
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def build_config(binary):
    out = subprocess.run([binary, "-v"], capture_output=True, text=True,
                         timeout=30).stdout
    cfg = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            cfg[parts[0]] = parts[1]
    return cfg


def run(cmd, env, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env,
                       timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--no-rt", action="store_true",
                    help="skip the -rt suite (iterating on tests/int only)")
    ap.add_argument("--timeout", type=float, default=1800.0)
    args = ap.parse_args()
    binary = os.path.abspath(args.binary)

    cfg = build_config(binary)
    if cfg.get("int_tests") != "1":
        print("int_run: %s is not an INT_TESTS build (-v says int_tests=%s);"
              " build it with `make INT_TESTS=1`" % (binary,
                                                     cfg.get("int_tests")),
              file=sys.stderr)
        return 2

    failures = []
    with tempfile.TemporaryDirectory(prefix="mylang-int-") as tmp:
        census = os.path.join(tmp, "census.txt")
        env = dict(os.environ, MYLANG_INT_OUT=census, TMPDIR=tmp)

        if not args.no_rt:
            rc, out, err = run([binary, "-rt"], env, args.timeout)
            tail = [l for l in out.splitlines()
                    if l.startswith(("Tests passed", "Differential"))]
            for l in tail:
                print("  -rt  " + l)
            if rc != 0:
                failures.append("-rt exited %d" % rc)

        progs = sorted(glob.glob(os.path.join(HERE, "int", "*.my")))
        if not progs:
            failures.append("no tests/int/*.my programs")
        for prog in progs:
            name = os.path.relpath(prog, os.path.dirname(HERE))
            rc_d, out_d, err_d = run([binary, prog], env, args.timeout)
            rc_t, out_t, err_t = run([binary, "-tw", prog], env, args.timeout)
            ok = rc_d == 0 and rc_t == 0 and out_d == out_t
            print("  %s  %s" % ("ok  " if ok else "FAIL", name))
            if not ok:
                failures.append(name)
                if rc_d != 0:
                    print("        default engine rc=%d\n%s" % (rc_d, err_d))
                if rc_t != 0:
                    print("        tree-walker rc=%d\n%s" % (rc_t, err_t))
                if out_d != out_t:
                    print("        stdout differs between the engines")

        totals = {}
        if os.path.exists(census):
            with open(census) as f:
                for line in f:
                    site, count = line.split()
                    totals[site] = totals.get(site, 0) + int(count)
        if not totals:
            failures.append("no census was written (MYLANG_INT_OUT)")
        unreached = sorted(s for s, c in totals.items() if c == 0)
        print("  census: %d site(s), %d unreached"
              % (len(totals), len(unreached)))
        for s in unreached:
            print("    UNREACHED  %s" % s)
            failures.append("site %s reached by no test" % s)

    if failures:
        print("int_run: FAIL (%d)" % len(failures))
        return 1
    print("int_run: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
