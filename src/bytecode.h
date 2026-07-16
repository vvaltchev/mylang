/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "syntax.h"
#include <cstring>   /* memcpy - the packed flit accessors (B3) */

#include <vector>

/*
 * The runtime bytecode for the -vm execution engine (see plans/bytecode-vm.md).
 * The VM consumes the ALREADY-OPTIMIZED AST (post infer / resolve_names /
 * specialize_types) and lowers it to a flat instruction list, removing the
 * tree-walker's per-node virtual-dispatch tax. It is built strictly
 * incrementally - ORIGINALLY behind AST-fallback opcodes that ran the WHOLE
 * language from day one while native opcodes replaced them one tested step at
 * a time; those fallback ops are since DELETED: the codegen is NO-FAIL (every
 * construct lowers natively, or compilation refuses with NotLoweredEx), so no
 * op can re-enter the AST.
 *
 * Phase 0: fallback + Halt. Phase 1 adds native control flow (Jump /
 * the since-deleted LoopBackEdge). Phase 2 adds a REGISTER machine over the frame
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

    /* Unconditional jump: pc = `target`. */
    Jump,

    /* (LoopBackEdge - the Phase-1 flow-consuming back edge - is DELETED:
     * since the no-fail codegen removed every fallback body, nothing set a
     * brk/cont/ret FlowState inside a chunk, and codegen had already
     * stopped emitting it. Native loops branch with Jump/JumpUnless*;
     * `return` is ReturnV. See plans/vm-native-call-stack.md Phase A.) */

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
     * discarded (statement). AST-FREE: its TWO error carets - the SUBSCRIPT
     * loc (a subscript-internal KeyNotFound/OOB) vs the INC-DEC loc (its own
     * NotLValue/TypeError) - come from the incdec_sites pool (`b` = the
     * index); the undefined-global-base caret from the loc side table.
     */
    IncDecElemCheckedV,

    /*
     * CHECKED MEMBER inc-dec `d.f++` / `d.f--` for a DYN/unproven base (`d.f`
     * where `d` is dyn/general holding a struct or dict). target = the base slot
     * kind (0 local / 1 global / 2 capture), target2 = the base slot, aop =
     * plus/minus. Mirrors IncDecExpr::do_eval's dyn path over MemberExpr's
     * lvalue logic: a mutable boxed STRUCT field or a DICT value is an lvalue
     * (read, check int/float, ±1); a POD field / readonly / a missing dict key
     * throws (NotLValueEx / KeyNotFoundEx), exactly as the tree-walker.
     * AST-FREE: the member key + its TWO error carets - the MEMBER loc for a
     * KeyNotFound vs the INC-DEC loc for its own NotLValue/TypeError - come
     * from the incdec_sites pool (`b` = the index).
     */
    IncDecMemberCheckedV,

    /*
     * GENERAL inc-dec over an arbitrary lvalue CHAIN, VALUE form (the R4
     * residue: `y = a[f()]++`, `z = ++d[kf()]`, `y = a[f()][g()]++`,
     * `y = a[f()].x++`, `y = mk()[0]++` - an inc-dec whose lvalue has a
     * side-effecting index/base, so the read+mutate double-compile of the
     * pure value path is unsound). The ROOT is a slot (`target2`, kind in
     * `a.lit`: 0 local / 1 global / 2 capture) or a compiled RVALUE temp
     * (kind 3 - seeding the walk with a VALUE reproduces the tree-walker's
     * rvalue-ness, so `mk()[0]++` still throws NotLValueEx); the member/
     * subscript steps + tier/flags/carets live in the `incdec_chains` pool
     * (`b` = the index; each subscript key is a pre-evaluated temp, so
     * every side effect runs EXACTLY ONCE, in source order). The walk is
     * the StoreLValueChainV intermediate walk; the FINAL step runs
     * IncDecExpr::do_eval's exact tier semantics (vm_incdec_final,
     * eval.cpp): tier 2 (a proven int/float lvalue) = the compound-store
     * `±= 1` (flat_store_core when the codegen proved the base
     * side-effect-free, exactly try_flat's gate; else the general
     * subscript/member lvalue + slot_rmw) then old = new ∓ 1 derived with
     * NO re-read; tier 3 (dyn) = the checked read-modify-write (NotLValue/
     * const/TypeError at the INC-DEC caret). `target` = the dst slot
     * (-1 = statement, value discarded), `aop` = plus/minus (inc/dec).
     */
    IncDecChainV,

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
     * Native `foreach (<ids> in [indexed] <dyn container>)`: the container's
     * static type is dyn, so the array-vs-dict choice is at RUNTIME. A live
     * iterator like DictIter, over a `Chunk::n_dyn_iters` state pool indexed by
     * `target` (a codegen-assigned iter_id). GENERAL over the id list: any
     * var count, the `indexed` form, and `_` placeholders — the per-var frame
     * slots (-1 == `_`) live in the unpack_targets pool.
     *
     * ForeachDynInit: `target` = iter_id; `target2` = the container slot;
     * `a` = nvars | (indexed << 8); `b` = the unpack_targets pool index. Pins
     * the container; if an array -> {arr, idx=0, size}; if a dict ->
     * {dict, it=begin}; else throws TypeErrorEx via the loc side table (the
     * container caret; `node` recorded by extract_locs, then nulled).
     */
    ForeachDynInit,

    /*
     * ForeachDynNext: `target2` = iter_id; `target` = end_pc. On exhaustion
     * jumps to end_pc; else binds the loop vars from the state (slots in the
     * unpack_targets entry, -1 skipped) exactly as do_iter: `indexed` binds
     * targets[0] = the iteration counter; an ARRAY element binds the single
     * remaining var BOX-FREE (arr_elem_at) or STRICT-unpacks an array element
     * into N remaining vars (the two do_iter TypeErrorEx's, carets via the
     * loc side table); a DICT binds key [, value [, none...]] (do_iter's
     * count=2 padding). Then advances.
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
     * Native BUILTIN call with the VALUE ABI (no node->eval): the args are
     * already evaluated into the register run [a.lit, a.lit+b.lit); `node` =
     * the DirectBuiltinCallExpr (its baked `builtin.func_v` + its args ExprList
     * for error locs); `target` = the dst slot. Copies the arg values into a
     * buffer and calls func_v. Emitted only for a builtin that HAS func_v (a
     * migrated, read-only builtin); a mutating / AST builtin takes another
     * path (CallBuiltinLV*).
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
     * `a.lit` = arg0's slot KIND packed with the emplace_sites pool index
     * (`kind | idx << 2`; the kind forms the target ptr like CallBuiltinLV);
     * `target2` = arg0's slot; `target` = the dst slot. AST-FREE: the ctor's
     * POD def, the container-arg caret, and the per-field coerce carets come
     * from the emplace_sites pool; the whole-args caret (the catch) from the
     * loc side table. vm_emplace_struct coerces the values straight into
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
     * with a slotted-id base; a nested base / insert/erase decline (the
     * statement falls back whole).
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
     * (DirectBuiltinCallExpr) and struct constructors take their own ops.
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
     * (returns from vm_run_chunk), exactly as the tree-walker's return - so
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
     * slot[target] = coerce_to_decl_type(RValue(slot[a]), int/float) - the
     * typed-store numeric coerce for a PLAIN assign to a `decl_type` i/f
     * variable. The live producer of that shape is the inferencer's
     * coerces_dyn accumulator stamp (`var s = 0; s = s + dyn` keeps `s` int
     * and the store runtime-coerces the dyn value); an EXPLICIT `int x = dyn`
     * is compile-rejected (TypeMismatchEx), so it never reaches codegen.
     * WIDENs (float <- int/bool, int <- bool), passes `none`, THROWS
     * TypeErrorEx on a non-fitting dyn runtime value (never narrows) - the
     * exact coerce_to_decl_type the tree-walker's handle_single_expr14
     * op==assign path runs. `target2` = 1 for float / 0 for int. For a LOCAL
     * lvalue this op IS the store (target = the lvalue slot); a
     * global/capture store coerces into a temp first. Caret via the loc side
     * table (the whole Expr14 span, matching the tree-walker's stamp). A
     * COMPOUND (`s += v`) does NOT coerce - handle_single_expr14 coerces
     * op==assign only - so it lowers as a plain CompoundV/StoreGlobalV.
     */
    CoerceNumV,

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
     * operand lowers to (was a node->eval fallback).
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
     * the ONE runtime `defined` case native (no node->eval).
     */
    DefinedGlobalV,

    /*
     * PLAIN assignment to a GLOBAL-table slot `g = <expr>` (a top-level var a
     * function reads - the write counterpart of LoadGlobalV): the value temp is
     * in `a`, `target` = the GlobalFuncTable slot index. Writes
     * gfuncs->slots[target] and marks defined[target]=1 - byte-identical to the
     * tree-walker's decl (bind + define) AND reassign (overwrite), which for a
     * plain assign are the same put(). Never throws (a typed/const/compound
     * global write takes CoerceNumV). Script-only (no SymKind::global
     * exist in the REPL), so gfuncs is never null here.
     */
    StoreGlobalV,

    /*
     * A CONST DECL of an arr/dict/func kept as a runtime symbol (`const x =
     * <LiteralObj>`). Materialize the rvalue (`a`) then BIND the slot as a
     * CONST LValue - `target` = the slot, `target2` = 0 (a main-frame LOCAL
     * slot) or 1 (a GLOBAL slot). Binding CONST (not a plain put) is what makes
     * a later rebind still throw CannotRebindConstEx (a rebind lowers to the
     * pooled rebind_const throw).
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
     * place of a tree-walked fallback, so the same exception fires at the same
     * point with the byte-identical caret — AST-free.
     */
    ThrowRuntimeV,

    /*
     * Generic INDIRECT call of a `dyn` callee — AST-FREE (F1 step 2).
     * `target` = dst; `target2` = the callee temp; `a` = the arg-run base;
     * `b` = nargs | (call_sites pool idx << 12). The CallSite entry carries
     * the ArgLocs data (whole-args caret + per-arg carets + arr_hint) AND
     * arg0's LVALUE DESCRIPTOR — the by-ref encoding: frame slots can't hold
     * an LValue*-boxed value (LValue::type_checks), so instead of
     * materializing the tree-walker's raw arg value, the op RE-DERIVES
     * arg0's LValue* at dispatch, only when the callee turns out func_lv
     * (the CallBuiltinLV/LVElem/LVMember model). Args 1..n-1 are
     * pre-evaluated into the run; arg0's VALUE fill depends on its form —
     * elem/member fill run[0] via the ordinary SubscriptV/MemberV (throws at
     * arg0's position, the tree-walker's order; the elem INDEX rides a
     * RESERVED temp the descriptor re-reads), slot/undef leave run[0] for
     * the dispatch to derive (an undefined global/unresolved name surfaces
     * at the CONSUMER — bind/adapter — like the tree-walker's raw
     * UndefinedId). Dispatch: FuncObject → vm_call_func (run[0] filled with
     * the derived RValue first); struct → construct_struct_v; Builtin →
     * dispatch_builtin_values by Kind (value / lvalue / EAGER map-filter /
     * lazy tripwire) — the same shared code the tree-walker's indirect
     * branch calls, so both engines agree. A CheckCallableV precedes the
     * arg run (a non-callable callee throws BEFORE the args evaluate). The
     * call-site loc rides the loc side table (do_func_call backtraces).
     */
    CallValueGenericV,

    /*
     * Throw NotCallableEx (callee caret via the loc side table) unless the
     * value in slot `a` is callable — a FuncObject, a Builtin, or a struct
     * type descriptor. Emitted between an indirect call's callee and its arg
     * run: the tree-walker's dispatch throws NotCallableEx BEFORE any arg
     * evaluates, and the eager arg run would otherwise reorder that. (The
     * earlier FuncObject-only CheckCallableV was reverted for rejecting
     * builtin callees; this one admits all three callable kinds.)
     */
    CheckCallableV,


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
     * Null-coalescing short-circuit: if slot[a] is NOT none, pc = target.
     * `a ?? b` compiles to <lhs into dst> + this (skip the rhs) + <rhs into
     * dst> - CoalesceExpr::do_eval's exact semantics (the rhs is never
     * evaluated for a non-none lhs). A pure none test - never throws, so it
     * is node/loc-free.
     */
    JumpIfNotNoneV,

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

    /*
     * B1/B2 SPECIALIZED ARITHMETIC (plans/vm-performance-roadmap.md): the
     * per-operator, per-operand-shape variants of IntBin/FloatBin, selected
     * by specialize_arith_ops (codegen.cpp) as an in-place post-codegen
     * rewrite. Each removes IntBin's inner 11-way `aop` switch (a second
     * data-dependent indirect branch per arith op) AND the two `is_lit`
     * operand-decode branches - a tight 3-address handler under the
     * computed-goto dispatch. RR = both operands slots; RI = b is an
     * immediate (a lit-first COMMUTATIVE op is operand-swapped into RI at
     * specialize time; lit-first non-commutative stays IntBin). Div stays
     * IntBin (zero check + rare); IntModRI is selected only for a NONZERO
     * immediate (the `s % 1000000007` checksum shape - no zero check
     * needed); shifts call bit_shl/bit_shr for the exact count semantics.
     * FloatBin keeps div (and any bool-literal oddity); float `+`/`*`
     * operand-swap is exact for IEEE values (NaN payloads are not
     * observable in-language).
     */
    IntAddRR, IntAddRI, IntSubRR, IntSubRI, IntMulRR, IntMulRI,
    IntAndRR, IntAndRI, IntOrRR, IntOrRI, IntXorRR, IntXorRI,
    IntShlRR, IntShlRI, IntShrRR, IntShrRI,
    IntModRI,
    FloatAddRR, FloatAddRI, FloatSubRR, FloatSubRI, FloatMulRR, FloatMulRI,

    /*
     * D1 (plans/vm-performance-roadmap.md): the dedicated `append(a, x)` /
     * `push(a, x)` op - the CallBuiltinLV shape (a.slot = builtin_calls pool
     * idx, a.lit = arg0's slot KIND, target2 = arg0's slot, b.lit = the
     * value's slot, target = the result dst) with the marshaling deleted:
     * arr_append_fast appends a fitting element straight into the array
     * (flat or general, hash maintained); any other shape (const/readonly/
     * slice/non-array/flat mismatch/undefined global) falls back to the
     * FULL builtin path with the pooled carets - byte-identical errors.
     */
    AppendV,

    /*
     * F1 (plans/vm-performance-roadmap.md): a TYPED math-builtin call -
     * `sqrt(x)`/`sin(x)`/`log(x)`/`float(x)`/`abs(float)`/`pow(x,y)` with a
     * float-proven result and float-compilable arg(s). Reads the operand(s)
     * RAW (read_float_operand - an int operand promotes), calls the C
     * function, writes the raw double via write_float_slot - the whole
     * CallBuiltinV marshal (arg moves into the run, the buffer copy, the
     * ArgLocs, the builtin fn-pointer call, the boxed result store) is
     * deleted. NEVER THROWS: the 1/2-arg float builtins have no domain
     * checks (libm NaN/inf semantics) and their only errors - arity,
     * non-numeric arg - are excluded at compile time (arity gated at
     * selection; the arg PROVEN numeric by inference, tag-checked under
     * VM_HARDENING like every float operand). So the op is loc- AND
     * node-free. Layout: target = dst, target2 = the MathFn selector,
     * a = x, b = y (2-arg only). Selected by try_math_fn (codegen.cpp)
     * ahead of the generic CallBuiltinV lowering.
     */
    MathFnV,

    /*
     * H1 (plans/vm-performance-roadmap.md): the TYPED standalone struct-member
     * READ `p.x` (th==i / th==f, a proven non-opt struct base in a LOCAL slot)
     * - the standalone twin of the flat-array LoadStructField* pair, and the
     * VM analog of the tree-walker's M8 MemberExpr::eval_int/eval_float. The
     * fast path reads a POD field's scalar STRAIGHT from the instance's bytes
     * (slot_of + a raw load - no boxed temp, so the consuming arith stays
     * IntBin/FloatBin instead of a member.v + bin.v boxed chain); any other
     * shape (a boxed struct's field, a dict base, a const member) falls to
     * the shared member_read_core + the write_scalar_slot promotion/type-
     * check, byte-identical to the tree-walker's Construct::eval_int/float
     * fallback. Layout: target = dst, target2 = the base slot, a.lit = the
     * member_keys pool index (uid + carets). AST-free.
     */
    LoadMemberInt, LoadMemberFloat,

    /*
     * E4 FUSIONS (plans/vm-peephole.md) - adjacent-pair superinstructions the
     * peephole's fusion rules synthesize, CHOSEN FROM THE DYNAMIC PAIR
     * PROFILE (760M dispatches over the bench suite; each rule requires the
     * intermediate temp DEAD - the existing liveness - and no branch target
     * on the second op):
     *
     * IntAddModRI - `IntAdd{RR,RI} t = a + b; IntModRI dst = t % imm` (3.4%
     * of all dispatches: the checksum shape `s = (s + x) % M`) fuses to
     * `dst = (a + b) % imm`. NEVER THROWS (the ModRI imm is nonzero by
     * selection; the add wraps) - loc/node-free. The int32 imm rides in
     * `target2` (gated on fitting; 1e9+7 does), operands in a/b.
     *
     * JumpUnlessElemInt - `LoadElemInt t = arr[i]; JumpUnlessTrueV t, L`
     * (1.7%: the sieve test `if (a[i]) ...`) fuses to "load + test + branch"
     * in one dispatch. Keeps the LOAD's node/loc (the OOB caret) - the
     * fused op is written over the load in place, so node_idx rides along.
     * target = L, target2 = the array base slot, a = the index operand.
     */
    IntAddModRI, JumpUnlessElemInt,

    /*
     * The #9 RESIDUAL FUSION BATCH (roadmap; same pair-profile method):
     *
     * IntAddStep - `IntBin(+) s = s + x; ForLoopStep(step imm 1)` (the
     * accumulate-then-step loop tail) in one dispatch. Gated on the
     * accumulator shape (add dst == add a, both slots) and the i++/i--
     * step. NEVER THROWS (the add wraps; the step never throws) -
     * loc/node-free. Encoding: `target` = the loop pc, `target2` = the
     * counter slot, `aop` = the compare, a_dual = (add dst, BOUND: an
     * int32 imm when a_is_lit else a slot - this op's private use of the
     * a-lit bit alongside the dual view), b = the add's rhs operand.
     *
     * ForStepElemInt - `ForLoopStep(step imm 1)` whose branch target is a
     * `LoadElemInt` indexed BY THE COUNTER (the `for i: ... a[i]` back
     * edge): on a taken back edge the element load runs in the SAME
     * dispatch and control lands PAST the original load (target = load pc
     * + 1); the original load stays in place for the loop-entry path. The
     * load's OOB caret rides along (this op's loc = the load's node, like
     * JumpUnlessElemInt). Encoding: `target2` = counter, `aop` = compare,
     * a = the bound operand (as ForLoopStep), b_dual = (array slot, elem
     * dst slot).
     *
     * StructFieldAddInt - `LoadStructFieldInt t = a[i].f; IntBin(+)
     * dst = other + t` (bench 65's reduction - the adds chain through
     * temps, so this is GENERAL 3-address, the accumulator being the
     * special case) fuses to `dst = other + a[i].f` when t is a dead-
     * after temp. The field read is proven no-fault (the struct-foreach
     * guarantee) and the add wraps - loc/node-free. Encoding: `target` =
     * the add's dst, `target2` = the struct-array slot, a = the index
     * operand, b_dual = (field index, the other add operand's slot).
     */
    IntAddStep, ForStepElemInt, StructFieldAddInt,

    /*
     * SENTINEL - the opcode count, never emitted or executed. Backs the
     * computed-goto dispatch table's size/order static checks (see
     * ML_FOR_EACH_OPCODE below and vm.cpp's vm_optbl); disasm handles it
     * with an unreachable case so its -Wswitch exhaustiveness stays intact.
     */
    OpCount_,
};

