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
# Each bench runs at its per-bench scale from bench/scales.txt (a short/noisy
# bench can be made reliable by raising ITS scale - see bench/tune_scales.py),
# with an ADAPTIVE rep count: 3 reps, and if the min isn't stable (the 2-fastest
# gap exceeds --var-threshold) escalate 3 -> 5 -> 8 -> 13 -> 21. Needing more
# reps to settle is NOT a problem (more best-of samples) and is not reported;
# only a bench whose variance is STILL over the threshold at 21 is flagged
# (NOISY + a closing WARNING - its min is unreliable) - it does NOT abort the
# run, so a full suite always completes. The 13/21 tail rides out a transient
# scheduling burst so a well-scaled bench isn't falsely flagged (see REP_STEPS).
# The reported time is always the MIN.
# We also best-effort raise our scheduling priority (nice) to cut preemption
# noise - all languages benefit equally since they run as our children.
#
# A FEW benches can't be lengthened by scale (their runtime is flat - e.g.
# 52_cse_dedup is const-folded to a ~2ms loop no scale can grow), so the min is
# always short enough to be jittery at 3 reps. Such a bench gets a per-bench
# STARTING rep count in bench/reps.txt (name reps) - its adaptive schedule
# begins there (then still escalates +2/+3), giving its min more best-of samples
# from the start so it settles without ever tripping the variance WARNING.
# PREFER SCALE; reps.txt is only for the genuinely scale-immune benches.
#
# Usage:
#   python3 bench/run.py                 # everything, per-bench scales
#   python3 bench/run.py --scale 3       # force ALL benches to scale 3
#   python3 bench/run.py --scale 23:5    # force bench 23 to scale 5 (repeatable)
#   python3 bench/run.py --repeat 5      # fixed 5 reps (skip the adaptive gate)
#   python3 bench/run.py --filter slice  # only benchmarks whose name matches
#                                        # (comma-separated for several)
#   python3 bench/run.py --mylang ./build/mylang
#   python3 bench/run.py -cl cpp         # compare vs C++ (cached) instead of py
#   python3 bench/run.py -cl py --recompute        # (re)populate the py cache -
#                                        # a SEPARATE step; a measure run is
#                                        # cache-ONLY and fails fast if stale
#   python3 bench/run.py --baseline OLD  # also time a 2nd mylang binary and
#                                        # report cur/base speedup (before/after)
#   python3 bench/run.py --csv out.csv   # also write the table as CSV
#   python3 bench/run.py -s              # also re-dump sorted by ratio (wins
#                                        # first, regressions last)

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MY_DIR = os.path.join(HERE, "my")
PY_DIR = os.path.join(HERE, "py")
BENCH_H = os.path.join(HERE, "cpp", "bench.h")   # affects every C++ bench

# Per-bench workload scale lives in a committed table (name -> multiplier), so a
# noisy short bench can be made reliable by bumping ITS scale without touching
# the others. A bench absent from the table runs at DEFAULT_SCALE. tune_scales.py
# rewrites this file; `--scale <name-or-number>:<N>` overrides it per run.
SCALES_FILE = os.path.join(HERE, "scales.txt")
DEFAULT_SCALE = 1

# A per-bench STARTING rep count (name -> reps), for the rare bench whose
# runtime is flat and too short to settle at 3 reps no matter the scale (see the
# header). A bench absent here starts at REP_STEPS[0] (3). This is the SECONDARY
# lever - use it only when a bench genuinely can't be lengthened by scale.
REPS_FILE = os.path.join(HERE, "reps.txt")


def load_scales():
    """Read bench/scales.txt -> {name: scale}. Lines are `name scale`; blank
    lines and `#` comments are skipped. Missing/unreadable -> empty (all
    benches then run at DEFAULT_SCALE)."""
    table = {}
    try:
        with open(SCALES_FILE) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) >= 2 and parts[1].lstrip("-").isdigit():
                    table[parts[0]] = int(parts[1])
    except OSError:
        pass
    return table


def load_reps():
    """Read bench/reps.txt -> {name: min_reps}. Same `name value` line format as
    scales.txt (blank lines and `#` comments skipped); only entries with a
    positive-int value are kept. Missing/unreadable -> empty (every bench then
    starts at the global REP_STEPS[0])."""
    table = {}
    try:
        with open(REPS_FILE) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) >= 2 and parts[1].isdigit() and int(parts[1]) > 0:
                    table[parts[0]] = int(parts[1])
    except OSError:
        pass
    return table


