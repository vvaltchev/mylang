#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# THE INTRUSIVE-TEST RUNNER (#107, plans/intrusive-tests.md).
#
#   tests/int_run.py BINARY [--no-rt] [--gcov [--require-floor] [--report F]]
#
# 1. REFUSES a binary whose `mylang -v` does not say `int_tests 1`: every
#    check below would be vacuous on an ordinary build.
# 2. Runs the TEST UNITS, each one an oracle:
#      `-rt`                  the C++ half (it exits non-zero on a failure);
#      every tests/int/*.my   under each configuration of its
#                             `# INT-CONFIGS:` header (default: none) and
#                             each engine of `# INT-ENGINES:` (default: the
#                             default engine AND the tree-walker) - each
#                             run must exit 0 (the programs self-assert),
#                             and the engines of one configuration must
#                             print the same stdout.
# 3. THE SITE CENSUS: every process appends `site hits queries` to one file
#    (MYLANG_INT_OUT, src/inttest.cpp). A site of src/intsites.h that no
#    test CHECKED (read through int_hits / int_events) fails - reaching a
#    site and asserting nothing about it verifies nothing.
# 4. --gcov (a GCOV=1 INT build): the COVERAGE UNIVERSE of plan section 9.
#    Each unit runs with the gcov counters cleared first, so it yields its
#    own coverage vector of
#        br:   each non-exception branch outcome in src/
#        mcdc: each condition of each decision, shown TRUE and FALSE on its
#              own (GCC >= 14, -fcondition-coverage)
#        site: each intrusive-test site the unit checked
#    named `kind:file:function:+line-offset:...` so an edit elsewhere in a
#    file does not rename them. The universe is the PRODUCT: the test
#    harness (tests.cpp), the INT core and every INT helper function (named
#    int_* / jit_int_* / bc_int_*) are outside it, and so is a line marked
#    `INT-COV-EXEMPT: reason`. Two checks, as in SQLite's discipline:
#      - every unit OWNS at least one element no other unit covers
#        (otherwise it is redundant and must go - the suite is minimal);
#      - uncovered elements <= the FLOOR (a ratchet: it may only go down).
#        Branch and condition counts depend on the COMPILER, so the floor
#        file (--floor-file, default tests/int/coverage-floor.txt) holds one
#        `<compiler> <N>` line per compiler `mylang -v` can report; with
#        --require-floor a compiler with no line fails, naming the count to
#        record.
#
# Deterministic: no seeds, no sampling, a fixed unit order. The random-
# program fuzzers never run an INT binary (plan section 7).

import argparse
import glob
import gzip
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXEMPT = "INT-COV-EXEMPT"

# The universe is the PRODUCT, as in SQLite: the test harness and the
# intrusive instrumentation are not measured. A test's failure arm never
# runs while the test passes, so counting it would make 100% impossible
# for a reason that has nothing to do with testing. Whole files, plus every
# INT helper - which is why they are named int_* / jit_int_* / bc_int_*.
TEST_FILES = {"src/tests.cpp", "src/inttest.cpp", "src/inttest.h",
              "src/intsites.h", "src/builtins/inttest.cpp.h"}
INT_HELPER = re.compile(r"(^|[\s:*&])(int_|jit_int_|bc_int_)\w*\(")


def build_config(binary):
    out = subprocess.run([binary, "-v"], capture_output=True, text=True,
                         timeout=30).stdout
    cfg = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            cfg[parts[0]] = parts[1]
            if parts[0] == "compiler":       # `compiler gcc 16.2`
                cfg["compiler"] = " ".join(parts[1:3])
    return cfg


def run(cmd, env, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env,
                       timeout=timeout)
    return p.returncode, p.stdout, p.stderr


# ------------------------------------------------------------- coverage --

def exempt_lines(rel, cache):
    """1-based line numbers of `rel` carrying the INT-COV-EXEMPT marker."""
    if rel not in cache:
        marked = set()
        try:
            with open(os.path.join(ROOT, rel), errors="replace") as f:
                for n, text in enumerate(f, 1):
                    if EXEMPT in text:
                        marked.add(n)
        except OSError:
            pass
        cache[rel] = marked
    return cache[rel]