/*
 * Every opcode, IN ENUM ORDER - the single generation point for the
 * computed-goto dispatch table (vm.cpp builds `vm_optbl` from it, one
 * `&&lbl_<Op>` per entry). KEEP IN EXACT ENUM ORDER when adding an op:
 * `ml_opcode_list_in_order()` below static-asserts every listed name sits
 * at its own enum index and that the list covers the whole enum (OpCount_),
 * so a missing, extra, or misplaced entry is a COMPILE error, never a
 * silently mis-dispatching table.
 */
#define ML_FOR_EACH_OPCODE(X) \
    X(Jump) X(IntBin) X(IncDecCheckedV) X(IncDecElemCheckedV) \
    X(IncDecMemberCheckedV) X(IncDecChainV) \
    X(StoreLValueChainV) X(JumpUnlessIntCmp) X(FloatBin) \
    X(JumpUnlessFloatCmp) X(ForLoopStep) X(LoadElemInt) X(LoadElemFloat) \
    X(LoadElemBool) X(ArrLen) X(StrLen) X(LoadStrChar) X(LoadElemValue) \
    X(LoadStructFieldInt) X(LoadStructFieldFloat) X(LoadStructElemV) \
    X(DictIterInit) X(DictIterNext) X(ForeachDynInit) X(ForeachDynNext) \
    X(UnpackElemInt) X(UnpackElemFloat) X(UnpackElemValue) \
    X(UnpackElemTargets) X(StoreElemInt) X(StoreElemFloat) X(DictStore) \
    X(StoreMemberV) X(StoreElemValue) X(MultiUnpackV) X(StoreElem2V) \
    X(StoreElemChainV) X(DictLoadInt) X(DictLoadFloat) X(CallBuiltinV) \
    X(CallBuiltinLV) X(EmplaceStruct) X(CallBuiltinLVElem) \
    X(CallBuiltinLVMember) X(CallV) X(CachedCallV) X(CallValueV) \
    X(CheckFuncV) X(MapFilterV) X(ReturnV) X(LoadImmInt) X(LoadImmFloat) \
    X(LoadConstV) X(MoveV) X(BinOpV) X(CoerceNumV) X(LoadLiteralObjV) \
    X(CompoundV) X(CmpV) X(LogV) X(UnaryV) X(LoadGlobalV) X(LoadCaptureV) \
    X(LoadBuiltinV) X(DefinedGlobalV) X(StoreGlobalV) X(DeclConstV) \
    X(StoreCaptureV) X(SubscriptV) X(MemberV) X(SliceV) X(MakeArrayV) \
    X(MakeDictV) X(MakeClosureV) X(StructCtorV) X(StructCtorBoxedV) \
    X(ThrowRuntimeV) X(CallValueGenericV) X(CheckCallableV) \
    X(MakeStructArrayV) X(JumpUnlessTrueV) X(JumpIfNotNoneV) X(Throw) \
    X(PushHandler) X(PopHandler) X(CatchTest) X(Reraise) X(Rethrow) \
    X(SetPend) X(EndFinally) X(Halt) \
    X(IntAddRR) X(IntAddRI) X(IntSubRR) X(IntSubRI) X(IntMulRR) \
    X(IntMulRI) X(IntAndRR) X(IntAndRI) X(IntOrRR) X(IntOrRI) \
    X(IntXorRR) X(IntXorRI) X(IntShlRR) X(IntShlRI) X(IntShrRR) \
    X(IntShrRI) X(IntModRI) X(FloatAddRR) X(FloatAddRI) X(FloatSubRR) \
    X(FloatSubRI) X(FloatMulRR) X(FloatMulRI) X(AppendV) X(MathFnV) \
    X(LoadMemberInt) X(LoadMemberFloat) X(IntAddModRI) X(JumpUnlessElemInt) \
    X(IntAddStep) X(ForStepElemInt) X(StructFieldAddInt)

