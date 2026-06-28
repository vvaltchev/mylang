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
4. **Cross-boundary re-fold** *(done)* — the inliner runs a focused bottom-up
   constant folder (`Inliner::refold`) on each spliced body, collapsing an all-
   const `MultiOpConstruct` (over scalar literals) to a literal against a const
   `EvalContext`. A const argument substituted into a NON-pure expression
   function now propagates and folds (`f(3)` with `f(x) => x*10+g` -> `30+g`) -
   the const-propagation half AutoConst's whole-call folding misses (a pure
   call already folded earlier, before inlining). A subexpression that would
   throw (e.g. `6/0`) is left for runtime, matching the un-inlined call.
   **Extended** to non-`MultiOp` ops: `refold` now also folds a subscript,
   slice, member access, or const-builtin call whose operands are self-contained
   constants (`is_const_literal`: a scalar, a `LiteralObj`, or an array/dict
   literal of constants) - e.g. a substituted array arg makes `a[0]` -> `10` and
   `len(a)` -> `3`. It only evaluates nodes with literal operands (never a
   slotted local), so it is also run on specialized clones safely (no frame).
   `sub_ok` correspondingly lets a const literal (not just a scalar) be
   duplicated/dropped. **Not folded:** subscript/slice of a const *global*
   (`tbl[0]`) - the global isn't a literal in the body and there's no const-
   global value context here (see Remaining).
5. **Const-arg specialization** *(done)* — a non-inlined call to a block-bodied,
   non-capturing function with scalar-const arg(s) (on never-reassigned,
   non-blocked params) clones it, binds those params and folds the body
   via `AutoConst::fold_specialized` (full folding + DCE; it *catches* const
   errors and discards, so a runtime error never becomes a compile error). If
   folding shrinks the body it registers a shared clone `$specN` (deduped by
   (func, const-arg tuple), inserted at the root block's front so it exists
   before any call) and redirects the call. The clone keeps the same
   signature/frame, so no re-resolution is needed (the const args are still
   passed but ignored). The clone's synthetic `id` resolves the redirected call;
   a `FuncDeclStmt::display_name` carries the original name so backtraces are
   identical with specialization on/off. Verified: all `bench/` scripts produce
   identical output and a div-by-zero inside a clone shows `f`, not `$spec0`.
   (Scalar consts only; array/dict const args and recursion-into-clones are not
   specialized.)
## Done since the milestones

