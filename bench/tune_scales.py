#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# Fine-tune bench/scales.txt so each benchmark SETTLES within run.py's adaptive
# 3->5->8-rep variance gate at its table scale (so a normal run neither aborts
# nor keeps escalating to 8 reps on it). It runs each bench one at a time
# through run.py's OWN gate (imported - the tuner and runner agree exactly on
# "stable"); a bench that already settles needs NO change, and one that does NOT
# settle even at 8 reps has its scale DELICATELY bumped (a factor sized by how
# far over threshold, clamped to 1.5x..2x, never a wild 10x), re-measured, and
# repeated until it settles or a safety cap is hit. Scales are only ever RAISED,
# never lowered. The updated table is written back to bench/scales.txt (LEFT
# UNCOMMITTED for review); benches that cannot be stabilized within the cap are
# reported, not silently dropped.
#
# ⛔⛔ IT GATES THE COMPARISON SIDE TOO, AND THAT IS NOT OPTIONAL (2026-08-20).
#
# The headline figure is a RATIO - my/cpp, my/python - so its error is the
# error of BOTH sides. This tuner used to raise a scale until MYLANG settled
# and never look at the denominator at all. The consequence was a false
# regression report: 76_funcval_dispatch printed 15.26x against a recorded
# 10.68x, and re-timing the SAME binaries at four scales in one sitting gave
# 9.84x .. 13.02x. MyLang settled fine; the C++ side was 1.25 ms, and a 1 ms
# process time is not a measurement.
#
# It was systemic, not one bench: 53 of 83 cached C++ results were under 20 ms
# and 27 were under 5 ms - and the smallest were exactly the benches at the top
# of the my/cpp ladder (30_str_index_iterate, 63_closures, 76_funcval_dispatch,
# 11_closure_counter, 64_struct_create). The whole ladder rested on ~1 ms
# denominators.
#
# So a scale must ALSO make the comparison run long enough that process
# startup is negligible. MIN_CMP_SECONDS is derived, not guessed - see its
# comment. The gate is applied to the FASTEST comparison language available
# (C++), because one scale serves them all and the fastest one is the binding
# constraint; python is slower by a wide margin and comes along for free.
#
# ⛔ A VARIANCE GATE ALONE CANNOT SUBSTITUTE. A 1.25 ms run can be perfectly
# repeatable - 1.25, 1.25, 1.26 - and still be 30% process startup. Low
# variance says the number is STABLE, not that it measures the LOOP. Both
# checks are needed and they answer different questions.
#
# Usage:
#   python3 bench/tune_scales.py                    # tune all, write scales.txt
#   python3 bench/tune_scales.py --filter dict      # only some benches
#   python3 bench/tune_scales.py --dry-run          # report, don't write
#   python3 bench/tune_scales.py --mylang build-rel/mylang
#   python3 bench/tune_scales.py --complang python  # gate that side instead
#   python3 bench/tune_scales.py --min-cmp-ms 0     # MyLang-only (the old
#                                                   # behaviour; not advised)
#
# AFTER TUNING, THE COMPARISON CACHES ARE STALE BY CONSTRUCTION (they are keyed
# by scale), so rebuild them:
#   python3 bench/run.py -cl cpp --recompute
# A recompute that covers EVERY bench re-stamps the machine-speed marker; a
# partial one deliberately does not.

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run  # noqa: E402  (bench/run.py - shared timing + variance)

# The safety cap on a single bench's scale. Raised from 64 to 256 when the
# comparison-side floor arrived: 64 was sized for a VARIANCE-only tuner, where
# a bench needing more than ~2 doublings was a noise problem to investigate
# rather than a scale to raise. The floor is a different kind of demand - it is
# arithmetic, and a bench whose C++ side is 1 ms genuinely needs ~20x - so the
# old cap refused two benches (30_str_index_iterate, 46_matrix_mult) that are
# simply short, not noisy.
MAX_SCALE = 256
MAX_ITERS = 6           # bump attempts per bench before giving up