/*
 * MathFnV's function selector (Instr::target2). The names match the builtin
 * names (tofloat_ is the numeric `float(x)` conversion; fabs_ is `abs` on a
 * float-proven operand). All are float(float[,float]) with NO domain throw -
 * a new entry must keep that contract (MathFnV is loc/node-free).
 */
enum class MathFn : int {
    sqrt_, cbrt_, sin_, cos_, tan_, asin_, acos_, atan_,
    exp_, exp2_, log_, log2_, log10_, ceil_, floor_, trunc_,
    tofloat_, fabs_,
    pow_,   /* the only 2-arg entry (Instr::b = y) */
};

namespace ml_opcheck {
#define ML_OPCODE_ENUMV(N) OpCode::N,
constexpr OpCode ml_op_order[] = { ML_FOR_EACH_OPCODE(ML_OPCODE_ENUMV) };
#undef ML_OPCODE_ENUMV
constexpr bool ml_opcode_list_in_order()
{
    constexpr size_t n = sizeof(ml_op_order) / sizeof(ml_op_order[0]);
    if (n != static_cast<size_t>(OpCode::OpCount_))
        return false;
    for (size_t i = 0; i < n; i++)
        if (static_cast<size_t>(ml_op_order[i]) != i)
            return false;
    return true;
}
static_assert(ml_opcode_list_in_order(),
              "ML_FOR_EACH_OPCODE is out of sync with enum OpCode - keep the "
              "list in EXACT enum order (see the comment above it)");
}   /* namespace ml_opcheck */


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

