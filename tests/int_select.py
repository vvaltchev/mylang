#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# SELECTION AND REDUCTION (#107 P8, plans/intrusive-tests.md section 9).
#
#   tests/int_select.py GCOV_INT_BINARY [--configs default,-nj,...]
#                       [--programs-file F] [--with-rt] [--jobs N]
#                       [--out FILE.json] [--shrink NAME[,NAME...]]
#                       [--shrink-dir DIR]
#                       [--heartbeat S] [--resume TOKEN]
#
# OFFLINE tooling, not CI. It answers "which tests are worth running?"
# with facts instead of guesses, because a test's coverage is a FACT
# here: the build is deterministic, so one run measures it for good.
#
#   1. EXPLORATION. Every CANDIDATE - a program x a configuration (an
#      engine flag and/or KEY=VALUE environment) - runs ONCE on a GCOV
#      INT build and gets its coverage vector: the branch outcomes and
#      MC/DC conditions of src/ (int_run.py's universe, so INT helpers
#      and tests.cpp are outside it) plus the int sites it READ. Each run
#      is also an ORACLE run: its stdout and exit code must equal the
#      tree-walker's on the same program, or the candidate is EXCLUDED
#      (and reported) - covering code is worth nothing if the answer is
#      wrong. Candidates run in parallel: each writes its counters under
#      its own GCOV_PREFIX tree, so no two runs share a .gcda.
#   2. SELECTION. An IRREDUNDANT cover of everything any candidate
#      reached: greedy (most new elements, then the cheaper run, then
#      the name - deterministic), then a removal pass dropping any
#      selected test whose elements the others all cover. Globally
#      minimal is set cover, NP-hard, and is not claimed; irredundant is
#      claimed and CHECKED - every selected test OWNS an element no other
#      selected test covers.
#   3. THE OWNERSHIP TABLE: each selected test, its cost and what it
#      owns - the reviewable answer to "why does this test exist?".
#   4. SHRINK (--shrink): a selected .my program reduced by line-level
#      delta debugging (ddmin) while it still (a) prints what the
#      tree-walker prints, ending with the same exit code, and (b)
#      covers every element it owns. The result goes to --shrink-dir
#      (default: the temp dir) and its path is printed; nothing in the
#      tree is changed.
#
# Candidates by default: tests/functional/*.my, the non-interactive
# samples, tests/bt_oracle/*.my (whose ERRORS are the output), each under
# every --configs entry (default: the default engine, -nj, -tw,
# MYLANG_JIT_OFF=lsra and MYLANG_NO_LOWMEM=1). --with-rt adds `-rt` as
# one candidate (minutes on a debug build). Runs through
# tests/testrun.py: heartbeat, control socket, resume.

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import int_run  # noqa: E402
from testrun import Run, fingerprint, file_digest  # noqa: E402

SAMPLES = ["fib", "gcd", "loop", "primes", "primes2", "strloop"]
DEFAULT_CONFIGS = ["default", "-nj", "-tw", "MYLANG_JIT_OFF=lsra",
                   "MYLANG_NO_LOWMEM=1"]


def jobs_count():
    try:
        out = subprocess.run([os.path.join(HERE, "jobs.sh"), "count"],
                             capture_output=True, text=True, timeout=30)
        return max(1, int(out.stdout.strip()))
    except (OSError, ValueError, subprocess.SubprocessError):
        return os.cpu_count() or 2


def parse_config(spec):
    """`default`, an engine flag (`-nj`), or space-separated KEY=VALUE
    settings, optionally with flags among them - (flags, env)."""
    flags, env = [], {}
    if spec != "default":
        for part in spec.split():
            if part.startswith("-"):
                flags.append(part)
            else:
                k, _, v = part.partition("=")
                env[k] = v
    return flags, env


class Candidate:
    def __init__(self, prog, config):
        self.prog = prog            # a path, or "-rt"
        self.config = config
        self.flags, self.env = parse_config(config)

    @property
    def name(self):
        p = self.prog if self.prog == "-rt" else os.path.relpath(self.prog,
                                                                 ROOT)
        return "%s [%s]" % (p, self.config)

    def argv(self, binary, prog=None):
        if self.prog == "-rt":
            return [binary, "-rt"]
        return [binary] + self.flags + [prog or self.prog]


def mirror_build(build_dir, prefix):
    """The object directory as GCOV_PREFIX leaves it for one run: the
    run's .gcda files sit under prefix + the absolute build path, and
    gcov needs each object and its notes beside them - symlinked."""
    d = os.path.join(prefix, build_dir.lstrip("/"))
    os.makedirs(d, exist_ok=True)
    for f in glob.glob(os.path.join(build_dir, "*.o")) + \
            glob.glob(os.path.join(build_dir, "*.gcno")):
        dst = os.path.join(d, os.path.basename(f))
        if not os.path.exists(dst):
            os.symlink(f, dst)
    return d


