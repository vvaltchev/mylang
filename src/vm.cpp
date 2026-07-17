/* SPDX-License-Identifier: BSD-2-Clause */

#include "vm.h"
#include "codegen.h"
#include "inferencer.h"
#include "bytecode.h"
#include "syntax.h"
#include "eval.h"
#include "errors.h"
#include "backtrace.h"   /* flush_inline_frames (Inc 4 backtrace parity) */
#include "bitops.h"
#include "env.h"    /* env_get - the MYLANG_VM_STACK cap */

#include <memory>
#include <cstdlib>   /* atoll */
#include <cmath>
#include <unordered_map>

/* The harness's engine switch (see vm.h). Default: the tree-walker. */
ExecEngine g_exec_engine = ExecEngine::TreeWalk;

/* Read a register operand as an int: an immediate, or a frame slot's int (a
 * bool slot reads as 0/1, mirroring Identifier::eval_int). The register machine
 * uses the frame slots directly - no value stack. */
static ML_ALWAYS_INLINE int_type
read_int_slot(EvalContext *ctx, int_type slot)
{
    const LValue &lv = ctx->frame->at(slot);
    if (lv.is<bool>())
        return lv.getval<bool>() ? 1 : 0;
    /* A th==i operand must hold an int here; anything else is an inference bug
     * or a corrupt/garbage slot (its type ptr won't match int) - caught before
     * the raw getval misreads the union. */
    ML_VM_CHECK(lv.is<int_type>());
    return lv.getval<int_type>();
}

static ML_ALWAYS_INLINE int_type
read_int_operand(const Operand &o, EvalContext *ctx)
{
    if (o.is_lit)
        return o.lit;
    return read_int_slot(ctx, o.slot);
}

/* Write an int result into a frame slot: overwrite the int in place when the
 * slot already holds one (the common, hot case), else set value + int type. */
static ML_ALWAYS_INLINE void
write_int_slot(EvalContext *ctx, int_type slot, int_type v)
{
    LValue &lv = ctx->frame->at(slot);
    if (lv.is<int_type>())
        lv.getval<int_type>() = v;
    else
        lv.put(EvalValue(v));
}

/* The float analogues: read an operand as float (an int/bool slot promotes,
 * mirroring Identifier::eval_float) and write a float result into a slot. */
static ML_ALWAYS_INLINE float_type
read_float_slot(EvalContext *ctx, int_type slot)
{
    const LValue &lv = ctx->frame->at(slot);
    if (lv.is<int_type>())
        return static_cast<float_type>(lv.getval<int_type>());
    if (lv.is<bool>())
        return lv.getval<bool>() ? 1.0 : 0.0;
    /* Likewise a th==f operand must hold a float here (int/bool promoted
     * above); a wrong-typed / garbage slot fails before the raw union read. */
    ML_VM_CHECK(lv.is<float_type>());
    return lv.getval<float_type>();
}

static ML_ALWAYS_INLINE float_type
read_float_operand(const Operand &o, EvalContext *ctx)
{
    if (o.is_lit)
        return o.flit;
    return read_float_slot(ctx, o.slot);
}

static ML_ALWAYS_INLINE void
write_float_slot(EvalContext *ctx, int_type slot, float_type v)
{
    LValue &lv = ctx->frame->at(slot);
    if (lv.is<float_type>())
        lv.getval<float_type>() = v;
    else
        lv.put(EvalValue(v));
}

/*
 * F1 - MathFnV's out-of-line body (loop-body text rule: the libm call
 * dominates, so the selector switch lives in its own frame, not in
 * vm_run_chunk's). Never throws: every entry is float(float[,float]) with
 * libm NaN/inf semantics (no domain checks - matching float_func's builtins
 * exactly), and the operands are inference-proven numeric.
 */
ML_NOINLINE static float_type
vm_math_fn(const Instr &in, EvalContext *ctx)
{
    const float_type x = read_float_operand(in.a(), ctx);
    switch (static_cast<MathFn>(in.target2)) {
    case MathFn::sqrt_:    return std::sqrt(x);
    case MathFn::cbrt_:    return std::cbrt(x);
    case MathFn::sin_:     return std::sin(x);
    case MathFn::cos_:     return std::cos(x);
    case MathFn::tan_:     return std::tan(x);
    case MathFn::asin_:    return std::asin(x);
    case MathFn::acos_:    return std::acos(x);
    case MathFn::atan_:    return std::atan(x);
    case MathFn::exp_:     return std::exp(x);
    case MathFn::exp2_:    return std::exp2(x);
    case MathFn::log_:     return std::log(x);
    case MathFn::log2_:    return std::log2(x);
    case MathFn::log10_:   return std::log10(x);
    case MathFn::ceil_:    return std::ceil(x);
    case MathFn::floor_:   return std::floor(x);
    case MathFn::trunc_:   return std::trunc(x);
    case MathFn::tofloat_: return x;   /* float(x): the read already widened */
    case MathFn::fabs_:    return std::fabs(x);
    case MathFn::pow_:     return std::pow(x, read_float_operand(in.b(), ctx));
    }
    ML_CHECK(false);       /* a selector the codegen never emits */
    return 0.0;
}

/* Write a boxed scalar `v` into a slot as an int OR float, with the same
 * int/bool -> promotion the tree-walker's eval_int/eval_float dict reads use.
 * Shared by DictLoadInt/Float's present-key and missing-key (default) paths. */
static ML_ALWAYS_INLINE void
write_scalar_slot(EvalContext *ctx, int_type slot, bool is_int,
                  const EvalValue &v)
{
    if (is_int)
        write_int_slot(ctx, slot,
            v.is<bool>() ? (v.get<bool>() ? 1 : 0) : v.get<int_type>());
    else
        write_float_slot(ctx, slot,
            v.is<int_type>() ? static_cast<float_type>(v.get<int_type>())
          : v.is<bool>()     ? (v.get<bool>() ? 1.0 : 0.0)
                             : v.get<float_type>());
}

/* Read a BOXED operand as an EvalValue: a slot returns a const ref (no copy);
 * an immediate materializes into `scratch` (by its lit_kind) and returns a ref
 * to it - so a boxed op (BinOpV/CmpV/LogV/CompoundV) takes an int/float/bool
 * immediate directly, with no LoadConstV first. */
static ML_ALWAYS_INLINE const EvalValue &
boxed_operand(const Operand &o, EvalContext *ctx, EvalValue &scratch)
{
    if (!o.is_lit)
        return ctx->frame->at(o.slot).get();
    switch (o.lit_kind) {
    case Operand::LitKind::f:
        scratch = EvalValue(o.flit);
        break;
    case Operand::LitKind::b:
        scratch = EvalValue(static_cast<bool>(o.lit));
        break;
    default:
        scratch = EvalValue(o.lit);
        break;
    }
    return scratch;
}

/* Stamp an unlocated in-flight exception with the op's caret from the loc side
 * table (used by the catch of an AST-free op whose runtime function threw with
 * no loc). Cold - the error path only. */
static ML_COLD void
vm_stamp_loc(const Chunk &chunk, size_t pc, Exception &e)
{
    if (!e.loc_start)
        chunk.loc_at(pc, e.loc_start, e.loc_end);
}

/* Resolve a container-store op's BASE LValue* by slot KIND (0 local / 1 global /
 * 2 capture), so `a[i]=v` / `d[k]=v` / `s.f=v` can target a top-level container
 * a function reads (global) or a captured one, not only a frame local. An
 * undefined global throws UndefinedVariableEx (name from gfuncs, caret from the
 * loc side table) - matching the tree-walker's base-read of an undefined name.
 * Cold on the global-undefined path only; the common local/global paths inline.
 */
/* `kind` is int_type, not int: two callers pass an Operand's int_type `lit`
 * (StoreElemChainV / StoreLValueChainV) - a widening param keeps every call
 * warning-free on all three compilers (MSVC C4244). */
static LValue *
vm_store_base(EvalContext &ctx, int_type kind, int slot,
              const Chunk &chunk, size_t pc, const Construct *node)
{
    if (kind == 1) {                            /* global */
        if (!ctx.gfuncs->defined[slot]) {
            Loc s, en;
            if (node) { s = node->start; en = node->end; }
            else chunk.loc_at(pc, s, en);       /* AST-free op: side table */
            throw UndefinedVariableEx(ctx.gfuncs->names[slot]->val, s, en);
        }
        return &ctx.gfuncs->slots[slot];
    }
    if (kind == 2)                              /* capture */
        return &(*ctx.captures)[slot];
    return &ctx.frame->at(slot);                /* local */
}

/* Map a flat-store's BASE arith op (Op::invalid = plain, else plus/minus/…) to
 * the Expr14 op vm_subscript_store expects (Op::assign / addeq / …) - the
 * StoreElem* universal fallback boxes its operands and dispatches through the
 * shared store, which uses the Expr14 op convention (like DictStore). */
static inline Op vm_base_to_expr14_op(Op base)
{
    switch (base) {
    case Op::invalid: return Op::assign;
    case Op::plus:    return Op::addeq;
    case Op::minus:   return Op::subeq;
    case Op::times:   return Op::muleq;
    case Op::div:     return Op::diveq;
    case Op::mod:     return Op::modeq;
    default:          return Op::assign;
    }
}

/* The two STRICT foreach-unpack errors (UnpackElem*), matching do_iter's
 * messages + loc (the container's, from the loc side table) BYTE-for-byte, so
 * differential agrees. Cold [[noreturn]] helpers, out of the hot loop. */
[[noreturn]] static ML_COLD void
vm_throw_unpack_nonarray(const Chunk &chunk, size_t pc, int_type nvars)
{
    Loc s, en;
    chunk.loc_at(pc, s, en);
    throw TypeErrorEx(intern_msg("foreach: cannot unpack a non-array element "
                                 "into " + std::to_string(nvars) +
                                 " variables"), s, en);
}

[[noreturn]] static ML_COLD void
vm_throw_unpack_len(const Chunk &chunk, size_t pc, size_type m, int_type nvars)
{
    Loc s, en;
    chunk.loc_at(pc, s, en);
    throw TypeErrorEx(intern_msg("foreach: cannot unpack an array of length " +
                                 std::to_string(m) + " into " +
                                 std::to_string(nvars) + " variables"), s, en);
}

/* Cold out-of-bounds / div-by-zero throws (loc from the side table). Kept as
 * [[noreturn]] ML_NOINLINE helpers so the STORE/LOAD hot paths carry no loc_at
 * (a binary search) and no throw/string code inline - only a call on the cold
 * error path, which also keeps vm_run_chunk's hot body compact. */
[[noreturn]] static ML_COLD void
vm_throw_oob(const Chunk &chunk, size_t pc)
{
    Loc s, en;
    chunk.loc_at(pc, s, en);
    throw OutOfBoundsEx(s, en);
}

[[noreturn]] static ML_COLD void
vm_throw_div0(const Chunk &chunk, size_t pc)
{
    Loc s, en;
    chunk.loc_at(pc, s, en);
    throw DivisionByZeroEx(s, en);
}

/* The MULTI-ASSIGN strict-unpack length error (F-1) - the same message the
 * tree-walker's handle_single_expr14 throws, WITHOUT the "foreach:" prefix. */
[[noreturn]] static ML_COLD void
vm_throw_multi_unpack_len(const Chunk &chunk, size_t pc, size_type m,
                          size_t nvars)
{
    Loc s, en;
    chunk.loc_at(pc, s, en);
    throw TypeErrorEx(intern_msg("cannot unpack an array of length " +
                                 std::to_string(m) + " into " +
                                 std::to_string(nvars) + " variables"), s, en);
}

/* Cold path for a value-ABI builtin with > 8 args: heap-allocate the arg
 * buffer. ML_NOINLINE so the hot CallBuiltinV case (the common n <= 8, a stack
 * buffer) carries NO std::vector ctor/dtor - that overhead, paid on every
 * CallBuiltinV, measurably regressed a builtin-heavy loop. Also keeps the
 * vector code out of vm_run_chunk, so it stays small enough to inline the hot
 * int/float operand helpers. */
