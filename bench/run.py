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
#                                        # (comma-separated for several)
#   python3 bench/run.py --mylang ./build/mylang
#   python3 bench/run.py --baseline OLD  # also time a 2nd mylang binary and
#                                        # report cur/base speedup (before/after)
#   python3 bench/run.py --csv out.csv   # also write the table as CSV
#   python3 bench/run.py -s              # also re-dump sorted by ratio (wins
#                                        # first, regressions last)

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


def rgb_to_xterm256(r, g, b):
    """Nearest xterm-256 palette index for an RGB triple - picks the closer of
    the 6x6x6 color cube and the 24-step grey ramp, so true greys stay grey."""
    cube = (0, 95, 135, 175, 215, 255)

    def level(v):
        return min(range(6), key=lambda i: abs(cube[i] - v))

    cl = (level(r), level(g), level(b))
    cube_rgb = tuple(cube[i] for i in cl)
    cube_idx = 16 + 36 * cl[0] + 6 * cl[1] + cl[2]

    gi = max(0, min(23, round(((r + g + b) / 3 - 8) / 10)))
    grey_rgb = (8 + 10 * gi,) * 3

    def dist2(a, b):
        return sum((x - y) ** 2 for x, y in zip(a, b))

    rgb = (r, g, b)
    if dist2(rgb, cube_rgb) <= dist2(rgb, grey_rgb):
        return cube_idx
    return 232 + gi


# my/py ratio gradient: brightest green for the best wins, a neutral grey across
# the break-even band, brightest red for the worst regressions. We interpolate
# in RGB *through grey* so the transition is smooth and small differences near
# 1.0 read as grey rather than faint green/red.
_GREEN = (0, 255, 0)
_GREY = (150, 150, 150)
_RED = (255, 0, 0)
_NEUTRAL_LO = 0.95
_NEUTRAL_HI = 1.05


def _lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def ratio_xterm_color(ratio):
    """xterm-256 color for a my/py ratio: brightest green at ratio <= 0.35,
    smoothly fading to neutral grey across the 0.95-1.05 break-even band, then
    to brightest red at ratio >= 3.0."""
    if ratio <= 0.35:
        rgb = _GREEN
    elif ratio < _NEUTRAL_LO:
        rgb = _lerp(_GREY, _GREEN, (_NEUTRAL_LO - ratio) / (_NEUTRAL_LO - 0.35))
    elif ratio <= _NEUTRAL_HI:
        rgb = _GREY
    elif ratio < 3.0:
        rgb = _lerp(_GREY, _RED, (ratio - _NEUTRAL_HI) / (3.0 - _NEUTRAL_HI))
    else:
        rgb = _RED
    return rgb_to_xterm256(*rgb)


def colorize_ratio(ratio, text):
    """Wrap `text` (already padded) in the ratio's color, on a TTY only."""
    if not USE_COLOR or ratio is None:
        return text
    return "\x1b[38;5;%dm%s\x1b[0m" % (ratio_xterm_color(ratio), text)


def render_row(row, has_base):
    """One formatted table line; the ratio fields are colored on a TTY. Used for
    both the streaming run and the --sorted re-dump, so they stay identical.

    A row is (name, base_s, my_s, py_s, speedup_s, ratio_s, status, speedup,
    ratio); the base(s) / cur-base columns appear only when `has_base`."""
    (name, base_s, my_s, py_s, speedup_s, ratio_s, status,
     speedup, ratio) = row
    out = "%-24s" % name
    if has_base:
        # cur/base: <1 means the current binary is FASTER (colored green, the
        # same good/bad convention as my/py).
        sp_field = colorize_ratio(speedup, "%8s" % speedup_s)
        out += " %10s %10s %s" % (base_s, my_s, sp_field)
    else:
        out += " %10s" % my_s
    out += " %10s %s  %s" % (py_s, colorize_ratio(ratio, "%8s" % ratio_s), status)
    return out


