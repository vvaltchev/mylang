# Removing MIN_RUN from the JIT (+ fixing the two tests it breaks)

Status: **LANDED (2026-07-25).** MIN_RUN and `run_has_native_call` are gone;
every eligible run compiles, and a whole tiny body (a 2-op comparator) becomes
a `native_leaf`. Re-measured on the final build (callgrind Ir, matched plain
releases): 34_sort_custom_cmp **0.929x**, 35_map_filter **0.948x**,
57_bool_reduce **0.973x**; 01_while_loop / 09_fib_recursive / 43_sieve
1.0000-1.0008x (the residual is the one-time extra fragment-compile cost —
at scale it vanishes: 01_while_loop scale-10 = 1.0033x).

**Post-landing corrections to part 3** (the recorded diagnosis was partly
wrong — kept below for history, corrected here):

* **3b's real root cause:** `codegen_counts` did NOT throw (the earlier
  "an earlier guard fires" inference was itself an artifact of the botched
  instrumentation). Per-flag instrumentation showed exactly two flags failing
  — `break_finally_native_ok` and `expr_body_chunk_ok` — and BOTH use
  **`codegen_func_counts`**, the sibling helper, which (unlike
  `codegen_counts`) did not disable the JIT: it called
  `codegen_chunk(body, fsize)` with the default `jit=true`. With no MIN_RUN,
  the 2-op `func inc1(int x) => x + 1` body becomes a native_leaf whose
  interpreted ops are DELETED (retv 1 -> 0), and the finally-test's short
  IntBin runs get deleted too (intbin < 3). Fix: pass `/*jit=*/false` —
  count the PRE-JIT chunk, exactly what `codegen_counts`' comment already
  prescribed for itself.
* **A THIRD test failed** (the plan predicted two): `vm: disasm closures +
  halt-drop`. Same class twice over — its part (b) called `codegen_chunk`
  with `jit=true` (the `{ return x + 1; }` body's ReturnV got deleted), and
  its part (a) greps `disassemble_program` for `load.capture`, which vanished
  because the 2-op closure body is now itself a native_leaf with deleted
  ops. Fix: `jit=false` for (b), and `g_jit_enabled=false` around the dump
  for (a) — both assert disassembler/codegen behavior, not JIT behavior.
* 3a was as predicted, except the disasm prints a `; native_leaf: whole
  body -> one fragment` line INSTEAD of a container-plan line for a
  native_leaf chunk — the test now asserts that line + the `enter.nat`.

Everything below is the pre-landing recipe, kept for reference.


## 1. What MIN_RUN is and why it should go

`MIN_RUN` (`src/jit.cpp`, `static constexpr size_t MIN_RUN = 4;`) is the
minimum length of a run of JIT-eligible ops that gets compiled into a native
fragment. A shorter run is left interpreted.

Its original rationale is recorded in the code, in the comment above
`run_has_native_call`:

> *(MIN_RUN exists to avoid EnterNative overhead for a tiny ARITH run, not a
> call.)*

i.e. entering a fragment costs an `EnterNative` dispatch + the fragment's
prologue (`movabs rsi, t_int`, the N5 cache entry loads) and its exit (the
cache flush + `ret`); for a 2–3 op arithmetic run that overhead can exceed the
2–3 VM dispatches it removes. That reasoning was sound when very few ops were
jit-eligible.

**Why it no longer holds.** With most ops now nativized, the runs MIN_RUN
excludes are mostly whole *tiny function bodies* — a comparator or predicate
`func(a, b) => a < b` is exactly two ops (`i.cmp.v`, `return.v`). Such a body,
when it is a single fully-native run ending in `ReturnV`, becomes a
**`native_leaf`**, which a caller fragment `call`s DIRECTLY (#55,
`plans/archived/native-call-impl.md`) — paying **no `EnterNative` at all**. The MIN_RUN
rationale simply does not apply to them, and MIN_RUN is what stops them from
qualifying (`jit_chunk_is_native_leaf` rejects `n < MIN_RUN`).

Concretely, today the sort comparator's chunk dumps as:

```
; ===== lambda#0  (2 instr, 1 temps) =====
; container plan: READY - whole body native (2 ops, 1 native run)
   0  i.cmp.v      r2 = r0 < r1   ; bool
```

Note there is **no `enter.nat`** — it is "READY" (every op eligible) but gets no
fragment, so the native code never runs.

### Measurement (callgrind instruction counts, MIN_RUN=1 vs MIN_RUN=4)

| bench | ratio | note |
|---|---|---|
| `34_sort_custom_cmp` | **0.899x** | the comparator body |
| `35_map_filter` | **0.911x** | the predicate body |
| `57_bool_reduce` | **0.960x** | |
| `12_higher_order` | 1.0001x | neutral |
| `09_fib_recursive` | 0.9997x | neutral |
| `01_while_loop` | 1.0000x | neutral |

No instruction regressions anywhere measured; two solid ~10% wins.

**Wall-clock caveat, and the maintainer's ruling.** A full `bench/run.py` A/B
showed geomean `cur/base 0.996x` (≈neutral) but with 7 benches 4–9% *slower* in
wall-clock (`33_sort_ints`, `34_sort_custom_cmp`, `38_min_max`, `45_gcd`,
`72_exc_finally`, `76_funcval_dispatch`, `77_struct_array_lit`). The maintainer's
explicit instruction:

> *"don't obsess with wall-clock time to go monotonically down. If 10% fewer
> instructions are executed, that's good. Eventually the wall clock time will go
> down if you deterministically observe the emitted instructions with the
> disassembler and continue to nativize everything."*

So **do not block this change on wall-clock.** Fewer instructions is the
criterion.


## 2. The code change (src/jit.cpp)

All four edits are in `src/jit.cpp`. They were made and compiled cleanly.

**(a) The run-forming gate** — inside `jit_compile_chunk`'s run-building loop
(search for `runs.push_back({i, j})`). Replace:

```cpp
        if (j - i >= MIN_RUN
                || run_has_native_call(chunk.code, i, j, jc))
            runs.push_back({i, j});
```

with an unconditional `runs.push_back({i, j});` plus a comment recording why
MIN_RUN went away (the tiny-body / native_leaf argument + the measured numbers
from part 1).

**(b) Delete the constant** `static constexpr size_t MIN_RUN = 4;`.

⚠ **Do NOT touch `MIN_CONTAINER_ISLAND`** — a different constant, used by the
M3 straight-line-container gate. It is unrelated.

**(c) Delete `run_has_native_call`** (function + its comment block). Once every
run forms, it has no callers. The compiler will tell you if that is wrong.

**(d) `jit_chunk_is_native_leaf`** — drop the length floor:

```cpp
    if (n < MIN_RUN || chunk.code[n - 1].op != OpCode::ReturnV)
```
becomes
```cpp
    if (n < 1 || chunk.code[n - 1].op != OpCode::ReturnV)
```

and update its doc comment, which currently ends *"and it is >= MIN_RUN (else no
run forms)"*.