/*
 * B3 (plans/vm-performance-roadmap.md): the PACKED 32-byte instruction -
 * exactly two per cache line (the old shape was 56 bytes: two 16-byte
 * Operands whose 8-byte lit/flit unions forced alignment padding around
 * their 2 tag bytes + 4-byte slot). The packing: `slot` and `lit` are
 * MUTUALLY EXCLUSIVE (is_lit discriminates), so each operand is ONE 8-byte
 * payload; the two tag bits per operand live in the shared `opflags` byte;
 * `aop` is a 1-byte enum. `Operand` (above) SURVIVES as the codegen-side
 * VALUE type (int_lit()/slot_op()/float_lit(), the compile_* plumbing) -
 * an Instr packs one in via set_a/set_b and hands one back via a()/b();
 * the HOT readers use the direct field accessors (a_slot()/a_lit()/...)
 * so no unpacking happens in the dispatch loop.
 */
struct Instr {
    OpCode op;
    Op aop = Op::invalid;   /* IntBin: arith op; JumpUnlessIntCmp: compare op */
    /* a: bit0 = is_lit, bits1-2 = lit_kind; b: bit3 = is_lit, bits4-5. */
    uint8_t opflags = 0;
    int target = -1;    /* Jump dest; IntBin dst
                         * slot; JumpUnlessIntCmp jump dest */
    int target2 = -1;   /* secondary operand (op-specific) */
    /* The packed operand payloads: a frame SLOT (as a small int; -1 = the
     * default "unset" slot, preserving the old default Operand's slot),
     * an int/bool immediate, or a float immediate's bits. */
    int64_t pa = -1;
    int64_t pb = -1;