def gcov_tool(cfg):
    """The gcov that matches the compiler: gcov reads the .gcno/.gcda format
    of ITS OWN GCC version and fails on another's. $GCOV wins; else, for
    `compiler gcc 14.2`, `gcov-14` when it exists (a runner whose default
    gcc is older), else plain `gcov`."""
    if os.environ.get("GCOV"):
        return os.environ["GCOV"]
    comp = cfg.get("compiler", "")
    if comp.startswith("gcc "):
        major = comp.split()[1].split(".")[0]
        for d in os.environ.get("PATH", "").split(os.pathsep):
            if os.access(os.path.join(d, "gcov-" + major), os.X_OK):
                return "gcov-" + major
    return "gcov"


def collect(build_dir, scratch, ex_cache, tool):
    """gcov every object of `build_dir`; return {element: covered?}."""
    for g in glob.glob(os.path.join(scratch, "*.gcov.json.gz")):
        os.remove(g)
    # Makefile objects sit next to the binary; CMake's under
    # CMakeFiles/mylang.dir/src - so find every .gcno, gcov per directory.
    by_dir = {}
    for gcno in glob.glob(os.path.join(build_dir, "**", "*.gcno"),
                          recursive=True):
        base = gcno[:-len(".gcno")]
        obj = next((base + ext for ext in (".o", ".cpp.o")
                    if os.path.exists(base + ext)), None)
        if obj:
            by_dir.setdefault(os.path.dirname(obj), []).append(obj)
    for d, objs in sorted(by_dir.items()):
        try:
            r = subprocess.run([tool, "--json-format",
                                "--branch-probabilities", "--conditions",
                                "-o", d] + sorted(objs),
                               cwd=scratch, capture_output=True, text=True,
                               timeout=1800)
        except OSError as err:
            raise RuntimeError("cannot run %s: %s" % (tool, err))
        if r.returncode != 0:
            raise RuntimeError("%s failed in %s:\n%s" % (tool, d,
                                                       r.stderr[-2000:]))
    elems = {}
    for g in sorted(glob.glob(os.path.join(scratch, "*.gcov.json.gz"))):
        with gzip.open(g, "rt") as f:
            data = json.load(f)
        for fobj in data.get("files", []):
            rel = fobj["file"].replace("\\", "/")
            if "/src/" in rel and not rel.startswith("src/"):
                rel = rel[rel.rindex("/src/") + 1:]  # an absolute CMake path
            if not rel.startswith("src/") or rel in TEST_FILES:
                continue
            funcs = sorted((fn["start_line"], fn["end_line"],
                            fn.get("demangled_name", fn["name"]))
                           for fn in fobj.get("functions", []))
            marked = exempt_lines(rel, ex_cache)

            def owner(line):
                best = None
                for s, e, name in funcs:
                    if s <= line <= e and (best is None or s >= best[0]):
                        best = (s, name)
                return best

            for ln in fobj.get("lines", []):
                n = ln["line_number"]
                if n in marked:
                    continue
                o = owner(n)
                if o and INT_HELPER.search(o[1]):
                    continue
                where = ("%s:%s:+%d" % (rel, o[1], n - o[0]) if o
                         else "%s:?:%d" % (rel, n))
                k = 0
                for b in ln.get("branches", []):
                    if b.get("throw"):
                        continue
                    name = "br:%s:%d" % (where, k)
                    k += 1
                    elems[name] = elems.get(name, False) or b["count"] > 0
                for j, dec in enumerate(ln.get("conditions", [])):
                    nconds = dec["count"] // 2
                    for c in range(nconds):
                        for tag, miss in (("T", dec["not_covered_true"]),
                                          ("F", dec["not_covered_false"])):
                            name = "mcdc:%s:%d:%d:%s" % (where, j, c, tag)
                            elems[name] = elems.get(name, False) \
                                or c not in miss
    return elems


def int_header(prog, key):
    """The value of a `# KEY:` line in the program's leading comment."""
    with open(prog) as f:
        for line in f:
            if line.startswith("# " + key + ":"):
                return line.split(":", 1)[1].strip()
            if not line.startswith("#"):
                break
    return None


def int_configs(prog):
    """The CONFIGURATIONS a tests/int program runs under: its
    `# INT-CONFIGS:` line, `;`-separated, each `default` or space-separated
    KEY=VALUE environment settings (`default ; MYLANG_JIT_OFF=lsra` runs it
    under both allocators). Every configuration must pass; stdout is
    compared only between ENGINES of one configuration, since another
    configuration may legitimately record different decisions."""
    h = int_header(prog, "INT-CONFIGS")
    if not h:
        return [{}]
    confs = []
    for part in h.split(";"):
        part = part.strip()
        env = {}
        if part != "default":
            for kv in part.split():
                k, _, v = kv.partition("=")
                env[k] = v
        confs.append(env)
    return confs