# ⛔ THE COMPARISON-SIDE FLOOR, DERIVED FROM THE MEASURED PROCESS COST.
# An empty `int main(){}` compiled -O3 costs 0.31 ms best / 0.38 ms median to
# fork+exec+exit on this class of box (measured 2026-08-20, 40 reps). For that
# fixed cost to be under 2% of a timing - i.e. for the number to be about the
# LOOP rather than about process startup - the run must last ~19 ms. 20 ms is
# that, rounded.
#
# Raise it for a tighter ratio (37 ms puts startup under 1%); set it to 0 with
# --min-cmp-ms 0 to get the old MyLang-only behaviour, which is what produced a
# ladder built on 1 ms denominators.
MIN_CMP_SECONDS = 0.020
DEFAULT_MIN_CMP_MS = MIN_CMP_SECONDS * 1000


def settle_target(base_reps):
    """Tune with MARGIN: require the bench to settle within ONE escalation step
    of its baseline (baseline + REP_STEPS[1]), NOT the full 3->5->8. A bench
    that needs the last step is borderline - a slightly noisier run.py
    invocation would then ABORT it. Settling one step in gives run.py headroom.
    For the default baseline 3 this is 5 (unchanged); a reps.txt baseline of B
    gives B + REP_STEPS[1]."""
    return base_reps + run.REP_STEPS[1]


def measure(mylang, my_path, scale, threshold, timeout, min_reps):
    """Does the bench SETTLE within run.py's OWN adaptive gate at `scale`
    (started at this bench's `min_reps` baseline)? Returns (settled_bool,
    variance, n_reps, error_or_None). We use the exact gate run.py uses so a
    scale the tuner accepts is one run.py won't abort on - and a bench that
    settles at its baseline needs no bump at all."""
    _best, _out, err, n_reps, v = run.measure_adaptive(
        [mylang, my_path, str(scale)], threshold, timeout, min_reps=min_reps)
    if err and not err.startswith("variance"):
        return (False, None, n_reps, err)     # a real run error
    return (err is None, v, n_reps, None)


def measure_cmp(lang, bench, scale, threshold, timeout):
    """Time the COMPARISON side at `scale` through run.py's own adaptive gate.

    Returns (best_seconds, settled_bool, error_or_None); (None, False, err)
    when the language has no source for this bench or the build/run failed."""
    if lang is None or not lang.has(bench):
        return (None, False, None)        # MyLang-only bench: nothing to gate
    prefix, err = lang.prepare(bench)
    if err:
        return (None, False, err)
    best, _out, err, _n, _v = run.measure_adaptive(
        prefix + [str(scale)], threshold, timeout)
    if err and not err.startswith("variance"):
        return (None, False, err)
    return (best, err is None, None)


def bump(scale, variance, threshold, cmp_time=None):
    """The delicately-sized next scale.

    TWO REASONS TO BUMP, and they are sized DIFFERENTLY on purpose:

    (1) VARIANCE (noisy run). The fixed-overhead noise falls ~linearly with
        workload, so factor ~= variance/threshold is about what is needed -
        but variance is itself a noisy estimate, so we clamp to [1.5x, 2x]
        (the benches are almost stable; a wild jump would waste time).

    (2) THE COMPARISON FLOOR (run too short to mean anything). This is NOT a
        noise estimate - it is arithmetic. Time scales linearly with `scale`,
        so reaching MIN_CMP_SECONDS from `cmp_time` needs exactly
        MIN_CMP_SECONDS/cmp_time, and clamping that to 2x would take six
        doublings to cross a 25x gap - more than MAX_ITERS, so the bench would
        be reported as "did not converge" when the answer was known in one
        step. So this factor is applied DIRECTLY, with 15% headroom.

        (Linearity is what the `-npc` default buys: with the pure-call cache
        ON, 09_fib_recursive costs the same at scale 1 and scale 3 because
        iterations 2..N are cache hits. run.py passes -npc, so scale is linear
        again - see CLAUDE.md's note on the invalid 0.09x reading.)

    Whichever demands more wins; always advance at least +1 (integer scale)."""
    factor = min(2.0, max(1.5, variance / threshold))
    if cmp_time is not None and cmp_time > 0 and cmp_time < MIN_CMP_SECONDS:
        factor = max(factor, 1.15 * MIN_CMP_SECONDS / cmp_time)
    nxt = math.ceil(scale * factor)
    return max(nxt, scale + 1)