    bool a_is_lit() const { return opflags & 1; }
    bool b_is_lit() const { return opflags & 8; }
    Operand::LitKind a_kind() const {
        return static_cast<Operand::LitKind>((opflags >> 1) & 3);
    }
    Operand::LitKind b_kind() const {
        return static_cast<Operand::LitKind>((opflags >> 4) & 3);
    }
    int a_slot() const { return static_cast<int>(pa); }
    int b_slot() const { return static_cast<int>(pb); }
    int_type a_lit() const { return static_cast<int_type>(pa); }
    int_type b_lit() const { return static_cast<int_type>(pb); }
    float_type a_flit() const {
        float_type f;
        std::memcpy(&f, &pa, sizeof f);
        return f;
    }
    float_type b_flit() const {
        float_type f;
        std::memcpy(&f, &pb, sizeof f);
        return f;
    }

    /* Unpack a full codegen-side Operand (the cold pass-by-const-ref
     * sites); the hot readers use the field accessors above. */
    Operand a() const {
        Operand o;
        o.is_lit = a_is_lit();
        o.lit_kind = a_kind();
        if (o.is_lit) {
            if (o.lit_kind == Operand::LitKind::f)
                o.flit = a_flit();
            else
                o.lit = a_lit();
        } else {
            o.slot = a_slot();
        }
        return o;
    }
    Operand b() const {
        Operand o;
        o.is_lit = b_is_lit();
        o.lit_kind = b_kind();
        if (o.is_lit) {
            if (o.lit_kind == Operand::LitKind::f)
                o.flit = b_flit();
            else
                o.lit = b_lit();
        } else {
            o.slot = b_slot();
        }
        return o;
    }

