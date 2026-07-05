# VM: an AST-free, serializable bytecode (free the whole AST in -vm mode)

**Goal:** in `-vm` mode, after codegen, **free the entire code AST** (every
`Construct`) and run purely off the `Chunk` — a flat instruction stream plus
serializable side tables. No `Instr` may hold a `Construct *node`. This is what
makes the bytecode (a) dumpable to a file like CPython's `__pycache__`, (b)
lowerable to machine code (JIT / AOT), and (c) smaller per instruction (the
8-byte `node` field goes → better I-cache). See also `bytecode-vm.md` and
`vm-fallback-elimination.md` (the two converge on this end-state).

## The fundamental problem (why we even need a runtime loc table)

A statically-typed compiled language resolves **every type error at compile
time**, so at runtime there is nothing type-related to attach a source location
to; the only runtime faults left (null deref, div-by-zero) are hardware traps.
MyLang is dynamically typed in its `dyn` part, so `a + b` can be a
**`TypeErrorEx` at RUNTIME**, at essentially any operation. To keep error
quality we must point a caret at that op at runtime → we need `pc → loc` for a
large fraction of ops.

This is a solved problem: it is exactly Java's `.class` **`LineNumberTable`**,
CPython's **`co_positions`**, and native **DWARF line programs** — a `pc → loc`
side table consulted **only on the throw/unwind path**, never on the hot path.
MyLang just needs the table to cover MORE ops than a static language, because
more errors are deferred to runtime. The hot path pays nothing; the error path
pays one O(log n) lookup.

## Status of the loc side table so far

`Chunk::locs` (`{pc → (start,end)}`, sorted, binary-searched by `loc_at`) exists
and already made these ops fully `node`-free (they throw via the table): the
register/loop core (`IntBin`/`FloatBin`/`JumpUnlessIntCmp`/`JumpUnlessFloatCmp`/
`ForLoopStep`), `DictLoadInt/Float`, `SubscriptV`, boxed `BinOpV`/`CompoundV`/
`CmpV`/`LogV`, `LoadGlobalV`, `MemberV` (via a member-key pool), and
`CallV`/`CachedCallV` (via a deferred-loc `do_func_call` param). Remaining
`node` users: the builtin calls, `EmplaceStruct`, and the fallback ops
(`EvalStmt`/`JumpIfFalse` + the element-store fallbacks).

## ⚠ Perf regression to RECOVER here (drop `Instr::node`, Step 5)

**2026-07-08 — TRACKED, do NOT lose:** the `bench/run.py --vm` geomean slipped
from ~**4.5×** CPython to ~**4.3×** over the recent fallback-nativization pushes
(sort/reverse, map/filter, const-decl, scalar-spread, ...). This is EXPECTED,
and expected to recover — and then some — at **Step 5 (drop `Instr::node`)**:
every op still carries an 8-byte `Construct *node`, so `Instr` is 64 bytes (2×
an `EvalValue`), and the recent work ADDED node-holding ops
(`CheckFuncV`/`MapFilterV`/`DeclConstV`, plus `sort`/`reverse` via
`CallBuiltinLV`). More native ops → more of the program runs through the VM
dispatch loop, which pays that bloated-`Instr` I-cache cost. Removing the field
shrinks `Instr` → far better dispatch-loop density (the SAME locality argument
that sizes `EvalValue`). So the dip is the temporary cost of nativizing MORE
constructs *before* the node field is gone — the fix is the builtin loc handle
(Steps 1–3) + freeing the AST (Step 5), **NOT** reverting any nativization.
**Action: re-measure `run.py --vm` geomean after Step 5 and confirm it clears
4.5× (target: well beyond, toward the 5× floor).**

## The plan

### Step 1 — Generalize the loc table to carry per-op AND per-arg locs

Each entry references its argument locs via a flat pool (compact + serializable,
DWARF-style):

```cpp
struct LocEntry { uint32_t pc; Loc start, end; uint32_t arg_off, arg_n; };
std::vector<LocEntry> locs;      // sorted by pc, binary-searched on throw
std::vector<Loc>      arg_locs;  // flat pool; entry's args = [arg_off, +arg_n)
```

