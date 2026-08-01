# Fuzzer variability roadmap (`tests/nested_fuzz.py`)

The deep-nesting differential fuzzer generates random MyLang programs + Python
twins and checks `tree-walker == VM == CPython`. The initial generator was
STRUCTURALLY uniform: every statement was "mutate a scalar/array/dict slot with
an arithmetic expression." Bigger numbers / more terms are *more work, not more
variety* (`1+1+1` is not more complex than `1+1`). This plan adds QUALITATIVELY
different constructs, one per increment, each validated by a fuzz run and
committed separately.

## Hard invariants (every construct must preserve these)

Stay inside the documented MyLang/Python **equivalence subset** (see
`bench/README.md`), so a mismatch is a real interpreter bug:
- every value non-negative and small (`% MOD`, MOD=100) - MyLang `%` truncates,
  Python floors; they diverge on a negative operand;
- no 64-bit overflow (products of two <MOD values fit);
- NO division (`/` truncates in MyLang, is float in Python);
- dicts: fixed integer keys read by index, never iterated (MyLang unordered);
- a value/text emitted for BOTH engines must be built ONCE (a double-eval so the
  `.my` and `.py` sides diverge was a real bug we already hit);
- constructs whose SYNTAX differs (ternary, `func`, `pop`, multi-assign, loop
  headers) emit an explicit `(my, py)` pair; identical-syntax ones (arithmetic,
  comparisons, `f(x)` calls, `A[i][j]`, `A[i:j]`, `min`/`max`/`len`) can share a
  single string.

## Constructs to add (status)

Order = manage risk first, biggest/most-valuable last. Each: implement -> fuzz
200-500 (fix translation bugs) -> `--show` sanity -> commit.

- [x] **1. Ternary / conditional values** - `cond ? a : b` (MyLang) vs
      `a if cond else b` (Python). A VALUE that depends on control flow, not a
      branch statement. Emitted as a `(my, py)` pair at the value level (not
      nested inside arithmetic, to avoid an expr()->pair refactor).
- [x] **2. More loop shapes** - a DOWN-counting `for (i = N; i > 0; i--)` and a
      DATA-DEPENDENT-bound `while (s < limit)` (bound changes during the loop;
      needs a guaranteed-progress counter to terminate). Reverse iteration +
      changing bounds.
- [x] **3. Nested arrays `M[i][j]`** - a real 2-D structure (an array of
      arrays). Multi-level subscript load/store + COW on inner arrays. Identical
      syntax in both engines.
- [x] **4. Data-structure ops** - `pop`/`insert`/`erase` on the growing array
      (mutating length mid-loop; guarded so it never underflows), array SLICES
      `A[i:j]` (aliasing / COW), `clone`. `pop`/`insert`/`erase` are
      MyLang-builtin vs Python-method -> `(my, py)` pairs; slices identical.
- [x] **5. Multi-assignment / destructuring** - `var a, b = [x, y]` (MyLang) vs
      `a, b = x, y` (Python), incl. the swap idiom `a, b = [b, a]`. STRICT
      arity (MyLang throws on a length mismatch - generate exact-length only).
- [x] **6. Nested functions (highest value)** - top-level `func f(a, b) { ...
      return ...; }` (and expression-bodied `=>`), called as an operand `f(x,
      y)` (identical call syntax). Includes a BOUNDED self-recursive helper
      (e.g. a small fib/gcd with a non-negative, depth-limited arg). Exercises
      call/return, argument binding, own frame/scope, recursion - a whole
      dimension the VM handles very differently from straight-line code. Defs
      are `(my, py)` statement pairs at program top; calls are single-string
      operands.

## Done

All 6 constructs landed (see the checklist above), each in its own
commit. Final validation: 1500 random programs at depth <= 20 - 0 diverged
(tree-walker == VM == CPython). The generator now uses small numbers (MOD=100,
readable) and its variability is STRUCTURAL (construct/operation kinds), not
expression length. Bugs the differential caught along the way: a bare-block
loop fallback (real VM codegen bug, fixed), a `%%`-escaping slip, a
double-eval that diverged the .my/.py sides, and an unbounded slice-sum that
broke the non-negative invariant (negative `%` diverges + OOB erase index).