    void set_a(const Operand &o) {
        opflags = static_cast<uint8_t>(
            (opflags & ~0x07u)
            | (o.is_lit ? 1u : 0u)
            | (static_cast<unsigned>(o.lit_kind) << 1));
        if (o.is_lit && o.lit_kind == Operand::LitKind::f)
            std::memcpy(&pa, &o.flit, sizeof pa);
        else
            pa = o.is_lit ? o.lit : o.slot;
    }
    void set_b(const Operand &o) {
        opflags = static_cast<uint8_t>(
            (opflags & ~0x38u)
            | (o.is_lit ? 8u : 0u)
            | (static_cast<unsigned>(o.lit_kind) << 4));
        if (o.is_lit && o.lit_kind == Operand::LitKind::f)
            std::memcpy(&pb, &o.flit, sizeof pb);
        else
            pb = o.is_lit ? o.lit : o.slot;
    }
    /*
     * The DUAL form: a few ops (the CallBuiltinLV family incl. AppendV, and
     * the chain stores) used the old 16-byte Operand's `slot` AND `lit` as
     * TWO independent int fields at once (pool idx + arg0-slot kind, chain
     * idx + base kind, ...). The packed payload carries both as int32
     * halves via these DEDICATED accessors - an op uses EITHER the plain
     * a_slot()/a_lit() view OR the dual view, never both (documented per
     * op at its emit site). Both halves are int32-range by construction
     * (pool indices, frame slots, 0-2 kinds).
     */
    int a_dual_lo() const { return static_cast<int>(static_cast<int32_t>(pa)); }
    int a_dual_hi() const { return static_cast<int>(pa >> 32); }
    void set_a_dual(int lo, int hi) {
        pa = static_cast<int64_t>(static_cast<uint32_t>(lo))
             | (static_cast<int64_t>(hi) << 32);
        opflags = static_cast<uint8_t>(opflags & ~0x07u);  /* not a lit */
    }
    int b_dual_lo() const { return static_cast<int>(static_cast<int32_t>(pb)); }
    int b_dual_hi() const { return static_cast<int>(pb >> 32); }
    void set_b_dual(int lo, int hi) {
        pb = static_cast<int64_t>(static_cast<uint32_t>(lo))
             | (static_cast<int64_t>(hi) << 32);
        opflags = static_cast<uint8_t>(opflags & ~0x38u);  /* not a lit */
    }

    void swap_ab() {
        const Operand ta = a(), tb = b();
        set_a(tb);
        set_b(ta);
    }
};

static_assert(sizeof(Instr) == 32,
              "the B3 packed instruction - two per cache line; a new field "
              "must fit the layout above, not grow it");