`chunk.loc_at(pc)` → the op's caret (arity / whole-call errors);
`chunk.arg_loc(pc, i)` → the i-th argument's caret (per-arg type errors). All
`Loc` + `uint32` — no pointers, fully dumpable.

### Step 2 — Give builtins a loc HANDLE, not an `ExprList` (the key move)

A builtin today reaches into `exprList->elems[i]->start` for a per-arg caret.
Replace that with a small handle it queries only inside a `throw`:

```cpp
struct BuiltinLocs {
    const ExprList *ast;    // tree-walker mode: the AST is retained
    const Chunk   *chunk;   // VM mode: the AST is freed
    uint32_t       pc;
    void arg(size_t i, Loc &s, Loc &e) const {
        if (ast) { s = ast->elems[i]->start; e = ast->elems[i]->end; }
        else       chunk->arg_loc(pc, i, s, e);
    }
    void list(Loc &s, Loc &e) const {                       // arity errors
        if (ast) { s = ast->start; e = ast->end; }
        else       chunk->loc_at(pc, s, e);
    }
};
```

The tree-walker (which keeps its AST) backs it with the `ExprList`; the VM backs
it with `(chunk, pc)`. **One builtin implementation, two loc sources, zero
hot-path cost** — the handle is only touched inside a `throw`.

**Why this is what makes the ~60-site edit tractable.** A first attempt (an
`ArgsLoc {start,end}` param) was reverted because read-only (`func_v`) and
mutating (`func_lv`) builtins BOTH write `exprList->elems[i]->start`, so a sed
to change only `func_v` corrupted the `func_lv` ones, ~37 builtins / ~60 sites.
Routing **every** builtin's loc access through the SAME
`locs.arg(i)` / `locs.list()` handle makes the rewrite uniform and mechanical
(and a `func_lv` builtin keeps the real node SEPARATELY, only for the eval it
still does). "One place for the locs" is precisely what unblocks the edit.

### Step 3 — Remove the last node-EVAL uses

A few `func_lv` builtins (`append`/`push` self-eval) still read arg NODES to
construct-in-place. Make them "rest-native" (args pre-evaluated, like
`insert`/`erase` already are) so they need no node. `EmplaceStruct` already
proves the pattern for the struct-ctor case.

### Step 4 — Nativize the fallback ops (the other half)

`EvalStmt` and `JumpIfFalse` literally call `node->eval()` — the AST cannot be
freed while they exist. Their elimination (see `vm-fallback-elimination.md`) is
the same end-state as this plan, not a detour. This is the current priority.

### Step 5 — Free the code AST in -vm mode, drop `Instr::node`

Once no op and no builtin holds a `Construct *`, free the root AST after codegen
(`-vm` only; tree-walker mode keeps it). Then `Instr` sheds the 8-byte `node`
field.

## Caveats to design in NOW

1. **Freeing "the AST" ≠ freeing everything it OWNS.** Some AST-owned things are
   live runtime DATA, not code: `StructTypeDef`s (a struct instance points at
   its def), interned `UniqueId`s, and the const / member-key / arg-loc pools.
   Those
   must be lifted out (or their owning nodes retained) before the code nodes are
   freed. So it is "free the code Constructs, keep the data descriptors."
2. **The backtrace is already mostly AST-free** — `do_func_call` captures each
   frame's name/params as STRINGS during unwinding (the AST may be gone). Only
   the call-site loc needs the table (already done for `CallV`).
3. **General/user exceptions stay fast.** MyLang uses `FlowState` (not C++
   exceptions) for return/break/continue, so the loc table is touched only by a
   genuine `throw` — one O(log n) lookup on the cold path, nothing on the hot
   path.

## Order of work

Fallback ops first (Step 4 — the real `node->eval` re-dispatch, highest value),
then the builtin loc handle (Steps 1–3, a well-scoped mechanical pass), then
free the AST (Step 5). The builtin loc ABI is NOT a speed win (cold carets
only); its value is serializability + dropping the `node` field.