def int_engines(prog):
    """The engines a tests/int program runs under: its `# INT-ENGINES:`
    header line (`default`, `tw`, `nj`), else both the default engine and
    the tree-walker. A test asserting on a CODEGEN or JIT decision names
    `default` alone - the tree-walker never runs those passes, so it
    records nothing there."""
    h = int_header(prog, "INT-ENGINES")
    if not h:
        return ["default", "tw"]
    engs = h.split()
    for e in engs:
        if e not in ("default", "tw", "nj"):
            raise SystemExit("int_run: %s: unknown engine %r" % (prog, e))
    return engs


# --------------------------------------------------------------- driver --

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--no-rt", action="store_true",
                    help="skip the -rt unit (iterating on tests/int only)")
    ap.add_argument("--gcov", action="store_true",
                    help="measure the coverage universe (a GCOV=1 build)")
    ap.add_argument("--floor-file",
                    default=os.path.join(HERE, "int", "coverage-floor.txt"),
                    help="--gcov: `<compiler> <max uncovered>` lines")
    ap.add_argument("--require-floor", action="store_true",
                    help="--gcov: fail when the floor file has no line for "
                         "this binary's compiler")
    ap.add_argument("--report", default=None,
                    help="--gcov: write the ownership table and the "
                         "uncovered list here")
    ap.add_argument("--timeout", type=float, default=3600.0)
    args = ap.parse_args()
    binary = os.path.abspath(args.binary)
    build_dir = os.path.dirname(binary)

    cfg = build_config(binary)
    if cfg.get("int_tests") != "1":
        print("int_run: %s is not an INT_TESTS build (-v says int_tests=%s);"
              " build it with `make INT_TESTS=1`" % (binary,
                                                     cfg.get("int_tests")),
              file=sys.stderr)
        return 2
    if args.gcov and not glob.glob(os.path.join(build_dir, "**", "*.gcno"),
                                   recursive=True):
        print("int_run: --gcov needs a GCOV=1 build (no .gcno files next "
              "to %s)" % binary, file=sys.stderr)
        return 2

    tool = gcov_tool(cfg) if args.gcov else None
    if tool:
        print("  gcov: %s" % tool)
    units = []
    if not args.no_rt:
        units.append(("-rt", [[(["-rt"], {})]]))
    progs = sorted(glob.glob(os.path.join(HERE, "int", "*.my")))
    for prog in progs:
        groups = []          # one per configuration: [(cmd, env-extra)]
        for conf in int_configs(prog):
            groups.append([([prog] if eng == "default" else ["-" + eng, prog],
                            conf) for eng in int_engines(prog)])
        units.append((os.path.relpath(prog, ROOT), groups))

    failures = []
    if not progs:
        failures.append("no tests/int/*.my programs")
    covered_by = {}
    universe = {}
    ex_cache = {}
    with tempfile.TemporaryDirectory(prefix="mylang-int-") as tmp:
        census = os.path.join(tmp, "census.txt")
        scratch = os.path.join(tmp, "gcov")
        os.mkdir(scratch)
        env = dict(os.environ, MYLANG_INT_OUT=census, TMPDIR=tmp)

        for name, cmds in units:
            if args.gcov:
                for g in glob.glob(os.path.join(build_dir, "**", "*.gcda"),
                                   recursive=True):
                    os.remove(g)
            before = os.path.getsize(census) if os.path.exists(census) else 0
            ok = True
            fails = []
            for group in cmds:
                results = [run([binary] + c, dict(env, **extra), args.timeout)
                           for c, extra in group]
                if name == "-rt":
                    for l in results[0][1].splitlines():
                        if l.startswith(("Tests passed", "Differential")):
                            print("  -rt  " + l)
                if any(r[0] != 0 for r in results):
                    ok = False
                if len(results) >= 2 and any(r[1] != results[0][1]
                                             for r in results[1:]):
                    ok = False
                    fails.append("stdout differs between the engines (%s)"
                                 % (" ".join("%s=%s" % kv for kv in
                                             group[0][1].items())
                                    or "default"))
                for (c, extra), r in zip(group, results):
                    if r[0] != 0:
                        fails.append("`%s%s` rc=%d\n%s" % (
                            "".join("%s=%s " % kv for kv in extra.items()),
                            " ".join(c), r[0], r[2][-2000:]))
            print("  %s  %s" % ("ok  " if ok else "FAIL", name))
            for f in fails:
                print("        " + f)
            if not ok:
                failures.append(name)

            if args.gcov:
                try:
                    elems = collect(build_dir, scratch, ex_cache, tool)
                except RuntimeError as err:
                    print("int_run: %s" % err, file=sys.stderr)
                    return 2
                # THE VACUITY GUARD: a coverage pass that measured nothing
                # must not pass (a gcov of the wrong version once returned
                # nothing here, silently, and every check was green).
                if not any(e.startswith(("br:", "mcdc:")) for e in elems):
                    print("int_run: gcov reported no src/ branch at all for "
                          "%s - the coverage pass measured nothing" % name,
                          file=sys.stderr)
                    return 2
                cov = {e for e, c in elems.items() if c}
                for e in elems:
                    universe.setdefault(e, True)
                # the sites this unit CHECKED are elements too
                with open(census) as f:
                    f.seek(before)
                    for line in f:
                        site, _hits, queries = line.split()
                        universe.setdefault("site:" + site, True)
                        if int(queries) > 0:
                            cov.add("site:" + site)
                covered_by[name] = cov

        totals = {}
        if os.path.exists(census):
            with open(census) as f:
                for line in f:
                    site, hits, queries = line.split()
                    h, q = totals.get(site, (0, 0))
                    totals[site] = (h + int(hits), q + int(queries))
        if not totals:
            failures.append("no census was written (MYLANG_INT_OUT)")
        unchecked = sorted(s for s, (h, q) in totals.items() if q == 0)
        print("  census: %d site(s), %d not checked by any test"
              % (len(totals), len(unchecked)))
        for s in unchecked:
            print("    UNCHECKED  %s  (%d event(s) recorded)"
                  % (s, totals[s][0]))
            failures.append("site %s checked by no test" % s)

    if args.gcov:
        all_cov = set().union(*covered_by.values()) if covered_by else set()
        uncovered = sorted(e for e in universe if e not in all_cov)
        print("  coverage: %d element(s), %d covered, %d uncovered"
              % (len(universe), len(all_cov), len(uncovered)))
        for kind in ("br", "mcdc", "site"):
            tot = sum(1 for e in universe if e.startswith(kind + ":"))
            cov = sum(1 for e in all_cov if e.startswith(kind + ":"))
            print("    %-5s %7d / %7d" % (kind, cov, tot))
        owned = {}
        for name, cov in covered_by.items():
            others = set().union(*(c for n, c in covered_by.items()
                                   if n != name))
            owned[name] = sorted(cov - others)
            print("    owns %7d  %s" % (len(owned[name]), name))
            if not owned[name]:
                failures.append("%s owns no element - redundant" % name)
        comp = cfg.get("compiler", "?")
        floor = None
        if os.path.exists(args.floor_file):
            with open(args.floor_file) as f:
                for line in f:
                    line = line.split("#", 1)[0].strip()
                    if not line:
                        continue
                    key, _, n = line.rpartition(" ")
                    if key == comp:
                        floor = int(n)
        if floor is None:
            msg = ("no coverage floor for `%s` in %s - record `%s %d`"
                   % (comp, os.path.relpath(args.floor_file, ROOT), comp,
                      len(uncovered)))
            print("  " + msg)
            if args.require_floor:
                failures.append(msg)
        elif len(uncovered) > floor:
            failures.append("%d uncovered elements, over the %s floor of %d"
                            % (len(uncovered), comp, floor))
        elif len(uncovered) < floor:
            print("  note: %d uncovered, below the %s floor of %d - lower "
                  "it in %s" % (len(uncovered), comp, floor,
                                os.path.relpath(args.floor_file, ROOT)))
        if args.report:
            with open(args.report, "w") as f:
                for name in sorted(owned):
                    f.write("== %s owns %d\n" % (name, len(owned[name])))
                    for e in owned[name]:
                        f.write("  %s\n" % e)
                f.write("== uncovered %d\n" % len(uncovered))
                for e in uncovered:
                    f.write("  %s\n" % e)

    if failures:
        print("int_run: FAIL (%d)" % len(failures))
        for f in failures:
            print("  - " + f)
        return 1
    print("int_run: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
