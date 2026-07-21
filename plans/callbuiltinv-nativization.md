<!-- SPDX-License-Identifier: BSD-2-Clause -->
# Nativize CallBuiltinV (model-flip nativize-ops) — design decisions

Part of the model-flip nativize-ops arc (`plans/model-flip.md`). CallBuiltinV is
the **biggest island source (~294 occurrences)** — a value-ABI (`func_v`)
read-only builtin call. Attempting it surfaced several issues that make it bigger
than the clean incremental ops (MoveV/Subscript/... already done). This file
records the decisions from the 2026-07-22 discussion so the work can be executed
later. Status: **DESIGN — not yet implemented** (a first attempt was reverted to
green; the branch is clean).

The mechanism (working, from the reverted attempt): a helper
`jit_call_builtin(dst, base, n, bc)` where `bc = &chunk.builtin_calls[idx]` (a
stable pool-buffer address). It builds the `ArgLocs` from `bc` (no chunk needed —
exactly `arglocs_at`'s fields), copies args from frame slots `[base, base+n)`
into a stack buffer (n<=8) or a heap one, calls `bc->builtin.func_v(ctx, &al,
buf, n)` (the interpreter's exact body — a callback builtin like
`make_array`/`make_dict`/`find` re-enters `vm_dispatch`, which saves/restores
`g_current_ctx`, so `ctx` is valid for the dst write after), and writes the
result. N5-disqualified (a callback can mutate arbitrary slots). Emit: rdi=dst,
rsi=base, rdx=n, rcx=bc, then `test eax; jnz exit_pc(pc)` on the throw path.

---

## #1 — EXCEPTION CONVEYANCE (correctness blocker) — DECIDED

**Problem.** `func_v` builtins throw plain `Exception` types (`DECL_SIMPLE_EX`,
NON-`RuntimeException`, non-script-catchable) at runtime — the inferencer does
NOT arity-check builtin calls (only `builtin_result` for the type; user funcs get
compile-time `WrongArgCountEx`), so those throws are REACHABLE:
- `InvalidNumberOfArgsEx` — `write("a","b","c")` (write takes 1-2 args; the opt
  2nd is a filename). Confirmed runtime throw (`-e 'write("a","b","c");'` runs to
  execution then throws at the args caret).
- `InvalidArgumentEx` — a bad VALUE into `max`/`min`/`round` (all `func_v`).
  Inherently DYNAMIC, so NOT compile-checkable.

The JIT helper is `noexcept` (called from the native fragment — no C++ unwind
info), so it catches the exception and stashes it in `g_vm_jit_exc`
(`unique_ptr<RuntimeException>`). `catch (RuntimeException &e)` does NOT match a
plain `Exception` → it escapes the `noexcept` helper → **`std::terminate`**. (The
tree-walker's CallBuiltinV catches the broad base `Exception &e`, so it's fine.)

**DECISION (maintainer, per "FIX the design, don't work around it"): make those
exceptions inherit from `RuntimeException` — do NOT widen the native code to
carry non-runtime exceptions.**
- `InvalidArgumentEx`: `DECL_SIMPLE_EX` → `DECL_RUNTIME_EX` — **permanent**
  (inherently dynamic; belongs with the catchable runtime errors).
- `InvalidNumberOfArgsEx`: `DECL_SIMPLE_EX` → `DECL_RUNTIME_EX` — **for now**.
- Then the JIT's EXISTING `catch (RuntimeException)` + `g_vm_jit_exc` conveys
  them — **zero native-code change**.

**FOLLOW-UP (later, separate task — NOT part of this):** give builtins FIXED
arities so the inferencer can arity-check builtin calls at COMPILE time, which
makes the runtime `InvalidNumberOfArgsEx` throw disappear for direct calls (and
`InvalidNumberOfArgsEx` could then go back to a hard `DECL_SIMPLE_EX`). Concrete:
split `write(str [, file])` into two fixed-arity builtins:
- `write(str)` — to stdout.
- **`fwrite(file_or_handle, str)`** — FILE/HANDLE FIRST, string SECOND.

**Completeness (verified) for `func_v`/CallBuiltinV:** the two above are the only
REACHABLE plain-`Exception`s. Others:
- `CannotChangeConstEx` (const mutation) is `func_lv`-only (insert/erase/append/
  sort) — NOT reachable via CallBuiltinV; apply the SAME "make it
  `RuntimeException`" fix when the LVALUE builtins (CallBuiltinLV) are nativized.
- `InternalErrorEx` in `builtin_str` (`snprintf` returning < 0 for a float) is a
  can't-happen defensive check — effectively unreachable; leave it.

**Doc-sync when implementing:** README error-model section moves
`InvalidNumberOfArgsEx`/`InvalidArgumentEx` to the catchable/`RuntimeException`
list + the `fwrite`/fixed-arity follow-up note. `typeid` is unchanged, so the
existing `typeid`-checking tests (e.g. "write() with too many args") still pass.
NOTE the behavior change: these two become **script-catchable** (a `try/catch`
can now catch them) — accepted (arguably more correct).

---

## #2 — DELETE-ORIGINALS interaction — DECIDED (one line, no regression)

**Apparent problem.** CallBuiltinV can throw, so it is NOT `op_fully_native`;
when it joins a preceding fully-native run (e.g. a pure-int loop then
`print(s)`), that run is no longer DELETABLE and keeps its interpreted originals
(a `.myv`-cleanliness regression + the `jit_delete_originals` test broke).

**Key insight.** Deletability really requires *"no bail that RE-EXECUTES the
interpreted original"*, NOT *"never throws"*. `op_fully_native` conflates the
two. Every approach-A throwing op (CallBuiltinV, SubscriptV, DictLoad,
boxed-arith) does not bail-to-re-execute — it **RE-RAISES**: on a throw it
stashes into `g_vm_jit_exc` + exits to its pc, and `EnterNative` re-raises
(never re-running the interpreted original). So deleting the original is
harmless for control flow. The ONE thing deletion breaks is the **caret**: a
deleted run collapses every interior pc (and the remapped pc-keyed loc side
table, `jit.cpp` ~2588) onto the `EnterNative` pc. An op whose caret lives in
the loc side table (SubscriptV/DictLoad) would then be lossy/ambiguous.

**But CallBuiltinV does NOT use the loc side table** — its caret is in the
`builtin_calls` pool (`bc.start/end`), and `jit_call_builtin` stamps
`e.loc_start` from `bc` BEFORE stashing. So a deleted CallBuiltinV's throw is
byte-correct regardless of deletion (`vm_raise`'s `if (!e.loc_start)` won't
re-stamp).

**DECISION: add `CallBuiltinV` to `op_fully_native`.** A run of (fully-native
ops + CallBuiltinV) becomes deletable again — the pure-loop-plus-`print` case
deletes as before. **No regression, no `jit_delete_originals` test change.**
Sound precisely because CallBuiltinV conveys its own loc.

### BIGGER OPPORTUNITY (separate task, DEFERRED — keep this note)
The OTHER re-raise ops — **SubscriptV, DictLoad, boxed-arith (BinOpV/CmpV/
CompoundV/UnaryV), CoerceNumV** — could ALSO be made deletable (they all
re-raise, never re-executing their original), which would **EXTEND**
delete-originals to loops containing subscripts / dict reads / boxed arith — a
real `.myv`-size + "100% native" win, and the right model-flip direction. The
blocker is the loc side table: those ops get their caret from the pc-keyed
`chunk.locs`, so a deleted run with >=2 such ops collides their loc entries on
the single `EnterNative` pc (`loc_at` returns the wrong one). To do it right:
either give each such op an INDEPENDENT loc conveyance (like CallBuiltinV's
pool), or keep the loc side table addressable after deletion (e.g. do NOT
collapse a deleted throwing op's loc onto the EnterNative pc — keep a per-op
entry), or restrict a deletable run to at most one side-table-loc throwing op.
Worth a dedicated pass; NOT part of CallBuiltinV.

## #3 — fragmentation quirk — ROOT-CAUSED (a PRE-EXISTING gap, NOT CallBuiltinV)

**Observation.** `for (i) s = s + abs(0 - i)` did NOT fragment (the loop stayed
interpreted) while a `len`-based loop did.

**Root cause (per-op eligibility dump `52E 52E 7E 1- 41E 122E 51E`):** pc 3 is a
**generic `IntBin` (opcode 1) — NOT `jit_op_eligible`**. `0 - i` is
*literal-minus-slot*; it can't specialize to `IntSubRI` (that's *slot* − imm, and
subtraction isn't commutative so `imm - slot` can't be swapped), so it stays a
generic `IntBin`. Only the B1/B2-SPECIALIZED int ops (`IntSubRR`/`IntSubRI`/...)
are `jit_op_eligible`; generic `IntBin` is not. So that one op SPLITS the 7-op
loop into `[0,3)` + `[4,7)`, both below `MIN_RUN=4` → `runs=0` → no fragment.
**CONFIRMED:** `abs(i)` (no subtraction → no generic IntBin) gives
`runs=1 [0,7)` — the loop fragments fine, CallBuiltinV included.

