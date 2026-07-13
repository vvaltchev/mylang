# The native in-VM call stack (roadmap C1) — design

Goal: a VM->VM call becomes a STATE CHANGE inside the dispatch loop — push a
call record, rebase the frame window, jump — instead of the current C++
protocol (vm_call_func -> do_func_call -> EvalContext + Frame construction ->
recursive vm_run_chunk). Measured motivation (vm-performance-roadmap Part 2):
do_func_call machinery is ~30% of ALL instructions on 35_map_filter (the
callback body itself: 14%); EvalContext's ctor alone is 4.7%; fib is only at
parity with the tree-walker (vm/tw 0.97) because the call protocol hands back
everything threaded dispatch wins. Targets: 09/10/11/12/34/35/63/64/67/76 —
the call-bound dozen, including most of the sub-par NEW benches.

## What a call pays today (the deletion list)

Per VM->VM call: a C++ call chain out of the dispatch loop (spilling
`in`/`pc`/frame registers), a 136-byte `EvalContext` (std::map member +
embedded FlowState) constructed/destroyed, a 432-byte `Frame` + `init()`
placement-constructing one 48-byte LValue per slot, a per-param copy loop
(refcount bump per non-trivial arg), a RECURSIVE `vm_run_chunk` entry whose
prologue constructs THREE std::vectors (dict_iters, dyn_iters, handlers) even
when the chunk needs none, and a FlowState round-trip + full dtor cascade on
return. The new model deletes every item; none is shaved, all are gone.

## Audited facts the design builds on (2026-07-16, HEAD 69fef27)

- **Every callable body has a chunk** (the no-fail rule; the only chunk-less
  descriptor is a never-called template base, ML_CHECKed). So "call a chunk"
  is total for VM->VM calls.
- **FlowState is ALREADY vestigial in the VM**: the only writer is `ReturnV`
  (sets flow then `return`s from vm_run_chunk; do_func_call reads it) and
  the only reader is `LoopBackEdge` — which **codegen no longer emits at
  all** (a Phase-1 leftover: no emit site; only a disasm render and a test
  counter survive). The new model deletes both: `ReturnV` writes the
  caller's result slot directly; `LoopBackEdge` is removed from the enum.
- **Per-invocation state that must become per-frame**: `handlers`
  (VmHandler stack), `vm_exc` (in-flight caught exception), `vm_pend`
  (finally resume action), `dict_iters`/`dyn_iters` (live iterator state,
  sized by the chunk's n_dict_iters/n_dyn_iters).
- **Builtins need an EvalContext*** (func_v/func_lv ABIs) but use it only
  for: frame slot access via `ctx->frame`, `gfuncs`, `captures`,
  `const_ctx`/`in_const_eval` gates, and root access. No map lookups in a
  script. A single REUSABLE per-activation view context suffices.
- **Slot addresses never outlive an op** except via container-registered
  slice back-pointers (SharedArrayObj `slices` holds LValue* of slice
  VALUES living in slots) — which move-ctors re-register correctly. So the
  slot stack may relocate ONLY via element-wise std::move, and only at call
  boundaries (no op holds a frame LValue* across a call op).
- **Real recursion capacity today is small**: ~5-8K frames of 8MB C stack
  (do_func_call + vm_run_chunk native frames), 1MB/Windows before the
  /STACK fix. bench 10_recursion_deep is depth 900. A 256K-slot VM stack
  (~12MB) already EXCEEDS today's effective depth several-fold.

## New data structures (vm.cpp; sizes are targets, not contracts)

```cpp
/* One VM frame: the per-call record. Lives in VmActivation::records. */
struct VmCallRec {
    const Chunk *chunk;          /* the RUNNING chunk */
    const FuncDescriptor *desc;  /* callee identity (backtrace, flags);
                                  * null for the root/main frame */
    size_t ret_pc;               /* resume pc in the PARENT frame */
    int_type base;               /* this frame's first slot (stack INDEX,
                                  * never a pointer - survives growth) */
    int_type nslots;             /* frame_size + chunk->n_temps (bounds) */
    int_type dst;                /* PARENT slot receiving the return value */
    uint32_t handler_base;       /* activation handler-stack watermark */
    uint32_t diter_base, dyiter_base;   /* iterator-stack watermarks */
    Pend pend;                   /* finally resume action (was vm_pend) */
    std::unique_ptr<RuntimeException> exc;  /* in-flight caught exc
                                             * (was vm_exc) */
    std::unique_ptr<PureCache> pure_cache;  /* rehomed from Frame */
    unsigned char boundary;      /* 1 = popping returns control to C++
                                  * (the activation entry frame) */
};

/* One ACTIVATION: a run of VM frames entered from C++ (main, a builtin
 * callback, a tree-walker boundary). Owns the slot stack + parallel
 * stacks. One per vm_run_activation entry; nested activations (a builtin
 * invoking a closure) create their own - rare, cold. */
struct VmActivation {
    std::vector<LValue> slots;        /* THE stack; grows by std::move
                                       * relocation at call boundaries */
    std::vector<VmCallRec> records;
    std::vector<VmHandler> handlers;  /* shared; per-frame watermarks */
    std::vector<DictIterState> dict_iters;
    std::vector<DynIterState>  dyn_iters;
    EvalContext view_ctx;             /* the ONE reusable context handed to
                                       * builtins/helpers: parent=root,
                                       * frame=&view_frame, captures/gfuncs
                                       * updated on frame switch */
    Frame view_frame;                 /* slots/size repointed per frame;
                                       * owns nothing (dtor no-op mode) */
};
```

Descriptor precompute (at vm_precompile_all, serializable):
- `total_slots` = frame_size + chunk->n_temps (frame window size);
- `fast_bind` = no const params, no DeclType::i/f params (no coercion), so
  a call with nargs == nparams binds with NO per-param work;
- min_args/nparams already present.

## The calling convention

**Call (CallV / CachedCallV / CallValueV, callee = FuncObject with chunk):**
1. arity check: `min_args <= nargs <= nparams` (two compares; throw
   InvalidNumberOfArgsEx exactly as do_func_call).
2. ensure capacity `top + callee->total_slots` (grow = relocate, below).
3. bind: copy the nargs arg slots from the caller's contiguous arg run to
   `[newbase, newbase+nargs)`; fill `[nargs, nparams)` with none (opt
   params); if `!fast_bind`, run the small fixup loop (coerce i/f params,
   const-flag const params). Phase E upgrades the common case to ZERO-COPY:
   codegen places the arg run at the caller's window TOP, and newbase = the
   arg run itself (Lua-style overlapping windows) - binding disappears.
4. push VmCallRec {callee chunk, desc, ret_pc = pc+1, base, dst, watermarks,
   boundary=0}; update view_ctx.captures = &fo.capture_slots; frame window
   repoint; `pc = 0; chunk = callee_chunk; VM_NEXT`.

The dispatch loop's `chunk`/`pc` become locals of the ACTIVATION loop
(rebased per frame), so cross-frame transfer never leaves the loop.

**Return (ReturnV / Halt-fallthrough):**
1. read the result from the frame (ReturnV a.slot; Halt = none);
2. destroy the frame's constructed slots `[base, top)` (or mark for lazy
   reset - see slot lifecycle below), truncate handler/iter stacks to the
   watermarks, drop pure_cache/exc with the record;
