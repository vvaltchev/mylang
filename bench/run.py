#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# MyLang vs CPython benchmark runner.
#
# Pairs every script in bench/ml/<name>.ml with bench/py/<name>.py (when one
# exists), runs each a few times, keeps the best wall-clock time, and prints a
# comparison table. Benchmarks without a Python counterpart (a MyLang-only
# feature, e.g. parse-time const folding) are still timed; their Python column
# shows "-".
#
# Stdlib only - no third-party dependencies, matching the project's ethos.
#
# Usage:
#   python3 bench/run.py                 # run everything, scale 1
#   python3 bench/run.py --scale 2       # 2x the per-benchmark workload
#   python3 bench/run.py --repeat 5      # best of 5 runs (default 3)
#   python3 bench/run.py --filter slice  # only benchmarks whose name matches
#   python3 bench/run.py --mylang ./build/mylang
#   python3 bench/run.py --csv out.csv   # also write the table as CSV

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ML_DIR = os.path.join(HERE, "ml")
PY_DIR = os.path.join(HERE, "py")


def find_mylang(explicit):
    if explicit:
        return explicit
    repo = os.path.dirname(HERE)
    for cand in (os.path.join(repo, "build", "mylang"),
                 os.path.join(repo, "mylang")):
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def optimization_warning(mylang):
    """Time a tiny fixed loop; a slow result means an unoptimized binary.

    Benchmark numbers are only meaningful against an -O3 release build
    (`make -j`). A debug / TESTS / no-optimization build runs ~7x slower and
    makes every result misleading - this probe catches that automatically."""
    probe = "var i = 0; while (i < 1000000) i += 1;"
    start = time.perf_counter()
    try:
        subprocess.run([mylang, "-e", probe], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)
    except Exception:
        return None
    dt = time.perf_counter() - start
    if dt > 0.15:
        return ("WARNING: '%s' runs a 1M-iteration loop in %.2fs - that looks "
                "like an UNOPTIMIZED build.\n"
                "         Benchmark with an -O3 release build (make -j) or the "
                "numbers are meaningless." % (mylang, dt))
    return None


def time_cmd(cmd, repeat, timeout):
    """Run cmd `repeat` times; return (best_seconds, stdout, error_or_None)."""
    best = None
    out = ""
    for _ in range(repeat):
        start = time.perf_counter()
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            return (None, "", "timeout")
        except OSError as e:
            return (None, "", str(e))
        elapsed = time.perf_counter() - start
        out = p.stdout.decode("utf-8", "replace").strip()
        if p.returncode != 0:
            return (None, out, "exit %d" % p.returncode)
        if best is None or elapsed < best:
            best = elapsed
    return (best, out, None)


def tokens(s):
    return s.split()


def results_match(a, b):
    """Token-wise compare; ints exact, floats within a relative tolerance.

    This absorbs cosmetic differences (MyLang prints floats as 3.500000,
    Python as 3.5) and the tiny long-double-vs-double drift, while still
    catching genuine logic divergences."""
    ta, tb = tokens(a), tokens(b)
    if len(ta) != len(tb):
        return False
    for x, y in zip(ta, tb):
        if x == y:
            continue
        try:
            fx, fy = float(x), float(y)
        except ValueError:
            return False
        scale = max(abs(fx), abs(fy), 1.0)
        if abs(fx - fy) / scale > 1e-6:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description="MyLang vs Python benchmarks")
    ap.add_argument("--scale", type=int, default=1,
                    help="workload multiplier passed to each script (default 1)")
    ap.add_argument("--repeat", type=int, default=3,
                    help="runs per benchmark, best time kept (default 3)")
    ap.add_argument("--filter", default="",
                    help="only run benchmarks whose name contains this substring")
    ap.add_argument("--mylang", default="",
                    help="path to the mylang binary (default build/mylang)")
    ap.add_argument("--python", default=sys.executable,
                    help="python interpreter to compare against")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-run timeout in seconds (default 120)")
    ap.add_argument("--csv", default="",
                    help="also write the results table to this CSV file")
    args = ap.parse_args()

    mylang = find_mylang(args.mylang)
    if not mylang:
        sys.exit("error: mylang binary not found; build it (make -j) or pass --mylang")

    names = sorted(f[:-3] for f in os.listdir(ML_DIR) if f.endswith(".ml"))
    if args.filter:
        names = [n for n in names if args.filter in n]
    if not names:
        sys.exit("no benchmarks matched")

    scale_arg = str(args.scale)
    print("mylang : %s" % mylang)
    print("python : %s" % args.python)
    print("scale  : %d    repeat (best of): %d\n" % (args.scale, args.repeat))

    warn = optimization_warning(mylang)
    if warn:
        print(warn + "\n")

    hdr = "%-24s %10s %10s %8s  %s" % (
        "benchmark", "mylang(s)", "python(s)", "ml/py", "result")
    print(hdr)
    print("-" * len(hdr))

    rows = []
    ratios = []
    for name in names:
        ml_path = os.path.join(ML_DIR, name + ".ml")
        py_path = os.path.join(PY_DIR, name + ".py")

        ml_t, ml_out, ml_err = time_cmd(
            [mylang, ml_path, scale_arg], args.repeat, args.timeout)

        if os.path.isfile(py_path):
            py_t, py_out, py_err = time_cmd(
                [args.python, py_path, scale_arg], args.repeat, args.timeout)
        else:
            py_t, py_out, py_err = (None, None, "no-py")

        if ml_err:
            status = "ML " + ml_err
        elif py_err == "no-py":
            status = "ml-only"
        elif py_err:
            status = "PY " + py_err
        elif results_match(ml_out, py_out):
            status = "ok"
        else:
            status = "DIFF: ml=%r py=%r" % (ml_out, py_out)

        ml_s = "%.3f" % ml_t if ml_t is not None else "-"
        py_s = "%.3f" % py_t if py_t is not None else "-"
        if ml_t and py_t:
            ratio = ml_t / py_t
            ratios.append(ratio)
            ratio_s = "%.2fx" % ratio
        else:
            ratio_s = "-"

        print("%-24s %10s %10s %8s  %s" % (name, ml_s, py_s, ratio_s, status))
        rows.append((name, ml_s, py_s, ratio_s, status))

    if ratios:
        prod = 1.0
        for r in ratios:
            prod *= r
        geomean = prod ** (1.0 / len(ratios))
        print("-" * len(hdr))
        print("geomean ml/py over %d paired benchmarks: %.2fx "
              "(MyLang is ~%.1fx slower)" % (len(ratios), geomean, geomean))

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("benchmark,mylang_s,python_s,ml_over_py,status\n")
            for name, ml_s, py_s, ratio_s, status in rows:
                f.write("%s,%s,%s,%s,%s\n" %
                        (name, ml_s, py_s, ratio_s.rstrip("x"),
                         status.replace(",", ";")))
        print("\nwrote %s" % args.csv)


if __name__ == "__main__":
    main()
