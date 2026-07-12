/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "syntax.h"

#include <vector>

/*
 * The runtime bytecode for the -vm execution engine (see plans/bytecode-vm.md).
 * The VM consumes the ALREADY-OPTIMIZED AST (post infer / resolve_names /
 * specialize_types) and lowers it to a flat instruction list, removing the
 * tree-walker's per-node virtual-dispatch tax. It is built strictly
 * incrementally behind an AST-fallback opcode: EvalStmt (and, later, EvalExpr)
 * just call the node's existing eval(), so the VM runs the WHOLE language from
 * day one, and native opcodes replace the fallbacks one tested step at a time.
 *
 * Phase 0: EvalStmt + Halt. Phase 1 adds native control flow (Jump /
 * JumpIfFalse / LoopBackEdge). Phase 2 adds a REGISTER machine over the frame
 * slots (the VM's registers ARE the resolved-local slots, so there is no value
 * stack) with native int ops (IntBin / JumpUnlessIntCmp) and fused
 * superinstructions, so a resolved-local int scalar loop runs with no
 * tree-walker fallback. See plans/bytecode-vm.md.
 *
 * (Named OpCode, not Op: `Op` is already the operator enum in operators.h.)
 */
/* P8 Inc 2b: the pending action a `finally` block must resume after it runs (a
 * SetPend value; EndFinally consumes it). `normal` falls through; `reraise`
 * re-raises the in-flight vm_exc; `ret`/`brk`/`cont` (Inc 2c) resume a flow
 * signal that crossed the try. */
/* The pending action the SHARED finally block resumes. A return/break/continue
 * crossing a try INLINES its own finally copy (Inc 2c), so only the try's
 * normal and reraise (exception) exits reach the shared finally - just these
 * two. */
enum class Pend : unsigned char { normal, reraise };

enum class OpCode : unsigned char {

    /*
     * AST fallback (the incremental-safety pillar): evaluate `node` with the
     * tree-walker. EvalStmt runs a statement - its value is discarded, and an
     * UndefinedId result throws UndefinedVariableEx as Block::do_eval. It does
     * NOT act on FlowState: an in-flight break/continue set by a loop body is
     * consumed by the following LoopBackEdge, not here.
     */
    EvalStmt,

    /* Unconditional jump: pc = `target`. */
    Jump,

    /*
     * Evaluate `node` as a condition (fallback: the same typed-or-boxed path as
     * the tree-walker's eval_cond). If FALSE, pc = `target`; else fall through.
     */
    JumpIfFalse,

    /*
     * Placed right after a loop body; reads ctx->flow and branches, mirroring
     * While/ForStmt::do_eval exactly. `target` = the continue destination (a
     * while's re-test, a for's increment), `target2` = the loop exit:
     *   ret  -> pc = target2 (leave flow set; it propagates out of the loop)
     *   brk  -> flow = none, pc = target2
     *   cont -> flow = none, pc = target
     *   none -> pc = target
     */
    LoopBackEdge,

    /*
     * Native 3-address int op (register machine): slot[target] = a <aop> b,
     * where a/b are Operands (a frame slot or an int immediate) and aop is an
     * arithmetic/bitwise Op. `node` carries the source statement's Loc for a
     * div/mod-by-zero error. No value stack - operands and result are frame
     * slots. This is what `s += i` / `i++` compile to.
     */
    IntBin,

    /*
     * A DYN/general inc-dec STATEMENT `--d`/`d++` on a scalar slot: `target` =
     * the slot, `target2` = the slot KIND (0 local / 2 capture), `a.lit` = 1 for
     * `++` / 0 for `--`. Reads the value, THROWS TypeErrorEx (loc side table) if
     * it isn't int/float — inc-dec is int/float-ONLY, unlike a compound `+= 1`
     * (which would concat a string) — then applies ±1 and writes back. The
     * value is discarded (statement). A proven int/float local is handled by
     * IntBin/FloatBin; a struct/dict MEMBER dyn inc-dec falls back.
     */
    IncDecCheckedV,

    /*
     * CHECKED subscript inc-dec `c[k]++` / `c[k]--` for a DYN/unproven base
     * (a proven flat-int array / dict goes through StoreElemInt / DictStore).
     * target = the base slot kind (0 local / 1 global / 2 capture), target2 =
     * the base slot, a = the key temp, aop = plus/minus. Mirrors
     * IncDecExpr::do_eval's dyn path: forms the element LValue via the runtime
     * Type::subscript(for_write=false) (a general array / dict has a boxed
     * element; a flat scalar element has none -> NotLValueEx, exactly as the
     * tree-walker), THROWS if it isn't int/float, then applies ±1 (int/float-
     * ONLY, so byte-identical to the dyn scalar IncDecCheckedV). The value is
     * discarded (statement). Carets via the loc side table.
     */
    IncDecElemCheckedV,

    /*
     * CHECKED MEMBER inc-dec `d.f++` / `d.f--` for a DYN/unproven base (`d.f`
     * where `d` is dyn/general holding a struct or dict). target = the base slot
     * kind (0 local / 1 global / 2 capture), target2 = the base slot, aop =
     * plus/minus. Mirrors IncDecExpr::do_eval's dyn path over MemberExpr's
     * lvalue logic: a mutable boxed STRUCT field or a DICT value is an lvalue
     * (read, check int/float, ±1); a POD field / readonly / a missing dict key
     * throws (NotLValueEx / KeyNotFoundEx), exactly as the tree-walker. KEEPS
     * its node (the IncDecExpr) for its TWO error carets - the MEMBER loc for a
     * KeyNotFound vs the INC-DEC loc for its own NotLValue/TypeError.
     */
    IncDecMemberCheckedV,

    /*
     * GENERAL nested lvalue-chain store `base.step1.step2... = v` / `OP= v`
     * mixing MEMBER and SUBSCRIPT steps (`a[i].f=v`, `q.p.x=v`, `d.a[0].f=v`,
     * `s.f[i]=v`). A pure-subscript chain keeps StoreElem2V/StoreElemChainV; a
     * single `s.f`/`a[i]` keeps StoreMemberV/StoreElemValue - this is the
     * ≥2-step chain with ≥1 MEMBER step. target = value temp, target2 = base
     * slot, a.lit = base kind (0 loc/1 gbl/2 cap), a.slot = chain_steps pool
     * index, aop = the Expr14 op. Walks each intermediate step as an lvalue REF
     * (for_write=false), then stores the final step (member: vm_member_store;
     * subscript: vm_subscript_store) - byte-identical to the tree-walker's
     * chained lvalue eval. All carets use the outer lvalue loc (the loc side
     * table), matching StoreElemChainV.
     */
    StoreLValueChainV,

    /*
     * Fused compare-and-branch: if NOT (a <aop> b) then pc = target, else fall
     * through. a/b are int Operands, aop a comparison Op. One dispatch replaces
     * the tree-walker's eval_cond -> TypedScalarExpr -> Identifier chain (this
     * is what a `while (i < N)` header compiles to: jump out unless i < N).
     */
    JumpUnlessIntCmp,

    /* The FLOAT analogues of IntBin / JumpUnlessIntCmp: operands are read as
     * float (an int/bool slot promotes), the arithmetic is `double`, the result
     * is written to a float slot. div/mod (fmod) keep the zero check. For a
     * pure-float scalar loop. */
    FloatBin,
    JumpUnlessFloatCmp,

    /*
     * Fused counted-loop back-edge (the register-machine superinstruction for a
     * `for`): in ONE dispatch, i += step (or -= for a descending loop), then if
     * (i <aop> bound) pc = target (loop back to the body), else fall through
     * (exit) - matching the tree-walker's raw-C ForRangeStmt counter so a `for`
     * doesn't regress. Fields: `target2` = i's slot, `b` = step, `a` = bound,
     * `aop` = the comparison Op (lt/le ascend +step, ge/gt descend -step),
     * `target` = the loop body. int-only (ForRangeStmt is always int).
     */
    ForLoopStep,