def write_scales(table, path):
    """Rewrite bench/scales.txt with every bench (sorted), keeping the header."""
    names = sorted(f[:-3] for f in os.listdir(run.MY_DIR) if f.endswith(".my"))
    with open(path, "w") as f:
        f.write("# Per-bench workload scale for bench/run.py (name scale).\n")
        f.write("# A bench absent here runs at scale 1. Raise a bench's scale "
                "to make a\n")
        f.write("# too-short/noisy bench reliable; bench/tune_scales.py "
                "fine-tunes this.\n")
        f.write("# Regenerate/adjust with: python3 bench/tune_scales.py\n\n")
        for n in names:
            f.write("%-24s %d\n" % (n, table.get(n, run.DEFAULT_SCALE)))


def main():
    ap = argparse.ArgumentParser(
        description="Fine-tune bench/scales.txt to keep each bench stable")
    ap.add_argument("--mylang", default="")
    ap.add_argument("--filter", default="")
    ap.add_argument("--var-threshold", type=float, default=0.05,
                    help="target (2nd-min - min)/min - the same gate run.py "
                         "uses (default 0.05 = 5%%; a pre-pooled-allocator "
                         "floor, tighten after the pooled-alloc TODO)")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--complang", "-cl", default="cpp",
                    help="which comparison language's runtime to ALSO gate "
                         "(default cpp - the fastest, so the binding one; "
                         "one scale serves them all)")
    ap.add_argument("--min-cmp-ms", type=float, default=DEFAULT_MIN_CMP_MS,
                    help="the comparison run must last at least this long, so "
                         "process startup is a negligible share of it "
                         "(default %(default).0f ms; 0 disables the gate and "
                         "restores the MyLang-only behaviour)")
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--dry-run", action="store_true",
                    help="report proposed bumps but do not write scales.txt")
    args = ap.parse_args()
    global MIN_CMP_SECONDS
    MIN_CMP_SECONDS = args.min_cmp_ms / 1000.0

    mylang = run.find_mylang(args.mylang)
    if not mylang:
        sys.exit("error: mylang binary not found; build it or pass --mylang")
    prio = run.raise_priority()   # best-effort less-noise (children inherit)
    if prio is None:
        print("note: could not raise priority (run with sudo / cap_sys_nice "
              "for a less noisy tune)\n")
    warn = run.optimization_warning(mylang)
    if warn:
        print(warn + "\n")

    names = sorted(f[:-3] for f in os.listdir(run.MY_DIR) if f.endswith(".my"))
    if args.filter:
        subs = [s for s in args.filter.split(",") if s]
        names = [n for n in names if any(s in n for s in subs)]

    table = run.load_scales()
    reps_table = run.load_reps()     # per-bench reps baselines (scale-immune)
    thr = args.var_threshold
    lang = None
    if MIN_CMP_SECONDS > 0:
        langs = run.build_complangs(sys.executable, args.cxx)
        lang = langs.get(args.complang)
        if lang is None:
            sys.exit("error: unknown comparison language %r (have: %s)"
                     % (args.complang, ", ".join(sorted(langs))))
    print("tuning %d benches with %s (run.py's adaptive gate, "
          "target var<=%.1f%%)" % (len(names), mylang, thr * 100))
    if lang is not None:
        print("comparison side GATED: %s must run >= %.0f ms "
              "(so process startup is a negligible share of the RATIO)"
              % (lang.name, MIN_CMP_SECONDS * 1000))
    else:
        print("comparison side NOT gated (--min-cmp-ms 0) - the ratios this "
              "produces can be dominated by process startup")
    print()

    changed = []     # (name, old, new, final_var)
    failed = []      # (name, reason)
    for name in names:
        my_path = os.path.join(run.MY_DIR, name + ".my")
        start_scale = table.get(name, run.DEFAULT_SCALE)
        # A reps.txt bench starts its gate at that baseline; accept it settling
        # within one escalation step OF that baseline (so we don't chase a
        # scale bump on a bench the reps baseline already handles).
        base_reps = run.resolve_reps(name, reps_table)
        target = settle_target(base_reps)
        scale = start_scale
        last_v = None
        last_reps = 0
        last_cmp = None
        for _ in range(MAX_ITERS):
            settled, v, n_reps, err = measure(mylang, my_path, scale, thr,
                                              args.timeout, base_reps)
            if err:
                failed.append((name, "run error: %s" % err))
                break
            last_v = v
            last_reps = n_reps
            # THE DENOMINATOR. A bench with no source in this language is
            # MyLang-only and has no ratio to protect, so it is not gated.
            cmp_t, cmp_settled, cmp_err = measure_cmp(
                lang, name, scale, thr, args.timeout)
            if cmp_err:
                failed.append((name, "%s: %s" % (lang.name, cmp_err)))
                break
            last_cmp = cmp_t
            cmp_ok = (cmp_t is None
                      or (cmp_t >= MIN_CMP_SECONDS and cmp_settled))
            if settled and n_reps <= target and cmp_ok:
                if scale != start_scale:
                    changed.append((name, start_scale, scale, v))
                table[name] = scale
                break
            nxt = bump(scale, v, thr, cmp_t)
            # ⛔ TRY THE CAP, DO NOT REFUSE A PREDICTION. `bump` EXTRAPOLATES
            # (linear in scale); the cap is a fact. Giving up because the
            # prediction overshoots reports "could not stabilize" for a bench
            # the cap might well handle - and it did, for the two benches
            # whose work is superlinear-ish in wall time. Measure the cap
            # instead, and only then report failure.
            if nxt > MAX_SCALE and scale < MAX_SCALE:
                nxt = MAX_SCALE
            if nxt > MAX_SCALE:
                why = "var %.1f%%" % (v * 100)
                if last_cmp is not None and last_cmp < MIN_CMP_SECONDS:
                    why = "%s only %.1f ms (need %.0f)" % (
                        lang.name, last_cmp * 1000, MIN_CMP_SECONDS * 1000)
                failed.append((name, "%s at scale %d, next %d > cap %d"
                               % (why, scale, nxt, MAX_SCALE)))
                table[name] = scale
                break
            scale = nxt
        else:
            failed.append((name, "did not converge (last var %.1f%%)"
                           % ((last_v or 0) * 100)))
        marker = ("bumped %d->%d" % (start_scale, table.get(name, start_scale))
                  if table.get(name, start_scale) != start_scale else "ok")
        cmp_s = ("%s %6.1fms" % (lang.name, last_cmp * 1000)
                 if last_cmp is not None else "no cmp    ")
        print("  %-24s scale=%-3d var=%5.1f%% (%dr)  %s  %s"
              % (name, table.get(name, start_scale),
                 (last_v or 0) * 100, last_reps, cmp_s, marker))

    print()
    if changed:
        print("BUMPED %d bench(es):" % len(changed))
        for n, o, nw, v in changed:
            print("  %-24s %d -> %d  (now var %.1f%%)" % (n, o, nw, v * 100))
    else:
        print("no scale changes needed.")
    if failed:
        print("\nCOULD NOT STABILIZE %d bench(es) within the cap "
              "(hand-tune or investigate the noise):" % len(failed))
        for n, why in failed:
            print("  %-24s %s" % (n, why))

    if args.dry_run:
        print("\n--dry-run: bench/scales.txt NOT written.")
    else:
        write_scales(table, run.SCALES_FILE)
        print("\nwrote %s (uncommitted - review + commit)." % run.SCALES_FILE)


if __name__ == "__main__":
    main()
