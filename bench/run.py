#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# MyLang vs CPython benchmark runner.
#
# Pairs every script in bench/my/<name>.my with bench/py/<name>.py (when one
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
MY_DIR = os.path.join(HERE, "my")
PY_DIR = os.path.join(HERE, "py")

# Color the my/py ratio on a TTY only (never when redirected / into the CSV).
USE_COLOR = sys.stdout.isatty()


def ratio_xterm_color(ratio):
    """xterm-256 color for a my/py ratio: a gradient from brightest green at
    ratio <= 0.35 (best improvement) through dark green / dark red around the
    1.0 break-even point up to brightest red at ratio >= 3.0 (worst
    regression). Returns a 256-color palette index."""
    if ratio <= 0.35:
        return 46                       # brightest green (cube 0,5,0)
    if ratio < 1.0:
        # bright green at 0.35 -> dark green at 1.0
        frac = (1.0 - ratio) / (1.0 - 0.35)         # 1.0 .. 0.0
        return 16 + 6 * (1 + round(4 * frac))       # 46 .. 22
    if ratio >= 3.0:
        return 196                      # brightest red (cube 5,0,0)
    # dark red just above 1.0 -> bright red at 3.0
    frac = (ratio - 1.0) / (3.0 - 1.0)              # 0.0 .. 1.0
    return 16 + 36 * (1 + round(4 * frac))          # 52 .. 196


def colorize_ratio(ratio, text):
    """Wrap `text` (already padded) in the ratio's color, on a TTY only."""
    if not USE_COLOR or ratio is None:
        return text
    return "\x1b[38;5;%dm%s\x1b[0m" % (ratio_xterm_color(ratio), text)


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

    names = sorted(f[:-3] for f in os.listdir(MY_DIR) if f.endswith(".my"))
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
        "benchmark", "mylang(s)", "python(s)", "my/py", "result")
    print(hdr)
    print("-" * len(hdr))

    rows = []
    ratios = []
    for name in names:
        my_path = os.path.join(MY_DIR, name + ".my")
        py_path = os.path.join(PY_DIR, name + ".py")

        my_t, my_out, my_err = time_cmd(
            [mylang, my_path, scale_arg], args.repeat, args.timeout)

        if os.path.isfile(py_path):
            py_t, py_out, py_err = time_cmd(
                [args.python, py_path, scale_arg], args.repeat, args.timeout)
        else:
            py_t, py_out, py_err = (None, None, "no-py")

        if my_err:
            status = "MY " + my_err
        elif py_err == "no-py":
            status = "my-only"
        elif py_err:
            status = "PY " + py_err
        elif results_match(my_out, py_out):
            status = "ok"
        else:
            status = "DIFF: my=%r py=%r" % (my_out, py_out)

        my_s = "%.3f" % my_t if my_t is not None else "-"
        py_s = "%.3f" % py_t if py_t is not None else "-"
        ratio = None
        if my_t and py_t:
            ratio = my_t / py_t
            ratios.append(ratio)
            ratio_s = "%.2fx" % ratio
        else:
            ratio_s = "-"

        # Pad to width first, then color, so the ANSI escapes don't throw off
        # column alignment. CSV/rows keep the plain string.
        ratio_field = colorize_ratio(ratio, "%8s" % ratio_s)
        print("%-24s %10s %10s %s  %s" %
              (name, my_s, py_s, ratio_field, status))
        rows.append((name, my_s, py_s, ratio_s, status))

    if ratios:
        prod = 1.0
        for r in ratios:
            prod *= r
        geomean = prod ** (1.0 / len(ratios))
        gm = colorize_ratio(geomean, "%.2fx" % geomean)
        if geomean >= 1.0:
            tail = "MyLang is ~%.1fx slower" % geomean
        else:
            tail = "MyLang is ~%.1fx faster" % (1.0 / geomean)
        print("-" * len(hdr))
        print("geomean my/py over %d paired benchmarks: %s (%s)"
              % (len(ratios), gm, tail))

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("benchmark,mylang_s,python_s,my_over_py,status\n")
            for name, my_s, py_s, ratio_s, status in rows:
                f.write("%s,%s,%s,%s,%s\n" %
                        (name, my_s, py_s, ratio_s.rstrip("x"),
                         status.replace(",", ";")))
        print("\nwrote %s" % args.csv)


if __name__ == "__main__":
    main()