def resolve_reps(name, reps_table):
    """Starting rep count for `name`: its reps.txt entry, else REP_STEPS[0] (3).
    A configured value below that global default is clamped up to it - the
    default 3 is the floor, reps.txt only ever RAISES a bench's baseline."""
    return max(REP_STEPS[0], reps_table.get(name, REP_STEPS[0]))


def _scale_key_matches(key, name):
    """A --scale override key matches a bench: a numeric key matches the NN
    prefix (`23` -> `23_dict_insert`, leading zeros ignored); a non-numeric key
    is a plain substring (`dict` -> every dict bench)."""
    if key.isdigit():
        num = name.split("_", 1)[0]
        return num == key or num.lstrip("0") == key.lstrip("0")
    return key in name


def resolve_scale(name, table, overrides, global_scale):
    """Scale for `name`: an explicit --scale override wins, then a bare global
    --scale, then the table, then DEFAULT_SCALE."""
    for key, val in overrides:
        if _scale_key_matches(key, name):
            return val
    if global_scale is not None:
        return global_scale
    return table.get(name, DEFAULT_SCALE)

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


def raise_priority():
    """Best-effort: raise THIS process's scheduling priority so the benchmark
    subprocesses (children inherit our nice value) get preempted less - the
    single biggest source of short-bench timing noise. All languages benefit
    equally (mylang, python, C++, ...) since they all run as our children at
    the same priority, so the my/py ratio stays fair.

    Raising priority (a NEGATIVE nice) needs CAP_SYS_NICE; if we're not
    privileged it fails and we just continue at normal priority (adaptive reps
    + best-of-N still filter most noise). Returns the new nice value, or None
    if we couldn't lower it. Run with `sudo` or grant the binary the cap
    (`sudo setcap cap_sys_nice+ep $(which python3)`) to enable it."""
    try:
        cur = os.nice(0)
        if cur <= -10:
            return cur                      # already high enough
        return os.nice(-10 - cur)           # target nice -10
    except (OSError, AttributeError):
        return None


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


def run_reps(cmd, reps, timeout):
    """Run cmd `reps` times; return (times_list, last_stdout, error_or_None).

    times_list is the wall-clock of every rep (unsorted). We keep them all so
    the caller can take the MIN (best-of, filters preemption spikes) AND assess
    the variance (see bench_variance)."""
    times = []
    out = ""
    for _ in range(reps):
        start = time.perf_counter()
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            return ([], "", "timeout")
        except OSError as e:
            return ([], "", str(e))
        elapsed = time.perf_counter() - start
        out = p.stdout.decode("utf-8", "replace").strip()
        if p.returncode != 0:
            return ([], out, "exit %d" % p.returncode)
        times.append(elapsed)
    return (times, out, None)


def bench_variance(times):
    """The variance metric used to decide if a bench is stable enough.

    We always REPORT the MIN, so preemption (which only makes runs SLOWER)
    can't hurt the reported number. What we must catch is a bench whose run is
    too SHORT for the min to be reliable (timer granularity / fixed overhead).
    Metric = (2nd-smallest - min) / min : the gap between the two FASTEST runs.
    It asks 'have the fast runs converged?' and IGNORES every slower rep - so a
    preempted spike never triggers a spurious scale bump.

    Crucially this is REP-COUNT-INDEPENDENT: for run.py's 3 reps it equals the
    (median-min)/min the design calls for, but it does NOT drift when the TUNER
    takes more reps (a median would climb as N grows). So the tuner can average
    over many reps for a STABLE decision that still matches run.py's 3-rep
    check. Returns 0 for a degenerate (<=1 rep / zero min)."""
    if len(times) < 2:
        return 0.0
    ts = sorted(times)
    mn = ts[0]
    return (ts[1] - mn) / mn if mn > 0 else 0.0


