# VM optimizations — deferred (parking lot)

The CURRENT backlog of perf ideas that are real but not scheduled. The
live, evidence-ordered work list is the RE-PROFILE top-10 in
`plans/vm-performance-roadmap.md`; this file holds only what is parked
and still OPEN.

The ideas that were TRIED AND REJECTED, and the entries that have since
been merged, are not in flight and live in
`plans/archived/vm-optimizations-rejected.md`. **Read that file before
re-proposing anything here** - several of its entries were built,
measured, and reverted, and the measurements are the reason not to
re-attempt them.

Ground rules (maintainer): a change must be perf-neutral-or-better in
the worst case, verified by the FULL-SUITE interleaved A/B rule
(CLAUDE.md, Benchmarks). Correctness is gated by the `-rt` VM
differential + `tests/nested_fuzz.py`.

---

## Open

- **E2 — peephole temp renumbering** (`plans/archived/vm-peephole.md`):
  evaluated + deferred — the native call stack made per-call temp cost
  ~nil, so compacting `n_temps` buys little. Revisit only if a profile
  shows frame-size cost.
- **H1 v2 — inline small-buffer for BOXED structs** (roadmap, alloc
  study): a boxed instance's `vector<LValue>` fields could live inline
  under N slots. Only if a boxed-struct bench ever matters.
- **Value-template v2 — REPL cross-input value instantiation**
  (`plans/value-template-instantiation.md` known gap): an input-1 array
  called indirectly from input 2 keeps the boxed base (correct,
  unoptimized; pinned by a `repl:` test).
- **Dead-STORE deletion** (carried forward from `plans/archived/vm-peephole.md`, now
  archived): a side-effect-free op in the `retargetable_dst` whitelist
  whose dst temp is dead on every successor path could be deleted
  outright, not just retargeted. The peephole already computes the
  liveness this needs. Never built.
- **Tail-call elision at `ReturnV(CallV)` pairs** (carried forward from
  `plans/archived/vm-native-call-stack.md`, now archived): a `return f(args)` could
  REUSE the current frame's window instead of pushing a second one -
  "a natural follow-up, not v1". Verified unbuilt (no tail-call
  machinery in src/).
- **C3 residual — builtin arg-view ABI**: pass the frame run to
  `func_v` by view instead of copying into the stack `EvalValue[8]`.
  Marginal (scalar args copy cheap; only non-trivial args pay a
  refcount bump) and touches all ~84 signatures; its valuable half
  (AST-free carets) already shipped as `ArgLocs`/`builtin_calls`.

---

Tried and rejected, and already-merged entries:
`plans/archived/vm-optimizations-rejected.md`.