    /*
     * Native array-element read `a[i]` into a temp (Phase 5): `target2` = the
     * array's frame slot, `a` = the int index operand, `target` = the dst
     * temp slot; `node` is the Subscript (for the OutOfBoundsEx loc, and the
     * fallback below). When the slot holds a flat array it reads the element
     * directly - no Identifier/Subscript dispatch, mirroring
     * Subscript::eval_int/eval_float (negative index wraps, bounds throw). When
     * it's NOT an array (a dict / a general array), it falls back to
     * `node->eval_int` / `eval_float`. Int / float element variants.
     */
    LoadElemInt,
    LoadElemFloat,

    /*
     * Native flat-`array<bool>` element read `a[i]` into the loop var as a real
     * BOOL (the bool-foreach analogue of LoadElemInt/Float): `target2` = the
     * array slot, `a` = the int index/counter, `target` = the loop var slot.
     * Reads `flat_bools()[offset + i]` and binds `EvalValue(bool)` - so the loop
     * var is a bool, matching the tree-walker (`print(x)` -> true/false, not
     * 1/0). `i` is loop-bounded (< ArrLen), so no bounds check; the base is a
     * proven flat bool array (elem_is_bool), so no type dispatch.
     */
    LoadElemBool,

    /*
     * Array length into an int slot: slot[target] = size(slot[target2]). The
     * native-foreach loop bound over a flat array, evaluated once. `target2`
     * holds a flat array (guaranteed by ForeachStmt::elem_th), so it reads
     * SharedArrayObj::size() directly.
     */
    ArrLen,

    /*
     * String CHAR count into an int slot: slot[target] = length(slot[target2]).
     * The native `foreach (c in s)` loop bound (ForeachStmt::container_is_str),
     * evaluated once. `target2` holds a SharedStr, so it reads its view size.
     */
    StrLen,

    /*
     * Native string-CHAR read `s[i]` into the loop var: slot[target] = a 1-char
     * SharedStr of `slot[target2]`'s i-th char (`a` = the int index). The string
     * foreach analogue of LoadElemValue - a fresh 1-char string, byte-identical
     * to the tree-walker's `SharedStr(string(&view[i], 1))`. `i` is loop-bounded
     * (`< StrLen`), so no bounds check.
     */
    LoadStrChar,

    /*
     * Native array-VALUE-element read `a[i]` into a temp slot (Phase 5, for a
     * nested subscript `a[i][j]`): `target2` = the (general) array slot, `a` =
     * the int index, `target` = the dst temp; `node` = the Subscript. Reads the
     * i-th element VALUE (itself an array, since `a` is array<array<..>> - a
     * GENERAL array) straight from the element vector into the temp, so the
     * outer `a[i]` of a 2-D read is native; the temp is then the array slot of
     * an inner LoadElem/etc. A non-array / non-general base falls back to
     * `node->eval`. READ path only - a 2-D *write* through a temp would COW the
     * temp and not write back, so those stay fallback.
     */
    LoadElemValue,

    /*
     * Native struct-FIELD read from a flat array<PodStruct> element `pts[i].f`
     * into a scalar slot (the struct-foreach direct read): `target` = the dst,
     * `target2` = the struct-array slot, `a` = the int index/counter, `b` =
     * the field INDEX (into the def). Reads the scalar straight from the array
     * bytes at (arr.offset() + i)*stride + field.offset - NO StructObject,
     * no memcpy (what beats the tree-walker's reused-object foreach). The
     * codegen proves the base is a flat struct array + the field a non-null
     * scalar, so the read can't fault (index in range by the counted loop);
     * node-free. Int / float variants. */
    LoadStructFieldInt,
    LoadStructFieldFloat,

    /*
     * Native WHOLE-element bind from a flat array<PodStruct> `foreach (var p in
     * a)` whose body uses `p` as a value (not only scalar-field reads): `target`
     * = the loop var slot, `target2` = the struct-array slot, `a` = the int
     * index/counter. Materializes a FRESH StructObject from element i's bytes
     * (make_intrusive + memcpy) into the loop var - byte-identical to the
     * tree-walker's reused-object bind (its COW guard only ever avoids
     * overwriting a captured/stored element, which a fresh alloc trivially
     * satisfies; the values match). `i` is loop-bounded, so no bounds check;
     * node-free.
     */
    LoadStructElemV,

    /*
     * Native dict `foreach` via a LIVE unordered_map iterator (a dict has no
     * O(1) index, so it can't use the counted-loop machine). The per-loop
     * iterator STATE lives in a `vm_run_chunk`-local vector sized by
     * `Chunk::n_dict_iters`, indexed by a codegen-assigned `iter_id`.
     *
     * DictIterInit  target=iter_id, target2=dict_slot: pin the dict
     *               (intrusive_ptr copy -> alive for loop) + set it=begin().
     * DictIterNext  target=end_pc, target2=iter_id, a.slot=k, b.slot=v
     *               (-1 == `_`/unused): if it==end -> pc=end_pc; else bind
     *               k=it->first, v=it->second.get() (box-free, like
     *               LoadElemValue), ++it, fall through to the body. Advance is
     *               BEFORE the body - the visited sequence is identical to the
     *               tree-walker's range-for; the only difference (++it timing)
     *               is observable only under mutation-during-iteration, UB in
     *               both engines. Neither op throws (node stays null).
     */
    DictIterInit,
    DictIterNext,

    /*
     * Native SINGLE-var `foreach (e in <dyn container>)`: the container's
     * static type is dyn, so the array-vs-dict choice is at RUNTIME. A live
     * iterator like DictIter, over a `Chunk::n_dyn_iters` state pool indexed by
     * `target` (a codegen-assigned iter_id).
     *
     * ForeachDynInit: `target` = iter_id; `target2` = the container slot. Pins
     * the container; if an array -> {arr, idx=0, size}; if a dict ->
     * {dict, it=begin}; else throws TypeErrorEx via the loc side table (the
     * container caret; `node` recorded by extract_locs, then nulled).
     */
    ForeachDynInit,

    /*
     * ForeachDynNext: `target2` = iter_id; `a` = the loop-var slot (`_` = -1);
     * `target` = end_pc. On exhaustion jumps to end_pc; else binds the loop var
     * BOX-FREE - the array ELEMENT (arr_elem_at) or the dict KEY (it->first) -
     * then advances. Never throws (node-free).
     */
    ForeachDynNext,

    /*
     * Native STRICT foreach-unpack element: `foreach (x, y in pairs)` over a
     * proven array<array<int/float>>. The counted loop over the OUTER array is
     * the usual ArrLen/ForLoopStep; per iteration this op reads pairs[i] (a
     * general element = a flat sub-array), checks it is an array of EXACTLY N,
     * and writes its N scalars BOX-FREE into the consecutive loop-var slots.
     *   target2 = outer general-array slot, a = the index operand (loop
     *   counter, in-range), target = the first loop-var slot (x; y=tgt+1,..),
     *   b = N (loop-var count), node = the container (loc for the two strict
     *   throws - a non-array element / a length mismatch - matching do_iter,
     *   nulled by extract_locs). Int / float variants (the sub-array kind).
     */
    UnpackElemInt,
    UnpackElemFloat,

    /*
     * The GENERAL-value analogue of UnpackElemInt/Float (F-2b): the sub-array is
     * NOT flat int/float (a general / dyn / str / mixed element, e.g. shopping's
     * [str, float]), so each element binds its BOXED value (vm_arr_elem) into
     * the target slot instead of a raw scalar. Also serves the INDEXED unpack
     * `foreach (i, name, price in indexed products)` (the index var is the loop
     * counter; the unpack targets start after it). Same operands as UnpackElem*:
     *   target2 = outer array slot, a = index operand, target = first unpack
     *   slot, b = the unpack width, node = the container (strict-throw loc).
     */
    UnpackElemValue,