class Explorer:
    """Runs candidates and measures their coverage (see the header)."""

    def __init__(self, binary, tmp, timeout):
        self.binary = binary
        self.build_dir = os.path.dirname(binary)
        self.tmp = tmp
        self.timeout = timeout
        self.tool = int_run.gcov_tool(int_run.build_config(binary))
        self.ex_cache = {}
        self.refs = {}              # program -> tree-walker (rc, stdout)

    def reference(self, prog):
        if prog not in self.refs:
            r = int_run.run([self.binary, "-tw", prog], dict(os.environ),
                            self.timeout)
            self.refs[prog] = (r[0], r[1])
        return self.refs[prog]

    def measure(self, cand, key, prog=None):
        """(ok, elements-covered set, seconds, why). `prog` overrides the
        candidate's program (a shrink trial)."""
        prefix = os.path.join(self.tmp, "p-%s" % key)
        shutil.rmtree(prefix, ignore_errors=True)
        os.makedirs(prefix)
        out = os.path.join(prefix, "int-out.txt")
        env = dict(os.environ, GCOV_PREFIX=prefix, GCOV_PREFIX_STRIP="0",
                   MYLANG_INT_OUT=out, TMPDIR=prefix, **cand.env)
        t0 = time.time()
        try:
            p = subprocess.run(cand.argv(self.binary, prog),
                               capture_output=True, text=True, env=env,
                               timeout=self.timeout,
                               stdin=subprocess.DEVNULL)
            rc, stdout = p.returncode, p.stdout
        except subprocess.TimeoutExpired:
            return False, set(), self.timeout, "timeout"
        secs = time.time() - t0
        why = None
        if cand.prog == "-rt":
            if rc != 0:
                why = "-rt failed (rc %d)" % rc
        elif cand.flags != ["-tw"]:
            ref = self.reference(prog or cand.prog)
            if (rc, stdout) != ref:
                why = "output differs from the tree-walker"
        objdir = mirror_build(self.build_dir, prefix)
        scratch = os.path.join(prefix, "gcov")
        os.makedirs(scratch, exist_ok=True)
        elems = int_run.collect(objdir, scratch, self.ex_cache, self.tool)
        cov = {e for e, c in elems.items() if c}
        if os.path.exists(out):
            with open(out) as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 3 and int(parts[2]) > 0:
                        cov.add("site:" + parts[0])
        shutil.rmtree(prefix, ignore_errors=True)
        return why is None, cov, secs, why


def select(cover, cost):
    """Greedy + removal (see the header); returns the selected names in
    selection order and each one's OWNED set."""
    remaining = set().union(*cover.values()) if cover else set()
    chosen = []
    while remaining:
        best = min(cover, key=lambda n: (-len(cover[n] & remaining),
                                         cost[n], n))
        gain = cover[best] & remaining
        if not gain:
            break
        chosen.append(best)
        remaining -= gain
    # removal: latest first, a test the rest already cover goes
    for n in reversed(list(chosen)):
        others = set().union(*(cover[m] for m in chosen if m != n))
        if cover[n] <= others:
            chosen.remove(n)
    owned = {}
    for n in chosen:
        others = set().union(*(cover[m] for m in chosen if m != n))
        owned[n] = cover[n] - others
    return chosen, owned


