#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# THE DECISION ENUMERATOR's DRIVER (#107 P3, plans/intrusive-tests.md 4.2).
#
#   tests/int_enum.py BINARY [PROGRAM.my ...] [--programs-file F]
#                     [--jobs N] [--resume TOKEN] [--heartbeat S]
#                     [--failures-out FILE]
#
# Every heuristic with several LEGAL answers asks the INT core through
# int_choose and records an instance event (`reg_choice key=... n=...
# dflt=... pick=...`). Correctness must not depend on any of those
# answers, so this re-runs each program under the OTHER legal answers and
# requires the result the tree-walker gives - no seeds, no sampling:
#
#   TIER 1 - EVERY SINGLE DEVIATION. For each instance the default run
#   reached, one run per alternative value, with MYLANG_INT_CHOOSE=key=idx.
#   Complete for a bug that needs one decision to be different - the r9 pin
#   and the float-literal rcx clobber were both of that kind.
#
# Each deviation run must print the tree-walker's stdout, exit with its
# code, and ACTUALLY TAKE the deviation (its dump's `choose_applied` line:
# int_choose honoured the override, possibly in an emission attempt the JIT
# then discarded and redid - that redo is the deviation's effect) - a
# vacuous deviation fails too. A later instance may
# legitimately vanish under a deviation; only the deviated one must stay.
#
# LONG-RUNNING by nature (71,583 deviations over tests/functional), so it
# runs through tests/testrun.py: a heartbeat line every minute, a control
# socket (tests/testctl.py status | stop | jobs N ...) and RESUME - the
# plan is an ordered list, `--resume <mark>@<fp>` skips what a stopped run
# had finished, and a different binary or input set refuses the token.
#
# Programs: the arguments, else --programs-file (one path per line, `#`
# comments), else every tests/functional/*.my - the whole corpus, since on
# an `OPT=1 ASSERTS=1 INT_TESTS=1` build tier 1 over it is 68,379 runs in
# ~40 s (35x the debug+ASan lane; REGTRACK and the ML_CHECKs stay live,
# and the lane was watched catching the xmm0 clobbers). A coverage-chosen
# subset was planned and dropped: it would save seconds, and a default
# run's coverage does not measure what its deviations reach. Their output
# must depend only on the language: a tests/int program, which prints the
# JIT's own decisions, is not a subject. The random-program fuzzers never
# feed this (section 7).

import argparse
import concurrent.futures as cf
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from testrun import Run, fingerprint, file_digest  # noqa: E402

CHOICE = re.compile(r'^reg_choice key="([^"]+)" n=(\d+) dflt=(\d+) pick=(\d+)')
APPLIED = re.compile(r'^choose_applied key="([^"]+)" pick=(\d+)')


def jobs_count():
    try:
        out = subprocess.run([os.path.join(HERE, "jobs.sh"), "count"],
                             capture_output=True, text=True, timeout=30)
        return max(1, int(out.stdout.strip()))
    except (OSError, ValueError, subprocess.SubprocessError):
        return os.cpu_count() or 2