    /*
     * The `_`-aware / non-consecutive analogue of UnpackElemValue: instead of a
     * consecutive `target..+width` run, `target` indexes a Chunk::unpack_targets
     * per-position slot list (-1 == `_`, skipped). Handles `foreach (var a, _, b
     * in pairs)` (a `_` gets NO slot, so a/b aren't consecutive-by-position).
     * `target2` = outer array slot, `a` = index, `b` = the position count (the
     * strict length). Each element binds box-free via vm_arr_elem (flat or
     * general). node = the container (strict-throw loc).
     */
    UnpackElemTargets,

    /*
     * Native array-element store `a[i] = v` / `a[i] OP= v` (Phase 5): `target2`
     * = the array slot, `a` = the int index operand, `b` = the value operand,
     * `aop` = the store op (`Op::invalid` = plain assign, else a compound arith
     * op: a[i] = a[i] <aop> v). For a FLAT mutable int/float array it
     * stores/updates the scalar directly, mirroring try_flat_subscript_store
     * (bounds, negative wrap, div/mod-by-zero checked BEFORE any clone like the
     * tree-walker, COW: a slice clones, an aliased non-slice clones its live
     * slices; then invalidate the hash). A flat BOOL array is handled too (P1),
     * PLAIN-assign only (`aop == invalid`) - bool has no compound - writing the
     * value operand's 0/1 to bvec. For a const/read-only / general /
     * float-in-StoreElemInt / dyn-laundered base, or a compound on a bool array,
     * it BOXES the already-computed index/value operands and dispatches through
     * the UNIVERSAL vm_subscript_store (the same shared store StoreElemValue /
     * DictStore use, mapping the base op back to its Expr14 op) - AST-FREE and
     * byte-identical to the tree-walker's flat->general path. So NO node: the
     * OOB/div0 caret comes from the loc side table (the Expr14 loc - the same
     * single loc the pre-AST-free op recorded; OOB's narrower subscript caret in
     * the tree-walker is a pre-existing single-loc limitation). Value ops are
     * emitted before the index ops so a both-throw case matches the tree-walker
     * (rhs before index). Int / float value variants (the int variant carries
     * bool arrays too; LoadElemInt reads them).
     */
    StoreElemInt,
    StoreElemFloat,

    /*
     * Native DICT element store `d[k] = v` / `d[k] OP= v` (P2): `target2` = the
     * dict's (local) slot, `a` = the KEY operand (a boxed temp slot), `b` = the
     * VALUE operand (a boxed temp slot), `aop` = the Expr14 op (`Op::assign` /
     * `addeq` / ...), `node` = the Expr14 (loc/fallback). The value is compiled
     * before the key (tree-walker order: rhs, then the lvalue's base+index; the
     * base is a side-effect-free slot read done here). vm_subscript_store uses
     * shared Type::subscript(for_write) path (auto-vivify on a plain miss,
     * COW, key-freeze) + slot_rmw, matching the tree-walker exactly. If the
     * slot doesn't hold a dict at runtime (a dyn-laundered base) it falls back
     * to `node->eval`. Emitted only when the inferencer proved base_dict.
     */
    DictStore,

    /*
     * Native STRUCT field store `s.member = v` / `s.member OP= v` (residual).
     * `target2` = the struct base's (local) slot, `a` = the member-key pool
     * index (`a.lit`, giving memUid + carets), `b` = the VALUE (a boxed temp),
     * `aop` = the Expr14 op. vm_member_store does the store (a POD field: coerce
     * + byte pod_set; a boxed field: the field LValue + slot_rmw), matching the
     * tree-walker's try_pod_struct_store / boxed-field store. A dict member
     * store uses DictStore instead; emitted only for a proven struct base.
     * AST-free: no `node`, carets from the member-key pool.
     */
    StoreMemberV,

    /*
     * Native GENERAL array element store `a[i] = v` / `a[i] OP= v` (P4), where
     * the element is non-scalar (array/str/struct/dyn - the StoreElem* flat
     * fast paths don't apply). `target2` = the array's (local) slot, `a` = the
     * KEY (index) operand (a boxed temp), `b` = the VALUE (a boxed temp),
     * `aop` = the Expr14 op, `node` = the Expr14. Same value-then-index compile
     * order as StoreElem; vm_subscript_store does the store via the shared
     * Type::subscript(for_write) + slot_rmw (bounds check + COW as the
     * tree-walker). A non-array runtime base falls back to `node->eval`.
     */
    StoreElemValue,

    /*
     * Native multi-assign / IdList destructure `a, b, c = <rvalue>` (F-1): the
     * rvalue is compiled into `a.slot`; `target` indexes `Chunk::unpack_targets`
     * (the target frame slots, -1 for a `_` placeholder). vm_multi_unpack does
     * the tree-walker's STRICT rule: if the rvalue is an ARRAY, its length must
     * EXACTLY match the target count (else a "cannot unpack an array of length M
     * into N variables" TypeErrorEx, loc from the side table), and each element
     * (arr_elem_boxed - box-free for a flat scalar) is written to its target;
     * a NON-array rvalue SPREADS to every target. Handles the const-literal
     * (LiteralObj), for-init, and runtime-array-value forms uniformly - the
     * array-elide fast paths (try_multi_literal_store / _scalar_spread) still
     * take the non-const LITERAL / proven-scalar cases first. AST-free.
     */
    MultiUnpackV,

    /*
     * Native NESTED general store `a[i][j] = v` / `a[i][j] OP= v` (residual): a
     * Subscript lvalue whose base is another Subscript over a slotted base.
     * `target2` = the outer base's (local) slot, `a` = KEY1 (i) temp, `b` = KEY2
     * (j) temp, `target` = the VALUE temp, `aop` = the Expr14 op. vm_nested_
     * subscript_store reads `a[i]` as a reference then stores `[j]` into it
     * (two-level vm_subscript_store; COW writes back through the inner element).
     * AST-free: caret from the loc side table (node = the outer Subscript).
     */
    StoreElem2V,

    /*
     * GENERIC N-level (>= 3) nested store `a[k0][k1]...[kn] = v` / `OP= v` - the
     * arbitrary-depth generalization of StoreElem2V (which stays the tuned 2-
     * level fast path). `target2` = the base's (local) slot, `a.lit` = the base
     * SLOT KIND (0 loc / 1 gbl / 2 cap), `a.slot` = nkeys, `b.lit` = the KEYS run
     * base (nkeys temps, BASE-to-innermost), `target` = the VALUE temp, `aop` =
     * the op. vm_subscript_chain_store walks keys[0..n-2] as reads then stores
     * keys[n-1]. AST-free: caret from the loc side table (the outer Subscript).
     */
    StoreElemChainV,

    /*
     * Typed DICT scalar READ `d[k]` / `d.k` into a temp (P3), when inference
     * proved the value is a non-null int/float. `target2` = the dict's (local)
     * slot, `target` = the dst temp; `node` = the Subscript or MemberExpr - a
     * Subscript reads its key from `a` (a boxed temp), a MemberExpr from
     * its own `memId` (interned name), keyed on `node->is_subscript`.
     * On a PRESENT key it reads the stored scalar directly via the shared
     * dict_present_value (a map find - the SAME the tree-walker uses),
     * a bool value as 0/1. On a MISSING key / non-dict base it falls back to
     * `node->eval_int` / `eval_float` (the default-dict / KeyNotFoundEx path),
     * exactly like LoadElemInt's fallback. This removes the box+unbox of the
     * generic MemberV/SubscriptV so a `s += d.a + d.b` chain is fully typed.
     */
    DictLoadInt,
    DictLoadFloat,

