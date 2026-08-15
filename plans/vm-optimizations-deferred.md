# VM / JIT optimizations — deferred (parking lot)

The CURRENT backlog of perf ideas that are real but not scheduled. The
live, evidence-ordered work list is **`plans/top5-cpp-gap.md`** (the
my/cpp decomposition); this file holds only what is parked and still
OPEN.

The ideas that were TRIED AND REJECTED, and the entries that have since
been merged, are not in flight and live in
`plans/archived/vm-optimizations-rejected.md`. **Read that file before
re-proposing anything here** - several of its entries were built,
measured, and reverted, and the measurements are the reason not to
re-attempt them.

Items that change **what the language MEANS** are not here: they need a
maintainer decision first and live in `plans/language-deferred.md`.

Ground rules (maintainer): a change must be perf-neutral-or-better in
the worst case, verified by the FULL-SUITE interleaved A/B rule
(CLAUDE.md, Benchmarks). Correctness is gated by the `-rt` VM
differential + `tests/nested_fuzz.py`.

Most entries below were lifted out of plans archived on 2026-08-13;
each names the archived file that holds its full design. Those files
are kept, so the detail is one link away - what is here is enough to
know the item EXISTS.

---

## Compiler / AST transforms

- **Linear-tail-recursion -> loop** (`archived/native-gap-roadmap.md`):
  an AST pass turning self-tail-recursion into a loop with accumulator
  rotation, exactly as g++ does to `sumto`. **Not** the same as the
  tail-call entry below, which reuses the VM window; this removes the
  call outright, and would make the `10_recursion_deep` class a native
  loop.
- **Tail-call elision at `ReturnV(CallV)` pairs** (carried forward
  2026-08-01 from `archived/vm-native-call-stack.md`): `return f(args)`
  could REUSE the current frame's window instead of pushing a second
  one - "a natural follow-up, not v1". Verified unbuilt.
- **Dead-STORE deletion** (carried forward 2026-08-01 from
  `archived/vm-peephole.md`): a side-effect-free op in the
  `retargetable_dst` whitelist whose dst temp is dead on every
  successor path could be DELETED, not just retargeted. The peephole
  already computes the liveness this needs. Verified unbuilt -
  `retargetable_dst` is consulted only for retargeting.
- **LICM follow-ups NOT taken** (`archived/cpp-gap-extremes.md`):
  `while` loops (the guard would have to be the condition itself,
  needing a separate side-effect-free proof - only `for` is handled);
  SCALAR elements (`a[i][j]`, both indices invariant, int result - a
  real but smaller win needing a typed temp and touching the flat-store
  paths); DICT bases (vivify / `for_write` subtleties).
- **`??` on a non-opt lhs is dead** (`archived/optional-and-ternary.md`):
  fold to the lhs and emit an autoconst/infer trace note - do not
  error, mirroring how a const-true `if` is handled.

## VM interpreter

- **E2 - peephole temp renumbering** (`archived/vm-peephole.md`):
  evaluated + deferred - the native call stack made per-call temp cost
  ~nil, so compacting `n_temps` buys little. Revisit only if a profile
  shows frame-size cost.
- **Sort-comparator boxing** (`archived/vm-performance-roadmap.md`,
  board item 10): `sort(arr, cmp)` over a FLAT array boxes two
  `EvalValue`s per compare. An int-specialized compare path needs a
  typed-comparator contract. ~10% of `34_sort_custom_cmp`'s lambda
  cost.
- **H4 - a pre-checked container write op**
  (`archived/vm-performance-roadmap.md`): `LValue::get_value_for_put`
  is the per-write alias/slice/COW test, 7.4% of `13_array_append`. A
  container PROVEN unaliased at codegen (a local never copied or
  sliced, from inferencer escape data) could take a pre-checked write.
  **Sound only with a real escape proof** - evaluate carefully.
- **F2 - bool-typed conditions**
  (`archived/vm-performance-roadmap.md`): `while (flag)` boxes through
  `JumpUnlessTrueV`; a `JumpUnlessBool` on a proven-bool slot reads the
  byte directly. Interpreter-only residue - the JIT already inlines an
  int/bool test for that op.
- **A3 - pc as a pointer, and hot-case ordering**
  (`archived/vm-performance-roadmap.md`): iterate `const Instr *ip`
  instead of `code[pc]` (saves an index-scale per op; pc reconstructed
  only on the cold error path via `ip - code.data()`), plus keeping hot
  opcodes first in the enum for jump-table locality. **A2** (hot/cold
  handler split) belongs with it - the plan says A1/A2 must be A/B'd as
  a PAIR, and A1 (`CGOTO`) landed alone.
