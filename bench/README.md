# MyLang performance benchmarks

A construct-by-construct performance comparison between **MyLang** (this
interpreter) and **CPython**, plus a small runner that times both and prints a
table.

> **TL;DR** — built `-O3`, MyLang holds up remarkably well for a tree-walking
> interpreter: **geomean ~1.5× slower than CPython** across the paired
> benchmarks, and actually *faster* on several (lazy slices, naive string `+=`,
> linear `find`, dict iteration, the plain counting loop). Most everyday
> constructs land within **0.8–2×**. The one real outlier left is
> **per-iteration *genuine* exceptions** (`42_exceptions` ~22×) — and that's by
> design (real `throw`/`catch` still uses C++ exceptions). Recursion *used* to
> be the worst case (`fib` ~31×) until `return`/`break`/`continue` were moved
> off C++ exceptions onto a flag (`FlowState`); that alone took fib to ~3.5×.
> See the investigation in the git history for why C++ `throw` is ~1.6µs and
> irreducible by build flags.
>
> ⚠️ **These numbers are only valid for an optimized build.** A debug /
> `TESTS` / no-`-O` build runs ~7× slower and makes the whole comparison
> meaningless; the harness times a probe loop at startup and warns if the
> binary it's about to use looks unoptimized.

## Layout

```
bench/
  my/NN_name.my     # the MyLang benchmark
  py/NN_name.py     # the equivalent Python benchmark (omitted if MyLang-only)
  run.py            # the runner / comparison harness
  results.csv       # last recorded run (regenerate with --csv)
```

Each `my`/`py` pair implements the *same* algorithm on the *same* data and
prints a `result:` line so the harness can confirm the two agree.

## Running

**Build an optimized interpreter first — this is mandatory.** A debug /
`TESTS` / no-`-O` build runs ~7× slower and every number becomes meaningless.
The project's release build is just `make -j` (it defaults to `-O3`, no tests):

```
make -j BUILD_DIR=build_rel           # -> build_rel/mylang  (-O3, no TESTS)
python3 bench/run.py --mylang ./build_rel/mylang
```

(Using a separate `build_rel/` keeps your `-rt`-capable debug `build/` intact.)
If you just run `make -j`, the optimized binary lands at `build/mylang`, which
is what the harness auto-discovers when `--mylang` is omitted. Either way the
harness probes the binary at startup and prints a warning if it looks
unoptimized. Other options:

```
python3 bench/run.py --scale 4        # 4x the per-benchmark workload
python3 bench/run.py --filter slice   # just the slice benchmarks
python3 bench/run.py --repeat 5 --csv bench/results.csv
```

Every script takes an optional integer **scale** argument (the harness passes
it through) that multiplies the base workload, so you can dial total runtime up
or down without editing anything. The harness reports the **best** wall-clock
time of `--repeat` runs for each side and the `my/py` ratio, and verifies the
two implementations printed matching results (numeric tokens are compared with
a tolerance so cosmetic float-formatting differences don't register as
mismatches).