- **Const-global subscript/slice/member/call folding** in spliced/specialized
  bodies. `refold` now seeds the const array/dict globals (from the top-level
  `const` decls) into `cctx` and folds a read whose operands reference only
  literals + const globals/builtins (no slotted local - `has_slotted_local`
  guards against a missing-frame deref). It skips lvalue positions (an
  assignment target, an lvalue builtin's first arg) so a write target is never
  turned into a value. `tbl[0]` (const global) now folds to its element.
- **Inlined errors with a pre-set loc keep their virtual frames.** Was: a
  non-`do_func_call` error thrown *with* a loc inside an inlined body (a builtin
  like `append(tbl, 9)`, a not-an-lvalue assignment `d.k = v`, ...) dropped the
  inlined frames, because the flush was gated by `Construct::eval`'s loc
  once-guard and that guard was already satisfied. Fix: the flush is now keyed
  off a dedicated `Exception::inline_origin_emitted` flag instead of the loc
  guard, so the innermost inlined node always emits its frames once;
  `do_func_call` sets the same flag after its call-site flush so the enclosing
  `CallExpr` doesn't re-emit, while each physical call's flush stays
  unconditional (multi-level inlined call sites all appear). Verified
  byte-identical to `-ni` for builtin errors, member-assign errors, multi-level
  inlining, and physical-call-from-inlined chains, across every `bench/` script.
- **Array/dict const args in specialization.** A block-bodied function called
  with a deep read-only (const) array/dict arg is now specialized: its element/
  member reads fold in the clone. Sound because a read-only value is only ever
  substituted in read positions (`fold_reads` never rewrites an lvalue or an
  lvalue-builtin first arg) and any mutation throws the same error at runtime as
  the un-specialized call. Mechanism: `try_specialize` seeds an arg whose value
  `is_readonly_value`; `prescan_blocked` gained a `block_subscript_bases` flag
  so the relaxed (array) seed set keeps every block except subscript/member READ
  bases (the param decl is kept, so an lvalue base can't dangle); `fold_reads`
  bakes a seeded array/dict const into a read-only `LiteralObj` so `refold`
  folds its reads; the shrink check uses `count_all_nodes` (a full pass, so a
  fold inside a kept `var t = a[0]+a[1]` rvalue counts); the cache keys array/
  dict args by `intptr` identity (`value_repr`). Also fixed a pre-existing bug
  this exposed: a promoted/folded `var` used only in a `return` expression had
  its decl dropped but the use left dangling (undefined variable) — `fold_child`
  now folds a `ReturnStmt`'s expression. Verified byte-identical to `-ni` for
  mutation-of-const-arg, subscript-assign, `intptr`, foreach over a const arg,
  member access, mutable (non-const) array args, and reassigned params, plus
  every `bench/` script.
- **Rebasing fixpoint for deeper inline nesting.** The inliner now re-scans each
  splice (`walk(slot, depth+1)` after the splice in `try_inline`), so a
  g-into-f-into-h chain collapses in one pass even when declaration order
  defeats the bottom-up walk (h declared before the callees it transitively
  reaches) or a call is newly exposed by the re-fold. Bounds: `MAX_INLINE_DEPTH`
  (16) caps nesting so mutual recursion terminates; `inline_budget`
  (`max(4096, 8 × program nodes)`, spent per inlined body) caps total growth so
  breadth-doubling can't blow up the tree — hitting either just leaves the
  remaining calls in place (still correct). Backtraces stay exact at any depth:
  a re-scanned splice's call site already carries an `InlineCtx`, so the new
  frame's parent is that chain (not null) and `rebase` re-roots the body's own
  chains under it. Verified: a forward-decl chain collapses to zero surviving
  calls (single-level left one), a 5-level chain and a const chain fold through,
  mutual-recursion / breadth-doubling terminate, an inlined+physical+recursive
  chain backtraces identically to `-ni`, and every `bench/` script matches.
- **Re-resolution of spliced block bodies** (slot remapping). A tail call to a
  block-bodied function (`return f(args);` where f's body always returns) is now
  inlined directly by `try_inline_tail`: f's body block replaces the return
  statement (sound because f's returns become the caller's returns and f never
  falls through). A single `splice_tail` pass decides each identifier by its
  ORIGINAL slot - `< nparams` is a param (substitute the value-stable arg, per
  `tail_arg_ok`: a caller local or const literal, never a global/side-effecting
  expr), `>= nparams` is a local (remap by `caller_fsize - nparams` into a fresh
  range at the top of the caller's frame). The caller's frame size
  (`FuncDeclStmt::frame_size`, or the root block's `slot_count` for main,
  threaded through `walk` as `fsize`) grows by f's local count, capped at 64
  (`Frame::live` is one 64-bit word); over that, the call is left as-is. Self-
  recursion is excluded; the spliced body carries an `InlineCtx` so a runtime
  error shows f's virtual frame. Verified byte-identical to `-ni` for the sharp
  slot-aliasing case (a body local that would clobber a caller slot read by a
  later arg), a param shadowed by a same-name local (slot-based substitution),
  loops/foreach/control-flow with locals, re-entry, caller recursion, nested
  tail calls, `caller_fsize < nparams`, array mutation through a param, and
  every `bench/` script. Not yet inlined directly (left to specialization or a
  call): non-tail calls, reassigned params, and global/side-effecting args -
  all would need an args-as-locals form that binds args once up front.
- **Specialization keeps the original frame** (no re-resolution), so it is
  unaffected by the above and remains the fallback for non-tail const-arg calls.

## Remaining / tracked tasks

Not yet done; roughly in priority order:

1. **Block-bodied inlining at non-tail positions + bounded recursive unrolling**
   (the active task).

   **v1 DONE (`try_inline_block` → `InlinedCallExpr`).** A block-bodied function
   is now inlined in ANY expression position by replacing the `CallExpr` with an
   `InlinedCallExpr` whose `do_eval` runs the cloned, param-substituted body
   behind its own `FlowState` boundary (a stack FlowState swapped onto `ctx->flow`
   and restored — so a `return` yields the call's value, not the caller's). NO
   statement hoisting and no eval-order change (the node sits where the call was),
   NO child EvalContext (the no-locals body is `scope_free`, runs in place). The
   approach below (statement-level splice with a result temp) turned out
   unnecessary for the common case — the FlowState-boundary expression is
   simpler and cheaper. The size gate is the measured **cost model**
   (`body_weight` < `CALL_WEIGHT` = 21, weights from `--weights`), non-capturing,
   no nested func, non-recursive.

   **v2 DONE — bodies WITH locals.** `splice_tail` substitutes the params (by
   slot) and **remaps the locals** into a fresh range at the top of the caller's
   frame (grown by the local count, capped at 64) — the same machinery
   tail-inline uses; two inlines in one expression get distinct ranges. Args use
   **`tail_arg_ok`** (a caller local or const literal, never a global — a block
   body can change shared state between a param's uses; `sub_ok` was a latent v1
   unsoundness that allowed a global arg). Two bugs the broadened eligibility
   exposed and fixed: (1) `contains_func` was incomplete (`for_each_child` skips
   an `Expr14` rvalue), so a closure decl `var h = func[v]..` wasn't seen and the
   capture broke — made it a COMPLETE walk; (2) block-inlining a pure call in a
   `for`-condition ran before the for-range specializer and turned a once-cached
   `ForRangeStmt` bound into a per-iteration `ForStmt` — `walk` now SUPPRESSES
   block-inline inside loop conds (`no_block`), leaving the call for for-range
   (expression-inline there is fine — it yields a recognizable arithmetic bound).
   Result: ~1.4x on a call-heavy non-const loop; 1290/1290 across the matrix.

   **v3 DONE** (recursion unroll + per-frame pure-call cache; ~2.1x on naive
   fib). Sound: reusing a pure result is lazy reuse of an already-evaluated
   value, NOT the unsound hoist-out-of-a-guard (see [[pure-call-cse-soundness]]).
   - **A — args-as-locals (cc3b0f8).** A non-value-stable arg (non-trivial expr,
     global, or side-effecting call) is bound to a fresh frame TEMP once and the
     param reads it; value-stable args substitute directly. Captures the
     call-time value (fixes the old `sub_ok` global-arg unsoundness) and is what
     a self-call's `n-1` arg needs.
   - **B — recursion unroll.** A pure tree-recursive func (≥2 self-calls,
     `func_is_cacheable_recursive`) is admitted to `block_funcs` and unrolled in
     place: the walk inlines its own self-calls, growing the decl body to
     `REC_NODE_CAP`. Each self-call splices a clone of the SAVED ORIGINAL body
     (`rec_orig`), not the growing body (no compounding); the bound is by SIZE
     (robust to template-instance name redirects). Instances are at the root
     front, so the decl is unrolled before external call sites are walked — those
     see the body ≥ cap and CALL the unrolled func (no call-site re-unroll).
   - **C — per-frame pure-call cache.** The InlinedCallExpr shares the caller
     frame, so the unroll's duplicate self-calls land in one frame; the inliner
     sets `cache_results`, devirt makes such a call a **`CachedCallExpr`** (a
     `DirectCallExpr` subclass — separate node so the plain path pays no per-call
     check), and `cached_call` checks the caller `Frame`'s lazy `PureCache`
     ({func, args}→result). Lazy → sound (a base-case-misses-negatives recursion
     like `fact(-1)` can't diverge); frame-scoped → not global memoization; only
     SCALAR results cached (a container result would alias).
   The VARIABLE-arg COMPILE-TIME CSE (hoisting `fib(n-5)` to a temp) is NOT sound
   (the runtime lazy cache is the sound form). Effect is a base reduction of the
   exponential (~1.6→~1.45), NOT linear (that's global memoization = the script's
   job). Remaining: the deferred type-narrowing/algebraic pass.

   The original gap: a block-bodied function used **in expression position**
   (`var y = f(z) + 1;`) and **recursive** functions (banned by
   `count_uses(body, self) == 0` and the tail self-recursion exclusion).

   **Mechanism — statement-level splice (the "args-as-locals" form the tail-
   inliner's note pointed to).** A `{ }` body can't drop into an expression, so
   at `var y = f(z) + 1;`:
   - clone f's body; bind each propagatable param (a const literal, or a
     single-use / side-effect-free arg, substituted directly; otherwise
     **temp-bind** the arg once up front — `var __p = <arg>;` — to preserve
     single evaluation);
   - rewrite the body so its result reaches the call site: a body that always
     returns can deliver via the existing block-`ret` `FlowState`; an early
     return in non-tail position uses a **result temp** (`__ret = e;` + the body
     wrapped so control reaches the end) ;
   - **hoist** the (substituted, return-rewritten) body as statements *before*
     the enclosing statement, and replace `f(z)` with `__ret`.
   Locals + the temps are remapped into the caller's frame exactly as
   `splice_tail` already does (frame growth capped at 64 slots); the splice
   carries an `InlineCtx` for an identical backtrace, like every existing splice.

   **Recursion is bounded unrolling of the DEFINITION, not the top call.** The
   inliner already walks each function's own body, so lifting the recursion ban
   lets it inline a function's `self(...)` calls *into its own body*; the
   existing re-scan fixpoint (`walk(slot, depth+1)`) unrolls one level at a time,
   bounded by `MAX_INLINE_DEPTH` (depth) and `inline_budget` (breadth/total
   nodes). The frontier calls past the budget stay real (`fib(n-k)` calls left as
   calls — the A->B->C->stop-at-D chain, self-referential). Because the
   *definition* is unrolled, **every** call advances k levels of the recursion
   per invocation, so the call count drops by a factor that grows with k while
   the arithmetic is unchanged — a pure call-overhead reduction (the canonical
   win, e.g. `fib`). With a **const arg + a pure** function the unrolled frontier
   folds to literals, so the recursion bottoms out at compile time
   (`fib(5) -> 5`). Termination is the existing depth cap + budget.

   **CSE of duplicate pure calls in the unrolled body (key to making it pay).**
   Unrolling `fib` k levels duplicates subtrees — `fib(n-3)` appears twice at
   depth 2, and the duplication compounds. If `fib` is **pure**, each distinct
   `fib(arg)` is computed once and reused (`fib(n-3) + fib(n-3)` -> `2 * t`,
   `t = fib(n-3)`), which collapses the exponential unroll into something
   sharable and lets us unroll deeper within budget. This needs `fib` recognized
   pure. **DONE (prerequisite):** a self-recursive function is now auto-pure when
   its body is otherwise pure (`process_function`'s optimistic self-add), and
   such a function is excluded from AutoConst's eager const-fold
   (`func_is_self_recursive`) so a const-arg recursion never runs at compile time
   (`fib(40)` won't hang) — it folds only via this bounded unroll. **Remaining:**
   the CSE pass over the spliced/unrolled body (dedupe identical pure-call
   subexpressions into a temp), reusing the `cse_key` canonicalization from the
   parse-time CSE work.

   **The benefit function (size-tiered "decide"), keyed on original body size O
   (weighted; a surviving loop weighs heavily and *disqualifies* a call-overhead-
   only inline — the loop dominates):**
   - **Tier 1 — small (body weight `< CALL_WEIGHT`):** inline **unconditionally**,
     no fold required, subject only to `inline_budget` + `MAX_INLINE_DEPTH`. The
     body's raw node count may **grow** (recursion unroll, multi-use args) —
     accepted, the benefit is the dropped call count, not a smaller tree. (fib.)

     **Cost model (measured, not guessed) — a per-node-type weight table.** A
     flat node count is wrong: fib's body is dominated by its two *calls*, which
     are ~20x an arith op. So weigh each node by its measured eval cost. The
     **`--weights`** mode (`run_weight_bench`, eval.cpp) builds the AST nodes by
     hand in C++ (never parsed, so no fold/inline/specialize can perturb them or
     the loop count) and times each in a tight C++ loop, isolating per-node
     marginal cost by subtracting child-subtree costs. **Re-runnable** as the
     interpreter changes — and reusable for the bytecode VM (the weights change,
     the benefit function does not). Measured on the `OPT=1 ASSERTS=0` build
     (ns/eval relative to a slot read = 1):

     | node | xId | node | xId |
     |---|---|---|---|
     | id (slot read) | 1 | return | 3 |
     | literal | 1 | if | 7 |
     | arith op (+) | 1 | assignment | 11 |
     | compare (<) | 1 | **CALL (2-param)** | **~21** |

     (assign/if are heavy because a *statement* pays the `Construct::eval`
     wrapper; a CALL is ~21x — the reference.) **Benefit function:** sum the
     body's per-node weights; **inline when the sum `< CALL_WEIGHT` (~21).** Then
     a body of a couple of cheap statements always inlines, but a body
     containing a call (≥21 by itself) does not — except via Tier-2 fold or the
     recursion path. Hard-code these weights into a small table in the inliner
     (id=1, lit=1, add=1, cmp=1, return=3, if=7, assign=11, call=21);
     re-derive with `--weights` when the interpreter changes. Calibrated to 2
     params (more params raise the call cost, so the threshold is conservative).
   - **Tier 2 — medium (`INLINE_SMALL < O <= ATTEMPT_MAX`, ~150-200):**
     **speculate** — clone, bind propagatable const/auto-const args, fold + DCE,
     measure folded size `S` and benefit `B = O - S`. Inline if `S <=
     INLINE_SMALL`; else emit/reuse a shared clone `f$specN` if `B` is large
     (substantial fold, e.g. `S <= O/2` or `B >= K`); else **discard** the
     speculative clone and keep the ordinary call (the discard is free — the
     existing `fold_specialized` already catches+drops const errors, so a
     speculative attempt has no side effects).
   - **Tier 3 — huge (`O > ATTEMPT_MAX`):** **don't even attempt** — the
     clone+fold compile cost isn't worth it and the call overhead is negligible
     vs the body. Keep the call.

   **Order.** (a) statement-level block-body splice for **non-recursive** funcs
   (the mechanism) under the size-tiered gate + the loop-disqualifier; verify
   backtraces identical to `-ni` and the `-rt` suite green. (b) Lift the
   recursion ban so the fixpoint unrolls recursive funcs under depth/budget; tune
   `INLINE_SMALL` / `ATTEMPT_MAX` / depth / budget against `bench/`. **Measure
   the proper way** (per `bench/README.md`): `ASSERTS=0` release builds compared
   with `run.py --baseline`, and confirm wins vs noise with **callgrind
   instruction counts** (layout-independent) — e.g. fib's call-count drop should
   show as fewer `do_func_call`/`EvalContext` instructions, not just a wall-clock
   wobble.

2. **(deferred) Type-narrowing -> algebraic simplification** (`x+1-1 -> x`).
   Unsound on unknown dynamic types (float non-associativity, `+` overloading,
   preserved type errors); needs a "provably-int" analysis first. Inlining
   already leaves spliced expressions with original locs + full structure so
   such a pass can run over them later.

## Open questions

- The Tier-1 threshold is `CALL_WEIGHT` from the measured weight table
  (`--weights`, ~21 xId), re-derivable as the interpreter changes. Still need
  `bench/` data for: `ATTEMPT_MAX` (Tier-3 don't-attempt cap), the recursion
  depth cap, and `inline_budget`. The existing body-size cap is tunable via
  `-it N` (default 24); add knobs for the new tiers so they're easy to sweep.
  Tune with `ASSERTS=0` release builds + `run.py --baseline` + callgrind (see
  `bench/README.md`).
- Whether `InlineCtx` lives as a node field or a side table (closure body-clone
  argues for a field).
- How `-s` should annotate inlined regions (cheap, useful — like the const-fold
  `-s` story).