**(e) Stale comment** in the `jit_container_plan` doc block: *"a run below
MIN_RUN gets no EnterNative but IS container-eligible"* — reword (the
container-eligibility point still stands, the MIN_RUN reference does not).


## 3. The two failing tests — the actual work

With the change in place, `./build/mylang -rt` fails exactly two entries. Both
are **expectation** tests, not correctness failures: the differential
(tree-walker == VM), the bench/samples differential and the fuzzer were all
clean with MIN_RUN removed.

### 3a. `vm: -vd container plan (model-flip M1 analysis)`

* Function: `vm_disasm_container_plan` in `src/tests.cpp` (~line 16105).
* Its program is:

```
func add(int a, int b) => a + b;
func wc(words) {
  var d = {};
  foreach (w in words) d[w] = 1;
  return d;
}
print(add(runtime(2), 3));
print(wc(["a"]));
```

* It asserts three things about `disassemble_program`'s output: `main` is
  `NOT ready` **and** contains an `island [pc` line; `func add` says `READY`;
  `func wc` says `READY`.
* Why it changes: `add` is 2 ops, so with MIN_RUN gone it now forms a run,
  becomes a `native_leaf`, gets a fragment, and its **fully-native ops are
  DELETED** (approach A) — so its section becomes essentially one `enter.nat`
  and the plan line's op counts change. `main`'s island is a `CallV` (M5
  territory) and should be unaffected, but verify.
* **Recipe:** run that exact program through `./build/mylang -vd`, read the
  three sections, and update the assertions to match the new reality while
  preserving the test's INTENT (the plan distinguishes a mixed chunk from a
  fully-native-eligible one). Note the comment above the function already
  documents an earlier migration of this kind (`wc` used to be the mixed
  example and became READY) — extend it in the same style.

### 3b. `vm: codegen shapes (native int loop + flatten)`

