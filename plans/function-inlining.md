# Function inlining & specialization

Status: **planning / in progress**. Branch: `exp-work`.

This is the design + task plan for the long-planned "function inlining" feature
noted in `CLAUDE.md` (the value & type model / const-evaluation sections). It
supersedes that note's two open questions (the inlining criterion and the
backtrace handling), which are resolved below.

## Goals

1. **Const specialization of non-pure functions.** Today only pure / auto-pure
   *whole-call* folding propagates constants across a call. A non-pure function
   with a const (or auto-const) argument gets no benefit. Fix that.
2. **Inline trivial functions even with zero known args.** `func f(x) => x + 1`
   called `f(y)` should become `y + 1` spliced in place: it removes the
   `do_func_call` overhead and drops the body into the caller where the existing
   const-folder can reach it (`f(2*3)` -> `7`).
3. **Keep backtraces identical** whether or not inlining happened. This is the
   hard constraint: inlining with wrong backtraces is unacceptable for an
   educational language.

## Non-goals (deferred, with reasons)

- **General algebraic simplification of non-constant operands** (`x+1-1 -> x`).
  Unsound in a dynamically-typed language without type knowledge:
  - floats are not associative (`(y+1.0)-1.0 != y`);
  - `+`/`-` are overloaded — `+` is concatenation for strings/arrays, and
    `("a"+1)` *throws* `TypeError`, so collapsing `("a"+1)-1` to `"a"` would
    erase a runtime error;
  - every operator is type-dispatched, so essentially no identity (`x+0`,
    `x*1`, ...) is sound across all dynamic types.
  Requires a type-narrowing / "provably-int" analysis first. Inlining will be
  built to *feed* such a pass (original locs + full structure preserved), but
  the pass itself is out of scope here. What IS captured for free after
  inlining is **constant** folding across the call boundary (sound).
- **Inlining builtin callbacks** (the `sort(arr, cmp)` comparator, `map`,
  `filter`). Those are C++-side callbacks, not AST call sites; AST inlining does
  not touch them. Highest-value target but a different mechanism — not here.
- **Recursive-function specialization** (bounded versioning, à la GCC). Risky;
  defer.

## Background (current pipeline)

`lexer -> parser (const-fold woven in) -> resolve_names() -> tree-walk eval`.

- `resolve_names()` (`resolver.cpp`): pass 1 resolves functions + collects
  `escaped`; pass 2 resolves the top level as "main"; then
  `AutoConst().run(root, writes)` folds write-once scalars, all-literal
  expressions, does DCE, and folds pure/const-builtin calls. It records
  `slot_writes` (per-slot write counts), `auto_const_param` (a param never
  reassigned), and `FuncDeclStmt::effective_pure`.
- Backtrace model (`errors.h`, `backtrace.cpp`): `Exception::backtrace` is a
  `vector<BacktraceFrame{ name, params, call_site }>` filled innermost-first as
  the exception unwinds. `do_func_call`'s `catch` pushes one frame per physical
  call (name/params copied as strings; `call_site` = the `CallExpr`'s loc).
  `format_backtrace` renders it: frame 0's line = `e.loc_start` (the error
  site); frame i>0's line = `bt[i-1].call_site.line` (where frame i-1 was
  called); a synthetic `main()` sits at the bottom.

## The unified mechanism: specialize -> fold -> measure -> decide

One routine drives everything. At a direct call site `f(args)`:

1. **Clone** `f`'s body (deep AST copy).
2. **Bind** each propagatable param to its constant: a param is propagatable iff
   it is `const` or never reassigned in the body (`auto_const_param` /
   `slot_writes == 0`) AND the corresponding arg is compile-time-known. Leave
   the rest opaque.
3. **Fold** the clone with the existing folder (`AutoConst` over a partial const
   environment).
4. **Measure**: weighted node count `S` of the folded body; benefit
   `B = original_size - S`.
5. **Decide**:
   - `S <= INLINE_THRESHOLD`, no surviving loop, not (transitively) self-
     recursive -> **inline** (splice the folded body at the call site).
   - else `B` large (significant fold) -> **emit / reuse a clone**
     `f$<const-tuple>` and redirect this call site to it.
   - else -> discard the temp; keep the ordinary call to `f`.

The **size-only inline trigger** is the same routine with the body already tiny
*as written* (zero const args needed): if `f`'s body is `<= INLINE_THRESHOLD`,
inline regardless of arg constness.