Python is run with **`-B`** (don't read or write `__pycache__`). MyLang has no
bytecode cache — it re-parses its source on every run — so letting CPython
reuse a compiled `.pyc` across the repeated timing runs would be an unfair head
start. With `-B` both sides re-parse every run.

On a terminal, the `my/py` ratio is colored on a 256-color gradient — brightest
green for the biggest wins (ratio ≤ 0.35), smoothly fading to a neutral grey
across the break-even band (0.95–1.05), then to brightest red for the worst
regressions (ratio ≥ 3.0). Output that isn't a TTY (a pipe, or `--csv`) stays
plain.

## Are MyLang and Python semantically the same? (read this first)

You asked specifically whether the two behave the same way — especially for the
subtle stuff like copy-on-write slices. Here is the full reconciliation. Where
behavior matches, the benchmark is a fair speed comparison. Where it does
**not**, I either adjusted the benchmark to compare equivalent work or dropped
the Python side entirely, and it is flagged here.

### Same *observable* behavior, different *cost* — the COW story

This is the important one, and the answer is: **yes, MyLang slices behave like
Python slices semantically — they just don't copy eagerly.**

- **Array slices.** `a[i:j]` in Python eagerly copies the `j-i` elements into a
  brand-new list. In MyLang it returns an **O(1) view** that shares storage
  with `a` via a reference count plus a registered "slice" record. The copy is
  deferred: the moment you *write* through either the slice or the original, the
  affected storage is cloned (`get_value_for_put` → `clone_aliased_slices` /
  `clone_internal_vec`) so the write never bleeds across the two. **The net
  effect a script can observe is identical to Python's** — writing to a slice
  never changes the parent and vice-versa (verified in
  `verify_semantics.my`). Only the performance differs:
  - `15_array_slice_readonly` — take a slice and only read it. MyLang does O(1)
    work, Python pays the full O(k) copy every time. **MyLang wins big here**,
    and it's not because it's a faster interpreter — it's doing asymptotically
    less work. This is the headline copy-on-write benchmark.
  - `16_array_slice_write` — take a slice and immediately write to it. Now
    MyLang's deferred clone fires (O(k)), which is the same O(k) Python paid up
    front. The lazy-slice advantage is gone and MyLang's per-op interpreter
    overhead reasserts itself (~2.5×).

- **String slices.** Identical story, and even simpler because strings are
  immutable in both languages: `29_str_slice_readonly` slices a 1000-char
  string in a loop. MyLang takes O(1) views (`SharedStr` is a shared buffer +
  offset/len); Python copies. Here the two come out ~even (~1.0×): a Python
  string-slice is a fast `memcpy` of 1000 bytes, small enough that it roughly
  cancels MyLang's higher per-operation cost. The array case (`15`) is the
  dramatic one (**0.37×, MyLang ~3× faster**) because copying 1000 *list
  elements* in Python is much pricier than copying 1000 bytes.

So: **same semantics, fair to compare.** The lazy slice flatters MyLang most
when the copy Python avoids is genuinely expensive (arrays), and barely at all
when that copy is cheap (short string slices).

### Same behavior, fair comparison (the bulk of the suite)

Integer/float/mixed arithmetic, control flow, function calls, recursion,
closures, arrays (append/subscript/concat/reverse/iterate), dictionaries
(insert/lookup/iterate/member), `map`/`filter`/`sum`/`sort`/`min`/`max`/`find`,
string `split`/`join`, exceptions, and the composite algorithms (sieve, trial
-division primes, GCD, matrix multiply, word count) all behave the same way in
both languages on the inputs used. These are straight speed comparisons.

### Behaves differently — adjusted, or flagged, so the comparison stays honest

- **Integer overflow.** MyLang `int` is a fixed 64-bit signed integer that
  **wraps** (`-fwrapv`); Python `int` is arbitrary precision and silently
  promotes to bignum. A benchmark that overflowed 64 bits would stop comparing
  the same thing (MyLang keeps doing native math, Python slows down doing
  bignum math). **Every benchmark is kept inside 64 bits** (accumulators are
  reduced mod 1e9+7, fib index ≤ 29, etc.) so both do native-width integer work.

- **Division & modulo sign.** MyLang `/` is C-style truncating integer division
  and `%` follows C; Python `/` is float division and `//`/`%` floor toward
  negative infinity. For **non-negative** operands they agree, so the Python
  benchmarks use `//` and keep operands ≥ 0. (`-7/3` is `-2` in MyLang but
  `-7//3` is `-3` in Python — avoided on purpose.)

- **Float width.** MyLang `float` and Python `float` are both 64-bit IEEE-754
  `double`, so float results now match exactly (no low-digit divergence). One
  formatting caveat remains: MyLang's default `str(x)` of a float uses C
  `%f`-style fixed notation (e.g. `-249999750000.000000`) where Python prints
  the shortest round-trippable form (`-249999750000.0`), so the harness
  normalizes float text before comparing.

- **`range()`.** MyLang `range(n)` eagerly builds a full array (like Python 2).
  Idiomatic Python 3 `range` is a lazy generator. To compare the *same work*,
  `37_range_builtin.py` wraps it in `list(range(n))` to force the same
  allocation. (Idiomatic lazy `range` would make Python look even faster.)

- **`sort` with a comparator.** MyLang `sort(arr, cmp)` calls a `cmp(a,b)->bool`
  for each comparison. Python's `list.sort` is key-based; the apples-to-apples
  mapping for a per-comparison callback is `functools.cmp_to_key`, which
  `34_sort_custom_cmp.py` uses. With a *custom* comparator MyLang sorts via
  heapsort rather than introsort (a comparator is arbitrary script code and may
  not be a valid ordering; heapsort stays in bounds regardless, so a bad
  comparator can't read off the buffer). Heapsort does somewhat more
  comparisons, but each is a script call that dominates, so the algorithm
  choice is in the noise here.

- **String `+=` (`28_str_concat`).** Both produce the same string, but the cost
  profile is different and **MyLang is actually faster here**. MyLang appends in
  place to the backing `std::string` (amortized O(1)); CPython's in-place
  `+=` optimization does *not* make this O(1) in this configuration, so it
  degrades toward O(n²). Note MyLang's speed here relies on the string not being
  aliased; the always-safe, idiomatic fast path in both languages is
  "collect parts in an array, then `join`" — that's `32_str_build_join`.

- **Dictionary ordering.** MyLang dicts are `unordered_map` (no defined order);
  Python dicts preserve insertion order. The dict-iteration benchmarks use
  order-independent checksums (sum of values) so the results still match.
- **Nullable dict reads.** A MyLang dict read (`d[k]` / `d.k`) is statically
  *nullable* (`opt V`, since the key might be absent), so the dict-lookup /
  member benchmarks narrow each read (`var v = d[k]; if (v != none) ...`) before
  using it — a null-check Python doesn't do, which is why those rows are a bit
  slower than the raw lookup cost. Results are identical (the keys all exist).

### MyLang features with no Python equivalent (Python side intentionally omitted)

- **Parse-time const evaluation** (`48_const_fold.my`). `const` values and
  `pure func` calls over const data are evaluated *by the parser* and inlined as
  literals into the runtime AST — the language's defining feature. There is no
  Python counterpart, so per the brief there is no `48_const_fold.py`. Run
  `./build/mylang -s bench/my/48_const_fold.my` to see the heavy computation
  replaced by a single integer literal in the tree.

- **Auto-const folding & dead-code elimination** (`49_autoconst_fold`,
  `50_autoconst_dce`). Unlike `48`, these *do* have Python counterparts, and
  that's the point: the scripts use plain `var` (no `const`), but because those
  variables are written exactly once MyLang promotes them to constants, folds
  the loop-invariant computation to a single literal, and eliminates dead
  branches — work CPython repeats on every iteration since it has no comparable
  folding. These are among the cases where MyLang is several times *faster* than
  CPython (~0.3–0.4× here). `runtime(x)` opts an expression back out of folding.

- **Auto-pure folding** (`51_purefunc_fold`). Plain `func`s with no side effects
  are auto-promoted to *pure*, so their constant-argument calls fold to
  literals; the loop-invariant result collapses to one literal while CPython
  actually calls the functions every iteration. ~0.38× here.

- **Const-expression de-duplication** (`52_cse_dedup`). Builds on `48`: the same
  heavy const expression (`sum(sort(base))`, `sum(base + base)`) appears twice
  in the loop body. MyLang evaluates each *once* at parse time —
  common-subexpression elimination shares the `sort(base)`/`base + base` array
  across the two occurrences — and folds the whole thing to two literals, so the
  loop is pure literal arithmetic. CPython, lacking both const-folding and CSE,
  re-sorts, re-concatenates and re-sums on every iteration. ~0.03× here (≈40×
  faster). CSE's *own* contribution (separate from plain const-folding) is
  **compile time and memory**, not runtime speed — folding already turns each
  use into a literal. To see it in isolation, compile *N* identical heavy const
  array decls (`const t0 = sort(base); ... const t23 = sort(base);`) and compare
  a build with CSE against one without: parse time drops from O(*N*) evaluations
  to one (≈10× faster to *compile* the file for N=24 here, via `mylang -nr`),
  and because the *N* read-only tables then share one buffer instead of *N*
  copies, peak RSS drops the same way (~620 MB → ~95 MB for N=24 × 300k ints).

(The reverse case — Python-only constructs — is deliberately *not* benchmarked,
since the goal is to measure MyLang's constructs, not Python's.)

## Results

Measured on this machine with an **`-O3` release build** (`build_rel/mylang`),
`--repeat 3` (best of 3), `--scale 1`, CPython 3.14. Your absolute numbers will
differ; the **ratio** column is the portable takeaway. `my/py` is MyLang time ÷
Python time, so higher = MyLang relatively slower and **< 1 = MyLang faster**.

| benchmark | mylang (s) | python (s) | my/py | note |
|-----------|-----------:|-----------:|------:|------|
| 01_while_loop | 0.183 | 0.200 | 0.92× | MyLang faster |
| 02_for_loop | 0.147 | 0.136 | 1.08× | |
| 03_int_arith | 0.180 | 0.150 | 1.20× | |
| 04_float_arith | 0.148 | 0.078 | 1.90× | interpreter overhead |
| 05_mixed_arith | 0.148 | 0.093 | 1.60× | int→float promotion |
| 06_if_branch | 0.194 | 0.099 | 1.96× | |
| 07_nested_loops | 0.159 | 0.127 | 1.25× | |
| 08_func_call | 0.170 | 0.115 | 1.48× | |
| 09_fib_recursive | 0.149 | 0.042 | 3.53× | was **31×** before the FlowState refactor |
| 10_recursion_deep | 0.248 | 0.237 | 1.05× | |
| 11_closure_counter | 0.091 | 0.089 | 1.03× | captured mutable state |
| 12_higher_order | 0.213 | 0.141 | 1.52× | |
| 13_array_append | 0.131 | 0.071 | 1.86× | |
| 14_array_subscript | 0.253 | 0.165 | 1.53× | |
| **15_array_slice_readonly** | 0.107 | 0.290 | **0.37×** | **O(1) COW view vs O(k) copy** |
| 16_array_slice_write | 0.336 | 0.132 | 2.54× | COW clone fires on write |
| 17_array_concat | 0.445 | 0.124 | 3.59× | builds 1000-elem array each iter |
| 18_foreach_array | 0.504 | 0.172 | 2.93× | |
| 19_foreach_indexed | 0.564 | 0.287 | 1.97× | `indexed` / enumerate |
| 20_foreach_unpack | 0.230 | 0.182 | 1.27× | tuple unpacking |
| 21_array_reverse | 0.049 | 0.018 | 2.64× | |
| 22_multi_assign | 0.162 | 0.129 | 1.25× | array-expansion assign |
| 23_dict_insert | 0.071 | 0.068 | 1.04× | |
| 24_dict_lookup | 0.166 | 0.145 | 1.15× | |
| 25_dict_member | 0.154 | 0.086 | 1.79× | `d.key` member-access path |
| **26_dict_iterate** | 0.045 | 0.062 | **0.73×** | MyLang faster |
| 27_dict_keys_values | 0.342 | 0.042 | 8.22× | materializes 2 arrays/iter |
| **28_str_concat** | 0.008 | 1.534 | **0.01×** | **MyLang faster: in-place append vs CPython O(n²)** |
| 29_str_slice_readonly | 0.065 | 0.063 | 1.02× | O(1) view ≈ cheap memcpy — a wash |
| 30_str_index_iterate | 0.121 | 0.091 | 1.33× | |
| 31_str_split_join | 0.070 | 0.057 | 1.22× | |
| 32_str_build_join | 0.056 | 0.036 | 1.54× | idiomatic fast concat |
| 33_sort_ints | 0.241 | 0.233 | 1.03× | |
| 34_sort_custom_cmp | 0.124 | 0.113 | 1.10× | per-comparison callback |
| 35_map_filter | 0.191 | 0.085 | 2.25× | |
| 36_sum_builtin | 0.204 | 0.078 | 2.62× | |
| 37_range_builtin | 0.118 | 0.089 | 1.32× | eager array both sides |
| 38_min_max | 0.331 | 0.324 | 1.02× | |
| **39_find_builtin** | 0.059 | 0.113 | **0.52×** | MyLang faster |
| 40_math_builtins | 0.179 | 0.088 | 2.04× | (stale: now faster, double libm) |
| 41_str_int_conv | 0.104 | 0.066 | 1.57× | |
| 42_exceptions | 1.468 | 0.065 | **22.5×** | genuine throw/catch per iteration |
| 43_sieve | 0.420 | 0.101 | 4.17× | |
| 44_primes_sqrt | 0.300 | 0.120 | 2.49× | `return` in loop, was 4.9× before refactor |
| 45_gcd | 0.308 | 0.088 | 3.50× | |
| 46_matrix_mult | 0.057 | 0.035 | 1.63× | nested subscripting |
| 47_wordcount | 0.143 | 0.074 | 1.93× | split + dict |
| 48_const_fold | 0.125 | — | — | MyLang-only (parse-time folding) |
| 49_autoconst_fold | 0.30 | 0.82 | 0.37× | write-once `var`s folded away |
| 50_autoconst_dce | 0.32 | 1.03 | 0.31× | constant guard + dead branch gone |
| 51_purefunc_fold | 0.27 | 0.70 | 0.38× | auto-pure const-arg calls folded |
| **52_cse_dedup** | 0.003 | 0.120 | **0.03×** | const fold + CSE precompute |

**geomean: ~1.3× slower** across the paired benchmarks (this box, CPython
3.14). MyLang is *faster* on several (`52_cse_dedup` 0.03×, `28_str_concat`
0.01×, `50_autoconst_dce` 0.31×, `15_array_slice_readonly` 0.37×,
`49_autoconst_fold` 0.37×, `39_find_builtin` 0.52×, `26_dict_iterate` 0.74×,
`01_while_loop` 0.90×) and within 2× on the large majority. The lone real
blow-out is per-iteration *genuine* exceptions (`42` ~22×).

### How to read the outliers

- **`28_str_concat` (0.01×, MyLang faster):** in-place `std::string` append
  (amortized O(1)) vs CPython's O(n²) `+=` in this configuration — see the note
  above. The single biggest gap in the suite, in MyLang's favor.
- **`15_array_slice_readonly` (0.37×), `39_find_builtin` (0.52×),
  `26_dict_iterate` (0.74×):** MyLang does less or tighter work — an O(1)
  copy-on-write view instead of an O(k) list copy; a C++ `std::find`/`==` scan
  and `unordered_map` walk instead of per-element Python object dispatch.
- **`42_exceptions` (~22×):** the remaining weak spot, and it's fundamental —
  every `throw` heap-allocates an exception object and unwinds the C++ stack via
  DWARF tables (~1.6µs, irreducible by build flags). This is the *right*
  tradeoff: C++ "zero-cost" EH is free until thrown, so it's reserved for rare
  events.
- **`09_fib_recursive` (3.5×), `44_primes_sqrt` (2.5×):** these were ~31× and
  ~4.9× when `return`/`break`/`continue` rode on C++ exceptions (a base-case
  `return` fired millions of times). Moving them onto a `FlowState` flag — so
  only real errors and user `throw` use exceptions — collapsed the gap to
  ordinary tree-walk overhead (fresh `EvalContext` per call + dynamic dispatch).
- **`27_dict_keys_values` (8.0×):** allocates two fresh arrays every iteration;
  array materialization is comparatively expensive.

## Adding a benchmark

1. Drop `my/NN_name.my` and (if Python has the feature) `py/NN_name.py`. Read
   `scale` from `argv[0]` (MyLang) / `sys.argv[1]` (Python), default 1.
2. Make both print a single `result:` line with an order-independent,
   within-64-bit checksum so the harness can confirm they agree.
3. Re-run `python3 bench/run.py --filter name`.