def ddmin(lines, keep):
    """Zeller's ddmin over LINES: the smallest subsequence (1-minimal)
    for which keep(subsequence) holds."""
    n = 2
    while len(lines) >= 2:
        chunk = max(1, len(lines) // n)
        parts = [lines[i:i + chunk] for i in range(0, len(lines), chunk)]
        reduced = False
        for i in range(len(parts)):
            comp = [l for j, p in enumerate(parts) if j != i for l in p]
            if comp and keep(comp):
                lines = comp
                n = max(n - 1, 2)
                reduced = True
                break
        if not reduced:
            if n >= len(lines):
                break
            n = min(len(lines), n * 2)
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--configs", default=",".join(DEFAULT_CONFIGS))
    ap.add_argument("--programs-file", default=None)
    ap.add_argument("--with-rt", action="store_true")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--out", default=None, help="write the result as JSON")
    ap.add_argument("--shrink", default="",
                    help="comma-separated selected test names to reduce")
    ap.add_argument("--shrink-dir", default=tempfile.gettempdir(),
                    help="where a shrunk program is written")
    ap.add_argument("--heartbeat", type=float, default=60.0)
    ap.add_argument("--resume", default=None)
    args = ap.parse_intermixed_args()
    binary = os.path.abspath(args.binary)

    cfg = int_run.build_config(binary)
    if cfg.get("int_tests") != "1" or not glob.glob(
            os.path.join(os.path.dirname(binary), "*.gcno")):
        print("int_select: %s is not a GCOV INT_TESTS build (make GCOV=1 "
              "INT_TESTS=1 TESTS=1 ...)" % binary, file=sys.stderr)
        return 2

    if args.programs_file:
        progs = int_run_list(args.programs_file)
    else:
        progs = sorted(glob.glob(os.path.join(HERE, "functional", "*.my")))
        progs += [os.path.join(ROOT, "samples", s) for s in SAMPLES]
        progs += sorted(glob.glob(os.path.join(HERE, "bt_oracle", "*.my")))
    configs = [c.strip() for c in args.configs.split(",") if c.strip()]
    cands = [Candidate(p, c) for p in progs for c in configs]
    if args.with_rt:
        cands.append(Candidate("-rt", "default"))

    jobs = args.jobs or jobs_count()
    with tempfile.TemporaryDirectory(prefix="mylang-select-") as tmp:
        ex = Explorer(binary, tmp, args.timeout)
        for p in progs:                 # the oracle's runs, up front
            ex.reference(p)
        results = {}

        def work(i, cand):
            ok, cov, secs, why = ex.measure(cand, str(i))
            results[cand.name] = (ok, cov, secs, why)
            return None if ok else "%s: %s" % (cand.name, why)

        fp = fingerprint(file_digest(binary),
                         "\n".join(c.name for c in cands))
        r = Run("int_select", cands, work, fp, jobs, resume=args.resume,
                heartbeat=args.heartbeat, describe=lambda c: c.name)
        excluded = r.execute()
        if not r.complete:
            print("int_select: STOPPED - resume with --resume %s" %
                  r.token())
            return 3

        # THE VACUITY GUARD (int_run's): a pass that measured no branch
        # at all is a broken gcov, not a clean result
        if not any(e.startswith(("br:", "mcdc:"))
                   for v in results.values() for e in v[1]):
            print("int_select: gcov reported no src/ branch for any "
                  "candidate - the pass measured nothing", file=sys.stderr)
            return 2
        cover = {n: v[1] for n, v in results.items() if v[0]}
        cost = {n: v[2] for n, v in results.items() if v[0]}
        universe = set().union(*cover.values()) if cover else set()
        chosen, owned = select(cover, cost)

        print("int_select: %d candidate(s), %d excluded (wrong answer), "
              "%d element(s) reached" % (len(cands), len(excluded),
                                         len(universe)))
        for e in excluded:
            print("  EXCLUDED  " + e)
        print("int_select: an irredundant cover of %d test(s) (%.1f s of "
              "runs; all %d candidates: %.1f s)"
              % (len(chosen), sum(cost[n] for n in chosen), len(cover),
                 sum(cost.values())))
        print("  %7s %6s  %s" % ("owns", "secs", "test"))
        for n in sorted(chosen, key=lambda m: (-len(owned[m]), m)):
            print("  %7d %6.2f  %s" % (len(owned[n]), cost[n], n))
        bad = [n for n in chosen if not owned[n]]
        if bad:                         # cannot happen after the removal
            print("int_select: NOT irredundant: %s" % ", ".join(bad))
            return 1

        shrunk = {}
        for name in [s.strip() for s in args.shrink.split(",")
                     if s.strip()]:
            if name not in owned:
                print("int_select: --shrink %s: not a selected test" % name)
                continue
            cand = next(c for c in cands if c.name == name)
            if cand.prog == "-rt":
                print("int_select: --shrink: -rt is not a program")
                continue
            with open(cand.prog) as f:
                lines = f.read().split("\n")
            want = owned[name]
            want_rc = ex.reference(cand.prog)[0]
            trials = [0]

            def keep(sub, cand=cand, want=want, want_rc=want_rc):
                # the reduced program must still end as the original does
                # (a compile error agrees with itself in every engine),
                # agree with the tree-walker, and cover what it owns
                trials[0] += 1
                path = os.path.join(tmp, "shrink-%d.my" % trials[0])
                with open(path, "w") as f:
                    f.write("\n".join(sub))
                ex.refs.pop(path, None)
                if ex.reference(path)[0] != want_rc:
                    return False
                ok, cov, _s, _w = ex.measure(cand, "s%d" % trials[0], path)
                return ok and want <= cov

            small = ddmin(lines, keep)
            dst = os.path.join(args.shrink_dir, "int-select-%s"
                               % re.sub(r"[^A-Za-z0-9_.]+", "_",
                                        os.path.basename(cand.prog)))
            with open(dst, "w") as f:
                f.write("\n".join(small))
            shrunk[name] = dst
            print("int_select: shrank %s: %d -> %d line(s) in %d trial(s), "
                  "still owning %d element(s): %s"
                  % (name, len(lines), len(small), trials[0], len(want),
                     dst))

        if args.out:
            with open(args.out, "w") as f:
                json.dump({"binary": binary, "candidates": len(cands),
                           "excluded": excluded,
                           "reached": len(universe),
                           "selected": [{"test": n, "secs": cost[n],
                                         "owns": len(owned[n])}
                                        for n in chosen],
                           "shrunk": shrunk}, f, indent=1)
    return 0


def int_run_list(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                out.append(os.path.join(ROOT, line))
    return out


if __name__ == "__main__":
    sys.exit(main())
