# VM exceptions (P8) — design + increments

**Goal.** Make `try` / `catch` / `finally` / `throw` / `rethrow` and the
built-in runtime errors run as **native VM ops**, so:
1. **`.myv` serialization** (`plans/bytecode-vm.md` END GOAL): a `try` currently
   compiles to ONE `EvalStmt` (an AST reference) — categorically disallowed in a
   serialized image. Exceptions is the **last construct-level fallback** and
   thus a hard PREREQUISITE for `.myv`.
2. **Kill the C++-throw overhead**: a user `throw` caught in the same frame
   today costs a C++ `throw` (~1.6 µs — heap alloc + DWARF unwind).
   `42_exceptions` (200k throw/catch in a loop) is the single most-dramatic
   per-bench win.

This expands the sketch in `plans/bytecode-vm.md` (lines ~771). See
`[[vm-endgame]]`.

---

## What must stay byte-identical (the differential contract)

The `-rt` VM differential + `nested_fuzz.py` compare **exception type, message,
`Loc`, and the full backtrace** against the tree-walker. So the VM mechanism
must reproduce, exactly:
- **User exceptions are STRUCTS.** `throw <struct>` wraps the instance in an
  `ExceptionObjectTempl` (a `RuntimeException`, C++ name `DynamicExceptionEx`,
  `dyn_name` = the struct type's name). `throw` also accepts a caught built-in
  exception (re-throw); anything else is a `TypeErrorEx` (and a statically-known
  non-struct throw is a compile-time `TypeMismatchEx`).
- **`catch` matches by NAME** (`do_catch`): the exception's `dyn_name` (user) or
  the built-in name (`DivisionByZeroEx`, `OutOfBoundsEx`, …). Clauses are tried
  **in order**; `catch (T as e)` binds `e` to the struct instance
  (`exObj->get_data()`), or to a fresh `ExceptionObject` wrapper for a
  payload-less built-in. The catch var is typed `dyn`.
- **`finally`** runs on EVERY exit path (normal, exception, and a `return` /
  `break` / `continue` crossing it), suspending an in-flight signal and resuming
  it after — today a C++ scope guard in `TryCatchStmt::do_eval`.
- **`rethrow`** (only in a catch) re-throws the saved exception with the rethrow
  site's `Loc`.
- **Runtime errors** (div-zero, OOB, KeyNotFound, …) are thrown by C++ `throw`
  from deep inside the runtime library (`num_bin_op`, `Type` ops, builtins) and
  are catchable by name.
- **Uncaught** → the top-level handler prints type/message/caret + the
  backtrace.

---

## The mechanism

Three pieces, all `vm_run_chunk`-local (so per function invocation, like the
`FlowState`):

```cpp
// 1. The in-flight exception ("pending") register.
EvalValue vm_exc; // the thrown value (an ExceptionObject); none == clear

// 2. The HANDLER STACK: one entry per ACTIVE try region (pushed on entry,
//    popped on normal exit / when it handles). A stack ⇒ nested try is free.
struct VmHandler {
    uint32_t catch_pc;     // catch-dispatch entry (0 ⇒ no catch clauses)
    uint32_t finally_pc;   // finally entry        (0 ⇒ no finally)
    uint32_t end_pc;       // resume point after the whole try/catch/finally
};
std::vector<VmHandler> vm_handlers;   // sized from Chunk::n_handlers (hint)

// 3. The PENDING ACTION a finally must resume after it runs (see finally).
enum class Pend : uint8_t { normal, reraise, ret, brk, cont };
Pend    vm_pend = Pend::normal;
EvalValue vm_pend_val;     // return value (for Pend::ret)
```

### New ops (AST-free / serializable — pc-offset + pool-index operands)

| Op | operands | effect |
|---|---|---|
| `PushHandler` | `catch_pc, finally_pc, end_pc` | `vm_handlers.push_back({...})` |
| `PopHandler`  | — | `vm_handlers.pop_back()` (normal exit of the try body) |
| `Throw`       | `a` = value temp | `vm_exc = wrap(RValue(slot a))`; `dispatch_exc()` |
| `Rethrow`     | — | `dispatch_exc()` with the current `vm_exc` (loc from side table) |
| `CatchTest`   | `type_idx`(pool), `handler_pc` | if `vm_exc` matches the type name, bind + jump `handler_pc`; else fall through |
| `CatchAll`    | `handler_pc` | unconditional (a bare `catch`/`catch (e)`); jump |
| `EndCatchDispatch` | — | no clause matched → run finally then re-raise (see below) |
| `EndFinally`  | — | resume `vm_pend` (reraise / ret / brk / cont / normal) |

`CatchTest` reuses `do_catch`'s name matching and `catch (T as e)` binding
verbatim (a factored `vm_catch_match(vm_exc, type_name, bind_slot)`), so
behavior is identical.

### `dispatch_exc()` — the core (native, no C++ throw)

```cpp
bool dispatch_exc(size_t &pc) {        // returns false ⇒ propagate to caller
    while (!vm_handlers.empty()) {
        VmHandler h = vm_handlers.back();
        vm_handlers.pop_back();        // this region is now being unwound
        if (h.catch_pc) {              // has catch clauses → try them
            pc = h.catch_pc;           // the CatchTest chain runs next
            return true; // vm_exc still set; a clause may clear it
        }
        if (h.finally_pc) {            // no catch, but a finally must run
            vm_pend = Pend::reraise;   // finally runs, THEN re-raises vm_exc
            pc = h.finally_pc;
            return true;
        }
        // neither → keep unwinding the handler stack
    }
    return false;                      // exhausted this frame's handlers
}
```

If `dispatch_exc` returns **false**, the current frame has no handler: the VM
re-throws `vm_exc` as a **C++** `RuntimeException` so it unwinds through
`do_func_call` to the CALLER's `vm_run_chunk`, whose C++ boundary (below) feeds
it into the caller's handler stack. So **cross-frame** propagation pays one C++
throw per un-catching frame crossed; a **same-frame** throw/catch pays NONE (the
`42_exceptions` win).

### The C++ boundary (REQUIRED — runtime errors + nested-call propagation)

Runtime errors (div-zero/OOB/…) are C++ throws from the runtime library, and an
un-catching callee re-throws C++. Both must land in THIS chunk's handler stack.
So `vm_run_chunk` wraps its dispatch loop:

```cpp
void vm_run_chunk(chunk, ctx) {
    ... vm_exc, vm_handlers, vm_pend ...
    size_t pc = 0;
  resume:
    try {
        for (;;) { switch (chunk.code[pc].op) {
            ...
            case Throw:
                vm_exc = wrap(RValue(frame[in.a]));
                if (dispatch_exc(pc)) break;          // native: jump to handler
                throw exc_to_cpp(vm_exc); // no handler here → C++ up
            case Rethrow:
                if (dispatch_exc(pc)) break;
                throw exc_to_cpp(vm_exc);
            ...
        } }
    } catch (Exception &e) { // runtime error OR callee C++
        vm_exc = cpp_to_exc(e); // reuse the existing object
        if (dispatch_exc(pc)) goto resume; // route into the handler stack
        throw; // no handler → propagate up
    }
}
```

The `try{}catch` is **zero-cost on the happy path** (table-based unwinding), so
wrapping the loop is free unless something actually throws. A same-frame native
`Throw` never reaches the `catch` (it jumped in `dispatch_exc`); only real
runtime errors and cross-frame propagation do. `exc_to_cpp` / `cpp_to_exc` reuse
the existing `ExceptionObject` / `RuntimeException` so the type/message/loc/
backtrace are the SAME objects the tree-walker produces.

### `finally` — the pending-action resume (shared, not duplicated)

`finally` code runs on the normal path, each catch's post-path, and the
no-clause-matched path. Instead of duplicating it, each path sets `vm_pend` and
jumps to the single `finally_pc`; `EndFinally` resumes:

```cpp
case EndFinally:
    switch (vm_pend) {
        case Pend::normal: vm_pend reset; break; // fall through to end_pc
        case Pend::reraise:
            vm_pend = normal;
            if (dispatch_exc(pc)) break; // re-raise to the OUTER handler
            throw exc_to_cpp(vm_exc);
        case Pend::ret:  flow->type=ret; flow->value=vm_pend_val; ret;
        case Pend::brk:  flow->type = brk;  ... // resume the suspended loop
        case Pend::cont: flow->type = cont; ...
    }
```

A `return`/`break`/`continue` **inside** a try-with-finally compiles to
`SetPend {ret|brk|cont, val}` + `Jump finally_pc` instead of the direct
flow-op, so finally always runs first — mirroring the tree-walker's scope-guard
suspend/resume exactly.

---

## Compiled bytecode layout (one worked example)

```
try   {  BODY                         }
catch (DivZero as e) {  CATCH_A       }
catch (MyErr)        {  CATCH_B       }
finally              {  FIN           }
```

lowers to (→ = fallthrough, all labels are pc offsets baked at codegen):

```
        PushHandler catch=Lcatch, finally=Lfin, end=Lend
        <BODY ops>
        PopHandler                    ; normal exit: this region done
        SetPend normal
        Jump Lfin                     ; normal path still runs finally
  Lcatch: ; dispatch_exc jumped here; handler popped; vm_exc set
        CatchTest DivZero -> Lca      ; matches → bind e, clear vm_exc, jump
        CatchTest MyErr   -> Lcb
        EndCatchDispatch              ; no clause: SetPend reraise; Jump Lfin
  Lca:  BindCatch e ; <CATCH_A> ; SetPend normal ; Jump Lfin
  Lcb:              ; <CATCH_B> ; SetPend normal ; Jump Lfin
  Lfin: <FIN ops>
        EndFinally ; resume vm_pend (reraise / ret / brk / cont / normal)
  Lend:
```

No `EvalStmt` anywhere → serializable. `PushHandler`/jumps carry pc offsets;
`CatchTest` carries a **pool index** to the interned type name (already how
`member_keys` works). `Throw`'s value is a register temp (the struct built by
the existing `MakeStruct`/`StructCtorV` ops).