def geomean(vals):
    """Geometric mean of a non-empty list of positive ratios."""
    prod = 1.0
    for v in vals:
        prod *= v
    return prod ** (1.0 / len(vals))


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
                    help="only run benchmarks whose name contains this substring "
                         "(comma-separated to match any of several)")
    ap.add_argument("--mylang", default="",
                    help="path to the mylang binary (default build/mylang)")
    ap.add_argument("--baseline", default="",
                    help="a 2nd mylang binary to compare against (before/after); "
                         "adds base(s) and cur/base (speedup) columns")
    ap.add_argument("--python", default=sys.executable,
                    help="python interpreter to compare against")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-run timeout in seconds (default 120)")
    ap.add_argument("--csv", default="",
                    help="also write the results table to this CSV file")
    ap.add_argument("-s", "--sorted", action="store_true",
                    help="after the run, re-dump the table sorted by my/py "
                         "ratio ascending (biggest win first, worst regression "
                         "last)")
    args = ap.parse_args()

    mylang = find_mylang(args.mylang)
    if not mylang:
        sys.exit("error: mylang binary not found; build it (make -j) or pass --mylang")

    has_base = bool(args.baseline)
    if has_base and not (os.path.isfile(args.baseline)
                         and os.access(args.baseline, os.X_OK)):
        sys.exit("error: --baseline '%s' is not an executable file" % args.baseline)

    names = sorted(f[:-3] for f in os.listdir(MY_DIR) if f.endswith(".my"))
    if args.filter:
        # comma-separated: a name matches if it contains ANY of the substrings.
        subs = [s for s in args.filter.split(",") if s]
        names = [n for n in names if any(s in n for s in subs)]
    if not names:
        sys.exit("no benchmarks matched")

    scale_arg = str(args.scale)
    print("mylang : %s" % mylang)
    if has_base:
        print("baseline: %s" % args.baseline)
    print("python : %s" % args.python)
    print("scale  : %d    repeat (best of): %d\n" % (args.scale, args.repeat))

    warn = optimization_warning(mylang)
    if warn:
        print(warn + "\n")

    if has_base:
        hdr = "%-24s %10s %10s %8s %10s %8s  %s" % (
            "benchmark", "base(s)", "mylang(s)", "cur/base",
            "python(s)", "my/py", "result")
    else:
        hdr = "%-24s %10s %10s %8s  %s" % (
            "benchmark", "mylang(s)", "python(s)", "my/py", "result")
    print(hdr)
    print("-" * len(hdr))

    rows = []
    ratios = []
    speedups = []
    for name in names:
        my_path = os.path.join(MY_DIR, name + ".my")
        py_path = os.path.join(PY_DIR, name + ".py")

        my_t, my_out, my_err = time_cmd(
            [mylang, my_path, scale_arg], args.repeat, args.timeout)

        base_t = None
        if has_base:
            base_t, _bout, _berr = time_cmd(
                [args.baseline, my_path, scale_arg], args.repeat, args.timeout)

        if os.path.isfile(py_path):
            # -B: don't read/write __pycache__. MyLang re-parses its source on
            # every run (it has no bytecode cache), so letting CPython reuse a
            # cached .pyc across the repeat runs would be an unfair head start.
            # With -B both re-parse every run.
            py_t, py_out, py_err = time_cmd(
                [args.python, "-B", py_path, scale_arg],
                args.repeat, args.timeout)
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
        base_s = "%.3f" % base_t if base_t is not None else "-"

        ratio = None
        if my_t and py_t:
            ratio = my_t / py_t
            ratios.append(ratio)
            ratio_s = "%.2fx" % ratio
        else:
            ratio_s = "-"

        # cur/base: current time over baseline time. <1 == current is faster.
        speedup = None
        speedup_s = "-"
        if has_base and my_t and base_t:
            speedup = my_t / base_t
            speedups.append(speedup)
            speedup_s = "%.2fx" % speedup

        # render_row pads to width before coloring, so the ANSI escapes don't
        # throw off column alignment. The numeric ratio/speedup stay in the row
        # so --sorted can order by them (CSV uses only the plain fields).
        row = (name, base_s, my_s, py_s, speedup_s, ratio_s, status,
               speedup, ratio)
        print(render_row(row, has_base))
        rows.append(row)

    if ratios:
        gm_v = geomean(ratios)
        gm = colorize_ratio(gm_v, "%.2fx" % gm_v)
        if gm_v >= 1.0:
            tail = "MyLang is ~%.1fx slower" % gm_v
        else:
            tail = "MyLang is ~%.1fx faster" % (1.0 / gm_v)
        print("-" * len(hdr))
        print("geomean my/py over %d paired benchmarks: %s (%s)"
              % (len(ratios), gm, tail))

    if speedups:
        sp_v = geomean(speedups)
        sp = colorize_ratio(sp_v, "%.2fx" % sp_v)
        if sp_v < 1.0:
            tail = "current is ~%.1fx FASTER than baseline" % (1.0 / sp_v)
        else:
            tail = "current is ~%.1fx SLOWER than baseline" % sp_v
        print("geomean cur/base over %d benchmarks: %s (%s)"
              % (len(speedups), sp, tail))

    if args.sorted:
        # Ascending: biggest win first, worst regression last. Order by cur/base
        # when comparing two binaries (most-improved first), else by my/py. Rows
        # without that ratio (my-only / failed) have nothing to compare -> trail.
        key_i = 7 if has_base else 8
        ordered = sorted(
            rows, key=lambda r: (r[key_i] is None,
                                 r[key_i] if r[key_i] is not None else 0))
        label = "cur/base" if has_base else "my/py"
        print("\nsorted by %s ratio (biggest win first):" % label)
        print(hdr)
        print("-" * len(hdr))
        for row in ordered:
            print(render_row(row, has_base))

    if args.csv:
        with open(args.csv, "w") as f:
            if has_base:
                f.write("benchmark,base_s,mylang_s,cur_over_base,"
                        "python_s,my_over_py,status\n")
                for (name, base_s, my_s, py_s, speedup_s, ratio_s,
                     status, _sp, _ratio) in rows:
                    f.write("%s,%s,%s,%s,%s,%s,%s\n" %
                            (name, base_s, my_s, speedup_s.rstrip("x"), py_s,
                             ratio_s.rstrip("x"), status.replace(",", ";")))
            else:
                f.write("benchmark,mylang_s,python_s,my_over_py,status\n")
                for (name, _base_s, my_s, py_s, _speedup_s, ratio_s,
                     status, _sp, _ratio) in rows:
                    f.write("%s,%s,%s,%s,%s\n" %
                            (name, my_s, py_s, ratio_s.rstrip("x"),
                             status.replace(",", ";")))
        print("\nwrote %s" % args.csv)


if __name__ == "__main__":
    main()