Clones are **deduped** by `(f, const-arg-tuple)` so multiple call sites share
one specialization — reusing the canonical-key idea from the parse-time CSE
work (`cse_key`).

### Why two outcomes

- **Non-inline clone**: still called via `do_func_call` (no call-overhead win),
  but the folded body does less work per call. **Backtrace-free** — it is a real
  callable; give it `f`'s display name and the existing machinery is unchanged.
- **True inline**: removes the call entirely; needs the backtrace machinery
  below.

## Argument-substitution soundness (the inliner's correctness spec)

Splicing "as if written in place" must preserve argument evaluation **order and
count**:

- **Const arg** -> substitute the literal directly (this unlocks cross-boundary
  folding).
- **Non-const arg, param used <= 1x**, OR **arg is side-effect-free and cheap**
  (a literal or a bare identifier) -> substitute the arg expression directly.
  Duplicating an identifier/literal read is safe, so multi-use `f(x)=>x*x`
  called `f(z)` -> `z*z` is fine.
- **Non-const arg, param used >= 2x and arg not trivially duplicable**
  (e.g. `f(g())`, `g` side-effecting, `x` used twice) -> **temp-bind**:
  `{ var __p = g(); ... }`. Preserves single evaluation but only works in
  *statement* position (no expression-context folding).
- **Param used 0x** -> still evaluate the arg for side effects, then discard.

**Expression-body vs block-body.** `=> expr` functions drop straight into an
expression context and compose with surrounding operators (the high-value case,
e.g. `f(2*3)+1`). Block-bodied functions can only be inlined in *statement*
position (body spliced, result via a temp), so they get the call-overhead win
but not the expression-folding win. Start with expression bodies.

## Backtrace design (true inlining)

DWARF-analogous "inlined-at" chains, mapped onto the existing `BacktraceFrame`
model. **`format_backtrace` needs no changes.**

- Add `InlineCtx { callee_name, params, call_site, parent }` — a chain ordered
  innermost-callee -> outermost. Each element maps **1:1** to a
  `BacktraceFrame{ name=callee_name, params, call_site }`.
- Every node spliced from `f`'s body carries (a pointer to) the `InlineCtx` for
  that expansion. Nested inlining (g-in-f-in-h) **rebases**: when f's
  already-g-inlined body is inlined into h, the g-nodes' chain gets f's context
  appended as the new outermost parent.
- Inlined nodes keep their **original locs** (so the caret still points at the
  real source of the inlined function).
- **Reconstruct by "flushing a node's chain" at two points, both already on the
  unwind path:**
  1. `Construct::eval`'s loc-stamp (`eval.cpp`, in the `catch`, guarded by
     `if (!e.loc_start)` so it fires once at the innermost node): flush the
     *error node's* chain -> the virtual frames for an error *inside* inlined
     code.
  2. `do_func_call`'s `catch`: after pushing the physical callee frame, flush
     the *call-site node's* chain -> the virtual frames when a *physical*
     function is called *from* inlined code.
- The two cases are **disjoint** (no double-count) and compose for arbitrary
  nesting. Worked example (error at plain node N in g-inlined-in-f, f physical):
  loc-stamp flushes `[g]` -> `bt=[g]`; f's `do_func_call` pushes `f`; result
  `[g, f, ... main]`, lines all correct.

Two properties that make this cheap and safe:

- **Error-path-only.** `inline_ctx` is never consulted during normal eval — only
  in the two `catch`/stamp handlers. Zero common-case cost.
- **String lifetime.** The flush copies `name`/`params` out of `InlineCtx` into
  `BacktraceFrame` at flush time (during unwind, while the node is still alive)
  — exactly what `do_func_call` already does, because capturing-closure bodies
  can be torn down mid-unwind. So `InlineCtx` may be a field on the node
  (survives closure body-clone) rather than a side table.

## Prerequisites & hard parts (discovered while grounding the plan)

1. **AST deep-clone does not exist.** `Construct` has no `clone()`. Inlining and
   specialization both require copying a callee body. Add a virtual
   `clone()` across the ~30 node types (faithful copy: locs, `is_const`,
   children deep-copied; `Identifier` keeps `uid`/`const_param`). This is the
   first foundational task.
2. **Re-resolution of spliced bodies.** Inlining runs *after* `resolve_names`,
   so a cloned body's `Identifier::sym`/slot indices refer to the *original*
   function's frame. Spliced into the caller they are wrong. Options:
   - re-resolve the spliced region in the caller's frame (remap slots, bind
     param refs to the substituted args/temps), or
   - leave the spliced locals unresolved (fall back to the map — always safe,
     slower). Start with the safe fallback, optimize later.