---

## Increment breakdown (multi-commit, each differential-gated + `-vd`-audited)

Deliberately NOT big-banged — `finally`/rethrow/nested/backtrace make it a
focused effort.

- **Inc 0 — structure, behavior identical (safe base).** `try/catch/finally`
  compile to a handler region (`PushHandler`/`PopHandler`/`CatchTest`/
  `EndFinally`) + the handler stack + the C++ boundary, **but `throw` and
  runtime errors STILL go through C++** (caught by the boundary and routed into
  the handler stack). Removes the `EvalStmt` for `try` (structure native) with
  ZERO behavior change — the safest possible base. `42_exceptions` is not yet
  faster (throw is still C++) but is now real ops.
- **Inc 1 — native `Throw` (THE win).** A same-frame `throw`→catch jumps via
  `dispatch_exc`, no C++ throw. `42_exceptions` drops sharply. Cross-frame
  throw still uses the C++ boundary.
- **Inc 2 — `finally` + `rethrow` + `catch (T as e)` binding fully native**
  (the pending-action resume; `SetPend` for return/break/continue in a try).
- **Inc 3 — nested try / dynamic nesting.** The handler STACK already supports
  it; this increment is really "prove it": lexical `try{ try{}catch{} }catch{}`
  and a `try` in a called function. (If Inc 0–2 shipped with a depth-1 handler
  for simplicity, this generalizes to the stack.)