3. if `boundary`: store the result for the C++ caller and RETURN from the
   activation loop;
4. else write `slots[parent.base + dst]`, `pc = ret_pc`, repoint the frame
   window/captures, `VM_NEXT`.

**Slot lifecycle.** The stack's LValues are constructed once (vector
growth) and REUSED across frames. On pop, each slot in the dead window is
reset to `none` (releases references — the semantic equivalent of today's
Frame dtor) — cost proportional to the frame size, same as today's dtor,
minus the construction side. A later phase can skip resetting slots the
next call immediately overwrites (param slots), which today's model cannot.

## FlowState, exceptions, backtraces

- **FlowState: deleted from VM execution.** ReturnV writes the parent slot;
  `LoopBackEdge` (dead: zero emit sites) is removed outright — opcode,
  handler, disasm render, test counter. The tree-walker keeps FlowState
  untouched. do_func_call keeps its FlowState for TREE-WALKED bodies only.
- **Library C++ throws** (Type ops: OOB/KeyNotFound/TypeError...) are caught
  by ONE try around the ACTIVATION loop (today: one per frame). The catch
  dispatches on the handler stack: the innermost VmHandler decides the
  target FRAME (records above it pop — appending their BacktraceFrames as
  they go — with slot/iter/handler cleanup) and the loop resumes at its
  catch pc. No handler in the activation → convert to `g_vm_exc_pending`
  and return (the C++ boundary above propagates as today).
- **vm_raise** (native-dispatched errors, no C++ throw) does the same walk
  directly.
- **Backtraces are byte-identical by construction**: each popped record
  yields exactly today's vm_capture_frame data — name/params from `desc`,
  call site from the PARENT record's {chunk, ret_pc-1} via loc_at, the
  pure-func UndefinedVariableEx tag from desc->pure_ctx, inline-frame
  flushes from the chunk's inline_ctxs table at the raise/call pcs. The
  differential suite pins this (every error test compares carets +
  backtraces on both engines).
- `SetPend`/`EndFinally`/`CatchTest`/rethrow read the CURRENT record's
  pend/exc instead of the old per-invocation locals — same semantics,
  now correctly scoped per frame (they always were per-frame in practice,
  because each frame was its own vm_run_chunk invocation).

## Boundaries (what keeps do_func_call)

