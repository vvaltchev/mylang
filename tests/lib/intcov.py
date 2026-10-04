# SPDX-License-Identifier: BSD-2-Clause
#
# intcov - the INT-build helpers the intrusive-test tools share
# (tests/int_run, tests/int_select, tests/mutate): reading `mylang -v`,
# running a process, masking a -vdj dump, and THE COVERAGE UNIVERSE -
# gcov's branch and MC/DC elements of the product code, named
# `kind:file:function:+offset` so an edit elsewhere renames nothing.
# The rules of the universe (what is product code, what is exempt) are
# described in tests/int_run's header and the root CLAUDE.md.

import glob
import gzip
import json
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
EXEMPT = "INT-COV-EXEMPT"
# the samples the census runs: the ones that read no stdin and call no
# rand() (phonebook / shopping read input; rand_sort is random)
CENSUS_SAMPLES = ["fib", "gcd", "loop", "primes", "primes2", "strloop"]

# The universe is the PRODUCT, as in SQLite: the test harness and the
# intrusive instrumentation are not measured. A test's failure arm never
# runs while the test passes, so counting it would make 100% impossible
# for a reason that has nothing to do with testing. Whole files, plus every
# INT helper - which is why they are named int_* / jit_int_* / bc_int_*.
TEST_FILES = {"src/tests.cpp", "src/inttest.cpp", "src/inttest.h",
              "src/intsites.h", "src/builtins/inttest.cpp.h"}
# `[(<[]`: a TEMPLATE helper demangles as `int_enumerate<...>(`, and one
# returning std::string as `bc_int_key_fn[abi:cxx11](` - a bare `\(` let
# either count as product code. `$`: an `extern "C"` helper (the JIT
# probe's checker, called from assembly) is not mangled, so gcov names it
# with no parameter list at all
INT_HELPER = re.compile(r"(^|[\s:*&])(int_|jit_int_|bc_int_)\w*([(<\[]|$)")


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


PROBE_LINE = re.compile(r"^\s*\.\s*\+\s*\d+: call \[<addr>\]\s*$", re.M)
_VDJ_OFF = re.compile(r"^(\s*\.\s*)\+\s*\d+:", re.M)
_VDJ_TGT = re.compile(r"(\b(?:j[a-z]+|call|loop[a-z]*)\s+)\+\d+")
_VDJ_FRAG = re.compile(r"@\+\d+")


def vdj_mask(text, strip=False):
    """A -vdj dump with every BYTE OFFSET masked (an instruction's own,
    a jump/call target, a fragment's `frag@+N`), and with strip, the
    probe calls dropped - what the probe-invisibility check compares."""
    if strip:
        text = PROBE_LINE.sub("", text)
    text = _VDJ_OFF.sub(r"\1+N:", text)
    text = _VDJ_TGT.sub(r"\1+N", text)
    text = _VDJ_FRAG.sub("@+N", text)
    return [l for l in text.splitlines() if l.strip()]


def run(cmd, env, timeout, stdin=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env,
                       timeout=timeout, errors="surrogateescape",
                       **({"input": stdin} if stdin is not None
                          else {"stdin": subprocess.DEVNULL}))
    return p.returncode, p.stdout, p.stderr


# ------------------------------------------------------------- coverage --

INT_SPAN = re.compile(r"\bML_INT(_ONLY)?\s*\(")


def exempt_lines(rel, cache):
    """1-based line numbers of `rel` outside the universe: a line carrying
    the INT-COV-EXEMPT marker, and every line of an `ML_INT(...)` or
    `ML_INT_ONLY(...)` call - the instrumentation's own branches inside a
    product function (a guard on an event, an INT-only bookkeeping `if`)
    are not product code, any more than an INT helper function is. The
    span is found by counting parentheses from the macro name (string and
    char literals skipped), so a multi-line ML_INT_ONLY block is covered
    whole."""
    if rel not in cache:
        marked = set()
        try:
            with open(os.path.join(ROOT, rel), errors="replace") as f:
                lines = f.readlines()
        except OSError:
            lines = []
        depth = 0
        for n, text in enumerate(lines, 1):
            if EXEMPT in text:
                marked.add(n)
            i = 0
            if depth == 0:
                m = INT_SPAN.search(text)
                if not m:
                    continue
                i = m.end()
                depth = 1
            marked.add(n)
            quote = None
            while i < len(text) and depth > 0:
                c = text[i]
                if quote:
                    if c == "\\":
                        i += 1
                    elif c == quote:
                        quote = None
                elif c in "\"'":
                    quote = c
                elif c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                i += 1
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
    # EVERY object must carry notes: make does not track flags, so a lane
    # rebuilt with GCOV=1 after a plain build recompiles only what changed
    # and leaves the rest un-instrumented - the universe then silently
    # loses whole files (watched: 7 of 23 objects, 70k elements of 114k)
    missing = []
    for obj in glob.glob(os.path.join(build_dir, "**", "*.o"),
                         recursive=True):
        # the notes sit beside the object, named after it minus `.o`:
        # Makefile `x.o` -> `x.gcno`, CMake `x.cpp.o` -> `x.cpp.gcno`
        if not os.path.exists(obj[:-2] + ".gcno"):
            missing.append(os.path.relpath(obj, build_dir))
    if missing:
        raise RuntimeError(
            "%d object(s) have no coverage notes (a lane built partly "
            "without GCOV=1 - `make clean` it): %s"
            % (len(missing), " ".join(sorted(missing)[:8])))
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
    # a value may continue on following `#   ...` lines (3+ spaces), so
    # a long configuration list stays within 80 columns
    with open(prog) as f:
        lines = f.read().split("\n")
    for i, line in enumerate(lines):
        if not line.startswith("#"):
            break
        if line.startswith("# " + key + ":"):
            val = line.split(":", 1)[1].strip()
            for more in lines[i + 1:]:
                if not more.startswith("#   "):
                    break
                val += " " + more[1:].strip()
            return val
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


# an engine name -> its command-line flags. `vm` is the default engine
# spelled out, `nbi` the bytecode inliner off, `noopt` every AST
# transform off (`--no-opt all`); `+` joins two (`nbi+nj`, `tw+noopt`).
ENGINE_FLAGS = {"default": [], "tw": ["-tw"], "nj": ["-nj"],
                "vm": ["-vm"], "nbi": ["-nbi"],
                "noopt": ["--no-opt", "all"]}


def engine_flags(eng):
    out = []
    for part in eng.split("+"):
        out += ENGINE_FLAGS[part]
    return out


def int_engines(prog):
    """The engines a tests/int program runs under: its `# INT-ENGINES:`
    header line (names from ENGINE_FLAGS, joined with `+`), else both the
    default engine and the tree-walker. A test asserting on a CODEGEN or
    JIT decision names `default` alone - the tree-walker never runs those
    passes, so it records nothing there."""
    h = int_header(prog, "INT-ENGINES")
    if not h:
        return ["default", "tw"]
    engs = h.split()
    for e in engs:
        if any(p not in ENGINE_FLAGS for p in e.split("+")):
            raise SystemExit("int_run: %s: unknown engine %r" % (prog, e))
    return engs


# --------------------------------------------------------------- driver --