    /*
     * Evaluate a scalar-returning CALL (a builtin - via the baked
     * DirectBuiltinCallExpr fn pointer - or a user function) into a temp slot
     * as an RValue (Phase 5): `target` = the dst temp, `node` = the CallExpr.
     * The result is then an ordinary int/float operand, so a loop body like
     * `s += sqrt(i)` / `s += f(i)` goes native instead of falling back whole.
     * The call still evaluates its own args (the builtin ABI takes the
     * unevaluated ExprList) - a fully Construct*-free builtin dispatch (args
     * pre-evaluated into slots) is later work.
     */
    EvalToSlot,

    /*
     * Native BUILTIN call with the VALUE ABI (no node->eval): the args are
     * already evaluated into the register run [a.lit, a.lit+b.lit); `node` =
     * the DirectBuiltinCallExpr (its baked `builtin.func_v` + its args ExprList
     * for error locs); `target` = the dst slot. Copies the arg values into a
     * buffer and calls func_v. Emitted only for a builtin that HAS func_v (a
     * migrated, read-only builtin); a mutating / AST / un-migrated one stays
     * EvalToSlot.
     */
    CallBuiltinV,

    /*
     * Native MUTATING-BUILTIN call (append/push/pop/insert/erase/intptr/sort/
     * reverse) with the lvalue ABI. AST-FREE: the Builtin (`func_lv`) + the
     * ArgLocs (carets + nargs) come from the `builtin_calls` pool (index in
     * `a.slot`; `a.lit` = arg0's slot KIND 0/1/2). `target` = the dst; `target2`
     * = arg0's slot; `b` (when is_lit) = a rest-run base of pre-evaluated value
     * args (REST-NATIVE - insert/erase always, a plain append/push/sort per-op),
     * unset for a no-value-arg builtin (pop/intptr). The handler forms arg0's
     * LValue* from the matching table (a not-yet-defined global -> null target ->
     * NotLValueEx, as the tree-walker) and calls func_lv, which NEVER self-evals
     * a node (append's construct-in-place is the tree-walker's append_tw + the
     * VM's EmplaceStruct). Emitted only when arg0 is a slotted identifier; a
     * subscript target is CallBuiltinLVElem.
     */
    CallBuiltinLV,

    /*
     * Emplace-append a struct (Phase 2b): `append(struct_arr, Ctor(args))` with
     * the ctor's arg VALUES already in a register run [b.lit, b.lit+nfields).
     * `a.lit` = arg0's slot KIND (like CallBuiltinLV, forms the target ptr);
     * `target2` = arg0's slot; `target` = the dst slot; `node` = the append
     * DirectBuiltinCallExpr (args[1] is the ctor, carrying vm_struct_ctor_def
     * + the field arg locs). vm_emplace_struct coerces the values straight into
     * the flat POD-struct array's bytes (no temporary StructObject); a non-flat
     * target falls back to building the struct + a general append.
     */
    EmplaceStruct,

    /*
     * Mutating builtin with a SUBSCRIPT lvalue target (Phase 2c): append/push/
     * pop of `a[i]` / `d[k]`. `a.lit` = the BASE's slot KIND, `target2` =
     * the base's slot, `b` = the (compiled) index operand; `node` = the append
     * DirectBuiltinCallExpr (its args[0] is the Subscript, for the error loc).
     * The handler forms the base's LValue*, then the ELEMENT's LValue* via the
     * runtime `Type::subscript(base, idx, for_write=false)` - the SAME path,
     * COW) the tree-walker's Subscript::do_eval uses, given the identical base
     * LValue* - and calls func_lv, which self-evaluates its remaining args. A
     * non-lvalue element (a flat scalar / read-only) gives a null target ->
     * NotLValueEx. Emitted for append/push/pop (self-eval, not rest-native)
     * with a slotted-id base; a nested base / insert/erase stay EvalToSlot.
     */
    CallBuiltinLVElem,

    /*
     * Mutating builtin with a struct-MEMBER lvalue target `append(s.f, x)`:
     * `a.lit` = the base's slot KIND, `target2` = the base's slot, `a.slot` =
     * the builtin_calls pool index (its `.member` is the field name, `.args` the
     * carets), `b` = the REST run base (pre-evaluated value args, rest-native).
     * The handler forms the base's LValue*, then the boxed FIELD LValue* via
     * vm_member_lvalue (the SAME check the tree-walker's MemberExpr does), and
     * calls func_lv REST-NATIVE. A POD/const/read-only/missing field -> the same
     * throw the tree-walker raises. AST-free.
     */
    CallBuiltinLVMember,

    /*
     * Native USER-function call (no node->eval): the args are already evaluated
     * into a contiguous register run [a.lit, a.lit + b.lit); `target2` = the
     * callee's GLOBAL-table slot (a DirectCallExpr with vm_direct_func, so the
     * callee is a proven user function); `target` = the dst slot for the
     * result;
     * `node` = the CallExpr (its loc is the backtrace call site + the
     * NotCallableEx loc if the slot was reassigned to a non-function). Gathers
     * the arg values and calls vm_call_func -> do_func_call, which runs the
     * callee's body via its own chunk (Phase 4) or the tree-walker. Builtins
     * (DirectBuiltinCallExpr) and struct constructors stay EvalToSlot/EvalStmt.
     */
    CallV,

    /*
     * Like CallV but for a CachedCallExpr (a pure tree-recursive callee the
     * unroll dedups): goes through vm_cached_call, which checks the caller
     * frame's per-frame PureCache ({func, arg values} -> result) before the
     * call.
     * WITHOUT this, the exponential recursion the cache collapses (fib$0) would
     * be recomputed - a huge regression. Same operand layout as CallV.
     */
    CachedCallV,

    /*
     * Indirect call of a FUNCTION VALUE (a closure / lambda / func-valued var):
     * a plain CallExpr whose callee is Func-typed (vm_direct_func) but NOT
     * devirtualized to a global slot. The callee EXPRESSION is evaluated into
     * `target2` (a temp holding the FuncObject) before the args register run
     * [a.lit, a.lit + b.lit); the op reads it and calls vm_call_func. A
     * non-FuncObject value (dyn-laundered) throws NotCallableEx via the loc
     * table (unreachable given the Func static type). `node` = the CallExpr,
     * for the backtrace call-site loc (nulled by extract_locs).
     */
    CallValueV,

    /*
     * map()/filter() validate arg0 (the function) BEFORE evaluating arg1 (the
     * container) - a TESTED order the eager value ABI would break. So the VM
     * emits, in eval order: [compile arg0 -> t0], CheckFuncV(t0), [compile arg1
     * -> t1], MapFilterV(dst, t0, t1). CheckFuncV throws before arg1's code
     * runs, exactly like the tree-walker.
     *
     * CheckFuncV: `a` = the slot holding arg0's value; throws TypeErrorEx
     * ("Expected function") via `node` (arg0's caret) if it is not a
     * FuncObject. No dst - a pure guard.
     */
    CheckFuncV,

    /*
     * MapFilterV: `target` = dst; `a` = the (already CheckFuncV'd) function
     * slot; `b` = the container slot; `target2` = is_filter (0 = map, 1 =
     * filter). Calls the shared vm_map_filter (generic.cpp.h) - map builds a
     * fresh array, filter keeps truthy elements (array->array, dict->dict).
     * `node` = arg1 (the unsupported-container caret).
     */
    MapFilterV,

