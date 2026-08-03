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