# Dynamic rep schedule: start with 3, and if the variance gate isn't met, add
# more reps (NOT more scale - sampling noise shrinks with reps, not workload
# size) in growing steps. A stable bench stops at 3 (cheap); only a noisy one
# pays for more. Needing more reps to settle is NORMAL (more best-of samples,
# not a problem) and is not reported; a bench STILL over threshold at the LAST
# step is genuinely unreliable and is FLAGGED (NOISY on its row + a closing
# WARNING), but it does NOT abort the run - the min is kept and the suite
# always completes. `--repeat N` overrides this with a fixed N.
#
# The common case is unchanged: 3 -> 5 -> 8. The two ADDED steps (-> 13 -> 21)
# are a RESILIENCE tail against a false-positive flag. On a noisy box a
# transient scheduling burst can make a WELL-SCALED bench miss the gate at 8
# reps (its 2 fastest of 8 happen to straddle a spike) - and across a 76-bench
# suite that near-miss lands on SOME random bench most runs (even ~1% per-bench
# odds compound to ~1-0.99^76 ~= 50%+ suite-wide). The flag is meant to catch a
# GENUINELY under-scaled bench, not a momentary burst, so we ride the burst out
# with two more best-of steps: a bench that still can't converge over 21 best-of
# samples really is unstable and wants a scale bump. A settled bench never
# reaches them, so this only costs time on a bench that is genuinely noisy.
REP_STEPS = (3, 2, 3, 5, 8)   # cumulative: 3, 5, 8, 13, 21
# Reps for the comparison language when (re)populating its cache. It has no
# variance gate (we don't optimize it) - a fixed best-of-N min is plenty, and
# it's a one-time cost per (bench, scale, source).
COMP_REPS = 5


def measure_adaptive(cmd, threshold, timeout, min_reps=REP_STEPS[0]):
    """Time `cmd` with the growing rep schedule until the 2-fastest-gap
    variance clears `threshold`. Returns (best_time, stdout, error_or_None,
    n_reps, variance). error_or_None is a 'variance ...' string if it never
    settled (the caller keeps the min + FLAGS it as NOISY, not aborts), or a
    run error.

    The schedule STARTS at `min_reps` (default the global REP_STEPS[0]=3) and
    then escalates by the usual REP_STEPS increments (+2, +3). So the default
    is the unchanged 3->5->8; a bench with a raised reps.txt baseline of B runs
    B -> B+2 -> B+5 (see load_reps / the header)."""
    steps = (min_reps,) + tuple(REP_STEPS[1:])
    times = []
    out = ""
    v = 0.0
    for step in steps:
        more, o, err = run_reps(cmd, step, timeout)
        if err:
            return (None, o, err, len(times), 0.0)
        times += more
        out = o
        v = bench_variance(times)
        if v <= threshold:
            return (min(times), out, None, len(times), v)
    return (min(times), out,
            "variance %.1f%% > %.1f%% after %d reps" %
            (v * 100, threshold * 100, len(times)),
            len(times), v)


def measure_fixed(cmd, reps, timeout):
    """--repeat N override: exactly N reps, no variance gate. Returns the same
    tuple shape as measure_adaptive (never a 'variance' give-up)."""
    times, out, err = run_reps(cmd, reps, timeout)
    if err:
        return (None, out, err, 0, 0.0)
    return (min(times), out, None, len(times), bench_variance(times))


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


# ------------------------- comparison languages ---------------------------
#
# MyLang is timed on EVERY run (never cached - its result is the whole point).
# The COMPARISON language (python by default, selectable with --complang) is
# timed ONCE and CACHED - it doesn't change between MyLang edits, so re-timing
# it every run is wasted work (and doubles the suite cost). The cache
# (bench/.bench_cache/<lang>.json, git-ignored) keys each bench by its scale + a
# sha1 of its comparison SOURCE(s) (the .py/.cpp, + bench.h for C++) - NOT a git
# commit, so a MyLang edit/commit never invalidates it; ONLY a scale change or a
# real comparison-source change does.
#
# The two are STRICTLY SEPARATED (so a comparison run can never interleave with
# the variance-gated MyLang timing and perturb it):
#   * a normal (MEASURE) run reads the cache READ-ONLY and FAILS FAST up front
#     if any selected bench is stale/missing - it never re-times a comparison;
#   * `--recompute` is the SEPARATE, explicit step that (re)times + re-caches
#     the comparison (stale only, or all with --force), then exits.
# Since the comparison sources rarely change, --recompute is rarely needed.
# Adding a language = one CompLang entry + a bench/<subdir>/.

class CompLang:
    """An interpreted comparison language: bench/<subdir>/<name><ext>, run as
    `interp + [src, scale]`. Compiled languages subclass and override
    prepare()/content_hash()."""
    def __init__(self, name, subdir, ext, interp):
        self.name = name
        self.subdir = subdir
        self.ext = ext
        self.interp = interp        # argv prefix, e.g. [python, "-B"]

    def src(self, bench):
        return os.path.join(HERE, self.subdir, bench + self.ext)

    def has(self, bench):
        return os.path.isfile(self.src(bench))

    def content_hash(self, bench):
        """Hash of everything that affects this bench's output/time - the
        source; compiled langs add their shared headers."""
        return _file_hash(self.src(bench))

    def prepare(self, bench):
        """Return (run_argv_prefix, error_or_None). Interpreted: no build."""
        return (self.interp + [self.src(bench)], None)