* Function: `vm_codegen_shapes` in `src/tests.cpp`; registered at ~line 16716.
* This one is **not yet diagnosed** — budget the time.
* What is known:
  - Its helper `codegen_counts` **already** sets `g_jit_enabled = false` around
    the compilation (there is an explicit comment saying it counts the PRE-JIT
    chunk precisely because approach A deletes a fully-native run's ops). So the
    naive explanation — "op counts changed because the JIT deleted them" —
    should NOT apply, and that is why this needs a real look.
  - `jit_chunk_is_native_leaf` starts with `if (!g_jit_enabled) return false;`,
    so the native_leaf flag is also unaffected while counting.
  - Verified by hand with `-nj -vd` that the two snippets I checked produce
    exactly the op counts the test expects:
    - `var i = 0; var s = 0; while (i < 5) { s += i; i += 1; } if (s > 3) s += 100; else s -= 1;`
      → `juic=2, intbin=4, jmp=1, halt=1` (matches `native_ok`).
    - `var s = 0; var i = 0; while (i < 5) { func g() => 1; i += 1; }`
      → `juic=1, intbin=1, makeclosure=1` (matches `fallback_ok`).
  - I instrumented the final `return a_ok && b_ok && ...` chain with per-flag
    prints: **every flag in that chain printed OK**, yet the test still
    returned false. Therefore the function returns false at one of the earlier
    `if (!codegen_counts(...)) return false;` guards — i.e. `codegen_counts`
    itself failed (it returns false when the compile throws).
  - My attempt to instrument those guards printed nothing, which means the
    textual patch did not actually land on them (my Python edit computed the
    function's extent with `s.index("\n}\n", i)` and probably truncated the
    segment early). **Do not repeat that approach.**
* **Recipe:**
  1. Instrument `codegen_counts` itself (not the caller): on the catch path,
     `fprintf(stderr, ...)` the source snippet and the exception `what()`/type.
     That immediately names the failing snippet and the reason.
  2. Alternative/quicker: give `vm_codegen_shapes` a `const char *why = nullptr;`
     and set it before each `return false;`, printing at the end.
  3. Likely candidates to look at first: any snippet whose chunk becomes a
     `native_leaf` now, and any `ML_CHECK` in `codegen_chunk` /
     `jit_compile_chunk` that assumes a run is >= MIN_RUN (e.g. the
     `chunk.native_leaf` block in `jit_compile_chunk` ML_CHECKs that "the run
     analysis agrees" — with the floor gone, that invariant may need updating).
  4. Fix the root cause if it is a real invariant break; otherwise update the
     expectation, preserving intent.


## 4. Verification checklist (the standing discipline)

Applies to this change and every JIT change:

1. **Read the emitted code first** (`-vdj`) — does the disassembly look right?
   There is no point measuring CPU time if the optimization did not engage.
   Specifically here: confirm a 2-op comparator body now shows an `enter.nat`
   and that `-vd` marks its chunk `native_leaf`.
2. `./build/mylang -rt` green on **debug**, **clang**
   (`make -j TESTS=1 OPT=0 CXX=clang++ BUILD_DIR=build-clang`) and
   **release + VM_HARDENING**
   (`make -j TESTS=1 OPT=1 VM_HARDENING=1 BUILD_DIR=build-rel`).
3. bench/ + samples/ differential: `-vm` output == `-tw` output for every file
   (skip `rand_sort` — uses `rand()` — and `shopping` — an interactive loop that
   the timeout truncates).
4. Fuzzer: **`python3 tests/nested_fuzz.py --count 300 --engines tw,vm`**.
   **300 is the maximum**; ask the maintainer before running more.
5. Measure with **callgrind instruction counts** (deterministic), not
   wall-clock, on at least `34_sort_custom_cmp`, `35_map_filter`,
   `57_bool_reduce` plus a couple of loop/recursion benches to confirm they stay
   neutral:
   `valgrind --tool=callgrind --callgrind-out-file=/dev/null ./build-rel/mylang bench/my/NAME.my 1 2>&1 | grep "refs:"`
6. A nativization that makes things slower is **wrong or unfinished**, never
   "acceptable" — owe a detailed story for any non-improvement.


## 5. Related gotchas worth knowing

* **A fragment is entered ONLY at its head.** An op that bails mid-run drops
  everything downstream to the interpreter until the next fragment head. This is
  why an incomplete native path is so costly, and why merging runs can *expose*
  a latent bail (see the `56_sieve_bool` story in `plans/model-flip.md`).
  Removing MIN_RUN creates more, smaller fragments — i.e. more fragment heads —
  which is the opposite pressure and generally helps recovery after a bail.
* **`patch8` asserts** that a rel8 displacement fits (added 2026-07-25 after it
  silently truncated and produced a SEGV in generated code). Use `j32` /
  `patch32_here` for any forward span that can grow.
* Expect `-vd` output to change broadly: many chunks that previously showed
  their ops will now show a single `enter.nat`, because a fully-native run's
  interpreted originals are deleted. Any other test that greps `-vd` output may
  need the same treatment as 3a.