    /*
     * Native `return <expr>;` (no node->eval of the return): `a` = the slot
     * holding the already-evaluated return value (a bare `return;` loads `none`
     * into it first). Sets ctx->flow to {ret, value} and STOPS the chunk
     * (returns from vm_run_chunk), exactly as an EvalStmt(ReturnStmt) does - so
     * do_func_call reads flow->value. The return EXPRESSION is compiled by
     * compile_boxed_expr, so its own calls (`return f(x)`) become CallV.
     */
    ReturnV,

    /*
     * Load an immediate into a frame slot: slot[target] = <int/float literal>
     * (`a.lit` / `a.flit`). This is the clean "move a constant to a slot" that
     * a `var i = 0` / `x = 5` compiles to (vs an `IntBin dst = imm + 0`
     * add-with-zero). Trivial, so it is NOT counted by the has_native gate (a
     * body of only constant loads isn't worth running via the VM chunk).
     */
    LoadImmInt,
    LoadImmFloat,

    /*
     * The BOXED general-value path (the zero-fallback / dyn tier). These
     * operate on EvalValue frame slots (the same slots the typed ops use, read
     * as boxed values) and call the RUNTIME directly - num_bin_op / the Type
     * vtable - so a `dyn` / string / bool scalar expr runs native instead of
     * falling back to node->eval. No Construct* (a machine-code backend lowers
     * these to runtime-library calls). Args pre-evaluated into slots.
     *
     * LoadConstV  slot[target] = consts[target2]           (a baked literal)
     * MoveV       slot[target] = RValue(slot[target2])     (alias, no clone)
     * BinOpV      slot[target] = clone(RValue(slot[a])) <aop> RValue(slot[b])
     *             via num_bin_op (arith/bitwise Op); mirrors the tree-walker
     *             (clone the left operand, then mutate it - so `a+b` doesn't
     *             corrupt a), incl. int/float promotion + string `+` concat.
     */
    LoadConstV,
    MoveV,
    BinOpV,

    /*
     * Materialize a baked const array/dict/struct literal (a LiteralObj):
     * slot[target] = eval_literal_obj(literal_objs[target2]) - an immutable
     * share or a fresh mutable deep clone (plus the general/flat_s cases). This
     * is what a `var a = [1,2,3]` / `d = {}` const-literal rvalue lowers to (a
     * scalar const is a LoadConstV; a non-const-element literal is MakeArrayV/
     * MakeDictV). Never throws (node-free).
     */
    LoadLiteralObjV,

    /*
     * Boxed compound-assign `dst OP= b` (dst = a frame slot, b a slot operand,
     * aop the BASE arith Op): copies the lvalue's value (NOT clone - so a
     * container mutates IN PLACE, matching the tree-walker's `newVal =
     * slot->get(); apply_compound_op(newVal, rhs, op)`), applies num_bin_op,
     * stores back. `node` = the statement (for the error loc). Only for a
     * dyn/string lvalue - a typed int/float compound is the IntBin/FloatBin
     * fast path.
     */
    CompoundV,

    /*
     * Boxed comparison `dst = (a <cmp> b)` -> a bool. Copies a, num_bin_op with
     * the comparison Type method (lt/gt/le/ge/eq/noteq), stores
     * EvalValue(result.is_true()) - exactly Expr06/Expr07::do_eval. `node` =
     * the right operand (error loc). A dyn/string cmp the typed path can't do.
     */
    CmpV,

    /*
     * Boxed logical `dst = a <&&|||> b` -> a bool. MyLang's `&&`/`||` do NOT
     * short-circuit at runtime (both operands always evaluate - only the
     * compile-time folder short-circuits const cases), so this is EAGER:
     * EvalValue(a.is_true() <op> b.is_true()), matching logop_loc /
     * Expr11/Expr12. `aop` = Op::land or Op::lor. No error path (is_true is
     * total).
     */
    LogV,

    /*
     * Boxed UNARY op `dst = <unaryop> a` for a dyn/general operand (the typed
     * int/float unary is the M8 TypedScalarExpr path). `a` = the operand
     * (slot/immediate), `aop` = the unary Op: `lnot` (`!` -> !is_true(), a bool),
     * `minus` (`-` -> opneg, a bool promotes to int), `bnot` (`~` -> bitwise not,
     * bool->int), `plus` (`+` -> bool->int else no-op). Mirrors Expr02::do_eval
     * (clone the operand, apply). Type errors (`-str`, `~str`) stamp the loc
     * side table. This is what an `if (!x)` / `var b = !s` over a dyn/string
     * operand lowers to (was a JumpIfFalse / EvalToSlot fallback).
     */
    UnaryV,

    /*
     * Boxed NON-LOCAL leaf loads (the boxed path's operand can be a global /
     * captured / builtin value, not only a resolved-local slot): read the value
     * into a temp `target` from `target2` = the table index. Mirror
     * Identifier::do_eval.
     *   LoadGlobalV   gfuncs->slots[target2] (throws UndefinedVariableEx via
     *                 `node` if !gfuncs->defined - a read before the decl ran)
     *   LoadCaptureV  (*captures)[target2] (a closure's per-instance capture)
     *   LoadBuiltinV  builtin_slot(target2) (an unshadowed builtin as a value)
     */
    LoadGlobalV,
    LoadCaptureV,
    LoadBuiltinV,

    /*
     * `defined(g)` where `g` is a GLOBAL-table symbol: `target` = the dst temp,
     * `target2` = the global slot. Reads `gfuncs->defined[target2]` as a bool -
     * the genuine runtime property a global's definedness is (false before its
     * decl executes, true after). AST-free (the slot is known at codegen; never
     * throws). The always-bound cases (param/local/capture/builtin) fold to
     * `true` at resolve time (try_fold_defined); this native op is what keeps
     * the ONE runtime `defined` case off the EvalToSlot fallback.
     */
    DefinedGlobalV,

    /*
     * PLAIN assignment to a GLOBAL-table slot `g = <expr>` (a top-level var a
     * function reads - the write counterpart of LoadGlobalV): the value temp is
     * in `a`, `target` = the GlobalFuncTable slot index. Writes
     * gfuncs->slots[target] and marks defined[target]=1 - byte-identical to the
     * tree-walker's decl (bind + define) AND reassign (overwrite), which for a
     * plain assign are the same put(). Never throws (a typed/const/compound
     * global write falls back to EvalStmt). Script-only (no SymKind::global
     * exist in the REPL), so gfuncs is never null here.
     */
    StoreGlobalV,

    /*
     * A CONST DECL of an arr/dict/func kept as a runtime symbol (`const x =
     * <LiteralObj>`). Materialize the rvalue (`a`) then BIND the slot as a
     * CONST LValue - `target` = the slot, `target2` = 0 (a main-frame LOCAL
     * slot) or 1 (a GLOBAL slot). Binding CONST (not a plain put) is what makes
     * a later rebind still throw CannotRebindConstEx (a rebind stays EvalStmt).
     * Only a DECL reaches here (Expr14 with pInConstDecl); a const reassign
     * never does. Const SCALARS are inlined, so they never appear.
     */
    DeclConstV,

    /*
     * Write to a closure CAPTURE slot `cap = <expr>` / `cap OP= v` / `cap++`
     * (the write counterpart of LoadCaptureV): the value/rhs in `a`, `target`
     * = the index into the called closure's per-instance capture vector
     * (ctx->captures). `aop == Op::invalid` is a plain assign (put(RValue)),
     * else a compound / inc-dec (copy-modify-store via num_bin_op, node for the
     * caret). Unlike a global slot a capture is ALWAYS defined (snapshot at
     * closure creation), so there is no defined check. This is what a counter
     * closure's `start++` compiles to. Never emitted in the REPL (captures stay
     * map-resident there).
     */
    StoreCaptureV,