static ML_COLD EvalValue
vm_call_builtin_big(EvalContext &ctx, const Chunk &chunk, int bc_idx,
                    int_type base, int_type n)
{
    const Chunk::BuiltinCall &bc = chunk.builtin_calls[bc_idx];
    std::vector<EvalValue> heapbuf(static_cast<size_t>(n));
    for (int_type i = 0; i < n; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    ArgLocs al = chunk.arglocs_at(bc_idx);
    return bc.builtin.func_v(&ctx, &al, heapbuf.data(), n);
}

/* Build an array LITERAL from the register run [base,base+n). ML_NOINLINE and
 * off vm_run_chunk's body ON PURPOSE: the element buffer (a stack buffer for the
 * common small n, a heap one past 16) must NOT live in vm_run_chunk's frame -
 * that frame is MULTIPLIED by VM recursion depth (each recursive call is a
 * vm_run_chunk frame), and its scoped case-block locals do NOT overlap on every
 * toolchain (MSVC Debug), so a big inline buffer there overflows a deep
 * recursion on a small (1MB Windows) stack. The buffer lives here instead. */
static ML_NOINLINE EvalValue
vm_make_array(EvalContext &ctx, int_type base, int_type n, ArrHint hint)
{
    if (n <= 16) {
        EvalValue stackbuf[16];
        for (int_type i = 0; i < n; i++)
            stackbuf[i] = ctx.frame->at(base + i).get();
        return build_array_from_values(stackbuf, n, hint, nullptr, false);
    }
    std::vector<EvalValue> heapbuf(static_cast<size_t>(n));
    for (int_type i = 0; i < n; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    return build_array_from_values(heapbuf.data(), n, hint, nullptr, false);
}

/* Build a dict LITERAL from the interleaved key/value run [base, base+2*npairs).
 * ML_NOINLINE + off vm_run_chunk's frame for the same recursion-stack reason as
 * vm_make_array (the key/value buffer must not inflate the recursive frame). */
static ML_NOINLINE EvalValue
vm_make_dict(EvalContext &ctx, int_type base, int_type npairs)
{
    if (npairs <= 8) {
        EvalValue stackbuf[16];
        for (int_type i = 0; i < 2 * npairs; i++)
            stackbuf[i] = ctx.frame->at(base + i).get();
        return build_dict_from_pairs(stackbuf, npairs, false);
    }
    std::vector<EvalValue> heapbuf(static_cast<size_t>(2 * npairs));
    for (int_type i = 0; i < 2 * npairs; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    return build_dict_from_pairs(heapbuf.data(), npairs, false);
}

/* Construct a POD struct P(x,y) from its field-arg run, INTO the dst slot.
 * ML_NOINLINE + off vm_run_chunk's frame for the recursion-stack reason above;
 * the caller wraps it in the defensive loc-stamp try/catch (the throw
 * propagates out of here).
 *
 * H1 (plans/vm-performance-roadmap.md) - DST-SLOT REUSE: the hot standalone-
 * construction shape is `var p = Point(...)` in a loop, where the dst slot
 * still holds LAST iteration's same-def POD instance and nothing else does
 * (`use_count() == 1` - the slot's handle is the only owner; readonly
 * excluded). Overwrite ITS bytes instead of paying the two heap allocations
 * (the StructObject + its `bytes` vector) plus the two frees per
 * construction - the same overwrite-in-place + COW-guard trick the flat-
 * struct-array foreach uses (do_iter). An aliased (`var q = p;`), captured,
 * const, other-def, or non-struct dst takes the fresh-allocation path, so a
 * held instance is never mutated. The fields are coerced into a stack
 * buffer BEFORE dst is touched (a defensive coerce throw leaves the old
 * value intact - construct_struct_from_values orders the same way). */
static ML_NOINLINE void
vm_struct_ctor(EvalContext &ctx, StructTypeDef *def, int_type base,
               int_type nf, int_type dst)
{
    const auto build = [&](const EvalValue *vals) {
        LValue &d = ctx.frame->at(dst);
        const EvalValue &cur = d.get();
        if (cur.is<intrusive_ptr<StructObject>>()) {
            const intrusive_ptr<StructObject> &so =
                cur.get<intrusive_ptr<StructObject>>();
            if (so->def == def && !so->readonly && so.use_count() == 1) {
                char *bytes = so->bytes.data();
                for (int_type i = 0; i < nf; i++)
                    pod_store_field(def->fields[static_cast<size_t>(i)],
                                    bytes, vals[i]);
                return;                    /* dst already holds the struct */
            }
        }
        /* Fresh path: the values are ALREADY coerced (above), so store the
         * bytes directly - construct_struct_from_values would coerce again. */
        auto obj = make_intrusive<StructObject>(def);
        char *bytes = obj->bytes.data();
        for (int_type i = 0; i < nf; i++)
            pod_store_field(def->fields[static_cast<size_t>(i)],
                            bytes, vals[i]);
        d.put(EvalValue(intrusive_ptr<StructObject>(obj)));
    };

    if (nf <= 16) {
        EvalValue vals[16];
        for (int_type i = 0; i < nf; i++)
            vals[i] = coerce_struct_field(def->fields[static_cast<size_t>(i)],
                                          ctx.frame->at(base + i).get(),
                                          Loc(), Loc());
        build(vals);
        return;
    }
    std::vector<EvalValue> heapbuf(static_cast<size_t>(nf));
    for (int_type i = 0; i < nf; i++)
        heapbuf[i] = coerce_struct_field(def->fields[static_cast<size_t>(i)],
                                         ctx.frame->at(base + i).get(),
                                         Loc(), Loc());
    build(heapbuf.data());
}

/* Construct a BOXED struct `B(a, x)` from its field-arg run [base, base+nargs).
 * ML_NOINLINE + off vm_run_chunk's frame (recursion-stack reason). Unlike the
 * POD ctor, a field coerce CAN throw - the caller passes `locs` so the throw
 * reports the offending arg's caret (byte-identical to construct_struct). */
static ML_NOINLINE EvalValue
vm_struct_ctor_boxed(EvalContext &ctx, StructTypeDef *def, int_type base,
                     int_type nargs, const ArgLoc *locs)
{
    if (nargs <= 16) {
        EvalValue vals[16];
        for (int_type i = 0; i < nargs; i++)
            vals[i] = ctx.frame->at(base + i).get();
        return EvalValue(construct_struct_boxed_from_values(
            def, vals, static_cast<size_t>(nargs), locs));
    }
    std::vector<EvalValue> heapbuf(static_cast<size_t>(nargs));
    for (int_type i = 0; i < nargs; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    return EvalValue(construct_struct_boxed_from_values(
        def, heapbuf.data(), static_cast<size_t>(nargs), locs));
}

/* GENERIC N-level nested store (StoreElemChainV): gather the `nkeys` key values
 * from the run [kbase, kbase+nkeys) and call vm_subscript_chain_store.
 * ML_NOINLINE + off vm_run_chunk's frame (the key buffer must not inflate the
 * recursive frame). The caller wraps the loc-stamp. */
static ML_NOINLINE void
vm_chain_store_op(EvalContext &ctx, LValue *base, int_type kbase,
                  const std::pair<Loc, Loc> *steplocs, size_t nkeys,
                  const EvalValue &val, Op op)
{
    if (nkeys <= 8) {
        EvalValue keybuf[8];
        for (size_t k = 0; k < nkeys; k++)
            keybuf[k] = ctx.frame->at(kbase + static_cast<int_type>(k)).get();
        vm_subscript_chain_store(base, keybuf, nkeys, val, op, steplocs);
        return;
    }
    std::vector<EvalValue> keyheap(nkeys);
    for (size_t k = 0; k < nkeys; k++)
        keyheap[k] = ctx.frame->at(kbase + static_cast<int_type>(k)).get();
    vm_subscript_chain_store(base, keyheap.data(), nkeys, val, op, steplocs);
}

/* GENERAL nested lvalue-chain store (StoreLValueChainV): walk the mixed
 * member/subscript steps (chunk.chain_steps[steps_idx]) from `base`, reading
 * each intermediate step as an lvalue REF (for_write=false), then store the
 * final step (member: vm_member_store; subscript: vm_subscript_store). All
 * throws are loc-less (Loc()) and the CALLER stamps the outer lvalue loc, like
 * StoreElemChainV. ML_NOINLINE + off vm_run_chunk's frame (the recursion-stack
 * hygiene reason above). */
/* The INTERMEDIATE-step walk shared by StoreLValueChainV and IncDecChainV:
 * advance `cur` through steps[0 .. upto). `cur` is EITHER an LValue* wrapper
 * (a mutable ref into a live container) OR a plain VALUE - exactly like the
 * tree-walker's chained do_eval, where an immutable step (a POD field, a
 * readonly instance) yields a value read and the walk CONTINUES on it, only
 * failing NotLValue at the FINAL step. A throw AT a step (OOB / KeyNotFound /
 * TypeError) carries THAT step's node loc - byte-identical to the
 * tree-walker's per-node stamp (an internal throw is loc-less). */
static void
vm_chain_walk(EvalContext &ctx, const Chunk &chunk, EvalValue &cur,
              const std::vector<Chunk::ChainStep> &steps, size_t upto)
{
    for (size_t i = 0; i < upto; i++) {
        const Chunk::ChainStep &step = steps[i];
        try {
            if (step.is_member) {
                const Chunk::MemberKey &mk = chunk.member_keys[step.operand];
                const EvalValue cval =
                    cur.is<LValue *>() ? cur.get<LValue *>()->get() : cur;
                /* for_write=false: an intermediate member is READ to walk into.
                 * A mutable boxed field / dict value -> an lvalue REF; else the
                 * tree-walker reads a VALUE (member_read: a POD/readonly copy,
                 * or a TypeError for a non-struct/dict base) and continues. */
                LValue *next = vm_member_lvalue_ref(cval, mk.memId, mk.memUid,
                    /*for_write=*/false, step.lstart, step.lend);
                if (next)
                    cur = EvalValue(next);
                else
                    cur = member_read_core(cval, mk.memId, mk.memUid,
                        mk.optional, step.lstart, step.lend,
                        step.lstart, step.lend);
            } else {
                const EvalValue &key = ctx.frame->at(step.operand).get();
                Type *ct = cur.is<LValue *>()
                    ? cur.get<LValue *>()->get().get_type() : cur.get_type();
                cur = ct->subscript(cur, key, /*for_write=*/false);
            }
        } catch (Exception &ex) {
            if (!ex.loc_start) { ex.loc_start = step.lstart;
                                 ex.loc_end = step.lend; }
            throw;
        }
    }
}

static ML_NOINLINE void
vm_chain_lvalue_store_op(EvalContext &ctx, const Chunk &chunk, LValue *base,
                         int_type steps_idx, const EvalValue &value, Op op)
{
    const std::vector<Chunk::ChainStep> &steps = chunk.chain_steps[steps_idx];
    const size_t n = steps.size();
    EvalValue cur = EvalValue(base);
    vm_chain_walk(ctx, chunk, cur, steps, n - 1);

    const Chunk::ChainStep &last = steps[n - 1];   /* node = the whole lvalue */
    try {
        /* The final store needs a live lvalue; a VALUE-walked chain (a POD /
         * readonly intermediate) can't be stored into -> NotLValueEx at the
         * whole-lvalue loc, exactly as the tree-walker's handle_single_expr14. */
        if (!cur.is<LValue *>())
            throw NotLValueEx(last.lstart, last.lend);
        LValue *curlv = cur.get<LValue *>();
        if (last.is_member) {
            const Chunk::MemberKey &mk = chunk.member_keys[last.operand];
            /* A DICT member store `d.f = v` IS `d["f"] = v` (auto-vivify on a
             * plain assign; KeyNotFound on a compound of a missing key) - route
             * it through vm_subscript_store with the member name as the key, as
             * the tree-walker's DictStore does. A STRUCT member (POD byte /
             * boxed field) goes through vm_member_store. */
            if (curlv->get().is<intrusive_ptr<DictObject>>())
                vm_subscript_store(curlv, mk.memId, value, op,
                                   last.lstart, last.lend);
            else
                vm_member_store(curlv, mk.memUid, op, value,
                                last.lstart, last.lend, last.lstart, last.lend);
        } else {
            const EvalValue &key = ctx.frame->at(last.operand).get();
            vm_subscript_store(curlv, key, value, op, last.lstart, last.lend);
        }
    } catch (Exception &ex) {
        if (!ex.loc_start) { ex.loc_start = last.lstart; ex.loc_end = last.lend; }
        throw;
    }
}

/* IncDecChainV (the R4 value form): root -> intermediate walk -> the final
 * step's exact IncDecExpr tier semantics (vm_incdec_final, eval.cpp). A kind-3
 * root seeds the walk with a VALUE (a compiled rvalue base keeps its
 * rvalue-ness); the result (old for postfix / new for prefix) lands in `dst`
 * (-1 = statement, discarded). ML_NOINLINE: off vm_run_chunk's recursive
 * frame (the recursion-stack hygiene rule). */
static ML_NOINLINE void
vm_incdec_chain_op(EvalContext &ctx, const Chunk &chunk, const Instr &in,
                   size_t pc)
{
    const Chunk::IncDecChain &site = chunk.incdec_chains[in.b_lit()];
    const std::vector<Chunk::ChainStep> &steps = site.steps;
    const size_t n = steps.size();

    EvalValue cur;
    if (in.a_lit() == 3)
        cur = ctx.frame->at(in.target2).get();      /* rvalue root: a VALUE */
    else
        cur = EvalValue(
            vm_store_base(ctx, in.a_lit(), in.target2, chunk, pc, nullptr));

    vm_chain_walk(ctx, chunk, cur, steps, n - 1);

    const Chunk::ChainStep &last = steps[n - 1];
    EvalValue memId;
    const UniqueId *memUid = nullptr;
    EvalValue key;
    if (last.is_member) {
        const Chunk::MemberKey &mk = chunk.member_keys[last.operand];
        memId = mk.memId;
        memUid = mk.memUid;
    } else {
        key = ctx.frame->at(last.operand).get();
    }

    EvalValue r = vm_incdec_final(cur, last.is_member, memId, memUid, key,
                                  site.tier2, in.aop == Op::plus,
                                  site.is_prefix,
                                  site.allow_flat, site.allow_pod,
                                  last.lstart, last.lend,
                                  site.kstart, site.kend,
                                  site.id_start, site.id_end);
    if (in.target >= 0)
        ctx.frame->at(in.target).put(std::move(r));
}

/* Build a FLAT array<PodStruct> literal from the N structs' interleaved
 * field-arg run [base, base+N*M). ML_NOINLINE + off vm_run_chunk's frame for the
 * recursion-stack reason above (the fused op's field-value buffer must not
 * inflate the recursive frame). The caller wraps the defensive loc-stamp. */
static ML_NOINLINE void
vm_make_struct_array_op(EvalContext &ctx, StructTypeDef *def, int_type base,
                        int_type n, int_type dst)
{
    const size_t total = static_cast<size_t>(n) * def->fields.size();

    const auto build = [&](const EvalValue *vals) {
        LValue &d = ctx.frame->at(dst);
        const EvalValue &cur = d.get();
        /* Top-10 #5 (the H1 dst-reuse trick for a CONTAINER literal): a
         * loop-carried `pts = [P(..), P(..)]` leaves LAST iteration's
         * same-def, same-count flat struct array in the dst slot with the
         * slot as its only owner (use_count() == 1 covers aliases AND live
         * slices - a slice holds its own handle) - overwrite its BYTES
         * instead of allocating a fresh SharedObject + buffer and freeing
         * the old pair (13%+ of 77_struct_array_lit was malloc/free).
         * Representation is DETERMINISTIC here (the op's gate proves one
         * POD def and a fixed count), so reuse cannot diverge from what a
         * fresh build would produce; struct arrays are not hash-cached.
         * The fields coerce into the caller's value buffer BEFORE dst is
         * touched (coerce is defensively throwing). */
        if (cur.is<SharedArrayObj>()) {
            const SharedArrayObj &arr = cur.get_ref<SharedArrayObj>();
            if (arr.skind() == SharedArrayObj::Storage::structs
                && !arr.is_slice()
                && arr.use_count() == 1
                && !arr.is_readonly()
                && arr.flat_structs().def == def
                && arr.size() == static_cast<size_type>(n)) {
                const size_t M = def->fields.size();
                const int stride = def->size;
                SharedArrayObj &ma = d.getval<SharedArrayObj>();
                char *out = ma.flat_structs().buf.data();
                for (size_t i = 0; i < static_cast<size_t>(n); i++) {
                    char *b = out + i * static_cast<size_t>(stride);
                    for (size_t j = 0; j < M; j++)
                        pod_store_field(def->fields[j], b,
                            coerce_struct_field(def->fields[j],
                                                vals[i * M + j],
                                                Loc(), Loc()));
                }
                return;
            }
        }
        d.put(vm_make_struct_array(def, static_cast<size_t>(n), vals));
    };

    if (total <= 32) {
        EvalValue stackbuf[32];
        for (size_t k = 0; k < total; k++)
            stackbuf[k] = ctx.frame->at(base + static_cast<int_type>(k)).get();
        build(stackbuf);
        return;
    }
    std::vector<EvalValue> heapbuf(total);
    for (size_t k = 0; k < total; k++)
        heapbuf[k] = ctx.frame->at(base + static_cast<int_type>(k)).get();
    build(heapbuf.data());
}

/* Cold helper for a REST-NATIVE mutating builtin (insert/erase, Phase 2a): copy
 * the `rest` args - the TAIL ARGS BY VALUE (args 1..n, everything after the arg0
 * lvalue) - from the register run [base, base+n_rest) into a buffer and call
 * func_lv with `target` + `rest`, zero node->eval. ML_NOINLINE keeps the (cold)
 * CallBuiltinLV case out of vm_run_chunk's hot body. n_rest is small for a valid
 * call (insert 2, erase 1); >8 (a wrong-arity call) heaps. */
static ML_COLD EvalValue
vm_call_builtin_lv_rest(EvalContext &ctx, const Chunk &chunk, int bc_idx,
                        LValue *target, int_type base)
{
    const Chunk::BuiltinCall &bc = chunk.builtin_calls[bc_idx];
    const int_type n_rest = static_cast<int_type>(bc.args.size()) - 1;
    ArgLocs al = chunk.arglocs_at(bc_idx);
    if (n_rest <= 8) {
        EvalValue stackbuf[8];
        for (int_type i = 0; i < n_rest; i++)
            stackbuf[i] = ctx.frame->at(base + i).get();
        return bc.builtin.func_lv(&ctx, &al, target, stackbuf,
                                  static_cast<size_t>(n_rest));
    }
    std::vector<EvalValue> heapbuf(static_cast<size_t>(n_rest));
    for (int_type i = 0; i < n_rest; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    return bc.builtin.func_lv(&ctx, &al, target, heapbuf.data(),
                              static_cast<size_t>(n_rest));
}

/* Cold helper for EmplaceStruct (Phase 2b): copy the ctor's field arg values
 * from the register run [base, base+nf) and give them to vm_emplace_struct.
 * AST-FREE: the ctor def + the container/field carets come from the
 * emplace_sites pool entry. ML_NOINLINE keeps this (cold) path out of
 * vm_run_chunk's hot body; a struct with >8 fields heaps. */
static ML_COLD EvalValue
vm_do_emplace(EvalContext &ctx, const Chunk::EmplaceSite &site,
              LValue *target, int_type base)
{
    const size_t nf = site.field_locs.size();
    if (nf <= 8) {
        EvalValue stackbuf[8];
        for (size_t i = 0; i < nf; i++)
            stackbuf[i] = ctx.frame->at(base + i).get();
        return vm_emplace_struct(&ctx, target, site.a0_start, site.a0_end,
                                 site.def, site.field_locs.data(),
                                 stackbuf, nf);
    }
    std::vector<EvalValue> heapbuf(nf);
    for (size_t i = 0; i < nf; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    return vm_emplace_struct(&ctx, target, site.a0_start, site.a0_end,
                             site.def, site.field_locs.data(),
                             heapbuf.data(), nf);
}

/* Map an arith/bitwise Op to its num_bin_op Type method, for the boxed BinOpV
 * (the same PMFs the tree-walker's binary-op eval uses). */
static NumBinOp binop_pmf(Op op)
{
    switch (op) {
        case Op::plus:  return &Type::add;
        case Op::minus: return &Type::sub;
        case Op::times: return &Type::mul;
        case Op::div:   return &Type::div;
        case Op::mod:   return &Type::mod;
        case Op::band:  return &Type::band;
        case Op::bor:   return &Type::bor;
        case Op::bxor:  return &Type::bxor;
        case Op::shl:   return &Type::shl;
        case Op::shr:   return &Type::shr;
        case Op::ushr:  return &Type::ushr;
        default:        return nullptr;
    }
}

/* The comparison Type method for the boxed CmpV (Expr06/Expr07's PMFs). */
static NumBinOp cmp_pmf(Op op)
{
    switch (op) {
        case Op::lt:    return &Type::lt;
        case Op::gt:    return &Type::gt;
        case Op::le:    return &Type::le;
        case Op::ge:    return &Type::ge;
        case Op::eq:    return &Type::eq;
        case Op::noteq: return &Type::noteq;
        default:        return nullptr;
    }
}

/*
 * True ONLY while vm_execute is running the (final) program AST. The VM's
 * function-body hook in do_func_call compiles a chunk (raw Construct* node
 * pointers) lazily on first call; that is safe only once the AST is final. A
 * compile-time fold (AutoConst / the inliner's refold, in resolve_names) that
 * reached the hook while the tree is still being mutated would cache pointers
 * into nodes the optimizer then frees -> a use-after-free (a real, rare
 * RECYCLE/ASan crash before do_func_call's `!ctx->in_const_eval()` gate). This
 * flag makes `vm_func_chunk` ASSERT it is only ever entered during execution,
 * so any future fold path that slips past that gate fails LOUDLY here instead
 * of corrupting memory. See defs.h ML_VM_CHECK / CLAUDE.md.
 */

/*
 * DISPATCH MODE - computed-goto (direct-threaded) vs switch.
 *
 * GCC/clang builds dispatch vm_run_chunk via the "labels as values"
 * extension: a static table of code-label addresses (`vm_optbl`, generated
 * in enum order from ML_FOR_EACH_OPCODE - order/coverage static-asserted in
 * bytecode.h) and a `goto *tbl[op]` at the TAIL OF EVERY HANDLER (VM_NEXT).
 * That removes the switch's bounds check and - the real win - gives each
 * handler its OWN indirect branch, so the BTB predicts per-op-PAIR
 * transitions instead of one 92-target mega-hub (whose mispredicts grew
 * +33% on cachegrind as ops were added - see plans/vm-performance-roadmap
 * A1/A2 and the falsified layout experiments recorded there). MSVC (no
 * computed goto) and a `make CGOTO=0` build keep the original switch: the
 * SAME handler bodies compile either way through VM_CASE/VM_NEXT, so there
 * is exactly one copy of the VM's semantics.
 *
 * VM_NEXT re-reads `in` and dispatches (cgoto) or `continue`s to the loop
 * top (switch) - semantically identical because the dispatch loop has an
 * EMPTY tail. It terminates every handler AND replaces the old vm_raise
 * `continue;`s (which may sit INSIDE a handler's inner switch - which is
 * why the switch flavor is `continue`, never `break`: it must reach the
 * dispatch loop, not fall out of the inner switch).
 */
#if defined(__GNUC__) && !defined(ML_NO_CGOTO)
#  define ML_CGOTO 1
#endif

#ifdef ML_CGOTO
#  define VM_CASE(N)  lbl_##N
#  define VM_NEXT     goto *vm_optbl[static_cast<size_t>(                    \
                          (in = &code[pc])->op)]
   /* A VM_NEXT that must EXIT a scope holding live non-trivially-
    * destructible locals (the call ops' cross-frame exception dispatch).
    * clang forbids an INDIRECT goto from exiting such a scope (it cannot
    * emit the cleanups), so these COLD sites take a DIRECT goto to a
    * per-function trampoline that re-dispatches - destructors run, and
    * only the cold exception path shares a dispatch point. */
#  define VM_NEXT_COLD  goto vm_dispatch_cold
#else
#  define VM_CASE(N)  case OpCode::N
   /* `continue`, not `break`: at outer case level the two are equivalent
    * (empty loop tail), but a VM_NEXT INSIDE a handler's inner switch (the
    * vm_raise paths in IntBin/FloatBin) must reach the dispatch loop, not
    * fall out of the inner switch into the slot write. */
#  define VM_NEXT       continue
#  define VM_NEXT_COLD  continue
#endif

static bool g_vm_executing = false;

/*
 * Per-run storage for compiled function-body chunks (Phase 4), keyed by the
 * FuncDeclStmt. Cleared at the start of every vm_execute so a fresh program run
 * never reuses a prior run's chunks (and a FuncDeclStmt's `vm_chunk` pointer
 * into this map only needs to stay valid for the current run). An unordered_map
 * is node-based, so a stored Chunk's address is stable across rehashes - the
 * `vm_chunk` pointers stay valid as more functions are compiled. Script-only
 * (the VM is not used in the REPL).
 */
static std::unordered_map<const FuncDescriptor *, Chunk> g_func_chunks;

static void vm_precompile_all(const Block *root);   /* AOT: defined below */

/*
 * Execute the optimized program via the bytecode VM. It builds the program's
 * root EvalContext exactly as Block::do_eval does for the root block
 * (ctx == nullptr): the implicit "main" Frame for slotted top-level vars, and
 * the program-wide GlobalFuncTable for top-level functions / escaped globals.
 * Both live for the whole run. run_chunk then drives the chunk; in Phase 0
 * every construct once ran via a fallback op, so this is byte-identical to
 * root->eval(nullptr) - which the differential harness enforces.
 */
/* Mark the program as EXECUTING for a scope, so vm_func_chunk's guard knows
 * any chunk it compiles is over the final AST (save/restore, not a bare set,
 * in case of re-entry; restored even on an exception). */
struct ExecGuard {
    bool prev;
    ExecGuard() : prev(g_vm_executing) { g_vm_executing = true; }
    ~ExecGuard() { g_vm_executing = prev; }
};

/* Per-loop LIVE dict-iterator state (DictIterInit/DictIterNext): the
 * intrusive_ptr pins the dict alive for the loop, the iterator persists across
 * iterations. One per native dict foreach in the chunk (Chunk::n_dict_iters),
 * indexed by the codegen-assigned iter_id. Local to vm_run_chunk, so a `return`
 * / exception mid-loop releases it when the frame unwinds - no cleanup op. */
struct DictIterState {
    intrusive_ptr<DictObject> dict;
    DictObject::inner_type::iterator it;
};
/* Per-loop LIVE state for a native `foreach (<ids> in [indexed] <dyn>)`: the
 * array-vs-dict choice is made ONCE at ForeachDynInit and recorded here (the
 * pinned `container` keeps the array/dict alive for the loop). One per native
 * ForeachDyn (Chunk::n_dyn_iters), indexed by the codegen-assigned iter_id.
 * `targets` points into the chunk's unpack_targets pool (immutable at
 * runtime): the per-var frame slots, -1 == a `_` placeholder. */
struct DynIterState {
    EvalValue container;   /* pins the array OR dict for the loop */
    bool is_dict = false;
    bool indexed = false;  /* ids[0] is the iteration counter */
    int nvars = 1;         /* TOTAL loop vars (incl. the indexed counter) */
    const std::vector<int32_t> *targets = nullptr;   /* per-var slots */
    size_type idx = 0, size = 0;               /* array cursor + snapshot */
    int_type counter = 0;                      /* dict `indexed` counter */
    DictObject::inner_type::iterator it;       /* dict cursor (iff is_dict) */
};
/* P8 Inc 0: an active `try` region on the VM handler stack. `catch_pc` is where
 * the boundary jumps on a caught exception (the CatchTest chain). */
struct VmHandler {
    uint32_t catch_pc;
};

/*
 * The VM's SLOT STACK (plans/vm-native-call-stack.md): every VM frame is a
 * WINDOW of a segment. SEGMENTED, not relocating, because C++ builtins hold
 * frame LValue* / EvalValue& ACROSS user-code callbacks (sort's arg0 over
 * its comparator calls, map/filter's container reference over per-element
 * calls) - a relocating stack would dangle them, so a window's address must
 * be stable for its whole lifetime. A frame never spans segments; a frame
 * larger than SEG_SLOTS gets a dedicated segment of exactly its size.
 * Segment slots are constructed ONCE and REUSED window-over-window; a pop
 * resets the window's slots to none (releasing references exactly where the
 * old per-call Frame's destructor did).
 */
struct VmStackSeg {
    std::vector<LValue> slots;
    int_type top = 0;                 /* allocation watermark */
    explicit VmStackSeg(size_t cap) : slots(cap) { }
};

/*
 * One VM frame's CALL RECORD. A BOUNDARY record (boundary == 1) is a frame
 * entered from C++ (main, or do_func_call for a builtin-callback / generic
 * call) - popping it returns control to C++, and its ret_* fields are
 * unused. An in-VM record (CallV/CachedCallV/CallValueV) resumes the parent
 * frame directly: ret_chunk/ret_pc, the result into parent slot `dst`.
 */
struct VmCallRec {
    LValue *window;                   /* stable - segments never move */
    int_type nslots;
    int seg;                          /* owning segment index */
    int_type seg_top_before;          /* watermark to restore on pop */

    const Chunk *run_chunk = nullptr; /* the chunk THIS frame executes */

    /* Resume state (in-VM records only). */
    const Chunk *ret_chunk = nullptr;
    size_t ret_pc = 0;                /* the pc AFTER the call op */
    int_type dst = -1;                /* parent slot for the return value */
    const FuncDescriptor *desc = nullptr;   /* callee (backtrace); null for
                                             * the root/main frame */
    std::vector<LValue> *caller_captures = nullptr;  /* ctx.captures to
                                                      * restore on pop */

    /* Per-FRAME exception/iterator state (was vm_run_chunk locals - one
     * frame per invocation then; watermarks into the activation's shared
     * stacks now). */
    uint32_t handler_base = 0;
    uint32_t diter_base = 0, dyiter_base = 0;
    std::unique_ptr<RuntimeException> exc;  /* in-flight caught exception */
    Pend pend = Pend::normal;               /* finally resume action */

    /* CachedCallV: the {func, args} key to store the (scalar) result under
     * in the CALLER's cache when this frame pops. */
    std::unique_ptr<PureCacheKey> cache_key;

    unsigned char boundary = 0;

    /* The CALLER's per-frame pure-call cache, stashed while a callee runs:
     * eval.cpp reaches the cache as `ctx->frame->pure_cache`, and the view
     * Frame is SHARED across the whole activation - without the stash the
     * cache would outlive its frame and become global memoization, which
     * the per-frame design explicitly forbids (see PureCache, eval.h). */
    std::unique_ptr<PureCache> caller_cache;
};

/*
 * A VM ACTIVATION (plans/vm-native-call-stack.md): one run of VM frames
 * entered from C++ (vm_run). It owns the segmented slot stack, the call
 * records, and the VIEW Frame the dispatch loop + builtins access slots
 * through (Frame::point_at - a non-owning window; repointing per frame is
 * two stores). do_func_call allocates a callee's frame window from here
 * (vm_window_push/pop) instead of constructing a Frame per call; Phase C's
 * later increments move the call protocol itself in-loop.
 */
struct VmActivation {
    static constexpr int_type SEG_SLOTS = 16 * 1024;

    std::vector<std::unique_ptr<VmStackSeg>> segs;
    int cur_seg = -1;
    std::vector<VmCallRec> records;
    /*
     * The REUSABLE context for builtin->callback invocations (vm_invoke,
     * Phase D): constructed ONCE per activation (vm_run parents it to the
     * root), reused for every callback - deleting the per-element 136-byte
     * EvalContext (std::map member) that do_func_call built. Its OWN
     * FlowState (func_ctx=true) is essential: reusing the CALLER's ctx
     * would let a callback's boundary ReturnV leave flow.type == ret in
     * the ENCLOSING frame's context, corrupting its fall-through result.
     * captures/flow are saved+restored around nested invokes.
     */
    std::unique_ptr<EvalContext> invoke_ctx;
    /* Shared per-frame stacks, sliced by the records' watermarks. */
    std::vector<VmHandler> handlers;
    std::vector<DictIterState> dict_iters;
    std::vector<DynIterState> dyn_iters;
    Frame view_frame;                 /* the loop's window into the stack */
    int_type used = 0;                /* live slots, for the depth cap */
    int_type cap;                     /* MYLANG_VM_STACK (slots) */

    VmActivation() : cap(stack_cap()) { records.reserve(32); }

    /* The configurable depth cap (slots). One env read per process. */
    static int_type stack_cap()
    {
        static const int_type cap = [] {
            if (auto v = env_get("MYLANG_VM_STACK")) {
                const long long n = atoll(v->c_str());
                if (n > 0)
                    return static_cast<int_type>(n);
            }
            return static_cast<int_type>(1) << 20;   /* 1M slots (~48MB) */
        }();
        return cap;
    }

    /* Allocate an n-slot frame window and repoint the view Frame at it.
     * Throws the CATCHABLE StackOverflowEx at the cap - a clean, located
     * error where the old per-call C-stack model segfaulted. */
    /* `ck` sizes the frame's iterator slices (n_dict_iters/n_dyn_iters);
     * it is the chunk the frame RUNS. */
    ML_ALWAYS_INLINE Frame *push_window(int_type n, const Chunk *ck,
                                        bool boundary)
    {
        if (used + n > cap)
            throw StackOverflowEx();

        VmStackSeg *sg = cur_seg >= 0 ? segs[cur_seg].get() : nullptr;
        if (!sg || sg->top + n > static_cast<int_type>(sg->slots.size())) {
            /* Advance to (or create) a segment with room. Reuse an already-
             * allocated successor when its capacity fits (the common pop/
             * push cycle at a segment edge); else append a fresh one. */
            const size_t need = static_cast<size_t>(
                n > SEG_SLOTS ? n : SEG_SLOTS);
            if (cur_seg + 1 < static_cast<int>(segs.size())
                    && segs[cur_seg + 1]->slots.size() >= need) {
                cur_seg++;
            } else {
                segs.insert(segs.begin() + (cur_seg + 1),
                            std::make_unique<VmStackSeg>(need));
                cur_seg++;
            }
            sg = segs[cur_seg].get();
            ML_CHECK(sg->top == 0);
        }

        records.emplace_back();          /* fill IN PLACE - no struct move */
        VmCallRec &rec = records.back();
        rec.window = sg->slots.data() + sg->top;
        rec.nslots = n;
        rec.seg = cur_seg;
        rec.seg_top_before = sg->top;
        rec.boundary = boundary;
        rec.run_chunk = ck;
        rec.handler_base = static_cast<uint32_t>(handlers.size());
        rec.diter_base = static_cast<uint32_t>(dict_iters.size());
        rec.dyiter_base = static_cast<uint32_t>(dyn_iters.size());
        if (ck->n_dict_iters)
            dict_iters.resize(dict_iters.size() + ck->n_dict_iters);
        if (ck->n_dyn_iters)
            dyn_iters.resize(dyn_iters.size() + ck->n_dyn_iters);
        sg->top += n;
        used += n;
        /* Stash the CALLER's pure cache; the callee's frame starts with
         * none (per-frame scoping - see VmCallRec::caller_cache). */
        if (view_frame.pure_cache)
            rec.caller_cache = std::move(view_frame.pure_cache);
        view_frame.point_at(rec.window, static_cast<int>(n));
        return &view_frame;
    }

    /* Release the TOP window: reset its slots to none (drop references -
     * the old Frame-dtor point), restore the watermark, repoint the view
     * at the new top (if any). */
    /* Pop the top record WITHOUT moving the (unique_ptr-bearing) struct
     * out: the caller reads any resume fields it needs BEFORE calling (they
     * are stable), we clean by reference, then pop_back (a cheap dtor - the
     * rare owning fields are null on the hot path). */
    ML_ALWAYS_INLINE void pop_window()
    {
        ML_CHECK(!records.empty());
        VmCallRec &rec = records.back();

        /* Release the window's REFERENCES (the old Frame-dtor point). A
         * slot holding a trivial value (int/float/bool/none - no refcount,
         * no slice registration) is left STALE: unobservable, since a slot
         * can only be read after its decl re-binds it (the no-hoist rule),
         * and COW/use_count semantics only see reference types.
         * Profile #2: iterate ONLY the chunk's audited ref_slots (params +
         * non-scalar-write dsts) - an all-scalar frame (fib-class) skips
         * the O(nslots) walk. A hardened build re-scans the whole window
         * and asserts the list missed nothing. */
        if (rec.run_chunk) {
            for (const int32_t s : rec.run_chunk->ref_slots) {
                if (s >= rec.nslots)
                    break;               /* sorted */
                LValue &lv = rec.window[s];
                if (lv.get().get_type()->t >= Type::t_str)
                    lv = LValue();
            }
#if ML_VM_HARDENING
            for (int_type i = 0; i < rec.nslots; i++)
                ML_VM_CHECK(rec.window[i].get().get_type()->t
                            < Type::t_str);
#endif
        } else {
            for (int_type i = 0; i < rec.nslots; i++) {
                LValue &lv = rec.window[i];
                if (lv.get().get_type()->t >= Type::t_str)
                    lv = LValue();
            }
        }

        /* Per-frame state dies with the frame. A normal return has already
         * popped its handlers (the codegen emits the crossed-try pops); the
         * exceptional walk truncates whatever is left. */
        if (handlers.size() != rec.handler_base)
            handlers.resize(rec.handler_base);
        if (dict_iters.size() != rec.diter_base)
            dict_iters.resize(rec.diter_base);
        if (dyn_iters.size() != rec.dyiter_base)
            dyn_iters.resize(rec.dyiter_base);

        segs[rec.seg]->top = rec.seg_top_before;
        used -= rec.nslots;
        cur_seg = rec.seg;

        /* The popped frame's cache dies HERE (per-frame scoping); the
         * caller's stashed cache comes back into the view. */
        if (rec.caller_cache || view_frame.pure_cache)
            view_frame.pure_cache = std::move(rec.caller_cache);

        records.pop_back();

        if (!records.empty()) {
            const VmCallRec &tp = records.back();
            view_frame.point_at(tp.window, static_cast<int>(tp.nslots));
            cur_seg = tp.seg;
        }
    }
};

/* The CURRENT activation (single-threaded; set for the duration of vm_run).
 * Null outside VM execution - do_func_call's window push falls back to a
 * plain per-call Frame then (e.g. a harness helper calling eval_func with
 * the engine flag set but no VM run active). */
static VmActivation *g_vm_act = nullptr;

Frame *vm_window_push(int_type nslots, const Chunk *ck)
{
    if (!g_vm_act)
        return nullptr;
    /* A do_func_call entry is a BOUNDARY frame: popping it returns control
     * to C++ (the in-VM protocol never pops past it). */
    return g_vm_act->push_window(nslots, ck, /*boundary=*/true);
}

void vm_window_pop()
{
    ML_CHECK(g_vm_act);
    g_vm_act->pop_window();
}

VmProgram
vm_compile(const Construct *root_c)
{
    const Block *root = static_cast<const Block *>(root_c);
    ExecGuard exec_guard;

    /* Fresh per program: drop any prior program's function chunks (and their
     * stale descriptor vm_chunk pointers, whose functions are long freed). */
    g_func_chunks.clear();

    VmProgram prog;
    prog.root = codegen_program(root);

    /* AOT: compile every function body upfront (no lazy per-call compile) - the
     * maintainer's no-lazy rule + a `.myv`-serialization prerequisite. After
     * this, do_func_call reads a precomputed vm_chunk and never compiles. */
    vm_precompile_all(root);

    /* Root-context data the run needs, copied OUT of the root Block. */
    prog.root_slot_count = root->slot_count;
    prog.global_func_names = root->global_func_names;

    /* OWNERSHIP TRANSFER (plans/vm-ast-free-runtime.md): move every function
     * descriptor and struct type def out of the AST into the program image,
     * so the tree is droppable. The decls keep their raw aliases (desc/def),
     * which stay valid - a unique_ptr move does not relocate the pointee. The
     * walks use the COMPLETE child visitor, so no decl is missed. */
    std::vector<const FuncDeclStmt *> funcs;
    for (const auto &e : root->elems)
        collect_funcs(e.get(), funcs);
    for (const FuncDeclStmt *fn : funcs)
        if (fn->desc_owner)
            prog.funcs.push_back(
                std::move(const_cast<FuncDeclStmt *>(fn)->desc_owner));

    std::function<void (Construct *)> take_structs = [&](Construct *c) {
        if (!c)
            return;
        if (auto *sd = dynamic_cast<StructDeclStmt *>(c)) {
            if (sd->def_owner)
                prog.structs.push_back(std::move(sd->def_owner));
            return;
        }
        for_each_child_of(c, take_structs);
    };
    for (const auto &e : root->elems)
        take_structs(e.get());

    return prog;
}

void
vm_run(VmProgram &prog)
{
    ExecGuard exec_guard;
    EvalContext ctx(nullptr, /*const_ctx=*/false);

    /* Main's frame - the resolved locals plus the register machine's scratch
     * temps [slot_count, slot_count + n_temps) - is the activation's FIRST
     * frame window; every callee's window stacks above it (vm_window_push
     * from do_func_call). The activation is announced in g_vm_act for the
     * run (restored even on an exception). */
    VmActivation act;
    struct ActGuard {
        VmActivation *prev;
        ActGuard(VmActivation *a) : prev(g_vm_act) { g_vm_act = a; }
        ~ActGuard() { g_vm_act = prev; }
    } act_guard(&act);

    const int_type main_slots = prog.root_slot_count + prog.root.n_temps;
    if (main_slots)
        ctx.frame = act.push_window(main_slots, &prog.root,
                                    /*boundary=*/true);

    std::unique_ptr<GlobalFuncTable> gtable;
    if (!prog.global_func_names.empty()) {
        gtable = std::make_unique<GlobalFuncTable>();
        gtable->init(prog.global_func_names);
        ctx.gfuncs = gtable.get();
    }

    /* The reusable callback-invoke context (vm_try_invoke): parented to the
     * ROOT (a callback body must not see its caller - lexical scoping; it
     * inherits gfuncs/frame from the root, func_ctx gives it its own
     * FlowState). Built once, after the root ctx is complete. */
    act.invoke_ctx = std::make_unique<EvalContext>(&ctx, false,
                                                   /*func_ctx=*/true);
    act.invoke_ctx->frame = &act.view_frame;

    vm_run_chunk(prog.root, ctx);

    /* Inc v2: a top-level-uncaught exception propagated to here as the pending
     * signal (main's boundary found no handler and converted it). Convert it
     * back to a C++ throw for the mylang.cpp / -rt top-level handler - the SAME
     * object, so type / message / loc / backtrace are unchanged. */
    if (g_vm_exc_pending) {
        std::unique_ptr<RuntimeException> ex = std::move(g_vm_exc_pending);
        ex->rethrow();
    }
}

void
vm_execute(const Construct *root_c)
{
    /*
     * The program is RETAINED for the session (a static list), not a local:
     * profile #3's lazy BacktraceFrames hold `FuncDescriptor *` into the
     * program image, and an exception unwinding out of vm_run must still
     * render its backtrace at the caller's catch - a local VmProgram would
     * die during the unwind and dangle the frames. The harness/REPL already
     * retain their ASTs for the same class of reason; the SCRIPT driver
     * keeps its VmProgram outside the try (mylang.cpp), so this list is
     * the harness-entry analogue. Bounded by the number of vm_execute
     * calls per process (the -rt suite: ~1400 small programs).
     */
    static std::vector<VmProgram> retained;
    retained.push_back(vm_compile(root_c));
    vm_run(retained.back());
}

void
vm_ast_teardown(std::unique_ptr<Construct> &root, VmProgram &prog)
{
#ifndef NDEBUG
    /* The tree is about to die: no descriptor may point back into it. */
    for (auto &fd : prog.funcs)
        fd->decl = nullptr;

    root.reset();

    /* NOTHING survived: every Construct ever allocated is freed (and zeroed
     * by operator delete). A nonzero count means some structure still owns a
     * node - a runtime AST dependence this proof exists to catch. */
    ML_CHECK_MSG(Construct::live_nodes == 0,
                 "AST teardown: Construct nodes still alive under -vm");
#else
    (void) root;
    (void) prog;
#endif
}

/*
 * Compile a block-bodied function's body to a chunk on first use and cache it
 * (see vm.h). Returns null - so the caller tree-walks - for an expression body,
 * or a block whose codegen produced NO register op (a native loop): such a body
 * is pure fallback, and driving it through the VM would only add dispatch over
 * Block::do_eval with no offsetting win.
 */
const Chunk *
vm_func_chunk(const FuncDescriptor *fdesc)
{
    /*
     * A chunk may be compiled ONLY while executing the final AST. If we are
     * here during a compile-time fold (resolve_names), the const-eval gate in
     * do_func_call was bypassed and we are about to cache node pointers the
     * optimizer will free (use-after-free). Fail loudly instead. Cold path
     * (once per function per run), so a plain ML_CHECK - on in every ASSERTS
     * build.
     */
    ML_CHECK_MSG(g_vm_executing,
                 "vm_func_chunk outside vm_execute: a compile-time fold "
                 "reached the VM body hook (const-eval gate bypassed)");

    auto it = g_func_chunks.find(fdesc);
    if (it != g_func_chunks.end())
        return &it->second;

    /* The compile + gate (base-template / scope-free / has-native) is the
     * shared codegen_func_body, so the VM's compiled set is byte-identical to
     * what -vd dumps. AOT (vm_precompile_all) fills g_func_chunks for the whole
     * program upfront, so this lazy miss path is only a safety net for a func
     * the precompile walk didn't reach (there should be none). */
    Chunk ck;
    ML_CHECK_MSG(fdesc->decl, "vm_func_chunk on a descriptor with no decl");
    if (!codegen_func_body(fdesc->decl, ck))
        return nullptr;
    return &g_func_chunks.emplace(fdesc, std::move(ck)).first->second;
}

/*
 * AOT (no lazy): compile EVERY function body in the program to its chunk
 * upfront, before execution, so no chunk is ever built lazily on first call
 * (the maintainer's no-lazy rule; also a prerequisite for serialized `.myv`
 * bytecode). Walks all FuncDeclStmts (collect_funcs order), stamps each with
 * its compiled chunk (or null → tree-walked) plus `vm_chunk_tried = true`, so
 * do_func_call reads the precomputed pointer and never enters the lazy branch.
 * Base templates get null (codegen_func_body skips them) → never compiled →
 * absent from the compiled chunk set (and from -vd). Runs inside vm_execute
 * (g_vm_executing true, the AST final), so vm_func_chunk's guard is satisfied.
 */
static void
vm_precompile_all(const Block *root)
{
    std::vector<const FuncDeclStmt *> funcs;
    for (const auto &e : root->elems)
        collect_funcs(e.get(), funcs);

    for (const FuncDeclStmt *fn : funcs) {
        /* compiles + caches (or null); stamped on the DESCRIPTOR */
        fn->desc->vm_chunk = vm_func_chunk(fn->desc);
        fn->desc->vm_chunk_tried = true;
        bool fast = true;
        for (const auto &p : fn->desc->params)
            if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
                fast = false;
        fn->desc->fast_bind = fast;
    }
}

/* The in-flight exception's type NAME for catch-matching (a user struct
 * exception's type name, else the built-in name). Mirrors do_catch's ex_name. */
static std::string_view vm_exc_name(const RuntimeException *ex)
{
    if (auto *eo = dynamic_cast<const ExceptionObject *>(ex))
        return eo->get_name();
    return ex->name;
}

/* The value `catch (T as e)` binds: a thrown struct's instance (so `e.field`
 * works), else a fresh ExceptionObject. Mirrors do_catch's bind_val. */
static EvalValue vm_catch_bind_val(RuntimeException *ex)
{
    auto *eo = dynamic_cast<ExceptionObject *>(ex);
    if (eo && eo->get_data().is<intrusive_ptr<StructObject>>())
        return eo->get_data();
    return EvalValue(make_intrusive<ExceptionObject>(
        eo ? *eo : ExceptionObject(ex->name)));
}

/* P8 Inc 1: build (WITHOUT C++-throwing) the RuntimeException a native `Throw`
 * raises. Mirrors ThrowStmt::do_eval: a struct instance → an ExceptionObject
 * named by its type + carrying the instance; a caught ExceptionObject value →
 * a copy (re-throw); anything else → TypeErrorEx (a real error, still C++). */
static std::unique_ptr<RuntimeException>
vm_make_thrown_exc(const EvalValue &v, Loc estart, Loc eend)
{
    if (v.is<intrusive_ptr<StructObject>>())
        return std::unique_ptr<RuntimeException>(new ExceptionObject(
            std::string(v.get<intrusive_ptr<StructObject>>()->def->name->val),
            v));
    if (v.is<intrusive_ptr<ExceptionObject>>())
        return std::unique_ptr<RuntimeException>(
            v.get<intrusive_ptr<ExceptionObject>>()->clone());
    throw TypeErrorEx("Can only throw a struct instance", estart, eend);
}

/* P8: pop the innermost handler OF THE CURRENT FRAME (entries below the
 * record's watermark belong to outer frames - those are reached by the
 * boundary catch's frame walk, never here) and set `pc` to its
 * catch-dispatch; false (pc unchanged) if this frame has none. */
static bool vm_dispatch_exc(VmActivation &act, const VmCallRec &cur,
                            size_t &pc)
{
    if (act.handlers.size() <= cur.handler_base)
        return false;
    pc = act.handlers.back().catch_pc;
    act.handlers.pop_back();
    return true;
}

/*
 * Append the frame a WALK is popping to the exception's backtrace - the
 * record-based twin of vm_capture_frame (eval.cpp): name/params from the
 * callee DESCRIPTOR, the call-site caret from the PARENT's chunk at the call
 * op (ret_pc - 1), the pure-func UndefinedVariableEx tag from pure_ctx.
 */
static void vm_capture_rec_frame(RuntimeException &e, const VmCallRec &rec)
{
    if (rec.desc->pure_ctx)
        if (auto *undefEx = dynamic_cast<UndefinedVariableEx *>(&e))
            undefEx->in_pure_func = true;

    /* profile #3: the LAZY frame - no strings at capture time */
    Loc cs, end_ignored;
    rec.ret_chunk->loc_at(rec.ret_pc - 1, cs, end_ignored);
    e.backtrace.emplace_back(rec.desc, cs);
}

/* Inc v2: the in-flight cross-frame exception signal (see vm.h). */
std::unique_ptr<RuntimeException> g_vm_exc_pending;

/*
 * Inc 4: if the op at `pc` was spliced from an INLINED body, flush that body's
 * virtual "inlined-at" frames into the exception's backtrace (once, keyed off
 * inline_origin_emitted - as the tree-walker's Construct::eval does). Called at
 * the raise site (a throw / runtime error IN inlined code) and where a signal
 * propagates through a call op (a call FROM inlined code). Cold: error path
 * only. No-op for a chunk with no inlined ops (inline_ctx_at returns null).
 */
static void vm_flush_inline(const Chunk &chunk, size_t pc, Exception &e)
{
    if (e.inline_origin_emitted)
        return;
    int32_t idx = chunk.inline_frame_at(pc);
    if (idx < 0)
        return;
    /* Walk the flattened inline_frames pool by parent index (innermost callee
     * first) - same BacktraceFrames as flush_inline_frames over the InlineCtx
     * chain, but AST-free (serializable pool). */
    for (int32_t i = idx; i >= 0; i = chunk.inline_frames[i].parent) {
        const Chunk::InlineFrame &f = chunk.inline_frames[i];
        e.backtrace.push_back({f.callee_name, f.params, f.call_site});
    }
    e.inline_origin_emitted = true;
}

/* fwd (defined below CachedCallV's probe) - vm_raise now walks natively. */
static ML_COLD bool
vm_unwind_walk(VmActivation &act, EvalContext &ctx, const Chunk *&chunk,
               size_t &pc, std::unique_ptr<RuntimeException> ex);

/*
 * Raise an exception FROM a VM op - a `throw` / `rethrow` / reraise, or a
 * runtime error the op detects itself (div/mod-by-zero). G1
 * (plans/vm-performance-roadmap.md): the ENTIRE propagation is the native
 * FRAME WALK (vm_unwind_walk) - dispatch to a handler in the current frame,
 * else pop in-VM records (capturing their backtrace frames) until a handler
 * or the activation's BOUNDARY record; only the boundary converts to the
 * g_vm_exc_pending signal. NO C++ throw anywhere on this path - a
 * 16-frame-deep `throw` is pointer work, not unwinding (69_exc_crossframe;
 * the old shape C++-threw to the boundary catch when no SAME-frame handler
 * existed, paying one landing pad + an exception clone per cross-frame
 * raise). Stamps the op's caret from the loc side table when the exception
 * carries none; flushes the raise site's inlined frames (both exactly as
 * the boundary catch did for a C++-thrown error). Cold path only. Returns
 * true = dispatched (chunk/pc point at the handler - the CALLER MUST refresh
 * `code` before dispatching); false = boundary reached (signal set - the
 * caller returns, do_func_call captures + propagates).
 */
static bool
vm_raise(const Chunk *&chunk, size_t &pc, VmActivation &act, EvalContext &ctx,
         std::unique_ptr<RuntimeException> ex)
{
    if (!ex->loc_start) {
        Loc s, en;
        chunk->loc_at(pc, s, en);
        ex->loc_start = s;
        ex->loc_end = en;
    }
    vm_flush_inline(*chunk, pc, *ex);      /* frames if raised in inlined */

    /* The SAME-FRAME fast path first (the pre-G1 shape, kept out of the
     * ML_COLD walk: a same-frame `throw`+catch loop - 42_exceptions - pays
     * only this dispatch; routing it through the cold-section walk cost a
     * measured +12% there). Not cold-marked itself for the same reason. */
    VmCallRec &cur = act.records.back();
    if (vm_dispatch_exc(act, cur, pc)) {
        cur.exc = std::move(ex);
        return true;                       /* chunk unchanged */
    }
    return vm_unwind_walk(act, ctx, chunk, pc, std::move(ex));
}

/* The desc-based twin of do_func_call's vm_capture_frame for the invoke
 * boundary: name/params/pure tag; the call site is loc-less (builtin
 * callbacks pass no call site - matching eval_func's captures today). */
static ML_COLD void
vm_capture_desc_frame(Exception &e, const FuncDescriptor *d)
{
    if (d->pure_ctx)
        if (auto *undefEx = dynamic_cast<UndefinedVariableEx *>(&e))
            undefEx->in_pure_func = true;

    /* profile #3: the LAZY frame - no strings at capture time */
    e.backtrace.emplace_back(d, Loc());
}

/*
 * Phase D (plans/vm-native-call-stack.md): a builtin's USER-CALLBACK call
 * (map/filter/sort's comparator/make_dict's generator/find's keyfunc, via
 * eval_func) runs the callee as a BOUNDARY frame on the CURRENT activation
 * - the per-element do_func_call + EvalContext construction is gone; the
 * cost is one window push/bind/pop plus the loop entry. Returns false
 * (nothing done) when the invoke path isn't available: no activation /
 * invoke context (a tree-walk or const-eval caller), or a chunk-less
 * callee - the caller falls back to do_func_call. Exception contract as
 * do_func_call with as_signal=false: a C++ throw and the pending signal
 * both surface as a C++ throw carrying this frame's backtrace entry.
 */
bool vm_try_invoke(EvalContext *caller_ctx, FuncObject &obj,
                   const EvalValue *argv, size_t n, EvalValue &out)
{
    if (g_exec_engine != ExecEngine::Vm || !g_vm_act
            || !g_vm_act->invoke_ctx || caller_ctx->in_const_eval())
        return false;
    if (!obj.func->vm_chunk_tried || !obj.func->vm_chunk)
        return false;

    VmActivation &act = *g_vm_act;
    EvalContext &c = *act.invoke_ctx;
    const FuncDescriptor *d = obj.func;
    const Chunk *cck = static_cast<const Chunk *>(d->vm_chunk);
    const size_t nparams = d->params.size();
    if (n > nparams || n < static_cast<size_t>(d->min_args))
        throw InvalidNumberOfArgsEx();

    const int_type total =
        d->frame_size + static_cast<int_type>(cck->n_temps);
    Frame *w = act.push_window(total, cck, /*boundary=*/true);

    struct Restore {
        VmActivation &a;
        EvalContext &c;
        std::vector<LValue> *caps;
        ~Restore() {
            a.pop_window();
            c.captures = caps;
        }
    } restore{act, c, c.captures};

    /* Bind from the pre-evaluated values (do_func_bind_params' value
     * overloads' exact semantics: non-const, none for omitted trailing opt
     * params, i/f coercion; a bind throw pops WITHOUT capturing). */
    if (d->fast_bind) {
        for (size_t i = 0; i < n; i++)
            w->at(static_cast<int_type>(i)) = LValue(argv[i], false);
        for (size_t i = n; i < nparams; i++)
            w->at(static_cast<int_type>(i)) = LValue(EvalValue(), false);
    } else {
        for (size_t i = 0; i < nparams; i++) {
            const FuncDescriptor::ParamDesc &p = d->params[i];
            EvalValue val = i < n ? argv[i] : EvalValue();
            if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
                val = vm_coerce_decl_num(val, p.decl_type == DeclType::f);
            w->at(static_cast<int_type>(i)) = LValue(std::move(val), false);
        }
    }

    c.captures = &obj.capture_slots;
    c.flow->type = FlowState::none;

    try {
        vm_run_chunk(*cck, c);
    } catch (Exception &e) {
        vm_capture_desc_frame(e, d);
        throw;
    }

    if (g_vm_exc_pending) {
        vm_capture_desc_frame(*g_vm_exc_pending, d);
        std::unique_ptr<RuntimeException> ex = std::move(g_vm_exc_pending);
        ex->rethrow();
    }

    if (c.flow->type == FlowState::ret) {
        c.flow->type = FlowState::none;
        out = std::move(c.flow->value);
    } else {
        out = EvalValue();
    }
    return true;
}

/*
 * VmInvoker (vm.h): the prepared per-loop callback invoker. The ctor gates
 * exactly like vm_try_invoke and pushes ONE boundary window; invoke()
 * rebinds the param slots, resets the reusable flow, re-enters the loop,
 * and resets the window's REFERENCE slots afterwards (per-call frame-death
 * semantics: references die where a fresh frame's would; stale trivial
 * values are unobservable). Exceptions: a C++ throw or the pending signal
 * surfaces as a C++ throw carrying this frame's backtrace entry - the dtor
 * pops the window and restores the shared invoke context's captures on any
 * exit path (including a throw unwinding the builtin's loop).
 */
VmInvoker::VmInvoker(EvalContext *ctx, FuncObject &obj)
{
    if (g_exec_engine != ExecEngine::Vm || !g_vm_act
            || !g_vm_act->invoke_ctx || ctx->in_const_eval())
        return;
    if (!obj.func->vm_chunk_tried || !obj.func->vm_chunk)
        return;

    act_ = g_vm_act;
    c_ = act_->invoke_ctx.get();
    desc_ = obj.func;
    cck_ = static_cast<const Chunk *>(desc_->vm_chunk);
    const int_type total =
        desc_->frame_size + static_cast<int_type>(cck_->n_temps);
    w_ = act_->push_window(total, cck_, /*boundary=*/true);
    saved_caps_ = c_->captures;
    c_->captures = &obj.capture_slots;
    ready_ = true;
}

VmInvoker::~VmInvoker()
{
    if (!ready_)
        return;
    c_->captures = saved_caps_;
    act_->pop_window();
}

EvalValue VmInvoker::invoke(const EvalValue *argv, size_t n)
{
    const FuncDescriptor *d = desc_;
    const size_t nparams = d->params.size();
    if (n > nparams || n < static_cast<size_t>(d->min_args))
        throw InvalidNumberOfArgsEx();

    if (d->fast_bind) {
        for (size_t i = 0; i < n; i++)
            w_->at(static_cast<int_type>(i)) = LValue(argv[i], false);
        for (size_t i = n; i < nparams; i++)
            w_->at(static_cast<int_type>(i)) = LValue(EvalValue(), false);
    } else {
        for (size_t i = 0; i < nparams; i++) {
            const FuncDescriptor::ParamDesc &p = d->params[i];
            EvalValue val = i < n ? argv[i] : EvalValue();
            if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
                val = vm_coerce_decl_num(val, p.decl_type == DeclType::f);
            w_->at(static_cast<int_type>(i)) = LValue(std::move(val), false);
        }
    }

    c_->flow->type = FlowState::none;

    try {
        vm_run_chunk(*cck_, *c_);
    } catch (Exception &e) {
        vm_capture_desc_frame(e, d);
        throw;
    }

    if (g_vm_exc_pending) {
        vm_capture_desc_frame(*g_vm_exc_pending, d);
        std::unique_ptr<RuntimeException> ex = std::move(g_vm_exc_pending);
        ex->rethrow();
    }

    EvalValue res;
    if (c_->flow->type == FlowState::ret) {
        c_->flow->type = FlowState::none;
        res = std::move(c_->flow->value);
    }

    /* Per-call frame death for REFERENCES (see the class comment).
     * Profile #2: only the chunk's audited ref_slots - this scan runs per
     * CALLBACK ELEMENT (sort's comparator, map/filter/make_dict), where
     * the old whole-window walk was a measured cost. */
    LValue *win = w_->slots;
    const int_type total = static_cast<int_type>(w_->size);
    for (const int32_t sidx : cck_->ref_slots) {
        if (sidx >= total)
            break;                       /* sorted */
        if (win[sidx].get().get_type()->t >= Type::t_str)
            win[sidx] = LValue();
    }
#if ML_VM_HARDENING
    for (int_type i = 0; i < total; i++)
        ML_VM_CHECK(win[i].get().get_type()->t < Type::t_str);
#endif

    return res;
}

/*
 * THE LOOP-BODY TEXT RULE (learned twice now, hard way): vm_run_chunk's
 * measured floor is its CODE LAYOUT - growing the function regressed
 * UNTOUCHED pure loops ~10% wall with +1.7% instructions and BETTER
 * branch prediction (the front-end effect, see the roadmap A2 notes). So
 * everything the native call stack added lives in these ML_NOINLINE
 * helpers: one CALL per call-op / invocation / unwind, and the dispatch
 * core keeps its size. Measured under the full-suite interleaved rule.
 */

/* The per-invocation entry: ensure an activation + this entry's BOUNDARY
 * record (see the comment in vm_run_chunk). Returns the activation. */
static ML_NOINLINE VmActivation *
vm_enter_invocation(const Chunk *chunk, std::unique_ptr<VmActivation> &own,
                    bool &swapped, bool &pushed);

/* Call-cluster #2: the COMMON re-entry (a builtin callback re-running its
 * prepared chunk per element - VmInvoker's loop) finds the activation set
 * and its own boundary record on top: two loads + two compares, INLINE, no
 * out-of-line call. Anything else takes the NOINLINE setup below. */
static ML_ALWAYS_INLINE VmActivation *
vm_enter_invocation_fast(const Chunk *chunk,
                         std::unique_ptr<VmActivation> &own,
                         bool &swapped, bool &pushed)
{
    VmActivation *a = g_vm_act;
    if (a && !a->records.empty() && a->records.back().run_chunk == chunk)
        return a;
    return vm_enter_invocation(chunk, own, swapped, pushed);
}

static ML_NOINLINE VmActivation *
vm_enter_invocation(const Chunk *chunk, std::unique_ptr<VmActivation> &own,
                    bool &swapped, bool &pushed)
{
    if (!g_vm_act) {
        own = std::make_unique<VmActivation>();
        swapped = true;
        g_vm_act = own.get();
    }
    if (g_vm_act->records.empty()
            || g_vm_act->records.back().run_chunk != chunk) {
        g_vm_act->push_window(0, chunk, /*boundary=*/true);
        pushed = true;
    }
    return g_vm_act;
}

/*
 * THE IN-VM CALL (plans/vm-native-call-stack.md): push the callee's frame
 * window + record and continue the loop in the callee's chunk - no
 * do_func_call, no EvalContext, no C++ recursion. Arity = two compares
 * (the same InvalidNumberOfArgsEx, reaching the unwind walk like any bind
 * throw did). Bind: the args already sit in the caller's contiguous run
 * (STABLE across the push - segments never move); fast_bind is a plain
 * copy loop (+ none-fill for omitted trailing opt params); a typed i/f
 * param coerces via vm_coerce_decl_num (the tree-walker's exact coercion);
 * runtime bindings are non-const (do_func_bind_params' value overloads
 * bound with ctx->const_ctx == false). A bind throw pops the half-built
 * frame WITHOUT capturing a backtrace frame for it - do_func_call's bind
 * ran BEFORE its try, so a bind error never recorded the callee.
 */
static ML_NOINLINE void
vm_enter_call(VmActivation &act, EvalContext &ctx, const Chunk *&chunk,
              size_t &pc, FuncObject &fo, const Chunk *cck,
              int_type argbase, size_t nargs, int_type dst,
              std::unique_ptr<PureCacheKey> ckey)
{
    const FuncDescriptor *d = fo.func;
    const size_t nparams = d->params.size();
    if (nargs > nparams || nargs < static_cast<size_t>(d->min_args))
        throw InvalidNumberOfArgsEx();

    LValue *argrun = nargs ? &ctx.frame->at(argbase) : nullptr;
    const int_type total =
        d->frame_size + static_cast<int_type>(cck->n_temps);
    Frame *w = act.push_window(total, cck, /*boundary=*/false);
    VmCallRec &rec = act.records.back();
    rec.ret_chunk = chunk;
    rec.ret_pc = pc + 1;
    rec.dst = dst;
    rec.desc = d;
    rec.caller_captures = ctx.captures;
    rec.cache_key = std::move(ckey);

    if (d->fast_bind) {
        for (size_t i = 0; i < nargs; i++)
            w->at(static_cast<int_type>(i)) =
                LValue(argrun[i].get(), false);
        for (size_t i = nargs; i < nparams; i++)
            w->at(static_cast<int_type>(i)) = LValue(EvalValue(), false);
    } else {
        try {
            for (size_t i = 0; i < nparams; i++) {
                const FuncDescriptor::ParamDesc &p = d->params[i];
                EvalValue val = i < nargs ? argrun[i].get()
                                          : EvalValue();
                if (p.decl_type == DeclType::i
                        || p.decl_type == DeclType::f)
                    val = vm_coerce_decl_num(
                        val, p.decl_type == DeclType::f);
                w->at(static_cast<int_type>(i)) =
                    LValue(std::move(val), false);
            }
        } catch (...) {
            act.pop_window();
            throw;
        }
    }

    ctx.captures = &fo.capture_slots;
    chunk = cck;
    pc = 0;
}

/* Pop the TOP (in-VM) frame with `res` as its return value: store a cached
 * call's SCALAR result in the caller's cache (pure_cache_call's exact
 * rule), write the parent's dst slot, resume after the call. */
static ML_NOINLINE void
vm_leave_call(VmActivation &act, EvalContext &ctx, const Chunk *&chunk,
              size_t &pc, EvalValue res)
{
    VmCallRec &dead = act.records.back();
    ctx.captures = dead.caller_captures;
    chunk = dead.ret_chunk;
    pc = dead.ret_pc;
    const int_type dst = dead.dst;
    std::unique_ptr<PureCacheKey> ckey = std::move(dead.cache_key);
    act.pop_window();
    if (ckey && res.get_type()->t < Type::t_str)
        ctx.frame->ensure_pure_cache().emplace(std::move(*ckey), res);
    if (dst >= 0)                /* -1 = a DISCARDED call statement's dst
                                  * (the peephole's dead-dst rule) */
        ctx.frame->at(dst).put(std::move(res));
}

/* CachedCallV's cache probe (vm_cached_call's exact flow): a HIT writes the
 * result copy into dst and returns true; a MISS leaves the {func, args} key
 * in `pending` for vm_enter_call to carry (stored on pop). */
static ML_NOINLINE bool
vm_cache_probe(EvalContext &ctx, const Instr *in, const FuncDescriptor *d,
               std::unique_ptr<PureCacheKey> &pending)
{
    std::vector<EvalValue> vals;
    const size_t n = static_cast<size_t>(in->b_lit());
    vals.reserve(n);
    const LValue *ap0 = n ? &ctx.frame->at(in->a_lit()) : nullptr;
    for (size_t i = 0; i < n; i++)
        vals.push_back(ap0[i].get());
    PureCacheKey key{ d, std::move(vals) };
    PureCache &cache = ctx.frame->ensure_pure_cache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        ctx.frame->at(in->target).put(EvalValue(it->second));
        return true;
    }
    pending = std::make_unique<PureCacheKey>(std::move(key));
    return false;
}

/*
 * The exceptional FRAME WALK (cold; the invocation's single landing pad):
 * dispatch to a handler in the current frame, else - for an in-VM frame -
 * append its backtrace frame (exactly what its do_func_call catch used to
 * record), pop it, flush the CALL op's inlined frames at the parent's call
 * pc (what the caller's signal check used to do), and continue in the
 * parent. Reaching the invocation's BOUNDARY frame converts to the
 * g_vm_exc_pending signal (false) - do_func_call above captures that frame
 * and propagates, the Inc-v2 contract. True = dispatched (chunk/pc set).
 */
static ML_COLD bool
vm_unwind_walk(VmActivation &act, EvalContext &ctx, const Chunk *&chunk,
               size_t &pc, std::unique_ptr<RuntimeException> ex)
{
    for (;;) {
        VmCallRec &cur = act.records.back();
        if (vm_dispatch_exc(act, cur, pc)) {
            cur.exc = std::move(ex);           /* like saved_ex */
            return true;
        }
        if (cur.boundary) {
            g_vm_exc_pending = std::move(ex);
            return false;
        }
        vm_capture_rec_frame(*ex, cur);
        ctx.captures = cur.caller_captures;
        chunk = cur.ret_chunk;
        pc = cur.ret_pc - 1;                   /* the call op */
        act.pop_window();
        vm_flush_inline(*chunk, pc, *ex);
    }
}

void
vm_run_chunk(const Chunk &chunk0, EvalContext &ctx)
{
    /* The CURRENT chunk - reseatable LOOP STATE (the native call stack: an
     * in-VM call switches it to the callee's chunk; a pop switches it
     * back). Every op reads the current chunk through it. */
    const Chunk *chunk = &chunk0;

    /*
     * Every invocation runs on an ACTIVATION whose TOP record is this
     * entry's BOUNDARY frame - normally pushed by the caller (vm_run for
     * main, do_func_call for a chunked body). A stray entry (no activation,
     * or no matching record: a plain-Frame fallback body, a 0-slot main)
     * gets a 0-slot boundary record - the SLOTS stay wherever ctx.frame
     * points; only the per-frame handler/iterator/exception state lives in
     * the record. Per-frame state (was locals here, one frame per
     * invocation): handlers/iterators are watermarked slices of the
     * activation's shared stacks; exc/pend live in the record.
     */
    std::unique_ptr<VmActivation> local_act;
    struct EntryGuard {
        bool swapped = false, pushed = false;
        ~EntryGuard() {
            if (pushed)
                g_vm_act->pop_window();
            if (swapped)
                g_vm_act = nullptr;
        }
    } entry_guard;

    VmActivation &act = *vm_enter_invocation_fast(chunk, local_act,
                                                  entry_guard.swapped,
                                                  entry_guard.pushed);

    /*
     * The CURRENT chunk's instruction array, cached as loop state: `chunk`
     * is a reseatable pointer (an in-VM call switches it), so the compiler
     * cannot hoist `chunk->code.data()` across dispatches - the cache
     * removes a double-load from EVERY dispatch (+1.7-2.1% instructions on
     * pure loops, measured). Refreshed at the FOUR places chunk changes:
     * enter/leave call, the unwind dispatch (vm_resume), and entry.
     */
    const Instr *code = chunk->code.data();
    size_t pc = 0;
    /* CachedCallV's pending {func,args} key between the cache miss and the
     * frame push. LOOP-scope (like `in`): clang forbids an INDIRECT goto
     * from exiting a scope with a live non-trivial local, and every label
     * lives inside this one, so no dispatch ever exits it. */
    std::unique_ptr<PureCacheKey> pending_key;
    auto cur_rec = [&]() -> VmCallRec & { return act.records.back(); };
    auto diter = [&](int_type i) -> DictIterState & {
        return act.dict_iters[cur_rec().diter_base + i];
    };
    auto dyiter = [&](int_type i) -> DynIterState & {
        return act.dyn_iters[cur_rec().dyiter_base + i];
    };


    /* Inc 0 (P8): the exception BOUNDARY (plans/vm-exceptions.md). It routes a
     * RuntimeException thrown by any op (a runtime-library error, a fallback
     * `throw`, or a callee) into the VM handler stack: on a caught exception
     * with an active handler, resume at its catch-dispatch pc. PROVEN hot-path-
     * neutral (zero-cost EH: 0.97-1.00 on the dispatch-bound VM benches), so it
     * wraps the dispatch loop directly - no cold-wrapper needed. Body
     * deliberately NOT reindented (a 1270-line switch; indentation is cosmetic).
     * `pc` lives out here so the catch can set it before `goto vm_resume`. */
  vm_resume:
    code = chunk->code.data();
    try {

    const Instr *in;

#ifdef ML_CGOTO
    /* The label-address dispatch table, generated in ENUM ORDER from
     * ML_FOR_EACH_OPCODE (bytecode.h, order/coverage static-asserted
     * there). Static: label addresses are link-time constants and the
     * table is shared by every (recursive) activation. */
#define ML_OP_LBLADDR(N) &&VM_CASE(N),
    static const void *const vm_optbl[] = {
        ML_FOR_EACH_OPCODE(ML_OP_LBLADDR)
    };
#undef ML_OP_LBLADDR
    static_assert(sizeof(vm_optbl) / sizeof(vm_optbl[0])
                      == static_cast<size_t>(OpCode::OpCount_),
                  "vm_optbl must cover every opcode");
  vm_dispatch_cold:      /* direct-goto re-dispatch target (VM_NEXT_COLD) */
    VM_NEXT;
#else
    for (; ; ) {

        in = &code[pc];

        switch (in->op) {
#endif

        VM_CASE(Jump):
            pc = in->target;
            VM_NEXT;


        VM_CASE(IncDecCheckedV): {
            /* A dyn/general scalar `--d`/`d++`: throw if not int/float
             * (inc-dec is int/float-ONLY), else apply +-1 in place. Slot kind:
             * 0 local / 1 global (defined-guarded) / 2 capture. */
            LValue *lvp;
            if (in->target2 == 1) {
                if (!ctx.gfuncs->defined[in->target]) {
                    Loc s, en;
                    chunk->loc_at(pc, s, en);
                    throw UndefinedVariableEx(
                        ctx.gfuncs->names[in->target]->val, s, en);
                }
                lvp = &ctx.gfuncs->slots[in->target];
            } else if (in->target2 == 2) {
                lvp = &(*ctx.captures)[in->target];
            } else {
                lvp = &ctx.frame->at(in->target);
            }
            LValue &lv = *lvp;
            EvalValue nv = lv.get();
            if (!nv.is<int_type>() && !nv.is<float_type>()) {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw TypeErrorEx("'++'/'--' requires an int or float", s, en);
            }
            num_bin_op(nv, EvalValue(static_cast<int_type>(1)),
                       binop_pmf(in->a_lit() ? Op::plus : Op::minus));
            lv.put(std::move(nv));
            pc++;
        }
        VM_NEXT;

        VM_CASE(IncDecElemCheckedV): {
            /* `c[k]++` / `c[k]--` on a dyn/unproven base: form the element
             * LValue, enforce int/float, apply +-1 (statement). Base kind in
             * .target (0 loc / 1 gbl / 2 cap), base slot in .target2, key temp
             * in .a, +/- in .aop. AST-FREE: its TWO error carets - the
             * SUBSCRIPT loc (a subscript-internal KeyNotFound/OOB) vs the
             * INC-DEC loc (its own NotLValue/const/TypeError) - come from the
             * incdec_sites pool (`b`); the undefined-global-base caret from
             * the loc side table (vm_store_base, node = null). */
            const Chunk::IncDecSite &site = chunk->incdec_sites[in->b_lit()];
            LValue *blv =
                vm_store_base(ctx, in->target, in->target2, *chunk, pc, nullptr);
            const EvalValue &key = ctx.frame->at(in->a_slot()).get();
            vm_incdec_elem(blv, key, in->aop == Op::plus,
                           site.lstart, site.lend, site.istart, site.iend);
            pc++;
        }
        VM_NEXT;

        VM_CASE(IncDecChainV):
            vm_incdec_chain_op(ctx, *chunk, *in, pc);
            pc++;
            VM_NEXT;

        VM_CASE(IncDecMemberCheckedV): {
            /* `d.f++` / `d.f--` on a dyn/unproven base: form the member LValue
             * (struct field / dict value), enforce int/float, apply +-1.
             * AST-FREE: the member key + its TWO carets - the MEMBER loc (a
             * KeyNotFound) vs the INC-DEC loc (its own NotLValue/const/
             * TypeError) - come from the incdec_sites pool (`b`). */
            const Chunk::IncDecSite &site = chunk->incdec_sites[in->b_lit()];
            LValue *blv =
                vm_store_base(ctx, in->target, in->target2, *chunk, pc, nullptr);
            vm_incdec_member(blv, site.memId, site.memUid, in->aop == Op::plus,
                             site.lstart, site.lend, site.istart, site.iend);
            pc++;
        }
        VM_NEXT;

        /*
         * B1/B2 specialized arithmetic (see bytecode.h): per-operator,
         * per-shape variants of IntBin/FloatBin - no inner `aop` switch, no
         * `is_lit` operand decode. Selected post-codegen by
         * specialize_arith_ops; none of these can throw (div stays IntBin;
         * IntModRI's immediate is nonzero by selection - ML_VM_CHECKed;
         * shifts go through bit_shl/bit_shr for the exact count semantics,
         * incl. the negative-count InvalidValueEx, which reaches the
         * boundary walk like any C++ throw).
         */
#define ML_IRR(NAME, EXPR)                                                   \
        VM_CASE(NAME): {                                                     \
            const int_type a = read_int_slot(&ctx, in->a_slot());              \
            const int_type b = read_int_slot(&ctx, in->b_slot());              \
            write_int_slot(&ctx, in->target, (EXPR));                        \
            pc++;                                                            \
        }                                                                    \
        VM_NEXT;
#define ML_IRI(NAME, EXPR)                                                   \
        VM_CASE(NAME): {                                                     \
            const int_type a = read_int_slot(&ctx, in->a_slot());              \
            const int_type b = in->b_lit();                                    \
            write_int_slot(&ctx, in->target, (EXPR));                        \
            pc++;                                                            \
        }                                                                    \
        VM_NEXT;
#define ML_FRR(NAME, EXPR)                                                   \
        VM_CASE(NAME): {                                                     \
            const float_type a = read_float_slot(&ctx, in->a_slot());          \
            const float_type b = read_float_slot(&ctx, in->b_slot());          \
            write_float_slot(&ctx, in->target, (EXPR));                      \
            pc++;                                                            \
        }                                                                    \
        VM_NEXT;
#define ML_FRI(NAME, EXPR)                                                   \
        VM_CASE(NAME): {                                                     \
            const float_type a = read_float_slot(&ctx, in->a_slot());          \
            const float_type b = in->b_flit();                                 \
            write_float_slot(&ctx, in->target, (EXPR));                      \
            pc++;                                                            \
        }                                                                    \
        VM_NEXT;

        ML_IRR(IntAddRR, a + b)
        ML_IRI(IntAddRI, a + b)
        ML_IRR(IntSubRR, a - b)
        ML_IRI(IntSubRI, a - b)
        ML_IRR(IntMulRR, a * b)
        ML_IRI(IntMulRI, a * b)
        ML_IRR(IntAndRR, a & b)
        ML_IRI(IntAndRI, a & b)
        ML_IRR(IntOrRR,  a | b)
        ML_IRI(IntOrRI,  a | b)
        ML_IRR(IntXorRR, a ^ b)
        ML_IRI(IntXorRI, a ^ b)
        ML_IRR(IntShlRR, bit_shl(a, b))
        ML_IRI(IntShlRI, bit_shl(a, b))
        ML_IRR(IntShrRR, bit_shr(a, b))
        ML_IRI(IntShrRI, bit_shr(a, b))

        VM_CASE(IntModRI): {
            /* selected only for a NONZERO immediate - no zero check */
            ML_VM_CHECK(in->b_lit() != 0);
            write_int_slot(&ctx, in->target,
                           read_int_slot(&ctx, in->a_slot()) % in->b_lit());
            pc++;
        }
        VM_NEXT;

        ML_FRR(FloatAddRR, a + b)
        ML_FRI(FloatAddRI, a + b)
        ML_FRR(FloatSubRR, a - b)
        ML_FRI(FloatSubRI, a - b)
        ML_FRR(FloatMulRR, a * b)
        ML_FRI(FloatMulRI, a * b)

        VM_CASE(MathFnV): {
            write_float_slot(&ctx, in->target, vm_math_fn(*in, &ctx));
            pc++;
        }
        VM_NEXT;

#undef ML_IRR
#undef ML_IRI
#undef ML_FRR
#undef ML_FRI

        VM_CASE(IntBin): {

            const int_type a = read_int_operand(in->a(), &ctx);
            const int_type b = read_int_operand(in->b(), &ctx);
            int_type r;

            switch (in->aop) {
            case Op::plus:  r = a + b; break;
            case Op::minus: r = a - b; break;
            case Op::times: r = a * b; break;
            case Op::div:
                if (b == 0) {
                    if (!vm_raise(chunk, pc, act, ctx,
                                  std::make_unique<DivisionByZeroEx>()))
                        return;       /* boundary: signal set */
                    code = chunk->code.data();
                    VM_NEXT;          /* dispatched: skip the write */
                }
                r = a / b; break;
            case Op::mod:
                if (b == 0) {
                    if (!vm_raise(chunk, pc, act, ctx,
                                  std::make_unique<DivisionByZeroEx>()))
                        return;
                    code = chunk->code.data();
                    VM_NEXT;
                }
                r = a % b; break;
            case Op::band: r = a & b;          break;
            case Op::bor:  r = a | b;          break;
            case Op::bxor: r = a ^ b;          break;
            case Op::shl:  r = bit_shl(a, b);  break;
            case Op::shr:  r = bit_shr(a, b);  break;
            case Op::ushr: r = bit_ushr(a, b); break;
            default: throw InternalErrorEx();
            }

            write_int_slot(&ctx, in->target, r);
            pc++;
        }
        VM_NEXT;

        VM_CASE(JumpUnlessIntCmp): {

            const int_type a = read_int_operand(in->a(), &ctx);
            const int_type b = read_int_operand(in->b(), &ctx);
            bool cond;

            switch (in->aop) {
            case Op::lt: cond = a <  b; break;
            case Op::gt: cond = a >  b; break;
            case Op::le: cond = a <= b; break;
            case Op::ge: cond = a >= b; break;
            case Op::eq: cond = a == b; break;
            default:     cond = a != b; break;   /* noteq */
            }

            if (cond)
                pc++;
            else
                pc = in->target;
        }
        VM_NEXT;

        VM_CASE(FloatBin): {

            const float_type a = read_float_operand(in->a(), &ctx);
            const float_type b = read_float_operand(in->b(), &ctx);
            float_type r;

            switch (in->aop) {
            case Op::plus:  r = a + b; break;
            case Op::minus: r = a - b; break;
            case Op::times: r = a * b; break;
            case Op::div:
                if (b == 0.0) {
                    if (!vm_raise(chunk, pc, act, ctx,
                                  std::make_unique<DivisionByZeroEx>()))
                        return;       /* boundary: signal set */
                    code = chunk->code.data();
                    VM_NEXT;          /* dispatched: skip the write */
                }
                r = a / b; break;
            case Op::mod:
                if (b == 0.0) {
                    if (!vm_raise(chunk, pc, act, ctx,
                                  std::make_unique<DivisionByZeroEx>()))
                        return;
                    code = chunk->code.data();
                    VM_NEXT;
                }
                r = std::fmod(a, b); break;
            default: throw InternalErrorEx();
            }

            write_float_slot(&ctx, in->target, r);
            pc++;
        }
        VM_NEXT;

        VM_CASE(JumpUnlessFloatCmp): {

            const float_type a = read_float_operand(in->a(), &ctx);
            const float_type b = read_float_operand(in->b(), &ctx);
            bool cond;

            switch (in->aop) {
            case Op::lt: cond = a <  b; break;
            case Op::gt: cond = a >  b; break;
            case Op::le: cond = a <= b; break;
            case Op::ge: cond = a >= b; break;
            case Op::eq: cond = a == b; break;
            default:     cond = a != b; break;   /* noteq */
            }

            if (cond)
                pc++;
            else
                pc = in->target;
        }
        VM_NEXT;

        VM_CASE(ForLoopStep): {

            /* i += step (or -=); if (i <aop> bound) loop back, else exit. One
             * dispatch for the whole counter (see bytecode.h). */
            LValue &ilv = ctx.frame->at(in->target2);
            int_type i = ilv.getval<int_type>();
            const int_type step = read_int_operand(in->b(), &ctx);

            i = (in->aop == Op::lt || in->aop == Op::le) ? i + step : i - step;
            ilv.getval<int_type>() = i;   /* counter slot always holds int */

            const int_type bound = read_int_operand(in->a(), &ctx);
            bool go;
            switch (in->aop) {
            case Op::lt: go = i <  bound; break;
            case Op::le: go = i <= bound; break;
            case Op::ge: go = i >= bound; break;
            default:     go = i >  bound; break;   /* gt */
            }

            if (go)
                pc = in->target;
            else
                pc++;
        }
        VM_NEXT;

        VM_CASE(IntAddModRI): {
            /* E4 fusion: dst = (a + b) % imm - the checksum shape. Never
             * throws (imm nonzero by selection, the add wraps). */
            const int_type av = in->a_is_lit()
                ? in->a_lit() : read_int_slot(&ctx, in->a_slot());
            const int_type bv = in->b_is_lit()
                ? in->b_lit() : read_int_slot(&ctx, in->b_slot());
            ML_VM_CHECK(in->target2 != 0);
            write_int_slot(&ctx, in->target,
                           (av + bv) % static_cast<int_type>(in->target2));
            pc++;
        }
        VM_NEXT;

        VM_CASE(JumpUnlessElemInt): {
            /* E4 fusion: `if (arr[i]) ...` - LoadElemInt + JumpUnlessTrueV in
             * one dispatch (the sieve test). Same read/bounds as LoadElemInt
             * (whose node/loc this op KEPT - the OOB caret is byte-identical);
             * the elem temp was proven dead on both paths, so nothing is
             * written. */
            const EvalValue &base = ctx.frame->at(in->target2).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = read_int_operand(in->a(), &ctx);
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                    Loc ls, le;
                    chunk->loc_at(pc, ls, le);
                    throw OutOfBoundsEx(ls, le);
                }
                const size_type at = arr.offset() + idx;
                int_type v;
                if (arr.skind() == SharedArrayObj::Storage::ints)
                    v = arr.flat_ints()[at];
                else if (arr.skind() == SharedArrayObj::Storage::bools)
                    v = arr.flat_bools()[at] ? 1 : 0;
                else
                    v = arr.get_vec()[at].getval<int_type>();
                if (v == 0) {
                    pc = in->target;
                    VM_NEXT;
                }
            } else {
                throw InternalErrorEx();   /* base proven an array */
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(IntAddStep): {
            /* #9 fusion: accumulate-then-step - `s += x; i++/i--; test;
             * branch` in one dispatch. Never throws (the add wraps). */
            const int_type bv = read_int_operand(in->b(), &ctx);
            const int adst = in->a_dual_lo();
            write_int_slot(&ctx, adst, read_int_slot(&ctx, adst) + bv);

            LValue &ilv = ctx.frame->at(in->target2);
            int_type i = ilv.getval<int_type>();
            i = (in->aop == Op::lt || in->aop == Op::le) ? i + 1 : i - 1;
            ilv.getval<int_type>() = i;

            const int_type bound = in->a_is_lit()
                ? static_cast<int_type>(in->a_dual_hi())
                : read_int_slot(&ctx, in->a_dual_hi());
            bool go;
            switch (in->aop) {
            case Op::lt: go = i <  bound; break;
            case Op::le: go = i <= bound; break;
            case Op::ge: go = i >= bound; break;
            default:     go = i >  bound; break;   /* gt */
            }

            if (go)
                pc = in->target;
            else
                pc++;
        }
        VM_NEXT;

        VM_CASE(ForStepElemInt): {
            /* #9 fusion: step + test + the back-edge element load in one
             * dispatch (`for i: ... a[i]`). On a taken branch the load runs
             * here (idx = the freshly-stepped counter) and control lands
             * PAST the original load (target = load pc + 1); the exit path
             * does NOT load (the counter is out of range there). The OOB
             * caret is the LOAD's (this pc's loc entry). */
            LValue &ilv = ctx.frame->at(in->target2);
            int_type i = ilv.getval<int_type>();
            i = (in->aop == Op::lt || in->aop == Op::le) ? i + 1 : i - 1;
            ilv.getval<int_type>() = i;

            const int_type bound = read_int_operand(in->a(), &ctx);
            bool go;
            switch (in->aop) {
            case Op::lt: go = i <  bound; break;
            case Op::le: go = i <= bound; break;
            case Op::ge: go = i >= bound; break;
            default:     go = i >  bound; break;   /* gt */
            }

            if (!go) {
                pc++;
                VM_NEXT;
            }

            const EvalValue &base = ctx.frame->at(in->b_dual_lo()).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = i;
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                    Loc ls, le;
                    chunk->loc_at(pc, ls, le);
                    throw OutOfBoundsEx(ls, le);
                }
                const size_type at = arr.offset() + idx;
                int_type v;
                if (arr.skind() == SharedArrayObj::Storage::ints)
                    v = arr.flat_ints()[at];
                else if (arr.skind() == SharedArrayObj::Storage::bools)
                    v = arr.flat_bools()[at] ? 1 : 0;
                else
                    v = arr.get_vec()[at].getval<int_type>();
                write_int_slot(&ctx, in->b_dual_hi(), v);
            } else {
                throw InternalErrorEx();   /* base proven an array */
            }
            pc = in->target;
        }
        VM_NEXT;

        VM_CASE(StructFieldAddInt): {
            /* #9 fusion: `dst = other + a[i].f` (general 3-address - the
             * struct-foreach reduction chains adds through temps). The
             * field read is proven no-fault; the add wraps. b_dual =
             * (field idx, other slot). */
            const int_type add =
                vm_struct_field_int(ctx.frame->at(in->target2).get(),
                                    read_int_operand(in->a(), &ctx),
                                    in->b_dual_lo());
            write_int_slot(&ctx, in->target,
                           read_int_slot(&ctx, in->b_dual_hi()) + add);
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadElemInt): {

            /* a[i] into a temp (mirrors Subscript::eval_int for a flat array;
             * a dict / general base falls back to the node). */
            const EvalValue &base = ctx.frame->at(in->target2).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = read_int_operand(in->a(), &ctx);
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                    Loc ls, le;
                    chunk->loc_at(pc, ls, le);
                    throw OutOfBoundsEx(ls, le);
                }
                const size_type at = arr.offset() + idx;
                int_type v;
                if (arr.skind() == SharedArrayObj::Storage::ints)
                    v = arr.flat_ints()[at];
                else if (arr.skind() == SharedArrayObj::Storage::bools)
                    v = arr.flat_bools()[at] ? 1 : 0;
                else
                    v = arr.get_vec()[at].getval<int_type>();
                write_int_slot(&ctx, in->target, v);
            } else {
                /* base_array is PROVEN at this op, so the base is always an
                 * array here - the old node->eval_int fallback was unreachable
                 * (an invariant net; freeing it dropped the op's node). */
                throw InternalErrorEx();
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadElemFloat): {

            const EvalValue &base = ctx.frame->at(in->target2).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = read_int_operand(in->a(), &ctx);
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                    Loc ls, le;
                    chunk->loc_at(pc, ls, le);
                    throw OutOfBoundsEx(ls, le);
                }
                const size_type at = arr.offset() + idx;
                float_type v;
                if (arr.skind() == SharedArrayObj::Storage::floats)
                    v = arr.flat_floats()[at];
                else if (arr.skind() == SharedArrayObj::Storage::ints)
                    v = static_cast<float_type>(arr.flat_ints()[at]);
                else
                    v = arr.get_vec()[at].getval<float_type>();
                write_float_slot(&ctx, in->target, v);
            } else {
                throw InternalErrorEx();   /* unreachable: base_array proven */
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadElemBool): {
            /* bool-foreach loop var: bind a[i] as a real BOOL (not 0/1), so
             * `print(x)` shows true/false. `i` is loop-bounded (< ArrLen); the
             * base is a proven flat array<bool> (elem_is_bool). */
            const SharedArrayObj &arr =
                ctx.frame->at(in->target2).get().get_ref<SharedArrayObj>();
            const int_type idx = read_int_operand(in->a(), &ctx);
            const bool b =
                arr.skind() == SharedArrayObj::Storage::bools
                    ? arr.flat_bools()[arr.offset() + idx] != 0
                    : arr.get_view()[idx].get().get<bool>();  /* general fallbk */
            ctx.frame->at(in->target).put(EvalValue(b));
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadElemValue): {

            /* a[i] (an array-valued element of a GENERAL array) into a temp
             * slot, so a 2-D read `a[i][k]` is native (both indices). The base
             * is a PROVEN general array (base_array + a general element type),
             * so the old non-general node->eval fallback was unreachable. */
            const EvalValue &base = ctx.frame->at(in->target2).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                if (arr.skind() == SharedArrayObj::Storage::general) {
                    int_type idx = read_int_operand(in->a(), &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                        Loc ls, le;
                        chunk->loc_at(pc, ls, le);
                        throw OutOfBoundsEx(ls, le);
                    }
                    ctx.frame->at(in->target).put(
                        arr.get_vec()[arr.offset() + idx].get());
                    pc++;
                    VM_NEXT;
                }
                if (arr.skind() == SharedArrayObj::Storage::strs) {
                    /* Flat STRING array (top-10 #7): an array<str> is a
                     * general-ELEMENT type statically but may hold flat strs
                     * storage - bind a boxed SharedStr copy (a handle;
                     * strings are immutable, so a copy == the tree-walker's
                     * reference bind). */
                    int_type idx = read_int_operand(in->a(), &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                        Loc ls, le;
                        chunk->loc_at(pc, ls, le);
                        throw OutOfBoundsEx(ls, le);
                    }
                    ctx.frame->at(in->target).put(EvalValue(
                        SharedStr(arr.flat_strs()[arr.offset() + idx])));
                    pc++;
                    VM_NEXT;
                }
            }
            throw InternalErrorEx();   /* unreachable: base_array general proven */
        }

        VM_CASE(DictIterInit): {

            /* Pin the dict (an intrusive_ptr copy keeps it alive for the loop,
             * matching the tree-walker's lifetime-extended `cval`) and set the
             * live iterator to begin(). The inferencer proved a Dict static
             * type; the ML_VM_CHECKs are the hardening net. */
            ML_VM_CHECK(in->target >= 0
                && in->target < static_cast<int_type>(chunk->n_dict_iters));
            const EvalValue &base = ctx.frame->at(in->target2).get();
            ML_VM_CHECK(base.is<intrusive_ptr<DictObject>>());
            DictIterState &st = diter(in->target);
            st.dict = base.get<intrusive_ptr<DictObject>>();
            st.it = st.dict->get_ref().begin();
            pc++;
        }
        VM_NEXT;

        VM_CASE(DictIterNext): {

            /* Test the live iterator: on end jump to end_pc; else bind the key
             * (and value) box-free - a plain EvalValue copy (LoadElemValue),
             * matching the tree-walker's `elems = {p.first, p.second.get()}` -
             * then advance. A slot of -1 is a `_` placeholder / the keys-only
             * 1-var form (bind nothing). */
            ML_VM_CHECK(in->target2 >= 0
                && in->target2 < static_cast<int_type>(chunk->n_dict_iters));
            DictIterState &st = diter(in->target2);
            if (st.it == st.dict->get_ref().end()) {
                pc = static_cast<size_t>(in->target);
                VM_NEXT;
            }
            if (in->a_slot() >= 0)
                ctx.frame->at(in->a_slot()).put(st.it->first);
            if (in->b_slot() >= 0)
                ctx.frame->at(in->b_slot()).put(st.it->second.get());
            ++st.it;
            pc++;
        }
        VM_NEXT;

        VM_CASE(ForeachDynInit): {
            /* Dispatch the DYN container once: pin it, record the loop shape
             * (nvars | indexed, the per-var target slots from the
             * unpack_targets pool), and set up an array or dict cursor. An
             * unsupported runtime value throws (loc side table). */
            ML_VM_CHECK(in->target >= 0
                && in->target < static_cast<int_type>(chunk->n_dyn_iters));
            DynIterState &st = dyiter(in->target);
            st.container = ctx.frame->at(in->target2).get();
            st.nvars = static_cast<int>(in->a_lit() & 0xff);
            st.indexed = (in->a_lit() >> 8) != 0;
            st.targets = &chunk->unpack_targets[in->b_lit()];
            st.counter = 0;
            if (st.container.is<SharedArrayObj>()) {
                st.is_dict = false;
                st.idx = 0;
                st.size = st.container.get<SharedArrayObj>().size();
            } else if (st.container.is<intrusive_ptr<DictObject>>()) {
                st.is_dict = true;
                st.it = st.container.get<intrusive_ptr<DictObject>>()
                            ->get_ref().begin();
            } else {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw TypeErrorEx(
                    "foreach: expected an array or dict", s, en);
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(ForeachDynNext): {
            /* On exhaustion jump to end_pc; else bind the loop vars from the
             * state (targets slots, -1 == `_` skipped) exactly as do_iter:
             * `indexed` binds targets[0] = the counter; an ARRAY element binds
             * the single remaining var BOX-FREE (vm_arr_elem) or STRICT-
             * unpacks an array element into the N remaining vars; a DICT
             * binds key [, value [, none...]] (do_iter's count=2 padding).
             * Then advance. */
            ML_VM_CHECK(in->target2 >= 0
                && in->target2 < static_cast<int_type>(chunk->n_dyn_iters));
            DynIterState &st = dyiter(in->target2);
            const std::vector<int32_t> &tg = *st.targets;
            const size_t tb = st.indexed ? 1 : 0;    /* first value var */
            const size_t nv = static_cast<size_t>(st.nvars) - tb;
            auto bind = [&](size_t i, const EvalValue &v) {
                if (tg[i] >= 0)
                    ctx.frame->at(tg[i]).put(v);
            };
            if (!st.is_dict) {
                if (st.idx >= st.size) {
                    pc = static_cast<size_t>(in->target);
                    VM_NEXT;
                }
                if (st.indexed)
                    bind(0, EvalValue(
                        static_cast<int_type>(st.idx)));
                if (nv == 1) {
                    bind(tb, vm_arr_elem(st.container, st.idx));
                } else if (nv >= 2) {
                    /* N-var over an ARRAY: the element must be an array of
                     * EXACTLY nv, unpacked into the vars - do_iter's STRICT
                     * destructure (same messages/loc via the side table). */
                    const EvalValue elem = vm_arr_elem(st.container, st.idx);
                    if (!elem.is<SharedArrayObj>())
                        vm_throw_unpack_nonarray(*chunk, pc,
                                                 static_cast<int>(nv));
                    const SharedArrayObj &sub = elem.get_ref<SharedArrayObj>();
                    if (sub.size() != static_cast<size_type>(nv))
                        vm_throw_unpack_len(*chunk, pc, sub.size(),
                                            static_cast<int>(nv));
                    for (size_t i = 0; i < nv; i++) {
                        bind(tb + i, vm_arr_elem(elem,
                                                 static_cast<size_type>(i)));
                }
                }
                st.idx++;
            } else {
                DictObject &d = *st.container.get<intrusive_ptr<DictObject>>();
                if (st.it == d.get_ref().end()) {
                    pc = static_cast<size_t>(in->target);
                    VM_NEXT;
                }
                if (st.indexed)
                    bind(0, EvalValue(st.counter++));
                /* do_iter's count==2 else-branch: key, value, then `none`
                 * for any further vars. */
                if (nv >= 1)
                    bind(tb, st.it->first);
                if (nv >= 2)
                    bind(tb + 1, st.it->second.get());
                for (size_t i = 2; i < nv; i++) {
                    bind(tb + i, none);
                }
                ++st.it;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(UnpackElemInt):
        VM_CASE(UnpackElemFloat):
        VM_CASE(UnpackElemValue): {

            /* STRICT foreach-unpack: read pairs[i] (a general outer element = a
             * sub-array), check it is an array of EXACTLY N, write its N scalars
             * into the consecutive loop-var slots base..base+N-1 - matching
             * do_iter's strict destructure element for element. `i` is the loop
             * counter (in-range), so the outer read never OOB; the only throws
             * are the two strict errors. */
            const bool is_int = in->op == OpCode::UnpackElemInt;
            const bool is_float = in->op == OpCode::UnpackElemFloat;
            const EvalValue &base_v = ctx.frame->at(in->target2).get();
            ML_VM_CHECK(base_v.is<SharedArrayObj>());
            const SharedArrayObj &outer = base_v.get_ref<SharedArrayObj>();
            const int_type idx = read_int_operand(in->a(), &ctx);
            const EvalValue &elem =
                outer.get_vec()[outer.offset() + idx].get();
            const int_type N = in->b_lit();
            if (!elem.is<SharedArrayObj>())
                vm_throw_unpack_nonarray(*chunk, pc, N);
            const SharedArrayObj &sub = elem.get_ref<SharedArrayObj>();
            if (sub.size() != static_cast<size_type>(N))
                vm_throw_unpack_len(*chunk, pc, sub.size(), N);
            const size_type off = sub.offset();
            const auto sk = sub.skind();
            if (is_int && sk == SharedArrayObj::Storage::ints) {
                for (int_type k = 0; k < N; k++) {
                    write_int_slot(&ctx, in->target + k,
                                   sub.flat_ints()[off + k]);
                }
            } else if (is_float && sk == SharedArrayObj::Storage::floats) {
                for (int_type k = 0; k < N; k++) {
                    write_float_slot(&ctx, in->target + k,
                                     sub.flat_floats()[off + k]);
                }
            } else {
                /* UnpackElemValue (a general/dyn/str/mixed sub-array), OR a flat
                 * op whose sub-array's storage is NOT the expected kind (a
                 * mixed-numeric `[int, float]` literal - inference types it
                 * array<float> by int|float join but builds GENERAL - or a flat
                 * array of the OTHER scalar kind). Bind each element's ACTUAL
                 * boxed value (vm_arr_elem is skind-dispatched) - byte-identical
                 * to do_iter's bind_loop_var, so an int stays int / a str stays
                 * str. */
                for (int_type k = 0; k < N; k++) {
                    ctx.frame->at(in->target + k).put(
                        vm_arr_elem(elem, static_cast<size_type>(k)));
                }
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(UnpackElemTargets): {
            /* STRICT foreach-unpack with a per-position target list (a `_`
             * placeholder / non-consecutive slots): read pairs[i] (a general
             * outer element = a sub-array), check length == N, then bind each
             * element box-free (vm_arr_elem) to targets[k] (skip -1 == `_`). */
            const EvalValue &base_v = ctx.frame->at(in->target2).get();
            ML_VM_CHECK(base_v.is<SharedArrayObj>());
            const SharedArrayObj &outer = base_v.get_ref<SharedArrayObj>();
            const int_type idx = read_int_operand(in->a(), &ctx);
            const EvalValue &elem =
                outer.get_vec()[outer.offset() + idx].get();
            const int_type N = in->b_lit();
            if (!elem.is<SharedArrayObj>())
                vm_throw_unpack_nonarray(*chunk, pc, N);
            const SharedArrayObj &sub = elem.get_ref<SharedArrayObj>();
            if (sub.size() != static_cast<size_type>(N))
                vm_throw_unpack_len(*chunk, pc, sub.size(), N);
            const std::vector<int32_t> &targets =
                chunk->unpack_targets[in->target];
            for (int_type k = 0; k < N; k++) {
                if (targets[k] >= 0)
                    ctx.frame->at(targets[k]).put(
                        vm_arr_elem(elem, static_cast<size_type>(k)));
                }
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreElemInt): {

            /* a[i] = v / a[i] OP= v for a flat mutable int array (mirrors the
             * int path of try_flat_subscript_store, COW), and a[i] = <bool>
             * for a flat bool array (P1: plain assign only - no compound
             * for bool; the value operand is the bool's 0/1, written to bvec).
             * Anything else (const / read-only / general / float / dyn, or a
             * COMPOUND on a bool array) takes the UNIVERSAL vm_subscript_store
             * fallback (box the already-computed index/value operands, map the
             * base op back to its Expr14 op) - AST-free and byte-identical to
             * the tree-walker (the same path StoreElemValue uses). The base may
             * be a global/capture array (in->target = the slot kind); the caret
             * comes from the loc side table - looked up LAZILY, only on the cold
             * throw/fallback paths, so the hot store pays no binary search. */
            LValue &alv =
                *vm_store_base(ctx, in->target, in->target2, *chunk, pc,
                               nullptr);
            if (alv.is<SharedArrayObj>()) {
                SharedArrayObj &arr = alv.getval<SharedArrayObj>();
                const auto sk = arr.skind();
                const bool is_bool = sk == SharedArrayObj::Storage::bools;
                if ((sk == SharedArrayObj::Storage::ints
                     || (is_bool && in->aop == Op::invalid))
                    && !alv.is_const_var() && !arr.is_readonly()) {
                    int_type idx = read_int_operand(in->a(), &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                        vm_throw_oob(*chunk, pc);
                    const int_type rhs = read_int_operand(in->b(), &ctx);
                    /* div/mod by zero throws BEFORE any clone (tree-walker
                     * throws during the op eval, before the COW). */
                    if ((in->aop == Op::div || in->aop == Op::mod) && rhs == 0)
                        vm_throw_div0(*chunk, pc);
                    if (arr.is_slice())
                        arr.clone_internal_vec();
                    else if (arr.use_count() > 1)
                        arr.clone_aliased_slices(arr.offset() + idx);
                    if (is_bool) {
                        arr.flat_bools()[arr.offset() + idx] = rhs ? 1 : 0;
                        arr.invalidate_hash();
                        pc++;
                        VM_NEXT;
                    }
                    int_type &el = arr.flat_ints()[arr.offset() + idx];
                    switch (in->aop) {
                    case Op::invalid: el = rhs;  break;
                    case Op::plus:    el += rhs; break;
                    case Op::minus:   el -= rhs; break;
                    case Op::times:   el *= rhs; break;
                    case Op::div:     el /= rhs; break;
                    case Op::mod:     el %= rhs; break;
                    default: throw InternalErrorEx();
                    }
                    arr.invalidate_hash();
                    pc++;
                    VM_NEXT;
                }
            }
            {
                Loc ls, le;
                chunk->loc_at(pc, ls, le);
                vm_subscript_store(&alv,
                                   EvalValue(read_int_operand(in->a(), &ctx)),
                                   EvalValue(read_int_operand(in->b(), &ctx)),
                                   vm_base_to_expr14_op(in->aop), ls, le);
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreElemFloat): {

            /* Caret from the loc side table, looked up LAZILY on the cold
             * throw/fallback paths only (see StoreElemInt). */
            LValue &alv =
                *vm_store_base(ctx, in->target, in->target2, *chunk, pc,
                               nullptr);
            if (alv.is<SharedArrayObj>()) {
                SharedArrayObj &arr = alv.getval<SharedArrayObj>();
                if (arr.skind() == SharedArrayObj::Storage::floats
                    && !alv.is_const_var() && !arr.is_readonly()) {
                    int_type idx = read_int_operand(in->a(), &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                        vm_throw_oob(*chunk, pc);
                    const float_type rhs = read_float_operand(in->b(), &ctx);
                    if ((in->aop == Op::div || in->aop == Op::mod)
                            && rhs == 0.0)
                        vm_throw_div0(*chunk, pc);
                    if (arr.is_slice())
                        arr.clone_internal_vec();
                    else if (arr.use_count() > 1)
                        arr.clone_aliased_slices(arr.offset() + idx);
                    float_type &el = arr.flat_floats()[arr.offset() + idx];
                    switch (in->aop) {
                    case Op::invalid: el = rhs;               break;
                    case Op::plus:    el += rhs;              break;
                    case Op::minus:   el -= rhs;              break;
                    case Op::times:   el *= rhs;              break;
                    case Op::div:     el /= rhs;              break;
                    case Op::mod:     el = std::fmod(el, rhs); break;
                    default: throw InternalErrorEx();
                    }
                    arr.invalidate_hash();
                    pc++;
                    VM_NEXT;
                }
            }
            {
                Loc ls, le;
                chunk->loc_at(pc, ls, le);
                vm_subscript_store(&alv,
                                   EvalValue(read_int_operand(in->a(), &ctx)),
                                   EvalValue(read_float_operand(in->b(), &ctx)),
                                   vm_base_to_expr14_op(in->aop), ls, le);
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(DictStore): {

            /* d[k] = v / d[k] OP= v (P2): store via the shared, type-dispatched
             * vm_subscript_store (auto-vivify / COW / key-freeze / throw -
             * matches the tree-walker for ANY base type, so no is-dict guard /
             * node->eval fallback is needed). AST-free: the subscript's caret
             * (`d[k]`) comes from the loc side table (recorded by extract_locs
             * from node = the Subscript). */
            LValue &dlv =
                *vm_store_base(ctx, in->target, in->target2, *chunk, pc,
                               nullptr);
            const EvalValue &key = ctx.frame->at(in->a_slot()).get();
            const EvalValue &val = ctx.frame->at(in->b_slot()).get();
            /* Loc looked up LAZILY on the throw path only (a successful store -
             * the hot case - pays no loc_at binary search); vm_subscript_store
             * uses it only when it throws. */
            try {
                vm_subscript_store(&dlv, key, val, in->aop, Loc(), Loc());
            } catch (Exception &e) {
                if (!e.loc_start)
                    chunk->loc_at(pc, e.loc_start, e.loc_end);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreMemberV): {
            /* s.member = v / s.member OP= v for a STRUCT base (a dict member
             * store uses DictStore). vm_member_store does the POD/boxed-field
             * store; AST-free - the member uid + carets come from the pool. */
            const Chunk::MemberKey &mk = chunk->member_keys[in->a_lit()];
            const EvalValue &val = ctx.frame->at(in->b_slot()).get();
            try {
                /* base inside the try so an undefined-global throw is stamped
                 * with the member caret; base may be global/capture (in->target
                 * = kind). */
                LValue *blv = vm_store_base(ctx, in->target, in->target2,
                                            *chunk, pc, nullptr);
                vm_member_store(blv, mk.memUid, in->aop, val,
                                mk.mstart, mk.mend, mk.bstart, mk.bend);
            } catch (Exception &e) {
                if (!e.loc_start) {          /* a compound div/mod is loc-less */
                    e.loc_start = mk.mstart;
                    e.loc_end = mk.mend;
                }
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreElemValue): {

            /* a[i] = v / a[i] OP= v for a GENERAL array (P4): store via the
             * shared, type-dispatched vm_subscript_store (bounds check + COW +
             * slot_rmw - matches the tree-walker for ANY base). AST-free: the
             * subscript's caret comes from the loc side table. */
            LValue &alv =
                *vm_store_base(ctx, in->target, in->target2, *chunk, pc,
                               nullptr);
            const EvalValue &idx = ctx.frame->at(in->a_slot()).get();
            const EvalValue &val = ctx.frame->at(in->b_slot()).get();
            /* Loc looked up LAZILY on the throw path only (see DictStore). */
            try {
                vm_subscript_store(&alv, idx, val, in->aop, Loc(), Loc());
            } catch (Exception &e) {
                if (!e.loc_start)
                    chunk->loc_at(pc, e.loc_start, e.loc_end);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreElem2V): {

            /* a[i][j] = v / OP= v (nested general store): read a[i] as a
             * reference then store [j] into it (two-level vm_subscript_store).
             * AST-free: the per-step carets (inner k1, outer k2) come from the
             * chain_locs pool (idx in a.lit), so an intermediate `a[i]` OOB and
             * the final store carry their OWN subscript loc. */
            LValue &alv = ctx.frame->at(in->target2);
            const EvalValue &k1 = ctx.frame->at(in->a_dual_lo()).get();
            const EvalValue &k2 = ctx.frame->at(in->b_slot()).get();
            const EvalValue &val = ctx.frame->at(in->target).get();
            vm_nested_subscript_store(&alv, k1, k2, val, in->aop,
                                      chunk->chain_locs[in->a_dual_hi()].data());
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreElemChainV): {
            /* GENERIC N-level nested store a[k0][k1]...[kn] = v / OP= v: form the
             * base LValue* (by slot kind = a.lit), then walk the keys run
             * ([b.lit, +nkeys)). AST-free: the per-step subscript carets are in
             * the chain_locs pool (idx in a.slot; nkeys = its size), so each
             * step's throw carries ITS OWN subscript loc. */
            LValue *base = vm_store_base(ctx, in->a_dual_hi(), in->target2,
                                         *chunk, pc, nullptr);
            const EvalValue &val = ctx.frame->at(in->target).get();
            const auto &cl = chunk->chain_locs[in->a_dual_lo()];
            vm_chain_store_op(ctx, base, in->b_lit(), cl.data(), cl.size(),
                              val, in->aop);
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreLValueChainV): {
            /* GENERAL nested store `base.step1.step2... = v` mixing MEMBER +
             * SUBSCRIPT steps. Form the base LValue* (by slot kind = a.lit),
             * walk the steps (chain_steps pool idx = a.slot), store the final.
             * AST-free: all carets come from the loc side table (the outer
             * lvalue node), matching StoreElemChainV. */
            LValue *base = vm_store_base(ctx, in->a_dual_hi(), in->target2,
                                         *chunk, pc, nullptr);
            const EvalValue &val = ctx.frame->at(in->target).get();
            try {
                vm_chain_lvalue_store_op(ctx, *chunk, base, in->a_dual_lo(), val,
                                         in->aop);
            } catch (Exception &e) {
                if (!e.loc_start)
                    chunk->loc_at(pc, e.loc_start, e.loc_end);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(MultiUnpackV): {

            /* Multi-assign `a, b, c = <rvalue>` (F-1): the tree-walker's STRICT
             * destructure, AST-free. An ARRAY rvalue must have EXACTLY as many
             * elements as targets (else the length TypeErrorEx, caret from the
             * side table); each element (box-free for a flat scalar via
             * vm_arr_elem) writes to its target slot (-1 == `_`, skipped). A
             * NON-array rvalue SPREADS to every target. */
            const EvalValue &rval = ctx.frame->at(in->a_slot()).get();
            const std::vector<int32_t> &targets =
                chunk->unpack_targets[in->target];
            /* Typed targets (R5): a PLAIN assign coerces each stored value
             * per target (widen / dyn-narrowing throw, Expr14-span caret);
             * a compound doesn't coerce. Kinds from the unpack_coerce pool. */
            const std::vector<unsigned char> *coerce =
                in->b_is_lit() ? &chunk->unpack_coerce[in->b_lit()] : nullptr;
            /* A COMPOUND `a, b OP= rhs` (in->aop != invalid): each target reads
             * its CURRENT value, applies the op with its element/scalar, writes
             * back — else a plain distribute. */
            const bool compound = in->aop != Op::invalid;
            size_t ti = 0;
            auto store = [&](int32_t t, const EvalValue &v) {
                if (!compound) {
                    if (coerce && (*coerce)[ti]) {
                        try {
                            ctx.frame->at(t).put(vm_coerce_decl_num(
                                v, (*coerce)[ti] == 2));
                        } catch (Exception &e) {
                            vm_stamp_loc(*chunk, pc, e);
                            throw;
                        }
                        return;
                    }
                    ctx.frame->at(t).put(v);
                    return;
                }
                EvalValue nv = ctx.frame->at(t).get();
                try {
                    num_bin_op(nv, v, binop_pmf(in->aop));
                } catch (Exception &e) {
                    vm_stamp_loc(*chunk, pc, e);
                    throw;
                }
                ctx.frame->at(t).put(std::move(nv));
            };
            if (rval.is<SharedArrayObj>()) {
                const size_type m = rval.get_ref<SharedArrayObj>().size();
                if (m != static_cast<size_type>(targets.size()))
                    vm_throw_multi_unpack_len(*chunk, pc, m, targets.size());
                for (size_t i = 0; i < targets.size(); i++) {
                    ti = i;
                    if (targets[i] >= 0)
                        store(targets[i],
                              vm_arr_elem(rval, static_cast<size_type>(i)));
                }
            } else {
                for (size_t i = 0; i < targets.size(); i++) {
                    ti = i;
                    if (targets[i] >= 0)
                        store(targets[i], rval);
                }
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(DictLoadInt):
        VM_CASE(DictLoadFloat): {

            /* Typed dict scalar read d[k] / d.k (P3), AST-FREE (foundation 2):
             * the KEY comes from the CONST POOL for a member `d.k` (in->a is an
             * immediate = the consts index of the interned name) or from a temp
             * slot for a subscript `d[k]` (in->a is a slot) - distinguished by
             * in->a_is_lit(), no `node`. A PRESENT key reads the scalar via
             * dict_present_value (hot); a MISSING key / non-dict base goes
             * through the shared Type::subscript (the tree-walker's exact
             * default-dict insert / KeyNotFoundEx / not-subscriptable logic),
             * its loc taken from the loc side table, NOT node->eval. */
            const bool is_int = in->op == OpCode::DictLoadInt;
            const EvalValue &key = in->a_is_lit()
                ? chunk->consts[in->a_lit()]
                : ctx.frame->at(in->a_slot()).get();
            const EvalValue &base = ctx.frame->at(in->target2).get();
            if (base.is<intrusive_ptr<DictObject>>())
                if (const EvalValue *v = dict_present_value(
                        base.get_ref<intrusive_ptr<DictObject>>(), key)) {
                    write_scalar_slot(&ctx, in->target, is_int, *v);
                    pc++;
                    VM_NEXT;
                }
            /* cold: missing key (default insert / throw) or a non-dict base. */
            LValue &dlv = ctx.frame->at(in->target2);
            EvalValue r;
            try {
                r = base.get_type()->subscript(EvalValue(&dlv), key, false);
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            write_scalar_slot(&ctx, in->target, is_int,
                              r.is<LValue *>() ? r.get<LValue *>()->get() : r);
            pc++;
        }
        VM_NEXT;

        VM_CASE(CallBuiltinV): {

            /* Native builtin call (value ABI): copy the pre-evaluated args from
             * the register run into a buffer and call func_v with the AST-free
             * ArgLocs (carets/hint) from the builtin_calls pool - NO node. The
             * try/catch stamps the args-list loc onto a loc-less error, exactly
             * as DirectBuiltinCallExpr::do_eval does. */
            const Chunk::BuiltinCall &bc = chunk->builtin_calls[in->target2];
            const int_type base = in->a_lit(), n = in->b_lit();
            try {
                if (n <= 8) {
                    EvalValue stackbuf[8];
                    for (int_type i = 0; i < n; i++) {
                        stackbuf[i] = ctx.frame->at(base + i).get();
                }
                    ArgLocs al = chunk->arglocs_at(in->target2);
                    ctx.frame->at(in->target).put(
                        bc.builtin.func_v(&ctx, &al, stackbuf, n));
                } else {
                    ctx.frame->at(in->target).put(
                        vm_call_builtin_big(ctx, *chunk, in->target2, base, n));
                }
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = bc.start;
                    e.loc_end = bc.end;
                }
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(MakeArrayV): {

            /* Build an array LITERAL via vm_make_array (its element buffer is
             * kept OUT of vm_run_chunk's frame - see that helper; the frame is
             * multiplied by VM recursion depth). `target2` = the ArrHint; never
             * throws. */
            ctx.frame->at(in->target).put(vm_make_array(
                ctx, in->a_lit(), in->b_lit(), static_cast<ArrHint>(in->target2)));
            pc++;
        }
        VM_NEXT;

        VM_CASE(MakeDictV): {

            /* Build a dict LITERAL via vm_make_dict (key/value buffer kept out
             * of vm_run_chunk's frame). Never throws (all values hashable). */
            ctx.frame->at(in->target).put(
                vm_make_dict(ctx, in->a_lit(), in->b_lit()));
            pc++;
        }
        VM_NEXT;

        VM_CASE(MakeClosureV): {
            /* func[caps]{..} in expression position: create the FuncObject +
             * snapshot the captures from ctx - byte-identical to
             * FuncDeclStmt::do_eval for a lambda. The def is a program-lifetime
             * FuncDescriptor* from the pool (the Instr holds only the index) -
             * no AST. The ctor never throws (a resolved closure's captures are
             * defined), so no loc. */
            ctx.frame->at(in->target).put(EvalValue(intrusive_ptr<FuncObject>(
                make_intrusive<FuncObject>(chunk->closure_defs[in->target2],
                                           &ctx))));
            pc++;
        }
        VM_NEXT;

        VM_CASE(StructCtorV): {
            /* Standalone POD struct construction P(x,y) via vm_struct_ctor (its
             * field buffer kept out of vm_run_chunk's frame). The typed-arg gate
             * means coerce won't throw; a defensive throw is stamped with the
             * ctor's loc (side table). */
            StructTypeDef *def =
                const_cast<StructTypeDef *>(chunk->struct_defs[in->target2]);
            try {
                vm_struct_ctor(ctx, def, in->a_lit(), in->b_lit(), in->target);
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(ThrowRuntimeV): {
            /* An always-throwing construct (undefined name, assign to a
             * non-lvalue / a builtin) — throw the pooled exception with its
             * exact caret, byte-identical to the tree-walker. */
            const Chunk::ThrowSite &t = chunk->throws[in->target];
            switch (t.kind) {
                case Chunk::ThrowKind::undefined_var:
                    throw UndefinedVariableEx(t.name->val, t.start, t.end);
                case Chunk::ThrowKind::not_lvalue:
                    throw NotLValueEx(t.start, t.end);
                case Chunk::ThrowKind::rebind_builtin:
                    throw CannotRebindBuiltinEx(t.start, t.end);
                case Chunk::ThrowKind::rebind_const:
                    throw CannotRebindConstEx(t.start, t.end);
                case Chunk::ThrowKind::bad_args:
                    throw InvalidNumberOfArgsEx(t.start, t.end);
            }
            VM_NEXT;   /* unreachable */
        }

        VM_CASE(StructCtorBoxedV): {
            /* Boxed (non-POD) struct construction B(a,x) via
             * vm_struct_ctor_boxed. A field coerce CAN throw (a dyn-laundered
             * wrong value); the per-arg carets come from the boxed_ctors pool so
             * the throw's caret matches the tree-walker's construct_struct. */
            const Chunk::BoxedCtor &bc = chunk->boxed_ctors[in->target2];
            StructTypeDef *def = const_cast<StructTypeDef *>(bc.def);
            ctx.frame->at(in->target).put(vm_struct_ctor_boxed(
                ctx, def, in->a_lit(),
                static_cast<int_type>(bc.arg_locs.size()),
                bc.arg_locs.data()));
            pc++;
        }
        VM_NEXT;

        VM_CASE(MakeStructArrayV): {
            /* Build a FLAT array<PodStruct> literal `[P(..), ..]` in one op via
             * vm_make_struct_array_op (field-value buffer kept out of
             * vm_run_chunk's frame - it coerces STRAIGHT into a flat byte buffer,
             * no per-element StructObject). All-scalar-field gate => coerce can't
             * throw; a defensive throw gets the ctor loc (side table). */
            StructTypeDef *def =
                const_cast<StructTypeDef *>(chunk->struct_defs[in->target2]);
            try {
                vm_make_struct_array_op(ctx, def, in->a_lit(), in->b_lit(),
                                        in->target);
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadStructFieldInt):
            /* pts[i].f (scalar int/bool field) read straight from the flat
             * struct-array bytes into a slot - the struct-foreach direct read,
             * no StructObject. target2 = the array slot, a = the counter, b =
             * the field index. */
            write_int_slot(&ctx, in->target,
                vm_struct_field_int(ctx.frame->at(in->target2).get(),
                                    read_int_operand(in->a(), &ctx), in->b_lit()));
            pc++;
            VM_NEXT;

        VM_CASE(LoadStructElemV):
            /* Whole-`p` foreach bind: materialize a fresh StructObject from the
             * flat struct-array element into the loop var. target = loop var,
             * target2 = the array slot, a = the counter. */
            ctx.frame->at(in->target).put(
                vm_struct_elem(ctx.frame->at(in->target2).get(),
                               read_int_operand(in->a(), &ctx)));
            pc++;
            VM_NEXT;

        VM_CASE(LoadStructFieldFloat):
            write_float_slot(&ctx, in->target,
                vm_struct_field_float(ctx.frame->at(in->target2).get(),
                                      read_int_operand(in->a(), &ctx),
                                      in->b_lit()));
            pc++;
            VM_NEXT;

        VM_CASE(AppendV): {
            /* D1: `append(a, x)` / `push(a, x)` - the CallBuiltinLV shape
             * with the marshaling deleted. arr_append_fast appends a fitting
             * element in place (flat or general, hash maintained); any other
             * shape (const/readonly/slice/non-array/flat mismatch/undefined
             * global -> null target) falls back to the FULL builtin via the
             * pooled carets - byte-identical errors and result. */
            LValue *target;
            switch (in->a_dual_hi()) {
            case 0:  target = &ctx.frame->at(in->target2); break;
            case 1:  target = ctx.gfuncs->defined[in->target2]
                                  ? &ctx.gfuncs->slots[in->target2]
                                  : nullptr;                     break;
            default: target = &(*ctx.captures)[in->target2];    break;
            }
            const EvalValue &elem = ctx.frame->at(in->b_lit()).get();
            if (target && arr_append_fast(target, elem, false)) {
                /* Call-cluster #4: a DISCARDED result (`append(a, x);` as a
                 * statement - the peephole proved the dst temp dead and set
                 * it to -1) skips materializing the array handle: one
                 * intrusive refcount round-trip + 32B copy per append. */
                if (in->target >= 0)
                    ctx.frame->at(in->target).put(target->get());
                pc++;
                VM_NEXT;
            }
            try {
                EvalValue res =
                    vm_call_builtin_lv_rest(ctx, *chunk, in->a_dual_lo(),
                                            target, in->b_lit());
                if (in->target >= 0)
                    ctx.frame->at(in->target).put(std::move(res));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(CallBuiltinLV): {

            /* Native mutating-builtin call (lvalue ABI): form arg0's LValue*
             * from its slot table (by kind = a.lit), then call func_lv - which
             * mutates through it. AST-FREE: the Builtin + carets come from the
             * builtin_calls pool (a.slot). A REST-NATIVE op (`b` set) gets its
             * value args from the register run at `b`; a `b`-unset op (pop/intptr
             * - no value args) gets an empty rest. Mirrors Identifier::do_eval
             * for each kind: a not-yet-defined global -> null target ->
             * NotLValueEx, like the tree-walker. */
            const Chunk::BuiltinCall &bc = chunk->builtin_calls[in->a_dual_lo()];
            LValue *target;
            switch (in->a_dual_hi()) {
            case 0:   /* local */
                target = &ctx.frame->at(in->target2);
                break;
            case 1:   /* global */
                target = ctx.gfuncs->defined[in->target2]
                             ? &ctx.gfuncs->slots[in->target2] : nullptr;
                break;
            default:  /* capture */
                target = &(*ctx.captures)[in->target2];
                break;
            }
            try {
                if (in->b_is_lit()) {
                    ctx.frame->at(in->target).put(
                        vm_call_builtin_lv_rest(ctx, *chunk, in->a_dual_lo(), target,
                                                in->b_lit()));
                } else {
                    ArgLocs al = chunk->arglocs_at(in->a_dual_lo());
                    ctx.frame->at(in->target).put(
                        bc.builtin.func_lv(&ctx, &al, target, nullptr, 0));
                }
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = bc.start;
                    e.loc_end = bc.end;
                }
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(EmplaceStruct): {

            /* append(struct_arr, Ctor(args)) with the ctor's field VALUES in
             * run at `b`: form arg0's LValue* (like CallBuiltinLV), then coerce
             * the values straight into the flat struct array's bytes (no temp
             * StructObject); vm_emplace_struct handles the flat path + the
             * general fallback and matches the tree-walker append. AST-FREE:
             * the ctor def + carets from the emplace_sites pool (`a` packs
             * kind | idx << 2); the whole-args caret from the loc table. */
            const Chunk::EmplaceSite &site =
                chunk->emplace_sites[in->a_lit() >> 2];
            LValue *target;
            switch (in->a_lit() & 3) {
            case 0:  target = &ctx.frame->at(in->target2); break;
            case 1:  target = ctx.gfuncs->defined[in->target2]
                         ? &ctx.gfuncs->slots[in->target2] : nullptr; break;
            default: target = &(*ctx.captures)[in->target2]; break;
            }
            try {
                ctx.frame->at(in->target).put(
                    vm_do_emplace(ctx, site, target, in->b_lit()));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(CallBuiltinLVElem): {

            /* Mutating builtin with a subscript target a[i]/d[k] (Phase 2c):
             * form the base's LValue* (by kind), then the ELEMENT's LValue* via
             * the runtime Type::subscript - the SAME COW path the tree-walker's
             * Subscript::do_eval uses, given the identical base LValue* - and
             * call func_lv REST-NATIVE. `b` = the run base: run[0] = the index,
             * run[1..] = the pre-evaluated value args (append/push 1, pop 0). A
             * non-lvalue element (a flat scalar / read-only / missing dict key,
             * which throws) gives a null target -> NotLValueEx, like the
             * tree-walker. AST-FREE: Builtin + carets from the pool (a.slot). */
            const Chunk::BuiltinCall &bc = chunk->builtin_calls[in->a_dual_lo()];
            LValue *base;
            switch (in->a_dual_hi()) {
            case 0:  base = &ctx.frame->at(in->target2); break;
            case 1:  base = ctx.gfuncs->defined[in->target2]
                         ? &ctx.gfuncs->slots[in->target2] : nullptr; break;
            default: base = &(*ctx.captures)[in->target2]; break;
            }
            const int_type n_rest = static_cast<int_type>(bc.args.size()) - 1;
            try {
                EvalValue holder;   /* keeps the subscript result alive */
                LValue *elem = nullptr;
                if (base) {
                    const EvalValue &idx = ctx.frame->at(in->b_lit()).get();
                    holder = base->get().get_type()->subscript(
                        EvalValue(base), idx, /*for_write=*/false);
                    if (holder.is<LValue *>())
                        elem = holder.get<LValue *>();
                }
                EvalValue restbuf[8];   /* n_rest is small (append 1, pop 0) */
                for (int_type i = 0; i < n_rest; i++) {
                    restbuf[i] = ctx.frame->at(in->b_lit() + 1 + i).get();
                }
                ArgLocs al = chunk->arglocs_at(in->a_dual_lo());
                ctx.frame->at(in->target).put(
                    bc.builtin.func_lv(&ctx, &al, elem,
                                       n_rest ? restbuf : nullptr,
                                       static_cast<size_t>(n_rest)));
            } catch (Exception &e) {
                if (!e.loc_start) {
                    /* the subscript's caret = arg0 (the a[i] target). */
                    e.loc_start = bc.args[0].start;
                    e.loc_end = bc.args[0].end;
                }
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(CallBuiltinLVMember): {

            /* Mutating builtin with a struct-MEMBER target `append(s.f, x)`:
             * form the base's LValue* (by kind), the boxed FIELD LValue* via
             * vm_member_lvalue (same checks as the tree-walker's MemberExpr),
             * then call func_lv REST-NATIVE. `b` = the rest run (values, NO
             * index — unlike LVElem). AST-free: Builtin + carets + field name
             * from the pool (a.slot). */
            const Chunk::BuiltinCall &bc = chunk->builtin_calls[in->a_dual_lo()];
            LValue *base;
            switch (in->a_dual_hi()) {
            case 0:  base = &ctx.frame->at(in->target2); break;
            case 1:  base = ctx.gfuncs->defined[in->target2]
                         ? &ctx.gfuncs->slots[in->target2] : nullptr; break;
            default: base = &(*ctx.captures)[in->target2]; break;
            }
            const int_type n_rest = static_cast<int_type>(bc.args.size()) - 1;
            try {
                LValue *field = nullptr;
                if (base)
                    field = vm_member_lvalue(base, bc.member,
                                             bc.args[0].start, bc.args[0].end,
                                             bc.args[0].start, bc.args[0].end);
                EvalValue restbuf[8];   /* append/push 1 value arg */
                for (int_type i = 0; i < n_rest; i++) {
                    restbuf[i] = ctx.frame->at(in->b_lit() + i).get();
                }
                ArgLocs al = chunk->arglocs_at(in->a_dual_lo());
                ctx.frame->at(in->target).put(
                    bc.builtin.func_lv(&ctx, &al, field,
                                       n_rest ? restbuf : nullptr,
                                       static_cast<size_t>(n_rest)));
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = bc.args[0].start;
                    e.loc_end = bc.args[0].end;
                }
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(CallV):
        VM_CASE(CachedCallV): {

            /* Native user-function call: the args occupy the contiguous run
             * [a.lit, a.lit+b.lit) of THIS frame's slots - pass a view (no
             * per-call vector allocation). vm_call_func -> do_func_call runs
             * the
             * callee body via its chunk or the tree-walker; CachedCallV routes
             * through the per-frame pure-call cache (the fib-unroll dedup). A
             * callee slot not yet defined / reassigned to a non-function throws
             * the same UndefinedVariableEx / NotCallableEx the tree-walker
             * would; the args are evaluated exactly once either way. */
            /* AST-free: the callee NAME (undefined-slot error) is in gfuncs's
             * slot->name list, the caret in the loc side table (recording the
             * callee-identifier loc, which matches the tree-walker), and the
             * backtrace call-site is resolved lazily inside do_func_call (pass
             * chunk, pc - no per-call lookup on the success path). */
            if (!ctx.gfuncs->defined[in->target2]) {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw UndefinedVariableEx(
                    ctx.gfuncs->names[in->target2]->val, s, en);
            }
            const EvalValue &callee = ctx.gfuncs->slots[in->target2].get();
            if (!callee.is<intrusive_ptr<FuncObject>>()) {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw NotCallableEx(s, en);
            }
            FuncObject &fo = *callee.get<intrusive_ptr<FuncObject>>().get();

            if (!fo.func->vm_chunk_tried) {       /* AOT net, as before */
                fo.func->vm_chunk = vm_func_chunk(fo.func);
                fo.func->vm_chunk_tried = true;
            }
            const Chunk *cck =
                static_cast<const Chunk *>(fo.func->vm_chunk);

            if (cck) {
                /* IN-VM call. CachedCallV first probes the CALLER's
                 * per-frame pure cache (vm_cached_call's exact flow); the
                 * miss key rides the loop-scope pending_key so no dispatch
                 * exits a destructor scope (the clang indirect-goto rule). */
                pending_key.reset();
                if (in->op == OpCode::CachedCallV && ctx.frame
                        && g_pure_cache_enabled
                        && vm_cache_probe(ctx, in, fo.func, pending_key)) {
                    pc++;
                    VM_NEXT;
                }
                vm_enter_call(act, ctx, chunk, pc, fo, cck, in->a_lit(),
                              static_cast<size_t>(in->b_lit()), in->target,
                              std::move(pending_key));
                code = chunk->code.data();
                VM_NEXT;
            }

            /* No chunk (a never-called template base reached indirectly -
             * do_func_call ML_CHECKs): the legacy boundary call. */
            LValue *ap = &ctx.frame->at(in->a_lit());
            EvalValue res =
                in->op == OpCode::CachedCallV
                    ? vm_cached_call(&ctx, fo, ap, in->b_lit(), chunk, pc)
                    : vm_call_func(&ctx, fo, ap, in->b_lit(), chunk, pc);
            /* Inc v2: the callee (or something it called) is unwinding
             * cross-frame - route to a same-frame handler, or return to keep
             * propagating (this frame's do_func_call captures it). Inc 4: if
             * THIS call was spliced from inlined code, flush its virtual frames
             * as the exception passes through. */
            if (g_vm_exc_pending) {
                vm_flush_inline(*chunk, pc, *g_vm_exc_pending);
                if (vm_dispatch_exc(act, cur_rec(), pc)) {
                    cur_rec().exc = std::move(g_vm_exc_pending);
                    VM_NEXT_COLD;
                }
                return;
            }
            if (in->target >= 0)     /* -1 = a discarded call statement
                                      * (CallV only - CachedCallV keeps
                                      * its dst for the cache path) */
                ctx.frame->at(in->target).put(std::move(res));
            pc++;
        }
        VM_NEXT;

        VM_CASE(CallValueV): {
            /* Indirect call of a func VALUE: the callee was evaluated into
             * target2 (a FuncObject - proven by the Func static type). Read it
             * and call vm_call_func; a dyn-laundered non-func throws
             * NotCallableEx via the loc side table (the call-site loc). */
            const EvalValue &callee = ctx.frame->at(in->target2).get();
            if (!callee.is<intrusive_ptr<FuncObject>>()) {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw NotCallableEx(s, en);
            }
            FuncObject &fo = *callee.get<intrusive_ptr<FuncObject>>().get();

            if (!fo.func->vm_chunk_tried) {       /* AOT net, as before */
                fo.func->vm_chunk = vm_func_chunk(fo.func);
                fo.func->vm_chunk_tried = true;
            }
            if (const Chunk *cck =
                    static_cast<const Chunk *>(fo.func->vm_chunk)) {
                vm_enter_call(act, ctx, chunk, pc, fo, cck, in->a_lit(),
                              static_cast<size_t>(in->b_lit()), in->target,
                              nullptr);
                code = chunk->code.data();
                VM_NEXT;
            }

            LValue *ap = &ctx.frame->at(in->a_lit());
            EvalValue res = vm_call_func(&ctx, fo, ap, in->b_lit(), chunk,
                                         pc);
            if (g_vm_exc_pending) {                /* Inc v2: cross-frame */
                vm_flush_inline(*chunk, pc, *g_vm_exc_pending);   /* Inc 4 */
                if (vm_dispatch_exc(act, cur_rec(), pc)) {
                    cur_rec().exc = std::move(g_vm_exc_pending);
                    VM_NEXT_COLD;
                }
                return;
            }
            if (in->target >= 0)     /* -1 = a discarded call statement */
                ctx.frame->at(in->target).put(std::move(res));
            pc++;
        }
        VM_NEXT;

        VM_CASE(CallValueGenericV): {
            /* Generic indirect call of a DYN callee - AST-FREE (F1 step 2):
             * args 1..n-1 pre-evaluated in the run [a.lit+1, +nargs-1);
             * arg0 is described by the CallSite's LVALUE DESCRIPTOR (the
             * by-ref encoding - frame slots can't hold an LValue*-boxed
             * value, so the LValue* is re-derived at dispatch, only for a
             * func_lv callee; an elem/member arg0's VALUE was filled into
             * run[0] at its position by SubscriptV/MemberV). Dispatch on the
             * runtime callee: FuncObject -> vm_call_func (run[0] filled with
             * the derived value first - an undefined name throws at BIND,
             * the tree-walker's raw-UndefinedId order); struct ->
             * construct_struct_v; Builtin -> dispatch_builtin_values by
             * Kind - the same shared code the tree-walker's indirect branch
             * calls, so both engines agree. CheckCallableV already threw
             * for a non-callable BEFORE the args evaluated. A loc-less
             * builtin throw is stamped with the args caret, mirroring
             * dispatch_call_value's catch. */
            const EvalValue &callee = ctx.frame->at(in->target2).get();
            const int_type argbase = in->a_lit();
            const size_t nargs = static_cast<size_t>(in->b_lit() & 0xfff);
            const int site_i = static_cast<int>(in->b_lit() >> 12);
            const Chunk::CallSite &cs = chunk->call_sites[site_i];
            ArgLocs al = chunk->call_arglocs_at(site_i);

            /* arg0's VALUE from the descriptor: run[0] for the filled forms
             * (none/elem/member), the slot's value for `slot` (an undefined
             * global -> UndefinedId, surfacing at the CONSUMER's RValue),
             * the UndefinedId for `undef`. */
            auto a0_value = [&]() -> EvalValue {
                switch (cs.a0_form) {
                case Chunk::CallSite::A0::slot:
                    switch (cs.a0_kind) {
                    case 0:  return ctx.frame->at(cs.a0_slot).get();
                    case 1:
                        if (ctx.gfuncs->defined[cs.a0_slot])
                            return ctx.gfuncs->slots[cs.a0_slot].get();
                        return EvalValue(
                            UndefinedId{ctx.gfuncs->names[cs.a0_slot]->val});
                    case 2:  return (*ctx.captures)[cs.a0_slot].get();
                    default: return builtin_slot(cs.a0_slot).get();
                    }
                case Chunk::CallSite::A0::undef:
                    return EvalValue(UndefinedId{cs.a0_name->val});
                default:
                    return ctx.frame->at(argbase).get();
                }
            };
            /* arg0's LValue* for a func_lv callee (null -> NotLValueEx in
             * the builtin, like a non-LValue* raw value). `holder` keeps an
             * elem's subscript result alive across the call. */
            EvalValue holder;
            auto a0_lvalue = [&]() -> LValue * {
                switch (cs.a0_form) {
                case Chunk::CallSite::A0::slot:
                    switch (cs.a0_kind) {
                    case 0:  return &ctx.frame->at(cs.a0_slot);
                    case 1:  return ctx.gfuncs->defined[cs.a0_slot]
                                 ? &ctx.gfuncs->slots[cs.a0_slot] : nullptr;
                    case 2:  return &(*ctx.captures)[cs.a0_slot];
                    default: return &builtin_slot(cs.a0_slot);
                    }
                case Chunk::CallSite::A0::elem: {
                    LValue *base = vm_store_base(ctx, cs.a0_kind, cs.a0_slot,
                                                 *chunk, pc, nullptr);
                    const EvalValue &key =
                        ctx.frame->at(cs.a0_operand).get();
                    holder = base->get().get_type()->subscript(
                        EvalValue(base), key, /*for_write=*/false);
                    return holder.is<LValue *>()
                               ? holder.get<LValue *>() : nullptr;
                }
                case Chunk::CallSite::A0::member: {
                    const Chunk::MemberKey &mk =
                        chunk->member_keys[cs.a0_operand];
                    LValue *base = vm_store_base(ctx, cs.a0_kind, cs.a0_slot,
                                                 *chunk, pc, nullptr);
                    return vm_member_lvalue_ref(base->get(), mk.memId,
                                                mk.memUid,
                                                /*for_write=*/false,
                                                mk.mstart, mk.mend);
                }
                default:
                    return nullptr;
                }
            };

            EvalValue res;
            try {
                if (callee.is<intrusive_ptr<FuncObject>>()) {
                    if (nargs)
                        ctx.frame->at(argbase).put(RValue(a0_value()));
                    res = vm_call_func(
                        &ctx,
                        *callee.get<intrusive_ptr<FuncObject>>().get(),
                        &ctx.frame->at(argbase), nargs, chunk, pc);
                } else {
                    /* Builtin / struct: the raw values (arg0 from the
                     * descriptor - may be UndefinedId, RValued by need). */
                    EvalValue stackbuf[8];
                    std::vector<EvalValue> heapbuf;
                    EvalValue *buf = stackbuf;
                    if (nargs > 8) {
                        heapbuf.resize(nargs);
                        buf = heapbuf.data();
                    }
                    if (nargs)
                        buf[0] = a0_value();
                    for (size_t i = 1; i < nargs; i++) {
                        buf[i] = ctx.frame->at(argbase
                                               + static_cast<int>(i)).get();
                }
                    if (callee.is<Builtin>()) {
                        const Builtin &b = callee.get<Builtin>();
                        if (b.kind == Builtin::Kind::lvalue) {
                            /* the by-ref path: derive arg0's true LValue* */
                            LValue *target = nargs ? a0_lvalue() : nullptr;
                            EvalValue rest[8];
                            std::vector<EvalValue> resth;
                            EvalValue *rp = rest;
                            const size_t nr = nargs ? nargs - 1 : 0;
                            if (nr > 8) {
                                resth.resize(nr);
                                rp = resth.data();
                            }
                            for (size_t i = 0; i < nr; i++) {
                                rp[i] = RValue(buf[i + 1]);
                }
                            res = b.func_lv(&ctx, &al, target,
                                            nr ? rp : nullptr, nr);
                        } else {
                            res = dispatch_builtin_values(&ctx, b, &al,
                                                          buf, nargs);
                        }
                    } else if (callee.is<StructTypeDef *>()) {
                        res = construct_struct_v(
                            callee.get<StructTypeDef *>(), &al, buf, nargs);
                    } else {
                        throw NotCallableEx();   /* net: CheckCallableV ran */
                    }
                }
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = al.start;
                    e.loc_end = al.end;
                }
                throw;
            }
            if (g_vm_exc_pending) {                /* FuncObject cross-frame */
                vm_flush_inline(*chunk, pc, *g_vm_exc_pending);
                if (vm_dispatch_exc(act, cur_rec(), pc)) {
                    cur_rec().exc = std::move(g_vm_exc_pending);
                    VM_NEXT_COLD;
                }
                return;
            }
            ctx.frame->at(in->target).put(std::move(res));
            pc++;
        }
        VM_NEXT;

        VM_CASE(CheckCallableV): {
            /* An indirect call's callable guard: throw NotCallableEx (callee
             * caret, loc side table) unless slot `a` holds a FuncObject, a
             * Builtin, or a struct type descriptor - BEFORE the arg run
             * evaluates, matching the tree-walker's dispatch order. */
            const EvalValue &cv = ctx.frame->at(in->a_slot()).get();
            if (!cv.is<intrusive_ptr<FuncObject>>() && !cv.is<Builtin>()
                && !cv.is<StructTypeDef *>()) {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw NotCallableEx(s, en);
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(CheckFuncV):
            /* map/filter's arg0 guard: throw (arg0's caret, from the loc side
             * table) if it isn't a function, BEFORE arg1's code runs - the
             * tree-walker's order. AST-free. */
            if (!ctx.frame->at(in->a_slot()).get()
                     .is<intrusive_ptr<FuncObject>>()) {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw TypeErrorEx("Expected function", s, en);
            }
            pc++;
            VM_NEXT;

        VM_CASE(MapFilterV): {
            /* map/filter over the pre-validated function + the container; the
             * unsupported-container caret comes from the loc side table. */
            Loc s, en;
            chunk->loc_at(pc, s, en);
            ctx.frame->at(in->target).put(
                vm_map_filter(&ctx, ctx.frame->at(in->a_slot()).get(),
                              ctx.frame->at(in->b_slot()).get(),
                              in->target2 != 0, s, en));
            pc++;
        }
        VM_NEXT;

        VM_CASE(ReturnV): {
            /* `return <expr>`: the value is in a.slot (a bare return loaded
             * `none`). An in-VM frame pops - result into the parent's dst
             * slot, resume after the call op. The BOUNDARY frame keeps the
             * do_func_call contract: set flow, stop the invocation. */
            if (cur_rec().boundary) {
                ctx.flow->value = ctx.frame->at(in->a_slot()).get();
                ctx.flow->type = FlowState::ret;
                return;
            }
            vm_leave_call(act, ctx, chunk, pc,
                          ctx.frame->at(in->a_slot()).get());
            code = chunk->code.data();
            VM_NEXT;
        }

        VM_CASE(ArrLen):
            /* n = size(array). The base is a flat array (ForeachStmt::elem_th
             * guarantees array<int>/array<float>), so read size() directly. */
            ctx.frame->at(in->target).put(EvalValue(static_cast<int_type>(
                ctx.frame->at(in->target2).get()
                    .get_ref<SharedArrayObj>().size())));
            pc++;
            VM_NEXT;

        VM_CASE(StrLen):
            /* n = char count of the string (foreach bound over a proven str).
             * get_view() accounts for a slice's offset. */
            ctx.frame->at(in->target).put(EvalValue(static_cast<int_type>(
                ctx.frame->at(in->target2).get()
                    .get_ref<SharedStr>().get_view().size())));
            pc++;
            VM_NEXT;

        VM_CASE(LoadStrChar): {
            /* x = a fresh 1-char string of the container's i-th char - matches
             * the tree-walker's SharedStr(string(&view[i], 1)). `i` is
             * loop-bounded (< StrLen), so view[i] is in range. */
            const std::string_view view = ctx.frame->at(in->target2).get()
                .get_ref<SharedStr>().get_view();
            const int_type i = read_int_operand(in->a(), &ctx);
            ctx.frame->at(in->target).put(
                EvalValue(SharedStr(std::string(&view[i], 1))));
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadImmInt):
            ctx.frame->at(in->target).put(
                EvalValue(static_cast<int_type>(in->a_lit())));
            pc++;
            VM_NEXT;

        VM_CASE(LoadImmFloat):
            ctx.frame->at(in->target).put(
                EvalValue(static_cast<float_type>(in->a_flit())));
            pc++;
            VM_NEXT;

        VM_CASE(LoadConstV):
            ctx.frame->at(in->target).put(chunk->consts[in->target2]);
            pc++;
            VM_NEXT;

        VM_CASE(LoadLiteralObjV): {
            /* Materialize a baked const array/dict/struct literal via shared
             * eval_literal_obj (immutable share vs a fresh mutable clone, plus
             * the general/flat_s arr_hint cases) - byte-identical to
             * LiteralObj::do_eval. */
            const Chunk::LiteralObjEntry &lo = chunk->literal_objs[in->target2];
            ctx.frame->at(in->target).put(
                eval_literal_obj(lo.value, lo.immutable, lo.arr_hint,
                                 lo.arr_hint_struct));
            pc++;
        }
        VM_NEXT;

        VM_CASE(MoveV):
            /* Alias, not clone (a container assignment shares the handle;
             * matches doAssign's `x = RValue(rval)` - COW protects later). */
            ctx.frame->at(in->target).put(
                ctx.frame->at(in->target2).get());
            pc++;
            VM_NEXT;

        VM_CASE(CoerceNumV):
            /* dst = coerce_to_decl_type(src, i/f): the typed-store numeric
             * coerce (the coerces_dyn accumulator's plain assign) - widen
             * float <- int/bool / int <- bool, pass none, THROW TypeError on
             * a non-fitting dyn value. Caret = the Expr14 span (loc table),
             * matching the tree-walker's stamp. */
            try {
                ctx.frame->at(in->target).put(
                    vm_coerce_decl_num(ctx.frame->at(in->a_slot()).get(),
                                       in->target2 != 0));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            pc++;
            VM_NEXT;

        VM_CASE(BinOpV): {
            /* Clone the left operand, then num_bin_op mutates the clone (so
             * `a + b` never corrupts a) - byte-identical to the tree-walker's
             * eval_first_rvalue().clone() + num_binop_loc chain (int/float
             * promotion, string `+` concat, bitwise). */
            EvalValue sa, sb;
            EvalValue val = boxed_operand(in->a(), &ctx, sa).clone();
            try {
                num_bin_op(val, boxed_operand(in->b(), &ctx, sb),
                           binop_pmf(in->aop));
            } catch (Exception &e) {
                /* Stamp the operand loc (side table) like stamp_operand_loc, so
                 * a div-zero / type error points where the tree-walker does. */
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            ctx.frame->at(in->target).put(std::move(val));
            pc++;
        }
        VM_NEXT;

        VM_CASE(CompoundV): {
            /* dst OP= b: COPY the lvalue (a container shares its handle, so the
             * op mutates it in place), apply num_bin_op, store back - identical
             * to doAssign's compound branch. `b` may be an immediate. */
            EvalValue sb;
            EvalValue nv = ctx.frame->at(in->target).get();
            try {
                num_bin_op(nv, boxed_operand(in->b(), &ctx, sb),
                           binop_pmf(in->aop));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            ctx.frame->at(in->target).put(std::move(nv));
            pc++;
        }
        VM_NEXT;

        VM_CASE(CmpV): {
            /* dst = (a <cmp> b) as a bool - copy a, num_bin_op with the
             * comparison PMF, store is_true() (= Expr06/Expr07::do_eval). */
            EvalValue sa, sb;
            EvalValue val = boxed_operand(in->a(), &ctx, sa);
            try {
                num_bin_op(val, boxed_operand(in->b(), &ctx, sb),
                           cmp_pmf(in->aop));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            ctx.frame->at(in->target).put(EvalValue(val.is_true()));
            pc++;
        }
        VM_NEXT;

        VM_CASE(LogV): {
            /* Eager (MyLang &&/|| don't short-circuit at runtime) - both
             * operands are already computed (a slot or an immediate); combine
             * truthiness. */
            EvalValue sa, sb;
            const bool a = boxed_operand(in->a(), &ctx, sa).is_true();
            const bool b = boxed_operand(in->b(), &ctx, sb).is_true();
            ctx.frame->at(in->target).put(
                EvalValue(in->aop == Op::land ? (a && b) : (a || b)));
            pc++;
        }
        VM_NEXT;

        VM_CASE(UnaryV): {
            /* Boxed unary over a dyn/general operand - mirrors Expr02::do_eval
             * (clone the operand, apply). `-str`/`~str` throw a type error via
             * the Type vtable -> stamp the loc side table. */
            EvalValue s;
            EvalValue v = boxed_operand(in->a(), &ctx, s).clone();
            try {
                switch (in->aop) {
                case Op::plus:
                    if (v.is<bool>())
                        v = static_cast<int_type>(v.get<bool>() ? 1 : 0);
                    break;
                case Op::minus:
                    if (v.is<bool>())
                        v = static_cast<int_type>(v.get<bool>() ? 1 : 0);
                    v.get_type()->opneg(v);
                    break;
                case Op::lnot:
                    v = EvalValue(!v.is_true());
                    break;
                case Op::bnot:
                    if (v.is<bool>())
                        v = static_cast<int_type>(v.get<bool>() ? 1 : 0);
                    v.get_type()->bnot(v);
                    break;
                default:
                    throw InternalErrorEx();
                }
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            ctx.frame->at(in->target).put(std::move(v));
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadGlobalV):
            /* AST-free: the hot read is a gfuncs slot; the cold undefined
             * error takes its NAME from gfuncs's slot->name list and its loc
             * from the side table - no `node`. */
            if (!ctx.gfuncs->defined[in->target2]) {
                Loc s, en;
                chunk->loc_at(pc, s, en);
                throw UndefinedVariableEx(
                    ctx.gfuncs->names[in->target2]->val, s, en);
            }
            ctx.frame->at(in->target).put(
                ctx.gfuncs->slots[in->target2].get());
            pc++;
            VM_NEXT;

        VM_CASE(LoadCaptureV):
            ctx.frame->at(in->target).put(
                (*ctx.captures)[in->target2].get());
            pc++;
            VM_NEXT;

        VM_CASE(LoadBuiltinV):
            ctx.frame->at(in->target).put(builtin_slot(in->target2).get());
            pc++;
            VM_NEXT;

        VM_CASE(DefinedGlobalV):
            /* defined(global): the slot's defined-flag IS the answer (false
             * before the decl ran, true after) - byte-identical to
             * builtin_defined evaluating the identifier to an UndefinedId or
             * not. AST-free, never throws. */
            ctx.frame->at(in->target).put(
                EvalValue(ctx.gfuncs->defined[in->target2] != 0));
            pc++;
            VM_NEXT;

        VM_CASE(StoreGlobalV): {
            LValue &lv = ctx.gfuncs->slots[in->target];
            if (in->aop == Op::invalid) {
                /* g = <expr>: write the shared global slot + mark defined -
                 * exactly slot_rmw(op==assign) (put(RValue)) + the decl's
                 * defined=1. Serves both a decl and a reassign (idempotent). */
                lv.put(RValue(ctx.frame->at(in->a_slot()).get()));
                ctx.gfuncs->defined[in->target] = 1;
            } else {
                /* g OP= rhs / g++ (aop = the base op, rhs in `a`): a compound
                 * requires the slot already DEFINED (else UndefinedVariableEx,
                 * like the tree-walker falling through its defined guard), then
                 * copy-modify-store via num_bin_op - identical to CompoundV. */
                if (!ctx.gfuncs->defined[in->target]) {
                    Loc s, en;
                    chunk->loc_at(pc, s, en);
                    throw UndefinedVariableEx(
                        ctx.gfuncs->names[in->target]->val, s, en);
                }
                EvalValue sb;
                EvalValue nv = lv.get();
                try {
                    num_bin_op(nv, boxed_operand(in->a(), &ctx, sb),
                               binop_pmf(in->aop));
                } catch (Exception &e) {
                    vm_stamp_loc(*chunk, pc, e);
                    throw;
                }
                lv.put(std::move(nv));
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(DeclConstV): {
            /* const arr/dict/func decl: bind the slot as a CONST LValue (so a
             * later rebind still throws), local (target2==0) or global (==1).
             * The rvalue value is already materialized in `a`. */
            EvalValue v = ctx.frame->at(in->a_slot()).get();
            if (in->target2 == 0) {
                ctx.frame->at(in->target) = LValue(std::move(v), true);
            } else {
                ctx.gfuncs->slots[in->target] = LValue(std::move(v), true);
                ctx.gfuncs->defined[in->target] = 1;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreCaptureV): {
            /* cap = <expr> / cap OP= v / cap++ : write the called closure's
             * per-instance capture slot. A capture is ALWAYS defined (snapshot
             * at closure creation), so - unlike a global - no defined check. */
            LValue &lv = (*ctx.captures)[in->target];
            if (in->aop == Op::invalid) {
                lv.put(RValue(ctx.frame->at(in->a_slot()).get()));
            } else {
                EvalValue sb;
                EvalValue nv = lv.get();
                try {
                    num_bin_op(nv, boxed_operand(in->a(), &ctx, sb),
                               binop_pmf(in->aop));
                } catch (Exception &e) {
                    vm_stamp_loc(*chunk, pc, e);
                    throw;
                }
                lv.put(std::move(nv));
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(SubscriptV): {
            /* base[idx] read via the runtime Type::subscript (any base type -
             * array / dict / string), an LValue* to the base slot passed like
             * Subscript::do_eval, for_write=false, RValue'd into dst. */
            LValue &base_lv = ctx.frame->at(in->target2);
            const EvalValue &idx = ctx.frame->at(in->a_slot()).get();
            try {
                Type *t = base_lv.get().get_type();
                ctx.frame->at(in->target).put(
                    RValue(t->subscript(EvalValue(&base_lv), idx, false)));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);   /* AST-free: loc side table */
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(MemberV): {
            /* base.member value read via the shared member_read_core (struct
             * field / const / dict key / optional); AST-free - the name
             * key/uid, optional flag and carets come from the pool (in->a()). */
            const Chunk::MemberKey &mk = chunk->member_keys[in->a_lit()];
            ctx.frame->at(in->target).put(
                member_read_core(ctx.frame->at(in->target2).get(), mk.memId,
                                 mk.memUid, mk.optional, mk.mstart, mk.mend,
                                 mk.bstart, mk.bend));
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadMemberInt): {
            /* H1: typed standalone struct-member read `p.x` (th==i). The POD
             * fast path reads the scalar straight from the instance's bytes -
             * the VM analog of MemberExpr::eval_int; anything else falls to
             * the shared member_read_core + write_scalar_slot (its int
             * type-check/promotion == the tree-walker's boxed fallback). */
            const Chunk::MemberKey &mk = chunk->member_keys[in->a_lit()];
            const EvalValue &b = ctx.frame->at(in->target2).get();
            if (b.is<intrusive_ptr<StructObject>>()) {
                const StructObject &o =
                    *b.get<intrusive_ptr<StructObject>>().get();
                const int fs = o.def->slot_of(mk.memUid);
                if (fs >= 0 && o.is_pod()) {
                    const FieldDef &f = o.def->fields[fs];
                    const char *p = o.bytes.data() + f.offset;
                    if (f.kind == FieldKind::f_int) {
                        int_type v;
                        std::memcpy(&v, p, sizeof v);
                        write_int_slot(&ctx, in->target, v);
                        pc++;
                        VM_NEXT;
                    }
                    if (f.kind == FieldKind::f_bool) {
                        write_int_slot(&ctx, in->target,
                            static_cast<unsigned char>(*p) != 0 ? 1 : 0);
                        pc++;
                        VM_NEXT;
                    }
                }
            }
            try {
                write_scalar_slot(&ctx, in->target, /*is_int=*/true,
                    member_read_core(b, mk.memId, mk.memUid, mk.optional,
                                     mk.mstart, mk.mend, mk.bstart, mk.bend));
            } catch (Exception &e) {
                /* write_scalar_slot's get<> throw is loc-less - give it the
                 * member's caret (member_read_core's own throws carry it) */
                if (!e.loc_start) {
                    e.loc_start = mk.mstart;
                    e.loc_end = mk.mend;
                }
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadMemberFloat): {
            /* the eval_float twin (th==f) */
            const Chunk::MemberKey &mk = chunk->member_keys[in->a_lit()];
            const EvalValue &b = ctx.frame->at(in->target2).get();
            if (b.is<intrusive_ptr<StructObject>>()) {
                const StructObject &o =
                    *b.get<intrusive_ptr<StructObject>>().get();
                const int fs = o.def->slot_of(mk.memUid);
                if (fs >= 0 && o.is_pod()) {
                    const FieldDef &f = o.def->fields[fs];
                    const char *p = o.bytes.data() + f.offset;
                    if (f.kind == FieldKind::f_float) {
                        float_type v;
                        std::memcpy(&v, p, sizeof v);
                        write_float_slot(&ctx, in->target, v);
                        pc++;
                        VM_NEXT;
                    }
                    if (f.kind == FieldKind::f_int) {
                        int_type v;
                        std::memcpy(&v, p, sizeof v);
                        write_float_slot(&ctx, in->target,
                                         static_cast<float_type>(v));
                        pc++;
                        VM_NEXT;
                    }
                }
            }
            try {
                write_scalar_slot(&ctx, in->target, /*is_int=*/false,
                    member_read_core(b, mk.memId, mk.memUid, mk.optional,
                                     mk.mstart, mk.mend, mk.bstart, mk.bend));
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = mk.mstart;
                    e.loc_end = mk.mend;
                }
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(SliceV): {
            /* base[start:end] via the runtime Type::slice (which RValues the
             * base + registers the COW slice view) - mirrors Slice::do_eval. An
             * absent bound (slot -1) passes `none`. The slice() throws (a
             * non-int index) get the caret from the loc side table. */
            const EvalValue &base = ctx.frame->at(in->target2).get();
            const EvalValue start = in->a_slot() >= 0
                ? ctx.frame->at(in->a_slot()).get() : EvalValue();
            const EvalValue end = in->b_slot() >= 0
                ? ctx.frame->at(in->b_slot()).get() : EvalValue();
            try {
                ctx.frame->at(in->target).put(
                    base.get_type()->slice(base, start, end));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(JumpUnlessTrueV):
            if (!ctx.frame->at(in->target2).get().is_true())
                pc = in->target;
            else
                pc++;
            VM_NEXT;

        VM_CASE(JumpIfNotNoneV):
            /* `a ?? b` short-circuit: a non-none lhs skips the rhs. */
            if (!ctx.frame->at(in->a_slot()).get().is<NoneVal>())
                pc = static_cast<size_t>(in->target);
            else
                pc++;
            VM_NEXT;

        VM_CASE(Throw): {
            /* P8 Inc 1: raise the value in slot `a`. vm_raise builds the native
             * dispatch (a same-frame catch jump, no C++ throw - the 42 win) or
             * C++-throws to the caller's boundary (cross-frame, like the
             * tree-walker) - the SAME path a runtime error takes. */
            Loc ls, le;
            chunk->loc_at(pc, ls, le);
            const EvalValue &tv = ctx.frame->at(in->a_slot()).get();
            if (!vm_raise(chunk, pc, act, ctx,
                          vm_make_thrown_exc(tv, ls, le)))
                return;                       /* boundary: signal set */
            code = chunk->code.data();
            VM_NEXT;                          /* re-dispatch at the handler */
        }

        VM_CASE(PushHandler):
            act.handlers.push_back({ static_cast<uint32_t>(in->target) });
            pc++;
            VM_NEXT;

        VM_CASE(PopHandler):
            act.handlers.pop_back();  /* the try body exited normally */
            pc++;
            VM_NEXT;

        VM_CASE(CatchTest): {
            /* vm_exc holds the caught exception. Match its type name against
             * this clause (a.lit = catch_types idx, or -1 = catch-all). On a
             * match: bind `catch (T as e)` (target2 = slot, -1 if none), jump to
             * the catch body (target). Else fall to the next CatchTest /
             * Reraise. vm_exc is KEPT (not cleared) so a `rethrow` in the catch
             * body can re-raise it; a new throw overwrites it. */
            bool match;
            if (in->a_lit() < 0) {
                match = true;
            } else {
                const std::vector<std::string> &names =
                    chunk->catch_types[in->a_lit()];
                const std::string_view en = vm_exc_name(cur_rec().exc.get());
                match = false;
                for (const std::string &nm : names) {
                    if (nm == en) { match = true; break; }
                }
            }
            if (match) {
                if (in->target2 >= 0)
                    ctx.frame->at(in->target2).put(
                        vm_catch_bind_val(cur_rec().exc.get()));
                pc = static_cast<size_t>(in->target);
            } else {
                pc++;
            }
        }
        VM_NEXT;

        VM_CASE(Reraise):
            /* No clause matched: re-raise vm_exc - the native WALK finds the
             * outer handler (same or ANY caller frame) or reaches the boundary
             * (G1: no C++ throw; the walk's first step IS the old same-frame
             * dispatch, and the boundary catch used to flush the reraise pc's
             * inlined frames + clone - vm_raise does the flush, sans clone). */
            if (!vm_raise(chunk, pc, act, ctx, std::move(cur_rec().exc)))
                return;                 /* boundary: signal set */
            code = chunk->code.data();
            VM_NEXT;

        VM_CASE(Rethrow):
            /* `rethrow` in a catch body: re-raise the being-handled vm_exc with
             * the rethrow-site loc (like do_catch's RethrowEx handling), to the
             * OUTER handler (native) or C++-propagate. */
            {
                Loc ls, le;
                chunk->loc_at(pc, ls, le);
                cur_rec().exc->loc_start = ls;
                cur_rec().exc->loc_end = le;
            }
            if (!vm_raise(chunk, pc, act, ctx, std::move(cur_rec().exc)))
                return;                 /* boundary: signal set */
            code = chunk->code.data();
            VM_NEXT;

        VM_CASE(SetPend):
            /* Record what the shared `finally` must resume - normal or reraise
             * (Inc 2b). Flow ops inline their finally, so ret/brk/cont never
             * reach the shared finally. */
            cur_rec().pend = static_cast<Pend>(in->target);
            pc++;
            VM_NEXT;

        VM_CASE(EndFinally):
            /* End of the SHARED finally block: resume the pending action. Only
             * the NORMAL and RERAISE exits reach here - a return/break/continue
             * crossing this try INLINES its own copy of the finally (Inc 2c),
             * so `vm_pend` is only ever normal or reraise here. */
            if (cur_rec().pend == Pend::reraise) {
                /* finally didn't handle the exception → re-raise it (the
                 * native walk, like Reraise - G1). */
                if (!vm_raise(chunk, pc, act, ctx,
                              std::move(cur_rec().exc)))
                    return;             /* boundary: signal set */
                code = chunk->code.data();
                VM_NEXT;
            } else {
                pc++;                                 /* fall through to Lend */
            }
            VM_NEXT;

        VM_CASE(Halt):
            /* End of a fall-through body (an implicit `return none`). */
            if (cur_rec().boundary)
                return;
            vm_leave_call(act, ctx, chunk, pc, EvalValue());
            code = chunk->code.data();
            VM_NEXT;
#ifndef ML_CGOTO
        case OpCode::OpCount_:      /* sentinel - never emitted (switch
                                     * exhaustiveness; cgoto has no entry) */
            throw InternalErrorEx();
        }   /* switch */
    }       /* for */
#endif

    } catch (RuntimeException &e) {
        /*
         * An exception reached the boundary: a native `throw` with no
         * same-frame handler, a runtime-library error (div0/OOB/
         * KeyNotFound/...), or a C++ throw out of a callee boundary. The
         * FRAME WALK (the native call stack): starting at the CURRENT
         * frame, dispatch to a handler in it; else - for an IN-VM frame -
         * append its backtrace frame (exactly what its do_func_call catch
         * used to record), pop it, flush the CALL op's inlined frames at
         * the parent's call pc (what the caller's signal check used to do),
         * and continue in the parent. Reaching the invocation's BOUNDARY
         * frame converts to the pending-exception SIGNAL and returns -
         * do_func_call above captures that frame and propagates, exactly
         * the Inc-v2 contract. ONE landing pad per invocation, however
         * many in-VM frames the exception crosses.
         */
        /* Inc 4: a library error thrown FROM an inlined op flushes its
         * virtual frames here (the raise happened in C++, not vm_raise). */
        vm_flush_inline(*chunk, pc, e);
        if (vm_unwind_walk(act, ctx, chunk, pc,
                           std::unique_ptr<RuntimeException>(e.clone())))
            goto vm_resume;    /* dispatched (chunk/pc set at the handler) */
        return;                /* signal set; do_func_call captures + goes on */
    }
}