**Conclusion:** this is a pre-existing gap ORTHOGONAL to CallBuiltinV -
nativizing CallBuiltinV neither causes nor worsens it. It needs NOTHING for the
CallBuiltinV work.

**Separate optional improvement (own task):** make the JIT cover a
literal-minus-slot subtraction, either by adding a specialized `imm - slot`
form to `specialize_arith_ops` (an `IntSubIR`), or by making a NON-div/mod
generic `IntBin` `jit_op_eligible` (div/mod throw on 0, so gate them out). Small
win for `0 - i` / `k - i` loop shapes; not needed here.

## #4 — is CallBuiltinV worth it?

The per-call dispatch removal is SMALL (the builtin's own C++ work dominates -
like the dict-store tier). The value is STRUCTURAL: CallBuiltinV is the biggest
single island source (~294), and the whole model-flip point is shrinking islands
toward full-native. So it's a milestone step, not a perf play. LANDED on that
basis.

---

## Implementation order (once decided)
1. **#1 exception (foundation):** flip `InvalidArgumentEx` + `InvalidNumberOfArgsEx`
   to `DECL_RUNTIME_EX`; README error-model + the `fwrite` follow-up note. Verify
   `-rt` (the "write() too many args" typeid test still passes) + that the two
   are now script-catchable.