class CppLang(CompLang):
    """C++: compile bench/cpp/<name>.cpp (-O3 -fwrapv) on demand, run the
    binary. The compile depends on bench/cpp/bench.h too."""
    def __init__(self, cxx):
        super().__init__("cpp", "cpp", ".cpp", [])
        self.cxx = cxx

    def content_hash(self, bench):
        return _file_hash(self.src(bench)) + _file_hash(BENCH_H)

    def prepare(self, bench):
        src = self.src(bench)
        exe = src[:-4]                       # strip .cpp
        need = (not os.path.exists(exe)
                or os.path.getmtime(exe) < os.path.getmtime(src)
                or (os.path.exists(BENCH_H)
                    and os.path.getmtime(exe) < os.path.getmtime(BENCH_H)))
        if need:
            r = subprocess.run([self.cxx, "-O3", "-fwrapv", "-std=c++17",
                                "-o", exe, src], capture_output=True, text=True)
            if r.returncode != 0:
                return (None, "compile failed: " + r.stderr.strip()[:120])
        return ([exe], None)


def build_complangs(python, cxx):
    """The registry. Extend with ruby/perl/lua/java by adding entries + the
    matching bench/<subdir>/ trees."""
    return {
        "python": CompLang("python", "py", ".py", [python, "-B"]),
        "cpp":    CppLang(cxx),
        "ruby":   CompLang("ruby", "rb", ".rb", ["ruby"]),
        "perl":   CompLang("perl", "pl", ".pl", ["perl"]),
        "lua":    CompLang("lua", "lua", ".lua", ["lua"]),
    }


def _file_hash(path):
    try:
        with open(path, "rb") as fh:
            return hashlib.sha1(fh.read()).hexdigest()[:16]
    except OSError:
        return ""


CACHE_DIR = os.path.join(HERE, ".bench_cache")


def load_cache(lang):
    try:
        with open(os.path.join(CACHE_DIR, lang + ".json")) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def save_cache(lang, cache):
    try:
        os.makedirs(CACHE_DIR, exist_ok=True)
        with open(os.path.join(CACHE_DIR, lang + ".json"), "w") as fh:
            json.dump(cache, fh, indent=1, sort_keys=True)
    except OSError:
        pass


def cache_entry_fresh(lobj, bench, scale, cache):
    """True iff `bench`'s comparison cache entry is a genuine HIT: present AND
    for the current scale AND matching the current SOURCE-FILE hash. The `hash`
    is a sha1 of the comparison SOURCE (the .py/.cpp, + bench.h for C++) - NOT a
    git commit - so it invalidates ONLY when that source actually changes, never
    on a MyLang edit / commit. A bench with no comparison source is handled
    separately by the caller (it is never "stale"). This is the exact key
    comp_time uses, factored out for the up-front staleness gate."""
    ent = cache.get(bench)
    return bool(ent and ent.get("scale") == scale
                and ent.get("hash") == lobj.content_hash(bench))


def comp_time(lobj, bench, scale, reps, timeout, cache, refresh,
              cache_only=False):
    """Time the comparison language for `bench` at `scale`, via the cache.
    Returns (min_time, output, error_or_None, from_cache). A hit (same scale +
    same source hash, unless --force) skips the run entirely.

    cache_only=True (the MEASURE path) NEVER runs a comparison subprocess: a
    miss returns a 'stale' error instead of re-timing. The up-front staleness
    gate guarantees every measured bench is a hit, so this is a hard backstop
    that keeps a comparison run from EVER interleaving with the variance-gated
    MyLang timing (the perturbation that made py- vs cpp-mode asymmetric)."""
    if not lobj.has(bench):
        return (None, None, "no-" + lobj.name, False)
    h = lobj.content_hash(bench)
    ent = cache.get(bench)
    if (not refresh and ent and ent.get("scale") == scale
            and ent.get("hash") == h):
        return (ent["time"], ent["out"], None, True)
    if cache_only:
        return (None, None, "stale", False)
    prefix, err = lobj.prepare(bench)
    if err:
        return (None, None, err, False)
    times, out, rerr = run_reps(prefix + [str(scale)], reps, timeout)
    if rerr:
        return (None, out, rerr, False)
    t = min(times)
    cache[bench] = {"scale": scale, "hash": h, "time": t, "out": out}
    return (t, out, None, False)