- **`vm_store_base`'s dict-store base copy**
  (`archived/value-model-campaign.md`, lever 5's own "STILL TODO"):
  ~40M Ir in the dict bench, murky because callgrind inlined the
  attribution. Still live, ~12 call sites.
- **A native-immediate path in `vm_num_binop`**
  (`archived/value-model-campaign.md`): pass an int/float LITERAL
  operand straight to the binop instead of boxing it into a scratch via
  `boxed_operand`. Measured cost: `scratch = EvalValue(o.lit)` is a
  `none`->`int` type-change move-assign once per boxed op with a
  literal operand (`66_dyn_foreach`: 5.99%, 20M calls). **Untried, and
  interesting precisely because it might capture that win WITHOUT
  touching `boxed_operand`'s inlining** - which is what turned the
  direct fix into a wash (see the rejected record).
- **EAFP - make a dict/array READ raise natively**
  (`archived/vm-exceptions.md`): `try { d[k] } catch (KeyNotFoundEx)`
  in a loop is "the one runtime-error-as-control-flow worth caring
  about". The VM's own read op would dispatch to a same-frame handler
  on the miss instead of C++-throwing from the runtime library. Today
  `KeyNotFoundEx` is still a loc-less C++ throw caught at the boundary.
- **Incremental O(1) hash for DICTS** (`archived/hash-and-dict.md`):
  sound only for the commutative combine (add on insert, subtract on
  remove). The ARRAY half landed (`arr_append_maintain_hash`);
  `DictObject` has no hash cache at all.
- **C3 residual - builtin arg-view ABI**: pass the frame run to
  `func_v` by view instead of copying into the stack `EvalValue[8]`.
  Marginal (scalar args copy cheap; only non-trivial args pay a
  refcount bump) and touches all ~84 signatures; its valuable half
  (AST-free carets) already shipped as `ArgLocs`/`builtin_calls`.

## JIT / native tier

- **A full register allocator over the FLOAT expression DAG**
  (`archived/typed-invariant-arrays.md`, C4b's "Still open"): the two
  scratches alternate, which handles chains, but a value that must live
  across an intervening op still round-trips through its slot (55's two
  non-adjacent temps), and operand `a` still costs a copy when it lives
  in a pin (SSE2's two-operand form makes that irreducible without
  AVX's three-operand VEX). Widening past xmm2/xmm3 wants REX-encoded
  xmm8-15; the current encoders are xmm0-7 only. **NOT covered by
  `plans/jit-registers.md` step 2c**, which is a SLOT-pin allocator
  that explicitly excludes temps - i.e. exactly these values.
- **C5 follow-up, stated and not taken**
  (`archived/typed-invariant-arrays.md`): C3 inc 3 admits temps to the
  float type-store elision only when `!ref_listed`, because a flush
  stamps `t_float` at an exit taken before the run's first write and
  would hide a reference from `pop_window`'s scan. **A released temp
  can no longer hold one, so that objection is gone for exactly this
  set** - worth measuring as its own increment.
- **What the C-series leaves on `64_struct_create`**
  (`archived/typed-invariant-arrays.md`): after C5 it read only -2.4%,
  because most member-read dsts do not qualify (the Vec3/Point ctor
  temps are written by the ctor plan, not a scalar op). Named residue:
  the remaining store guards, and the three `i * K.0` promotions
  feeding the Vec3 ctor (each an int-slot type dispatch plus an inline
  literal materialisation - the C4b pool declined here).
- **A cmov tier for simple select shapes**
  (`archived/native-gap-roadmap.md`): `06_if_branch` is 9.5x and C++
  likely cmov-ifies it.
- **Global/capture struct bases for `LoadMemberInt/Float`** (H1
  residue, `archived/vm-performance-roadmap.md`): `try_member_scalar`
  bails unless the base is a LOCAL, so a global/capture struct base
  stays on boxed `MemberV`.
- **Borrowed container args** (`archived/cpp-gap-extremes.md` item 3):
  binding an array parameter costs an `intrusive_ptr` retain/release
  per call; removing it needs an ESCAPE ANALYSIS (a param the callee
  never stores). #162 deleted the STAGING copy; the bind-side retain
  remains (`jit_bind_ref_arg` is still a helper call).
  `plans/top5-cpp-gap.md` carries a narrower successor (an inline
  emitted tier for that helper); the escape-analysis framing is only in
  the archived file.
- **The frame-pop micro-items, ~10 Ir together**
  (`archived/cpp-gap-extremes.md`): hoist the per-element
  `s >= rec.nslots` bound out of the loop (the chunk knows the total it
  was built against), and store BYTE OFFSETS instead of slot indices to
  drop the x48. Explicitly not worth an edit cycle on the hottest
  protocol in the VM without another reason to be in there.
- **Profile-guided run selection** (`archived/native-aot.md`): v1
  compiles every eligible run unconditionally at AOT time. Measure
  whether compile time ever matters (it will not for scripts this
  size).
- **COPY-AND-PATCH codegen** (`archived/native-aot.md`): pre-compile a
  C++ stencil per op to machine code, then copy the template and patch
  its operand holes at JIT time (Xu & Kjolstad 2021; CPython 3.13).
  Hand-emitting each op's bytes is verbose and fragile and does not
  scale to "a native container for every op". **Evaluate when the
  hand-emitted op set gets wide.**
- **v3 - a native direct call to a REASSIGNED callee**
  (`archived/native-aot.md`): the #55 direct tier still refuses
  `slot_reassigned`. Designed answer: emit a runtime TYPE CHECK on the
  resolved slot - a `FuncObject` of matching signature takes the native
  `call`, an unknown signature takes a bytecode island, a non-function
  RAISES `NotCallableEx`. "Type-check-and-do, not a silent fallback."
  Partly superseded by the M5 sync tier.
- **arm64 + Windows JIT backends** (`archived/native-aot.md`): jit.h
  states the POLICY (which platforms, and why) but not the mechanics -
  arm64/macOS needs `MAP_JIT` + `pthread_jit_write_protect_np` (plain
  `mprotect` suffices only on Linux/macOS x86-64); Windows needs
  `VirtualAlloc` + `FlushInstructionCache`.
- **A slow tier for a general/`dyn` inner in the nested read**
  (`archived/unboxing.md`, increment 5): today it declines to the
  unfused pair.

## Value model (large, invasive)

- **H1 v2 - inline small-buffer for BOXED structs** (roadmap alloc
  study): a boxed instance's `vector<LValue>` fields could live inline
  under N slots. Only if a boxed-struct bench ever matters.
- **Option B - borrowed (non-owning) references**
  (`archived/unboxing.md`): let a frame slot hold a non-owning
  reference generally, so ANY chain of container accesses avoids
  boxing. Touches copy-on-write (when does a detach invalidate a live
  borrow?), slices, the `ref_slots` release scan, and every builtin
  taking an `LValue *`. **The failure mode is a dangling pointer, i.e.
  silent memory corruption.** The plan's own rule: do not start B until
  A has been measured - A has now landed and been measured.
- **I3 - NaN-boxing / a 16-byte `EvalValue`**
  (`archived/vm-performance-roadmap.md`): halves every slot copy.
  Huge, invasive, last resort. Recorded only.
- **Pooled allocators for the other hot `std::` containers** (the
  closing maintainer TODO in `archived/vm-performance-roadmap.md`,
  2026-07-19): `SharedArrayObj`'s element vectors, `SharedStr`/`StrObj`
  storage, boxed-struct field vectors, the VM's segmented slot-stack
  segments. **Partly refuted since**: pooling a `std::vector` was
  measured and rejected (a custom allocator defeats libstdc++'s memmove
  fast path - see the rejected record), and the `FuncObject` half
  shipped. What survives untracked is the JITTER motivation - cutting
  the ~3% wall-clock noise floor `bench/tune_scales.py` found on
  allocation-bound benches.
- **Value-template v2 - REPL cross-input value instantiation**
  (`plans/archived/value-template-instantiation.md` known gap, now archived): an
  input-1 array called indirectly from input 2 keeps the boxed base
  (correct, unoptimized; pinned by a `repl:` test).

## Stored image (`.myv`)

From `archived/myv-serializer.md`'s explicit "Deferred (not v1)" list,
which exists nowhere else: cross-version compatibility / migration;
signing + verification; a combined archive (several scripts per file);
and mmap zero-copy loading - "revisit only if load time ever measures
as a problem".

## Compiler diagnostics

- **Step 7 residual - call sites below the top level**
  (`archived/undefined-name-elimination.md`): a call inside a function
  body is analysed only when that function is itself called from the
  top level. Both `prove_unbound_calls` and `warn_unbound_calls`
  iterate root-block statements and reach deeper calls only through
  `build_reachable_reads`' fixpoint from a top-level call site.
- **Delete `g_vm_jit_eptr`** (`archived/undefined-name-elimination.md`):
  once every runtime-raisable exception is a `RuntimeException` it has
  nothing left to carry, and the premise now largely holds
  (`InternalErrorEx` and `UndefinedVariableEx` are both
  `UncatchableRuntimeException` with `clone()`). Still present at ~15
  sites. Related stale comment to fix with it: `jit_load_global` still
  says `UndefinedVariableEx` "is a PLAIN Exception with no `clone()`".

## Testing

- **The G1 no-record tier's unbuilt nets** - task #85, designs in
  `archived/g1-no-record-tier.md`. **The tier is DEFAULT-ON**, which is
  what makes these worth carrying.
  - **Net 2 - DONE (2026-08-13)**: `MYLANG_RECON_AT=N` +
    `tests/norec_sweep.py`, with an in-suite seed. Sabotage-verified -
    a one-off `seg_top_before` leaves `-rt` and `corpus_diff` green
    and only the sweep fails.
  - **Net 3 - OPEN**: the exhaustive small-scope enumeration generator
    (per level, depth <= 4, frame kind x terminal x where-caught;
    uncaught programs compared BYTE-FOR-BYTE on stderr across tw /
    vm-nojit / jit / jit+shadow). Its axis list is OPEN to additions
    and CLOSED to removals, and three cases the depth bound does not
    cover must be enumerated explicitly: a call exactly at a
    `SEG_SLOTS` boundary, recursion deep enough to grow the native
    stack, and a reconstruction spanning both. Two entries in the
    plan's own sabotage matrix name Net 3 as their only catcher
    ("interleave ignores the SP bound", "caller_captures pop skipped").
  - **Net 4 - OPEN**: the GCOV coverage gate. The lane exists
    (`-DGCOV=ON`); the gate script does not. 100% line + branch over
    the walk/reconstruction code only, an uncovered branch either
    covered or listed with a written reason, never silently exempt.
    The plan sequenced this to GATE the step that shipped.
- **A `.myv` agreement lane** (`archived/myv-serializer.md`, S5's
  fourth lane, never built): `nested_fuzz.py --myv` (compile -> file ->
  load -> run as a fourth agreement engine) plus a CI round-trip step.
  Today CI compiles every sample/bench to an image but only feeds it to
  the doc reader, never RUNS it; `tests/myv_fuzz.py` is a CORRUPTION
  fuzzer, not an agreement lane; and CLAUDE.md's "83/84 run identically
  from an image" is a one-off manual verification.
- **A `VM_HARDENING` per-write type audit**
  (`archived/typed-invariant-arrays.md`, C3's unbuilt prerequisite):
  re-verify every "provably typed" slot's tag on every write (the
  audit-net pattern `ref_slots` already uses), green across the
  differential + fuzzer for a while BEFORE any code relies on the
  proof. Increments 1-3 shipped on the existing `pop_window` re-scan
  instead; the deliberate soundness trade it guards (a silently
  retained reference, or a garbage type pointer) is documented only in
  the archived file.
- **Struct-key / array-key dict benches** (`archived/hash-and-dict.md`
  phase 4): bench/ has only the int- and string-keyed dict benches,
  though any-type keys shipped.

## Tooling

- **The `-vdj` decoder polish** (`archived/native-gap-roadmap.md`,
  M5b): the disassembly's SIB print shows a bogus `*8` scale and falls
  to `.byte` on sub-from-mem / movsxd. The BYTES were hand-verified
  correct - it is the decoder that lies, which will mislead the next
  person reading a `-vdj` dump.
- **`-a` colouring for a flat `array<Struct>`**
  (`archived/structs.md` §9): a new legend entry, analogous to the
  green flat-scalar arrays. `collect_arrays` marks `flat_array` only
  for non-opt Int/Float/Bool elements, so a flat struct array (and a
  flat `array<str>`) gets no annotation today.
- **REPL follow-ups** (`archived/repl-introspection.md`): a `:type`
  non-committing inference PROBE for an arbitrary expression (today it
  gives a committed global's static type, else the RUNTIME structural
  type); Tab-completion of `:help` topics (`repl_help_topics(prefix)`
  exists but its only caller is a test); full inferred LOCAL types in
  `:show` (today best-effort from AST hints). Plus two open questions:
  trace VERBOSITY levels, and whether the type-query builtins should
  consult the inferencer for a static answer.
- **REPL `:reset` and `:tokens`** (`archived/repl.md`): `:reset` drops
  the retained input ASTs (they grow unbounded over a session -
  acceptable for a short-lived REPL, but the plan wanted the command);
  `:tokens` is the `-t` dump as a meta-command, which §3.1 presented as
  falling out for free and which was never built.

---

Tried and rejected, and already-merged entries:
`plans/archived/vm-optimizations-rejected.md`.