/*
 * The CODEGEN-ONLY instruction (B3 stage 2): `node_idx` indexes the Codegen
 * object's ast_nodes registry - the splice-stable handle extract_locs uses
 * to harvest an op's error carets into the loc side table (ops vectors are
 * rolled back / spliced / compacted during codegen, so the association must
 * ride INSIDE the element). The runtime `Instr` above cannot hold it AT THE
 * TYPE LEVEL: codegen builds a vector<CgInstr>, extract_locs/verify_ast_free
 * consume it, and codegen_chunk SLICES the Instr sub-objects into
 * Chunk::code - so "no op references an AST node at runtime" is enforced by
 * the type system, not an assert (verify_ast_free still checks the handles
 * were all CONSUMED). Implicitly constructible from Instr so plain emit
 * sites need no change.
 */
struct CgInstr : Instr {
    int32_t node_idx = -1;
    CgInstr() = default;
    CgInstr(const Instr &i) : Instr(i) {}
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
     * body's "inlined-at" chain here, so a backtrace crossing it reconstructs
     * the virtual inlined frames - the tree-walker gets them from
     * `node->inline_ctx` via `Construct::eval`, which native ops have no hook
     * for. SAME shape/cost as `locs`: sorted by pc, read ONLY on the throw path
     * (via inline_ctx_at).
     *
     * SERIALIZABLE (no AST pointer): the `InlineCtx*` chain is FLATTENED into
     * the `inline_frames` POOL (each entry pure data: name + params + call-site
     * loc + a PARENT INDEX into the same pool, -1 == root; a parent always has a
     * lower index), and the side table maps `pc -> a pool index`. `extract_locs`
     * interns each op's chain (deduping shared chains). This is the analogue of
     * the `chain_locs` per-step-loc pool - the last non-`locs` side table off
     * the AST.
     */
    struct InlineFrame {
        std::string callee_name;
        std::vector<std::string> params;
        Loc call_site;
        int32_t parent = -1;   /* index into inline_frames, -1 == root */
    };
    std::vector<InlineFrame> inline_frames;

    struct InlineEntry { uint32_t pc; int32_t frame; };  /* frame = pool index */
    std::vector<InlineEntry> inline_ctxs;

