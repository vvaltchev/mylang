# Archived plans — fully executed

`plans/` holds only IN-FLIGHT plans. A plan whose work has fully landed
moves here. These files are kept, not deleted: several are the only
written explanation of why present-day code has the shape it does, and
CLAUDE.md and source comments still cite them (their paths were
repointed when they moved).

Archived 2026-08-01 after auditing every plan against the source.

## What moved, and what made it finished

| plan | it drove |
|---|---|
| `bytecode-vm.md` | the AST→bytecode VM, phases 0-6. Its own central mechanism — the AST-fallback opcode — was later DELETED for the no-fail `NotLoweredEx` codegen, and several of its decisions (stack machine, "no JIT", "no serialized format") were reversed by what shipped. |
| `vm-fallback-elimination.md` | removing the fallback opcodes and `Instr::node`. All three opcodes are gone; the 5x-CPython goal it existed to reach was met and exceeded. |
| `vm-ast-free.md` | the first AST-free design. Superseded in its details by `vm-ast-free-runtime.md`; per-arg carets shipped as `ArgLocs`, not the `BuiltinLocs` it proposed. |
| `vm-ast-free-runtime.md` | `FuncDescriptor`/`VmProgram`/`vm_ast_teardown` — the zero-AST-at-runtime proof. Done, all six steps, machine-checked. |
| `vm-peephole.md` | the post-codegen peephole (E1/E3/E4). Complete; its pc-field and CFG tables have since drifted from `visit_pc_fields`, which is the maintained copy. |
| `vm-native-call-stack.md` | the in-VM call stack (A-F). The file itself records "THE PLAN IS COMPLETE (A-F, 100%)". |
| `native-call-impl.md` | #55 native `ReturnV` + `CallV`. All three steps done; it still instructs from `MIN_RUN`, deleted 2026-07-25. |
| `min-run-removal.md` | deleting the JIT's `MIN_RUN` floor. Landed, including the three test fixes it predicted. |
| `builtin-abi-migration.md` | moving read-only builtins to the value ABI. Fully migrated; its "must stay node-based" floor is now inaccurate. |
| `mutating-builtins-native.md` | native `append`/`push`/`pop`/`insert`/`erase`. Its three documented deferrals all lower natively today. |
| `function-inlining.md` | inlining, specialization, the recursion unroll + pure cache. Its long-deferred algebraic pass shipped as `fold_int_arith`/`guard_to_ternary`/`collapse_locals`. |
| `function-templates.md` | monomorphization. Built; its "value-used templates stay dynamic" note is superseded by `value-template-instantiation.md`. |
| `type-inference.md` | the inference/checking pass, M0-M8. Built; several passages now contradict the code (it predates `Bool`, and names exception types that do not exist). |
| `type-driven-specialization.md` | mandatory `dyn`, `-dti`, and the flip to type-driven array representation. Done, incl. removing runtime promotion. |
| `typed-arrays.md` | flat unboxed scalar arrays. Built; its core "promotion hook" no longer exists. |
| `typed-containers-syntax.md` | choosing `array<T>`/`dict<K,V>` over `A[]`. Decided and implemented. |
| `reflection.md` | `layout()`, the type-query family, `Type` objects. The file's own closing status is DONE, with no forward-looking section. |
| `bench-fairness.md` | making the C++ twins a fair comparison. Full pass landed; hands its worst-list off to `native-gap-roadmap.md`. |
| `fuzz-variability.md` | structural variety in the differential fuzzer. All six constructs landed, with a final 1500-program validation. |

`vm-optimizations-rejected.md` is not a moved plan but a SPLIT: the
"Rejected (do not revisit)" list and the merged-entry ledger were taken
out of the still-live `plans/vm-optimizations-deferred.md`, which now
holds only OPEN items. Read it before re-proposing a VM optimization -
several of its entries were fully built and proven correct before being
reverted on measurement, and that measurement is the reason not to try
again.

## Carried forward before archiving

Three items would have been buried by the move, so they were lifted out
first:

- **dead-STORE deletion** — the one peephole rule never built, and
  tracked nowhere else (from `vm-peephole.md`). Now an OPEN entry in the
  live `plans/vm-optimizations-deferred.md`.
- **tail-call elision at `ReturnV(CallV)` pairs** (from
  `vm-native-call-stack.md`). Also now an OPEN entry there.
- two **measured negative results** that existed only in
  `vm-ast-free.md` — splitting the cold handlers out of the dispatch
  loop made the front-end regression WORSE, and a `.rodata` jump table
  was ~2-3% slower than the compiler's own switch lowering. These are
  not open work, so they went into the rejected record
  (`vm-optimizations-rejected.md`) rather than the live backlog.

