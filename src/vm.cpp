/* SPDX-License-Identifier: BSD-2-Clause */

#include "vm.h"
#include "jit.h"
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

/* Write a real BOOL into a slot (the CmpIntV/CmpFloatV result - the value form
 * of a typed compare). In-place if the slot already holds a bool. */
static ML_ALWAYS_INLINE void
write_bool_slot(EvalContext *ctx, int_type slot, bool v)
{
    LValue &lv = ctx->frame->at(slot);
    if (lv.is<bool>())
        lv.getval<bool>() = v;
    else
        lv.put(EvalValue(v));
}

/* CmpIntV / CmpFloatV bodies (the VALUE form of a typed int/float compare ->
 * bool). ML_NOINLINE off vm_run_chunk's frame (the loop-body TEXT RULE): the
 * dispatch core must not grow, or an UNTOUCHED pure loop regresses via code
 * layout (measured: inlining these cost 55_float_sum ~3% wall / +0.6% I-count
 * with no bytecode change). One CALL per compare - still far below the boxed
 * CmpV (num_bin_op + PMF + is_true) it replaces. Never throws. */
ML_NOINLINE static void
vm_cmp_int_v(const Instr &in, EvalContext *ctx)
{
    const int_type a = read_int_operand(in.a(), ctx);
    const int_type b = read_int_operand(in.b(), ctx);
    bool r;
    switch (in.aop) {
    case Op::lt: r = a <  b; break;
    case Op::gt: r = a >  b; break;
    case Op::le: r = a <= b; break;
    case Op::ge: r = a >= b; break;
    case Op::eq: r = a == b; break;
    default:     r = a != b; break;   /* noteq */
    }
    write_bool_slot(ctx, in.target, r);
}