def main():
    ap = argparse.ArgumentParser(description="MyLang vs Python benchmarks")
    ap.add_argument("--scale", action="append", default=[], metavar="SPEC",
                    help="override the per-bench scale from bench/scales.txt. "
                         "A bare int (--scale 3) sets ALL benches; "
                         "<name-or-number>:<N> (--scale 23:5, repeatable) sets "
                         "one. Without --scale, each bench uses its table scale.")
    ap.add_argument("--repeat", type=int, default=0,
                    help="force a FIXED rep count (min reported), skipping the "
                         "adaptive 3->5->8 schedule. 0 (default) = adaptive: "
                         "start at 3 reps, add more only if the variance gate "
                         "isn't met.")
    ap.add_argument("--var-threshold", type=float, default=0.05,
                    help="max acceptable (2nd-min - min)/min per bench; a "
                         "bench still over it after the last rep step is "
                         "FLAGGED (NOISY + a closing WARNING - its min is "
                         "unreliable), not aborted. Fix by bumping its table "
                         "scale (run tune_scales.py). Default 0.05 = 5%% (a "
                         "pre-pooled-allocator floor; tighten after pooled-alloc)")
    ap.add_argument("--filter", default="",
                    help="only run benchmarks whose name contains this substring "
                         "(comma-separated to match any of several)")
    ap.add_argument("--mylang", default="",
                    help="path to the mylang binary (default build/mylang)")
    ap.add_argument("--baseline", default="",
                    help="a 2nd mylang binary to compare against (before/after); "
                         "adds base(s) and cur/base (speedup) columns")
    ap.add_argument("--vm", action="store_true",
                    help="run the current mylang with an explicit -vm (the "
                         "DEFAULT engine since 2026-07-18 - useful for a "
                         "pre-flip binary, whose default was the "
                         "tree-walker). Pair with --baseline <same binary> "
                         "to gate the VM vs the tree-walker: cur/base = "
                         "VM/tree-walk (<1 == VM faster). With --vm the "
                         "baseline runs -tw (it must be a post-flip binary).")
    ap.add_argument("--tw", action="store_true",
                    help="run the current mylang with -tw (the tree-walking "
                         "interpreter).")
    ap.add_argument("--python", default=sys.executable,
                    help="python interpreter (for --complang python)")
    ap.add_argument("-cl", "--complang", default="python",
                    metavar="LANG",
                    help="comparison language: python (default), cpp, ruby, "
                         "perl, lua. Its results are CACHED (bench/.bench_cache/"
                         "<lang>.json, git-ignored); a measure run reads them "
                         "cache-only and fails fast if stale - re-time via the "
                         "separate --recompute step.")
    ap.add_argument("--cxx", default="g++",
                    help="C++ compiler for --complang cpp (default g++)")
    ap.add_argument("--recompute", action="store_true",
                    help="SEPARATE STEP: (re)time the comparison language for "
                         "the selected --complang + --filter and update its "
                         "cache, then exit (does NOT time MyLang). Recomputes "
                         "only STALE entries unless --force forces all. "
                         "A normal (measure) run is cache-ONLY and fails fast "
                         "if anything is stale, so this is the only place the "
                         "comparison is ever re-timed - the comparison sources "
                         "rarely change, so you rarely need it.")
    ap.add_argument("--force", action="store_true",
                    help="with --recompute: force-recompute EVERY selected "
                         "bench (not just stale ones). Ignored otherwise.")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-run timeout in seconds (default 120)")
    ap.add_argument("--csv", default="",
                    help="also write the results table to this CSV file")
    ap.add_argument("-s", "--sorted", action="store_true",
                    help="after the run, re-dump the table sorted by my/py "
                         "ratio ascending (biggest win first, worst regression "
                         "last)")
    args = ap.parse_args()

    mylang = find_mylang(args.mylang)   # required only for a MEASURE run below

    complangs = build_complangs(args.python, args.cxx)
    if args.complang not in complangs:
        sys.exit("error: --complang '%s' unknown (have: %s)"
                 % (args.complang, ", ".join(sorted(complangs))))
    lobj = complangs[args.complang]
    cache = load_cache(lobj.name)

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

    # Parse --scale: a bare int is a global scale; <key>:<N> is a per-bench
    # override (repeatable). Both may be present (per-bench wins).
    table = load_scales()
    overrides = []
    global_scale = None
    for spec in args.scale:
        if ":" in spec:
            key, val = spec.rsplit(":", 1)
            try:
                overrides.append((key, int(val)))
            except ValueError:
                sys.exit("error: bad --scale override %r (want <key>:<int>)" % spec)
        else:
            try:
                global_scale = int(spec)
            except ValueError:
                sys.exit("error: bad --scale %r (want an int or <key>:<int>)" % spec)

    # The per-bench scale, resolved ONCE (used by both the recompute step and
    # the measure run's up-front staleness gate + loop). `comparable` = the
    # benches that HAVE a comparison source (the rest are MyLang-only).
    scales = {n: resolve_scale(n, table, overrides, global_scale) for n in names}
    comparable = [n for n in names if lobj.has(n)]

    # Per-bench STARTING rep count (default 3, raised only by bench/reps.txt for
    # the scale-immune benches). Used by the measure loop below; ignored under
    # --repeat (a fixed count skips the adaptive gate entirely).
    reps_table = load_reps()
    min_reps_map = {n: resolve_reps(n, reps_table) for n in names}

    filt = " --filter %s" % args.filter if args.filter else ""

    if args.recompute:
        # SEPARATE STEP: (re)time + re-cache the comparison language, then exit.
        # Only STALE entries by default (a no-op when all fresh); --force
        # forces all. MyLang is never run here. This is the ONLY place a
        # comparison is timed, so it can never interleave with a measure run.
        todo = comparable if args.force else [
            n for n in comparable
            if not cache_entry_fresh(lobj, n, scales[n], cache)]
        if not todo:
            print("%s: all %d selected result(s) already fresh - nothing to "
                  "recompute." % (lobj.name, len(comparable)))
            return
        print("%s: recomputing %d of %d selected result(s)%s ..."
              % (lobj.name, len(todo), len(comparable),
                 " (--force: all)" if args.force else " (stale)"))
        comp_reps = args.repeat if args.repeat > 0 else COMP_REPS
        failed = []
        for n in todo:
            t, _out, err, _fc = comp_time(lobj, n, scales[n], comp_reps,
                                          args.timeout, cache, refresh=True)
            if err:
                failed.append(n)
                print("  %-24s FAILED: %s" % (n, err))
            else:
                print("  %-24s %.3fs  (scale %d)" % (n, t, scales[n]))
            save_cache(lobj.name, cache)     # persist incrementally
        if failed:
            sys.exit("recompute: %d bench(es) failed to time" % len(failed))
        print("done - %d %s comparison result(s) cached." % (len(todo), lobj.name))
        return

    # MEASURE run: the comparison cache is READ-ONLY. --force has no
    # inline meaning anymore (a measure run never re-times) - it applies to
    # --recompute only.
    if args.force:
        sys.exit("--force applies to --recompute only (a measure run is "
                 "cache-only).\n  Run:  bench/run.py -cl %s --recompute "
                 "--force%s" % (lobj.name, filt))
    if not mylang:
        sys.exit("error: mylang binary not found; build it (make -j) or pass "
                 "--mylang")

    # UP-FRONT staleness gate: verify EVERY selected comparison is cached +
    # fresh BEFORE timing anything. A stale/missing entry fails FAST with the
    # recompute command, so a comparison subprocess never runs mid-measurement
    # (its interference with the variance-gated MyLang reps is what made py-
    # vs cpp-mode behave differently). MyLang-only benches (no comparison
    # source) are fine - they just show "-".
    stale = [n for n in comparable
             if not cache_entry_fresh(lobj, n, scales[n], cache)]
    if stale:
        shown = ", ".join(stale[:12]) + (
            ", ..." if len(stale) > 12 else "")
        sys.exit("error: %d of %d selected %s comparison result(s) are STALE or "
                 "MISSING\n  (a bench's scale or its %s source changed): %s\n"
                 "Recompute them first (a separate, explicit step - the "
                 "comparison\nlanguages are cached and are never re-timed during "
                 "a measure run):\n  bench/run.py -cl %s --recompute%s"
                 % (len(stale), len(comparable), lobj.name, lobj.name, shown,
                    lobj.name, filt))

    print("mylang : %s%s" % (mylang,
                             "  (engine: -vm bytecode VM)" if args.vm
                             else "  (engine: -tw tree-walker)" if args.tw
                             else "  (engine: the default = bytecode VM)"))
    if has_base:
        print("baseline: %s%s" % (args.baseline,
                                  "  (engine: -tw tree-walker)"
                                  if args.vm else ""))
    comp_desc = args.python if lobj.name == "python" else (
        "%s (%s)" % (lobj.name, args.cxx) if lobj.name == "cpp" else lobj.name)
    print("compare: %-8s %s  (cached)" % (lobj.name, comp_desc))
    scale_note = ("global %d" % global_scale if global_scale is not None
                  else "per-bench from scales.txt")
    raised_reps = sum(1 for n in names if min_reps_map[n] > REP_STEPS[0])
    reps_note = ("fixed %d" % args.repeat if args.repeat > 0
                 else "adaptive 3->5->8" + ("" if not raised_reps
                      else " (%d w/ raised reps.txt baseline)" % raised_reps))
    prio = raise_priority()
    prio_note = ("nice %d (raised)" % prio if prio is not None and prio < 0
                 else "normal (run with sudo / cap_sys_nice for less noise)")
    print("scale  : %s    reps (min kept): %s    var<=%.1f%%    prio: %s\n"
          % (scale_note, reps_note, args.var_threshold * 100, prio_note))

    warn = optimization_warning(mylang)
    if warn:
        print(warn + "\n")

    comp_col = "%s(s)" % lobj.name       # e.g. python(s) / cpp(s)
    ratio_col = "my/%s" % lobj.name       # e.g. my/py / my/cpp
    if has_base:
        hdr = "%-24s %10s %10s %8s %10s %8s  %s" % (
            "benchmark", "base(s)", "mylang(s)", "cur/base",
            comp_col, ratio_col, "result")
    else:
        hdr = "%-24s %10s %10s %8s  %s" % (
            "benchmark", "mylang(s)", comp_col, ratio_col, "result")
    print(hdr)
    print("-" * len(hdr))

    rows = []
    ratios = []
    speedups = []
    noisy_unsettled = []   # (name, reps, variance) - variance STILL over the
                           # threshold at the LAST rep step (result unreliable;
                           # kept the min + continued, not abort). Needing more
                           # reps that DO settle is fine and is NOT flagged.
    for name in names:
        my_path = os.path.join(MY_DIR, name + ".my")

        # Engine flags (the VM is the binary's DEFAULT since 2026-07-18):
        # --vm passes it explicitly (needed for a PRE-flip binary, whose
        # default was the tree-walker - the cross-flip A/B recipe is `--vm`
        # on both sides); --tw runs the tree-walker. With --vm the BASELINE
        # runs -tw, so `--vm --baseline <same binary>` still gates the VM
        # against the tree-walker exactly as before the flip.
        eng = ["-vm"] if args.vm else (["-tw"] if args.tw else [])
        scale = scales[name]
        scale_arg = str(scale)
        my_cmd = [mylang] + eng + [my_path, scale_arg]

        # Measure the PRIMARY binary: adaptive reps until the variance gate is
        # met (or --repeat N for a fixed count). Needing more reps to settle is
        # NOT a problem - it just buys more best-of samples - so it is not
        # flagged; ONLY a bench whose variance never clears the threshold, even
        # at the LAST rep step, is surfaced (kept-min + a NOISY flag, below).
        # base + python then run the SAME rep count at the SAME scale, so the
        # A/B / ratio stay comparable. A per-bench reps.txt baseline (default 3)
        # is where the adaptive schedule STARTS.
        base_reps = min_reps_map[name]
        if args.repeat > 0:
            my_t, my_out, my_err, n_reps, my_v = measure_fixed(
                my_cmd, args.repeat, args.timeout)
        else:
            my_t, my_out, my_err, n_reps, my_v = measure_adaptive(
                my_cmd, args.var_threshold, args.timeout, min_reps=base_reps)
        # A bench still over the gate at the LAST rep step doesn't ABORT the
        # whole suite (that throws away 75 good results because ONE bench hit a
        # transient scheduling burst - a false positive on a well-scaled bench;
        # see REP_STEPS). Instead we KEEP its min (already the best-of-21), flag
        # it, and CONTINUE - so a full run always completes. A genuinely
        # under-scaled bench is still surfaced, loudly, in the closing report.
        # Unsettled = the variance is STILL over the threshold after the last
        # rep step: the result is genuinely unreliable, so keep the min, flag
        # it, and continue (don't abort). Settling within more reps is fine and
        # is NOT flagged.
        unsettled = bool(my_err) and my_err.startswith("variance")
        if unsettled:
            my_err = None          # keep the min; treat as a (noisy) result
            noisy_unsettled.append((name, n_reps, my_v))

        base_t = None
        if has_base:
            beng = ["-tw"] if args.vm else []
            bt, _bout, _berr = run_reps(
                [args.baseline] + beng + [my_path, scale_arg],
                n_reps, args.timeout)
            base_t = min(bt) if bt else None

        # Comparison language (python by default): CACHE-ONLY here. The up-front
        # staleness gate already proved every selected bench is a fresh hit, so
        # this returns the cached time WITHOUT running any subprocess - a
        # comparison run can never interleave with the variance-gated MyLang
        # reps above. (Stale/missing was rejected before the loop; recompute is
        # the separate --recompute step.) The cached compare was timed at this
        # SAME per-bench scale, so the ratio is valid.
        comp_reps = args.repeat if args.repeat > 0 else COMP_REPS
        py_t, py_out, py_err, from_cache = comp_time(
            lobj, name, scale, comp_reps, args.timeout, cache,
            refresh=False, cache_only=True)

        if my_err:
            status = "MY " + my_err
        elif py_err and py_err.startswith("no-"):
            status = "my-only"
        elif py_err:
            status = "%s %s" % (lobj.name.upper(), py_err)
        elif results_match(my_out, py_out):
            status = "ok" + ("~" if from_cache else "")   # ~ = cached compare
        else:
            status = "DIFF: my=%r %s=%r" % (my_out, lobj.name, py_out)
        if unsettled:
            status += " NOISY[%dr]" % n_reps   # never cleared gate; min kept

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

    # (No cache write: a measure run is cache-ONLY - the comparison cache is
    # populated only by the separate --recompute step.)

    if ratios:
        gm_v = geomean(ratios)
        gm = colorize_ratio(gm_v, "%.3fx" % gm_v)
        if gm_v >= 1.0:
            tail = "MyLang is ~%.2fx slower than %s" % (gm_v, lobj.name)
        else:
            tail = "MyLang is ~%.2fx faster than %s" % (1.0 / gm_v, lobj.name)
        print("-" * len(hdr))
        print("geomean my/%s over %d paired benchmarks: %s (%s)"
              % (lobj.name, len(ratios), gm, tail))

    if speedups:
        sp_v = geomean(speedups)
        sp = colorize_ratio(sp_v, "%.3fx" % sp_v)
        if sp_v < 1.0:
            tail = "current is ~%.2fx FASTER than baseline" % (1.0 / sp_v)
        else:
            tail = "current is ~%.2fx SLOWER than baseline" % sp_v
        print("geomean cur/base over %d benchmarks: %s (%s)"
              % (len(speedups), sp, tail))

    # The ONLY volatility warning: a bench whose variance is STILL over the
    # threshold after the LAST rep step - the min can't be trusted. Needing more
    # (but bounded) reps to settle is normal and is NOT reported: more reps just
    # means more best-of samples, not a problem. A recurring entry here wants a
    # bigger scale (tune_scales.py) or reps baseline (bench/reps.txt).
    if noisy_unsettled:
        print("\nWARNING: %d bench(es) still exceeded the %.1f%% variance gate "
              "after the last rep step - their result is UNRELIABLE (kept the "
              "min, did NOT abort the run). Raise the bench's scale in "
              "bench/scales.txt (tune_scales.py) or its reps baseline in "
              "bench/reps.txt:"
              % (len(noisy_unsettled), args.var_threshold * 100))
        for name, reps, v in noisy_unsettled:
            print("  %-24s %d reps, var %.1f%%" % (name, reps, v * 100))

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
        cl = lobj.name
        with open(args.csv, "w") as f:
            if has_base:
                f.write("benchmark,base_s,mylang_s,cur_over_base,"
                        "%s_s,my_over_%s,status\n" % (cl, cl))
                for (name, base_s, my_s, py_s, speedup_s, ratio_s,
                     status, _sp, _ratio) in rows:
                    f.write("%s,%s,%s,%s,%s,%s,%s\n" %
                            (name, base_s, my_s, speedup_s.rstrip("x"), py_s,
                             ratio_s.rstrip("x"), status.replace(",", ";")))
            else:
                f.write("benchmark,mylang_s,%s_s,my_over_%s,status\n" % (cl, cl))
                for (name, _base_s, my_s, py_s, _speedup_s, ratio_s,
                     status, _sp, _ratio) in rows:
                    f.write("%s,%s,%s,%s,%s\n" %
                            (name, my_s, py_s, ratio_s.rstrip("x"),
                             status.replace(",", ";")))
        print("\nwrote %s" % args.csv)


if __name__ == "__main__":
    main()
