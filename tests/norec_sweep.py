#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
NET 2 - the DETERMINISTIC EVENT SWEEP for the G1 no-record call tier.

    tests/norec_sweep.py BINARY [programs...] [--max-events N]

WHAT IT IS FOR.  The no-record tier does not write a VmCallRec for a
call it can rebuild later; the rebuild happens on the rare paths that
ask - an unwind step, a backtrace frame.  Those paths are therefore
exercised only where an exception HAPPENS to fall.  This sweep demands
a reconstruction at the Nth call event instead, one deterministic run
per N, walking N over a program's whole event count - so every point at
which reconstruction could ever be demanded gets checked, not just the
points a corpus reaches.  (SQLite's fail-the-Nth-malloc, applied to
frame reconstruction.)

The in-process half is `norec_recon_probe` (src/vm.cpp), reached by
`MYLANG_RECON_AT=N`.  It reconstructs each frame's parent view and the
cumulative slot-stack arithmetic from hardware + baked constants and
compares against the live records.  A disagreement aborts the run with
`NOREC SHADOW MISMATCH`; this driver turns that into a test failure.

TWO MODES, and both matter:
  shadow      MYLANG_JIT_OFF=norec - records are still written, so the
              reconstruction can be compared field-for-field.  This is
              the oracle.
  production  the shipping configuration - frames are genuinely
              record-less, so only the record-free half (termination,
              site resolution, RA agreement, ascending links) can be
              asserted.  Run anyway: it is the mode that ships, and it
              walks the real mixed chain.

THE PROBE MUST BE INVISIBLE.  Every sweep run's stdout and exit code
are compared against a baseline run with no probe, so a probe that
perturbs the program fails the sweep even when nothing mismatches.

Stdlib only, like the other fuzzers here (the project takes no
third-party dependencies, tests included).
"""

import argparse
import os
import subprocess
import sys

PROBE = "norec recon probe:"
# Anything on stderr that means the run went wrong rather than merely
# printing.  MISMATCH is the probe's own abort; the sanitizer strings
# catch a walk that reads somewhere it should not.
BAD = ("MISMATCH", "AddressSanitizer", "runtime error:", "Assertion",
       "SEGV", "Sanitizer")

MODES = {
    "shadow": {"MYLANG_JIT_OFF": "norec"},
    "production": {},
}


def run(binary, prog, env_extra, timeout):
    env = dict(os.environ)
    env.update(env_extra)
    # a stable locale + no history side effects; stdin closed so an
    # interactive sample terminates instead of blocking
    try:
        p = subprocess.run([binary, prog], env=env, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "", "TIMEOUT"
    return p.returncode, p.stdout.decode("utf-8", "replace"), \
        p.stderr.decode("utf-8", "replace")


def bad_stderr(err):
    return [m for m in BAD if m in err]


def sweep_one(binary, prog, mode, max_events, timeout, verbose):
    """Returns (events_probed, [failures], skip_reason_or_None)."""
    envm = MODES[mode]
    base_rc, base_out, base_err = run(binary, prog, envm, timeout)
    fails = []
    # A program we cannot get a clean baseline from is SKIPPED, not
    # failed: an interactive sample (phonebook, shopping) blocks or
    # loops on an empty stdin, which says nothing about reconstruction.
    # Skips are reported, never silent - a sweep that quietly skipped
    # everything would print a green zero.
    if base_rc is None:
        return 0, fails, "baseline timed out (interactive?)"
    hits = bad_stderr(base_err)
    if hits:
        return 0, fails, "baseline stderr has %s" % hits

    n = 0
    while n < max_events:
        n += 1
        rc, out, err = run(binary, prog, dict(envm, MYLANG_RECON_AT=str(n)),
                           timeout)
        if rc is None:
            fails.append("%s [%s] N=%d: TIMED OUT" % (prog, mode, n))
            break
        hits = bad_stderr(err)
        if hits:
            fails.append("%s [%s] N=%d: %s\n%s"
                         % (prog, mode, n, hits, err.strip()[:2000]))
            break
        if PROBE not in err:
            n -= 1          # this N had no event: the run is exhausted
            break
        # the probe is read-only - the program must be unchanged by it
        if rc != base_rc:
            fails.append("%s [%s] N=%d: exit %s != baseline %s"
                         % (prog, mode, n, rc, base_rc))
        if out != base_out:
            fails.append("%s [%s] N=%d: stdout differs from baseline"
                         % (prog, mode, n))
        if verbose:
            line = [l for l in err.splitlines() if PROBE in l]
            print("    %s" % (line[0] if line else "?"))
    return n, fails, None


def main():
    ap = argparse.ArgumentParser(
        description="Net 2: sweep forced frame reconstruction over every "
                    "call event of each program.")
    ap.add_argument("binary", help="a TESTS=1 build of mylang")
    ap.add_argument("programs", nargs="*",
                    help="scripts to sweep (default: tests/functional/*.my "
                         "+ samples/*)")
    ap.add_argument("--max-events", type=int, default=400,
                    help="cap the events swept per program (default 400); "
                         "a program with more is reported as CAPPED")
    ap.add_argument("--modes", default="shadow,production",
                    help="comma-separated: shadow, production")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    progs = args.programs
    if not progs:
        fdir = os.path.join(here, "functional")
        if os.path.isdir(fdir):
            progs += sorted(os.path.join(fdir, f)
                            for f in os.listdir(fdir) if f.endswith(".my"))
        sdir = os.path.join(root, "samples")
        if os.path.isdir(sdir):
            progs += sorted(os.path.join(sdir, f)
                            for f in os.listdir(sdir)
                            if not f.startswith("."))
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for m in modes:
        if m not in MODES:
            sys.exit("unknown mode %r (want: %s)"
                     % (m, ", ".join(sorted(MODES))))

    all_fails = []
    total_events = 0
    with_events = 0
    capped = []
    skipped = []
    for prog in progs:
        shown = False
        for mode in modes:
            n, fails, skip = sweep_one(args.binary, prog, mode,
                                       args.max_events, args.timeout,
                                       args.verbose)
            if skip:
                skipped.append("%s [%s]: %s"
                               % (os.path.relpath(prog, root), mode, skip))
            if n and not shown:
                print("  %s" % os.path.relpath(prog, root))
                shown = True
            if n:
                print("    %-11s %d events" % (mode, n))
                total_events += n
                if n >= args.max_events:
                    capped.append("%s [%s]" % (prog, mode))
            all_fails += fails
        if shown:
            with_events += 1

    print()
    print("programs swept      : %d" % len(progs))
    if skipped:
        print("skipped (no usable baseline):")
        for sk in skipped:
            print("  %s" % sk)
    # A sweep over programs that make no emitted calls proves nothing;
    # say so loudly rather than printing a green zero.
    print("with call events    : %d" % with_events)
    print("events forced       : %d" % total_events)
    if capped:
        print("CAPPED at --max-events (not swept to the end):")
        for c in capped:
            print("  %s" % c)
    if with_events == 0:
        print("\nVACUOUS: no program produced a single call event, so "
              "nothing was reconstructed.\nCheck the binary is a TESTS=1 "
              "build with the JIT enabled.")
        return 1
    if all_fails:
        print("\nFAILURES (%d):" % len(all_fails))
        for f in all_fails:
            print("  %s" % f)
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
