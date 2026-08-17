# tests/ — auxiliary test tools (NOT the `-rt` unit suite)

These are standalone test *programs*, separate from the built-in `-rt` unit
suite (`src/tests.cpp`). They exist for kinds of testing that can't be
exhaustively hard-coded — mainly **differential fuzzing** of the interpreter
against CPython — or that `-rt` structurally cannot reach. They have no
third-party dependencies (Python 3 standard library only, or POSIX `sh`).

Two of them run in CI (`.github/workflows/linux.yml`); the rest are run by
hand. The full set:

- **`nested_fuzz.py`** — random deeply-nested programs, compared across
  tree-walker / VM / CPython / optimizers-off.
- **`corpus_diff.sh`** — tree-walker vs the default engine over
  `tests/functional/` + `samples/`, plus the `--levers` / `--cold` JIT
  matrices.
- **`myv_fuzz.py`** — a mutated `.myv` must never crash or hang the loader.
- **`repl_fuzz.py`** — template-generated REPL sessions (the REPL has its
  own inferencer, retained ASTs and open-world globals).
- **`myv_doc_check.py`** — *CI*. A reader written from `docs/myv-format.txt`
  alone must consume every image to exactly EOF.
- **`driver_checks.sh`** — *CI*. The CLI flags do what they say.

### Why `driver_checks.sh` exists

`-rt` runs **in-process**: it calls lexer/parser/infer/resolve directly and
never goes through `mylang.cpp`'s argument handling. So a flag wired to the
wrong side of an `if` is invisible to every one of its ~1866 tests. That is
not hypothetical — `-nr` ("compile and validate, don't run") called
`run_optimizers` only when it was going to *run*, so the step 7 prover, the
whole warning tier, FIX-1, the TDZ and the duplicate-decl check were all
skipped, and `-nr` exited 0 in silence on a program a plain run refuses
(#147). Reverting that wiring fails 7 of its 9 checks while `-rt` stays
green at 1866/1866.

## `nested_fuzz.py` — deep-nesting differential fuzzer

Generates thousands of random, **deeply-nested** MyLang programs (if / while /
counted-for / general-for, arbitrarily nested) full of side effects — a fixed
array with reads/writes at **arbitrary expression indices** (`A[<large expr>]`,
including nested `A[A[A[i]]]`) + `foreach`, a bounded growing array with
`append`, a dict, seven cross-level scalar accumulators, per-level temp
variables, `break`/`continue`, and rich value expressions (`+ - * % & | ^`,
comparisons used as `0/1`, `min`/`max`/`len`) — together with a
**semantically-identical Python** twin of each, then checks

```
tree-walker result  ==  VM (-vm) result  ==  CPython result
```

on every program.

**Why:** for the recursive tree-walker, "N levels of nesting work" is almost
self-evident. For the **flat bytecode VM** it is not — nested loops/ifs compile
into one linear instruction stream with jump backpatching and reused scratch
registers, so deep nesting can expose codegen bugs (a wrong backpatch target, a
temp-slot collision across levels, a `break`/`continue` targeting the wrong
loop, array COW under mutation) that never arise in the tree-walker. CPython is
the independent oracle that also validates the tree-walker itself.

The generator stays strictly inside the documented MyLang/Python **equivalence
subset** (see `bench/README.md`), so any mismatch is a real interpreter bug, not
a known semantic difference: every value is kept in `[0, MOD)` (operands are
non-negative before each `%` — MyLang truncates, Python floors), no 64-bit
overflow, no division (`/` truncates vs floats), and dicts use only fixed
integer keys read by index (never iterated — MyLang is unordered).

### Usage

```
python3 tests/nested_fuzz.py                 # 1000 programs, depth <= 15
python3 tests/nested_fuzz.py --count 5000 --max-depth 20
python3 tests/nested_fuzz.py --check-fallbacks   # also flag any AST fallback op
python3 tests/nested_fuzz.py --keep-failures /tmp/ff   # save diverging cases
python3 tests/nested_fuzz.py --engines vm,py   # compare only VM vs CPython
python3 tests/nested_fuzz.py --mylang build/mylang --seed 1 -v
```

Every program `i` uses `--seed + i`, so a reported failure is exactly
reproducible: rerun with `--seed <that> --count 1` (or `--keep-failures` to dump
the offending `.my`/`.py`). Exit code is non-zero if any program diverges (or,
with `--check-fallbacks`, compiles to an AST fallback op).

### Primary check = RESULT correctness; `--check-fallbacks` is a bonus audit

The default run asserts only that **the three engines produce the same result**
— that is the guarantee that matters, and it holds on every generated program.
`--check-fallbacks` is a separate, optional audit of native *coverage* (did the
program compile to bytecode with no AST-fallback op?). Deep NESTING is fully
native; but the maximally-varied EXPRESSIONS the fuzzer emits do surface one
benign, orthogonal gap: a **comparison result used as an int** (`0/1`) inside a
**flat-array index or value** (`A[(s > t) + r] = v`) isn't materialized on the
native int path, so that statement drops to the tree-walker — with an identical
result. It is correct, just not native, and unrelated to nesting. So expect a
nonzero `--check-fallbacks` count with the full expression palette; the result
agreement is what proves correctness.

### What it has already caught

- A **VM codegen gap**: a const-folded `if (true) { ... }` leaves a bare nested
  `{ ... }` block, which the loop-body compiler didn't handle → the whole
  enclosing loop fell back to one `EvalStmt`. Fixed in `codegen.cpp`
  (`compile_scalar_body` now compiles a scope-free bare block in place).
- Several **generator/translation bugs** in the harness itself (a `%`-format
  escaping slip; an expression evaluated separately for the `.my` and `.py`
  sides so they diverged) — exactly the class of subtle bug a hand-written test
  would hide, and the reason three-way agreement (incl. CPython) is the check.