ML_NOINLINE static void
vm_cmp_float_v(const Instr &in, EvalContext *ctx)
{
    const float_type a = read_float_operand(in.a(), ctx);
    const float_type b = read_float_operand(in.b(), ctx);
    bool r;
    switch (in.aop) {
    case Op::lt: r = a <  b; break;
    case Op::gt: r = a >  b; break;
    case Op::le: r = a <= b; break;
    case Op::ge: r = a >= b; break;
    case Op::eq: r = a == b; break;
    default:     r = a != b; break;   /* noteq */
    }
    write_bool_slot(ctx, in.target, r);
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
 * differential agrees. Cold [[noreturn]] helpers, out of the hot loop. A NULL
 * chunk (the JIT helpers, which have no chunk) throws LOC-LESS - EnterNative's
 * re-raise stamps the same side-table caret at the op's pc. */
[[noreturn]] static ML_COLD void
vm_throw_unpack_nonarray(const Chunk *chunk, size_t pc, int_type nvars)
{
    Loc s, en;
    if (chunk)
        chunk->loc_at(pc, s, en);
    throw TypeErrorEx(intern_msg("foreach: cannot unpack a non-array element "
                                 "into " + std::to_string(nvars) +
                                 " variables"), s, en);
}

[[noreturn]] static ML_COLD void
vm_throw_unpack_len(const Chunk *chunk, size_t pc, size_type m, int_type nvars)
{
    Loc s, en;
    if (chunk)
        chunk->loc_at(pc, s, en);
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

/* The store body below is shared by the interpreter (a valid chunk -> stamp
 * the caret from the loc table) AND the JIT helper (chunk == null -> throw
 * LOC-LESS; EnterNative re-stamps from the LIVE chunk at the returned pc -
 * the JIT fragment can't hold a chunk pointer, since codegen builds the
 * chunk on the STACK and MOVES it out after jit_compile_chunk). Cold. */
[[noreturn]] static ML_COLD void
vm_store_throw_oob(const Chunk *chunk, size_t pc)
{
    if (chunk)
        vm_throw_oob(*chunk, pc);
    throw OutOfBoundsEx();
}

[[noreturn]] static ML_COLD void
vm_store_throw_div0(const Chunk *chunk, size_t pc)
{
    if (chunk)
        vm_throw_div0(*chunk, pc);
    throw DivisionByZeroEx();
}

/* The StoreElemInt/StoreElemFloat store body, SHARED by the interpreter
 * handler AND the approach-A JIT helper (jit_store_elem_int/float, below):
 * a[i] = v / a[i] OP= v for a flat mutable int/bool/float array (COW), else
 * the UNIVERSAL vm_subscript_store fallback (const / readonly / general /
 * dyn / a wrong-kind base). idx/rhs are the PRE-READ scalar operands (the
 * caller resolved them from the Instr / the native fragment); the body
 * THROWS on OOB / div0 / not-an-lvalue / etc. - the interpreter lets it
 * propagate, the JIT helper catches it into g_vm_jit_exc. ML_ALWAYS_INLINE
 * so the interpreter's hot handler carries no call and the ONLY out-of-line
 * copy is the JIT helper's. (Reading rhs up front is byte-identical: an
 * operand read is a pure slot/lit load, no side effect - so the OOB check
 * still fires before any store, exactly as the lazy handler did.) */
static ML_ALWAYS_INLINE void
vm_store_elem_int_body(LValue &alv, int_type idx, int_type rhs, Op aop,
                       const Chunk *chunk, size_t pc)
{
    if (alv.is<SharedArrayObj>()) {
        SharedArrayObj &arr = alv.getval<SharedArrayObj>();
        const auto sk = arr.skind();
        const bool is_bool = sk == SharedArrayObj::Storage::bools;
        if ((sk == SharedArrayObj::Storage::ints
             || (is_bool && aop == Op::invalid))
            && !alv.is_const_var() && !arr.is_readonly()) {
            if (idx < 0)
                idx += arr.size();
            if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                vm_store_throw_oob(chunk, pc);
            if ((aop == Op::div || aop == Op::mod) && rhs == 0)
                vm_store_throw_div0(chunk, pc);
            if (arr.is_slice())
                arr.clone_internal_vec();
            else if (arr.use_count() > 1)
                arr.clone_aliased_slices(arr.offset() + idx);
            if (is_bool) {
                arr.flat_bools()[arr.offset() + idx] = rhs ? 1 : 0;
                arr.invalidate_hash();
                return;
            }
            int_type &el = arr.flat_ints()[arr.offset() + idx];
            switch (aop) {
            case Op::invalid: el = rhs;  break;
            case Op::plus:    el += rhs; break;
            case Op::minus:   el -= rhs; break;
            case Op::times:   el *= rhs; break;
            case Op::div:     el /= rhs; break;
            case Op::mod:     el %= rhs; break;
            default: throw InternalErrorEx();
            }
            arr.invalidate_hash();
            return;
        }
    }
    Loc ls, le;
    if (chunk)
        chunk->loc_at(pc, ls, le);       /* JIT (null): loc-less -> stamped */
    vm_subscript_store(&alv, EvalValue(idx), EvalValue(rhs),
                       vm_base_to_expr14_op(aop), ls, le);
}

static ML_ALWAYS_INLINE void
vm_store_elem_float_body(LValue &alv, int_type idx, float_type rhs, Op aop,
                         const Chunk *chunk, size_t pc)
{
    if (alv.is<SharedArrayObj>()) {
        SharedArrayObj &arr = alv.getval<SharedArrayObj>();
        if (arr.skind() == SharedArrayObj::Storage::floats
            && !alv.is_const_var() && !arr.is_readonly()) {
            if (idx < 0)
                idx += arr.size();
            if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                vm_store_throw_oob(chunk, pc);
            if ((aop == Op::div || aop == Op::mod) && rhs == 0.0)
                vm_store_throw_div0(chunk, pc);
            if (arr.is_slice())
                arr.clone_internal_vec();
            else if (arr.use_count() > 1)
                arr.clone_aliased_slices(arr.offset() + idx);
            float_type &el = arr.flat_floats()[arr.offset() + idx];
            switch (aop) {
            case Op::invalid: el = rhs;               break;
            case Op::plus:    el += rhs;              break;
            case Op::minus:   el -= rhs;              break;
            case Op::times:   el *= rhs;              break;
            case Op::div:     el /= rhs;              break;
            case Op::mod:     el = std::fmod(el, rhs); break;
            default: throw InternalErrorEx();
            }
            arr.invalidate_hash();
            return;
        }
    }
    Loc ls, le;
    if (chunk)
        chunk->loc_at(pc, ls, le);       /* JIT (null): loc-less -> stamped */
    vm_subscript_store(&alv, EvalValue(idx), EvalValue(rhs),
                       vm_base_to_expr14_op(aop), ls, le);
}

/* The MULTI-ASSIGN strict-unpack length error (F-1) - the same message the
 * tree-walker's handle_single_expr14 throws, WITHOUT the "foreach:" prefix. */
[[noreturn]] static ML_COLD void
vm_throw_multi_unpack_len(const Chunk *chunk, size_t pc, size_type m,
                          size_t nvars)
{
    Loc s, en;
    if (chunk)
        chunk->loc_at(pc, s, en);
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
            /* get_ref (NOT get<T>): a by-value get() would COPY the handle,
             * bumping use_count to 2, so the reuse check below could never
             * fire - H1 dst-reuse was silently dead, allocating a fresh
             * StructObject every `var p = Point(..)` iteration. The sibling
             * container-literal H1 (below) already uses get_ref. */
            const intrusive_ptr<StructObject> &so =
                cur.get_ref<intrusive_ptr<StructObject>>();
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

/* THE PLANNED POD CTOR (the 64_struct_create fix, 2026-07-26). Every arg
 * was compile-proven (the StructCtorV typed gate), so the per-field work is
 * a STATIC plan: {byte offset, act} - act 0 = raw int store (a bool arg's
 * payload is already 0/1; matches coerce's bool->int widen), 1 = float
 * store with read_float_slot's exact promote ladder (float raw, int/bool
 * cvt), 2 = bool byte store. NO coerce_struct_field calls, NO EvalValue
 * marshal buffer; H1 dst-slot reuse kept. NEVER throws (make_intrusive's
 * bad_alloc is terminal anyway), so the op needs no defensive catch. */
static ML_NOINLINE void
vm_struct_ctor_planned(EvalContext &ctx, StructTypeDef *def,
                       const Chunk::CtorPlan &cp, int_type dst)
{
    LValue &d = ctx.frame->at(dst);
    StructObject *o = nullptr;
    const EvalValue &cur = d.get();
    if (cur.is<intrusive_ptr<StructObject>>()) {
        const intrusive_ptr<StructObject> &so =
            cur.get_ref<intrusive_ptr<StructObject>>();
        if (so->def == def && !so->readonly && so.use_count() == 1)
            o = so.get();
    }
    intrusive_ptr<StructObject> fresh;
    if (!o) {
        fresh = make_intrusive<StructObject>(def);
        o = fresh.get();
    }
    char *bytes = o->bytes.data();
    for (size_t i = 0; i < cp.f.size(); i++) {
        const Chunk::CtorPlanField &pf = cp.f[i];
        switch (pf.act) {
        case 0: {                                   /* int (or bool) -> int */
            const EvalValue &v = ctx.frame->at(pf.src).get();
            const int_type iv = v.is<bool>() ? (v.get<bool>() ? 1 : 0)
                                             : v.get<int_type>();
            std::memcpy(bytes + pf.off, &iv, sizeof iv);
            break;
        }
        case 1: {                              /* float (int/bool promote) */
            const float_type fv = read_float_slot(&ctx, pf.src);
            std::memcpy(bytes + pf.off, &fv, sizeof fv);
            break;
        }
        default:                                            /* bool byte */
            bytes[pf.off] =
                ctx.frame->at(pf.src).get().get<bool>() ? 1 : 0;
            break;
        }
    }
    if (fresh)
        d.put(EvalValue(intrusive_ptr<StructObject>(std::move(fresh))));
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
/* `mkeys` = chunk.member_keys.data() (the buffer, not the whole chunk) - so the
 * JIT can bake it (a stable pool address) instead of a &chunk that dangles. */
static void
vm_chain_walk(EvalContext &ctx, const Chunk::MemberKey *mkeys, EvalValue &cur,
              const std::vector<Chunk::ChainStep> &steps, size_t upto)
{
    for (size_t i = 0; i < upto; i++) {
        const Chunk::ChainStep &step = steps[i];
        try {
            if (step.is_member) {
                const Chunk::MemberKey &mk = mkeys[step.operand];
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

/* Takes the steps vector + the member_keys buffer directly (not chunk +
 * steps_idx), so the JIT can bake both pool addresses (a &chunk would dangle). */
static ML_NOINLINE void
vm_chain_lvalue_store_op(EvalContext &ctx,
                         const std::vector<Chunk::ChainStep> &steps,
                         const Chunk::MemberKey *mkeys, LValue *base,
                         const EvalValue &value, Op op)
{
    const size_t n = steps.size();
    EvalValue cur = EvalValue(base);
    vm_chain_walk(ctx, mkeys, cur, steps, n - 1);

    const Chunk::ChainStep &last = steps[n - 1];   /* node = the whole lvalue */
    try {
        /* The final store needs a live lvalue; a VALUE-walked chain (a POD /
         * readonly intermediate) can't be stored into -> NotLValueEx at the
         * whole-lvalue loc, exactly as the tree-walker's handle_single_expr14. */
        if (!cur.is<LValue *>())
            throw NotLValueEx(last.lstart, last.lend);
        LValue *curlv = cur.get<LValue *>();
        if (last.is_member) {
            const Chunk::MemberKey &mk = mkeys[last.operand];
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
/* The chain walk + final step over an already-formed root - the SHARED core
 * for the interpreter op below AND jit_incdec_chain (the root forming
 * differs: the interpreter throws UndefinedVariableEx for an undefined
 * global root, the JIT helper BAILS). `mkeys` is the member_keys BUFFER. */
static void
vm_incdec_chain_core(EvalContext &ctx, const Chunk::IncDecChain &site,
                     const Chunk::MemberKey *mkeys, EvalValue cur,
                     int_type dst, bool is_inc)
{
    const std::vector<Chunk::ChainStep> &steps = site.steps;
    const size_t n = steps.size();

    vm_chain_walk(ctx, mkeys, cur, steps, n - 1);

    const Chunk::ChainStep &last = steps[n - 1];
    EvalValue memId;
    const UniqueId *memUid = nullptr;
    EvalValue key;
    if (last.is_member) {
        const Chunk::MemberKey &mk = mkeys[last.operand];
        memId = mk.memId;
        memUid = mk.memUid;
    } else {
        key = ctx.frame->at(last.operand).get();
    }

    EvalValue r = vm_incdec_final(cur, last.is_member, memId, memUid, key,
                                  site.tier2, is_inc,
                                  site.is_prefix,
                                  site.allow_flat, site.allow_pod,
                                  last.lstart, last.lend,
                                  site.kstart, site.kend,
                                  site.id_start, site.id_end);
    if (dst >= 0)
        ctx.frame->at(dst).put(std::move(r));
}

static ML_NOINLINE void
vm_incdec_chain_op(EvalContext &ctx, const Chunk &chunk, const Instr &in,
                   size_t pc)
{
    const Chunk::IncDecChain &site = chunk.incdec_chains[in.b_lit()];

    EvalValue cur;
    if (in.a_lit() == 3)
        cur = ctx.frame->at(in.target2).get();      /* rvalue root: a VALUE */
    else
        cur = EvalValue(
            vm_store_base(ctx, in.a_lit(), in.target2, chunk, pc, nullptr));

    vm_incdec_chain_core(ctx, site, chunk.member_keys.data(), std::move(cur),
                         in.target, in.aop == Op::plus);
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
/* Takes the BuiltinCall pool ENTRY directly (not chunk + idx), so the JIT can
 * bake `&chunk.builtin_calls[idx]` (a stable pool address) - a &chunk dangles. */
static ML_COLD EvalValue
vm_call_builtin_lv_rest(EvalContext &ctx, const Chunk::BuiltinCall &bc,
                        LValue *target, int_type base)
{
    const int_type n_rest = static_cast<int_type>(bc.args.size()) - 1;
    ArgLocs al;
    al.start = bc.start;
    al.end = bc.end;
    al.args = bc.args.data();
    al.nargs = bc.args.size();
    al.arr_hint = bc.arr_hint;
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
 * #60 lever 2: the boxed arith/compare handlers' (BinOpV / CompoundV / CmpV,
 * plus the compound global/capture stores and the dyn inc-dec) num_bin_op
 * front end. M8 lowers a PROVEN int/float node to a native IntBin/FloatBin, but
 * a DYN/general operand - or a comparison used as a VALUE (`x % 3 == 0` as a
 * return value has no native typed-VALUE compare) - still boxes here and paid
 * num_bin_op's promotion-check chain + an indirect PMF call + TypeInt's own
 * dispatch (~7% of 35_map_filter: num_bin_op 4.5% + TypeInt::eq 2.3%).
 *
 * For the overwhelmingly common case where BOTH runtime operands are plain int,
 * compute the result inline via a switch on the Op ENUM (the VM has it; the PMF
 * hid it), writing the int result (a comparison yields int 0/1, which the CmpV
 * caller wraps with is_true() exactly as TypeInt::lt + num_bin_op does). Any
 * other operand shape (float / bool / string / mixed / dyn-non-int) falls back
 * to the EXACT num_bin_op PMF path - byte-identical, including div/mod-by-zero
 * (thrown here too) and the bad-shift-count / type errors, whose loc the
 * caller's catch stamps. ML_NOINLINE: ONE out-of-line copy (num_bin_op no
 * longer inlines at each of the ~7 call sites) - a DIRECT call, not the removed
 * indirect PMF, and it keeps vm_dispatch's hot code layout unperturbed (the
 * loop-body text rule).
 */
static ML_NOINLINE void
vm_num_binop(EvalValue &a, const EvalValue &b, Op aop)
{
    if (a.is<int_type>() && b.is<int_type>()) {
        const int_type y = b.get<int_type>();
        int_type &x = a.get<int_type>();
        switch (aop) {
        case Op::plus:  x += y; return;
        case Op::minus: x -= y; return;
        case Op::times: x *= y; return;
        case Op::div:   if (y == 0) throw DivisionByZeroEx(); x /= y; return;
        case Op::mod:   if (y == 0) throw DivisionByZeroEx(); x %= y; return;
        case Op::band:  x &= y; return;
        case Op::bor:   x |= y; return;
        case Op::bxor:  x ^= y; return;
        case Op::shl:   x = bit_shl(x, y);  return;
        case Op::shr:   x = bit_shr(x, y);  return;
        case Op::ushr:  x = bit_ushr(x, y); return;
        case Op::lt:    x = (x <  y); return;
        case Op::gt:    x = (x >  y); return;
        case Op::le:    x = (x <= y); return;
        case Op::ge:    x = (x >= y); return;
        case Op::eq:    x = (x == y); return;
        case Op::noteq: x = (x != y); return;
        default:        break;   /* unreachable: aop is one of the above */
        }
    }
    NumBinOp pmf = binop_pmf(aop);
    if (!pmf)
        pmf = cmp_pmf(aop);
    ML_VM_CHECK(pmf != nullptr);
    num_bin_op(a, b, pmf);
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

/* The SHARED iterator-op bodies (model-flip nativize-ops): ONE implementation
 * each for the interpreter handler AND its jit_* twin, so the two cannot
 * drift (the emit_elem_int_read lesson). A NULL `chunk` (the JIT helpers)
 * makes a throw LOC-LESS - EnterNative's re-raise stamps the caret from the
 * loc side table at the op's pc, byte-identical to the interpreted throw. */

/* DictIterInit: pin the dict + set the live iterator to begin(). The
 * inferencer proved a Dict static type; the ML_VM_CHECK is the hardening
 * net (shared by both engines). Never throws. */
static ML_ALWAYS_INLINE void
vm_dict_iter_init_body(DictIterState &st, const EvalValue &base)
{
    ML_VM_CHECK(base.is<intrusive_ptr<DictObject>>());
    st.dict = base.get<intrusive_ptr<DictObject>>();
    st.it = st.dict->get_ref().begin();
}

/* DictIterNext: false on exhaustion; else bind the key (and value) box-free -
 * a plain EvalValue copy, matching the tree-walker's `{p.first,
 * p.second.get()}` - and advance. A slot of -1 is a `_` placeholder / the
 * keys-only 1-var form. Never throws (frame-slot puts have no COW path). */
static ML_ALWAYS_INLINE bool
vm_dict_iter_next_body(DictIterState &st, Frame &frame,
                       int_type k_slot, int_type v_slot)
{
    if (st.it == st.dict->get_ref().end())
        return false;
    if (k_slot >= 0)
        frame.at(k_slot).put(st.it->first);
    if (v_slot >= 0)
        frame.at(v_slot).put(st.it->second.get());
    ++st.it;
    return true;
}

/* ForeachDynInit: dispatch the DYN container once - pin it, record the loop
 * shape (`shape` = nvars | indexed << 8, `targets` = the unpack_targets pool
 * entry), and set up an array or dict cursor. An unsupported runtime value
 * throws TypeErrorEx (loc from `chunk`, or loc-less when null). */
static void
vm_foreach_dyn_init_body(DynIterState &st, const EvalValue &cont,
                         int_type shape, const std::vector<int32_t> *targets,
                         const Chunk *chunk, size_t pc)
{
    st.container = cont;
    st.nvars = static_cast<int>(shape & 0xff);
    st.indexed = (shape >> 8) != 0;
    st.targets = targets;
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
        if (chunk)
            chunk->loc_at(pc, s, en);
        throw TypeErrorEx("foreach: expected an array or dict", s, en);
    }
}

/* ForeachDynNext: false on exhaustion; else bind the loop vars from the state
 * (target slots, -1 == `_` skipped) exactly as do_iter - `indexed` binds
 * targets[0] = the counter; an ARRAY element binds the single remaining var
 * box-free (vm_arr_elem) or STRICT-unpacks an array element into the N
 * remaining vars (the two TypeErrorEx throws; loc-less when `chunk` null); a
 * DICT binds key [, value [, none...]] (do_iter's count=2 padding). Then
 * advance. */
static bool
vm_foreach_dyn_next_body(DynIterState &st, Frame &frame,
                         const Chunk *chunk, size_t pc)
{
    const std::vector<int32_t> &tg = *st.targets;
    const size_t tb = st.indexed ? 1 : 0;    /* first value var */
    const size_t nv = static_cast<size_t>(st.nvars) - tb;
    const auto bind = [&](size_t i, const EvalValue &v) {
        if (tg[i] >= 0)
            frame.at(tg[i]).put(v);
    };
    if (!st.is_dict) {
        if (st.idx >= st.size)
            return false;
        if (st.indexed)
            bind(0, EvalValue(static_cast<int_type>(st.idx)));
        if (nv == 1) {
            bind(tb, vm_arr_elem(st.container, st.idx));
        } else if (nv >= 2) {
            /* N-var over an ARRAY: the element must be an array of EXACTLY
             * nv, unpacked into the vars - do_iter's STRICT destructure. */
            const EvalValue elem = vm_arr_elem(st.container, st.idx);
            if (!elem.is<SharedArrayObj>())
                vm_throw_unpack_nonarray(chunk, pc, static_cast<int>(nv));
            const SharedArrayObj &sub = elem.get_ref<SharedArrayObj>();
            if (sub.size() != static_cast<size_type>(nv))
                vm_throw_unpack_len(chunk, pc, sub.size(),
                                    static_cast<int>(nv));
            for (size_t i = 0; i < nv; i++)
                bind(tb + i, vm_arr_elem(elem, static_cast<size_type>(i)));
        }
        st.idx++;
    } else {
        DictObject &d = *st.container.get<intrusive_ptr<DictObject>>();
        if (st.it == d.get_ref().end())
            return false;
        if (st.indexed)
            bind(0, EvalValue(st.counter++));
        /* do_iter's count==2 else-branch: key, value, then `none` for any
         * further vars. */
        if (nv >= 1)
            bind(tb, st.it->first);
        if (nv >= 2)
            bind(tb + 1, st.it->second.get());
        for (size_t i = 2; i < nv; i++)
            bind(tb + i, none);
        ++st.it;
    }
    return true;
}

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

    /* M5 LEAN SYNC ENTER (plans/model-flip.md): this frame was pushed by a
     * native caller's jit_call_sync* helper - its ret_chunk is the SENTINEL
     * stop chunk, so a normal return dispatches the sentinel's ExitBlock
     * (vm_dispatch returns to the helper), and the unwind walk STOPS here
     * like a boundary (capture this frame, pop, convert to g_vm_exc_pending)
     * instead of walking into the native caller's record. */
    unsigned char sync_stop = 0;

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
    /* The call-record stack. `records` is PHYSICAL storage that grows only to
     * the high-water mark; `rec_n` is the LIVE count. A push REUSES the
     * already-constructed record at rec_n (growing by one only at a new peak),
     * a pop resets its owning fields + decrements - so the ~140-byte record's
     * 3 unique_ptrs are NOT default-constructed/destructed per call (the
     * emplace_back construct was ~6% of a deep-recursion profile). RAII is
     * kept (the unique_ptrs live in the reused objects, null between uses;
     * pop_window frees any the exceptional/cache path left set). */
    std::vector<VmCallRec> records;
    size_t rec_n = 0;                 /* live records; records[rec_n..] reused */
    /* Lever 1 push/pop micro (the callgrind vector-size split): back_rec's
     * records[rec_n - 1] is an IMUL by the 136-byte stride at EVERY use
     * (~23 sites, several per call) - cache the top record's address,
     * updated at push/pop (records only reallocates in push_window's grow
     * branch; NATIVE code reads rec_n but never writes it, so the pointer
     * cannot go stale under a fragment). recs_high mirrors records.size()
     * (another per-push IMUL); diters_n/dyiters_n mirror the two iter
     * stacks' sizes (dyn's 72-byte stride divided per push AND pop) -
     * both vectors are mutated ONLY in push/pop below. handlers is NOT
     * mirrored: fragments push/pop it natively (inline PushHandler), and
     * its 4-byte stride is a plain shift anyway. ML_VM_CHECK re-verifies
     * every mirror (the CI-release hardening net). */
    VmCallRec *top_rec = nullptr;
    uint32_t recs_high = 0;           /* == records.size() */
    uint32_t diters_n = 0;            /* == dict_iters.size() */
    uint32_t dyiters_n = 0;           /* == dyn_iters.size() */
    ML_ALWAYS_INLINE VmCallRec &back_rec() {
        ML_VM_CHECK(top_rec == &records[rec_n - 1]);
        return *top_rec;
    }
    ML_ALWAYS_INLINE bool no_recs() const { return rec_n == 0; }
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

        /* REUSE the constructed record at rec_n; grow only at a new peak. */
        ML_VM_CHECK(recs_high == records.size());
        if (rec_n == recs_high) {
            records.emplace_back();      /* one-time construct at high-water */
            recs_high++;
        }
        VmCallRec &rec = records[rec_n++];   /* fill IN PLACE - no move */
        top_rec = &rec;                  /* stays valid: grow is above */
        rec.window = sg->slots.data() + sg->top;
        rec.nslots = n;
        rec.seg = cur_seg;
        rec.seg_top_before = sg->top;
        rec.boundary = boundary;
        rec.sync_stop = 0;               /* a REUSED record must not carry a
                                          * stale lean-sync stop mark */
        rec.run_chunk = ck;
        rec.handler_base = static_cast<uint32_t>(handlers.size());
        ML_VM_CHECK(diters_n == dict_iters.size());
        ML_VM_CHECK(dyiters_n == dyn_iters.size());
        rec.diter_base = diters_n;
        rec.dyiter_base = dyiters_n;
        /* An IN-VM push (boundary=false) is followed by vm_enter_call setting
         * every resume field; a BOUNDARY push has no such follow-up, so on a
         * REUSED record its resume fields would be stale - clear them (they
         * are provably unused for a boundary frame, but staleness is a
         * future-change hazard, and a boundary push is rare/off the hot
         * recursion path). */
        if (boundary) {
            rec.ret_chunk = nullptr;
            rec.ret_pc = 0;
            rec.dst = -1;
            rec.desc = nullptr;
            rec.caller_captures = nullptr;
        }
        if (ck->n_dict_iters) {
            diters_n += static_cast<uint32_t>(ck->n_dict_iters);
            dict_iters.resize(diters_n);
        }
        if (ck->n_dyn_iters) {
            dyiters_n += static_cast<uint32_t>(ck->n_dyn_iters);
            dyn_iters.resize(dyiters_n);
        }
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
     * are stable), we clean by reference, then decrement rec_n (the record
     * object is REUSED by the next push - we free its owning fields here, the
     * equivalent of the old pop_back dtor, but keep the object). */
    ML_ALWAYS_INLINE void pop_window()
    {
        ML_CHECK(rec_n != 0);
        VmCallRec &rec = back_rec();

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
        if (diters_n != rec.diter_base) {
            dict_iters.resize(rec.diter_base);
            diters_n = rec.diter_base;
        }
        if (dyiters_n != rec.dyiter_base) {
            dyn_iters.resize(rec.dyiter_base);
            dyiters_n = rec.dyiter_base;
        }

        segs[rec.seg]->top = rec.seg_top_before;
        used -= rec.nslots;
        cur_seg = rec.seg;

        /* The popped frame's cache dies HERE (per-frame scoping); the
         * caller's stashed cache comes back into the view. */
        if (rec.caller_cache || view_frame.pure_cache)
            view_frame.pure_cache = std::move(rec.caller_cache);

        /* Free any owning state the frame still holds - the reuse-equivalent
         * of the old pop_back dtor. On the HOT path all three are already
         * null (cache_key moved out by vm_leave_call, caller_cache moved out
         * above, exc never set), so these are cheap null resets; the
         * exceptional-walk / cached-call paths may leave one set, and freeing
         * it here matches the old dtor exactly. pend must reset so a reused
         * record does not inherit a stale finally-pend. */
        rec.exc.reset();
        rec.cache_key.reset();
        rec.pend = Pend::normal;
        rec_n--;

        if (rec_n) {
            const VmCallRec &tp = records[rec_n - 1];
            top_rec = &records[rec_n - 1];
            view_frame.point_at(tp.window, static_cast<int>(tp.nslots));
            cur_seg = tp.seg;
        } else {
            top_rec = nullptr;
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
    /* #55 STEP 2: codegen main WITHOUT jit - its native_leaf flag is still set;
     * jit is deferred to below, after Pass A has set every function's flag (a
     * top-level native call to a function needs the callee's flag). */
    prog.root = codegen_program(root, /*jit=*/false);

    /* AOT: compile every function body upfront (no lazy per-call compile) - the
     * maintainer's no-lazy rule + a `.myv`-serialization prerequisite. After
     * this, do_func_call reads a precomputed vm_chunk and never compiles.
     * Pass A codegens all bodies (flags set), Pass B jits them. */
    vm_precompile_all(root);

    /* Now jit main - every function's native_leaf flag is set (Pass A), so
     * main's top-level native calls can see them (#55 STEP 2). */
    jit_compile_chunk(prog.root);

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
vm_func_chunk(const FuncDescriptor *fdesc, bool jit)
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
    if (!codegen_func_body(fdesc->decl, ck, jit))
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

    /* #55 STEP 2 - Pass A: CODEGEN every body (jit deferred), so every
     * `native_leaf` flag is set (codegen_chunk sets it from the ops) BEFORE any
     * jit. A caller's native-call gate needs EVERY callee's flag, and a caller
     * may be declared before its callee, so all flags must exist first. */
    for (const FuncDeclStmt *fn : funcs) {
        /* compiles + caches (or null); stamped on the DESCRIPTOR */
        fn->desc->vm_chunk = vm_func_chunk(fn->desc, /*jit=*/false);
        fn->desc->vm_chunk_tried = true;
        bool fast = true;
        for (const auto &p : fn->desc->params)
            if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
                fast = false;
        fn->desc->fast_bind = fast;
    }

    /* #55 STEP 2.1: the global slot -> callee FuncDescriptor* map, so a
     * caller's native-call gate can resolve + bake a callee. Only a function
     * bound to a global slot is a native-call target; every other slot stays
     * null (not native-callable). Sized like the global table. */
    std::vector<const FuncDescriptor *> slot_desc(
        root->global_func_names.size(), nullptr);
    for (const FuncDeclStmt *fn : funcs)
        if (fn->id && fn->id->sym.kind == SymKind::global) {
            const int slot = fn->id->sym.slot;
            if (slot >= 0 && static_cast<size_t>(slot) < slot_desc.size())
                slot_desc[slot] = fn->desc;
        }

    /* Pass B: JIT every compiled body, each with its own JitCtx (caller_desc =
     * the descriptor keying its chunk). Order-independent (all native_leaf
     * flags set in Pass A; a caller bakes the callee DESCRIPTOR and loads its
     * native entry at RUNTIME, by when every body is jit'd). Main is jit'd by
     * vm_compile after this (with no JitCtx -> no native call from main in v1,
     * since main has no stable descriptor for the record's ret_chunk). */
    for (auto &kv : g_func_chunks) {
        JitCtx jc;
        jc.slot_desc = &slot_desc;
        jc.slot_reassigned = &root->global_slot_reassigned;
        jc.caller_desc = kv.first;
        jit_compile_chunk(kv.second, &jc);
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

/* Approach A: a native fragment sets this (a JitRaiseKind) instead of
 * re-interpreting an op that hit a proven exception; EnterNative raises. */
int g_vm_jit_raise = 0;

/* #55 native calls: jit_frame_leave (the return-side leave core, shared by the
 * interpreter's ReturnV and - later - the native ReturnV) stashes the PARENT's
 * resume point here (a machine-code caller can't take C++ refs to the loop's
 * chunk/pc locals). The interpreter's vm_leave_call reads them into its refs;
 * EnterNative will read them when a native ReturnV rets the resume SENTINEL. */
static const Chunk *g_vm_resume_chunk = nullptr;
static size_t g_vm_resume_pc = 0;

/* model-flip M2 (plans/model-flip.md): the ISLAND fall-through resume pc. An
 * ExitBlock sets it (the container's next-block pc) and returns from
 * vm_dispatch; vm_exec_block reads it to distinguish a fall-through exit
 * (g_vm_block_resume != NONE) from a ReturnV/Halt (flow) or a raise. NONE
 * (SIZE_MAX) between islands - cleared by vm_exec_block before each run. */
static const size_t VM_BLOCK_NONE = static_cast<size_t>(-1);
static size_t g_vm_block_resume = VM_BLOCK_NONE;

/* model-flip M3: jit_exec_block returns this (high bit set, so a container
 * fragment's `test rax; jns` distinguishes it from a small fall-through resume
 * pc) when an island RAISED - the pending exception is bridged into
 * g_vm_jit_exc and the fragment exits so EnterNative re-raises. */
static const size_t JIT_BLOCK_RAISED = static_cast<size_t>(-3);

/* #55 native calls: the EvalContext of the CURRENTLY EXECUTING vm_run_chunk
 * (set + restored at each entry - a builtin callback re-enters with the invoke
 * ctx), so the native ReturnV helper (jit_ret) reaches the right frame / flow /
 * captures; the fragment holds no ctx handle. The matching activation is the
 * existing g_vm_act. */
static EvalContext *g_current_ctx = nullptr;

/* #55: jit_ret's return values - a resume PC no real chunk pc can equal (a
 * remapped pc is always small). IN-VM: EnterNative switches to the parent
 * (g_vm_resume_chunk/pc). BOUNDARY: EnterNative stops the invocation. */
static const size_t JIT_RET_SENTINEL = static_cast<size_t>(-1);
static const size_t JIT_RET_BOUNDARY = static_cast<size_t>(-2);

/* #55: native ReturnVs executed process-wide (a coverage counter - see jit.h). */
unsigned long g_jit_native_returns = 0;

/* Approach A (container-store helper ops): a noexcept JIT helper that CAUGHT
 * an arbitrary RuntimeException stashes a CLONE here (loc intact) and returns
 * non-0; EnterNative raises it. Complements g_vm_jit_raise (a KIND a fragment
 * can signal by itself) - this carries the exact object a C++ helper threw
 * (OOB / div0 / NotLValue / CannotChangeConst / KeyNotFound / TypeError),
 * whose type + message + caret can't be reconstructed from a kind. */
static std::unique_ptr<RuntimeException> g_vm_jit_exc;

/* M5: a PLAIN (non-Runtime) exception crossing a fragment - e.g. a callee's
 * UndefinedVariableEx surfacing from a native sync call. It has no clone()
 * (only RuntimeException does), so it rides a std::exception_ptr; EnterNative
 * rethrows it, after which it propagates out of vm_dispatch exactly like the
 * interpreted in-VM call's own throw (non-catchable, top-level render). */
static std::exception_ptr g_vm_jit_eptr;

/* The approach-A container-store JIT helpers (declared in jit.h). A native
 * a[i]=v fragment marshals the base LValue*, index and value and CALLS one of
 * these instead of splitting the run at the store - keeping the surrounding
 * matrix/sieve loop native. They run vm_store_elem_*_body (the interpreter's
 * EXACT store) with a NULL chunk, noexcept: a raise is thrown LOC-LESS,
 * caught into g_vm_jit_exc, and reported by the return value; EnterNative
 * re-stamps the caret from the LIVE chunk at the returned pc (a fragment
 * can't hold a chunk pointer - the chunk is stack-built + moved out after
 * jit_compile_chunk). aop is an int to keep the Op enum out of jit.h. */
extern "C" int jit_store_elem_int(LValue *base, int_type idx, int_type rhs,
                                  int aop) noexcept
{
    try {
        vm_store_elem_int_body(*base, idx, rhs, static_cast<Op>(aop),
                               nullptr, 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

extern "C" int jit_store_elem_float(LValue *base, int_type idx, double rhs,
                                    int aop) noexcept
{
    try {
        vm_store_elem_float_body(*base, idx, rhs, static_cast<Op>(aop),
                                 nullptr, 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* Approach A - the DICT-STORE JIT helper (d[k] = v / d[k] OP= v): the native
 * fragment passes the base dict LValue* and the key/value slot EvalValue*s
 * (the frame slots hold the BOXED values, so no marshaling - the fragment
 * just leas their addresses; EvalValue is the first LValue member) plus the
 * Expr14 op. Runs the SAME vm_subscript_store the interpreter's DictStore
 * handler calls (auto-vivify / COW / key-freeze / any base type), so a dict
 * loop no longer splits the run at the store. LOC-LESS throw -> g_vm_jit_exc
 * -> EnterNative stamps the caret (see jit_store_elem_int). `op` is the Expr14
 * op (Op) as an int. Measured ~6.5% wall / 12% fewer instructions on the
 * dict-insert loop (23_dict_insert) - dispatch IS a real chunk of the dict
 * tier; the bigger headroom is still the boxed-value/alloc model (N7). */
extern "C" int jit_dict_store(LValue *base, const EvalValue *key,
                              const EvalValue *val, int op) noexcept
{
    try {
        vm_subscript_store(base, *key, *val, static_cast<Op>(op),
                           Loc(), Loc());
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native StoreElemValue - the UNIVERSAL subscript
 * store `a[i] = v` / `a[i] OP= v` for ANY base (flat / general / dict), the
 * interpreter's exact vm_subscript_store (bounds + COW + slot_rmw, type-
 * dispatched). Unlike jit_dict_store (a frame-slot base lea'd in the emit), the
 * base may be a GLOBAL or CAPTURE container, so the emit passes `kind` (0 local
 * / 1 global / 2 capture) + the base SLOT and the helper forms the LValue* (like
 * vm_store_base). `idx`/`val` are frame slots (the computed index + value);
 * `aop` is the Expr14 op (assign / addeq / ...). TWO throw sources: (1) an
 * UNDEFINED GLOBAL base (kind==1, use-before-def) - BAILS (return 1 with NO
 * g_vm_jit_exc set, so EnterNative resumes the INTERPRETER, which re-runs
 * StoreElemValue + throws UndefinedVariableEx, a plain Exception; the N4 pattern,
 * like LoadGlobalV); (2) vm_subscript_store's OOB/KeyNotFound/TypeError/NotLValue
 * (all RuntimeException) -> caught loc-less into g_vm_jit_exc + return 1, so
 * EnterNative re-raises (caret from the loc side table). Both exits are `return
 * 1`; EnterNative distinguishes by whether g_vm_jit_exc is set. NOT
 * op_fully_native (side-table caret / the bail keeps the original).
 */
extern "C" int jit_store_elem_value(int_type kind, int_type base_slot,
                                    int_type idx_slot, int_type val_slot,
                                    int_type aop) noexcept
{
    ML_JIT_OP_RAN(StoreElemValue);
    EvalContext *ctx = g_current_ctx;
    if (kind == 1 && !ctx->gfuncs->defined[base_slot])
        return 1;                            /* bail (no exc): re-run -> throw */
    LValue *alv = kind == 1 ? &ctx->gfuncs->slots[base_slot]
                : kind == 2 ? &(*ctx->captures)[base_slot]
                            : &ctx->frame->at(base_slot);
    const EvalValue &idx = ctx->frame->at(idx_slot).get();
    const EvalValue &val = ctx->frame->at(val_slot).get();
    try {
        vm_subscript_store(alv, idx, val, static_cast<Op>(aop), Loc(), Loc());
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native StoreMemberV - a struct field store
 * `s.f = v` / `s.f OP= v` (a dict member store uses DictStore), the
 * interpreter's exact vm_member_store (POD byte / boxed-field store). Like
 * jit_store_elem_value but the "key" is a MEMBER: the emit bakes
 * `&chunk.member_keys[idx]` (the pool BUFFER addr, stable across the chunk move)
 * and the helper reads memUid + the 4 carets from it. The base may be a
 * GLOBAL/CAPTURE struct (`kind`). Throws: an UNDEFINED GLOBAL base BAILS (return
 * 1, no exc -> interpreter re-runs + throws UndefinedVariableEx); vm_member_store
 * throws only RuntimeExceptions (a non-struct base / a bad POD field type ->
 * TypeErrorEx already carrying its caret; a readonly instance -> NotLValueEx
 * (mstart); a compound `s.f += v` div/mod -> a LOC-LESS DivisionByZeroEx, stamped
 * with the MEMBER caret here exactly as the interpreted StoreMemberV catch does)
 * -> g_vm_jit_exc -> EnterNative re-raises. NOT op_fully_native.
 */
extern "C" int jit_store_member(int_type kind, int_type base_slot,
                                int_type val_slot, int_type aop,
                                const void *mkv) noexcept
{
    ML_JIT_OP_RAN(StoreMemberV);
    const Chunk::MemberKey *mk = static_cast<const Chunk::MemberKey *>(mkv);
    EvalContext *ctx = g_current_ctx;
    if (kind == 1 && !ctx->gfuncs->defined[base_slot])
        return 1;                            /* bail (no exc): re-run -> throw */
    LValue *blv = kind == 1 ? &ctx->gfuncs->slots[base_slot]
                : kind == 2 ? &(*ctx->captures)[base_slot]
                            : &ctx->frame->at(base_slot);
    const EvalValue &val = ctx->frame->at(val_slot).get();
    try {
        vm_member_store(blv, mk->memUid, static_cast<Op>(aop), val,
                        mk->mstart, mk->mend, mk->bstart, mk->bend,
                        mk->bake_def, mk->bake_slot);
    } catch (RuntimeException &e) {
        if (!e.loc_start) {                  /* a compound div/mod is loc-less */
            e.loc_start = mk->mstart;
            e.loc_end = mk->mend;
        }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native StoreElem2V - a 2-level nested store
 * `a[i][j] = v` / `a[i][j] OP= v` (read a[i] as a ref, store [j] into it), the
 * interpreter's exact vm_nested_subscript_store. The base `a` is a LOCAL frame
 * slot (no kind); k1/k2/val are frame slots; `locs` = chunk.chain_locs[idx].data()
 * (the per-step carets, baked - so an intermediate a[i] OOB and the final store
 * carry their OWN subscript loc). vm_nested_subscript_store throws only
 * RuntimeExceptions (OOB/KeyNotFound/TypeError/NotLValue), already carrying their
 * per-step caret -> caught into g_vm_jit_exc + re-raised. NOT op_fully_native.
 */
extern "C" int jit_store_elem2(int_type base_slot, int_type k1_slot,
                               int_type k2_slot, int_type val_slot,
                               int_type aop, const void *locsv) noexcept
{
    ML_JIT_OP_RAN(StoreElem2V);
    const std::pair<Loc, Loc> *locs =
        static_cast<const std::pair<Loc, Loc> *>(locsv);
    EvalContext *ctx = g_current_ctx;
    LValue &alv = ctx->frame->at(base_slot);
    const EvalValue &k1 = ctx->frame->at(k1_slot).get();
    const EvalValue &k2 = ctx->frame->at(k2_slot).get();
    const EvalValue &val = ctx->frame->at(val_slot).get();
    try {
        vm_nested_subscript_store(&alv, k1, k2, val, static_cast<Op>(aop), locs);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native StoreElemChainV - a GENERIC N-level
 * nested store `a[k0][k1]...[kn] = v`, the interpreter's exact vm_chain_store_op
 * (walk the `nkeys` key values from the run [kbase, +nkeys) + store). The base
 * may be GLOBAL/CAPTURE (`kind`); `cl` = &chunk.chain_locs[idx] (baked - the
 * helper reads cl->data()/size() = the per-step carets + nkeys). An undefined-
 * global base BAILS; vm_chain_store_op throws only RuntimeExceptions (each
 * carrying its per-step caret) -> g_vm_jit_exc -> re-raise. NOT op_fully_native.
 */
extern "C" int jit_store_elem_chain(int_type kind, int_type base_slot,
                                    int_type kbase, int_type val_slot,
                                    int_type aop, const void *clv) noexcept
{
    ML_JIT_OP_RAN(StoreElemChainV);
    const std::vector<std::pair<Loc, Loc>> *cl =
        static_cast<const std::vector<std::pair<Loc, Loc>> *>(clv);
    EvalContext *ctx = g_current_ctx;
    if (kind == 1 && !ctx->gfuncs->defined[base_slot])
        return 1;                            /* bail (no exc): re-run -> throw */
    LValue *base = kind == 1 ? &ctx->gfuncs->slots[base_slot]
                 : kind == 2 ? &(*ctx->captures)[base_slot]
                             : &ctx->frame->at(base_slot);
    const EvalValue &val = ctx->frame->at(val_slot).get();
    try {
        vm_chain_store_op(*ctx, base, kbase, cl->data(), cl->size(), val,
                          static_cast<Op>(aop));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native StoreLValueChainV - a GENERAL nested
 * lvalue-chain store `base.step1.step2... = v` mixing MEMBER + SUBSCRIPT steps,
 * the interpreter's exact vm_chain_lvalue_store_op (walk the steps as lvalue
 * refs, store the final). The base may be GLOBAL/CAPTURE (`kind`); `steps` =
 * &chunk.chain_steps[idx] and `mkeys` = chunk.member_keys.data() are baked (the
 * refactored vm_chain_lvalue_store_op takes both pool pointers, not the &chunk
 * that would dangle). An undefined-global base BAILS; the chain walk throws only
 * RuntimeExceptions (each step stamps its own caret; a still-loc-less throw is
 * re-stamped by EnterNative from the loc side table, matching the interpreted
 * outer catch) -> g_vm_jit_exc -> re-raise. NOT op_fully_native.
 */
extern "C" int jit_store_lvalue_chain(int_type kind, int_type base_slot,
                                      int_type val_slot, int_type aop,
                                      const void *stepsv,
                                      const void *mkeysv) noexcept
{
    ML_JIT_OP_RAN(StoreLValueChainV);
    const std::vector<Chunk::ChainStep> *steps =
        static_cast<const std::vector<Chunk::ChainStep> *>(stepsv);
    const Chunk::MemberKey *mkeys =
        static_cast<const Chunk::MemberKey *>(mkeysv);
    EvalContext *ctx = g_current_ctx;
    if (kind == 1 && !ctx->gfuncs->defined[base_slot])
        return 1;                            /* bail (no exc): re-run -> throw */
    LValue *base = kind == 1 ? &ctx->gfuncs->slots[base_slot]
                 : kind == 2 ? &(*ctx->captures)[base_slot]
                             : &ctx->frame->at(base_slot);
    const EvalValue &val = ctx->frame->at(val_slot).get();
    try {
        vm_chain_lvalue_store_op(*ctx, *steps, mkeys, base, val,
                                 static_cast<Op>(aop));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): PER-OP runtime coverage (see jit.h). Sized by
 * the opcode count; g_jit_op_run[op] proves that op's native helper RAN. */
unsigned long g_jit_op_run[static_cast<size_t>(OpCode::OpCount_)] = {0};

/* model-flip (nativize-ops): the native MoveV body - the interpreter's exact
 * `frame->at(dst).put(frame->at(src).get())` (an alias-copy, ref-aware; never
 * throws). `slots` is the frame slot base (rdi). */
extern "C" void jit_move(LValue *slots, int_type dst, int_type src) noexcept
{
    ML_JIT_OP_RAN(MoveV);
    slots[dst].put(slots[src].get());
}

/*
 * Re-raise DELETABILITY (plans/model-flip.md): a conveying op's caret must
 * be pc-INDEPENDENT (a DELETED run collapses every pc onto its EnterNative,
 * where a loc-table lookup would be wrong). Two zero-hot-cost carriers:
 * the BoxedOp-family helpers stamp from their pool entry's start/end (the
 * pool pointer is hot-live anyway), and the pointer-arg helpers
 * (subscript/slice/dict-load/coerce, the stores) stay LOC-LESS - their
 * EMIT stamps the op's baked caret DIRECTLY INTO the conveyed exception
 * object on the COLD failure branch: load g_vm_jit_exc's pointer, and only
 * if it is non-null (a BAIL conveys nothing - the null check makes a
 * stale-state bug structurally impossible, where an earlier side-global
 * design left a value a later unrelated raise could mis-consume) and the
 * exception is still loc-less (a nested throw keeps its own caret), store
 * the start/end Locs into it. A helper ARG was measured to cost +4 Ir per
 * CALL on the hot path (the value stays live across the try -> an extra
 * callee-saved spill; 62_dict_word_count +0.6%); the cold-side stores
 * cost nothing on success. The accessors below give the emitter the
 * unique_ptr's storage address + the Loc offsets inside the object.
 */
void **jit_addr_exc()
{
    static_assert(sizeof(g_vm_jit_exc) == sizeof(void *),
                  "unique_ptr<RuntimeException> must be one raw pointer "
                  "for the native null-check + stamp");
    return reinterpret_cast<void **>(&g_vm_jit_exc);
}

ptrdiff_t jit_off_exc_loc_start()
{
    DivisionByZeroEx e;              /* any concrete subclass; loc_start
                                      * lives in the Exception base, same
                                      * offset for all (single inheritance) */
    RuntimeException *b = &e;
    return reinterpret_cast<char *>(&b->loc_start)
         - reinterpret_cast<char *>(b);
}

ptrdiff_t jit_off_exc_loc_end()
{
    DivisionByZeroEx e;
    RuntimeException *b = &e;
    return reinterpret_cast<char *>(&b->loc_end)
         - reinterpret_cast<char *>(b);
}

/* model-flip (nativize-ops): the native SubscriptV body - the interpreter's
 * exact `dst = RValue(base.subscript(idx, for_write=false))`. base_lv is passed
 * as an LValue* (like Subscript::do_eval, for COW identity). Throws -> conveyed
 * WITH the op's own (baked) caret -> op_fully_native (deletable). */
extern "C" int jit_subscript(LValue *base_lv, const EvalValue *idx,
                             LValue *dst) noexcept
{
    ML_JIT_OP_RAN(SubscriptV);
    try {
        Type *t = base_lv->get().get_type();
        dst->put(RValue(t->subscript(EvalValue(base_lv), *idx, false)));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native SliceV body - the interpreter's exact
 * `dst = base.slice(start, end)` (the runtime Type::slice: the COW-registered
 * sub-view, absent bounds passed as none). base/start/end/dst are frame slots
 * (start/end == -1 -> none, a[:j] / a[i:]); read via g_current_ctx->frame. The
 * RHS (slice) is evaluated fully before dst is written, so `dst == base` (a
 * self-slice a=a[i:j]) reads the old base first - matching the tree-walker. Only
 * TypeErrorEx (a non-int bound / non-sliceable base) is thrown (slices CLAMP
 * out-of-range indices, no OOB), and it is a RuntimeException -> caught loc-less
 * into g_vm_jit_exc + return 1 - conveyed WITH the op's own baked caret
 * (`lep`, the fifth register arg) -> op_fully_native (deletable).
 */
extern "C" int jit_slice(int_type base_slot, int_type start_slot,
                         int_type end_slot, int_type dst_slot) noexcept
{
    ML_JIT_OP_RAN(SliceV);
    EvalContext *ctx = g_current_ctx;
    const EvalValue &base = ctx->frame->at(base_slot).get();
    const EvalValue start = start_slot >= 0
        ? ctx->frame->at(start_slot).get() : EvalValue();
    const EvalValue end = end_slot >= 0
        ? ctx->frame->at(end_slot).get() : EvalValue();
    try {
        ctx->frame->at(dst_slot).put(base.get_type()->slice(base, start, end));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native LoadBuiltinV body - the interpreter's
 * exact `slots[dst] = builtin_slot[idx].get()`. A builtin is a trivial value
 * (< t_str), never throws. */
extern "C" void jit_load_builtin(LValue *slots, int_type dst,
                                 int_type idx) noexcept
{
    ML_JIT_OP_RAN(LoadBuiltinV);
    slots[dst].put(builtin_slot(static_cast<int>(idx)).get());
}

/* model-flip (nativize-ops): the native LoadCaptureV body - the interpreter's
 * exact `frame[dst] = (*ctx->captures)[idx]`. A capture is snapshot into the
 * closure's capture_slots at creation, so it is always defined and the read
 * never throws. Uses g_current_ctx (the running closure's ctx - captures +
 * frame), like jit_make_closure. */
extern "C" void jit_load_capture(int_type dst, int_type idx) noexcept
{
    ML_JIT_OP_RAN(LoadCaptureV);
    g_current_ctx->frame->at(dst).put(
        (*g_current_ctx->captures)[idx].get());
}

/* model-flip (nativize-ops): the native LoadConstV body - the interpreter's
 * exact `slots[dst] = chunk->consts[idx]` (a value copy; never throws). `src`
 * points into the const-pool buffer (baked by the emitter). */
extern "C" void jit_load_const(LValue *slots, int_type dst,
                               const EvalValue *src) noexcept
{
    ML_JIT_OP_RAN(LoadConstV);
    slots[dst].put(*src);
}

/* model-flip (nativize-ops): the native LoadLiteralObjV body - the exact
 * `slots[dst] = eval_literal_obj(...)` (immutable share vs a fresh mutable
 * clone; never throws). `lov` is a baked `&chunk.literal_objs[idx]` (void*
 * because Chunk::LiteralObjEntry is nested; the pool buffer addr is stable
 * across the chunk's std::move). */
extern "C" void jit_load_literal_obj(LValue *slots, int_type dst,
                                     const void *lov) noexcept
{
    ML_JIT_OP_RAN(LoadLiteralObjV);
    const Chunk::LiteralObjEntry *lo =
        static_cast<const Chunk::LiteralObjEntry *>(lov);
    slots[dst].put(eval_literal_obj(lo->value, lo->immutable, lo->arr_hint,
                                    lo->arr_hint_struct));
}

extern "C" void jit_store_global(int_type gslot,
                                 const EvalValue *src) noexcept
{
    ML_JIT_OP_RAN(StoreGlobalV);
    GlobalFuncTable *g = g_current_ctx->gfuncs;
    g->slots[gslot].put(RValue(*src));
    g->defined[gslot] = 1;
}

/* model-flip (nativize-ops): the native StoreCaptureV PLAIN `cap = <expr>` - the
 * capture-slot twin of jit_store_global. Writes the running closure's
 * per-instance capture slot `(*ctx->captures)[cap_slot] = RValue(*src)`; a
 * capture is ALWAYS defined (snapshot at closure creation), so - unlike a global
 * - there is no defined check and no throw (op_fully_native). Only the PLAIN
 * case is JIT-eligible (like StoreGlobalV); a COMPOUND `cap OP= v` runs
 * num_bin_op and stays interpreted. */
extern "C" void jit_store_capture(int_type cap_slot,
                                  const EvalValue *src) noexcept
{
    ML_JIT_OP_RAN(StoreCaptureV);
    (*g_current_ctx->captures)[cap_slot].put(RValue(*src));
}

/*
 * model-flip (nativize-ops): the native LoadGlobalV body - `frame[dst] =
 * gfuncs->slots[gslot]`. The COMMON case (the global is defined) runs native.
 * The RARE undefined case (a use-before-def read of a global) CONVEYS the
 * exact interpreted throw - UndefinedVariableEx(names[gslot], caret) - via
 * g_vm_jit_eptr (it is a PLAIN Exception with no clone(), so it can't ride
 * g_vm_jit_exc; the M5 eptr channel rethrows it from EnterNative, and it
 * propagates out of vm_dispatch exactly like the interpreted throw does:
 * a non-catchable hard error either way). The caret comes from the op's
 * baked LocEntry (`lep`) - pc-independent, so the op is op_fully_native
 * (deletable); the pre-eptr version BAILED to a re-interpret instead,
 * which kept the original alive. noexcept: the throw is caught and
 * converted here; the defined-slot write can't throw.
 */
extern "C" int jit_load_global(int_type dst_gslot,
                               const void *lep) noexcept
{
    /* dst_gslot = dst (lo32) | gslot (hi32) - the same two movabs as the
     * pre-deletability emit, so the lep costs nothing per call site. */
    ML_JIT_OP_RAN(LoadGlobalV);
    const int_type dst = dst_gslot & 0xffffffff;
    const int_type gslot = static_cast<int_type>(
        static_cast<uint64_t>(dst_gslot) >> 32);
    GlobalFuncTable *g = g_current_ctx->gfuncs;
    if (!g->defined[gslot]) {
        Loc s, en;
        if (lep) {
            const Chunk::LocEntry *le =
                static_cast<const Chunk::LocEntry *>(lep);
            s = le->start;
            en = le->end;
        }
        g_vm_jit_eptr = std::make_exception_ptr(
            UndefinedVariableEx(g->names[gslot]->val, s, en));
        return 1;
    }
    g_current_ctx->frame->at(dst).put(g->slots[gslot].get());
    return 0;
}

/* model-flip (nativize-ops): the native ArrLen body - the interpreter's exact
 * `frame[dst] = size(frame[base])`. `base` is a proven flat array, so size()
 * never throws (op_fully_native). */
extern "C" void jit_arr_len(LValue *slots, int_type dst, int_type base) noexcept
{
    ML_JIT_OP_RAN(ArrLen);
    slots[dst].put(EvalValue(static_cast<int_type>(
        slots[base].get().get_ref<SharedArrayObj>().size())));
}

/* model-flip (nativize-ops): the native DictLoadInt/Float body - the typed
 * scalar dict read d.k / d[k]. The KEY is baked by the emitter: a member's key
 * is `&chunk.consts[idx]` (a const-pool value), a subscript's is `&slot[k]`
 * (the key temp - EvalValue is the first LValue member), passed either way as
 * `key`. A PRESENT key reads via dict_present_value (hot); a MISSING key /
 * non-dict base runs the shared Type::subscript (the tree-walker's exact
 * default-dict insert / KeyNotFoundEx / not-subscriptable) LOC-LESS -> caught
 * into g_vm_jit_exc + return 1, so EnterNative re-raises stamping DictLoad's
 * caret from the loc side table (recorded by extract_locs). `is_int` selects
 * DictLoadInt vs Float; g_current_ctx->frame is the running callee frame. */
/* model-flip (nativize-ops): the native MakeClosureV body - the interpreter's
 * exact `frame[dst] = FuncObject(def, ctx)` (create the closure + snapshot the
 * captures from the running ctx), byte-identical to FuncDeclStmt::do_eval for a
 * lambda. `defv` is the closure's program-lifetime FuncDescriptor* (baked by the
 * emitter as a value - the descriptor address is stable, like CallV's callee).
 * A resolved closure's captures are defined, so the ctor never throws. */
extern "C" void jit_make_closure(int_type dst, const void *defv) noexcept
{
    ML_JIT_OP_RAN(MakeClosureV);
    const FuncDescriptor *def = static_cast<const FuncDescriptor *>(defv);
    g_current_ctx->frame->at(dst).put(EvalValue(intrusive_ptr<FuncObject>(
        make_intrusive<FuncObject>(def, g_current_ctx))));
}

/* model-flip (nativize-ops): the native MakeArrayV body - the interpreter's
 * exact `frame[dst] = vm_make_array(ctx, base, n, hint)`: build an array LITERAL
 * from the element run [base, base+n) via the shared build_array_from_values
 * (flat int/float/bool/struct or general per the ArrHint). It NEVER THROWS (the
 * build has no error path - a mixed literal just goes general), so the helper
 * returns void and MakeArrayV is op_fully_native (deletable). The element buffer
 * lives in vm_make_array's frame ON PURPOSE (see that helper) - never inline it
 * into a fragment or vm_run_chunk's recursion-multiplied frame. */
extern "C" void jit_make_array(int_type dst, int_type base, int_type n,
                               int_type hint) noexcept
{
    ML_JIT_OP_RAN(MakeArrayV);
    EvalContext *ctx = g_current_ctx;
    EvalValue v = vm_make_array(*ctx, base, n, static_cast<ArrHint>(hint));
    ctx->frame->at(dst).put(std::move(v));
}

/* model-flip (nativize-ops): the native MakeDictV body - the interpreter's exact
 * `frame[dst] = vm_make_dict(ctx, base, npairs)`: build a dict LITERAL from the
 * INTERLEAVED key/value run [base, base + 2*npairs) via the shared
 * build_dict_from_pairs (which FREEZES each key with make_const_clone, so the
 * VM and the tree-walker share one builder). Unlike MakeArrayV this CAN throw:
 * freezing/inserting a key HASHES it, and an UNHASHABLE key (a function value
 * laundered through `dyn`) raises TypeErrorEx - a RuntimeException, so it rides
 * g_vm_jit_exc LOC-LESS and EnterNative re-raises stamping the `{...}` literal's
 * caret from the loc side table (recorded by extract_locs). Hence NOT
 * op_fully_native. The pair buffer lives in vm_make_dict's frame ON PURPOSE
 * (the recursion-depth reason - see vm_make_array). */
extern "C" int jit_make_dict(int_type dst, int_type base,
                             int_type npairs) noexcept
{
    ML_JIT_OP_RAN(MakeDictV);
    EvalContext *ctx = g_current_ctx;
    try {
        EvalValue v = vm_make_dict(*ctx, base, npairs);
        ctx->frame->at(dst).put(std::move(v));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native ITERATOR ops. The per-loop state lives
 * on the ACTIVATION as a watermarked slice (act.dict_iters / act.dyn_iters,
 * indexed by the current record's diter_base/dyiter_base + the codegen-assigned
 * iter_id) - the helpers reach it via g_vm_act exactly like jit_ret reaches the
 * frame, computing the address FRESH per call (the vectors can realloc when a
 * nested call pushes more iterator slices). Each runs the SHARED body its
 * interpreter handler runs, so the two cannot drift. */
static ML_ALWAYS_INLINE DictIterState &jit_diter(int_type i)
{
    VmActivation &act = *g_vm_act;
    ML_VM_CHECK(i >= 0 && act.back_rec().diter_base + static_cast<size_t>(i)
                              < act.dict_iters.size());
    return act.dict_iters[act.back_rec().diter_base + i];
}
static ML_ALWAYS_INLINE DynIterState &jit_dyiter(int_type i)
{
    VmActivation &act = *g_vm_act;
    ML_VM_CHECK(i >= 0 && act.back_rec().dyiter_base + static_cast<size_t>(i)
                              < act.dyn_iters.size());
    return act.dyn_iters[act.back_rec().dyiter_base + i];
}

/* DictIterInit: pin the dict + iterator = begin(). The dict is PROVEN (a Dict
 * static type), so it never throws -> void + op_fully_native. */
extern "C" void jit_dict_iter_init(int_type iter_id,
                                   int_type dict_slot) noexcept
{
    ML_JIT_OP_RAN(DictIterInit);
    vm_dict_iter_init_body(jit_diter(iter_id),
                           g_current_ctx->frame->at(dict_slot).get());
}

/* DictIterNext: 1 = bound (fall through into the body), 0 = exhausted (the
 * fragment jumps to end_pc). Never throws -> op_fully_native. */
extern "C" int jit_dict_iter_next(int_type iter_id, int_type k_slot,
                                  int_type v_slot) noexcept
{
    ML_JIT_OP_RAN(DictIterNext);
    return vm_dict_iter_next_body(jit_diter(iter_id), *g_current_ctx->frame,
                                  k_slot, v_slot) ? 1 : 0;
}

/* ForeachDynInit: dispatch the dyn container once. `targets` is the baked
 * `&chunk.unpack_targets[idx]` (a pool-element address, stable across the
 * chunk's std::move). A non-container throws TypeErrorEx LOC-LESS ->
 * g_vm_jit_exc + return 1 (EnterNative re-raises with the container's caret
 * from the loc side table); 0 otherwise. */
extern "C" int jit_foreach_dyn_init(int_type iter_id, int_type cont_slot,
                                    int_type shape,
                                    const void *targets) noexcept
{
    ML_JIT_OP_RAN(ForeachDynInit);
    try {
        vm_foreach_dyn_init_body(
            jit_dyiter(iter_id), g_current_ctx->frame->at(cont_slot).get(),
            shape, static_cast<const std::vector<int32_t> *>(targets),
            nullptr, 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* ForeachDynNext: 1 = bound, 0 = exhausted (jump to end_pc), -1 = THREW (the
 * strict N-var unpack's TypeErrorEx rides g_vm_jit_exc LOC-LESS; EnterNative
 * stamps the container's caret from the loc side table). */
extern "C" int jit_foreach_dyn_next(int_type iter_id) noexcept
{
    ML_JIT_OP_RAN(ForeachDynNext);
    try {
        return vm_foreach_dyn_next_body(jit_dyiter(iter_id),
                                        *g_current_ctx->frame,
                                        nullptr, 0) ? 1 : 0;
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return -1;
    }
}

/* model-flip (nativize-ops): the native StructCtorV body - a standalone POD
 * struct construction `P(x, y)` from its field-arg run, via vm_struct_ctor (the
 * interpreter's exact call, incl. the H1 DST-SLOT REUSE that overwrites last
 * iteration's same-def instance in place). `defv` is the program-lifetime
 * StructTypeDef* from the struct_defs pool, baked by the emitter as a VALUE
 * (like MakeClosureV's FuncDescriptor*). The codegen's typed-scalar arg gate
 * means coerce_struct_field cannot throw here, but the field buffer is coerced
 * DEFENSIVELY, so a throw is caught LOC-LESS -> g_vm_jit_exc and EnterNative
 * re-raises stamping the construction's caret from the loc side table (vm_raise
 * stamps only when the exception has no loc). Every reachable throw is a
 * TypeErrorEx (a RuntimeException). NOT op_fully_native (side-table caret). */
extern "C" int jit_struct_ctor(const void *defv, int_type base, int_type nf,
                               int_type dst) noexcept
{
    ML_JIT_OP_RAN(StructCtorV);
    StructTypeDef *def =
        const_cast<StructTypeDef *>(static_cast<const StructTypeDef *>(defv));
    try {
        vm_struct_ctor(*g_current_ctx, def, base, nf, dst);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* The planned StructCtorV's SLOW branch (fresh alloc / aliased dst /
 * layout-probe-off): the interpreter's planned body - lean raw-slot
 * reads + direct byte stores, NEVER throws, so the fragment emits no
 * status test. The FAST inline path bumps g_jit_ctor_fast instead. */
extern "C" void jit_struct_ctor_planned(const void *defv, const void *planv,
                                        int_type dst) noexcept
{
    ML_JIT_OP_RAN(StructCtorV);
    vm_struct_ctor_planned(
        *g_current_ctx,
        const_cast<StructTypeDef *>(static_cast<const StructTypeDef *>(defv)),
        *static_cast<const Chunk::CtorPlan *>(planv), dst);
}

/* model-flip (nativize-ops): the native StructCtorBoxedV body - a BOXED (non-POD)
 * struct construction `B(a, x)` with runtime args, via vm_struct_ctor_boxed (it
 * also doubles as the CHECKED POD ctor). `bcv` is a baked
 * `&chunk.boxed_ctors[idx]` (the pool BUFFER address, stable across the chunk's
 * std::move) holding the def + the PER-ARG carets. Here a field coerce genuinely
 * CAN throw (a dyn-laundered wrong value), and the exception already carries the
 * offending arg's caret from the pool - vm_raise's stamp is conditional on an
 * EMPTY loc, so that pooled caret survives the re-raise untouched (byte-identical
 * to the tree-walker's construct_struct). NOT op_fully_native. */
extern "C" int jit_struct_ctor_boxed(int_type dst, int_type base,
                                     const void *bcv) noexcept
{
    ML_JIT_OP_RAN(StructCtorBoxedV);
    const Chunk::BoxedCtor *bc = static_cast<const Chunk::BoxedCtor *>(bcv);
    EvalContext *ctx = g_current_ctx;
    StructTypeDef *def = const_cast<StructTypeDef *>(bc->def);
    try {
        EvalValue v = vm_struct_ctor_boxed(
            *ctx, def, base, static_cast<int_type>(bc->arg_locs.size()),
            bc->arg_locs.data());
        ctx->frame->at(dst).put(std::move(v));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native MakeStructArrayV body - the FUSED flat
 * `array<PodStruct>` literal `[P(..), P(..)]`, via vm_make_struct_array_op (which
 * coerces the interleaved field values STRAIGHT into a contiguous byte buffer -
 * no per-element StructObject - and reuses the dst slot's same-def, same-count
 * buffer when it owns it). `defv` is the baked program-lifetime StructTypeDef*;
 * `n` is the ELEMENT count (the run holds n * nfields values). The all-scalar
 * field gate means the coerce can't throw; a defensive throw is caught LOC-LESS
 * -> g_vm_jit_exc, EnterNative stamps from the loc side table. NOT
 * op_fully_native. */
extern "C" int jit_make_struct_array(const void *defv, int_type base,
                                     int_type n, int_type dst) noexcept
{
    ML_JIT_OP_RAN(MakeStructArrayV);
    StructTypeDef *def =
        const_cast<StructTypeDef *>(static_cast<const StructTypeDef *>(defv));
    try {
        vm_make_struct_array_op(*g_current_ctx, def, base, n, dst);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): THE FOREACH ELEMENT/FIELD LOADS. Each is the
 * interpreter's exact handler body; the INDEX arrives as a VALUE (the emitter
 * materializes the op's slot-or-literal operand with the cache-aware
 * load_operand BEFORE the call prologue), so a foreach counter pinned in an N5
 * register is read from the REGISTER, not from a stale memory slot.
 *
 * All of these are NON-THROWING (the index is loop-bounded by the ArrLen/StrLen
 * that produced it, and the base kind is proven by the inferencer), hence void
 * + op_fully_native - EXCEPT jit_load_elem_value, whose interpreter body does
 * bounds-check (it also serves 2-D `a[i][k]` reads where the index is NOT the
 * loop counter) and can raise OutOfBoundsEx.
 */

/* model-flip (nativize-ops): the native JumpUnlessTrueV CONDITION - the BOXED
 * truthiness test behind `if (dynvalue)`, `while (flag)`, a `&&`/`||` conjunct
 * and the boxed ternary. Unlike every other nativized op this is a BRANCH: the
 * helper only evaluates the condition and the FRAGMENT does the jump (a
 * fragment-local jcc for an in-run target, an exit_pc otherwise) - which is what
 * lets a loop whose body holds a boxed condition iterate entirely in machine
 * code instead of splitting the run at the branch and leaving the back edge
 * interpreted.
 *
 * Returns 1 = true, 0 = false, -1 = THREW. `is_true` is a virtual Type op whose
 * BASE implementation throws TypeErrorEx ("does NOT support conversion to
 * bool") - reachable from a script with a builtin value as a condition
 * (`var dyn f = print; if (f)`), so the status is a tri-state, not a bool. The
 * throw rides g_vm_jit_exc LOC-LESS and EnterNative stamps the CONDITION's caret
 * from the loc side table. */
extern "C" int jit_is_true(int_type cond_slot) noexcept
{
    /* NO ML_JIT_OP_RAN here: the EMITTER bumps the counter for both paths (the
     * inline int/bool fast path calls no helper), so bumping again would
     * double-count the slow path. */
    try {
        return g_current_ctx->frame->at(cond_slot).get().is_true() ? 1 : 0;
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return -1;
    }
}

/* LoadElemBool: bind a[i] of a flat array<bool> as a REAL bool (not 0/1). */
extern "C" void jit_load_elem_bool(int_type dst, int_type base,
                                   int_type idx) noexcept
{
    ML_JIT_OP_RAN(LoadElemBool);
    Frame *f = g_current_ctx->frame;
    const SharedArrayObj &arr = f->at(base).get().get_ref<SharedArrayObj>();
    const bool b = arr.skind() == SharedArrayObj::Storage::bools
                       ? arr.flat_bools()[arr.offset() + idx] != 0
                       : arr.get_view()[idx].get().get<bool>();
    f->at(dst).put(EvalValue(b));
}

/* StrLen: the foreach bound n = the string's char count (get_view accounts for
 * a slice's offset). */
extern "C" void jit_str_len(int_type dst, int_type base) noexcept
{
    ML_JIT_OP_RAN(StrLen);
    Frame *f = g_current_ctx->frame;
    f->at(dst).put(EvalValue(static_cast<int_type>(
        f->at(base).get().get_ref<SharedStr>().get_view().size())));
}

/* LoadStrChar: bind a FRESH 1-char string for the container's i-th char -
 * matches the tree-walker's SharedStr(string(&view[i], 1)). */
extern "C" void jit_load_str_char(int_type dst, int_type base,
                                  int_type idx) noexcept
{
    ML_JIT_OP_RAN(LoadStrChar);
    Frame *f = g_current_ctx->frame;
    const std::string_view view =
        f->at(base).get().get_ref<SharedStr>().get_view();
    f->at(dst).put(EvalValue(SharedStr(std::string(&view[idx], 1))));
}

/* LoadStructFieldInt/Float: `pts[i].f` read STRAIGHT from the flat struct-array
 * bytes into a scalar slot (no StructObject) - `is_float` picks the pair. */
extern "C" void jit_load_struct_field(int_type dst, int_type base, int_type idx,
                                      int_type fidx, int is_float) noexcept
{
#ifdef TESTS
    g_jit_op_run[static_cast<size_t>(is_float ? OpCode::LoadStructFieldFloat
                                              : OpCode::LoadStructFieldInt)]++;
#endif
    EvalContext *ctx = g_current_ctx;
    const EvalValue &arrv = ctx->frame->at(base).get();
    if (is_float)
        write_float_slot(ctx, dst, vm_struct_field_float(arrv, idx, fidx));
    else
        write_int_slot(ctx, dst, vm_struct_field_int(arrv, idx, fidx));
}

/* LoadStructElemV: the whole-`p` foreach bind - materialize a fresh
 * StructObject from the flat struct-array element into the loop var. */
extern "C" void jit_load_struct_elem(int_type dst, int_type base,
                                     int_type idx) noexcept
{
    ML_JIT_OP_RAN(LoadStructElemV);
    Frame *f = g_current_ctx->frame;
    f->at(dst).put(vm_struct_elem(f->at(base).get(), idx));
}

/* LoadElemValue: a GENERAL (or flat-str) array element into a slot, box-free.
 * Unlike its siblings this BOUNDS-CHECKS (it also serves a 2-D `a[i][k]` read,
 * whose index is not the loop counter) - an OOB raises OutOfBoundsEx LOC-LESS
 * -> g_vm_jit_exc, and EnterNative re-raises with the loc side table's caret.
 * The interpreter's unreachable non-general tail is an InternalErrorEx there;
 * here the SAME InternalErrorEx is conveyed via g_vm_jit_eptr (no clone() -
 * a plain exception) so the op has NO bail and is deletable. */
extern "C" int jit_load_elem_value(int_type dst, int_type base,
                                   int_type idx) noexcept
{
    ML_JIT_OP_RAN(LoadElemValue);
    Frame *f = g_current_ctx->frame;
    const EvalValue &basev = f->at(base).get();
    if (!basev.is<SharedArrayObj>()) {
        /* the interpreted handler's unreachable tail (base_array proven):
         * CONVEY the same InternalErrorEx via eptr (a DECL_SIMPLE
         * exception, no clone()) instead of the old bail - deletability
         * forbids a re-run exit, and the propagation is identical (a
         * non-catchable hard error either way). */
        g_vm_jit_eptr = std::make_exception_ptr(InternalErrorEx());
        return 1;
    }
    const SharedArrayObj &arr = basev.get_ref<SharedArrayObj>();
    const SharedArrayObj::Storage k = arr.skind();
    if (k != SharedArrayObj::Storage::general
            && k != SharedArrayObj::Storage::strs) {
        g_vm_jit_eptr = std::make_exception_ptr(InternalErrorEx());
        return 1;
    }
    int_type i = idx;
    if (i < 0)
        i += arr.size();
    if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
        g_vm_jit_exc.reset(new OutOfBoundsEx());   /* loc-less: side table */
        return 1;
    }
    if (k == SharedArrayObj::Storage::general)
        f->at(dst).put(arr.get_vec()[arr.offset() + i].get());
    else
        f->at(dst).put(EvalValue(SharedStr(arr.flat_strs()[arr.offset() + i])));
    return 0;
}

/* TWO ENTRY POINTS (int/float) so the scalar kind is a compile-time
 * constant: the single-entry is_int ARG cost a movabs per call site, and
 * folding it into lep's low bit cost an unpack per CALL (+4M Ir on
 * 25_dict_member - measured; this form is back to the pre-deletability
 * instruction count, the lep movabs simply replacing the is_int movabs). */
static ML_ALWAYS_INLINE int
jit_dict_load_body(int_type dst, int_type base_slot, const EvalValue *key,
                   bool is_int) noexcept
{
    EvalContext *ctx = g_current_ctx;
    /* The PRESENT-key write sits inside the try too: write_scalar_slot's
     * get<> throws TypeErrorEx for a dyn-laundered WRONG-typed value (the
     * inference safety net) - previously an unguarded throw in a noexcept
     * helper == std::terminate, a latent JIT crash; now conveyed with a
     * caret. The try is the zero-cost-EH shape (a TYPE PRE-CHECK variant
     * measured WORSE: +2.0% Ir on 25_dict_member vs this form's +0.0%). */
    try {
        const EvalValue &base = ctx->frame->at(base_slot).get();
        if (base.is<intrusive_ptr<DictObject>>())
            if (const EvalValue *v = dict_present_value(
                    base.get_ref<intrusive_ptr<DictObject>>(), *key)) {
                write_scalar_slot(ctx, dst, is_int, *v);
                return 0;
            }
        LValue &dlv = ctx->frame->at(base_slot);
        EvalValue r = base.get_type()->subscript(EvalValue(&dlv), *key, false);
        write_scalar_slot(ctx, dst, is_int,
                          r.is<LValue *>() ? r.get<LValue *>()->get() : r);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

extern "C" int jit_dict_load_int(int_type dst, int_type base_slot,
                                 const EvalValue *key) noexcept
{
    ML_JIT_OP_RAN(DictLoadInt);
    return jit_dict_load_body(dst, base_slot, key, true);
}

extern "C" int jit_dict_load_float(int_type dst, int_type base_slot,
                                   const EvalValue *key) noexcept
{
    ML_JIT_OP_RAN(DictLoadFloat);
    return jit_dict_load_body(dst, base_slot, key, false);
}

/* The SHARED UnpackElem* body (the STRICT foreach-unpack of pairs[i] into N
 * loop vars): ONE implementation for the four interpreter handlers AND
 * jit_unpack_elem. `kind` 0 = int (flat-ints fast path), 1 = float, 2 =
 * value; `targets` non-null = the per-position slot list (UnpackElemTargets,
 * -1 == `_`), else the consecutive run dst_base..dst_base+N-1. The two
 * strict errors throw loc-less when `chunk` is null (the JIT; the re-raise
 * stamps the same side-table caret). */
static void
vm_unpack_elem_body(EvalContext &ctx, const EvalValue &base_v, int_type idx,
                    int_type N, int kind, int_type dst_base,
                    const std::vector<int32_t> *targets,
                    const Chunk *chunk, size_t pc)
{
    ML_VM_CHECK(base_v.is<SharedArrayObj>());
    const SharedArrayObj &outer = base_v.get_ref<SharedArrayObj>();
    const EvalValue &elem = outer.get_vec()[outer.offset() + idx].get();
    if (!elem.is<SharedArrayObj>())
        vm_throw_unpack_nonarray(chunk, pc, N);
    const SharedArrayObj &sub = elem.get_ref<SharedArrayObj>();
    if (sub.size() != static_cast<size_type>(N))
        vm_throw_unpack_len(chunk, pc, sub.size(), N);
    if (targets) {
        for (int_type k = 0; k < N; k++) {
            if ((*targets)[k] >= 0)
                ctx.frame->at((*targets)[k]).put(
                    vm_arr_elem(elem, static_cast<size_type>(k)));
        }
        return;
    }
    const size_type off = sub.offset();
    const auto sk = sub.skind();
    if (kind == 0 && sk == SharedArrayObj::Storage::ints) {
        for (int_type k = 0; k < N; k++)
            write_int_slot(&ctx, dst_base + k, sub.flat_ints()[off + k]);
    } else if (kind == 1 && sk == SharedArrayObj::Storage::floats) {
        for (int_type k = 0; k < N; k++)
            write_float_slot(&ctx, dst_base + k, sub.flat_floats()[off + k]);
    } else {
        /* UnpackElemValue, OR a flat op whose sub-array's storage is NOT the
         * expected kind (a mixed-numeric literal built GENERAL, or the other
         * scalar kind): bind each element's ACTUAL boxed value - identical
         * to do_iter's bind_loop_var. */
        for (int_type k = 0; k < N; k++)
            ctx.frame->at(dst_base + k).put(
                vm_arr_elem(elem, static_cast<size_type>(k)));
    }
}

/* The SHARED MultiUnpackV body (the multi-assign strict destructure /
 * spread): ONE implementation for the interpreter handler AND
 * jit_multi_unpack. A null `chunk` makes the throws loc-less (the JIT; the
 * re-raise stamps the same side-table caret vm_stamp_loc would). */
static void
vm_multi_unpack_body(EvalContext &ctx, const EvalValue &rval,
                     const std::vector<int32_t> &targets,
                     const std::vector<unsigned char> *coerce, Op aop,
                     const Chunk *chunk, size_t pc)
{
    const bool compound = aop != Op::invalid;
    size_t ti = 0;
    const auto store = [&](int32_t t, const EvalValue &v) {
        if (!compound) {
            if (coerce && (*coerce)[ti]) {
                try {
                    ctx.frame->at(t).put(vm_coerce_decl_num(
                        v, (*coerce)[ti] == 2));
                } catch (Exception &e) {
                    if (chunk)
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
            vm_num_binop(nv, v, aop);
        } catch (Exception &e) {
            if (chunk)
                vm_stamp_loc(*chunk, pc, e);
            throw;
        }
        ctx.frame->at(t).put(std::move(nv));
    };
    if (rval.is<SharedArrayObj>()) {
        const size_type m = rval.get_ref<SharedArrayObj>().size();
        if (m != static_cast<size_type>(targets.size()))
            vm_throw_multi_unpack_len(chunk, pc, m, targets.size());
        for (size_t i = 0; i < targets.size(); i++) {
            ti = i;
            if (targets[i] >= 0)
                store(targets[i], vm_arr_elem(rval,
                                              static_cast<size_type>(i)));
        }
    } else {
        for (size_t i = 0; i < targets.size(); i++) {
            ti = i;
            if (targets[i] >= 0)
                store(targets[i], rval);
        }
    }
}

/* model-flip (nativize-ops): the native UnpackElem* / MultiUnpackV bodies -
 * the shared strict-unpack cores above. `targets` / `coerce` are baked pool
 * BUFFER addresses. Throws -> g_vm_jit_exc + return 1 (the loc-less throw
 * gets the side-table caret at the re-raise); 0 on success. */
extern "C" int jit_unpack_elem(int_type dst_base, int_type base_slot,
                               int_type idx, int_type n_kind,
                               const void *targets) noexcept
{
#ifdef TESTS
    {
        const int k = static_cast<int>(n_kind >> 8);
        g_jit_op_run[static_cast<size_t>(
            targets ? OpCode::UnpackElemTargets
                    : k == 0 ? OpCode::UnpackElemInt
                    : k == 1 ? OpCode::UnpackElemFloat
                             : OpCode::UnpackElemValue)]++;
    }
#endif
    EvalContext *ctx = g_current_ctx;
    try {
        vm_unpack_elem_body(
            *ctx, ctx->frame->at(base_slot).get(), idx,
            n_kind & 0xff, static_cast<int>(n_kind >> 8), dst_base,
            static_cast<const std::vector<int32_t> *>(targets), nullptr, 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

extern "C" int jit_multi_unpack(int_type rval_slot, const void *targets,
                                const void *coerce, int_type aop) noexcept
{
    ML_JIT_OP_RAN(MultiUnpackV);
    EvalContext *ctx = g_current_ctx;
    try {
        vm_multi_unpack_body(
            *ctx, ctx->frame->at(rval_slot).get(),
            *static_cast<const std::vector<int32_t> *>(targets),
            static_cast<const std::vector<unsigned char> *>(coerce),
            static_cast<Op>(aop), nullptr, 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* The SHARED IncDecCheckedV body (the dyn scalar ++/--): int/float-only
 * (a TypeErrorEx otherwise - loc-less when `chunk` is null, the JIT; the
 * re-raise stamps the same side-table caret), then +-1 in place. ONE
 * implementation for the interpreter handler AND jit_incdec_checked. */
static void
vm_incdec_scalar_body(LValue &lv, bool is_inc, const Chunk *chunk, size_t pc)
{
    EvalValue nv = lv.get();
    if (!nv.is<int_type>() && !nv.is<float_type>()) {
        Loc s, en;
        if (chunk)
            chunk->loc_at(pc, s, en);
        throw TypeErrorEx("'++'/'--' requires an int or float", s, en);
    }
    vm_num_binop(nv, EvalValue(static_cast<int_type>(1)),
                 is_inc ? Op::plus : Op::minus);
    lv.put(std::move(nv));
}

/* model-flip (nativize-ops): the native CHECKED INC-DEC bodies. Each forms
 * its base/root like the interpreter, EXCEPT an undefined GLOBAL, which
 * BAILS (return 1 with NO exception set - UndefinedVariableEx is not
 * conveyable via g_vm_jit_exc; the interpreter re-runs the op and throws
 * with the side-table caret, the LoadGlobalV pattern). Every other throw is
 * a RuntimeException -> g_vm_jit_exc + return 1; the Elem/Member/Chain
 * throws carry their POOLED carets (which survive the re-raise), the scalar
 * TypeErrorEx is loc-less -> the side-table caret. */
extern "C" int jit_incdec_checked(int_type slot, int_type kind,
                                  int_type is_inc) noexcept
{
    ML_JIT_OP_RAN(IncDecCheckedV);
    EvalContext *ctx = g_current_ctx;
    LValue *lvp;
    if (kind == 1) {
        if (!ctx->gfuncs->defined[slot])
            return 1;                       /* bail: interpreter re-runs */
        lvp = &ctx->gfuncs->slots[slot];
    } else if (kind == 2) {
        lvp = &(*ctx->captures)[slot];
    } else {
        lvp = &ctx->frame->at(slot);
    }
    try {
        vm_incdec_scalar_body(*lvp, is_inc != 0, nullptr, 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

extern "C" int jit_incdec_elem(int_type kind, int_type base_slot,
                               int_type key_slot, int_type is_inc,
                               const void *sitev) noexcept
{
    ML_JIT_OP_RAN(IncDecElemCheckedV);
    EvalContext *ctx = g_current_ctx;
    const Chunk::IncDecSite &site =
        *static_cast<const Chunk::IncDecSite *>(sitev);
    LValue *blv;
    if (kind == 1) {
        if (!ctx->gfuncs->defined[base_slot])
            return 1;                       /* bail */
        blv = &ctx->gfuncs->slots[base_slot];
    } else if (kind == 2) {
        blv = &(*ctx->captures)[base_slot];
    } else {
        blv = &ctx->frame->at(base_slot);
    }
    try {
        vm_incdec_elem(blv, ctx->frame->at(key_slot).get(), is_inc != 0,
                       site.lstart, site.lend, site.istart, site.iend);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

extern "C" int jit_incdec_member(int_type kind, int_type base_slot,
                                 int_type is_inc, const void *sitev) noexcept
{
    ML_JIT_OP_RAN(IncDecMemberCheckedV);
    EvalContext *ctx = g_current_ctx;
    const Chunk::IncDecSite &site =
        *static_cast<const Chunk::IncDecSite *>(sitev);
    LValue *blv;
    if (kind == 1) {
        if (!ctx->gfuncs->defined[base_slot])
            return 1;                       /* bail */
        blv = &ctx->gfuncs->slots[base_slot];
    } else if (kind == 2) {
        blv = &(*ctx->captures)[base_slot];
    } else {
        blv = &ctx->frame->at(base_slot);
    }
    try {
        vm_incdec_member(blv, site.memId, site.memUid, is_inc != 0,
                         site.lstart, site.lend, site.istart, site.iend);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

extern "C" int jit_incdec_chain(int_type root_kind, int_type root_slot,
                                int_type dst, int_type is_inc,
                                const void *chainv,
                                const void *mkeys) noexcept
{
    ML_JIT_OP_RAN(IncDecChainV);
    EvalContext *ctx = g_current_ctx;
    const Chunk::IncDecChain &site =
        *static_cast<const Chunk::IncDecChain *>(chainv);
    EvalValue cur;
    if (root_kind == 3) {
        cur = ctx->frame->at(root_slot).get();   /* rvalue root: a VALUE */
    } else if (root_kind == 1) {
        if (!ctx->gfuncs->defined[root_slot])
            return 1;                       /* bail */
        cur = EvalValue(&ctx->gfuncs->slots[root_slot]);
    } else if (root_kind == 2) {
        cur = EvalValue(&(*ctx->captures)[root_slot]);
    } else {
        cur = EvalValue(&ctx->frame->at(root_slot));
    }
    try {
        vm_incdec_chain_core(*ctx, site,
                             static_cast<const Chunk::MemberKey *>(mkeys),
                             std::move(cur), dst, is_inc != 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* The BAKED member read (the 64_struct_create fix): try_member_scalar
 * resolved the field at COMPILE time (b dual: lo = byte offset or -1,
 * hi = struct_defs idx << 2 | form) - so the hot path is a def-identity
 * check + one byte read, NO name scan (slot_of compared interned names
 * over the fields vector on EVERY read before). Forms: 0 int, 1 float,
 * 2 bool->0/1, 3 int promoted to float. Returns false (untouched dst) on
 * any mismatch -> the generic vm_load_member_scalar below. */
static ML_ALWAYS_INLINE bool
vm_load_member_baked(EvalContext &ctx, const Instr *in, const Chunk *chunk)
{
    const int32_t off = in->b_dual_lo();
    if (off < 0)
        return false;
    const EvalValue &b = ctx.frame->at(in->target2).get();
    if (!b.is<intrusive_ptr<StructObject>>())
        return false;
    const int32_t hi = in->b_dual_hi();
    const StructObject &o = *b.get_ref<intrusive_ptr<StructObject>>().get();
    if (o.def != chunk->struct_defs[hi >> 2])
        return false;
    const char *p = o.bytes.data() + off;
    switch (hi & 3) {
    case 0: {
        int_type v;
        std::memcpy(&v, p, sizeof v);
        write_int_slot(&ctx, in->target, v);
        break;
    }
    case 1: {
        float_type v;
        std::memcpy(&v, p, sizeof v);
        write_float_slot(&ctx, in->target, v);
        break;
    }
    case 2:
        write_int_slot(&ctx, in->target,
                       static_cast<unsigned char>(*p) != 0 ? 1 : 0);
        break;
    default: {                                  /* int field read as float */
        int_type v;
        std::memcpy(&v, p, sizeof v);
        write_float_slot(&ctx, in->target, static_cast<float_type>(v));
        break;
    }
    }
    return true;
}

/* The SHARED LoadMemberInt/Float body (H1 - the typed standalone struct-member
 * read `p.x`, th==i/f): the POD fast path reads the scalar straight from the
 * instance's bytes; anything else (boxed struct / dict / const member / a dyn
 * base) falls to member_read_core + write_scalar_slot, whose loc-less get<>
 * throw is stamped with the member's caret. ONE implementation for the
 * interpreter handlers AND jit_load_member, so they cannot drift. */
static void
vm_load_member_scalar(EvalContext &ctx, const Chunk::MemberKey &mk,
                      int_type base_slot, int_type dst, bool is_int)
{
    const EvalValue &b = ctx.frame->at(base_slot).get();
    if (b.is<intrusive_ptr<StructObject>>()) {
        const StructObject &o = *b.get<intrusive_ptr<StructObject>>().get();
        const int fs = o.def->slot_of(mk.memUid);
        if (fs >= 0 && o.is_pod()) {
            const FieldDef &f = o.def->fields[fs];
            const char *p = o.bytes.data() + f.offset;
            if (is_int) {
                if (f.kind == FieldKind::f_int) {
                    int_type v;
                    std::memcpy(&v, p, sizeof v);
                    write_int_slot(&ctx, dst, v);
                    return;
                }
                if (f.kind == FieldKind::f_bool) {
                    write_int_slot(&ctx, dst,
                        static_cast<unsigned char>(*p) != 0 ? 1 : 0);
                    return;
                }
            } else {
                if (f.kind == FieldKind::f_float) {
                    float_type v;
                    std::memcpy(&v, p, sizeof v);
                    write_float_slot(&ctx, dst, v);
                    return;
                }
                if (f.kind == FieldKind::f_int) {
                    int_type v;
                    std::memcpy(&v, p, sizeof v);
                    write_float_slot(&ctx, dst, static_cast<float_type>(v));
                    return;
                }
            }
        }
    }
    try {
        write_scalar_slot(&ctx, dst, is_int,
            member_read_core(b, mk.memId, mk.memUid, mk.optional,
                             mk.mstart, mk.mend, mk.bstart, mk.bend));
    } catch (Exception &e) {
        /* write_scalar_slot's get<> throw is loc-less - give it the member's
         * caret (member_read_core's own throws carry it) */
        if (!e.loc_start) {
            e.loc_start = mk.mstart;
            e.loc_end = mk.mend;
        }
        throw;
    }
}

/* model-flip (nativize-ops): the native LoadMemberInt/Float body - the shared
 * H1 typed member read above. `mkv` is a baked `&chunk.member_keys[idx]`. The
 * fallback path throws WITH the member caret -> g_vm_jit_exc + return 1
 * (vm_raise's empty-loc-only stamp keeps it); 0 on success. */
extern "C" int jit_load_member(int_type dst, int_type base_slot,
                               const void *mkv, int is_int) noexcept
{
#ifdef TESTS
    g_jit_op_run[static_cast<size_t>(
        is_int ? OpCode::LoadMemberInt : OpCode::LoadMemberFloat)]++;
#endif
    EvalContext *ctx = g_current_ctx;
    const Chunk::MemberKey *mk = static_cast<const Chunk::MemberKey *>(mkv);
    try {
        vm_load_member_scalar(*ctx, *mk, base_slot, dst, is_int != 0);
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native DeclConstV body - bind a const
 * arr/dict/func decl's slot as a CONST LValue (so a later rebind still
 * throws), local or global. The rvalue is already materialized in `src`.
 * Never throws. */
extern "C" void jit_decl_const(int_type dst, int_type is_global,
                               int_type src) noexcept
{
    ML_JIT_OP_RAN(DeclConstV);
    EvalContext *ctx = g_current_ctx;
    EvalValue v = ctx->frame->at(src).get();
    if (!is_global) {
        ctx->frame->at(dst) = LValue(std::move(v), true);
    } else {
        ctx->gfuncs->slots[dst] = LValue(std::move(v), true);
        ctx->gfuncs->defined[dst] = 1;
    }
}

/* model-flip (nativize-ops): the native DefinedGlobalV body - `defined(g)`
 * is the global slot's defined-flag as a bool. Never throws. */
extern "C" void jit_defined_global(int_type dst, int_type gslot) noexcept
{
    ML_JIT_OP_RAN(DefinedGlobalV);
    EvalContext *ctx = g_current_ctx;
    ctx->frame->at(dst).put(EvalValue(ctx->gfuncs->defined[gslot] != 0));
}

/* model-flip (nativize-ops): the native StructFieldAddInt READ half - the #9
 * fusion `dst = other + a[i].f`'s proven no-fault field read (the exact
 * vm_struct_field_int the interpreter calls). The ADD and the dst write run
 * in the FRAGMENT (cache-aware - the dst is the reduction's hot accumulator,
 * which must stay N5-pinnable), so the helper only reads. Never throws. */
extern "C" int_type jit_struct_field_add_int(int_type base_slot, int_type idx,
                                             int_type fidx) noexcept
{
    ML_JIT_OP_RAN(StructFieldAddInt);
    Frame *f = g_current_ctx->frame;
    return vm_struct_field_int(f->at(base_slot).get(), idx, fidx);
}

/* model-flip (nativize-ops): the native EmplaceStruct body - append(struct_arr,
 * Ctor(args)) with the ctor's field VALUES in the run at run_base. Forms
 * arg0's LValue* by kind EXACTLY like the interpreter handler (an undefined
 * GLOBAL passes nullptr - vm_do_emplace's own error), runs the shared
 * vm_do_emplace, writes dst. A throw (coerce / const / non-lvalue) rides
 * g_vm_jit_exc; a LOC-LESS one gets this pc's side-table caret at the
 * re-raise == the interpreted handler's vm_stamp_loc; one carrying a pooled
 * per-field caret keeps it (vm_raise stamps only empty locs). `sitev` = a
 * baked &chunk.emplace_sites[idx]. */
extern "C" int jit_emplace_struct(int_type dst, int_type base_slot,
                                  int_type kind, const void *sitev,
                                  int_type run_base) noexcept
{
    ML_JIT_OP_RAN(EmplaceStruct);
    EvalContext *ctx = g_current_ctx;
    const Chunk::EmplaceSite &site =
        *static_cast<const Chunk::EmplaceSite *>(sitev);
    LValue *target;
    switch (kind) {
    case 0:  target = &ctx->frame->at(base_slot); break;
    case 1:  target = ctx->gfuncs->defined[base_slot]
                 ? &ctx->gfuncs->slots[base_slot] : nullptr; break;
    default: target = &(*ctx->captures)[base_slot]; break;
    }
    try {
        ctx->frame->at(dst).put(vm_do_emplace(*ctx, site, target, run_base));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native MemberV body - the interpreter's exact
 * `dst = member_read_core(base, key...)`. `mkv` is a `&chunk.member_keys[idx]`
 * (baked; void* because Chunk::MemberKey is nested). A missing field/key throws
 * WITH the member caret already set, so EnterNative's re-raise preserves it. */
extern "C" int jit_member(const EvalValue *base, LValue *dst,
                          const void *mkv) noexcept
{
    const Chunk::MemberKey *mk = static_cast<const Chunk::MemberKey *>(mkv);
    ML_JIT_OP_RAN(MemberV);
    try {
        dst->put(member_read_core(*base, mk->memId, mk->memUid, mk->optional,
                                  mk->mstart, mk->mend, mk->bstart, mk->bend));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native BOXED-ARITH bodies (BinOpV / CmpV /
 * CompoundV) - the interpreter's EXACT bodies (boxed_operand + vm_num_binop,
 * int/float promotion, string `+`, bitwise). `bop` is a `&chunk.boxed_ops[idx]`
 * (baked pool-buffer address, stable across the chunk move); it holds the FINAL
 * operand data (target/a/b/aop) so the JIT never bakes an unstable &code[pc].
 * g_current_ctx->frame is the running callee frame. A num_bin_op throw (div0 /
 * type) is caught LOC-LESS into g_vm_jit_exc + returns 1, so EnterNative
 * re-raises stamping the op's caret from the loc side table (extract_locs
 * records these ops) - byte-identical to the interpreted stamp_operand_loc. */
extern "C" int jit_boxed_binop(const void *bop) noexcept
{
    ML_JIT_OP_RAN(BinOpV);
    const Chunk::BoxedOp *bo = static_cast<const Chunk::BoxedOp *>(bop);
    EvalContext *ctx = g_current_ctx;
    EvalValue sa, sb;
    EvalValue val = boxed_operand(bo->a, ctx, sa).clone();
    try {
        vm_num_binop(val, boxed_operand(bo->b, ctx, sb), bo->aop);
    } catch (RuntimeException &e) {
        /* deletability: the op's own caret from the pool entry */
        if (!e.loc_start) { e.loc_start = bo->start; e.loc_end = bo->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    ctx->frame->at(bo->target).put(std::move(val));
    return 0;
}

extern "C" int jit_boxed_cmp(const void *bop) noexcept
{
    ML_JIT_OP_RAN(CmpV);
    const Chunk::BoxedOp *bo = static_cast<const Chunk::BoxedOp *>(bop);
    EvalContext *ctx = g_current_ctx;
    EvalValue sa, sb;
    EvalValue val = boxed_operand(bo->a, ctx, sa);
    try {
        vm_num_binop(val, boxed_operand(bo->b, ctx, sb), bo->aop);
    } catch (RuntimeException &e) {
        /* deletability: the op's own caret from the pool entry */
        if (!e.loc_start) { e.loc_start = bo->start; e.loc_end = bo->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    ctx->frame->at(bo->target).put(EvalValue(val.is_true()));
    return 0;
}

extern "C" int jit_boxed_compound(const void *bop) noexcept
{
    ML_JIT_OP_RAN(CompoundV);
    const Chunk::BoxedOp *bo = static_cast<const Chunk::BoxedOp *>(bop);
    EvalContext *ctx = g_current_ctx;
    EvalValue sb;
    EvalValue nv = ctx->frame->at(bo->target).get();
    try {
        vm_num_binop(nv, boxed_operand(bo->b, ctx, sb), bo->aop);
    } catch (RuntimeException &e) {
        /* deletability: the op's own caret from the pool entry */
        if (!e.loc_start) { e.loc_start = bo->start; e.loc_end = bo->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    ctx->frame->at(bo->target).put(std::move(nv));
    return 0;
}

/*
 * model-flip (nativize-ops): the native COMPOUND global store `g OP= rhs` /
 * `g++` - the interpreter's compound StoreGlobalV branch. Reuses the boxed_ops
 * pool (bo->target = the GLOBAL slot, bo->a = the rhs operand, bo->aop = the
 * op). An UNDEFINED global BAILS (return 1, no g_vm_jit_exc -> the interpreter
 * re-runs + throws UndefinedVariableEx, like jit_load_global). A num_bin_op
 * throw (div0/type, loc-less) -> g_vm_jit_exc + return 1, so EnterNative
 * re-raises with the caret from the loc side table (like the interpreted
 * vm_stamp_loc). Both exits return 1; EnterNative distinguishes by whether
 * g_vm_jit_exc is set. NOT op_fully_native.
 */
extern "C" int jit_store_global_compound(const void *bop) noexcept
{
    ML_JIT_OP_RAN(StoreGlobalV);
    const Chunk::BoxedOp *bo = static_cast<const Chunk::BoxedOp *>(bop);
    EvalContext *ctx = g_current_ctx;
    GlobalFuncTable *g = ctx->gfuncs;
    if (!g->defined[bo->target]) {
        /* re-raise deletability: CONVEY the exact interpreted throw (the
         * eptr channel - UndefinedVariableEx has no clone()) with the
         * name + the op's own pool caret, instead of the old bail-to-
         * re-run; the pool entry is already loaded, so this costs nothing
         * on the defined path. */
        g_vm_jit_eptr = std::make_exception_ptr(UndefinedVariableEx(
            g->names[bo->target]->val, bo->start, bo->end));
        return 1;
    }
    LValue &lv = g->slots[bo->target];
    EvalValue sb;
    EvalValue nv = lv.get();
    try {
        vm_num_binop(nv, boxed_operand(bo->a, ctx, sb), bo->aop);
    } catch (RuntimeException &e) {
        /* deletability: the op's own caret from the pool entry */
        if (!e.loc_start) { e.loc_start = bo->start; e.loc_end = bo->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    lv.put(std::move(nv));
    return 0;
}

/* model-flip (nativize-ops): the native COMPOUND capture store `cap OP= rhs` /
 * `cap++` - the interpreter's compound StoreCaptureV branch. Like
 * jit_store_global_compound but the slot is a CAPTURE (bo->target) - a capture
 * is always defined, so NO bail (only the num_bin_op re-raise). NOT
 * op_fully_native. */
extern "C" int jit_store_capture_compound(const void *bop) noexcept
{
    ML_JIT_OP_RAN(StoreCaptureV);
    const Chunk::BoxedOp *bo = static_cast<const Chunk::BoxedOp *>(bop);
    EvalContext *ctx = g_current_ctx;
    LValue &lv = (*ctx->captures)[bo->target];
    EvalValue sb;
    EvalValue nv = lv.get();
    try {
        vm_num_binop(nv, boxed_operand(bo->a, ctx, sb), bo->aop);
    } catch (RuntimeException &e) {
        /* deletability: the op's own caret from the pool entry */
        if (!e.loc_start) { e.loc_start = bo->start; e.loc_end = bo->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    lv.put(std::move(nv));
    return 0;
}

/* model-flip (nativize-ops): the native LogV body - EAGER logical `a && b` /
 * `a || b` (MyLang's don't short-circuit at runtime; both operands are already
 * computed). is_true() never throws, so LogV is op_fully_native (no eax check).
 * `bop` is a `&chunk.boxed_ops[idx]` (the same pool as the arith ops). */
extern "C" int jit_boxed_log(const void *bop) noexcept
{
    ML_JIT_OP_RAN(LogV);
    const Chunk::BoxedOp *bo = static_cast<const Chunk::BoxedOp *>(bop);
    EvalContext *ctx = g_current_ctx;
    try {
        EvalValue sa, sb;
        const bool a = boxed_operand(bo->a, ctx, sa).is_true();
        const bool b = boxed_operand(bo->b, ctx, sb).is_true();
        ctx->frame->at(bo->target).put(
            EvalValue(bo->aop == Op::land ? (a && b) : (a || b)));
    } catch (RuntimeException &e) {
        /* is_true's BASE Type op throws for an operand with no bool conversion
         * (a builtin value laundered through `dyn`). This helper is noexcept, so
         * before this catch existed the exception ESCAPED -> std::terminate: a
         * real JIT crash, from the "is_true never throws" assumption LogV was
         * nativized under. Re-raise like every other throwing op. */
        /* deletability: the op's own caret from the pool entry */
        if (!e.loc_start) { e.loc_start = bo->start; e.loc_end = bo->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native UnaryV body - boxed unary over a
 * dyn/general operand (Expr02::do_eval: clone the operand, apply). `-str`/`~str`
 * throw a type error via the Type vtable -> caught into g_vm_jit_exc + return 1
 * (EnterNative re-raises with the operand caret from the loc side table).
 * Reuses the boxed_ops pool (1-operand: target/a/aop, b unused). */
extern "C" int jit_unary(const void *bop) noexcept
{
    ML_JIT_OP_RAN(UnaryV);
    const Chunk::BoxedOp *bo = static_cast<const Chunk::BoxedOp *>(bop);
    EvalContext *ctx = g_current_ctx;
    EvalValue s;
    EvalValue v = boxed_operand(bo->a, ctx, s).clone();
    try {
        switch (bo->aop) {
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
            break;                     /* unreachable: codegen emits a valid
                                        * unary aop only (the interpreter's
                                        * InternalErrorEx default is dead) */
        }
    } catch (RuntimeException &e) {
        /* deletability: the op's own caret from the pool entry */
        if (!e.loc_start) { e.loc_start = bo->start; e.loc_end = bo->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    ctx->frame->at(bo->target).put(std::move(v));
    return 0;
}

/* model-flip (nativize-ops): the native CoerceNumV body - the typed-store
 * numeric coerce (the coerces_dyn accumulator's plain assign): widen
 * float<-int/bool, int<-bool, pass none, THROW TypeError on a non-fitting dyn
 * value. `src_slot` holds the dyn value, `dst` the typed int/float slot,
 * `is_float` the target kind. A throw catches LOC-LESS into g_vm_jit_exc +
 * returns 1 -> EnterNative re-raises with the Expr14 caret from the loc side
 * table (extract_locs records it), byte-identical to the interpreted stamp. */
extern "C" int jit_coerce_num(int_type dst, int_type src_slot,
                              int is_float) noexcept
{
    ML_JIT_OP_RAN(CoerceNumV);
    EvalContext *ctx = g_current_ctx;
    try {
        ctx->frame->at(dst).put(vm_coerce_decl_num(
            ctx->frame->at(src_slot).get(), is_float != 0));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* model-flip (nativize-ops): the native CallBuiltinV body - a value-ABI
 * read-only builtin call. `bcv` is a baked `&chunk.builtin_calls[idx]` (the pool
 * entry: the func_v pointer, the args-list caret start/end, per-arg carets,
 * arr_hint). The ArgLocs is built from it (no chunk needed - exactly
 * arglocs_at's fields). Args are copied from frame slots [base, base+n) into a
 * stack buffer (n<=8) or a heap one, then func_v runs the interpreter's EXACT
 * body - a builtin with a callback (make_array/make_dict/find) re-enters
 * vm_dispatch, which saves/restores g_current_ctx, so `ctx` is valid for the dst
 * write after. Every REACHABLE builtin throw is now a RuntimeException (arity +
 * bad-argument are DECL_RUNTIME_EX; the only DECL_SIMPLE one, InternalErrorEx in
 * str() on snprintf<0, is a can't-happen defensive check) - caught, the loc
 * stamped from the pool (bc->start/end, exactly the interpreter's stamp), cloned
 * into g_vm_jit_exc + return 1 -> EnterNative re-raises with that (already-set)
 * loc. */
extern "C" int jit_call_builtin(int_type dst, int_type base, int_type n,
                                const void *bcv) noexcept
{
    ML_JIT_OP_RAN(CallBuiltinV);
    const Chunk::BuiltinCall *bc =
        static_cast<const Chunk::BuiltinCall *>(bcv);
    EvalContext *ctx = g_current_ctx;
    ArgLocs al;
    al.start = bc->start;
    al.end = bc->end;
    al.args = bc->args.data();
    al.nargs = bc->args.size();
    al.arr_hint = bc->arr_hint;
    try {
        EvalValue r;
        if (n <= 8) {
            EvalValue stackbuf[8];
            for (int_type i = 0; i < n; i++)
                stackbuf[i] = ctx->frame->at(base + i).get();
            r = bc->builtin.func_v(ctx, &al, stackbuf, n);
        } else {
            std::vector<EvalValue> heapbuf(static_cast<size_t>(n));
            for (int_type i = 0; i < n; i++)
                heapbuf[i] = ctx->frame->at(base + i).get();
            r = bc->builtin.func_v(ctx, &al, heapbuf.data(), n);
        }
        ctx->frame->at(dst).put(std::move(r));
    } catch (RuntimeException &e) {
        if (!e.loc_start) { e.loc_start = bc->start; e.loc_end = bc->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/* Re-raise deletability (ThrowRuntimeV): build the POOLED exception
 * natively - `tv` is a baked &chunk.throws[idx] (kind + the exact caret +
 * the name). The two RUNTIME kinds ride g_vm_jit_exc; the three PLAIN
 * kinds (UndefinedVariableEx / CannotRebind*) ride g_vm_jit_eptr - the
 * channel that did not exist when the op's old unconditional-exit
 * (re-run-to-throw) form was chosen. Every kind carries its pooled loc at
 * CONSTRUCTION, so the caret is pc-independent and the op is deletable. */
extern "C" int jit_throw_runtime(const void *tv) noexcept
{
    ML_JIT_OP_RAN(ThrowRuntimeV);
    const Chunk::ThrowSite &t = *static_cast<const Chunk::ThrowSite *>(tv);
    switch (t.kind) {
    case Chunk::ThrowKind::not_lvalue:
        g_vm_jit_exc = std::make_unique<NotLValueEx>(t.start, t.end);
        break;
    case Chunk::ThrowKind::bad_args:
        g_vm_jit_exc =
            std::make_unique<InvalidNumberOfArgsEx>(t.start, t.end);
        break;
    case Chunk::ThrowKind::undefined_var:
        g_vm_jit_eptr = std::make_exception_ptr(
            UndefinedVariableEx(t.name->val, t.start, t.end));
        break;
    case Chunk::ThrowKind::rebind_builtin:
        g_vm_jit_eptr = std::make_exception_ptr(
            CannotRebindBuiltinEx(t.start, t.end));
        break;
    case Chunk::ThrowKind::rebind_const:
        g_vm_jit_eptr = std::make_exception_ptr(
            CannotRebindConstEx(t.start, t.end));
        break;
    }
    return 1;
}

/* Re-raise deletability (the raise-kind ops): build the JR-kind exception
 * LOC-LESS into g_vm_jit_exc from the fragment's cold raise branch; the
 * emit's exc-stamp then adds the op's own caret, so the op CONVEYS like
 * the rest of the family instead of signalling g_vm_jit_raise (whose
 * exception EnterNative builds with a loc_at(pc) caret - wrong at a
 * deleted run's collapsed pc). The construction mirrors EnterNative's
 * exactly. Cold - called only when the div/shift actually faults. */
extern "C" void jit_raise_kind_exc(int kind) noexcept
{
    g_vm_jit_exc.reset(
        kind == JR_DIV0
            ? static_cast<RuntimeException *>(new DivisionByZeroEx())
            : static_cast<RuntimeException *>(
                  new InvalidValueEx("negative shift count")));
}

/* model-flip (nativize-ops): the native CheckCallableV - an indirect
 * call's callable guard (FuncObject / Builtin / struct type descriptor),
 * run BEFORE the arg run evaluates (the tree-walker's dispatch order). A
 * non-callable conveys a LOC-LESS NotCallableEx; the emit's cold-side
 * exc-stamp gives it the callee caret -> op_fully_native (deletable). */
extern "C" int jit_check_callable(int_type slot) noexcept
{
    ML_JIT_OP_RAN(CheckCallableV);
    const EvalValue &cv = g_current_ctx->frame->at(slot).get();
    if (cv.is<intrusive_ptr<FuncObject>>() || cv.is<Builtin>()
            || cv.is<StructTypeDef *>())
        return 0;
    g_vm_jit_exc = std::make_unique<NotCallableEx>();
    return 1;
}


/* model-flip (nativize-ops): the native CheckFuncV - map/filter's arg0
 * guard. A non-function conveys a LOC-LESS TypeErrorEx; EnterNative's
 * re-raise (vm_raise) stamps arg0's caret from the loc table at the op's
 * pc - exactly the loc_at(pc) the interpreted throw uses. */
extern "C" int jit_check_func(int_type slot) noexcept
{
    ML_JIT_OP_RAN(CheckFuncV);
    EvalContext *ctx = g_current_ctx;
    if (ctx->frame->at(slot).get().is<intrusive_ptr<FuncObject>>())
        return 0;
    g_vm_jit_exc = std::make_unique<TypeErrorEx>("Expected function");
    return 1;
}

/* model-flip (nativize-ops): the native MapFilterV - map/filter over the
 * pre-validated function + container via the SHARED vm_map_filter (the
 * interpreter's exact body; a callback re-enters vm_dispatch through
 * VmInvoker, which owns g_current_ctx for its loop, so `ctx` stays valid
 * for the dst write). The unsupported-container TypeErrorEx is built
 * LOC-LESS (empty locs passed) and stamped at the op's pc by EnterNative's
 * re-raise - the same loc_at(pc) caret the interpreted op passes in; a
 * callback's own throw already carries its loc. A PLAIN exception (a
 * callback's UndefinedVariableEx - no clone()) rides g_vm_jit_eptr. */
extern "C" int jit_map_filter(int_type fn_slot, int_type cont_slot,
                              int_type dst, int_type is_map) noexcept
{
    ML_JIT_OP_RAN(MapFilterV);
    EvalContext *ctx = g_current_ctx;
    try {
        ctx->frame->at(dst).put(
            vm_map_filter(ctx, ctx->frame->at(fn_slot).get(),
                          ctx->frame->at(cont_slot).get(),
                          is_map != 0, Loc(), Loc()));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    } catch (...) {
        g_vm_jit_eptr = std::current_exception();
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native AppendV `append(a, x)` / `push(a, x)` -
 * the interpreter's D1 fast op. Forms arg0's LValue* from `kind` (0 local / 1
 * global / 2 capture) + `arg0_slot` (an undefined global -> null target, handled
 * by the fallback's NotLValueEx), then runs arr_append_fast (the shared
 * NEVER-THROWING append core - flat/general, hash upkeep) inline. Any decline
 * (const/readonly/non-array/slice/flat-mismatch/null) falls back to the FULL
 * vm_call_builtin_lv_rest (builtin_append), which CAN throw - all
 * RuntimeExceptions now (NotLValueEx / TypeErrorEx / CannotChangeConstEx, the
 * last just made a RuntimeException) already carrying the arg caret -> caught
 * into g_vm_jit_exc + re-raised, byte-identical to the interpreted AppendV.
 * `dst_slot` = the result dst (-1 = a discarded `append(a,x);` statement).
 * `bc` = &chunk.builtin_calls[idx] (the pool entry, baked). NOT op_fully_native.
 */
extern "C" int jit_append(int_type kind, int_type arg0_slot, int_type val_slot,
                          int_type dst_slot, const void *bcv) noexcept
{
    ML_JIT_OP_RAN(AppendV);
    const Chunk::BuiltinCall *bc =
        static_cast<const Chunk::BuiltinCall *>(bcv);
    EvalContext *ctx = g_current_ctx;
    LValue *target;
    switch (kind) {
    case 0:  target = &ctx->frame->at(arg0_slot); break;
    case 1:  target = ctx->gfuncs->defined[arg0_slot]
                          ? &ctx->gfuncs->slots[arg0_slot] : nullptr; break;
    default: target = &(*ctx->captures)[arg0_slot]; break;
    }
    const EvalValue &elem = ctx->frame->at(val_slot).get();
    if (target && arr_append_fast(target, elem, false)) {
        if (dst_slot >= 0)
            ctx->frame->at(dst_slot).put(target->get());
        return 0;
    }
    try {
        EvalValue res = vm_call_builtin_lv_rest(*ctx, *bc, target, val_slot);
        if (dst_slot >= 0)
            ctx->frame->at(dst_slot).put(std::move(res));
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native CallBuiltinLV - a mutating (lvalue-ABI)
 * builtin call `pop(a)`/`insert(a,i,v)`/`erase(a,i)`/`sort(a[,cmp])`/`reverse(a)`
 * /`intptr(a)`, the interpreter's exact CallBuiltinLV. Forms arg0's LValue* from
 * `kind` (0 local / 1 global / 2 capture) + arg0_slot (an undefined global ->
 * null target -> the builtin's NotLValueEx). `rest_base` >= 0 -> a REST-NATIVE
 * op (its value args are the register run [rest_base, +n_rest); via
 * vm_call_builtin_lv_rest); `rest_base` == -1 -> a NO-value-arg op (pop/intptr/
 * sort-no-cmp -> func_lv with an empty rest). Every reachable throw is a
 * RuntimeException now (NotLValueEx/TypeErrorEx/OutOfBoundsEx/CannotChangeConstEx
 * /InvalidArgument/InvalidNumberOfArgs/InvalidValue) -> caught into g_vm_jit_exc
 * (loc from the builtin_calls pool) + re-raised. NOT op_fully_native.
 */
extern "C" int jit_call_builtin_lv(int_type kind, int_type arg0_slot,
                                   int_type dst_slot, int_type rest_base,
                                   const void *bcv) noexcept
{
    ML_JIT_OP_RAN(CallBuiltinLV);
    const Chunk::BuiltinCall *bc =
        static_cast<const Chunk::BuiltinCall *>(bcv);
    EvalContext *ctx = g_current_ctx;
    LValue *target;
    switch (kind) {
    case 0:  target = &ctx->frame->at(arg0_slot); break;
    case 1:  target = ctx->gfuncs->defined[arg0_slot]
                          ? &ctx->gfuncs->slots[arg0_slot] : nullptr; break;
    default: target = &(*ctx->captures)[arg0_slot]; break;
    }
    try {
        EvalValue res;
        if (rest_base >= 0) {
            res = vm_call_builtin_lv_rest(*ctx, *bc, target, rest_base);
        } else {
            ArgLocs al;
            al.start = bc->start;
            al.end = bc->end;
            al.args = bc->args.data();
            al.nargs = bc->args.size();
            al.arr_hint = bc->arr_hint;
            res = bc->builtin.func_lv(ctx, &al, target, nullptr, 0);
        }
        ctx->frame->at(dst_slot).put(std::move(res));
    } catch (RuntimeException &e) {
        if (!e.loc_start) { e.loc_start = bc->start; e.loc_end = bc->end; }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native CallBuiltinLVElem - a mutating lvalue
 * builtin whose arg0 is a SUBSCRIPT target `append(a[i], x)`/`pop(a[i])`. Forms
 * the base's LValue* (by kind), then the ELEMENT's LValue* via the runtime
 * Type::subscript (the SAME COW path Subscript::do_eval uses given the identical
 * base LValue*); a non-lvalue element (flat scalar / read-only / missing key ->
 * a throw) gives a null target -> NotLValueEx, like the tree-walker. The run at
 * `run_base` holds the index (run[0]) then the value args (run[1..]). The
 * interpreter's exact CallBuiltinLVElem; every throw is a RuntimeException ->
 * g_vm_jit_exc (arg0's caret if loc-less) + re-raise. NOT op_fully_native.
 */
extern "C" int jit_call_builtin_lv_elem(int_type kind, int_type base_slot,
                                        int_type dst_slot, int_type run_base,
                                        const void *bcv) noexcept
{
    ML_JIT_OP_RAN(CallBuiltinLVElem);
    const Chunk::BuiltinCall *bc =
        static_cast<const Chunk::BuiltinCall *>(bcv);
    EvalContext *ctx = g_current_ctx;
    LValue *base;
    switch (kind) {
    case 0:  base = &ctx->frame->at(base_slot); break;
    case 1:  base = ctx->gfuncs->defined[base_slot]
                        ? &ctx->gfuncs->slots[base_slot] : nullptr; break;
    default: base = &(*ctx->captures)[base_slot]; break;
    }
    const int_type n_rest = static_cast<int_type>(bc->args.size()) - 1;
    try {
        EvalValue holder;   /* keeps the subscript result alive */
        LValue *elem = nullptr;
        if (base) {
            const EvalValue &idx = ctx->frame->at(run_base).get();
            holder = base->get().get_type()->subscript(
                EvalValue(base), idx, /*for_write=*/false);
            if (holder.is<LValue *>())
                elem = holder.get<LValue *>();
        }
        EvalValue restbuf[8];   /* n_rest small (append 1, pop 0) */
        for (int_type i = 0; i < n_rest; i++)
            restbuf[i] = ctx->frame->at(run_base + 1 + i).get();
        ArgLocs al;
        al.start = bc->start;
        al.end = bc->end;
        al.args = bc->args.data();
        al.nargs = bc->args.size();
        al.arr_hint = bc->arr_hint;
        ctx->frame->at(dst_slot).put(
            bc->builtin.func_lv(ctx, &al, elem, n_rest ? restbuf : nullptr,
                                static_cast<size_t>(n_rest)));
    } catch (RuntimeException &e) {
        if (!e.loc_start) {                /* the subscript's caret = arg0. */
            e.loc_start = bc->args[0].start;
            e.loc_end = bc->args[0].end;
        }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

/*
 * model-flip (nativize-ops): the native CallBuiltinLVMember - a mutating lvalue
 * builtin whose arg0 is a struct-MEMBER target `append(s.f, x)`. Forms the base's
 * LValue* (by kind), the boxed FIELD LValue* via vm_member_lvalue (the SAME check
 * MemberExpr does; a POD/readonly/missing field throws), then func_lv. The run at
 * `run_base` holds ONLY the value args (NO index, unlike LVElem). The
 * interpreter's exact CallBuiltinLVMember; every throw is a RuntimeException ->
 * g_vm_jit_exc (arg0's caret if loc-less) + re-raise. NOT op_fully_native.
 */
extern "C" int jit_call_builtin_lv_member(int_type kind, int_type base_slot,
                                          int_type dst_slot, int_type run_base,
                                          const void *bcv) noexcept
{
    ML_JIT_OP_RAN(CallBuiltinLVMember);
    const Chunk::BuiltinCall *bc =
        static_cast<const Chunk::BuiltinCall *>(bcv);
    EvalContext *ctx = g_current_ctx;
    LValue *base;
    switch (kind) {
    case 0:  base = &ctx->frame->at(base_slot); break;
    case 1:  base = ctx->gfuncs->defined[base_slot]
                        ? &ctx->gfuncs->slots[base_slot] : nullptr; break;
    default: base = &(*ctx->captures)[base_slot]; break;
    }
    const int_type n_rest = static_cast<int_type>(bc->args.size()) - 1;
    try {
        LValue *field = nullptr;
        if (base)
            field = vm_member_lvalue(base, bc->member,
                                     bc->args[0].start, bc->args[0].end,
                                     bc->args[0].start, bc->args[0].end);
        EvalValue restbuf[8];   /* append/push 1 value arg */
        for (int_type i = 0; i < n_rest; i++)
            restbuf[i] = ctx->frame->at(run_base + i).get();
        ArgLocs al;
        al.start = bc->start;
        al.end = bc->end;
        al.args = bc->args.data();
        al.nargs = bc->args.size();
        al.arr_hint = bc->arr_hint;
        ctx->frame->at(dst_slot).put(
            bc->builtin.func_lv(ctx, &al, field, n_rest ? restbuf : nullptr,
                                static_cast<size_t>(n_rest)));
    } catch (RuntimeException &e) {
        if (!e.loc_start) {
            e.loc_start = bc->args[0].start;
            e.loc_end = bc->args[0].end;
        }
        g_vm_jit_exc.reset(e.clone());
        return 1;
    }
    return 0;
}

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
    VmCallRec &cur = act.back_rec();
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

/* #60 (b): the bare dispatch loop, split out of vm_run_chunk so a builtin
 * CALLBACK loop (VmInvoker / vm_try_invoke) that already owns the activation +
 * boundary window + g_current_ctx re-enters it PER ELEMENT with ZERO entry
 * setup (no EntryGuard construct/destruct, no vm_enter_invocation_fast, no
 * CtxGuard). vm_run_chunk = the entry prologue + this. `act` is the caller's
 * activation (the top boundary record is this invocation's frame). */
static void vm_dispatch(const Chunk &chunk0, EvalContext &ctx,
                        VmActivation &act, size_t start_pc = 0);

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
        EvalContext *gctx;                 /* #60: saved g_current_ctx */
        ~Restore() {
            g_current_ctx = gctx;
            a.pop_window();
            c.captures = caps;
        }
    } restore{act, c, c.captures, g_current_ctx};
    /* #60: announce the callback ctx here (vm_dispatch runs without a CtxGuard,
     * so this is the store the entry prologue would otherwise do; jit_ret reads
     * g_current_ctx). */
    g_current_ctx = &c;

    /* Bind from the pre-evaluated values (do_func_bind_params' value
     * overloads' exact semantics: non-const, none for omitted trailing opt
     * params, i/f coercion; a bind throw pops WITHOUT capturing). */
    if (d->fast_bind) {
        for (size_t i = 0; i < n; i++)
            w->at(static_cast<int_type>(i)).rebind(argv[i]);
        for (size_t i = n; i < nparams; i++)
            w->at(static_cast<int_type>(i)).rebind(EvalValue());
    } else {
        for (size_t i = 0; i < nparams; i++) {
            const FuncDescriptor::ParamDesc &p = d->params[i];
            EvalValue val = i < n ? argv[i] : EvalValue();
            if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
                val = vm_coerce_decl_num(val, p.decl_type == DeclType::f);
            w->at(static_cast<int_type>(i)).rebind(std::move(val));
        }
    }

    c.captures = &obj.capture_slots;
    c.flow->type = FlowState::none;

    /* #60 (b): re-enter the dispatch loop DIRECTLY - the window + captures +
     * g_current_ctx are already set up above, so no vm_run_chunk entry setup. */
    try {
        vm_dispatch(*cck, c, act);
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
#ifdef TESTS
unsigned long g_jit_invoke_direct = 0;   /* lever 2 execution proof */
#endif

/* Lever 2's COLD exit path: the invoker's direct-entered fragment exited
 * at an interior pc. Mirrors EnterNative's post-exit for a BOUNDARY frame
 * (standalone rather than sharing jit_sync_postexit - the sync version's
 * pending-conveyance/site-stamping halves are sync_stop semantics that do
 * not apply here): a conveyed raise dispatches via vm_raise (same-frame
 * handler, else the walk stops at the boundary record and sets
 * g_vm_exc_pending - which invoke()'s existing check converts); a plain
 * eptr rethrows (propagates out of invoke like the dispatch path's own
 * throw); a bail or a dispatched handler continues INTERPRETED at the
 * resolved pc - the one dispatch re-entry the old path paid ALWAYS. */
static ML_NOINLINE void
vm_invoke_postexit(const Chunk &cck, EvalContext &ctx, VmActivation &act,
                   size_t r)
{
    const Chunk *c2 = &cck;
    size_t p2 = r;
    bool cont = true;
    if (g_vm_jit_raise) {
        const int kind = g_vm_jit_raise;
        g_vm_jit_raise = 0;
        cont = vm_raise(c2, p2, act, ctx,
            kind == JR_OOB
              ? std::unique_ptr<RuntimeException>(new OutOfBoundsEx())
              : kind == JR_DIV0
              ? std::unique_ptr<RuntimeException>(new DivisionByZeroEx())
              : std::unique_ptr<RuntimeException>(
                    new InvalidValueEx("negative shift count")));
    } else if (g_vm_jit_exc) {
        cont = vm_raise(c2, p2, act, ctx, std::move(g_vm_jit_exc));
    } else if (g_vm_jit_eptr) {
        std::exception_ptr p = std::move(g_vm_jit_eptr);
        g_vm_jit_eptr = nullptr;
        std::rethrow_exception(p);
    }
    if (cont)
        vm_dispatch(*c2, ctx, act, p2);
    /* !cont: the walk stopped at the boundary record - g_vm_exc_pending
     * is set for invoke()'s existing conversion. */
}

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
    /* Lever 2 (plans/native-gap-roadmap.md): a body that STARTS native
     * (the whole-body-native common case) is entered DIRECTLY per element
     * - jit_enter, no vm_dispatch entry/exit, no EnterNative op dispatch.
     * Computed once per LOOP. */
    entry_ = cck_->sync_entry_off >= 0
        ? static_cast<const char *>(cck_->native.base) + cck_->sync_entry_off
        : nullptr;
    fast_bind_ = desc_->fast_bind;
    nparams_ = desc_->params.size();
    min_args_ = static_cast<size_t>(desc_->min_args);
    const int_type total =
        desc_->frame_size + static_cast<int_type>(cck_->n_temps);
    w_ = act_->push_window(total, cck_, /*boundary=*/true);
    saved_caps_ = c_->captures;
    c_->captures = &obj.capture_slots;
    /* #60: own g_current_ctx for the whole loop - each invoke() re-enters
     * vm_dispatch directly (no per-element CtxGuard); jit_ret reads it. */
    saved_gctx_ = g_current_ctx;
    g_current_ctx = c_;
    ready_ = true;
}

VmInvoker::~VmInvoker()
{
    if (!ready_)
        return;
    g_current_ctx = saved_gctx_;
    c_->captures = saved_caps_;
    act_->pop_window();
}

EvalValue VmInvoker::invoke(const EvalValue *argv, size_t n)
{
    const FuncDescriptor *d = desc_;
    const size_t nparams = nparams_;
    if (n > nparams || n < min_args_)
        throw InvalidNumberOfArgsEx();

    if (fast_bind_) {
        for (size_t i = 0; i < n; i++)
            w_->at(static_cast<int_type>(i)).rebind(argv[i]);
        for (size_t i = n; i < nparams; i++)
            w_->at(static_cast<int_type>(i)).rebind(EvalValue());
    } else {
        for (size_t i = 0; i < nparams; i++) {
            const FuncDescriptor::ParamDesc &p = d->params[i];
            EvalValue val = i < n ? argv[i] : EvalValue();
            if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
                val = vm_coerce_decl_num(val, p.decl_type == DeclType::f);
            w_->at(static_cast<int_type>(i)).rebind(std::move(val));
        }
    }

    c_->flow->type = FlowState::none;

    /* Lever 2: DIRECT fragment entry per element - the whole vm_dispatch
     * entry/exit + the EnterNative op dispatch removed from the hot loop.
     * A BOUNDARY sentinel (native ReturnV set flow / native Halt left it
     * none) falls straight through to the flow read below; a non-sentinel
     * exit takes the cold vm_invoke_postexit (raise dispatch or the ONE
     * interpreted continuation the dispatch path always paid). #60 (b)'s
     * dispatch re-entry remains the fallback for a body that does not
     * start native. */
    try {
        if (entry_) {
#ifdef TESTS
            g_jit_invoke_direct++;
#endif
            const size_t r = jit_enter(entry_, w_->slots);
            /* an IN-VM sentinel is impossible here (this frame is a
             * BOUNDARY record; jit_ret/jit_halt return the boundary form)
             * - a surfaced one would mean ignored resume globals */
            ML_CHECK(r != JIT_RET_SENTINEL);
            if (r != JIT_RET_BOUNDARY)
                vm_invoke_postexit(*cck_, *c_, *act_, r);
        } else {
            vm_dispatch(*cck_, *c_, *act_);
        }
    } catch (Exception &e) {
        vm_capture_desc_frame(e, d);
        throw;
    }

    if (g_vm_exc_pending) {
        vm_capture_desc_frame(*g_vm_exc_pending, d);
        std::unique_ptr<RuntimeException> ex = std::move(g_vm_exc_pending);
        ex->rethrow();
    }

    /* Per-call frame death for REFERENCES (see the class comment).
     * Profile #2: only the chunk's audited ref_slots - this scan runs per
     * CALLBACK ELEMENT (sort's comparator, map/filter/make_dict), where
     * the old whole-window walk was a measured cost. Runs BEFORE the result
     * extraction below - it touches only the window SLOTS, never flow->value
     * (a separate FlowState member), so the order is immaterial and this lets
     * the result be MOVE-CONSTRUCTED into the return (#60 Tier 1) instead of
     * move-ASSIGNED into a default-constructed local. */
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

    if (c_->flow->type == FlowState::ret) {
        c_->flow->type = FlowState::none;
        return std::move(c_->flow->value);
    }
    return EvalValue();
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
    if (a && !a->no_recs() && a->back_rec().run_chunk == chunk)
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
    if (g_vm_act->no_recs()
            || g_vm_act->back_rec().run_chunk != chunk) {
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
static ML_NOINLINE Frame *
vm_frame_setup(VmActivation &act, EvalContext &ctx, const Chunk *ret_chunk,
               size_t ret_pc, FuncObject &fo, const Chunk *cck,
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
    VmCallRec &rec = act.back_rec();
    rec.ret_chunk = ret_chunk;
    rec.ret_pc = ret_pc + 1;
    rec.dst = dst;
    rec.desc = d;
    rec.caller_captures = ctx.captures;
    rec.cache_key = std::move(ckey);

    if (d->fast_bind) {
        for (size_t i = 0; i < nargs; i++)
            w->at(static_cast<int_type>(i)).rebind(argrun[i].get());
        for (size_t i = nargs; i < nparams; i++)
            w->at(static_cast<int_type>(i)).rebind(EvalValue());
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
                w->at(static_cast<int_type>(i)).rebind(std::move(val));
            }
        } catch (...) {
            act.pop_window();
            throw;
        }
    }

    ctx.captures = &fo.capture_slots;
    return w;
}

/*
 * LEVER 1 STEP 2 (plans/native-gap-roadmap.md) - the COMMON-SHAPE call
 * push. The callgrind split on 10_recursion_deep put ~500 Ir of protocol
 * on EVERY call: vm_frame_setup 232 (of which ~30 was just the prologue/
 * epilogue - the unique_ptr<PureCacheKey> PARAMETER drags in exception
 * scaffolding and a fat spill frame even when it is always null),
 * vm_frame_leave ~174, jit_ret ~64, and the vm_enter_call wrapper ~36 (a
 * second NOINLINE layer whose only job was two assignments). This twin is
 * the same push with the cold machinery hoisted by its GATE instead of
 * re-decided per call: the caller checks `d->fast_bind` (no coerced
 * params -> the bind is a plain copy loop that CANNOT throw, so no
 * try/cleanup), and there is NO cache-key parameter at all (a plain CallV
 * / a value call never has one; a CachedCallV with a live miss-key takes
 * the general path). push_window itself measured lean already (its
 * segment/record-reuse branches are predicted) - it is shared, not
 * duplicated. rec.cache_key is left untouched: a reused record's was
 * reset by pop_window, a fresh one default-constructs null (checked).
 * ALWAYS_INLINE: inlined into the two NOINLINE users below/in the sync
 * core, so the call depth stays ONE out-of-line call per VM call op.
 */
static ML_ALWAYS_INLINE Frame *
vm_frame_setup_lean(VmActivation &act, EvalContext &ctx,
                    const Chunk *ret_chunk, size_t ret_pc, FuncObject &fo,
                    const Chunk *cck, int_type argbase, size_t nargs,
                    int_type dst)
{
    const FuncDescriptor *d = fo.func;
    const size_t nparams = d->params.size();
    if (nargs > nparams || nargs < static_cast<size_t>(d->min_args))
        throw InvalidNumberOfArgsEx();

    LValue *argrun = nargs ? &ctx.frame->at(argbase) : nullptr;
    const int_type total =
        d->frame_size + static_cast<int_type>(cck->n_temps);
    Frame *w = act.push_window(total, cck, /*boundary=*/false);
    VmCallRec &rec = act.back_rec();
    rec.ret_chunk = ret_chunk;
    rec.ret_pc = ret_pc + 1;
    rec.dst = dst;
    rec.desc = d;
    rec.caller_captures = ctx.captures;
    ML_CHECK(!rec.cache_key);            /* pop reset it / fresh is null */

    for (size_t i = 0; i < nargs; i++)
        w->at(static_cast<int_type>(i)).rebind(argrun[i].get());
    for (size_t i = nargs; i < nparams; i++)
        w->at(static_cast<int_type>(i)).rebind(EvalValue());

    ctx.captures = &fo.capture_slots;
    return w;
}

/* The lean twin of vm_enter_call (the common shape: fast_bind + no cache
 * key). ONE out-of-line call from the dispatch loop (the loop-body text
 * rule), with the setup inlined - the old shape paid a second nested
 * NOINLINE call plus the null unique_ptr's ABI. */
static ML_NOINLINE void
vm_enter_call_lean(VmActivation &act, EvalContext &ctx, const Chunk *&chunk,
                   size_t &pc, FuncObject &fo, const Chunk *cck,
                   int_type argbase, size_t nargs, int_type dst)
{
    vm_frame_setup_lean(act, ctx, chunk, pc, fo, cck, argbase, nargs, dst);
    chunk = cck;
    pc = 0;
}

/*
 * The interpreter's in-VM CALL: the frame-setup core plus the chunk/pc SWITCH
 * to the callee (the loop then dispatches the callee's ops). The native-call
 * path (plans/native-aot.md #55) will instead `call` the callee's fragment
 * after vm_frame_setup, so the switch stays here.
 */
static ML_NOINLINE void
vm_enter_call(VmActivation &act, EvalContext &ctx, const Chunk *&chunk,
              size_t &pc, FuncObject &fo, const Chunk *cck,
              int_type argbase, size_t nargs, int_type dst,
              std::unique_ptr<PureCacheKey> ckey)
{
    vm_frame_setup(act, ctx, chunk, pc, fo, cck, argbase, nargs, dst,
                   std::move(ckey));
    chunk = cck;
    pc = 0;
}

/*
 * The RETURN-side leave CORE (the twin of vm_frame_setup; #55). Pop the TOP
 * in-VM frame with `res` as its return value: store a cached call's SCALAR
 * result in the caller's cache (pure_cache_call's exact rule), pop the window,
 * write the parent's dst slot. The PARENT's resume point goes into
 * g_vm_resume_chunk/pc (NOT a C++ ref to the loop's chunk/pc locals) so this
 * core is reusable by the native ReturnV (#55), whose fragment has no handle
 * on those locals: the native ReturnV will read a slot into `res`, call this,
 * and ret a SENTINEL that makes EnterNative apply the same globals.
 */
/* The CACHED-call tail (cold): the unique_ptr key handling lives OUT of
 * the hot leave below - a unique_ptr local drags exception scaffolding
 * and a fat spill frame into every return otherwise (the same disease
 * the lean setup cured on the push side; lever 1). */
static ML_NOINLINE void
vm_frame_leave_cached(VmActivation &act, EvalContext &ctx, EvalValue &&res,
                      int_type dst)
{
    std::unique_ptr<PureCacheKey> ckey = std::move(act.back_rec().cache_key);
    act.pop_window();
    if (res.get_type()->t < Type::t_str)
        ctx.frame->ensure_pure_cache().emplace(std::move(*ckey), res);
    if (dst >= 0)
        ctx.frame->at(dst).put(std::move(res));
}

static ML_NOINLINE void
vm_frame_leave(VmActivation &act, EvalContext &ctx, EvalValue res)
{
    VmCallRec &dead = act.back_rec();
    ctx.captures = dead.caller_captures;
    g_vm_resume_chunk = dead.ret_chunk;
    g_vm_resume_pc = dead.ret_pc;
    const int_type dst = dead.dst;
    if (dead.cache_key) {                        /* cold: a CachedCallV miss */
        vm_frame_leave_cached(act, ctx, std::move(res), dst);
        return;
    }
    act.pop_window();
    if (dst >= 0)                /* -1 = a DISCARDED call statement's dst
                                  * (the peephole's dead-dst rule) */
        ctx.frame->at(dst).put(std::move(res));
}

/*
 * #55 native calls: the native ReturnV. The fragment flushed its register
 * cache and calls this with the result value's frame slot; g_current_ctx (the
 * running vm_run_chunk's ctx) + g_vm_act (its activation) locate the frame.
 * IN-VM frame: read the result from the CALLEE window (still current), pop it
 * (vm_frame_leave writes the parent's dst + sets the resume globals), and
 * return the resume SENTINEL - EnterNative switches to the parent. BOUNDARY
 * frame: set flow (the do_func_call / callback contract, like the interpreted
 * ReturnV boundary path) and return the boundary sentinel - EnterNative stops
 * the invocation; the C++ caller (vm_try_invoke / VmInvoker) pops the window.
 * noexcept: a fully-native leaf body is throw-free, so nothing here throws.
 */
extern "C" size_t jit_ret(int_type res_slot) noexcept
{
    g_jit_native_returns++;
    EvalContext &ctx = *g_current_ctx;
    VmActivation &act = *g_vm_act;
    ML_CHECK(res_slot >= 0
             && res_slot < static_cast<int_type>(ctx.frame->size));
    /* the callee window dies right after - MOVE the result out (lever 1) */
    EvalValue res = ctx.frame->at(res_slot).steal_value();
    if (act.back_rec().boundary) {
        ctx.flow->value = std::move(res);
        ctx.flow->type = FlowState::ret;
        return JIT_RET_BOUNDARY;
    }
    vm_frame_leave(act, ctx, std::move(res));
    return JIT_RET_SENTINEL;
}

/*
 * model-flip (nativize-ops): the native Halt - a fall-through body's implicit
 * `return none`. The interpreted analog (VM_CASE(Halt)) with the result value
 * hard-wired to none (no result slot). IN-VM frame: vm_frame_leave writes the
 * parent's dst = none + sets the resume globals -> JIT_RET_SENTINEL (EnterNative
 * switches to the parent). BOUNDARY frame (main / a callback callee): just
 * return the boundary sentinel WITHOUT touching flow - the interpreted Halt
 * boundary path is a bare `return` (a fall-through body left flow == none, so
 * the caller reads none), NOT the ReturnV boundary path (which sets flow=ret).
 * noexcept: nothing here throws.
 */
extern "C" size_t jit_halt() noexcept
{
    ML_JIT_OP_RAN(Halt);
    EvalContext &ctx = *g_current_ctx;
    VmActivation &act = *g_vm_act;
    if (act.back_rec().boundary)
        return JIT_RET_BOUNDARY;             /* flow stays none -> caller none */
    vm_frame_leave(act, ctx, EvalValue());   /* return none to the parent */
    return JIT_RET_SENTINEL;
}

/* #55 STEP 2.1: native CallVs set up process-wide (coverage - see jit.h). */
unsigned long g_jit_native_calls = 0;

/* model-flip M3: native-container island calls set up process-wide (coverage -
 * proves a container's jit_exec_block path actually ran; a `jit:` test asserts
 * it > 0, per the "prove the code ran" rule). */
unsigned long g_jit_container_calls = 0;

/* #55 STEP 2.1: member offsets the native-call emitter bakes. Measured via a
 * probe object (no offsetof-on-non-standard-layout warning); both types are
 * default-constructible and cheap (a Chunk's NativeCode dtor is a no-op when
 * base is null, which it is here). */
/* De-helperize 6b (roadmap step 6): the ctx-indirect address chain the
 * LoadCaptureV / StoreCaptureV / StoreGlobalV inlines walk at runtime -
 * &g_current_ctx (the file-static the helpers already use), the
 * EvalContext::captures / ::gfuncs member offsets, and GlobalFuncTable's
 * slots / defined vector offsets (their _M_start lives at +0, the layout
 * fact the flat-array reads already rely on). Probed from real objects so
 * they cannot drift. */
EvalContext **jit_addr_current_ctx()
{
    return &g_current_ctx;
}
ptrdiff_t jit_off_ctx_captures()
{
    EvalContext c(nullptr, true);
    return reinterpret_cast<const char *>(&c.captures)
         - reinterpret_cast<const char *>(&c);
}
ptrdiff_t jit_off_ctx_gfuncs()
{
    EvalContext c(nullptr, true);
    return reinterpret_cast<const char *>(&c.gfuncs)
         - reinterpret_cast<const char *>(&c);
}
ptrdiff_t jit_off_gft_slots()
{
    GlobalFuncTable g;
    return reinterpret_cast<const char *>(&g.slots)
         - reinterpret_cast<const char *>(&g);
}
ptrdiff_t jit_off_gft_defined()
{
    GlobalFuncTable g;
    return reinterpret_cast<const char *>(&g.defined)
         - reinterpret_cast<const char *>(&g);
}

/* CachedCallV's cache probe over an explicit (argbase, n, dst) triple - the
 * shared core of the interpreted probe (vm_cache_probe below) and the lean
 * sync helper. Hit -> dst written, true. Miss -> `pending` carries the key
 * (it rides the callee record and is stored by vm_frame_leave). */
static bool
vm_cache_probe_vals(EvalContext &ctx, const FuncDescriptor *d,
                    int_type argbase, size_t n, int_type dst,
                    std::unique_ptr<PureCacheKey> &pending)
{
    std::vector<EvalValue> vals;
    vals.reserve(n);
    const LValue *ap0 = n ? &ctx.frame->at(argbase) : nullptr;
    for (size_t i = 0; i < n; i++)
        vals.push_back(ap0[i].get());
    PureCacheKey key{ d, std::move(vals) };
    PureCache &cache = ctx.frame->ensure_pure_cache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        ctx.frame->at(dst).put(EvalValue(it->second));
        return true;
    }
    pending = std::make_unique<PureCacheKey>(std::move(key));
    return false;
}

/*
 * M5 LEAN SYNC ENTER (inc 3; plans/model-flip.md): a fragment's call op runs
 * the callee to completion INSIDE the helper and the CALLER CONTINUES
 * NATIVELY. Inc 1 used vm_try_invoke's BOUNDARY machinery (arg value copy,
 * boundary window, flow round-trip) - measured heavier than the interpreted
 * vm_enter_call it displaces (76_funcval 1.16x, 11_closure 1.23x). This
 * reuses the interpreted call's OWN lean protocol: vm_frame_setup pushes a
 * NON-boundary record (record REUSE, fast_bind straight from the caller's
 * contiguous slot run - no arg copy) whose ret_chunk is the SENTINEL STOP
 * CHUNK below, so the callee's ordinary ReturnV/Halt pop (vm_frame_leave)
 * writes the caller's dst - and performs a CachedCallV's cache store for
 * free - then "resumes after the call op", which IS the sentinel's
 * ExitBlock: vm_dispatch returns to the helper. The record's sync_stop flag
 * makes vm_unwind_walk stop there (capture + pop + g_vm_exc_pending), so an
 * uncaught RuntimeException also returns here; a PLAIN (non-Runtime)
 * exception C++-propagates out of vm_dispatch and rides g_vm_jit_eptr
 * (records above die with the dying invocation, as interpreted).
 *
 * The C stack grows one helper+dispatch level per NESTED sync call
 * (recursion through native call sites), so a depth cap bails the deep tail
 * to the interpreted call op, whose flat in-VM record stack is unbounded.
 * Returns 0 = done (dst written); 1 = BAIL pre-side-effect (undefined /
 * non-callable callee, chunk-less callee, depth cap, an arity/bind/
 * StackOverflow throw - all idempotent, the interpreter re-runs the op and
 * re-throws byte-identically); 2 = the callee threw: a RuntimeException
 * rides g_vm_jit_exc (its backtrace's innermost frame gets the BAKED
 * call-site loc the lazy in-VM capture would have used), anything else
 * rides g_vm_jit_eptr.
 */
static int g_jit_sync_depth = 0;
/* M5a: 200 = the C-stack-safe cap; jit_native_stack_init raises it to
 * ~500k when the dedicated native stack is armed (jit.cpp). A VARIABLE:
 * the emitted guards bake it at chunk-compile time (the init runs before
 * any emission), the helper wrappers read it at runtime, and a test can
 * pin it low to exercise the interpreted-fallback paths. */
static int g_jit_sync_cap = 200;

/* The SENTINEL STOP CHUNK: one ExitBlock at the resume pc. A sync frame's
 * rec.ret_pc = 1 (vm_frame_setup's ret_pc+1 with ret_pc = 0), so every
 * return shape - interpreted ReturnV/Halt (vm_leave_call switches to the
 * resume globals) AND the native ReturnV (jit_ret's sentinel makes
 * EnterNative apply the same globals) - lands on code[1] = ExitBlock, which
 * returns from vm_dispatch to the helper. code[0] exists only so the
 * walk-side ret_pc-1 view stays in range (never executed; loc_at finds
 * nothing on either pc - the capture is deliberately loc-less). */
static const Chunk &vm_sync_stop_chunk()
{
    static const Chunk ck = [] {
        Chunk c;
        Instr ex{};
        ex.op = OpCode::ExitBlock;
        Operand o;
        o.is_lit = true;
        o.lit = 0;                       /* the (unused) block-resume pc */
        ex.set_a(o);
        c.code.push_back(ex);
        c.code.push_back(ex);
        return c;
    }();
    return ck;
}

/*
 * Lever 1 step 5 (plans/native-gap-roadmap.md) - the post-exit of a
 * DIRECT-ENTERED sync callee fragment, shared by jit_call_sync_core's
 * direct branch and the fragment-INLINE call path (which emits the push +
 * the `call` + the sentinel test in machine code and calls this only on a
 * non-sentinel exit). `r` = the exit pc (JIT_RET_BOUNDARY impossible - a
 * sync record is non-boundary). The callee record is still TOP here (a
 * bail does not pop), so the callee desc/chunk are read from it. Faithful
 * EnterNative post-exit: a conveyed raise dispatches via vm_raise (caret
 * from the callee's loc table; same-frame handler first, else the walk
 * stops at the sync_stop record), a plain bail / dispatched handler
 * continues INTERPRETED at the resolved pc; the walk's pending exception
 * converts to g_vm_jit_exc with the baked call-site loc stamped (the lazy
 * in-VM capture's loc_at would have produced it - the sentinel stop chunk
 * is loc-less). Depth is the CALLER's business (already decremented).
 */
extern "C" int jit_sync_postexit(size_t r, int_type site_packed) noexcept
{
    EvalContext &ctx = *g_current_ctx;
    VmActivation &act = *g_vm_act;
    const FuncDescriptor *d = act.back_rec().desc;
    const Chunk *cck = act.back_rec().run_chunk;
    try {
        const Chunk *c2 = cck;
        size_t p2 = r;
        bool cont = true;
        if (g_vm_jit_raise) {
            const int kind = g_vm_jit_raise;
            g_vm_jit_raise = 0;
            cont = vm_raise(c2, p2, act, ctx,
                kind == JR_OOB
                  ? std::unique_ptr<RuntimeException>(new OutOfBoundsEx())
                  : kind == JR_DIV0
                  ? std::unique_ptr<RuntimeException>(
                        new DivisionByZeroEx())
                  : std::unique_ptr<RuntimeException>(
                        new InvalidValueEx("negative shift count")));
        } else if (g_vm_jit_exc) {
            cont = vm_raise(c2, p2, act, ctx, std::move(g_vm_jit_exc));
        } else if (g_vm_jit_eptr) {
            /* a nested sync callee's plain exception: convey upward as-is
             * (the records above die with the invocation, exactly the
             * dispatch path's rethrow contract). */
            return 2;
        }
        if (cont)
            vm_dispatch(*c2, ctx, act, p2);
        /* !cont: the walk reached the sync record - popped, pending set;
         * fall through to the conveyance below. */
    } catch (Exception &e) {
        /* A PLAIN exception from the interpreted continuation (a
         * RuntimeException is walked inside vm_dispatch, never
         * C++-escapes it): capture the callee frame + stamp the baked
         * site, exactly what the walk + the pending path produce for a
         * RuntimeException. */
        vm_capture_desc_frame(e, d);
        if (!e.backtrace.empty() && e.backtrace.back().desc == d
                && !e.backtrace.back().call_site.line)
            e.backtrace.back().call_site =
                Loc(static_cast<int>(site_packed >> 32),
                    static_cast<int>(site_packed & 0xffffffff));
        if (auto *re = dynamic_cast<RuntimeException *>(&e))
            g_vm_jit_exc.reset(re->clone());
        else
            g_vm_jit_eptr = std::current_exception();
        return 2;
    } catch (...) {
        g_vm_jit_eptr = std::current_exception();
        return 2;
    }
    if (g_vm_exc_pending) {
        Exception &e = *g_vm_exc_pending;
        if (!e.backtrace.empty() && e.backtrace.back().desc == d
                && !e.backtrace.back().call_site.line)
            e.backtrace.back().call_site =
                Loc(static_cast<int>(site_packed >> 32),
                    static_cast<int>(site_packed & 0xffffffff));
        g_vm_jit_exc = std::move(g_vm_exc_pending);
        return 2;
    }
    return 0;                              /* dst written by vm_frame_leave */
}

/* M5c - the CachedCallV fast path's probe. The map lookup is inherently
 * C++ (PureCacheKey hash/eq over arg values); the emitted site calls this
 * lean helper: a HIT writes the dst slot and returns 1 (the fragment
 * continues past the call); a MISS constructs the key ONCE and parks the
 * released pointer in g_jit_pending_key for the INLINE push to store into
 * rec.cache_key (3 emitted movs). Any decline between the probe and that
 * store jumps to the full jit_call_sync_cached tier, whose core CONSUMES
 * the pending key instead of re-probing - no leak, no double probe. A
 * disabled/unavailable cache is a plain miss with no pending key. */
static PureCacheKey *g_jit_pending_key = nullptr;

extern "C" int jit_cached_probe(const void *descv, int_type argbase,
                                int_type nargs, int_type dst) noexcept
{
    EvalContext &ctx = *g_current_ctx;
    const FuncDescriptor *d =
        static_cast<const FuncDescriptor *>(descv);
    if (!ctx.frame || !g_pure_cache_enabled)
        return 0;
    std::unique_ptr<PureCacheKey> key;
    if (vm_cache_probe_vals(ctx, d, argbase,
                            static_cast<size_t>(nargs), dst, key))
        return 1;                          /* hit - dst written */
    g_jit_pending_key = key.release();
    return 0;
}

void *jit_addr_pending_key()
{
    return &g_jit_pending_key;
}

/* M5b: the jit_sync_push_* HELPERS are gone - the push is emitted INLINE
 * at the call site (emit_sync_push_native, jit.cpp; offsets from
 * jit_fill_push_layout below). Declines go straight to the full
 * jit_call_sync* tier. */

/* the emitted depth guard's address (g_jit_sync_depth is TU-local) */
void *jit_addr_sync_depth()
{
    return &g_jit_sync_depth;
}

int jit_sync_depth_cap()
{
    return g_jit_sync_cap;
}

void jit_set_sync_depth_cap(int cap)
{
    g_jit_sync_cap = cap;
}

static int
jit_call_sync_core(FuncObject &fo, int_type argbase, int_type nargs,
                   int_type dst, int_type site_packed, bool cached) noexcept
{
    EvalContext &ctx = *g_current_ctx;
    VmActivation &act = *g_vm_act;
    const FuncDescriptor *d = fo.func;
    if (!d->vm_chunk_tried || !d->vm_chunk)
        return 1;                          /* AOT net -> interpreted */
    const Chunk *cck = static_cast<const Chunk *>(d->vm_chunk);

    std::unique_ptr<PureCacheKey> key;
    if (cached && g_jit_pending_key) {
        /* M5c: the emitted probe already ran (miss) and parked the key -
         * consume it instead of re-probing (the inline push declined
         * between the probe and the record store). */
        key.reset(g_jit_pending_key);
        g_jit_pending_key = nullptr;
    } else if (cached && ctx.frame && g_pure_cache_enabled
            && vm_cache_probe_vals(ctx, d, argbase,
                                   static_cast<size_t>(nargs), dst, key))
        return 0;                          /* cache hit - dst written */

    /* The push. An arity/bind-coerce/StackOverflow throw is PRE-side-effect
     * and idempotent (setup pops its half-built frame) - bail and let the
     * interpreted op re-run and re-throw byte-identically. */
    Frame *w;
    try {
        if (d->fast_bind && !key)        /* lever 1: the lean push */
            w = vm_frame_setup_lean(act, ctx, &vm_sync_stop_chunk(),
                                    /*ret_pc=*/0, fo, cck, argbase,
                                    static_cast<size_t>(nargs), dst);
        else
            w = vm_frame_setup(act, ctx, &vm_sync_stop_chunk(), /*ret_pc=*/0,
                               fo, cck, argbase, static_cast<size_t>(nargs),
                               dst, std::move(key));
    } catch (...) {
        return 1;
    }

    act.back_rec().sync_stop = 1;

    g_jit_sync_depth++;
    /* DIRECT FRAGMENT ENTRY: when the callee body STARTS with an
     * EnterNative (the common shape - a whole-native body is a bare
     * `enter.nat`), hoist that first op out of vm_dispatch: enter the
     * fragment (its head sets its own tag registers) and hand any
     * non-sentinel exit to the SHARED jit_sync_postexit (which the
     * fragment-INLINE call path also uses - lever 1 step 5). A fragment
     * cannot C++-throw (machine-code frames are non-unwindable; every
     * helper it calls conveys), so the entry sits OUTSIDE the try. */
    if (cck->sync_entry_off >= 0) {
        const size_t r =
            jit_enter_deep(static_cast<const char *>(cck->native.base)
                               + cck->sync_entry_off,
                           w->slots);
        g_jit_sync_depth--;
        if (r == JIT_RET_SENTINEL)     /* native ReturnV: frame popped,
                                        * dst written - done */
            return 0;
        return jit_sync_postexit(r, site_packed);
    }
    try {
        vm_dispatch(*cck, ctx, act);
        g_jit_sync_depth--;
    } catch (Exception &e) {
        /* A PLAIN exception (a RuntimeException is walked inside
         * vm_dispatch, never C++-escapes it): capture the callee frame +
         * stamp the baked site, exactly what the walk + the pending path
         * below produce for a RuntimeException. */
        g_jit_sync_depth--;
        vm_capture_desc_frame(e, d);
        if (!e.backtrace.empty() && e.backtrace.back().desc == d
                && !e.backtrace.back().call_site.line)
            e.backtrace.back().call_site =
                Loc(static_cast<int>(site_packed >> 32),
                    static_cast<int>(site_packed & 0xffffffff));
        if (auto *re = dynamic_cast<RuntimeException *>(&e))
            g_vm_jit_exc.reset(re->clone());
        else
            g_vm_jit_eptr = std::current_exception();
        return 2;
    } catch (...) {
        g_jit_sync_depth--;
        g_vm_jit_eptr = std::current_exception();
        return 2;
    }

    if (g_vm_exc_pending) {
        /* The walk stopped at our sync_stop record: the callee frame was
         * captured LOC-LESS (the sentinel has no locs); stamp the baked
         * call-site loc the lazy in-VM capture (loc_at(ret_chunk,
         * ret_pc-1)) would have used. EnterNative re-raises at the call
         * op's pc, so the caller-side handler dispatch / inline-frame flush
         * runs exactly as an interpreted cross-frame throw. */
        Exception &e = *g_vm_exc_pending;
        if (!e.backtrace.empty() && e.backtrace.back().desc == d
                && !e.backtrace.back().call_site.line)
            e.backtrace.back().call_site =
                Loc(static_cast<int>(site_packed >> 32),
                    static_cast<int>(site_packed & 0xffffffff));
        g_vm_jit_exc = std::move(g_vm_exc_pending);
        return 2;
    }
    return 0;                              /* dst written by vm_frame_leave */
}

extern "C" int jit_call_sync(int_type callee_slot, int_type argbase,
                             int_type nargs, int_type dst,
                             int_type site_packed) noexcept
{
    ML_JIT_OP_RAN(CallV);
    EvalContext *ctx = g_current_ctx;
    if (g_jit_sync_depth >= g_jit_sync_cap)
        return 1;                          /* deep tail -> interpreted */
    if (!ctx->gfuncs->defined[callee_slot])
        return 1;                          /* undefined -> interpreted throw */
    const EvalValue &cv = ctx->gfuncs->slots[callee_slot].get();
    if (!cv.is<intrusive_ptr<FuncObject>>())
        return 1;                          /* not callable -> interpreted */
    return jit_call_sync_core(*cv.get_ref<intrusive_ptr<FuncObject>>().get(),
                              argbase, nargs, dst, site_packed,
                              /*cached=*/false);
}

extern "C" int jit_call_sync_cached(int_type callee_slot, int_type argbase,
                                    int_type nargs, int_type dst,
                                    int_type site_packed) noexcept
{
    ML_JIT_OP_RAN(CachedCallV);
    EvalContext *ctx = g_current_ctx;
    if (g_jit_sync_depth >= g_jit_sync_cap)
        return 1;
    if (!ctx->gfuncs->defined[callee_slot])
        return 1;
    const EvalValue &cv = ctx->gfuncs->slots[callee_slot].get();
    if (!cv.is<intrusive_ptr<FuncObject>>())
        return 1;
    return jit_call_sync_core(*cv.get_ref<intrusive_ptr<FuncObject>>().get(),
                              argbase, nargs, dst, site_packed,
                              /*cached=*/true);
}

extern "C" int jit_call_sync_value(int_type callee_temp, int_type argbase,
                                   int_type nargs, int_type dst,
                                   int_type site_packed) noexcept
{
    ML_JIT_OP_RAN(CallValueV);
    EvalContext *ctx = g_current_ctx;
    if (g_jit_sync_depth >= g_jit_sync_cap)
        return 1;
    /* the callee VALUE was evaluated into a temp slot (CallValueV's
     * target2); a dyn-laundered non-func bails to the interpreted op's
     * NotCallableEx (its caret from the loc side table). */
    const EvalValue &cv = ctx->frame->at(callee_temp).get();
    if (!cv.is<intrusive_ptr<FuncObject>>())
        return 1;
    return jit_call_sync_core(*cv.get_ref<intrusive_ptr<FuncObject>>().get(),
                              argbase, nargs, dst, site_packed,
                              /*cached=*/false);
}

/*
 * model-flip (nativize-ops): the native CallValueGenericV - the generic
 * indirect call of a DYN callee, the interpreter's exact dispatch over the
 * serializable CallSite pool (the op has been AST-free since F1 step 2;
 * only its emit case was missing). `cs` = &chunk.call_sites[i] and `mkeys`
 * = chunk.member_keys.data() are baked pool-buffer addresses; `dst_callee`
 * packs the dst slot (lo32, sign-preserved) with the callee temp (hi32);
 * `site_packed` = the call-site loc (line << 32 | col) for a FuncObject
 * callee's backtrace frame.
 *
 * Dispatch by the RUNTIME callee (CheckCallableV already threw for a
 * non-callable BEFORE the args evaluated):
 *  - FuncObject: fill run[0] with arg0's RValue (an UndefinedId throws
 *    here, stamped with the args caret - the interpreted order), then run
 *    the callee via the LEAN SYNC core: the caller fragment stays native
 *    across the call. A depth-cap / chunkless bail (1) is SOUND - all
 *    pre-call work is idempotent, the interpreted op re-runs identically.
 *  - Builtin: dispatch_builtin_values by Kind; a func_lv (mutating)
 *    builtin re-derives arg0's true LValue* from the descriptor (slot /
 *    elem via Type::subscript / member via vm_member_lvalue_ref - the
 *    same idempotent re-derive the interpreted op does). An elem/member
 *    arg0 whose GLOBAL base is undefined BAILS (the interpreted re-run
 *    throws the exact UndefinedVariableEx with its table caret).
 *  - struct descriptor: construct_struct_v (runtime arity + per-field
 *    coercion with the pooled carets).
 * A loc-less throw is stamped with the ARGS caret from the pool (the
 * interpreted catch's exact rule) - self-loc'd, no emit-side stamp; a
 * plain Exception (an UndefinedId's RValue) rides g_vm_jit_eptr. Returns
 * 0 done (dst written) / 1 bail / 2 threw. NOT op_fully_native (bails).
 */
extern "C" int jit_call_value_generic(int_type dst_callee, int_type argbase,
                                      int_type nargs, const void *csv,
                                      const void *mkeysv,
                                      int_type site_packed) noexcept
{
    ML_JIT_OP_RAN(CallValueGenericV);
    const Chunk::CallSite &cs = *static_cast<const Chunk::CallSite *>(csv);
    const Chunk::MemberKey *mkeys =
        static_cast<const Chunk::MemberKey *>(mkeysv);
    EvalContext &ctx = *g_current_ctx;
    const int_type callee_slot = static_cast<int_type>(
        static_cast<uint64_t>(dst_callee) >> 32);
    const int_type dst = static_cast<int_type>(
        static_cast<int32_t>(dst_callee & 0xffffffff));
    const size_t n = static_cast<size_t>(nargs);

    ArgLocs al;
    al.start = cs.start;
    al.end = cs.end;
    al.args = cs.args.data();
    al.nargs = cs.args.size();
    al.arr_hint = cs.arr_hint;

    const EvalValue &callee = ctx.frame->at(callee_slot).get();

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

    if (callee.is<intrusive_ptr<FuncObject>>()) {
        if (g_jit_sync_depth >= g_jit_sync_cap)
            return 1;                       /* deep tail -> interpreted */
        if (n) {
            try {
                ctx.frame->at(argbase).put(RValue(a0_value()));
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = al.start;
                    e.loc_end = al.end;
                }
                if (auto *re = dynamic_cast<RuntimeException *>(&e))
                    g_vm_jit_exc.reset(re->clone());
                else
                    g_vm_jit_eptr = std::current_exception();
                return 2;
            } catch (...) {
                g_vm_jit_eptr = std::current_exception();
                return 2;
            }
        }
        return jit_call_sync_core(
            *callee.get_ref<intrusive_ptr<FuncObject>>().get(), argbase, nargs,
            dst, site_packed, /*cached=*/false);
    }

    bool bail = false;
    EvalValue holder;                       /* keeps an elem arg0 alive */
    auto a0_base = [&]() -> LValue * {
        if (cs.a0_kind == 1) {
            if (!ctx.gfuncs->defined[cs.a0_slot]) {
                bail = true;                /* undefined global base: the
                                             * interpreted re-run throws
                                             * with its table caret */
                return nullptr;
            }
            return &ctx.gfuncs->slots[cs.a0_slot];
        }
        if (cs.a0_kind == 2)
            return &(*ctx.captures)[cs.a0_slot];
        return &ctx.frame->at(cs.a0_slot);
    };
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
            LValue *base = a0_base();
            if (!base)
                return nullptr;
            const EvalValue &key = ctx.frame->at(cs.a0_operand).get();
            holder = base->get().get_type()->subscript(
                EvalValue(base), key, /*for_write=*/false);
            return holder.is<LValue *>() ? holder.get<LValue *>() : nullptr;
        }
        case Chunk::CallSite::A0::member: {
            LValue *base = a0_base();
            if (!base)
                return nullptr;
            const Chunk::MemberKey &mk = mkeys[cs.a0_operand];
            return vm_member_lvalue_ref(base->get(), mk.memId, mk.memUid,
                                        /*for_write=*/false,
                                        mk.mstart, mk.mend);
        }
        default:
            return nullptr;
        }
    };

    try {
        EvalValue stackbuf[8];
        std::vector<EvalValue> heapbuf;
        EvalValue *buf = stackbuf;
        if (n > 8) {
            heapbuf.resize(n);
            buf = heapbuf.data();
        }
        if (n)
            buf[0] = a0_value();
        for (size_t i = 1; i < n; i++)
            buf[i] = ctx.frame->at(argbase + static_cast<int_type>(i)).get();

        EvalValue res;
        if (callee.is<Builtin>()) {
            const Builtin &b = callee.get<Builtin>();
            if (b.kind == Builtin::Kind::lvalue) {
                LValue *target = n ? a0_lvalue() : nullptr;
                if (bail)
                    return 1;
                EvalValue rest[8];
                std::vector<EvalValue> resth;
                EvalValue *rp = rest;
                const size_t nr = n ? n - 1 : 0;
                if (nr > 8) {
                    resth.resize(nr);
                    rp = resth.data();
                }
                for (size_t i = 0; i < nr; i++)
                    rp[i] = RValue(buf[i + 1]);
                res = b.func_lv(&ctx, &al, target, nr ? rp : nullptr, nr);
            } else {
                res = dispatch_builtin_values(&ctx, b, &al, buf, n);
            }
        } else if (callee.is<StructTypeDef *>()) {
            res = construct_struct_v(callee.get<StructTypeDef *>(), &al,
                                     buf, n);
        } else {
            throw NotCallableEx();          /* net: CheckCallableV ran */
        }
        ctx.frame->at(dst).put(std::move(res));
    } catch (Exception &e) {
        if (!e.loc_start) {
            e.loc_start = al.start;
            e.loc_end = al.end;
        }
        if (auto *re = dynamic_cast<RuntimeException *>(&e))
            g_vm_jit_exc.reset(re->clone());
        else
            g_vm_jit_eptr = std::current_exception();
        return 2;
    } catch (...) {
        g_vm_jit_eptr = std::current_exception();
        return 2;
    }
    return 0;
}

/* Step 7a (the INLINE exception ops): the activation-side layout the
 * PushHandler/PopHandler/SetPend inlines walk - &g_vm_act, the handlers /
 * records / rec_n member offsets, the record stride and its pend offset.
 * Probed from real objects so they cannot drift. The vector-internals
 * layout (_M_start +0 / _M_finish +8 / _M_end_of_storage +16) is the same
 * three-pointer fact the flat-array reads already rely on. */
VmActivation **jit_addr_vm_act()
{
    return &g_vm_act;
}
ptrdiff_t jit_off_act_handlers()
{
    VmActivation a;
    return reinterpret_cast<const char *>(&a.handlers)
         - reinterpret_cast<const char *>(&a);
}
ptrdiff_t jit_off_act_records()
{
    VmActivation a;
    return reinterpret_cast<const char *>(&a.records)
         - reinterpret_cast<const char *>(&a);
}
/* M5b: the aggregated push-layout probe (see jit.h). Real objects, real
 * members - the co-located-probe rule. The FuncObject probe needs a live
 * root EvalContext (its ctor walks get_root_ctx); a throwaway root is
 * cheap (a script root loads no builtins into the map). */
void jit_fill_push_layout(JitPushLayout *L)
{
    VmActivation a;
    const char *ab = reinterpret_cast<const char *>(&a);
    L->act_segs = reinterpret_cast<const char *>(&a.segs) - ab;
    L->act_cur_seg = reinterpret_cast<const char *>(&a.cur_seg) - ab;
    L->act_recs_high = reinterpret_cast<const char *>(&a.recs_high) - ab;
    L->act_diters_n = reinterpret_cast<const char *>(&a.diters_n) - ab;
    L->act_dyiters_n = reinterpret_cast<const char *>(&a.dyiters_n) - ab;
    L->act_used = reinterpret_cast<const char *>(&a.used) - ab;
    L->act_cap = reinterpret_cast<const char *>(&a.cap) - ab;
    L->act_top_rec = reinterpret_cast<const char *>(&a.top_rec) - ab;
    L->act_vframe = reinterpret_cast<const char *>(&a.view_frame) - ab;
    L->act_handlers2 = reinterpret_cast<const char *>(&a.handlers) - ab;
    Frame &f = a.view_frame;
    const char *fb = reinterpret_cast<const char *>(&f);
    L->frame_slots = reinterpret_cast<const char *>(&f.slots) - fb;
    L->frame_size = reinterpret_cast<const char *>(&f.size) - fb;
    L->frame_pure_cache =
        reinterpret_cast<const char *>(&f.pure_cache) - fb;
    VmStackSeg sg(1);
    const char *sb = reinterpret_cast<const char *>(&sg);
    L->seg_slots = reinterpret_cast<const char *>(&sg.slots) - sb;
    L->seg_top = reinterpret_cast<const char *>(&sg.top) - sb;
    VmCallRec r;
    const char *rb = reinterpret_cast<const char *>(&r);
    L->rec_window = reinterpret_cast<const char *>(&r.window) - rb;
    L->rec_nslots = reinterpret_cast<const char *>(&r.nslots) - rb;
    L->rec_seg = reinterpret_cast<const char *>(&r.seg) - rb;
    L->rec_seg_top_before =
        reinterpret_cast<const char *>(&r.seg_top_before) - rb;
    L->rec_run_chunk = reinterpret_cast<const char *>(&r.run_chunk) - rb;
    L->rec_ret_chunk = reinterpret_cast<const char *>(&r.ret_chunk) - rb;
    L->rec_ret_pc = reinterpret_cast<const char *>(&r.ret_pc) - rb;
    L->rec_dst = reinterpret_cast<const char *>(&r.dst) - rb;
    L->rec_desc = reinterpret_cast<const char *>(&r.desc) - rb;
    L->rec_caller_caps =
        reinterpret_cast<const char *>(&r.caller_captures) - rb;
    L->rec_handler_base =
        reinterpret_cast<const char *>(&r.handler_base) - rb;
    L->rec_diter_base = reinterpret_cast<const char *>(&r.diter_base) - rb;
    L->rec_dyiter_base =
        reinterpret_cast<const char *>(&r.dyiter_base) - rb;
    L->rec_boundary = reinterpret_cast<const char *>(&r.boundary) - rb;
    L->rec_sync_stop = reinterpret_cast<const char *>(&r.sync_stop) - rb;
    L->rec_cache_key = reinterpret_cast<const char *>(&r.cache_key) - rb;
    L->rec_caller_cache =
        reinterpret_cast<const char *>(&r.caller_cache) - rb;
    static FuncDescriptor fd;         /* static: fo.func may outlive scope */
    const char *db = reinterpret_cast<const char *>(&fd);
    L->desc_params = reinterpret_cast<const char *>(&fd.params) - db;
    L->desc_frame_size =
        reinterpret_cast<const char *>(&fd.frame_size) - db;
    L->desc_fast_bind = reinterpret_cast<const char *>(&fd.fast_bind) - db;
    L->param_desc_size = sizeof(FuncDescriptor::ParamDesc);
    Chunk ck;
    const char *cb = reinterpret_cast<const char *>(&ck);
    L->ck_n_temps = reinterpret_cast<const char *>(&ck.n_temps) - cb;
    L->ck_n_dict_iters =
        reinterpret_cast<const char *>(&ck.n_dict_iters) - cb;
    L->ck_n_dyn_iters =
        reinterpret_cast<const char *>(&ck.n_dyn_iters) - cb;
    L->ck_sync_entry =
        reinterpret_cast<const char *>(&ck.sync_entry_off) - cb;
    {
        static EvalContext probe_root(nullptr, false, false);
        FuncObject fo(&fd, &probe_root);
        const char *ob = reinterpret_cast<const char *>(&fo);
        L->fo_func = reinterpret_cast<const char *>(&fo.func) - ob;
        L->fo_capture_slots =
            reinterpret_cast<const char *>(&fo.capture_slots) - ob;
        L->t_func =
            EvalValue(intrusive_ptr<FuncObject>(
                make_intrusive<FuncObject>(&fd, &probe_root))).get_type();
    }
    L->stop_chunk = &vm_sync_stop_chunk();
    /* the emitted handler_base computation shifts by 2 */
    static_assert(sizeof(VmHandler) == 4, "handler_base >> 2 bake");
    /* the emitted seg/rec addressing */
    static_assert(sizeof(std::unique_ptr<VmStackSeg>) == sizeof(void *),
                  "segs[i] raw-pointer load");
}

ptrdiff_t jit_off_act_rec_n()
{
    VmActivation a;
    return reinterpret_cast<const char *>(&a.rec_n)
         - reinterpret_cast<const char *>(&a);
}
ptrdiff_t jit_sizeof_vm_rec()
{
    return static_cast<ptrdiff_t>(sizeof(VmCallRec));
}
ptrdiff_t jit_off_rec_pend()
{
    VmCallRec r;
    return reinterpret_cast<const char *>(&r.pend)
         - reinterpret_cast<const char *>(&r);
}

/* The COLD grow path of the inline PushHandler: the handlers vector is at
 * capacity - do the real push_back (realloc). Never throws in practice. */
extern "C" void jit_push_handler_grow(int_type catch_pc) noexcept
{
    g_vm_act->handlers.push_back({ static_cast<uint32_t>(catch_pc) });
}

ptrdiff_t jit_off_desc_vm_chunk()
{
    FuncDescriptor fd;
    return reinterpret_cast<const char *>(&fd.vm_chunk)
         - reinterpret_cast<const char *>(&fd);
}
ptrdiff_t jit_off_chunk_native_base()
{
    Chunk ck;
    return reinterpret_cast<const char *>(&ck.native.base)
         - reinterpret_cast<const char *>(&ck);
}
ptrdiff_t jit_off_chunk_native_entry()
{
    Chunk ck;
    return reinterpret_cast<const char *>(&ck.native_entry_off)
         - reinterpret_cast<const char *>(&ck);
}

/*
 * #55 STEP 2.1: push the callee frame for a NATIVE CallV (see jit.h). The gate
 * proved the callee slot is write-once + holds a native_leaf FuncObject, so the
 * resolve can't miss; vm_frame_setup binds the args from the caller window at
 * [argbase, argbase+nargs) and sets the record's ret_chunk/ret_pc (backtrace).
 * Returns the callee window slots (the caller loads rdi from it, then `call`s
 * the callee fragment). Any RuntimeException (StackOverflow at push, or a bind
 * coercion the gate didn't exclude) is caught -> g_vm_jit_exc + null return; the
 * caller exits to callv_pc and EnterNative re-raises with the call-site caret.
 */
extern "C" LValue *jit_call_setup(int_type callee_slot, int_type argbase,
                                  size_t nargs, int_type dst,
                                  const FuncDescriptor *caller_desc,
                                  size_t callv_pc) noexcept
{
    g_jit_native_calls++;
    EvalContext &ctx = *g_current_ctx;
    VmActivation &act = *g_vm_act;
    FuncObject &fo = *ctx.gfuncs->slots[callee_slot].get()
                          .get_ref<intrusive_ptr<FuncObject>>().get();
    try {
        Frame *w = vm_frame_setup(
            act, ctx, static_cast<const Chunk *>(caller_desc->vm_chunk),
            callv_pc, fo, static_cast<const Chunk *>(fo.func->vm_chunk),
            argbase, nargs, dst, nullptr);
        return w->slots;
    } catch (RuntimeException &e) {
        g_vm_jit_exc.reset(e.clone());
        return nullptr;
    }
}

/* The interpreter's ReturnV/Halt: the leave core + the chunk/pc SWITCH back to
 * the parent (from the resume globals vm_frame_leave set). The native ReturnV
 * (#55) will instead ret a SENTINEL and let EnterNative apply the same globals
 * - the reason the parent resume is a global, not a ref. */
static ML_NOINLINE void
vm_leave_call(VmActivation &act, EvalContext &ctx, const Chunk *&chunk,
              size_t &pc, EvalValue res)
{
    vm_frame_leave(act, ctx, std::move(res));
    chunk = g_vm_resume_chunk;
    pc = g_vm_resume_pc;
}

/* CachedCallV's cache probe (vm_cached_call's exact flow): a HIT writes the
 * result copy into dst and returns true; a MISS leaves the {func, args} key
 * in `pending` for vm_enter_call to carry (stored on pop). */
static ML_NOINLINE bool
vm_cache_probe(EvalContext &ctx, const Instr *in, const FuncDescriptor *d,
               std::unique_ptr<PureCacheKey> &pending)
{
    return vm_cache_probe_vals(ctx, d, in->a_lit(),
                               static_cast<size_t>(in->b_lit()), in->target,
                               pending);
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
        VmCallRec &cur = act.back_rec();
        if (vm_dispatch_exc(act, cur, pc)) {
            cur.exc = std::move(ex);           /* like saved_ex */
            return true;
        }
        if (cur.boundary) {
            g_vm_exc_pending = std::move(ex);
            return false;
        }
        if (cur.sync_stop) {
            /* M5 lean sync enter: this frame's C++ owner is a native
             * caller's jit_call_sync* helper, not a parent record - capture
             * the callee frame (site LOC-LESS: the sentinel ret_chunk has no
             * locs; the helper stamps the baked call-site loc), pop it, and
             * convert like a boundary so the helper's vm_dispatch re-entry
             * returns with g_vm_exc_pending set. */
            vm_capture_rec_frame(*ex, cur);
            ctx.captures = cur.caller_captures;
            act.pop_window();
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

    /* #55 native calls: announce THIS invocation's ctx for jit_ret (the native
     * ReturnV helper), restored on every exit incl. a throw. A nested callback
     * (a builtin re-entering with the invoke ctx) saves/restores the outer. */
    struct CtxGuard {
        EvalContext *prev;
        CtxGuard(EvalContext *c) : prev(g_current_ctx) { g_current_ctx = c; }
        ~CtxGuard() { g_current_ctx = prev; }
    } ctx_guard(&ctx);

    /* #60 (b): the entry setup above is per-INVOCATION; the dispatch loop is
     * split into vm_dispatch so a callback loop re-enters it directly (see the
     * forward decl / VmInvoker::invoke). */
    vm_dispatch(chunk0, ctx, act);
}

static void
vm_dispatch(const Chunk &chunk0, EvalContext &ctx, VmActivation &act,
            size_t start_pc)
{
    /* The CURRENT chunk - reseatable LOOP STATE (the native call stack: an
     * in-VM call switches it to the callee's chunk; a pop switches it
     * back). Every op reads the current chunk through it. */
    const Chunk *chunk = &chunk0;

    /*
     * The CURRENT chunk's instruction array, cached as loop state: `chunk`
     * is a reseatable pointer (an in-VM call switches it), so the compiler
     * cannot hoist `chunk->code.data()` across dispatches - the cache
     * removes a double-load from EVERY dispatch (+1.7-2.1% instructions on
     * pure loops, measured). Refreshed at the FOUR places chunk changes:
     * enter/leave call, the unwind dispatch (vm_resume), and entry.
     */
    const Instr *code = chunk->code.data();
    size_t pc = start_pc;   /* model-flip: vm_exec_block enters mid-chunk */
    /* CachedCallV's pending {func,args} key between the cache miss and the
     * frame push. LOOP-scope (like `in`): clang forbids an INDIRECT goto
     * from exiting a scope with a live non-trivial local, and every label
     * lives inside this one, so no dispatch ever exits it. */
    std::unique_ptr<PureCacheKey> pending_key;
    auto cur_rec = [&]() -> VmCallRec & { return act.back_rec(); };
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
            /* A dyn/general scalar `--d`/`d++` - the shared body (also
             * jit_incdec_checked's). Slot kind: 0 local / 1 global
             * (defined-guarded) / 2 capture. */
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
            vm_incdec_scalar_body(*lvp, in->a_lit() != 0, chunk, pc);
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

        VM_CASE(CmpIntV):
            /* dst = (a <cmp> b) as a bool - the VALUE form of a typed int
             * compare (the native counterpart of the boxed CmpV). Body off the
             * dispatch frame (loop-body text rule). Never throws -> loc/node-
             * free. */
            vm_cmp_int_v(*in, &ctx);
            pc++;
            VM_NEXT;

        VM_CASE(CmpFloatV):
            /* Float operands (int/bool promote via read_float_operand); plain
             * C++ compares give IEEE semantics (a NaN yields false for
             * </<=/>/>=/==, true for !=), matching TypeFloat + the tree-walker.
             * Never throws. */
            vm_cmp_float_v(*in, &ctx);
            pc++;
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

        VM_CASE(EnterNative): {
            /* Native-AOT (plans/native-aot.md): run the compiled fragment
             * - a frameless leaf (slots base in rdi, resume pc out). It
             * either completes the whole run or BAILS at some interior
             * pc, whose ORIGINAL op the interpreter then re-executes
             * (incl. any throw, with the exact caret). Fragments never
             * throw and touch nothing but the slot window. jit_enter (a
             * no_sanitize("function") helper) makes the indirect call, so
             * UBSan's -fsanitize=function does NOT read a CFI type
             * signature from before the JIT fragment (which has none -
             * the read faults on the guard page below `base`). */
            pc = jit_enter(static_cast<char *>(chunk->native.base)
                               + in->a_lit(),
                           ctx.frame->slots);
            /* #55: a native ReturnV returned a resume SENTINEL. IN-VM: switch
             * to the parent (the globals vm_frame_leave set). BOUNDARY: stop
             * this invocation (the do_func_call / callback contract) - exactly
             * the interpreted ReturnV's two paths. Checked BEFORE the raise
             * flags (a return set neither). */
            if (pc == JIT_RET_SENTINEL) {
                ML_CHECK(g_vm_resume_chunk != nullptr);
                chunk = g_vm_resume_chunk;
                pc = g_vm_resume_pc;
                code = chunk->code.data();
                VM_NEXT;
            }
            if (pc == JIT_RET_BOUNDARY)
                return;
            /* Approach A: a native fragment that hit a proven exception
             * (OOB / negative shift) set g_vm_jit_raise + returned the op's
             * pc; raise it via vm_raise (the caret is stamped from the loc
             * table at `pc`, byte-identical to the interpreted throw), NOT
             * by re-interpreting the op. A temporary (not a named local)
             * keeps no destructor live at the dispatch goto. */
            if (g_vm_jit_raise) {
                const int kind = g_vm_jit_raise;
                g_vm_jit_raise = 0;
                if (!vm_raise(chunk, pc, act, ctx,
                        kind == JR_OOB
                          ? std::unique_ptr<RuntimeException>(
                                new OutOfBoundsEx())
                          : kind == JR_DIV0
                          ? std::unique_ptr<RuntimeException>(
                                new DivisionByZeroEx())
                          : std::unique_ptr<RuntimeException>(
                                new InvalidValueEx("negative shift count"))))
                    return;                    /* boundary: signal set */
                code = chunk->code.data();     /* dispatched to a handler */
            }
            /* A container-store helper (jit_store_elem_int/float) CAUGHT a
             * LOC-LESS exception; vm_raise stamps its caret from THIS chunk's
             * loc table at the returned (store op) pc - byte-identical to the
             * interpreted store's throw. Mutually exclusive with
             * g_vm_jit_raise per exit. */
            else if (g_vm_jit_exc) {
                if (!vm_raise(chunk, pc, act, ctx, std::move(g_vm_jit_exc)))
                    return;                    /* boundary: signal set */
                code = chunk->code.data();     /* dispatched to a handler */
            }
            /* M5: a PLAIN (non-Runtime) exception from a native sync call
             * (a callee's UndefinedVariableEx - no clone(), so it rode a
             * std::exception_ptr). Rethrow: it propagates out of
             * vm_dispatch exactly like the interpreted in-VM call's own
             * throw (non-catchable, rendered at the top level). */
            else if (g_vm_jit_eptr) {
                std::exception_ptr p = std::move(g_vm_jit_eptr);
                g_vm_jit_eptr = nullptr;
                std::rethrow_exception(p);
            }
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
             * live iterator to begin() - the shared body (also jit_dict_iter_
             * init's). The ML_VM_CHECKs are the hardening net. */
            ML_VM_CHECK(in->target >= 0
                && in->target < static_cast<int_type>(chunk->n_dict_iters));
            vm_dict_iter_init_body(diter(in->target),
                                   ctx.frame->at(in->target2).get());
            pc++;
        }
        VM_NEXT;

        VM_CASE(DictIterNext): {

            /* Test the live iterator: on end jump to end_pc; else bind the key
             * (and value) box-free + advance - the shared body (also
             * jit_dict_iter_next's). A slot of -1 is a `_` placeholder / the
             * keys-only 1-var form (bind nothing). */
            ML_VM_CHECK(in->target2 >= 0
                && in->target2 < static_cast<int_type>(chunk->n_dict_iters));
            if (!vm_dict_iter_next_body(diter(in->target2), *ctx.frame,
                                        in->a_slot(), in->b_slot())) {
                pc = static_cast<size_t>(in->target);
                VM_NEXT;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(ForeachDynInit): {
            /* Dispatch the DYN container once: pin it, record the loop shape
             * (nvars | indexed, the per-var target slots from the
             * unpack_targets pool), and set up an array or dict cursor - the
             * shared body (also jit_foreach_dyn_init's). An unsupported
             * runtime value throws (loc side table). */
            ML_VM_CHECK(in->target >= 0
                && in->target < static_cast<int_type>(chunk->n_dyn_iters));
            vm_foreach_dyn_init_body(dyiter(in->target),
                                     ctx.frame->at(in->target2).get(),
                                     in->a_lit(),
                                     &chunk->unpack_targets[in->b_lit()],
                                     chunk, pc);
            pc++;
        }
        VM_NEXT;

        VM_CASE(ForeachDynNext): {
            /* On exhaustion jump to end_pc; else bind the loop vars from the
             * state + advance - the shared body (also jit_foreach_dyn_next's):
             * `indexed` binds targets[0] = the counter; an ARRAY element binds
             * the single remaining var BOX-FREE (vm_arr_elem) or STRICT-
             * unpacks an array element into the N remaining vars; a DICT
             * binds key [, value [, none...]] (do_iter's count=2 padding). */
            ML_VM_CHECK(in->target2 >= 0
                && in->target2 < static_cast<int_type>(chunk->n_dyn_iters));
            if (!vm_foreach_dyn_next_body(dyiter(in->target2), *ctx.frame,
                                          chunk, pc)) {
                pc = static_cast<size_t>(in->target);
                VM_NEXT;
            }
            pc++;
        }
        VM_NEXT;

        VM_CASE(UnpackElemInt):
        VM_CASE(UnpackElemFloat):
        VM_CASE(UnpackElemValue): {

            /* STRICT foreach-unpack: pairs[i] must be an array of EXACTLY N,
             * its scalars written to the consecutive loop-var slots - the
             * shared body (also jit_unpack_elem's). `i` is the loop counter
             * (in-range), so the outer read never OOB. */
            const int kind = in->op == OpCode::UnpackElemInt ? 0
                           : in->op == OpCode::UnpackElemFloat ? 1 : 2;
            vm_unpack_elem_body(ctx, ctx.frame->at(in->target2).get(),
                                read_int_operand(in->a(), &ctx), in->b_lit(),
                                kind, in->target, nullptr, chunk, pc);
            pc++;
        }
        VM_NEXT;

        VM_CASE(UnpackElemTargets): {
            /* STRICT foreach-unpack with a per-position target list (`_` /
             * non-consecutive slots) - the shared body over the
             * unpack_targets pool entry. */
            vm_unpack_elem_body(ctx, ctx.frame->at(in->target2).get(),
                                read_int_operand(in->a(), &ctx), in->b_lit(),
                                /*kind=*/2, /*dst_base=*/-1,
                                &chunk->unpack_targets[in->target], chunk, pc);
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreElemInt): {

            /* a[i] = v / a[i] OP= v for a flat mutable int/bool array (COW),
             * else the UNIVERSAL vm_subscript_store fallback (const / general
             * / dyn). The store logic lives in vm_store_elem_int_body, SHARED
             * with the approach-A JIT helper jit_store_elem_int - so a native
             * store fragment runs byte-identical C++ (see plans/native-aot.md).
             * The base may be a global/capture array (in->target = the slot
             * kind); the caret comes from the loc side table, looked up LAZILY
             * on the cold throw/fallback path only. */
            LValue &alv =
                *vm_store_base(ctx, in->target, in->target2, *chunk, pc,
                               nullptr);
            vm_store_elem_int_body(alv, read_int_operand(in->a(), &ctx),
                                   read_int_operand(in->b(), &ctx), in->aop,
                                   chunk, pc);
            pc++;
        }
        VM_NEXT;

        VM_CASE(StoreElemFloat): {

            /* Caret from the loc side table, looked up LAZILY on the cold
             * throw/fallback paths only (see StoreElemInt). */
            LValue &alv =
                *vm_store_base(ctx, in->target, in->target2, *chunk, pc,
                               nullptr);
            vm_store_elem_float_body(alv, read_int_operand(in->a(), &ctx),
                                     read_float_operand(in->b(), &ctx),
                                     in->aop, chunk, pc);
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
                vm_chain_lvalue_store_op(ctx, chunk->chain_steps[in->a_dual_lo()],
                                         chunk->member_keys.data(), base, val,
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

            /* Multi-assign `a, b, c = <rvalue>` (F-1): the tree-walker's
             * STRICT destructure / spread, via the shared body (also
             * jit_multi_unpack's). Typed targets coerce per the
             * unpack_coerce pool; a compound applies the base op per
             * target. */
            vm_multi_unpack_body(
                ctx, ctx.frame->at(in->a_slot()).get(),
                chunk->unpack_targets[in->target],
                in->b_is_lit() ? &chunk->unpack_coerce[in->b_lit()] : nullptr,
                in->aop, chunk, pc);
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
             * of vm_run_chunk's frame). It FREEZES + HASHES each key, so an
             * UNHASHABLE key (a function value laundered through `dyn`) throws
             * TypeErrorEx - stamp the literal's caret from the loc side table,
             * matching the tree-walker's Construct::eval stamp. */
            try {
                ctx.frame->at(in->target).put(
                    vm_make_dict(ctx, in->a_lit(), in->b_lit()));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
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
            const int32_t plan = in->b_dual_hi();
            if (plan >= 0) {
                /* the baked plan: raw reads (from each field's src slot -
                 * a computed-run temp or a direct local), direct byte
                 * stores, no coerce calls, never throws */
                vm_struct_ctor_planned(ctx, def, chunk->ctor_plans[plan],
                                       in->target);
            } else {
                try {
                    vm_struct_ctor(ctx, def, in->a_lit(), in->b_dual_lo(),
                                   in->target);   /* unplanned: a = run base */
                } catch (Exception &e) {
                    vm_stamp_loc(*chunk, pc, e);
                    throw;
                }
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
                    vm_call_builtin_lv_rest(
                        ctx, chunk->builtin_calls[in->a_dual_lo()],
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
                        vm_call_builtin_lv_rest(
                            ctx, chunk->builtin_calls[in->a_dual_lo()], target,
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
            FuncObject &fo = *callee.get_ref<intrusive_ptr<FuncObject>>().get();

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
                /* lever 1: the common shape (plain-copy bind, no live
                 * cache miss-key) takes the lean push - no unique_ptr
                 * ABI, one call layer. */
                if (fo.func->fast_bind && !pending_key)
                    vm_enter_call_lean(act, ctx, chunk, pc, fo, cck,
                                       in->a_lit(),
                                       static_cast<size_t>(in->b_lit()),
                                       in->target);
                else
                    vm_enter_call(act, ctx, chunk, pc, fo, cck, in->a_lit(),
                                  static_cast<size_t>(in->b_lit()),
                                  in->target, std::move(pending_key));
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
            FuncObject &fo = *callee.get_ref<intrusive_ptr<FuncObject>>().get();

            if (!fo.func->vm_chunk_tried) {       /* AOT net, as before */
                fo.func->vm_chunk = vm_func_chunk(fo.func);
                fo.func->vm_chunk_tried = true;
            }
            if (const Chunk *cck =
                    static_cast<const Chunk *>(fo.func->vm_chunk)) {
                if (fo.func->fast_bind)          /* lever 1: lean push */
                    vm_enter_call_lean(act, ctx, chunk, pc, fo, cck,
                                       in->a_lit(),
                                       static_cast<size_t>(in->b_lit()),
                                       in->target);
                else
                    vm_enter_call(act, ctx, chunk, pc, fo, cck, in->a_lit(),
                                  static_cast<size_t>(in->b_lit()),
                                  in->target, nullptr);
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
                        *callee.get_ref<intrusive_ptr<FuncObject>>().get(),
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
                /* the frame dies with the invocation - move the result out */
                ctx.flow->value = ctx.frame->at(in->a_slot()).steal_value();
                ctx.flow->type = FlowState::ret;
                return;
            }
            vm_leave_call(act, ctx, chunk, pc,
                          ctx.frame->at(in->a_slot()).steal_value());
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
                vm_num_binop(val, boxed_operand(in->b(), &ctx, sb), in->aop);
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
                vm_num_binop(nv, boxed_operand(in->b(), &ctx, sb), in->aop);
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
                vm_num_binop(val, boxed_operand(in->b(), &ctx, sb), in->aop);
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
             * truthiness. `is_true` is a virtual Type op whose BASE throws for a
             * value with no bool conversion, so stamp the expression's caret
             * from the loc side table (the VM used to report it with none). */
            try {
                EvalValue sa, sb;
                const bool a = boxed_operand(in->a(), &ctx, sa).is_true();
                const bool b = boxed_operand(in->b(), &ctx, sb).is_true();
                ctx.frame->at(in->target).put(
                    EvalValue(in->aop == Op::land ? (a && b) : (a || b)));
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
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
                    vm_num_binop(nv, boxed_operand(in->a(), &ctx, sb), in->aop);
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
                    vm_num_binop(nv, boxed_operand(in->a(), &ctx, sb), in->aop);
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
            /* H1: typed standalone struct-member read `p.x` (th==i) - the
             * shared body (also jit_load_member's): the POD fast path reads
             * the scalar straight from the instance's bytes, anything else
             * falls to member_read_core + write_scalar_slot. */
            if (!vm_load_member_baked(ctx, in, chunk))
                vm_load_member_scalar(ctx, chunk->member_keys[in->a_lit()],
                                      in->target2, in->target,
                                      /*is_int=*/true);
            pc++;
        }
        VM_NEXT;

        VM_CASE(LoadMemberFloat): {
            /* the eval_float twin (th==f) - the shared body */
            if (!vm_load_member_baked(ctx, in, chunk))
                vm_load_member_scalar(ctx, chunk->member_keys[in->a_lit()],
                                      in->target2, in->target,
                                      /*is_int=*/false);
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

        VM_CASE(JumpUnlessTrueV): {
            /* The BOXED truthiness test (`if (dynvalue)`, `while (flag)`, a
             * &&/|| conjunct, the boxed ternary). `is_true` is a virtual Type op
             * whose BASE throws TypeErrorEx - reachable from a script with e.g.
             * a builtin value as the condition - so stamp the CONDITION's caret
             * from the loc side table, matching the tree-walker's
             * Construct::eval stamp (the VM used to report it with NO caret). */
            bool t;
            try {
                t = ctx.frame->at(in->target2).get().is_true();
            } catch (Exception &e) {
                vm_stamp_loc(*chunk, pc, e);
                throw;
            }
            if (!t)
                pc = in->target;
            else
                pc++;
        }
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

        VM_CASE(ExitBlock):
            /* model-flip M2 (plans/model-flip.md): an island's fall-through
             * exit. Hand control back to the native container (vm_exec_block):
             * stash the resume pc (the next block the container continues at)
             * and return from the dispatch loop. ONLY reached via vm_exec_block
             * - the codegen doesn't emit it yet (M3). A terminator (rets),
             * never a fall-through, so no VM_NEXT. */
            g_vm_block_resume = static_cast<size_t>(in->a_lit());
            return;
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

/* =====================================================================
 * model-flip M2 (plans/model-flip.md): the interpreted-ISLAND executor.
 * The endgame flips "bytecode with native islands" into "native with
 * bytecode islands": every function becomes ONE call-able native container
 * whose un-nativizable regions are ISLANDS reached via a `call vm_exec_block`.
 * This is that executor + its self-test; the emitter wiring is M3.
 * ===================================================================== */

/* How an island run by vm_exec_block ended. */
enum class BlockStatus {
    FellThrough,   /* ExitBlock: island done -> container continues (resume pc)*/
    Returned,      /* ReturnV/Halt: the function returns (value in ctx.flow)    */
    Raised,        /* an UNCAUGHT throw: g_vm_exc_pending set (container raises)*/
};

/*
 * Run a single-entry ISLAND of interpreted ops starting at from_pc in the
 * CURRENT frame, then hand control back to the native container. It reuses the
 * FULL vm_dispatch (every handler, nested calls, the per-frame handler stack) -
 * the island is real interpreted execution, not a second interpreter. The one
 * trick: the current frame's `boundary` bit is flipped for the run, so an island
 * ReturnV/Halt or an UNCAUGHT throw HANDS BACK here (set flow / set the signal,
 * then return) instead of popping the frame - the CONTAINER owns the frame's
 * actual return. A fall-through island ends with an ExitBlock (-> FellThrough,
 * *resume_pc = the next block). An exception CAUGHT within the island is handled
 * by the handler stack and the island simply continues (-> FellThrough); only an
 * uncaught one -> Raised. Do NOT hold a VmCallRec& across vm_dispatch (a nested
 * call can grow+realloc the records vector); re-fetch back_rec() to restore.
 */
[[maybe_unused]] static BlockStatus
vm_exec_block(EvalContext &ctx, VmActivation &act, const Chunk &chunk,
              size_t from_pc, size_t *resume_pc)
{
    const unsigned char saved_boundary = act.back_rec().boundary;
    act.back_rec().boundary = 1;         /* ReturnV/Halt/throw -> hand back */
    const size_t saved_resume = g_vm_block_resume;
    g_vm_block_resume = VM_BLOCK_NONE;

    vm_dispatch(chunk, ctx, act, from_pc);

    act.back_rec().boundary = saved_boundary;   /* re-fetch (realloc-safe) */
    const size_t got = g_vm_block_resume;
    g_vm_block_resume = saved_resume;

    if (got != VM_BLOCK_NONE) {
        if (resume_pc)
            *resume_pc = got;
        return BlockStatus::FellThrough;
    }
    if (g_vm_exc_pending)
        return BlockStatus::Raised;
    return BlockStatus::Returned;        /* ReturnV set flow.ret; Halt -> none */
}

/*
 * model-flip M3 (plans/model-flip.md): a native CONTAINER fragment, at an
 * island, `call`s this - baking its OWN FuncDescriptor + the island's start pc.
 * It runs the interpreted island via vm_exec_block in the container's frame
 * (reached through g_current_ctx / g_vm_act, exactly like jit_ret) and returns
 * the fall-through resume pc; on an UNCAUGHT island exception it bridges the
 * pending signal into g_vm_jit_exc and returns the RAISED sentinel, so the
 * caller fragment exits to the island pc and the EnterNative handler re-raises
 * (byte-identical caret + backtrace). extern "C" + noexcept: vm_exec_block
 * catches into the signal, so this never C++-throws out of native code.
 */
extern "C" size_t jit_exec_block(const FuncDescriptor *desc,
                                 size_t from_pc) noexcept
{
    g_jit_container_calls++;
    EvalContext &ctx = *g_current_ctx;
    VmActivation &act = *g_vm_act;
    const Chunk *ck = static_cast<const Chunk *>(desc->vm_chunk);
    size_t resume = 0;
    const BlockStatus st = vm_exec_block(ctx, act, *ck, from_pc, &resume);
    if (st == BlockStatus::FellThrough)
        return resume;                    /* a small pc; the fragment continues*/
    if (st == BlockStatus::Raised) {
        g_vm_jit_exc = std::move(g_vm_exc_pending);   /* EnterNative re-raises */
        return JIT_BLOCK_RAISED;
    }
    /* Returned: an island ReturnV - the M3 container gate excludes it (ReturnV
     * is native/terminal, never inside an island). A loud abort if a future
     * gate ever admits one. */
    ML_CHECK_MSG(false, "island ReturnV - excluded by the M3 container gate");
    return JIT_BLOCK_RAISED;
}

#ifdef TESTS
/* Hand-build tiny islands and drive vm_exec_block over each - the M2 unit
 * test (no emitter wiring yet). */
bool vm_exec_block_selftest()
{
    auto lit = [](int_type v) {
        Operand o; o.is_lit = true; o.lit_kind = Operand::LitKind::i; o.lit = v;
        return o;
    };
    auto sl = [](int s) { Operand o; o.is_lit = false; o.slot = s; return o; };
    auto imm = [&](int slot, int_type v) {
        Instr in; in.op = OpCode::LoadImmInt; in.target = slot; in.set_a(lit(v));
        return in;
    };

    struct Res { BlockStatus st; size_t resume; EvalValue slot0; FlowState flow;};
    auto run = [](std::vector<Instr> ops, int nslots) -> Res {
        Chunk ck;
        ck.code = std::move(ops);
        ck.slot_count = nslots;
        VmActivation act;
        VmActivation *prev = g_vm_act;
        g_vm_act = &act;
        EvalContext ctx(nullptr, /*const_ctx=*/false);
        ctx.frame = act.push_window(nslots, &ck, /*boundary=*/false);
        g_vm_exc_pending.reset();
        ctx.flow->type = FlowState::none;
        size_t resume = VM_BLOCK_NONE;
        const BlockStatus st = vm_exec_block(ctx, act, ck, 0, &resume);
        Res r{ st, resume, ctx.frame->at(0).get(), *ctx.flow };
        if (st == BlockStatus::Raised)
            g_vm_exc_pending.reset();       /* consume the test's exception */
        act.pop_window();
        g_vm_act = prev;
        return r;
    };
    auto is_int = [](const EvalValue &v, int_type n) {
        return v.get_type()->t == Type::t_int && v.get<int_type>() == n;
    };

    bool ok = true;

    /* (1) fall-through: LoadImmInt r0=42; ExitBlock -> resume 2. */
    {
        Instr ex; ex.op = OpCode::ExitBlock; ex.set_a(lit(2));
        const Res r = run({ imm(0, 42), ex }, 1);
        ok = ok && r.st == BlockStatus::FellThrough && r.resume == 2
             && is_int(r.slot0, 42);
    }
    /* (2) internal boxed branch (NOT taken): r0=1; JumpUnlessTrueV r0 -> L3;
     *     r0=99; ExitBlock -> resume 4. r0 true, so it falls to r0=99. */
    {
        Instr j; j.op = OpCode::JumpUnlessTrueV; j.target2 = 0; j.target = 3;
        Instr ex; ex.op = OpCode::ExitBlock; ex.set_a(lit(4));
        const Res r = run({ imm(0, 1), j, imm(0, 99), ex }, 1);
        ok = ok && r.st == BlockStatus::FellThrough && r.resume == 4
             && is_int(r.slot0, 99);
    }
    /* (2b) same branch TAKEN: r0=0 -> jump past r0=99, r0 stays 0. */
    {
        Instr j; j.op = OpCode::JumpUnlessTrueV; j.target2 = 0; j.target = 3;
        Instr ex; ex.op = OpCode::ExitBlock; ex.set_a(lit(4));
        const Res r = run({ imm(0, 0), j, imm(0, 99), ex }, 1);
        ok = ok && r.st == BlockStatus::FellThrough && is_int(r.slot0, 0);
    }
    /* (3) a return: LoadImmInt r0=7; ReturnV r0 -> Returned, flow.value == 7. */
    {
        Instr ret; ret.op = OpCode::ReturnV; ret.set_a(sl(0));
        const Res r = run({ imm(0, 7), ret }, 1);
        ok = ok && r.st == BlockStatus::Returned
             && r.flow.type == FlowState::ret && is_int(r.flow.value, 7);
    }
    /* (4) an uncaught throw: r0=10; r1=0; IntBin r2 = r0 / r1 -> Raised. */
    {
        Instr d; d.op = OpCode::IntBin; d.aop = Op::div; d.target = 2;
        d.set_a(sl(0)); d.set_b(sl(1));
        const Res r = run({ imm(0, 10), imm(1, 0), d }, 3);
        ok = ok && r.st == BlockStatus::Raised;
    }

    return ok;
}
#endif  /* TESTS */