    /*
     * Boxed subscript READ `dst = base[idx]` (a general array / dict / string
     * element - the typed flat-array path is LoadElem*). `target2` = the base
     * slot, `a` = the index slot, `node` = the Subscript (error loc). Calls the
     * runtime Type::subscript(for_write=false) with an LValue* to the base slot
     * (matching Subscript::do_eval) and RValues the result into `target`.
     */
    SubscriptV,

    /*
     * Boxed member READ `dst = base.member` (a struct field / const / dict key
     * / optional-`?.`). `target2` = the base slot, `node` = the MemberExpr (its
     * memUid/memId/optional/loc). Calls the shared member_read (the value-read
     * path of MemberExpr::do_eval) into `target`.
     */
    MemberV,

    /*
     * Boxed slice READ `dst = base[start:end]` (an array / string sub-view):
     * `target2` = the base slot, `a` = the start-index slot (-1 = absent -> the
     * slice defaults to 0), `b` = the end-index slot (-1 = absent -> to the
     * end); `node` = the Slice (loc for a non-int-index TypeError, nulled by
     * extract_locs). Calls the runtime `base.get_type()->slice(base, start,
     * end)` (which RValues the base + registers the COW slice view), mirroring
     * Slice::do_eval.
     */
    SliceV,

    /*
     * Build an array VALUE from element values in a register run, into `target`
     * - the native form of a `[a, b, ..]` LITERAL whose elements aren't all
     * const (a fully-const literal is baked to a single LoadConstV, not this).
     * The N element values are in slots [a.lit, a.lit + b.lit); `target2`
     * carries the flat/general ArrHint. Shares the tree-walker's build core
     * (build_array_from_values). No Construct*: the build never throws.
     */
    MakeArrayV,

    /*
     * Build a dict VALUE `{k0: v0, ..}` from a register run into `target` - the
     * native form of a `{..}` LITERAL. The `b.lit` key/value pairs are
     * INTERLEAVED in slots [a.lit, a.lit + 2*b.lit): key at even, value at odd
     * offset. Shares build_dict_from_pairs (freezes each key). No Construct*.
     */
    MakeDictV,

    /*
     * Build a CLOSURE value `func [caps] (params) {..}` (a lambda in expression
     * position) into `target`: make_intrusive<FuncObject>(def, &ctx), which
     * snapshots the captures from ctx - byte-identical to FuncDeclStmt::do_eval
     * for a lambda (id == null). `target2` = the index into Chunk::closure_defs
     * (the program-lifetime FuncDeclStmt*; the closure inherently needs its
     * definition, but the Instr carries only an index, no raw Construct*). The
     * ctor never throws for a resolved closure (captures are all resolved), so
     * node-free / no loc. This is what a `return <lambda>` / `var f = <lambda>`
     * folds to.
     */
    MakeClosureV,

    /*
     * Construct a POD struct STANDALONE `P(x, y)` into `target`: the field arg
     * values are in the register run [a.lit, a.lit + b.lit), `target2` = the
     * index into Chunk::struct_defs (the program-lifetime StructTypeDef*; the
     * Instr holds only the index). construct_struct_from_values coerces the
     * values into the POD bytes. The codegen emits this ONLY when nargs ==
     * nfields and every arg is a typed scalar (th==i/f) the inferencer proved
     * assignable, so coerce cannot throw; a defensive throw is caught + given
     * the construction's loc (loc side table). The append-fused ctor is
     * EmplaceStruct, not this.
     */
    StructCtorV,

    /*
     * Construct a BOXED (non-POD) struct `B(a, x)` into `target`: the field arg
     * values are in the register run at [a.lit, a.lit + nargs), where nargs =
     * boxed_ctors[target2].arg_locs.size(); `target2` = the index into
     * Chunk::boxed_ctors (the StructTypeDef* + per-arg carets). Unlike the POD
     * ctor, a boxed field's coerce CAN throw (a dyn-laundered wrong value), so
     * the per-arg carets are pooled (serializable, no `node`) and the throw
     * reports the offending arg's caret - byte-identical to the tree-walker's
     * construct_struct. Omitted trailing opt fields (nargs < nfields) bind none.
     */
    StructCtorBoxedV,

    /*
     * Unconditionally THROW a runtime error whose kind/loc/name are pooled in
     * Chunk::throws[target] (a serializable `{ThrowKind, Loc, name}`). This is
     * the native form of an always-throwing construct the tree-walker runs and
     * throws on — an undefined name in an rvalue/callee position, an assignment
     * to a non-lvalue (a literal) or a builtin, an lvalue-builtin on a literal
     * arg0. The codegen emits it (after compiling any side-effecting rvalue) in
     * place of an EvalStmt fallback, so the same exception fires at the same
     * point with the byte-identical caret — AST-free.
     */
    ThrowRuntimeV,

    /*
     * Generic INDIRECT call of a `dyn` callee (`Instr::a` = the callee temp;
     * `node_idx` = the CallExpr). Reads the callee value and dispatches on its
     * RUNTIME type via the shared `dispatch_call_value` — a FuncObject (its body
     * runs native via the do_func_call hook), a Builtin (its ExprList ABI), a
     * struct descriptor (construct), else NotCallableEx at the callee caret —
     * byte-identical to the tree-walker's CallExpr::do_eval. This op KEEPS its
     * node: a dyn callee may resolve to an AST builtin (`defined` — needs the
     * unevaluated arg node) or a mutating builtin (needs an lvalue arg), so the
     * arg AST is intrinsically required; only the callee LOAD is native.
     */
    CallValueGenericV,

    /*
     * Build a FLAT array<PodStruct> LITERAL `[P(a,b), P(c,d), ..]` into `target`
     * in ONE op - the FUSED form of N StructCtorV + MakeArrayV (F-4). The N
     * structs' field args are compiled INTERLEAVED into the register run
     * [a.lit, a.lit + N*M) (struct i's field j at base + i*M + j, M =
     * def->fields.size()); `b.lit` = N; `target2` = the Chunk::struct_defs index.
     * vm_make_struct_array coerces the values STRAIGHT into a contiguous flat
     * byte buffer (no intermediate StructObject per element, unlike the
     * StructCtorV path) and builds the flat mode-5 array - so it BEATS the
     * tree-walker's LiteralArray::do_eval (which allocates N StructObjects then
     * packs them). Emitted only when every element is a same-POD-struct ctor
     * with all-scalar field args (coerce can't throw); a defensive throw is
     * stamped with the ctor loc (loc side table). This is the EmplaceStruct
     * pattern for a whole literal.
     */
    MakeStructArrayV,

    /*
     * Branch on a BOXED bool slot: if NOT slot[target2].is_true(), pc = target.
     * A boxed condition (`if (a == b)`, `while (x != none)`, `if (x)`) compiles
     * to <boxed expr into a slot> + this. Mirrors the tree-walker's is_true()
     * truthiness (0/none/[]/{} false).
     */
    JumpUnlessTrueV,

    /*
     * VM exceptions (P8 Inc 0). A `try/catch` (no finally) lowers to a handler
     * region; `throw` + runtime errors still go through the C++ boundary in
     * vm_run_chunk, which routes them into the handler stack. All AST-free
     * (offsets + pool indices), so serializable.
     *   PushHandler(target=catch_pc): push an active try region.
     *   PopHandler: pop it (the try body exited normally).
     *   CatchTest(a=catch_types idx or -1=catch-all, target2=bind slot or -1,
     *             target=catch-body pc): if the in-flight exception's type name
     *             matches, bind `catch (T as e)` + jump to the body; else fall
     *             through to the next CatchTest / Reraise.
     *   Reraise: no clause matched → re-raise the in-flight exception to the
     *            OUTER handler (native jump) or propagate (C++ throw).
     *   Rethrow: `rethrow` in a catch body → re-raise vm_exc with the
     *            rethrow-site loc (from the loc side table).
     *   Throw(a=value slot): raise the value (Inc 1). A same-frame catch is a
     *            NATIVE jump (no C++ throw); no handler here → C++ throw
     *            (cross-frame). The throw-site loc is in the loc side table.
     *   SetPend(target=Pend): set the pending action the SHARED `finally` must
     *            resume - normal or reraise (Inc 2b). A flow op inlines its own
     *            finally (Inc 2c), so it never sets a pending action.
     *   EndFinally: at the shared `finally` block's end, resume vm_pend - fall
     *            through (normal) or re-raise vm_exc (reraise).
     */
    Throw,
    PushHandler,
    PopHandler,
    CatchTest,
    Reraise,
    Rethrow,
    SetPend,
    EndFinally,