- **Inc 4 — backtrace parity for the uncaught path** (the `BacktraceFrame`
  capture must stay byte-identical — the differential checks it).

## "Nested exceptions" — clarifying the maintainer's ask

The maintainer: *"initial version OK without nested exceptions (like C++); after
that, like Python does."* Two readings, and this plan covers both:

- **Nested TRY BLOCKS** (a try in a try, or a try in a called function). The
  handler STACK gives these essentially for free, so the plan **builds them in
  from Inc 0** (a stack, not a single slot) — dynamic nesting across frames is
  unavoidable anyway (a function with a `try` called from within a `try`).
- **Exception CHAINING** — the more likely reading of "C++ vs Python", and the
  real v1-vs-later feature: when a `throw` happens **while handling** another
  exception (inside a `catch`/`finally`), C++ by default just replaces it, while
  **Python chains** it (`During handling of the above exception, another
  exception occurred:` — `__context__`). **v1 = C++ default (replace);** a
  **later increment adds Python-style chaining**: keep the being-handled
  `vm_exc` as the new exception's `context`, and the uncaught printer walks the
  chain. This needs (a) a `context` field on the exception object/struct
  wrapper, (b) the printer/backtrace to render the chain, (c) a decision on the
  surface syntax (implicit context like Python, or explicit `throw X from Y`).
  Deferred, but the handler-stack design leaves room (the currently-handled
  exception is knowable).

**Open question for the maintainer:** did "nested" mean nested try blocks or
exception chaining? The plan assumes chaining is the "later, like Python" part
and builds nested try blocks in from the start; confirm.

---

## Non-goals / notes

- Runtime errors stay C++ throws for now (caught at the boundary). Making THEM
  non-C++ (return-code propagation, CPython-style) is a separate, much larger
  refactor of the whole runtime library — deferred; not needed for `.myv` (the
  C++ throw of a runtime library fn is not an AST reference) nor for the
  `42_exceptions` win (which is user throw/catch).
- The eventual **machine-code** phase (`[[vm-endgame]]`) will need the runtime
  library's error signaling revisited (a landing-pad / error-register), but the
  interpreter VM's boundary-catch is correct and cheap until then.
- Serialization: every op here is pc-offsets + pool indices → fully
  serializable; `Chunk` gains `n_handlers` (a sizing hint) + the type-name pool
  entries (already interned strings).
