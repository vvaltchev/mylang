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
# Usage:
#   python3 bench/tune_scales.py                    # tune all, write scales.txt
#   python3 bench/tune_scales.py --filter dict      # only some benches
#   python3 bench/tune_scales.py --dry-run          # report, don't write
#   python3 bench/tune_scales.py --mylang build-rel/mylang

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run  # noqa: E402  (bench/run.py - shared timing + variance)

MAX_SCALE = 64          # never bump a single bench past this multiplier
MAX_ITERS = 6           # bump attempts per bench before giving up
# Tune with MARGIN: require the bench to settle within the first two rep steps
# (3->5), NOT the full 3->5->8. A bench that needs all 8 reps is borderline -
# a slightly noisier run.py invocation would then ABORT it. Bumping it so it
# settles by 5 reps gives run.py a full step of headroom.
SETTLE_TARGET = run.REP_STEPS[0] + run.REP_STEPS[1]     # 5


def measure(mylang, my_path, scale, threshold, timeout):
    """Does the bench SETTLE within run.py's OWN adaptive 3->5->8 gate at
    `scale`? Returns (settled_bool, variance, n_reps, error_or_None). We use
    the exact gate run.py uses so a scale the tuner accepts is one run.py won't
    abort on - and a bench that settles in 3 reps needs no bump at all."""
    _best, _out, err, n_reps, v = run.measure_adaptive(
        [mylang, my_path, str(scale)], threshold, timeout)
    if err and not err.startswith("variance"):
        return (False, None, n_reps, err)     # a real run error
    return (err is None, v, n_reps, None)


def bump(scale, variance, threshold):
    """The delicately-sized next scale. The run's fixed-overhead noise falls
    ~linearly with workload, so factor ~= variance/threshold is about what is
    needed - but we clamp it to [1.5x, 2x] (the benches are almost stable, a
    wild jump would waste time), and always advance by at least +1 (integer
    scale)."""
    factor = min(2.0, max(1.5, variance / threshold))
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
    ap.add_argument("--dry-run", action="store_true",
                    help="report proposed bumps but do not write scales.txt")
    args = ap.parse_args()

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
    thr = args.var_threshold
    print("tuning %d benches with %s (run.py's adaptive gate, target var<=%.1f%%)"
          "\n" % (len(names), mylang, thr * 100))

    changed = []     # (name, old, new, final_var)
    failed = []      # (name, reason)
    for name in names:
        my_path = os.path.join(run.MY_DIR, name + ".my")
        start_scale = table.get(name, run.DEFAULT_SCALE)
        scale = start_scale
        last_v = None
        last_reps = 0
        for _ in range(MAX_ITERS):
            settled, v, n_reps, err = measure(mylang, my_path, scale, thr,
                                              args.timeout)
            if err:
                failed.append((name, "run error: %s" % err))
                break
            last_v = v
            last_reps = n_reps
            if settled and n_reps <= SETTLE_TARGET:
                if scale != start_scale:
                    changed.append((name, start_scale, scale, v))
                table[name] = scale
                break
            nxt = bump(scale, v, thr)
            if nxt > MAX_SCALE:
                failed.append((name, "var %.1f%% at scale %d, next %d > cap %d"
                               % (v * 100, scale, nxt, MAX_SCALE)))
                table[name] = scale
                break
            scale = nxt
        else:
            failed.append((name, "did not converge (last var %.1f%%)"
                           % ((last_v or 0) * 100)))
        marker = ("bumped %d->%d" % (start_scale, table.get(name, start_scale))
                  if table.get(name, start_scale) != start_scale else "ok")
        print("  %-24s scale=%-3d var=%5.1f%% (%dr)  %s"
              % (name, table.get(name, start_scale),
                 (last_v or 0) * 100, last_reps, marker))

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