    /* Stop the program. */
    Halt,
};

/*
 * A register-machine operand: a resolved-local frame SLOT (the VM's registers
 * are the frame slots) or an immediate int literal. Read by the native int ops.
 */
struct Operand {
    bool is_lit = false;
    /* When is_lit, which immediate kind is live: an int op (IntBin) always
     * reads `lit`, a float op (FloatBin) `flit`, so for those it is implied; a
     * BOXED
     * op (BinOpV/CmpV/LogV/CompoundV) reads whichever this says and
     * materializes an EvalValue of that kind - so `r0 - 3`, `f + 1.5`,
     * `b && true` need no LoadConstV first. `b` (bool) stores 0/1 in `lit`. */
    enum class LitKind : unsigned char { i, f, b };
    LitKind lit_kind = LitKind::i;
    int slot = -1;         /* frame slot index when !is_lit */
    union {
        int_type lit = 0;
        float_type flit;
    };
};

struct Instr {
    OpCode op;
    /* Index into Chunk::ast_nodes for an op that still needs the AST at runtime
     * (fallback / builtin-call / a store's caret); -1 == none. NO raw Construct*
     * in the Instr - that is what lets the bytecode be serialized. */
    int32_t node_idx = -1;
    int target = -1;    /* Jump/JumpIfFalse dest; LoopBackEdge cont; IntBin dst
                         * slot; JumpUnlessIntCmp jump dest */
    int target2 = -1;   /* LoopBackEdge exit dest */
    Op aop = Op::invalid;   /* IntBin: arith op; JumpUnlessIntCmp: compare op */
    Operand a;              /* IntBin / JumpUnlessIntCmp: left operand */
    Operand b;              /* IntBin / JumpUnlessIntCmp: right operand */
};

struct Chunk {
    std::vector<Instr> code;
    /*
     * Scratch frame slots the register machine needs for expression
     * intermediates, laid out ABOVE the resolved locals: temps occupy
     * [slot_count, slot_count + n_temps). vm_execute sizes the frame to fit
     * them. Zero when no native expression needed a temp.
     */
    int n_temps = 0;
    /*
     * The number of live dict-iterator state slots vm_run_chunk must allocate
     * (max iter_id + 1). Sized like n_temps; one per native dict foreach in the
     * chunk (nested/sequential get distinct ids). See the DictIter ops.
     */
    int n_dict_iters = 0;

    /* Live dyn-foreach iterator state slots (max iter_id + 1); one per native
     * ForeachDyn in the chunk. See the ForeachDyn ops. */
    int n_dyn_iters = 0;
    /*
     * The BOXED general-value path's constant pool: literal EvalValues baked at
     * codegen (a machine-code backend would put these in the data section),
     * each referenced by index from a LoadConstV. Empty until a boxed op needs
     * a literal operand.
     */
    std::vector<EvalValue> consts;
    /*
     * Debug info for the disassembler (-vd) ONLY - not consulted by the VM.
     * `slot_count` = the number of resolved-local slots; a register below
     * slot_count is a named variable, at/above it a scratch temp.
     * `slot_names[s]` is
     * that local's source name (empty for an unnamed/temp slot), so `-vd` can
     * print `s = s + i` instead of `r3 = r3 + r2`. Populated by codegen.
     */
    int slot_count = 0;
    std::vector<std::string> slot_names;

    /*
     * Loc SIDE TABLE (foundation for an AST-free bytecode). An op that can
     * throw
     * records its source Loc here as `{pc, start, end}` instead of carrying an
     * `Instr::node` AST pointer just for the caret - so the op becomes
     * self-contained (serializable, JIT-able) and `Instr` loses a `node` use.
     * Built by a post-codegen pass (extract_locs) over the finished chunk,
     * so the codegen's rollback logic is untouched; naturally sorted by pc
     * (the walk is ascending). Cold: read ONLY on the throw path via loc_at.
     */
    struct LocEntry { uint32_t pc; Loc start; Loc end; };
    std::vector<LocEntry> locs;