---

# Archived 2026-08-13 — the second sweep

Every one of the 28 live plans was re-audited against the source (not
against its own status header, which in several cases was years of work
out of date — `g1-no-record-tier.md` still said "DESIGN ONLY. Nothing
here is implemented" about a tier that is DEFAULT-ON, and
`typed-invariant-arrays.md` said "PLANNED, not started" above its own
record of the whole landed C staircase).

**23 moved; 6 stay live**: `top5-cpp-gap.md` (the current my/cpp
decomposition, with H6/H7/H8 still open), `jit-registers.md` (step 2c,
the real linear-scan allocator, unbuilt — and the `rsi`/`r8` pins are
still not killed), `bytecode-inliner.md` (increments 4 and 5 unbuilt,
main not covered, 10 of 19 call sites still blocked on a runtime-callee
guard), `vm-optimizations-deferred.md`, `language-deferred.md` (new —
see below) and `exception-object-lifecycle.md`.

## What moved, and what made it finished

| plan | it drove |
|---|---|
| `model-flip.md` | M1-M6 and then, after M4b's decisive negative result, the whole nativize-ops campaign that replaced it (~44 opcodes). Its own verdict is in CLAUDE.md: M5-flip subsumed by the M5b/c sync calls, M6 landed. Every count in it is stale (its "7.84x vs CPython" predates the 10x crossing). |
| `native-aot.md` | the native x86-64 tier N0-N6a, approach A (compile-time fallback, never a runtime bail), and #55 v1 native calls. Its v2/v3/endgame sections were answered elsewhere — M5a's 1GB native stack, the sync tier, `model-flip.md`. Most heavily cited of all; paths repointed. |
| `native-gap-roadmap.md` | the six-lever program that took my/cpp 4.06x → ~2.4x: the native call protocol, direct fragment calls, pooled+hoisted slices, dyn-foreach specialization, native `len()`/`ord()`, the struct baked layout. |
| `cpp-gap-extremes.md` | the G-series (G2-G6) plus subscript LICM and `plain_frame`. Its central open question — "what does a callee that needs NO VM frame record look like?" — was answered and built as the no-record tier. |
| `g1-no-record-tier.md` | the JIT's no-record call tier, DEFAULT-ON since 291c2fc. Every step of its build order is in the source; the per-change record moved to `docs/jit-optimizations.md`. |
| `vm-performance-roadmap.md` | the 3.9x → 5x CPython push, as dated profile boards. Every board item is closed, superseded (#7 by the no-record tier, #8 by the `vm_dispatch` split) or rejected. Its goal was hit ~4 weeks and ~2.5x ago. |
| `typed-invariant-arrays.md` | the entire C staircase — C1a-e, C2a/b, C3 inc 1-3, C4a-e, C5 — each with its own JIT lever name. Its one unbuilt piece, the first-iteration PEEL, was measured and DECLINED (zero reach across 77 benches), and that verdict is now in CLAUDE.md. |
| `unboxing.md` | option A, the `LoadElem2Int/Float` nested-read fusion, plus the #93/#94 inline tiers and lever A dead-temp forwarding. Options B and C were handed to other plans. |
| `inline-store-tier.md` | the inline element-store tier (#92, #95 cases 1-4). The file's own words: "THE MATRIX IS COMPLETE". Its one loose thread was fixed as #96. |
| `vm-exceptions.md` | P8 end to end — handler regions, the cross-frame pending signal, native `Throw`/`Rethrow`/`finally`, flow-crossing-try — which removed the last construct-level AST fallback and unblocked `.myv`. Its central `CatchTest`/`dispatch_exc` mechanism was then DELETED by #78's handler table, so it is now purely historical. |
| `myv-serializer.md` | the stored-bytecode artifact, phases S0-S5 and the v2-v6 size arc. Four format versions stale (v13 now); `docs/myv-format.txt` plus its CI doc-check and `tests/myv_fuzz.py` are the maintained surface. |
| `callbuiltinv-nativization.md` | native `CallBuiltinV`, and the exception-hierarchy fix it forced (`InvalidArgumentEx`/`InvalidNumberOfArgsEx` became runtime exceptions). Self-declared IMPLEMENTED; all three follow-ups tracked in `model-flip.md`. |
| `call-protocol-arc.md` | its three levers all landed or were reassigned — the `ref_slots` narrowing became C3 inc 1, the `FuncObject` pooling and inline `CaptureSlots` shipped as G2, and lever 3 became the bytecode splice. |
| `jit-backtrace-frames.md` | #88's distinct inline chains — the baked chain index plus the `-2` sentinel. Self-declared DONE, nothing open. |
| `value-model-campaign.md` | all five levers settled: callback re-entry and the int-int fast path landed, `LValue::rebind` landed, the flat dict was REJECTED by the maintainer, and Tier 2a was built, measured and reverted. |
| `structs.md` | the whole struct feature, all 9 phases. Two things it listed as remaining have since landed too: struct hashing / dict keys, and the foreach-body specialization that avoids the loop-var `StructObject`. |
| `hash-and-dict.md` | the universal deep `hash()`, the lazy string hash cache, freeze-on-insert container keys, and the incremental array hash. |
| `value-template-instantiation.md` | value-used template instantiation via the finfo set, the escape ledger, and the bottom-signature decline. Its REPL gap was already an entry in the live backlog. |
| `type-inference-questions.md` | a record of autonomous decisions Q1-Q9, every one shipped. Its last "still deferred" item — deep function subtyping — landed 2026-08-12 as option B. |
| `optional-and-ternary.md` | `??`, `?.`, the ternary, and the `?`/`null` surface syntax. Self-declared ALL DONE; its M8 deferral has since been overtaken. |
| `undefined-name-elimination.md` | all 7 approved steps — FIX-0/FIX-1, `UncatchableRuntimeException`, the TDZ pair, the caret/frames set (myv v12+v13), scope-entry binding, and step 7's prover / `--strict` / warning tier. |
| `repl.md` | the entire REPL: the persistent engine, the hand-rolled termios editor under the no-deps rule, live highlighting, multi-line editing, and the faithful per-input pipeline with cross-input type commitment. |
| `repl-introspection.md` | phases 0-8 — the reflection builtins, `:help`, the tracer, the decompiler, `:show`/`:globals`/`:type`. |

## Carried forward before archiving

The audit found roughly 35 items that were still open and recorded in
NO other file. They were lifted first, because a plan moving to
`archived/` is a plan nobody reads by accident:

- **Most went into `plans/vm-optimizations-deferred.md`**, which was
  restructured by area (compiler/AST, VM interpreter, JIT, value model,
  stored image, diagnostics, testing, tooling) to stay readable at ~35
  entries. Each names the archived file holding its full design, so the
  detail is one link away. Its header also had to be repointed: it
  named `vm-performance-roadmap.md` as "the live work list", and that
  file is now archived.
- **`plans/language-deferred.md` is NEW.** Several buried items change
  what MyLang MEANS rather than how fast it runs, and filing those in a
  document titled "VM optimizations — parking lot" would have buried
  them just as thoroughly as archiving did. The most important is
  **Python-style exception chaining (`__context__`)** — maintainer-
  CONFIRMED on 2026-07-09 as what his "nested exceptions" ask meant,
  deliberately deferred to ship the C++ replace-on-throw default, with
  its three prerequisites written down and recorded nowhere else. Also
  there: the four struct deferrals (including SUBTYPING, which CLAUDE.md
  does not mention at all), `ordered_dict`, the bottom-container
  element-read lattice change, and inline-POD-in-`EvalValue`.
- **Four DECLINES went to `vm-optimizations-rejected.md`**, not the
  backlog, since they are closed rather than open: flattening the catch
  handler table (#81 — built, measured negative at ASSERTS=0, and the
  repo was actively pointing readers at it; see below), the SIMD
  deferral and its reasoning, slot-level slice borrowing, and the
  `cur_seg >= 0` guard that was kept deliberately and is marked
  overturnable. The `EvalValue` assign-operator entry was also updated:
  it has now been tried and declined THREE times, and the third (H5)
  postdated the original entry.

## Two defects the audit surfaced

- **`docs/vm-ops.md` was pointing at work already measured and
  refused.** It told the reader that flattening the nested handler
  vectors "is the next step" — that was built on
  `wip/81-flatten-handler-table`, measured +1.14% / +2.88% Ir at
  `OPT=1 ASSERTS=0`, and discarded. Fixed in this change, with the
  measurement preserved in the rejected record so the branch can go.
- **Empty structs are accepted but documented as rejected.**
  `struct Unit {}` compiles, constructs, prints and compares equal
  today, while `structs.md` said it is "rejected at decl time",
  CLAUDE.md lists it as deferred, README.md is silent, and there are no
  tests. That is a language decision, so it is task #86 rather than a
  fix made here.