def run(binary, args, env, timeout):
    try:
        p = subprocess.run([binary] + args, capture_output=True, text=True,
                           env=env, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def choices(dump_path):
    out = {}
    try:
        with open(dump_path) as f:
            for line in f:
                m = CHOICE.match(line)
                if m:
                    out[m.group(1)] = (int(m.group(2)), int(m.group(3)),
                                       int(m.group(4)))
    except OSError:
        pass
    return out


def applied(dump_path):
    out = set()
    try:
        with open(dump_path) as f:
            for line in f:
                m = APPLIED.match(line)
                if m:
                    out.add((m.group(1), int(m.group(2))))
    except OSError:
        pass
    return out


def chosen_regs(dump_path):
    out = []
    try:
        with open(dump_path) as f:
            for line in f:
                if line.startswith("choose_reg "):
                    out.append(line.split(" reg=", 1)[1].strip())
    except OSError:
        pass
    return out


def read_list(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                out.append(os.path.join(ROOT, line))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("programs", nargs="*")
    ap.add_argument("--programs-file", default=None)
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--resume", default=None,
                    help="continue a stopped run from its <mark>@<fp> token")
    ap.add_argument("--heartbeat", type=float, default=60.0)
    ap.add_argument("--failures-out", default=None,
                    help="write EVERY failure here (stdout shows 40)")
    args = ap.parse_intermixed_args()
    binary = os.path.abspath(args.binary)

    v = subprocess.run([binary, "-v"], capture_output=True, text=True).stdout
    if not re.search(r"^  int_tests\s+1", v, re.M):
        print("int_enum: %s is not an INT_TESTS build" % binary,
              file=sys.stderr)
        return 2
    if args.programs:
        progs = [os.path.abspath(p) for p in args.programs]
    elif args.programs_file:
        progs = read_list(args.programs_file)
    else:
        progs = sorted(glob.glob(os.path.join(HERE, "functional", "*.my")))
    jobs = args.jobs or jobs_count()

    with tempfile.TemporaryDirectory(prefix="mylang-enum-") as tmp:
        base_env = dict(os.environ, TMPDIR=tmp)
        base_env.pop("MYLANG_INT_CHOOSE", None)

        # DISCOVERY: each program's tree-walker reference and the decision
        # instances its default run reaches (cheap - ~0.1 s a program)
        def discover(k_prog):
            k, prog = k_prog
            ref = run(binary, ["-tw", prog], base_env, args.timeout)
            dp = os.path.join(tmp, "default-%d.txt" % k)
            got = run(binary, [prog], dict(base_env, MYLANG_INT_DUMP=dp),
                      args.timeout)
            return ref, got, choices(dp)

        with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
            found = list(ex.map(discover, enumerate(progs)))

        failures = []
        refs = []
        items = []
        plan_text = []
        for k, (prog, (ref, got, inst)) in enumerate(zip(progs, found)):
            refs.append(ref)
            name = os.path.relpath(prog, ROOT)
            if got[:2] != ref[:2]:
                failures.append("%s: the DEFAULT run differs from the "
                                "tree-walker" % name)
                continue
            for key in sorted(inst):
                n, dflt, _ = inst[key]
                plan_text.append("%s %s %d %d" % (name, key, n, dflt))
                for alt in range(n):
                    if alt != dflt:
                        items.append((k, key, alt))
        fp = fingerprint(file_digest(binary), "\n".join(plan_text),
                         args.timeout)
        print("int_enum: %d program(s), %d deviation(s), plan %s"
              % (len(progs), len(items), fp), file=sys.stderr, flush=True)

        def work(i, item):
            k, key, alt = item
            dp = os.path.join(tmp, "d-%d.txt" % i)
            try:
                os.remove(dp)       # the dump is appended to
            except OSError:
                pass
            env = dict(base_env, MYLANG_INT_CHOOSE="%s=%d" % (key, alt),
                       MYLANG_INT_DUMP=dp)
            r = run(binary, [progs[k]], env, args.timeout)
            taken = (key, alt) in applied(dp)
            regs = chosen_regs(dp)
            try:
                os.remove(dp)
            except OSError:
                pass
            ref = refs[k]
            name = os.path.relpath(progs[k], ROOT)
            if r[:2] != ref[:2]:
                msg = "%s %s=%d%s: rc=%s%s" % (
                    name, key, alt,
                    " (reg %s)" % "/".join(regs) if regs else "", r[0],
                    "" if r[1] == ref[1] else ", stdout differs")
                if r[2]:
                    msg += " | " + r[2].strip().splitlines()[0][:150]
                return msg
            if not taken:
                return "%s %s=%d: the deviation was NOT taken (vacuous)" % (
                    name, key, alt)
            return None

        def describe(item):
            k, key, alt = item
            return "%s %s=%d" % (os.path.basename(progs[k]), key, alt)

        r = Run("int_enum", items, work, fp, jobs, resume=args.resume,
                heartbeat=args.heartbeat, describe=describe)
        failures += r.execute()

    if args.failures_out:
        with open(args.failures_out, "w") as f:
            for line in failures:
                f.write(line + "\n")
    for f in failures[:40]:
        print("  FAIL " + f)
    if len(failures) > 40:
        print("  ... and %d more" % (len(failures) - 40))
    if not r.complete:
        print("int_enum: STOPPED at %d/%d with %d failure(s) so far"
              % (r.mark, len(items), len(failures)))
        return 3
    print("int_enum: %d deviation run(s), %d failure(s)"
          % (len(items), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