    /* The recorded Loc of the op at `pc` (exact match; sets start/end to {} and
     * returns false if this op recorded none). Binary search - O(log n) on the
     * error path only. */
    bool loc_at(size_t pc, Loc &start, Loc &end) const
    {
        size_t lo = 0, hi = locs.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (locs[mid].pc < pc)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < locs.size() && locs[lo].pc == pc) {
            start = locs[lo].start;
            end = locs[lo].end;
            return true;
        }
        start = Loc();
        end = Loc();
        return false;
    }

    /*
     * INLINED-FRAME side table (P8 Inc 4 - backtrace parity for inlined code).
     * An op physically spliced from an INLINED function body records that
     * body's `InlineCtx` "inlined-at" chain here, so a backtrace crossing it
     * reconstructs the virtual inlined frames - the tree-walker gets them from
     * `node->inline_ctx` via `Construct::eval`, which native ops have no hook
     * for. SAME shape/cost as `locs`: `{pc, InlineCtx*}`, sorted by pc, read
     * ONLY on the throw path (via inline_ctx_at). The `InlineCtx*` is
     * program-lifetime AST-owned (like closure_defs / struct_defs); a
     * serializing backend would flatten the chain to interned strings + a
     * parent-index array (a separate axis from `locs`, which is pure data).
     */
    struct InlineEntry { uint32_t pc; const InlineCtx *ic; };
    std::vector<InlineEntry> inline_ctxs;

    /* The inlined-at chain of the op at `pc` (exact match; null if none).
     * Binary search - O(log n), throw path only. */
    const InlineCtx *inline_ctx_at(size_t pc) const
    {
        size_t lo = 0, hi = inline_ctxs.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (inline_ctxs[mid].pc < pc)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < inline_ctxs.size() && inline_ctxs[lo].pc == pc)
            return inline_ctxs[lo].ic;
        return nullptr;
    }

    /*
     * MEMBER-KEY POOL (foundation 2, AST-free op-data). A boxed member read
     * `base.member` (MemberV) needs the member's name (as a dict key AND an
     * interned uid), the optional-`?.` flag, and the carets member_read throws
     * with - all pulled out of the MemberExpr into a pool entry so the op is
     * a bare index (`Instr::a`), no `node`. Program-lifetime data, so the
     * interned uid pointer is stable; a serializing backend would re-intern by
     * name on load.
     */
    struct MemberKey {
        EvalValue memId;          /* the name as a dict key value */
        const UniqueId *memUid;   /* the interned name (struct slot / const) */
        bool optional;            /* `a?.b` short-circuits a none base */
        Loc mstart, mend;         /* the member-expr caret (most errors) */
        Loc bstart, bend;         /* the base caret ("Expected dict object") */
    };
    std::vector<MemberKey> member_keys;

    /*
     * LVALUE-CHAIN STEP POOL (StoreLValueChainV). One entry per general nested
     * store `base.step1.step2... = v` mixing MEMBER and SUBSCRIPT steps (a
     * pure-subscript chain keeps StoreElem2V/StoreElemChainV). Each step is a
     * member (`operand` = a member_keys pool index) or a subscript (`operand` =
     * the frame temp holding the pre-evaluated key). Inside-out (base -> final).
     */
    struct ChainStep {
        bool is_member;
        int32_t operand;   /* member: member_keys idx; subscript: key temp reg */
        Loc lstart, lend;  /* this step's NODE loc - a throw AT this step (OOB /
                            * KeyNotFound / NotLValue) uses it, byte-identical to
                            * the tree-walker's per-node stamp (the final step's
                            * node is the whole lvalue). */
    };
    std::vector<std::vector<ChainStep>> chain_steps;

    /*
     * PER-STEP LOCS for a pure-subscript nested store (StoreElem2V /
     * StoreElemChainV). One entry per op, INSIDE-OUT (matching the keys): each
     * pair is a subscript node's caret. A throw AT step k (an intermediate OOB /
     * KeyNotFound, or the final store) uses step k's loc - byte-identical to the
     * tree-walker's per-node stamp (the outer op's single side-table loc was an
     * off-by-a-few-cols imprecision for an INTERMEDIATE throw).
     */
    std::vector<std::vector<std::pair<Loc, Loc>>> chain_locs;

    /*
     * CATCH-TYPE POOL (P8). One entry per `catch (A, B, ...)` clause with a type
     * list: the interned type NAMES a CatchTest matches the in-flight
     * exception's name against (a user struct-type name or a built-in error
     * name). A catch-all clause (`catch`/`catch (e)`) has no entry (CatchTest
     * carries -1). Strings, so serializable as-is.
     */
    std::vector<std::vector<std::string>> catch_types;

    /*
     * Baked const array/dict/struct literals (LoadLiteralObjV). The op reads
     * entry here and calls eval_literal_obj (immutable share vs a fresh mutable
     * clone, plus the general/flat_s arr_hint cases) - AST-free, no LiteralObj
     * node at run time. `arr_hint_struct` is a program-lifetime StructTypeDef*
     * (a type descriptor, not a Construct), used only for the flat_s case.
     */
    struct LiteralObjEntry {
        EvalValue value;
        bool immutable;
        ArrHint arr_hint;
        const StructTypeDef *arr_hint_struct;
    };
    std::vector<LiteralObjEntry> literal_objs;

    /* Lambda definitions for MakeClosureV (the FuncDeclStmt* of each `func[..]`
     * expression). Program-lifetime AST-owned pointers; the Instr carries the
     * index, so it holds no raw Construct*. */
    std::vector<const FuncDeclStmt *> closure_defs;

    /* Struct type descriptors for StructCtorV (the StructTypeDef* of each
     * standalone `P(..)` construction). Program-lifetime AST-owned pointers,
     * indexed by the Instr. */
    std::vector<const StructTypeDef *> struct_defs;

    /* BOXED-struct-ctor pool for StructCtorBoxedV: the constructed def plus the
     * per-arg carets a field coerce throws with (a dyn-laundered wrong value).
     * The def pointer is AST-owned/program-lifetime (re-bindable by name on
     * load); the ArgLocs are pure data - so this pool is serializable. */
    struct BoxedCtor {
        const StructTypeDef *def;
        std::vector<ArgLoc> arg_locs;   /* provided args [0, nargs) */
    };
    std::vector<BoxedCtor> boxed_ctors;

    /* Always-throw sites for ThrowRuntimeV (`Instr::target` indexes this): the
     * runtime exception an always-throwing construct raises, as pure data — the
     * kind, the caret, and (for an undefined name) the interned name. Fully
     * serializable (the `name` re-interns from its string). */
    enum class ThrowKind : unsigned char {
        undefined_var,      /* UndefinedVariableEx(name, loc) */
        not_lvalue,         /* NotLValueEx(loc) */
        rebind_builtin,     /* CannotRebindBuiltinEx(loc) */
        rebind_const,       /* CannotRebindConstEx(loc) */
    };
    struct ThrowSite {
        ThrowKind kind;
        Loc start, end;
        const UniqueId *name = nullptr;   /* undefined_var only */
    };
    std::vector<ThrowSite> throws;

    /* Target frame slots for each MultiUnpackV (`a, b, c = <rvalue>`), in
     * order, with -1 for a `_` placeholder. `Instr::target` indexes this. Pure
     * data (ints) - serializable. */
    std::vector<std::vector<int32_t>> unpack_targets;

    /*
     * BUILTIN-CALL POOL (AST-free builtin dispatch). A value-ABI builtin call
     * (CallBuiltinV) needs only its Builtin (the baked func pointers) and the
     * AST-free ArgLocs data - the whole-args caret, the per-arg carets, and the
     * array-repr hint - all pulled out of the DirectBuiltinCallExpr into a pool
     * entry, so the op is a bare index (`Instr::target2`), holding NO `node`.
     * The Builtin's function pointer is resolve-at-load like struct_defs (a
     * serializer stores the builtin's registered name/id and re-binds on load);
     * everything else is pure data. This is what lets a read-only builtin call
     * lower with an empty ast_nodes pool.
     */
    struct BuiltinCall {
        Builtin builtin;              /* the baked func_v (+ func_lv) pointers */
        const UniqueId *name;         /* the builtin's name (disasm; a serializer
                                       * re-binds the func ptr from it on load) */
        Loc start, end;               /* the whole-args caret (arity/generic) */
        ArrHint arr_hint;             /* the array-repr hint (range/array/…) */
        std::vector<ArgLoc> args;     /* per-arg carets, [0, n) */
        const UniqueId *member = nullptr;  /* CallBuiltinLVMember: arg0's field
                                            * name `append(s.f, x)` (else null) */
    };
    std::vector<BuiltinCall> builtin_calls;

    /* Build the AST-free ArgLocs a func_v takes, from pool entry `idx`. Read on
     * the CallBuiltinV hot path (cheap: two Locs + two pointers, no copy). */
    ArgLocs arglocs_at(int idx) const
    {
        const BuiltinCall &bc = builtin_calls[static_cast<size_t>(idx)];
        ArgLocs al;
        al.start = bc.start;
        al.end = bc.end;
        al.args = bc.args.data();
        al.nargs = bc.args.size();   /* total arg count (a func_lv reads this) */
        al.arr_hint = bc.arr_hint;
        return al;
    }

    /*
     * AST-NODE POOL - the ONE non-serializable pool, holding the raw
     * `Construct *` of every op that still needs the AST at RUNTIME (an
     * `Instr::node_idx` indexes it, so `Instr` itself holds NO raw Construct*):
     *   - the fallback ops EvalStmt / EvalToSlot / JumpIfFalse (they re-enter
     *     the tree-walker via `node->eval`);
     *   - EmplaceStruct - the ONLY builtin op left here: it needs the ctor node
     *     (its vm_struct_ctor_def + field-arg carets). The value-ABI (CallBuiltinV)
     *     AND the mutating lvalue-ABI (CallBuiltinLV / CallBuiltinLVElem) calls
     *     are NO LONGER here - they pool their Builtin + ArgLocs in
     *     `builtin_calls` (serializable), so they hold no node.
     * A program that lowers 100% natively (no fallback, no dev-builtin) leaves
     * this EMPTY - so a non-empty `ast_nodes` is EXACTLY the "not yet fully
     * serializable" signal (a `.myv` writer must reject it or keep the AST).
     * Program-lifetime AST-owned pointers, like closure_defs / struct_defs.
     * Built during codegen (add_ast_node), then COMPACTED after extract_locs so
     * the loc-only nodes it dropped (and any codegen-rollback orphans) leave no
     * dead entries. `node_at` returns nullptr for the -1 (no-node) sentinel.
     */
    std::vector<const Construct *> ast_nodes;

    const Construct *node_at(int idx) const
    {
        return idx < 0 ? nullptr : ast_nodes[static_cast<size_t>(idx)];
    }
};