    /* The inline_frames index of the op at `pc`'s chain (exact match; -1 if
     * none). Binary search - O(log n), throw path only. */
    int32_t inline_frame_at(size_t pc) const
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
            return inline_ctxs[lo].frame;
        return -1;
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
     * CHECKED-INC-DEC SITE POOL (IncDecElemCheckedV / IncDecMemberCheckedV).
     * The dual error carets a one-loc-per-pc side table can't hold: `lstart/
     * lend` = the SUBSCRIPT/MEMBER child's caret (a subscript-internal
     * KeyNotFound/OOB, a member KeyNotFound), `istart/iend` = the INC-DEC
     * expr's own caret (its NotLValue / const / TypeError). The member form
     * also carries the member key (`memId` the name as a dict-key value,
     * `memUid` the interned name; elem leaves them empty/null). Indexed by
     * `Instr::b` (an immediate) - O(1), no node, fully serializable (Locs +
     * a string key + a re-internable name).
     */
    struct IncDecSite {
        Loc lstart, lend;              /* the subscript/member child's caret */
        Loc istart, iend;              /* the whole inc-dec expr's caret */
        EvalValue memId;               /* member form: the name as a dict key */
        const UniqueId *memUid = nullptr;   /* member form: interned name */
    };
    std::vector<IncDecSite> incdec_sites;

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
     * INC-DEC CHAIN POOL (IncDecChainV). One entry per general inc-dec value
     * form: the lvalue's steps (same shape as chain_steps - a subscript key is
     * a pre-evaluated frame temp, so a side-effecting index runs once), the
     * TIER (`tier2` = the inferencer proved the lvalue int/float, so the
     * compound-store semantics apply; else the dyn checked semantics), the
     * prefix/postfix flag, the codegen-proven `allow_flat`/`allow_pod` gates
     * (= no_side_effects(final step's base AST) - the tree-walker's
     * try_flat_subscript_store / try_pod_struct_store gate, an AST-shape
     * property, so it is compile-time data), and the carets: `id_*` = the
     * whole inc-dec expr (its NotLValue/const/TypeError in tier 3),
     * `k*` = the final subscript's INDEX node (the "Expected integer as
     * subscript" caret in flat_store_core). Fully serializable (Locs, ints,
     * member_keys indexes, temp regs).
     */
    struct IncDecChain {
        std::vector<ChainStep> steps;  /* inside-out; >= 1 */
        bool tier2 = false;            /* lvalue proven int/float */
        bool is_prefix = false;
        bool allow_flat = false;   /* final subscript may take the flat path */
        bool allow_pod = false;    /* final member may take the POD byte path */
        Loc id_start, id_end;          /* the inc-dec expr's caret */
        Loc kstart, kend;              /* the final subscript index's caret */
    };
    std::vector<IncDecChain> incdec_chains;

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

    /* Function DESCRIPTORS for MakeClosureV (the FuncDescriptor of each
     * `func[..]` expression / named decl) - the serializable runtime identity
     * (funcdesc.h), NOT an AST node. The Instr carries the index. */
    std::vector<const FuncDescriptor *> closure_defs;

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

    /* EMPLACE-STRUCT SITE POOL (EmplaceStruct: `append(struct_arr, Ctor(..))`).
     * Everything vm_emplace_struct needed from the node: the ctor's POD def
     * (AST-owned/program-lifetime, re-bindable by name on load like
     * struct_defs), the CONTAINER arg's caret (the NotLValue / Expected-array
     * / const errors), the ctor's per-FIELD coerce carets, and the callee
     * name (disasm / a serializer). The whole-args caret (the handler's
     * catch) rides the loc side table. Indexed by the kind-packed `Instr::a`
     * (`kind | idx << 2`), so the op is AST-free. */
    struct EmplaceSite {
        const StructTypeDef *def;
        const UniqueId *bname;           /* append / push (the callee name) */
        Loc a0_start, a0_end;            /* the container arg's caret */
        std::vector<ArgLoc> field_locs;  /* the ctor's per-field carets */
    };
    std::vector<EmplaceSite> emplace_sites;

    /* Always-throw sites for ThrowRuntimeV (`Instr::target` indexes this): the
     * runtime exception an always-throwing construct raises, as pure data — the
     * kind, the caret, and (for an undefined name) the interned name. Fully
     * serializable (the `name` re-interns from its string). */
    enum class ThrowKind : unsigned char {
        undefined_var,      /* UndefinedVariableEx(name, loc) */
        not_lvalue,         /* NotLValueEx(loc) */
        rebind_builtin,     /* CannotRebindBuiltinEx(loc) */
        rebind_const,       /* CannotRebindConstEx(loc) */
        bad_args,           /* InvalidNumberOfArgsEx(loc) - a wrong-arity
                             * AST-builtin call that throws BEFORE its args
                             * evaluate (defined(a,b)) */
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

    /* Per-target numeric-coerce kinds for a TYPED MultiUnpackV destructure
     * (`int a; float b; a, b = src` - R5): 0 = none, 1 = int, 2 = float,
     * parallel to the unpack_targets entry. A PLAIN multi-assign coerces each
     * stored element/spread value via coerce_to_decl_type (the widen /
     * dyn-narrowing throw, caret = the Expr14 span from the loc side table);
     * a COMPOUND does NOT coerce (op==assign only, like the single-var
     * store - measured). `Instr::b` (an immediate) indexes this; absent
     * (`!b.is_lit`) == no typed targets. Pure data - serializable. */
    std::vector<std::vector<unsigned char>> unpack_coerce;

    /*
     * BUILTIN-CALL POOL (AST-free builtin dispatch). A value-ABI builtin call
     * (CallBuiltinV) needs only its Builtin (the baked func pointers) and the
     * AST-free ArgLocs data - the whole-args caret, the per-arg carets, and the
     * array-repr hint - all pulled out of the DirectBuiltinCallExpr into a pool
     * entry, so the op is a bare index (`Instr::target2`), holding NO `node`.
     * The Builtin's function pointer is resolve-at-load like struct_defs (a
     * serializer stores the builtin's registered name/id and re-binds on load);
     * everything else is pure data. This is what makes a read-only builtin
     * call fully serializable.
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
     * INDIRECT-CALL SITE POOL (CallValueGenericV, F1 step 2). The ArgLocs
     * data (whole-args caret + per-arg carets + arr_hint) PLUS arg0's LVALUE
     * DESCRIPTOR — the by-ref encoding: how the dispatch re-derives arg0's
     * LValue* when the runtime callee turns out func_lv (a mutating builtin),
     * exactly like CallBuiltinLV/LVElem/LVMember form theirs. `a0_form`:
     *   none   - arg0 is not an lvalue shape (a call/literal/arith - its
     *            VALUE is in run[0]; a func_lv callee gets a null target ->
     *            NotLValueEx, like the tree-walker's non-LValue* raw value);
     *   slot   - a slotted id (a0_kind 0 loc/1 gbl/2 cap/3 builtin; a0_slot);
     *            run[0] is left to the dispatch (an UNDEFINED global
     *            surfaces at the consumer - bind/adapter - as the
     *            tree-walker's raw UndefinedId does);
     *   elem   - base[idx]: a0_kind/a0_slot = the base, a0_operand = the
     *            RESERVED index temp; run[0] holds the element VALUE (filled
     *            by an ordinary SubscriptV at arg0's position - OOB/key
     *            throws in order); the re-derive repeats the subscript
     *            (idempotent; a between-args container mutation makes it
     *            throw where the tree-walker's stale LValue* is UB - safer);
     *   member - base.member: a0_operand = the member_keys pool idx; run[0]
     *            via MemberV;
     *   undef  - an unresolved name (a0_name): value-consumers throw
     *            UndefinedVariableEx at the consumer, func_lv gets null.
     * Pure data + a re-internable name - serializable.
     */
    struct CallSite {
        Loc start, end;                  /* the whole-args caret */
        std::vector<ArgLoc> args;        /* per-arg carets */
        ArrHint arr_hint = ArrHint::dflt;
        enum class A0 : unsigned char { none, slot, elem, member, undef };
        A0 a0_form = A0::none;
        unsigned char a0_kind = 0;       /* 0 loc / 1 gbl / 2 cap / 3 builtin */
        int32_t a0_slot = -1;            /* the id/base slot */
        int32_t a0_operand = -1;         /* elem: index temp; member: key idx */
        const UniqueId *a0_name = nullptr;   /* undef: the name */
    };
    std::vector<CallSite> call_sites;

    ArgLocs call_arglocs_at(int idx) const
    {
        const CallSite &cs = call_sites[static_cast<size_t>(idx)];
        ArgLocs al;
        al.start = cs.start;
        al.end = cs.end;
        al.args = cs.args.data();
        al.nargs = cs.args.size();
        al.arr_hint = cs.arr_hint;
        return al;
    }

};