- **Builtin -> user-function callbacks** (map/filter/sort/make_dict/find):
  the top perf target after calls themselves (this is roadmap C2, absorbed
  here). Builtins get `vm_invoke(view_ctx, fobj, args...)`: it pushes a
  BOUNDARY frame onto the CURRENT activation (records.boundary=1) and runs
  the loop until that frame pops — per-element cost becomes an in-VM call
  plus one C++ call into the loop, with zero per-element allocations. The
  generic eval_func path (tree-walker engine, const-eval) is untouched.
- **Tree-walker engine / REPL / const-eval**: completely untouched —
  do_func_call remains their call protocol; the VM branch inside
  do_func_call remains only as the ACTIVATION ENTRY (main, and any callee
  invoked from non-VM code): it creates/reuses the activation and calls
  vm_run_activation instead of vm_run_chunk.
- **CallBuiltin\*V ops**: unchanged (they never recursed through
  do_func_call).
- **CallValueGenericV**: its FuncObject arm switches to the in-VM call;
  builtin/struct arms unchanged.

## Stack growth & the recursion limit

- Initial slot stack: 4K slots; grow x2 at call boundaries via
  element-wise `std::move` into the new buffer (move-ctors re-register
  array-slice back-pointers; bases are INDICES so records need no fixup;
  the view_frame repoints). Growth is amortized-rare and only at calls.
- Hard cap (new, explicit): default 1M slots (~48MB peak, reached only by
  ~100K-deep recursion), overridable via an env var (`MYLANG_VM_STACK`).
  Exceeding it throws a CATCHABLE RuntimeException
  ("maximum call depth exceeded") with a normal backtrace — an upgrade
  over today's hard segfault at ~5-8K frames. README documents it; the
  C-stack /STACK provisioning note in CLAUDE.md gets updated (the VM no
  longer consumes C stack per call; the tree-walker still does).
- ML_VM_CHECK hardening: `Frame::at` bounds come from the CURRENT record's
  nslots — unchanged strength.

## What this deletes / simplifies

- FlowState round-trip for VM calls; LoopBackEdge opcode (dead already).
- Per-call EvalContext + Frame + three vectors + try-block.
- The per-frame C++ landing-pad cost of cross-frame exceptions (one per
  ACTIVATION now) — 69_exc_crossframe's remaining gap (my/py 1.88, the
  last CPython loss) is this plus per-frame do_func_call unwinding.
- The Windows/ASan deep-recursion fragility for VM runs.

## Phases (each lands -rt green + differential + A/B measured)

- **A. Dead-code prep**: delete LoopBackEdge (opcode/handler/disasm/test
  counter; static_asserts keep the table honest); move the ReturnV comment
  to the new semantics. Zero-risk, isolates the mechanical enum churn.
- **B. The activation skeleton**: VmActivation/VmCallRec; vm_run_activation
  wrapping today's semantics with records but STILL ONE FRAME per
  activation (main only) — proves the view_ctx/builtin ABI compatibility
  with zero call-model change.
- **C. In-VM calls**: CallV/CachedCallV/CallValueV push records (copy
  binding, fast_bind flag); ReturnV/Halt pop; per-frame handler/iter/pend/
  exc/pure_cache moved into records; exception walk + backtrace parity.
  The big phase; gated hard on the differential suite's error tests.
- **D. Boundary invoker (C2)**: vm_invoke for map/filter/sort/make_dict/
  find callbacks (boundary frames).
- **E. Zero-copy arg windows**: codegen arranges arg runs at window top;
  fast_bind + exact-arity calls rebase without copying.
- **F. Measure + docs**: full A/B (call benches at scale 10 best-of-7,
  suite geomean, cachegrind), CLAUDE.md (call model, stack semantics,
  README recursion-limit note), roadmap C1/C2/G1 status.

## Falsifiable risks (and the planned probes)

- **View-ctx compatibility**: some builtin may depend on a property of the
  per-call args_ctx beyond {frame, captures, gfuncs, const flags} (e.g.
  parent-chain shape). Phase B exists precisely to flush this out while
  the call model is still unchanged.
- **Slot-reset semantics**: today a frame's LValues are DESTROYED per call;
  the pool resets them to none. Any code observing slot identity across
  calls (intptr tests?) would diverge — the differential + COW/intptr
  tests are the net.
- **Exception-order parity**: the activation-level catch changes WHERE
  library throws land (one catch instead of per-frame). The backtrace
  construction must reproduce the per-frame capture order exactly; the
  error-test suite pins every caret/backtrace byte.
- **Performance regression risk on NON-call code**: the dispatch loop gains
  frame-switch state (current record). Keep the hot loop's per-op work
  untouched (chunk/pc locals as today); A/B the dispatch set (01/44/60)
  must stay flat — the CGOTO=0 lever plus scale-10 best-of-7 protocol.

## Explicitly deferred

- Serialisation interplay: records/stacks are runtime-only; no .myv impact.
- Tail-call elision at ReturnV(CallV) pairs — natural follow-up, not v1.
- Register-window arg passing for BUILTIN calls (D2 in the roadmap).
