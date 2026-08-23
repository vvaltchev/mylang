#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
NET 4 - the COVERAGE GATE for the G1 no-record tier's walk and
reconstruction code (SQLite's 100%-coverage discipline, scoped).

    tests/norec_coverage.py --build-dir build-claude/gcov [--run]

WHY SCOPED.  Whole-program 100% coverage is not a goal here and never
will be.  What this gates is the code the no-record tier ADDED - the
chain walk, the frame reconstruction, the shadow verifiers and the
audits - because that code runs on rare paths (an unwind step, a
backtrace frame, a depth-cap switch) and a branch of it that no test
ever takes is a branch nobody has ever checked.  The tier is DEFAULT-ON,
so "never exercised" is not an acceptable state for any line of it.

WHY THE WORKLOAD MATTERS MORE THAN THE GATE.  A plain `./mylang -rt`
leaves `norec_walk_chain` at ZERO - it is gated on `!jit_norec_on()`,
i.e. it only runs in SHADOW mode, which -rt does not select.  The
default suite therefore never executes the project's primary shadow
oracle.  `--run` drives a workload that does: -rt, corpus_diff plain
AND --levers (which includes MYLANG_JIT_OFF=norec), the Net 3
enumeration and the Net 2 sweep, both of which run shadow and
production side by side.

EXEMPTIONS LIVE IN THE SOURCE, NOT IN THIS SCRIPT.  Mark a genuinely
unreachable line with a trailing comment:

    ML_VM_CHECK(...);   /* NOREC-COV-EXEMPT: plain_frame excludes X */

Keeping them next to the code means they cannot rot when line numbers
shift, and anyone editing the line sees the claim they have to keep
true.  An uncovered line or branch WITHOUT a marker fails the gate; a
marker that is no longer needed is reported as STALE so the list stays
honest.

THROW BRANCHES ARE EXCLUDED.  gcc emits an exception edge on every call
that could unwind; those are compiler artifacts, not program logic, and
counting them would make the target unreachable for reasons unrelated
to testing. `--with-throw` includes them for a look.

Stdlib only (gzip + json), GCC only - gcov's JSON output is what this
reads, and the GCOV build is already GCC-only.
"""

import argparse
import glob
import gzip
import json
import os
import re
import subprocess
import sys

MARKER = "NOREC-COV-EXEMPT"

# The tier's walk / reconstruction / verification surface, by DEMANGLED
# function name. Add a function here when you add one to that surface -
# the gate is only as good as this list, and a function missing from it
# is silently ungated.
SCOPE = {
    "vm.cpp": [
        "norec_walk_chain",            # Net 1's chain traversal
        "norec_recon_probe",           # Net 2's forced reconstruction
        "norec_materialize_shadow",    # the -3/switch materializer shadow
        "norec_check_rec",             # record <-> site association
        "jit_norec_push_verify",       # the per-push shadow verifier
        "jit_norec_retarm_verify",     # the record-less return arm oracle
        "jit_ret_audit",               # the return-time ref/watermark audit
        "jit_ret_norec",               # the record-less return
        "jit_norec_postexit",          # the level-up postexit stamp
        "norec_switch_retarget",       # the depth-cap switch retarget
        "norec_body_is_leaf",
        "norec_classify",
    ],
    "jit.cpp": [
        "jit_norec_site_for",          # return address -> site
        "jit_norec_frag_for",          # address -> owning fragment's chunk
        "jit_chunk_norec_ok",          # the callee gate
    ],
}


def bare_name(demangled):
    """`norec_walk_chain(VmActivation*, ...)` -> `norec_walk_chain`."""
    n = demangled.split("(")[0].strip()
    return n.split("::")[-1].split(" ")[-1]


def run_workload(binary, root, verbose):
    """Drive the paths the gate is about. Failures are REPORTED but do
    not stop coverage collection - a red test is a separate problem and
    the profile is still worth reading."""
    tests = os.path.join(root, "tests")
    steps = [
        ("-rt", [binary, "-rt"], {}),
        ("corpus_diff", [os.path.join(tests, "corpus_diff.sh"), binary], {}),
        ("corpus_diff --levers",
         [os.path.join(tests, "corpus_diff.sh"), binary, "--levers"], {}),
        # Net 3 and Net 2 both run SHADOW as well as production, which is
        # what reaches norec_walk_chain at all.
        ("norec_enum d2",
         [sys.executable, os.path.join(tests, "norec_enum.py"), binary,
          "--depth", "2", "--reach", "0"], {}),
        # deep enough to cross the sync depth cap, which is the ONLY
        # thing that drives the -3/switch materializer and its shadow
        ("deep switch (shadow)",
         [binary, os.path.join(root, "tests", "functional",
                               "12_deep_switch.my")],
         {"MYLANG_JIT_OFF": "norec"}),
        ("deep switch (production)",
         [binary, os.path.join(root, "tests", "functional",
                               "12_deep_switch.my")], {}),
        ("norec_sweep",
         [sys.executable, os.path.join(tests, "norec_sweep.py"), binary,
          "--max-events", "12"], {}),
    ]
    for label, argv, env_extra in steps:
        env = dict(os.environ)
        env.update(env_extra)
        try:
            p = subprocess.run(argv, env=env, stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=3600)
            print("  workload %-22s exit %s" % (label, p.returncode))
        except Exception as e:                       # noqa: BLE001
            print("  workload %-22s FAILED: %s" % (label, e))


def gcov_json(build_dir, src):
    """Run gcov on one TU and return its parsed JSON, or None."""
    pat = os.path.join(build_dir, "CMakeFiles", "mylang.dir", "src",
                       src + ".gcda")
    hits = glob.glob(pat)
    if not hits:
        return None
    d = os.path.dirname(hits[0])
    subprocess.run(["gcov", "--json-format", "-b", os.path.basename(hits[0])],
                   cwd=d, capture_output=True)
    out = os.path.join(d, src + ".gcov.json.gz")
    if not os.path.exists(out):
        return None
    with gzip.open(out) as f:
        return json.load(f)


def source_lines(root, src):
    path = os.path.join(root, "src", src)
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.readlines()
    except OSError:
        return []


def analyse(root, build_dir, src, wanted, with_throw):
    data = gcov_json(build_dir, src)
    if data is None:
        return None, ["no gcov data for %s (was the GCOV build run?)" % src]
    srclines = source_lines(root, src)

    # map every in-scope function to its line range
    ranges = []
    for f in data["files"]:
        if not f["file"].endswith(src):
            continue
        for fn in f.get("functions", []):
            nm = bare_name(fn.get("demangled_name") or fn.get("name", ""))
            if nm in wanted:
                ranges.append((fn["start_line"], fn["end_line"], nm))
        lines = f["lines"]
        break
    else:
        return None, ["%s not present in the gcov data" % src]

    def owner(n):
        for lo, hi, nm in ranges:
            if lo <= n <= hi:
                return nm
        return None

    stats = {nm: {"lines": 0, "lines_hit": 0, "br": 0, "br_hit": 0,
                  "miss": [], "stale": []} for _, _, nm in ranges}
    for ln in lines:
        nm = owner(ln["line_number"])
        if nm is None:
            continue
        s = stats[nm]
        n = ln["line_number"]
        text = srclines[n - 1].rstrip() if n <= len(srclines) else ""
        exempt = MARKER in text
        s["lines"] += 1
        if ln["count"] > 0:
            s["lines_hit"] += 1
            if exempt:
                s["stale"].append((n, "line is covered"))
        elif not exempt:
            s["miss"].append((n, "line never executed", text.strip()[:60]))
        for b in ln.get("branches", []):
            if b.get("throw") and not with_throw:
                continue
            s["br"] += 1
            if b["count"] > 0:
                s["br_hit"] += 1
            elif not exempt:
                s["miss"].append((n, "branch never taken",
                                  text.strip()[:60]))
    missing = [nm for nm in wanted
               if nm not in stats and nm not in (r[2] for r in ranges)]
    return stats, ["scoped function not found in %s: %s" % (src, nm)
                   for nm in missing]


def main():
    ap = argparse.ArgumentParser(
        description="Net 4: coverage gate over the no-record tier's walk "
                    "and reconstruction code.")
    ap.add_argument("--build-dir", required=True,
                    help="a CMake build configured with -DGCOV=1")
    ap.add_argument("--run", action="store_true",
                    help="drive the workload before measuring")
    ap.add_argument("--with-throw", action="store_true",
                    help="count gcc's exception edges too (noisy)")
    ap.add_argument("--gate", action="store_true",
                    help="exit non-zero unless every scoped line and "
                         "branch is covered or marked (the 100%% goal)")
    ap.add_argument("--min-lines", type=float, default=None,
                    help="RATCHET: fail if scoped line coverage drops "
                         "below this percentage")
    ap.add_argument("--min-branches", type=float, default=None,
                    help="RATCHET: fail if scoped branch coverage drops "
                         "below this percentage")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    binary = os.path.join(os.path.abspath(args.build_dir), "mylang")
    if not os.path.exists(binary):
        sys.exit("no binary at %s" % binary)
    if args.run:
        print("workload:")
        run_workload(binary, root, False)
        print()

    problems, stale_all = [], []
    tot_l = tot_lh = tot_b = tot_bh = 0
    for src, wanted in SCOPE.items():
        stats, errs = analyse(root, args.build_dir, src, set(wanted),
                              args.with_throw)
        problems += errs
        if stats is None:
            continue
        print("%s" % src)
        for nm in wanted:
            s = stats.get(nm)
            if s is None:
                print("  %-28s NOT FOUND" % nm)
                continue
            lp = 100.0 * s["lines_hit"] / s["lines"] if s["lines"] else 100.0
            bp = 100.0 * s["br_hit"] / s["br"] if s["br"] else 100.0
            flag = "" if not s["miss"] else "   <-- %d gap(s)" % len(s["miss"])
            print("  %-28s lines %3d/%-3d %5.1f%%   branches %3d/%-3d %5.1f%%%s"
                  % (nm, s["lines_hit"], s["lines"], lp,
                     s["br_hit"], s["br"], bp, flag))
            tot_l += s["lines"]; tot_lh += s["lines_hit"]
            tot_b += s["br"]; tot_bh += s["br_hit"]
            for n, why, text in s["miss"]:
                problems.append("%s:%d  %s (%s)  %s"
                                % (src, n, nm, why, text))
            for n, why in s["stale"]:
                stale_all.append("%s:%d  %s  STALE %s marker (%s)"
                                 % (src, n, nm, MARKER, why))
        print()

    print("scoped totals: lines %d/%d (%.1f%%), branches %d/%d (%.1f%%)"
          % (tot_lh, tot_l, 100.0 * tot_lh / tot_l if tot_l else 100.0,
             tot_bh, tot_b, 100.0 * tot_bh / tot_b if tot_b else 100.0))
    if stale_all:
        print("\nSTALE exemptions (%d) - the marker is no longer needed, "
              "remove it:" % len(stale_all))
        for s in stale_all:
            print("  %s" % s)
    # THE RATCHET. The 100%% goal (--gate) is not met yet - see
    # docs/jit-optimizations.md's Net 4 entry for what is left and why.
    # Until it is, CI pins the CURRENT floor so the surface can only
    # improve: a change that stops exercising the walk fails here even
    # though the absolute target is still ahead.
    lp = 100.0 * tot_lh / tot_l if tot_l else 100.0
    bp = 100.0 * tot_bh / tot_b if tot_b else 100.0
    ratchet_fail = False
    if args.min_lines is not None and lp < args.min_lines:
        print("\nRATCHET: line coverage %.1f%% < floor %.1f%%"
              % (lp, args.min_lines))
        ratchet_fail = True
    if args.min_branches is not None and bp < args.min_branches:
        print("\nRATCHET: branch coverage %.1f%% < floor %.1f%%"
              % (bp, args.min_branches))
        ratchet_fail = True
    if ratchet_fail:
        print("Coverage of the no-record walk/reconstruction surface "
              "REGRESSED.\nEither restore it, or lower the floor in the "
              "CI step with a reason.")
        return 1
    if problems:
        print("\nUNCOVERED and UNMARKED (%d):" % len(problems))
        for p in problems[:60]:
            print("  %s" % p)
        if len(problems) > 60:
            print("  ... and %d more" % (len(problems) - 60))
        print("\nEither cover it, or mark the line with a written reason:")
        print("    /* %s: why this cannot be reached */" % MARKER)
        return 1 if args.gate else 0
    print("\nOK - every scoped line and branch is covered or marked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