2. **CallBuiltinV op:** the helper + emit + pool-bake `bc`, `catch (RuntimeException)`
   now sufficient. Add to `jit_op_eligible` + `pick_cached_slots` return `{}`.
3. **#2:** add `CallBuiltinV` to `op_fully_native` (deletable; no test change).
4. Verify: `-rt`, differential, nested_fuzz, clang; the throwing paths
   (InvalidNumberOfArgs / InvalidArgument / a callback user-throw) caret-correct
   vs `-tw`; a real bench (dict/wordcount) runs CallBuiltinV natively (counter).
   NOTE the counter caveat: main's own `runtime()`/`print()` are CallBuiltinV, so
   a bench/loop verification must attribute the count to the loop, not main.

## STATUS: IMPLEMENTED (2026-07-22)

- #1 exception change: `errors: make InvalidArgumentEx + InvalidNumberOfArgsEx
  runtime (catchable)` (fa90955). README updated.
- CallBuiltinV op + #2 (op_fully_native): helper + emit + pool-bake `bc`;
  `catch (RuntimeException)` now sufficient; jit_op_eligible + op_fully_native +
  pick_cached_slots `{}`.
- VERIFIED: -rt 1570/1570, differential 1403/1403. The LOOP's builtin runs
  natively (CallBuiltinV=52 for a 50-iter `len` loop = 50 loop + 2 main). #2
  works (pure loop + `print` deletes its int ops JIT-on: 0 vs 5). Throwing paths
  byte-identical vs -tw: InvalidNumberOfArgsEx (write 3 args) caret + catchable
  in a fragment; a runtime callback throw (make_array's gen div0) caught in a
  fragment (vm==tw==nj). InternalErrorEx (str snprintf<0) is the one unreachable
  non-RuntimeException - documented, left.