3. **Pass ordering.** Decisions need fold info (post-`AutoConst`) and reassign
   info (post-resolve). Proposed: `resolve -> AutoConst -> inline pass ->
   re-resolve spliced regions -> re-fold (AutoConst)`, optionally iterated to a
   fixpoint with a budget.
4. **Termination / blowup guards.** Never inline a (transitively) recursive
   edge; cap inline depth (e.g. 2-3); cap total added nodes (AST-growth
   budget); `log`/document any cap hit.

## Criteria (initial; tune against `bench/`)

- **Size** = weighted `Construct` count of the (folded) body. A surviving loop
  *disqualifies* inlining (the loop dominates; call-overhead saving is noise).
- `INLINE_THRESHOLD`: small, ~<= 10-16 weighted nodes (≈ "single statement").
- `SPECIALIZE_THRESHOLD` (clone): significant fold (e.g. `S <= 50%` of original
  or `>= K` nodes removed), preferably with >= 2 call sites sharing the tuple.

## Testing strategy

- **`-ni` toggle** (mirroring `-nc` for const-eval) to disable inlining. Make it
  a precondition, not an afterthought.
- **Identical-backtrace invariant**: a test that runs a script erroring inside
  an inlined call and asserts the backtrace is identical with `-ni` on vs off.
  The `backtrace:` `extra_checks` in `tests.cpp` give the harness.
- **Behavior equivalence**: existing `-rt` suite must stay green with inlining
  on (the default in a debug build).
- **Fold wins**: `-s` assertions that `f(2*3)` collapses to a literal, that a
  tiny body is spliced, that a const-arg specialization folds.
- **Soundness**: side-effecting multi-use arg evaluated exactly once; float /
  string operands NOT algebraically simplified.

## Milestones (ordered)

1. **Backtrace `InlineCtx` foundation** (de-risks the #1 worry first):
   the `InlineCtx` type, the node field, the flush helper, the `Construct::eval`
   hook, a synthetic test. No inliner yet, so all tests stay green. *(done)*
2. **AST deep-clone** (`Construct::clone()` across the hierarchy) + test.
   *(done)* — pure-virtual `clone()` on every concrete node (faithful copy:
   children, locs, `is_const`, `inline_ctx`, resolved/slot state), via the
   `clone_as`/`copy_base_fields`/`clone_ops_into`/`clone_elems_into` helpers; a
   round-trip test asserts serialize-equality + correct independent eval.
3. **Size-only inliner** for expression-bodied direct calls + the `do_func_call`
   flush hook + `-ni` toggle + the identical-backtrace test. *(done)* — the
   `Inliner` in resolver.cpp runs after `AutoConst`, splicing eligible calls
   (top-level, expression-bodied, non-capturing, non-recursive, no nested
   function, arity match, body <= 24 nodes, sound arg use). Args inherit the
   parameter occurrence's loc and the whole splice is tagged with `InlineCtx`;
   chain rebasing handles a body itself inlined-into. `stamp_operand_loc` now
   also flushes inline frames (operator-ladder errors are stamped at the operand
   before reaching its `Construct::eval`). Verified: behavior identical with and
   without inlining across the suite, and the backtrace for a **body** error is
   byte-identical. **Known limitation:** an error while evaluating an *argument*
   (e.g. passing an undefined variable) is attributed to the inlined callee
   (`[f, main]` at the param position) rather than the call site (`[main]`) -
   inherent to expression-context direct substitution, where the arg node is
   both the call-site value and the in-body operand. Faithful separation would
   need temp-binding in a block (only possible in statement position). Re-fold
   and specialization come next.
4. **Cross-boundary re-fold** (run `AutoConst` over spliced regions) -> the
   `f(2*3) -> 7` wins.
5. **Const-arg specialization**: inline-if-tiny-after-fold; else clone + dedup +
   redirect.
6. **Re-resolution of spliced bodies** (slot remapping) for speed.
7. (deferred) type-narrowing pass -> algebraic simplification.

## Open questions

- Exact default thresholds (need `bench/` data). The body-size cap is tunable at
  runtime via `-it N` (default 24), which makes measuring easy.
- Whether `InlineCtx` lives as a node field or a side table (closure body-clone
  argues for a field).
- How `-s` should annotate inlined regions (cheap, useful — like the const-fold
  `-s` story).
