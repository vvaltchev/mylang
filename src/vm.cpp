/* SPDX-License-Identifier: BSD-2-Clause */

#include "vm.h"
#include "codegen.h"
#include "bytecode.h"
#include "syntax.h"
#include "eval.h"
#include "errors.h"
#include "backtrace.h"   /* flush_inline_frames (Inc 4 backtrace parity) */
#include "bitops.h"

#include <memory>
#include <cmath>
#include <unordered_map>

/* The harness's engine switch (see vm.h). Default: the tree-walker. */
ExecEngine g_exec_engine = ExecEngine::TreeWalk;

/*
 * Evaluate a condition node the same way the tree-walker's eval_cond does: the
 * unboxed int path when inference proved it a non-null int, else the boxed
 * is_true path. Mirrored here (eval_cond is a static inline in eval.cpp) so the
 * VM's JumpIfFalse costs exactly what an `if`/`while` test costs tree-walked -
 * no regression. The differential harness proves the two agree.
 */
static inline bool
vm_eval_cond(const Construct *c, EvalContext *ctx)
{
    if (c->th == TypeHint::i)
        return c->eval_int(ctx) != 0;
    return RValue(c->eval(ctx)).is_true();
}

/* Read a register operand as an int: an immediate, or a frame slot's int (a
 * bool slot reads as 0/1, mirroring Identifier::eval_int). The register machine
 * uses the frame slots directly - no value stack. */
static ML_ALWAYS_INLINE int_type
read_int_operand(const Operand &o, EvalContext *ctx)
{
    if (o.is_lit)
        return o.lit;
    const LValue &lv = ctx->frame->at(o.slot);
    if (lv.is<bool>())
        return lv.getval<bool>() ? 1 : 0;
    /* A th==i operand must hold an int here; anything else is an inference bug
     * or a corrupt/garbage slot (its type ptr won't match int) - caught before
     * the raw getval misreads the union. */
    ML_VM_CHECK(lv.is<int_type>());
    return lv.getval<int_type>();
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
read_float_operand(const Operand &o, EvalContext *ctx)
{
    if (o.is_lit)
        return o.flit;
    const LValue &lv = ctx->frame->at(o.slot);
    if (lv.is<int_type>())
        return static_cast<float_type>(lv.getval<int_type>());
    if (lv.is<bool>())
        return lv.getval<bool>() ? 1.0 : 0.0;
    /* Likewise a th==f operand must hold a float here (int/bool promoted
     * above); a wrong-typed / garbage slot fails before the raw union read. */
    ML_VM_CHECK(lv.is<float_type>());
    return lv.getval<float_type>();
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

/* Construct a POD struct P(x,y) from its field-arg run. ML_NOINLINE + off
 * vm_run_chunk's frame for the recursion-stack reason above; the caller wraps it
 * in the defensive loc-stamp try/catch (the throw propagates out of here). */
static ML_NOINLINE EvalValue
vm_struct_ctor(EvalContext &ctx, StructTypeDef *def, int_type base, int_type nf)
{
    if (nf <= 16) {
        EvalValue vals[16];
        for (int_type i = 0; i < nf; i++)
            vals[i] = ctx.frame->at(base + i).get();
        return EvalValue(construct_struct_from_values(def, vals, nf));
    }
    std::vector<EvalValue> heapbuf(static_cast<size_t>(nf));
    for (int_type i = 0; i < nf; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    return EvalValue(construct_struct_from_values(def, heapbuf.data(), nf));
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
                  int_type nkeys, const EvalValue &val, Op op)
{
    if (nkeys <= 8) {
        EvalValue keybuf[8];
        for (int_type k = 0; k < nkeys; k++)
            keybuf[k] = ctx.frame->at(kbase + k).get();
        vm_subscript_chain_store(base, keybuf, static_cast<size_t>(nkeys),
                                 val, op, Loc(), Loc());
        return;
    }
    std::vector<EvalValue> keyheap(static_cast<size_t>(nkeys));
    for (int_type k = 0; k < nkeys; k++)
        keyheap[k] = ctx.frame->at(kbase + k).get();
    vm_subscript_chain_store(base, keyheap.data(), static_cast<size_t>(nkeys),
                             val, op, Loc(), Loc());
}

/* Build a FLAT array<PodStruct> literal from the N structs' interleaved
 * field-arg run [base, base+N*M). ML_NOINLINE + off vm_run_chunk's frame for the
 * recursion-stack reason above (the fused op's field-value buffer must not
 * inflate the recursive frame). The caller wraps the defensive loc-stamp. */
static ML_NOINLINE EvalValue
vm_make_struct_array_op(EvalContext &ctx, StructTypeDef *def, int_type base,
                        int_type n)
{
    const size_t total = static_cast<size_t>(n) * def->fields.size();
    if (total <= 32) {
        EvalValue stackbuf[32];
        for (size_t k = 0; k < total; k++)
            stackbuf[k] = ctx.frame->at(base + static_cast<int_type>(k)).get();
        return vm_make_struct_array(def, static_cast<size_t>(n), stackbuf);
    }
    std::vector<EvalValue> heapbuf(total);
    for (size_t k = 0; k < total; k++)
        heapbuf[k] = ctx.frame->at(base + static_cast<int_type>(k)).get();
    return vm_make_struct_array(def, static_cast<size_t>(n), heapbuf.data());
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
 * from the register run [base, base+nf) and give them to vm_emplace_struct (a0
 * = the container arg, ctor = the struct construction with vm_struct_ctor_def).
 * ML_NOINLINE keeps this (cold) path out of vm_run_chunk's hot body;
 * a struct with >8 fields heaps. */
static ML_COLD EvalValue
vm_do_emplace(EvalContext &ctx, const DirectBuiltinCallExpr *dc,
              LValue *target, int_type base)
{
    const Construct *arg0 = dc->args->elems[0].get();
    const CallExpr *ctor =
        static_cast<const CallExpr *>(dc->args->elems[1].get());
    const size_t nf = ctor->args->elems.size();
    if (nf <= 8) {
        EvalValue stackbuf[8];
        for (size_t i = 0; i < nf; i++)
            stackbuf[i] = ctx.frame->at(base + i).get();
        return vm_emplace_struct(&ctx, target, arg0, ctor, stackbuf, nf);
    }
    std::vector<EvalValue> heapbuf(nf);
    for (size_t i = 0; i < nf; i++)
        heapbuf[i] = ctx.frame->at(base + i).get();
    return vm_emplace_struct(&ctx, target, arg0, ctor, heapbuf.data(), nf);
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
static std::unordered_map<const FuncDeclStmt *, Chunk> g_func_chunks;

static void vm_precompile_all(const Block *root);   /* AOT: defined below */

/*
 * Execute the optimized program via the bytecode VM. It builds the program's
 * root EvalContext exactly as Block::do_eval does for the root block
 * (ctx == nullptr): the implicit "main" Frame for slotted top-level vars, and
 * the program-wide GlobalFuncTable for top-level functions / escaped globals.
 * Both live for the whole run. run_chunk then drives the chunk; in Phase 0
 * every instruction is a fallback EvalStmt, so this is byte-identical to
 * root->eval(nullptr) - which the differential harness enforces.
 */
void
vm_execute(const Construct *root_c)
{
    const Block *root = static_cast<const Block *>(root_c);

    /* Mark the program as EXECUTING for the duration, so vm_func_chunk's guard
     * knows any chunk it compiles is over the final AST (save/restore, not a
     * bare set, in case of re-entry; restored even on an exception). */
    struct ExecGuard {
        bool prev;
        ExecGuard() : prev(g_vm_executing) { g_vm_executing = true; }
        ~ExecGuard() { g_vm_executing = prev; }
    } exec_guard;

    /* Fresh per run: drop any prior program's function chunks (and their stale
     * FuncDeclStmt::vm_chunk pointers, whose functions are long freed). */
    g_func_chunks.clear();

    const Chunk chunk = codegen_program(root);

    /* AOT: compile every function body upfront (no lazy per-call compile) - the
     * maintainer's no-lazy rule + a `.myv`-serialization prerequisite. After
     * this, do_func_call reads a precomputed vm_chunk and never compiles. */
    vm_precompile_all(root);

    EvalContext ctx(nullptr, /*const_ctx=*/false);

    /* The frame holds the resolved locals plus the register machine's scratch
     * temps [slot_count, slot_count + n_temps); the tree-walker fallback only
     * ever touches slots < slot_count, so the extra temps never collide. */
    std::unique_ptr<Frame> root_frame;
    if (root->slot_count || chunk.n_temps) {
        root_frame = std::make_unique<Frame>();
        root_frame->init(root->slot_count + chunk.n_temps);
        ctx.frame = root_frame.get();
    }

    std::unique_ptr<GlobalFuncTable> gtable;
    if (!root->global_func_names.empty()) {
        gtable = std::make_unique<GlobalFuncTable>();
        gtable->init(root->global_func_names);
        ctx.gfuncs = gtable.get();
    }

    vm_run_chunk(chunk, ctx);

    /* Inc v2: a top-level-uncaught exception propagated to here as the pending
     * signal (main's boundary found no handler and converted it). Convert it
     * back to a C++ throw for the mylang.cpp / -rt top-level handler - the SAME
     * object, so type / message / loc / backtrace are unchanged. */
    if (g_vm_exc_pending) {
        std::unique_ptr<RuntimeException> ex = std::move(g_vm_exc_pending);
        ex->rethrow();
    }
}

/*
 * Compile a block-bodied function's body to a chunk on first use and cache it
 * (see vm.h). Returns null - so the caller tree-walks - for an expression body,
 * or a block whose codegen produced NO register op (a native loop): such a body
 * is pure fallback, and driving it through the VM would only add dispatch over
 * Block::do_eval with no offsetting win.
 */
const Chunk *
vm_func_chunk(const FuncDeclStmt *fdecl)
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

    auto it = g_func_chunks.find(fdecl);
    if (it != g_func_chunks.end())
        return &it->second;

    /* The compile + gate (base-template / scope-free / has-native) is the
     * shared codegen_func_body, so the VM's compiled set is byte-identical to
     * what -vd dumps. AOT (vm_precompile_all) fills g_func_chunks for the whole
     * program upfront, so this lazy miss path is only a safety net for a func
     * the precompile walk didn't reach (there should be none). */
    Chunk ck;
    if (!codegen_func_body(fdecl, ck))
        return nullptr;
    return &g_func_chunks.emplace(fdecl, std::move(ck)).first->second;
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
        fn->vm_chunk = vm_func_chunk(fn);   /* compiles + caches (or null) */
        fn->vm_chunk_tried = true;
    }
}

/* Per-loop LIVE dict-iterator state (DictIterInit/DictIterNext): the
 * intrusive_ptr pins the dict alive for the loop, the iterator persists across
 * iterations. One per native dict foreach in the chunk (Chunk::n_dict_iters),
 * indexed by the codegen-assigned iter_id. Local to vm_run_chunk, so a `return`
 * / exception mid-loop releases it when the frame unwinds - no cleanup op. */
struct DictIterState {
    intrusive_ptr<DictObject> dict;
    DictObject::inner_type::iterator it;
};

/* Per-loop LIVE state for a native single-var `foreach (e in <dyn>)`: the
 * array-vs-dict choice is made ONCE at ForeachDynInit and recorded here (the
 * pinned `container` keeps the array/dict alive for the loop). One per native
 * ForeachDyn (Chunk::n_dyn_iters), indexed by the codegen-assigned iter_id. */
struct DynIterState {
    EvalValue container;   /* pins the array OR dict for the loop */
    bool is_dict = false;
    int nvars = 1;         /* 1 (element/key) or 2 (unpack / key+value) */
    size_type idx = 0, size = 0;               /* array cursor + snapshot */
    DictObject::inner_type::iterator it;       /* dict cursor (iff is_dict) */
};

/* P8 Inc 0: an active `try` region on the VM handler stack. `catch_pc` is where
 * the boundary jumps on a caught exception (the CatchTest chain). */
struct VmHandler {
    uint32_t catch_pc;
};

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

/* P8: pop the innermost active handler and set `pc` to its catch-dispatch;
 * false (pc unchanged) if none → the caller C++-throws (cross-frame). Shared by
 * the native Throw and the boundary. Inc 0/1: a handler is just a catch_pc. */
static bool vm_dispatch_exc(std::vector<VmHandler> &handlers, size_t &pc)
{
    if (handlers.empty())
        return false;
    pc = handlers.back().catch_pc;
    handlers.pop_back();
    return true;
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
    if (const InlineCtx *ic = chunk.inline_ctx_at(pc)) {
        flush_inline_frames(ic, e);
        e.inline_origin_emitted = true;
    }
}

/*
 * Raise an exception FROM a VM op - a `throw`, or a runtime error the op
 * detects itself (div/mod-by-zero, a flat-array bounds fault). Native-dispatch
 * to a same-frame handler (set `pc` to its catch + stash `vm_exc`, NO C++ throw
 * - the caller then `continue`s to re-dispatch at the CatchTest chain); or, no
 * active handler in this frame, C++-throw so it propagates to the caller's
 * boundary (cross-frame, like the tree-walker). Stamps the op's caret from the
 * loc side table when the exception carries none. Cold path only (a real
 * error); the non-error path never calls it. Returns iff it native-dispatched.
 */
static void
vm_raise(const Chunk &chunk, size_t &pc, std::vector<VmHandler> &handlers,
         std::unique_ptr<RuntimeException> &vm_exc,
         std::unique_ptr<RuntimeException> ex)
{
    if (!ex->loc_start) {
        Loc s, en;
        chunk.loc_at(pc, s, en);
        ex->loc_start = s;
        ex->loc_end = en;
    }
    vm_flush_inline(chunk, pc, *ex);       /* frames if raised in inlined */
    if (vm_dispatch_exc(handlers, pc)) {
        vm_exc = std::move(ex);            /* same-frame: native jump */
        return;
    }
    vm_exc = std::move(ex);                /* no handler → C++ propagate */
    vm_exc->rethrow();
}

void
vm_run_chunk(const Chunk &chunk, EvalContext &ctx)
{
    /* Sized once from the chunk; empty (no alloc) for a no-dict-foreach
     * chunk. */
    std::vector<DictIterState> dict_iters(chunk.n_dict_iters);
    std::vector<DynIterState> dyn_iters(chunk.n_dyn_iters);
    /* P8: active `try` regions + the in-flight caught exception. Empty/null when
     * no try is active (the common case), so no cost for exception-free code. */
    std::vector<VmHandler> handlers;
    std::unique_ptr<RuntimeException> vm_exc;
    /* P8 Inc 2b: the pending action the shared `finally` must resume - normal
     * or reraise (set by SetPend, consumed by EndFinally). */
    Pend vm_pend = Pend::normal;

    /* Inc 0 (P8): the exception BOUNDARY (plans/vm-exceptions.md). It routes a
     * RuntimeException thrown by any op (a runtime-library error, a fallback
     * `throw`, or a callee) into the VM handler stack: on a caught exception
     * with an active handler, resume at its catch-dispatch pc. PROVEN hot-path-
     * neutral (zero-cost EH: 0.97-1.00 on the dispatch-bound VM benches), so it
     * wraps the dispatch loop directly - no cold-wrapper needed. Body
     * deliberately NOT reindented (a 1270-line switch; indentation is cosmetic).
     * `pc` lives out here so the catch can set it before `goto vm_resume`. */
    size_t pc = 0;
  vm_resume:
    try {

    for (; ; ) {

        const Instr &in = chunk.code[pc];

        switch (in.op) {

        case OpCode::EvalStmt: {

            EvalValue &&tmp = chunk.node_at(in.node_idx)->eval(&ctx);

            /* An unresolved name surfaces as an UndefinedId sentinel, which
             * Block::do_eval turns into UndefinedVariableEx. */
            if (tmp.is<UndefinedId>())
                throw UndefinedVariableEx(tmp.get<UndefinedId>().id,
                                          chunk.node_at(in.node_idx)->start, chunk.node_at(in.node_idx)->end);

            /* A `return` from a function body (this statement was a ReturnStmt,
             * or a fallback loop/block that returned) stops the chunk; the
             * caller reads ctx->flow->value. break/continue in a loop body are
             * consumed by the following LoopBackEdge, not here; main never sets
             * a flow signal at the top level. */
            if (ctx.flow->type == FlowState::ret)
                return;

            pc++;
            break;
        }

        case OpCode::Jump:
            pc = in.target;
            break;

        case OpCode::JumpIfFalse:
            if (vm_eval_cond(chunk.node_at(in.node_idx), &ctx))
                pc++;
            else
                pc = in.target;
            break;

        case OpCode::LoopBackEdge: {

            /* Mirror While/ForStmt::do_eval's post-body flow handling. */
            FlowState &fs = *ctx.flow;

            switch (fs.type) {

            case FlowState::ret:
                return;             /* a return propagating out of the loop -
                                     * stops the whole chunk (function body) */

            case FlowState::brk:
                fs.type = FlowState::none;
                pc = in.target2;    /* exit loop */
                break;

            case FlowState::cont:
                fs.type = FlowState::none;
                pc = in.target;     /* continue dest */
                break;

            default:                /* none */
                pc = in.target;     /* continue dest */
                break;
            }

            break;
        }

        case OpCode::IncDecCheckedV: {
            /* A dyn/general scalar `--d`/`d++`: throw if not int/float
             * (inc-dec is int/float-ONLY), else apply +-1 in place. Slot kind:
             * 0 local / 1 global (defined-guarded) / 2 capture. */
            LValue *lvp;
            if (in.target2 == 1) {
                if (!ctx.gfuncs->defined[in.target]) {
                    Loc s, en;
                    chunk.loc_at(pc, s, en);
                    throw UndefinedVariableEx(
                        ctx.gfuncs->names[in.target]->val, s, en);
                }
                lvp = &ctx.gfuncs->slots[in.target];
            } else if (in.target2 == 2) {
                lvp = &(*ctx.captures)[in.target];
            } else {
                lvp = &ctx.frame->at(in.target);
            }
            LValue &lv = *lvp;
            EvalValue nv = lv.get();
            if (!nv.is<int_type>() && !nv.is<float_type>()) {
                Loc s, en;
                chunk.loc_at(pc, s, en);
                throw TypeErrorEx("'++'/'--' requires an int or float", s, en);
            }
            num_bin_op(nv, EvalValue(static_cast<int_type>(1)),
                       binop_pmf(in.a.lit ? Op::plus : Op::minus));
            lv.put(std::move(nv));
            pc++;
            break;
        }

        case OpCode::IntBin: {

            const int_type a = read_int_operand(in.a, &ctx);
            const int_type b = read_int_operand(in.b, &ctx);
            int_type r;

            switch (in.aop) {
            case Op::plus:  r = a + b; break;
            case Op::minus: r = a - b; break;
            case Op::times: r = a * b; break;
            case Op::div:
                if (b == 0) {
                    vm_raise(chunk, pc, handlers, vm_exc,
                             std::make_unique<DivisionByZeroEx>());
                    continue;          /* native-dispatched: skip the write */
                }
                r = a / b; break;
            case Op::mod:
                if (b == 0) {
                    vm_raise(chunk, pc, handlers, vm_exc,
                             std::make_unique<DivisionByZeroEx>());
                    continue;
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

            write_int_slot(&ctx, in.target, r);
            pc++;
            break;
        }

        case OpCode::JumpUnlessIntCmp: {

            const int_type a = read_int_operand(in.a, &ctx);
            const int_type b = read_int_operand(in.b, &ctx);
            bool cond;

            switch (in.aop) {
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
                pc = in.target;
            break;
        }

        case OpCode::FloatBin: {

            const float_type a = read_float_operand(in.a, &ctx);
            const float_type b = read_float_operand(in.b, &ctx);
            float_type r;

            switch (in.aop) {
            case Op::plus:  r = a + b; break;
            case Op::minus: r = a - b; break;
            case Op::times: r = a * b; break;
            case Op::div:
                if (b == 0.0) {
                    vm_raise(chunk, pc, handlers, vm_exc,
                             std::make_unique<DivisionByZeroEx>());
                    continue;          /* native-dispatched: skip the write */
                }
                r = a / b; break;
            case Op::mod:
                if (b == 0.0) {
                    vm_raise(chunk, pc, handlers, vm_exc,
                             std::make_unique<DivisionByZeroEx>());
                    continue;
                }
                r = std::fmod(a, b); break;
            default: throw InternalErrorEx();
            }

            write_float_slot(&ctx, in.target, r);
            pc++;
            break;
        }

        case OpCode::JumpUnlessFloatCmp: {

            const float_type a = read_float_operand(in.a, &ctx);
            const float_type b = read_float_operand(in.b, &ctx);
            bool cond;

            switch (in.aop) {
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
                pc = in.target;
            break;
        }

        case OpCode::ForLoopStep: {

            /* i += step (or -=); if (i <aop> bound) loop back, else exit. One
             * dispatch for the whole counter (see bytecode.h). */
            LValue &ilv = ctx.frame->at(in.target2);
            int_type i = ilv.getval<int_type>();
            const int_type step = read_int_operand(in.b, &ctx);

            i = (in.aop == Op::lt || in.aop == Op::le) ? i + step : i - step;
            ilv.getval<int_type>() = i;   /* counter slot always holds int */

            const int_type bound = read_int_operand(in.a, &ctx);
            bool go;
            switch (in.aop) {
            case Op::lt: go = i <  bound; break;
            case Op::le: go = i <= bound; break;
            case Op::ge: go = i >= bound; break;
            default:     go = i >  bound; break;   /* gt */
            }

            if (go)
                pc = in.target;
            else
                pc++;
            break;
        }

        case OpCode::LoadElemInt: {

            /* a[i] into a temp (mirrors Subscript::eval_int for a flat array;
             * a dict / general base falls back to the node). */
            const EvalValue &base = ctx.frame->at(in.target2).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = read_int_operand(in.a, &ctx);
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                    Loc ls, le;
                    chunk.loc_at(pc, ls, le);
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
                write_int_slot(&ctx, in.target, v);
            } else {
                /* base_array is PROVEN at this op, so the base is always an
                 * array here - the old node->eval_int fallback was unreachable
                 * (an invariant net; freeing it dropped the op's node). */
                throw InternalErrorEx();
            }
            pc++;
            break;
        }

        case OpCode::LoadElemFloat: {

            const EvalValue &base = ctx.frame->at(in.target2).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = read_int_operand(in.a, &ctx);
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                    Loc ls, le;
                    chunk.loc_at(pc, ls, le);
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
                write_float_slot(&ctx, in.target, v);
            } else {
                throw InternalErrorEx();   /* unreachable: base_array proven */
            }
            pc++;
            break;
        }

        case OpCode::LoadElemBool: {
            /* bool-foreach loop var: bind a[i] as a real BOOL (not 0/1), so
             * `print(x)` shows true/false. `i` is loop-bounded (< ArrLen); the
             * base is a proven flat array<bool> (elem_is_bool). */
            const SharedArrayObj &arr =
                ctx.frame->at(in.target2).get().get_ref<SharedArrayObj>();
            const int_type idx = read_int_operand(in.a, &ctx);
            const bool b =
                arr.skind() == SharedArrayObj::Storage::bools
                    ? arr.flat_bools()[arr.offset() + idx] != 0
                    : arr.get_view()[idx].get().get<bool>();  /* general fallbk */
            ctx.frame->at(in.target).put(EvalValue(b));
            pc++;
            break;
        }

        case OpCode::LoadElemValue: {

            /* a[i] (an array-valued element of a GENERAL array) into a temp
             * slot, so a 2-D read `a[i][k]` is native (both indices). The base
             * is a PROVEN general array (base_array + a general element type),
             * so the old non-general node->eval fallback was unreachable. */
            const EvalValue &base = ctx.frame->at(in.target2).get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                if (arr.skind() == SharedArrayObj::Storage::general) {
                    int_type idx = read_int_operand(in.a, &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size()) {
                        Loc ls, le;
                        chunk.loc_at(pc, ls, le);
                        throw OutOfBoundsEx(ls, le);
                    }
                    ctx.frame->at(in.target).put(
                        arr.get_vec()[arr.offset() + idx].get());
                    pc++;
                    break;
                }
            }
            throw InternalErrorEx();   /* unreachable: base_array general proven */
        }

        case OpCode::DictIterInit: {

            /* Pin the dict (an intrusive_ptr copy keeps it alive for the loop,
             * matching the tree-walker's lifetime-extended `cval`) and set the
             * live iterator to begin(). The inferencer proved a Dict static
             * type; the ML_VM_CHECKs are the hardening net. */
            ML_VM_CHECK(in.target >= 0
                && static_cast<size_t>(in.target) < dict_iters.size());
            const EvalValue &base = ctx.frame->at(in.target2).get();
            ML_VM_CHECK(base.is<intrusive_ptr<DictObject>>());
            DictIterState &st = dict_iters[in.target];
            st.dict = base.get<intrusive_ptr<DictObject>>();
            st.it = st.dict->get_ref().begin();
            pc++;
            break;
        }

        case OpCode::DictIterNext: {

            /* Test the live iterator: on end jump to end_pc; else bind the key
             * (and value) box-free - a plain EvalValue copy (LoadElemValue),
             * matching the tree-walker's `elems = {p.first, p.second.get()}` -
             * then advance. A slot of -1 is a `_` placeholder / the keys-only
             * 1-var form (bind nothing). */
            ML_VM_CHECK(in.target2 >= 0
                && static_cast<size_t>(in.target2) < dict_iters.size());
            DictIterState &st = dict_iters[in.target2];
            if (st.it == st.dict->get_ref().end()) {
                pc = static_cast<size_t>(in.target);
                break;
            }
            if (in.a.slot >= 0)
                ctx.frame->at(in.a.slot).put(st.it->first);
            if (in.b.slot >= 0)
                ctx.frame->at(in.b.slot).put(st.it->second.get());
            ++st.it;
            pc++;
            break;
        }

        case OpCode::ForeachDynInit: {
            /* Dispatch the DYN container once: pin it, and set up an array or
             * dict cursor. An unsupported runtime value throws (loc side
             * table). */
            ML_VM_CHECK(in.target >= 0
                && static_cast<size_t>(in.target) < dyn_iters.size());
            DynIterState &st = dyn_iters[in.target];
            st.container = ctx.frame->at(in.target2).get();
            st.nvars = static_cast<int>(in.a.lit);   /* 1 or 2 loop vars */
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
                chunk.loc_at(pc, s, en);
                throw TypeErrorEx(
                    "foreach: expected an array or dict", s, en);
            }
            pc++;
            break;
        }

        case OpCode::ForeachDynNext: {
            /* On exhaustion jump to end_pc; else bind the loop var BOX-FREE -
             * the array element (arr_elem_at) or the dict key - and advance. */
            ML_VM_CHECK(in.target2 >= 0
                && static_cast<size_t>(in.target2) < dyn_iters.size());
            DynIterState &st = dyn_iters[in.target2];
            if (!st.is_dict) {
                if (st.idx >= st.size) {
                    pc = static_cast<size_t>(in.target);
                    break;
                }
                if (st.nvars == 1) {
                    if (in.a.slot >= 0)
                        ctx.frame->at(in.a.slot).put(
                            vm_arr_elem(st.container, st.idx));
                } else {
                    /* 2-var over an ARRAY: the element must be an array of
                     * EXACTLY 2, unpacked into the vars - do_iter's STRICT
                     * destructure (same messages/loc via the side table). */
                    const EvalValue elem = vm_arr_elem(st.container, st.idx);
                    if (!elem.is<SharedArrayObj>())
                        vm_throw_unpack_nonarray(chunk, pc, st.nvars);
                    const SharedArrayObj &sub = elem.get_ref<SharedArrayObj>();
                    if (sub.size() != static_cast<size_type>(st.nvars))
                        vm_throw_unpack_len(chunk, pc, sub.size(), st.nvars);
                    if (in.a.slot >= 0)
                        ctx.frame->at(in.a.slot).put(vm_arr_elem(elem, 0));
                    if (in.b.slot >= 0)
                        ctx.frame->at(in.b.slot).put(vm_arr_elem(elem, 1));
                }
                st.idx++;
            } else {
                DictObject &d = *st.container.get<intrusive_ptr<DictObject>>();
                if (st.it == d.get_ref().end()) {
                    pc = static_cast<size_t>(in.target);
                    break;
                }
                if (in.a.slot >= 0)
                    ctx.frame->at(in.a.slot).put(st.it->first);
                if (st.nvars == 2 && in.b.slot >= 0)
                    ctx.frame->at(in.b.slot).put(st.it->second.get());
                ++st.it;
            }
            pc++;
            break;
        }

        case OpCode::UnpackElemInt:
        case OpCode::UnpackElemFloat:
        case OpCode::UnpackElemValue: {

            /* STRICT foreach-unpack: read pairs[i] (a general outer element = a
             * sub-array), check it is an array of EXACTLY N, write its N scalars
             * into the consecutive loop-var slots base..base+N-1 - matching
             * do_iter's strict destructure element for element. `i` is the loop
             * counter (in-range), so the outer read never OOB; the only throws
             * are the two strict errors. */
            const bool is_int = in.op == OpCode::UnpackElemInt;
            const bool is_float = in.op == OpCode::UnpackElemFloat;
            const EvalValue &base_v = ctx.frame->at(in.target2).get();
            ML_VM_CHECK(base_v.is<SharedArrayObj>());
            const SharedArrayObj &outer = base_v.get_ref<SharedArrayObj>();
            const int_type idx = read_int_operand(in.a, &ctx);
            const EvalValue &elem =
                outer.get_vec()[outer.offset() + idx].get();
            const int_type N = in.b.lit;
            if (!elem.is<SharedArrayObj>())
                vm_throw_unpack_nonarray(chunk, pc, N);
            const SharedArrayObj &sub = elem.get_ref<SharedArrayObj>();
            if (sub.size() != static_cast<size_type>(N))
                vm_throw_unpack_len(chunk, pc, sub.size(), N);
            const size_type off = sub.offset();
            const auto sk = sub.skind();
            if (is_int && sk == SharedArrayObj::Storage::ints) {
                for (int_type k = 0; k < N; k++)
                    write_int_slot(&ctx, in.target + k,
                                   sub.flat_ints()[off + k]);
            } else if (is_float && sk == SharedArrayObj::Storage::floats) {
                for (int_type k = 0; k < N; k++)
                    write_float_slot(&ctx, in.target + k,
                                     sub.flat_floats()[off + k]);
            } else {
                /* UnpackElemValue (a general/dyn/str/mixed sub-array), OR a flat
                 * op whose sub-array's storage is NOT the expected kind (a
                 * mixed-numeric `[int, float]` literal - inference types it
                 * array<float> by int|float join but builds GENERAL - or a flat
                 * array of the OTHER scalar kind). Bind each element's ACTUAL
                 * boxed value (vm_arr_elem is skind-dispatched) - byte-identical
                 * to do_iter's bind_loop_var, so an int stays int / a str stays
                 * str. */
                for (int_type k = 0; k < N; k++)
                    ctx.frame->at(in.target + k).put(
                        vm_arr_elem(elem, static_cast<size_type>(k)));
            }
            pc++;
            break;
        }

        case OpCode::StoreElemInt: {

            /* a[i] = v / a[i] OP= v for a flat mutable int array (mirrors the
             * int path of try_flat_subscript_store, COW), and a[i] = <bool>
             * for a flat bool array (P1: plain assign only - no compound
             * for bool; the value operand is the bool's 0/1, written to bvec).
             * Anything else (const / read-only / general / float / dyn, or a
             * COMPOUND on a bool array) takes the UNIVERSAL vm_subscript_store
             * fallback (box the already-computed index/value operands, map the
             * base op back to its Expr14 op) - AST-free and byte-identical to
             * the tree-walker (the same path StoreElemValue uses). The base may
             * be a global/capture array (in.target = the slot kind); the caret
             * comes from the loc side table - looked up LAZILY, only on the cold
             * throw/fallback paths, so the hot store pays no binary search. */
            LValue &alv =
                *vm_store_base(ctx, in.target, in.target2, chunk, pc, nullptr);
            if (alv.is<SharedArrayObj>()) {
                SharedArrayObj &arr = alv.getval<SharedArrayObj>();
                const auto sk = arr.skind();
                const bool is_bool = sk == SharedArrayObj::Storage::bools;
                if ((sk == SharedArrayObj::Storage::ints
                     || (is_bool && in.aop == Op::invalid))
                    && !alv.is_const_var() && !arr.is_readonly()) {
                    int_type idx = read_int_operand(in.a, &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                        vm_throw_oob(chunk, pc);
                    const int_type rhs = read_int_operand(in.b, &ctx);
                    /* div/mod by zero throws BEFORE any clone (tree-walker
                     * throws during the op eval, before the COW). */
                    if ((in.aop == Op::div || in.aop == Op::mod) && rhs == 0)
                        vm_throw_div0(chunk, pc);
                    if (arr.is_slice())
                        arr.clone_internal_vec();
                    else if (arr.use_count() > 1)
                        arr.clone_aliased_slices(arr.offset() + idx);
                    if (is_bool) {
                        arr.flat_bools()[arr.offset() + idx] = rhs ? 1 : 0;
                        arr.invalidate_hash();
                        pc++;
                        break;
                    }
                    int_type &el = arr.flat_ints()[arr.offset() + idx];
                    switch (in.aop) {
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
                    break;
                }
            }
            {
                Loc ls, le;
                chunk.loc_at(pc, ls, le);
                vm_subscript_store(&alv, EvalValue(read_int_operand(in.a, &ctx)),
                                   EvalValue(read_int_operand(in.b, &ctx)),
                                   vm_base_to_expr14_op(in.aop), ls, le);
            }
            pc++;
            break;
        }

        case OpCode::StoreElemFloat: {

            /* Caret from the loc side table, looked up LAZILY on the cold
             * throw/fallback paths only (see StoreElemInt). */
            LValue &alv =
                *vm_store_base(ctx, in.target, in.target2, chunk, pc, nullptr);
            if (alv.is<SharedArrayObj>()) {
                SharedArrayObj &arr = alv.getval<SharedArrayObj>();
                if (arr.skind() == SharedArrayObj::Storage::floats
                    && !alv.is_const_var() && !arr.is_readonly()) {
                    int_type idx = read_int_operand(in.a, &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                        vm_throw_oob(chunk, pc);
                    const float_type rhs = read_float_operand(in.b, &ctx);
                    if ((in.aop == Op::div || in.aop == Op::mod) && rhs == 0.0)
                        vm_throw_div0(chunk, pc);
                    if (arr.is_slice())
                        arr.clone_internal_vec();
                    else if (arr.use_count() > 1)
                        arr.clone_aliased_slices(arr.offset() + idx);
                    float_type &el = arr.flat_floats()[arr.offset() + idx];
                    switch (in.aop) {
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
                    break;
                }
            }
            {
                Loc ls, le;
                chunk.loc_at(pc, ls, le);
                vm_subscript_store(&alv, EvalValue(read_int_operand(in.a, &ctx)),
                                   EvalValue(read_float_operand(in.b, &ctx)),
                                   vm_base_to_expr14_op(in.aop), ls, le);
            }
            pc++;
            break;
        }

        case OpCode::DictStore: {

            /* d[k] = v / d[k] OP= v (P2): store via the shared, type-dispatched
             * vm_subscript_store (auto-vivify / COW / key-freeze / throw -
             * matches the tree-walker for ANY base type, so no is-dict guard /
             * node->eval fallback is needed). AST-free: the subscript's caret
             * (`d[k]`) comes from the loc side table (recorded by extract_locs
             * from node = the Subscript). */
            LValue &dlv =
                *vm_store_base(ctx, in.target, in.target2, chunk, pc, nullptr);
            const EvalValue &key = ctx.frame->at(in.a.slot).get();
            const EvalValue &val = ctx.frame->at(in.b.slot).get();
            /* Loc looked up LAZILY on the throw path only (a successful store -
             * the hot case - pays no loc_at binary search); vm_subscript_store
             * uses it only when it throws. */
            try {
                vm_subscript_store(&dlv, key, val, in.aop, Loc(), Loc());
            } catch (Exception &e) {
                if (!e.loc_start)
                    chunk.loc_at(pc, e.loc_start, e.loc_end);
                throw;
            }
            pc++;
            break;
        }

        case OpCode::StoreMemberV: {
            /* s.member = v / s.member OP= v for a STRUCT base (a dict member
             * store uses DictStore). vm_member_store does the POD/boxed-field
             * store; AST-free - the member uid + carets come from the pool. */
            const Chunk::MemberKey &mk = chunk.member_keys[in.a.lit];
            const EvalValue &val = ctx.frame->at(in.b.slot).get();
            try {
                /* base inside the try so an undefined-global throw is stamped
                 * with the member caret; base may be global/capture (in.target
                 * = kind). */
                LValue *blv = vm_store_base(ctx, in.target, in.target2,
                                            chunk, pc, nullptr);
                vm_member_store(blv, mk.memUid, in.aop, val,
                                mk.mstart, mk.mend, mk.bstart, mk.bend);
            } catch (Exception &e) {
                if (!e.loc_start) {          /* a compound div/mod is loc-less */
                    e.loc_start = mk.mstart;
                    e.loc_end = mk.mend;
                }
                throw;
            }
            pc++;
            break;
        }

        case OpCode::StoreElemValue: {

            /* a[i] = v / a[i] OP= v for a GENERAL array (P4): store via the
             * shared, type-dispatched vm_subscript_store (bounds check + COW +
             * slot_rmw - matches the tree-walker for ANY base). AST-free: the
             * subscript's caret comes from the loc side table. */
            LValue &alv =
                *vm_store_base(ctx, in.target, in.target2, chunk, pc, nullptr);
            const EvalValue &idx = ctx.frame->at(in.a.slot).get();
            const EvalValue &val = ctx.frame->at(in.b.slot).get();
            /* Loc looked up LAZILY on the throw path only (see DictStore). */
            try {
                vm_subscript_store(&alv, idx, val, in.aop, Loc(), Loc());
            } catch (Exception &e) {
                if (!e.loc_start)
                    chunk.loc_at(pc, e.loc_start, e.loc_end);
                throw;
            }
            pc++;
            break;
        }

        case OpCode::StoreElem2V: {

            /* a[i][j] = v / OP= v (nested general store): read a[i] as a
             * reference then store [j] into it (two-level vm_subscript_store).
             * AST-free: the outer subscript's caret comes from the side table. */
            LValue &alv = ctx.frame->at(in.target2);
            const EvalValue &k1 = ctx.frame->at(in.a.slot).get();
            const EvalValue &k2 = ctx.frame->at(in.b.slot).get();
            const EvalValue &val = ctx.frame->at(in.target).get();
            /* Loc looked up LAZILY on the throw path only (see DictStore). */
            try {
                vm_nested_subscript_store(&alv, k1, k2, val, in.aop,
                                          Loc(), Loc());
            } catch (Exception &e) {
                if (!e.loc_start)
                    chunk.loc_at(pc, e.loc_start, e.loc_end);
                throw;
            }
            pc++;
            break;
        }

        case OpCode::StoreElemChainV: {
            /* GENERIC N-level nested store a[k0][k1]...[kn] = v / OP= v: form the
             * base LValue* (by slot kind), then walk the keys run. AST-free: the
             * outer subscript's caret comes from the loc side table. */
            LValue *base = vm_store_base(ctx, in.a.lit, in.target2,
                                         chunk, pc, nullptr);
            const EvalValue &val = ctx.frame->at(in.target).get();
            try {
                vm_chain_store_op(ctx, base, in.b.lit, in.a.slot, val, in.aop);
            } catch (Exception &e) {
                if (!e.loc_start)
                    chunk.loc_at(pc, e.loc_start, e.loc_end);
                throw;
            }
            pc++;
            break;
        }

        case OpCode::MultiUnpackV: {

            /* Multi-assign `a, b, c = <rvalue>` (F-1): the tree-walker's STRICT
             * destructure, AST-free. An ARRAY rvalue must have EXACTLY as many
             * elements as targets (else the length TypeErrorEx, caret from the
             * side table); each element (box-free for a flat scalar via
             * vm_arr_elem) writes to its target slot (-1 == `_`, skipped). A
             * NON-array rvalue SPREADS to every target. */
            const EvalValue &rval = ctx.frame->at(in.a.slot).get();
            const std::vector<int32_t> &targets = chunk.unpack_targets[in.target];
            /* A COMPOUND `a, b OP= rhs` (in.aop != invalid): each target reads
             * its CURRENT value, applies the op with its element/scalar, writes
             * back — else a plain distribute. */
            const bool compound = in.aop != Op::invalid;
            auto store = [&](int32_t t, const EvalValue &v) {
                if (!compound) {
                    ctx.frame->at(t).put(v);
                    return;
                }
                EvalValue nv = ctx.frame->at(t).get();
                try {
                    num_bin_op(nv, v, binop_pmf(in.aop));
                } catch (Exception &e) {
                    vm_stamp_loc(chunk, pc, e);
                    throw;
                }
                ctx.frame->at(t).put(std::move(nv));
            };
            if (rval.is<SharedArrayObj>()) {
                const size_type m = rval.get_ref<SharedArrayObj>().size();
                if (m != static_cast<size_type>(targets.size()))
                    vm_throw_multi_unpack_len(chunk, pc, m, targets.size());
                for (size_t i = 0; i < targets.size(); i++)
                    if (targets[i] >= 0)
                        store(targets[i],
                              vm_arr_elem(rval, static_cast<size_type>(i)));
            } else {
                for (int32_t t : targets)
                    if (t >= 0)
                        store(t, rval);
            }
            pc++;
            break;
        }

        case OpCode::DictLoadInt:
        case OpCode::DictLoadFloat: {

            /* Typed dict scalar read d[k] / d.k (P3), AST-FREE (foundation 2):
             * the KEY comes from the CONST POOL for a member `d.k` (in.a is an
             * immediate = the consts index of the interned name) or from a temp
             * slot for a subscript `d[k]` (in.a is a slot) - distinguished by
             * in.a.is_lit, no `node`. A PRESENT key reads the scalar via
             * dict_present_value (hot); a MISSING key / non-dict base goes
             * through the shared Type::subscript (the tree-walker's exact
             * default-dict insert / KeyNotFoundEx / not-subscriptable logic),
             * its loc taken from the loc side table, NOT node->eval. */
            const bool is_int = in.op == OpCode::DictLoadInt;
            const EvalValue &key = in.a.is_lit
                ? chunk.consts[in.a.lit]
                : ctx.frame->at(in.a.slot).get();
            const EvalValue &base = ctx.frame->at(in.target2).get();
            if (base.is<intrusive_ptr<DictObject>>())
                if (const EvalValue *v = dict_present_value(
                        base.get_ref<intrusive_ptr<DictObject>>(), key)) {
                    write_scalar_slot(&ctx, in.target, is_int, *v);
                    pc++;
                    break;
                }
            /* cold: missing key (default insert / throw) or a non-dict base. */
            LValue &dlv = ctx.frame->at(in.target2);
            EvalValue r;
            try {
                r = base.get_type()->subscript(EvalValue(&dlv), key, false);
            } catch (Exception &e) {
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            write_scalar_slot(&ctx, in.target, is_int,
                              r.is<LValue *>() ? r.get<LValue *>()->get() : r);
            pc++;
            break;
        }

        case OpCode::EvalToSlot: {

            /* A scalar-result call evaluated into a temp (native builtin/call
             * dispatch): the result is then read as an int/float operand. */
            ctx.frame->at(in.target).put(RValue(chunk.node_at(in.node_idx)->eval(&ctx)));
            pc++;
            break;
        }

        case OpCode::CallBuiltinV: {

            /* Native builtin call (value ABI): copy the pre-evaluated args from
             * the register run into a buffer and call func_v with the AST-free
             * ArgLocs (carets/hint) from the builtin_calls pool - NO node. The
             * try/catch stamps the args-list loc onto a loc-less error, exactly
             * as DirectBuiltinCallExpr::do_eval does. */
            const Chunk::BuiltinCall &bc = chunk.builtin_calls[in.target2];
            const int_type base = in.a.lit, n = in.b.lit;
            try {
                if (n <= 8) {
                    EvalValue stackbuf[8];
                    for (int_type i = 0; i < n; i++)
                        stackbuf[i] = ctx.frame->at(base + i).get();
                    ArgLocs al = chunk.arglocs_at(in.target2);
                    ctx.frame->at(in.target).put(
                        bc.builtin.func_v(&ctx, &al, stackbuf, n));
                } else {
                    ctx.frame->at(in.target).put(
                        vm_call_builtin_big(ctx, chunk, in.target2, base, n));
                }
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = bc.start;
                    e.loc_end = bc.end;
                }
                throw;
            }
            pc++;
            break;
        }

        case OpCode::MakeArrayV: {

            /* Build an array LITERAL via vm_make_array (its element buffer is
             * kept OUT of vm_run_chunk's frame - see that helper; the frame is
             * multiplied by VM recursion depth). `target2` = the ArrHint; never
             * throws. */
            ctx.frame->at(in.target).put(vm_make_array(
                ctx, in.a.lit, in.b.lit, static_cast<ArrHint>(in.target2)));
            pc++;
            break;
        }

        case OpCode::MakeDictV: {

            /* Build a dict LITERAL via vm_make_dict (key/value buffer kept out
             * of vm_run_chunk's frame). Never throws (all values hashable). */
            ctx.frame->at(in.target).put(
                vm_make_dict(ctx, in.a.lit, in.b.lit));
            pc++;
            break;
        }

        case OpCode::MakeClosureV: {
            /* func[caps]{..} in expression position: create the FuncObject +
             * snapshot the captures from ctx - byte-identical to
             * FuncDeclStmt::do_eval for a lambda. The def is a program-lifetime
             * FuncDeclStmt* from the pool (the Instr holds only the index). The
             * ctor never throws (a resolved closure's captures are defined),
             * so no loc. */
            ctx.frame->at(in.target).put(EvalValue(intrusive_ptr<FuncObject>(
                make_intrusive<FuncObject>(chunk.closure_defs[in.target2],
                                           &ctx))));
            pc++;
            break;
        }

        case OpCode::StructCtorV: {
            /* Standalone POD struct construction P(x,y) via vm_struct_ctor (its
             * field buffer kept out of vm_run_chunk's frame). The typed-arg gate
             * means coerce won't throw; a defensive throw is stamped with the
             * ctor's loc (side table). */
            StructTypeDef *def =
                const_cast<StructTypeDef *>(chunk.struct_defs[in.target2]);
            try {
                ctx.frame->at(in.target).put(
                    vm_struct_ctor(ctx, def, in.a.lit, in.b.lit));
            } catch (Exception &e) {
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            pc++;
            break;
        }

        case OpCode::ThrowRuntimeV: {
            /* An always-throwing construct (undefined name, assign to a
             * non-lvalue / a builtin) — throw the pooled exception with its
             * exact caret, byte-identical to the tree-walker. */
            const Chunk::ThrowSite &t = chunk.throws[in.target];
            switch (t.kind) {
                case Chunk::ThrowKind::undefined_var:
                    throw UndefinedVariableEx(t.name->val, t.start, t.end);
                case Chunk::ThrowKind::not_lvalue:
                    throw NotLValueEx(t.start, t.end);
                case Chunk::ThrowKind::rebind_builtin:
                    throw CannotRebindBuiltinEx(t.start, t.end);
            }
            break;   /* unreachable */
        }

        case OpCode::StructCtorBoxedV: {
            /* Boxed (non-POD) struct construction B(a,x) via
             * vm_struct_ctor_boxed. A field coerce CAN throw (a dyn-laundered
             * wrong value); the per-arg carets come from the boxed_ctors pool so
             * the throw's caret matches the tree-walker's construct_struct. */
            const Chunk::BoxedCtor &bc = chunk.boxed_ctors[in.target2];
            StructTypeDef *def = const_cast<StructTypeDef *>(bc.def);
            ctx.frame->at(in.target).put(vm_struct_ctor_boxed(
                ctx, def, in.a.lit,
                static_cast<int_type>(bc.arg_locs.size()),
                bc.arg_locs.data()));
            pc++;
            break;
        }

        case OpCode::MakeStructArrayV: {
            /* Build a FLAT array<PodStruct> literal `[P(..), ..]` in one op via
             * vm_make_struct_array_op (field-value buffer kept out of
             * vm_run_chunk's frame - it coerces STRAIGHT into a flat byte buffer,
             * no per-element StructObject). All-scalar-field gate => coerce can't
             * throw; a defensive throw gets the ctor loc (side table). */
            StructTypeDef *def =
                const_cast<StructTypeDef *>(chunk.struct_defs[in.target2]);
            try {
                ctx.frame->at(in.target).put(
                    vm_make_struct_array_op(ctx, def, in.a.lit, in.b.lit));
            } catch (Exception &e) {
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            pc++;
            break;
        }

        case OpCode::LoadStructFieldInt:
            /* pts[i].f (scalar int/bool field) read straight from the flat
             * struct-array bytes into a slot - the struct-foreach direct read,
             * no StructObject. target2 = the array slot, a = the counter, b =
             * the field index. */
            write_int_slot(&ctx, in.target,
                vm_struct_field_int(ctx.frame->at(in.target2).get(),
                                    read_int_operand(in.a, &ctx), in.b.lit));
            pc++;
            break;

        case OpCode::LoadStructElemV:
            /* Whole-`p` foreach bind: materialize a fresh StructObject from the
             * flat struct-array element into the loop var. target = loop var,
             * target2 = the array slot, a = the counter. */
            ctx.frame->at(in.target).put(
                vm_struct_elem(ctx.frame->at(in.target2).get(),
                               read_int_operand(in.a, &ctx)));
            pc++;
            break;

        case OpCode::LoadStructFieldFloat:
            write_float_slot(&ctx, in.target,
                vm_struct_field_float(ctx.frame->at(in.target2).get(),
                                      read_int_operand(in.a, &ctx), in.b.lit));
            pc++;
            break;

        case OpCode::CallBuiltinLV: {

            /* Native mutating-builtin call (lvalue ABI): form arg0's LValue*
             * from its slot table (by kind = a.lit), then call func_lv - which
             * mutates through it. AST-FREE: the Builtin + carets come from the
             * builtin_calls pool (a.slot). A REST-NATIVE op (`b` set) gets its
             * value args from the register run at `b`; a `b`-unset op (pop/intptr
             * - no value args) gets an empty rest. Mirrors Identifier::do_eval
             * for each kind: a not-yet-defined global -> null target ->
             * NotLValueEx, like the tree-walker. */
            const Chunk::BuiltinCall &bc = chunk.builtin_calls[in.a.slot];
            LValue *target;
            switch (in.a.lit) {
            case 0:   /* local */
                target = &ctx.frame->at(in.target2);
                break;
            case 1:   /* global */
                target = ctx.gfuncs->defined[in.target2]
                             ? &ctx.gfuncs->slots[in.target2] : nullptr;
                break;
            default:  /* capture */
                target = &(*ctx.captures)[in.target2];
                break;
            }
            try {
                if (in.b.is_lit) {
                    ctx.frame->at(in.target).put(
                        vm_call_builtin_lv_rest(ctx, chunk, in.a.slot, target,
                                                in.b.lit));
                } else {
                    ArgLocs al = chunk.arglocs_at(in.a.slot);
                    ctx.frame->at(in.target).put(
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
            break;
        }

        case OpCode::EmplaceStruct: {

            /* append(struct_arr, Ctor(args)) with the ctor's field VALUES in
             * run at `b`: form arg0's LValue* (like CallBuiltinLV), then coerce
             * the values straight into the flat struct array's bytes (no temp
             * StructObject); vm_emplace_struct handles the flat path + the
             * general fallback and matches the tree-walker append. */
            const DirectBuiltinCallExpr *dc =
                static_cast<const DirectBuiltinCallExpr *>(chunk.node_at(in.node_idx));
            LValue *target;
            switch (in.a.lit) {
            case 0:  target = &ctx.frame->at(in.target2); break;
            case 1:  target = ctx.gfuncs->defined[in.target2]
                         ? &ctx.gfuncs->slots[in.target2] : nullptr; break;
            default: target = &(*ctx.captures)[in.target2]; break;
            }
            try {
                ctx.frame->at(in.target).put(
                    vm_do_emplace(ctx, dc, target, in.b.lit));
            } catch (Exception &e) {
                if (!e.loc_start) {
                    e.loc_start = dc->args->start;
                    e.loc_end = dc->args->end;
                }
                throw;
            }
            pc++;
            break;
        }

        case OpCode::CallBuiltinLVElem: {

            /* Mutating builtin with a subscript target a[i]/d[k] (Phase 2c):
             * form the base's LValue* (by kind), then the ELEMENT's LValue* via
             * the runtime Type::subscript - the SAME COW path the tree-walker's
             * Subscript::do_eval uses, given the identical base LValue* - and
             * call func_lv REST-NATIVE. `b` = the run base: run[0] = the index,
             * run[1..] = the pre-evaluated value args (append/push 1, pop 0). A
             * non-lvalue element (a flat scalar / read-only / missing dict key,
             * which throws) gives a null target -> NotLValueEx, like the
             * tree-walker. AST-FREE: Builtin + carets from the pool (a.slot). */
            const Chunk::BuiltinCall &bc = chunk.builtin_calls[in.a.slot];
            LValue *base;
            switch (in.a.lit) {
            case 0:  base = &ctx.frame->at(in.target2); break;
            case 1:  base = ctx.gfuncs->defined[in.target2]
                         ? &ctx.gfuncs->slots[in.target2] : nullptr; break;
            default: base = &(*ctx.captures)[in.target2]; break;
            }
            const int_type n_rest = static_cast<int_type>(bc.args.size()) - 1;
            try {
                EvalValue holder;   /* keeps the subscript result alive */
                LValue *elem = nullptr;
                if (base) {
                    const EvalValue &idx = ctx.frame->at(in.b.lit).get();
                    holder = base->get().get_type()->subscript(
                        EvalValue(base), idx, /*for_write=*/false);
                    if (holder.is<LValue *>())
                        elem = holder.get<LValue *>();
                }
                EvalValue restbuf[8];   /* n_rest is small (append 1, pop 0) */
                for (int_type i = 0; i < n_rest; i++)
                    restbuf[i] = ctx.frame->at(in.b.lit + 1 + i).get();
                ArgLocs al = chunk.arglocs_at(in.a.slot);
                ctx.frame->at(in.target).put(
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
            break;
        }

        case OpCode::CallBuiltinLVMember: {

            /* Mutating builtin with a struct-MEMBER target `append(s.f, x)`:
             * form the base's LValue* (by kind), the boxed FIELD LValue* via
             * vm_member_lvalue (same checks as the tree-walker's MemberExpr),
             * then call func_lv REST-NATIVE. `b` = the rest run (values, NO
             * index — unlike LVElem). AST-free: Builtin + carets + field name
             * from the pool (a.slot). */
            const Chunk::BuiltinCall &bc = chunk.builtin_calls[in.a.slot];
            LValue *base;
            switch (in.a.lit) {
            case 0:  base = &ctx.frame->at(in.target2); break;
            case 1:  base = ctx.gfuncs->defined[in.target2]
                         ? &ctx.gfuncs->slots[in.target2] : nullptr; break;
            default: base = &(*ctx.captures)[in.target2]; break;
            }
            const int_type n_rest = static_cast<int_type>(bc.args.size()) - 1;
            try {
                LValue *field = nullptr;
                if (base)
                    field = vm_member_lvalue(base, bc.member,
                                             bc.args[0].start, bc.args[0].end,
                                             bc.args[0].start, bc.args[0].end);
                EvalValue restbuf[8];   /* append/push 1 value arg */
                for (int_type i = 0; i < n_rest; i++)
                    restbuf[i] = ctx.frame->at(in.b.lit + i).get();
                ArgLocs al = chunk.arglocs_at(in.a.slot);
                ctx.frame->at(in.target).put(
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
            break;
        }

        case OpCode::CallV:
        case OpCode::CachedCallV: {

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
             * &chunk, pc - no per-call lookup on the success path). */
            if (!ctx.gfuncs->defined[in.target2]) {
                Loc s, en;
                chunk.loc_at(pc, s, en);
                throw UndefinedVariableEx(
                    ctx.gfuncs->names[in.target2]->val, s, en);
            }
            const EvalValue &callee = ctx.gfuncs->slots[in.target2].get();
            if (!callee.is<intrusive_ptr<FuncObject>>()) {
                Loc s, en;
                chunk.loc_at(pc, s, en);
                throw NotCallableEx(s, en);
            }
            FuncObject &fo = *callee.get<intrusive_ptr<FuncObject>>().get();
            LValue *ap = &ctx.frame->at(in.a.lit);
            EvalValue res =
                in.op == OpCode::CachedCallV
                    ? vm_cached_call(&ctx, fo, ap, in.b.lit, &chunk, pc)
                    : vm_call_func(&ctx, fo, ap, in.b.lit, &chunk, pc);
            /* Inc v2: the callee (or something it called) is unwinding
             * cross-frame - route to a same-frame handler, or return to keep
             * propagating (this frame's do_func_call captures it). Inc 4: if
             * THIS call was spliced from inlined code, flush its virtual frames
             * as the exception passes through. */
            if (g_vm_exc_pending) {
                vm_flush_inline(chunk, pc, *g_vm_exc_pending);
                if (vm_dispatch_exc(handlers, pc)) {
                    vm_exc = std::move(g_vm_exc_pending);
                    continue;
                }
                return;
            }
            ctx.frame->at(in.target).put(std::move(res));
            pc++;
            break;
        }

        case OpCode::CallValueV: {
            /* Indirect call of a func VALUE: the callee was evaluated into
             * target2 (a FuncObject - proven by the Func static type). Read it
             * and call vm_call_func; a dyn-laundered non-func throws
             * NotCallableEx via the loc side table (the call-site loc). */
            const EvalValue &callee = ctx.frame->at(in.target2).get();
            if (!callee.is<intrusive_ptr<FuncObject>>()) {
                Loc s, en;
                chunk.loc_at(pc, s, en);
                throw NotCallableEx(s, en);
            }
            FuncObject &fo = *callee.get<intrusive_ptr<FuncObject>>().get();
            LValue *ap = &ctx.frame->at(in.a.lit);
            EvalValue res = vm_call_func(&ctx, fo, ap, in.b.lit, &chunk, pc);
            if (g_vm_exc_pending) {                /* Inc v2: cross-frame */
                vm_flush_inline(chunk, pc, *g_vm_exc_pending);   /* Inc 4 */
                if (vm_dispatch_exc(handlers, pc)) {
                    vm_exc = std::move(g_vm_exc_pending);
                    continue;
                }
                return;
            }
            ctx.frame->at(in.target).put(std::move(res));
            pc++;
            break;
        }

        case OpCode::CallValueGenericV: {
            /* Generic indirect call of a DYN callee: read the callee (evaluated
             * into `a.lit`) and dispatch on its runtime type via the shared
             * dispatch_call_value (FuncObject / Builtin / struct / non-callable)
             * - byte-identical to CallExpr::do_eval. A FuncObject body runs
             * native (as_signal routes its cross-frame VM exception); a Builtin/
             * struct/non-callable error is a plain C++ throw the boundary
             * catches. `node` = the CallExpr (its args ExprList + callee caret). */
            const EvalValue &callee = ctx.frame->at(in.a.lit).get();
            const CallExpr *node =
                static_cast<const CallExpr *>(chunk.node_at(in.node_idx));
            EvalValue res =
                dispatch_call_value(&ctx, callee, node, &chunk, pc, true);
            if (g_vm_exc_pending) {                /* FuncObject cross-frame */
                vm_flush_inline(chunk, pc, *g_vm_exc_pending);
                if (vm_dispatch_exc(handlers, pc)) {
                    vm_exc = std::move(g_vm_exc_pending);
                    continue;
                }
                return;
            }
            ctx.frame->at(in.target).put(std::move(res));
            pc++;
            break;
        }

        case OpCode::CheckFuncV:
            /* map/filter's arg0 guard: throw (arg0's caret, from the loc side
             * table) if it isn't a function, BEFORE arg1's code runs - the
             * tree-walker's order. AST-free. */
            if (!ctx.frame->at(in.a.slot).get()
                     .is<intrusive_ptr<FuncObject>>()) {
                Loc s, en;
                chunk.loc_at(pc, s, en);
                throw TypeErrorEx("Expected function", s, en);
            }
            pc++;
            break;

        case OpCode::MapFilterV: {
            /* map/filter over the pre-validated function + the container; the
             * unsupported-container caret comes from the loc side table. */
            Loc s, en;
            chunk.loc_at(pc, s, en);
            ctx.frame->at(in.target).put(
                vm_map_filter(&ctx, ctx.frame->at(in.a.slot).get(),
                              ctx.frame->at(in.b.slot).get(),
                              in.target2 != 0, s, en));
            pc++;
            break;
        }

        case OpCode::ReturnV:
            /* `return <expr>`: the value is already in a.slot (a bare return
             * loaded `none`). Set flow and STOP the chunk, as an
             * EvalStmt(ReturnStmt) does; do_func_call reads flow->value. */
            ctx.flow->value = ctx.frame->at(in.a.slot).get();
            ctx.flow->type = FlowState::ret;
            return;

        case OpCode::ArrLen:
            /* n = size(array). The base is a flat array (ForeachStmt::elem_th
             * guarantees array<int>/array<float>), so read size() directly. */
            ctx.frame->at(in.target).put(EvalValue(static_cast<int_type>(
                ctx.frame->at(in.target2).get()
                    .get_ref<SharedArrayObj>().size())));
            pc++;
            break;

        case OpCode::StrLen:
            /* n = char count of the string (foreach bound over a proven str).
             * get_view() accounts for a slice's offset. */
            ctx.frame->at(in.target).put(EvalValue(static_cast<int_type>(
                ctx.frame->at(in.target2).get()
                    .get_ref<SharedStr>().get_view().size())));
            pc++;
            break;

        case OpCode::LoadStrChar: {
            /* x = a fresh 1-char string of the container's i-th char - matches
             * the tree-walker's SharedStr(string(&view[i], 1)). `i` is
             * loop-bounded (< StrLen), so view[i] is in range. */
            const std::string_view view = ctx.frame->at(in.target2).get()
                .get_ref<SharedStr>().get_view();
            const int_type i = read_int_operand(in.a, &ctx);
            ctx.frame->at(in.target).put(
                EvalValue(SharedStr(std::string(&view[i], 1))));
            pc++;
            break;
        }

        case OpCode::LoadImmInt:
            ctx.frame->at(in.target).put(
                EvalValue(static_cast<int_type>(in.a.lit)));
            pc++;
            break;

        case OpCode::LoadImmFloat:
            ctx.frame->at(in.target).put(
                EvalValue(static_cast<float_type>(in.a.flit)));
            pc++;
            break;

        case OpCode::LoadConstV:
            ctx.frame->at(in.target).put(chunk.consts[in.target2]);
            pc++;
            break;

        case OpCode::LoadLiteralObjV: {
            /* Materialize a baked const array/dict/struct literal via shared
             * eval_literal_obj (immutable share vs a fresh mutable clone, plus
             * the general/flat_s arr_hint cases) - byte-identical to
             * LiteralObj::do_eval. */
            const Chunk::LiteralObjEntry &lo = chunk.literal_objs[in.target2];
            ctx.frame->at(in.target).put(
                eval_literal_obj(lo.value, lo.immutable, lo.arr_hint,
                                 lo.arr_hint_struct));
            pc++;
            break;
        }

        case OpCode::MoveV:
            /* Alias, not clone (a container assignment shares the handle;
             * matches doAssign's `x = RValue(rval)` - COW protects later). */
            ctx.frame->at(in.target).put(
                ctx.frame->at(in.target2).get());
            pc++;
            break;

        case OpCode::BinOpV: {
            /* Clone the left operand, then num_bin_op mutates the clone (so
             * `a + b` never corrupts a) - byte-identical to the tree-walker's
             * eval_first_rvalue().clone() + num_binop_loc chain (int/float
             * promotion, string `+` concat, bitwise). */
            EvalValue sa, sb;
            EvalValue val = boxed_operand(in.a, &ctx, sa).clone();
            try {
                num_bin_op(val, boxed_operand(in.b, &ctx, sb),
                           binop_pmf(in.aop));
            } catch (Exception &e) {
                /* Stamp the operand loc (side table) like stamp_operand_loc, so
                 * a div-zero / type error points where the tree-walker does. */
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            ctx.frame->at(in.target).put(std::move(val));
            pc++;
            break;
        }

        case OpCode::CompoundV: {
            /* dst OP= b: COPY the lvalue (a container shares its handle, so the
             * op mutates it in place), apply num_bin_op, store back - identical
             * to doAssign's compound branch. `b` may be an immediate. */
            EvalValue sb;
            EvalValue nv = ctx.frame->at(in.target).get();
            try {
                num_bin_op(nv, boxed_operand(in.b, &ctx, sb),
                           binop_pmf(in.aop));
            } catch (Exception &e) {
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            ctx.frame->at(in.target).put(std::move(nv));
            pc++;
            break;
        }

        case OpCode::CmpV: {
            /* dst = (a <cmp> b) as a bool - copy a, num_bin_op with the
             * comparison PMF, store is_true() (= Expr06/Expr07::do_eval). */
            EvalValue sa, sb;
            EvalValue val = boxed_operand(in.a, &ctx, sa);
            try {
                num_bin_op(val, boxed_operand(in.b, &ctx, sb),
                           cmp_pmf(in.aop));
            } catch (Exception &e) {
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            ctx.frame->at(in.target).put(EvalValue(val.is_true()));
            pc++;
            break;
        }

        case OpCode::LogV: {
            /* Eager (MyLang &&/|| don't short-circuit at runtime) - both
             * operands are already computed (a slot or an immediate); combine
             * truthiness. */
            EvalValue sa, sb;
            const bool a = boxed_operand(in.a, &ctx, sa).is_true();
            const bool b = boxed_operand(in.b, &ctx, sb).is_true();
            ctx.frame->at(in.target).put(
                EvalValue(in.aop == Op::land ? (a && b) : (a || b)));
            pc++;
            break;
        }

        case OpCode::UnaryV: {
            /* Boxed unary over a dyn/general operand - mirrors Expr02::do_eval
             * (clone the operand, apply). `-str`/`~str` throw a type error via
             * the Type vtable -> stamp the loc side table. */
            EvalValue s;
            EvalValue v = boxed_operand(in.a, &ctx, s).clone();
            try {
                switch (in.aop) {
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
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            ctx.frame->at(in.target).put(std::move(v));
            pc++;
            break;
        }

        case OpCode::LoadGlobalV:
            /* AST-free: the hot read is a gfuncs slot; the cold undefined
             * error takes its NAME from gfuncs's slot->name list and its loc
             * from the side table - no `node`. */
            if (!ctx.gfuncs->defined[in.target2]) {
                Loc s, en;
                chunk.loc_at(pc, s, en);
                throw UndefinedVariableEx(
                    ctx.gfuncs->names[in.target2]->val, s, en);
            }
            ctx.frame->at(in.target).put(
                ctx.gfuncs->slots[in.target2].get());
            pc++;
            break;

        case OpCode::LoadCaptureV:
            ctx.frame->at(in.target).put(
                (*ctx.captures)[in.target2].get());
            pc++;
            break;

        case OpCode::LoadBuiltinV:
            ctx.frame->at(in.target).put(builtin_slot(in.target2).get());
            pc++;
            break;

        case OpCode::DefinedGlobalV:
            /* defined(global): the slot's defined-flag IS the answer (false
             * before the decl ran, true after) - byte-identical to
             * builtin_defined evaluating the identifier to an UndefinedId or
             * not. AST-free, never throws. */
            ctx.frame->at(in.target).put(
                EvalValue(ctx.gfuncs->defined[in.target2] != 0));
            pc++;
            break;

        case OpCode::StoreGlobalV: {
            LValue &lv = ctx.gfuncs->slots[in.target];
            if (in.aop == Op::invalid) {
                /* g = <expr>: write the shared global slot + mark defined -
                 * exactly slot_rmw(op==assign) (put(RValue)) + the decl's
                 * defined=1. Serves both a decl and a reassign (idempotent). */
                lv.put(RValue(ctx.frame->at(in.a.slot).get()));
                ctx.gfuncs->defined[in.target] = 1;
            } else {
                /* g OP= rhs / g++ (aop = the base op, rhs in `a`): a compound
                 * requires the slot already DEFINED (else UndefinedVariableEx,
                 * like the tree-walker falling through its defined guard), then
                 * copy-modify-store via num_bin_op - identical to CompoundV. */
                if (!ctx.gfuncs->defined[in.target]) {
                    Loc s, en;
                    chunk.loc_at(pc, s, en);
                    throw UndefinedVariableEx(
                        ctx.gfuncs->names[in.target]->val, s, en);
                }
                EvalValue sb;
                EvalValue nv = lv.get();
                try {
                    num_bin_op(nv, boxed_operand(in.a, &ctx, sb),
                               binop_pmf(in.aop));
                } catch (Exception &e) {
                    vm_stamp_loc(chunk, pc, e);
                    throw;
                }
                lv.put(std::move(nv));
            }
            pc++;
            break;
        }

        case OpCode::DeclConstV: {
            /* const arr/dict/func decl: bind the slot as a CONST LValue (so a
             * later rebind still throws), local (target2==0) or global (==1).
             * The rvalue value is already materialized in `a`. */
            EvalValue v = ctx.frame->at(in.a.slot).get();
            if (in.target2 == 0) {
                ctx.frame->at(in.target) = LValue(std::move(v), true);
            } else {
                ctx.gfuncs->slots[in.target] = LValue(std::move(v), true);
                ctx.gfuncs->defined[in.target] = 1;
            }
            pc++;
            break;
        }

        case OpCode::StoreCaptureV: {
            /* cap = <expr> / cap OP= v / cap++ : write the called closure's
             * per-instance capture slot. A capture is ALWAYS defined (snapshot
             * at closure creation), so - unlike a global - no defined check. */
            LValue &lv = (*ctx.captures)[in.target];
            if (in.aop == Op::invalid) {
                lv.put(RValue(ctx.frame->at(in.a.slot).get()));
            } else {
                EvalValue sb;
                EvalValue nv = lv.get();
                try {
                    num_bin_op(nv, boxed_operand(in.a, &ctx, sb),
                               binop_pmf(in.aop));
                } catch (Exception &e) {
                    vm_stamp_loc(chunk, pc, e);
                    throw;
                }
                lv.put(std::move(nv));
            }
            pc++;
            break;
        }

        case OpCode::SubscriptV: {
            /* base[idx] read via the runtime Type::subscript (any base type -
             * array / dict / string), an LValue* to the base slot passed like
             * Subscript::do_eval, for_write=false, RValue'd into dst. */
            LValue &base_lv = ctx.frame->at(in.target2);
            const EvalValue &idx = ctx.frame->at(in.a.slot).get();
            try {
                Type *t = base_lv.get().get_type();
                ctx.frame->at(in.target).put(
                    RValue(t->subscript(EvalValue(&base_lv), idx, false)));
            } catch (Exception &e) {
                vm_stamp_loc(chunk, pc, e);   /* AST-free: loc side table */
                throw;
            }
            pc++;
            break;
        }

        case OpCode::MemberV: {
            /* base.member value read via the shared member_read_core (struct
             * field / const / dict key / optional); AST-free - the name
             * key/uid, optional flag and carets come from the pool (in.a). */
            const Chunk::MemberKey &mk = chunk.member_keys[in.a.lit];
            ctx.frame->at(in.target).put(
                member_read_core(ctx.frame->at(in.target2).get(), mk.memId,
                                 mk.memUid, mk.optional, mk.mstart, mk.mend,
                                 mk.bstart, mk.bend));
            pc++;
            break;
        }

        case OpCode::SliceV: {
            /* base[start:end] via the runtime Type::slice (which RValues the
             * base + registers the COW slice view) - mirrors Slice::do_eval. An
             * absent bound (slot -1) passes `none`. The slice() throws (a
             * non-int index) get the caret from the loc side table. */
            const EvalValue &base = ctx.frame->at(in.target2).get();
            const EvalValue start = in.a.slot >= 0
                ? ctx.frame->at(in.a.slot).get() : EvalValue();
            const EvalValue end = in.b.slot >= 0
                ? ctx.frame->at(in.b.slot).get() : EvalValue();
            try {
                ctx.frame->at(in.target).put(
                    base.get_type()->slice(base, start, end));
            } catch (Exception &e) {
                vm_stamp_loc(chunk, pc, e);
                throw;
            }
            pc++;
            break;
        }

        case OpCode::JumpUnlessTrueV:
            if (!ctx.frame->at(in.target2).get().is_true())
                pc = in.target;
            else
                pc++;
            break;

        case OpCode::Throw: {
            /* P8 Inc 1: raise the value in slot `a`. vm_raise builds the native
             * dispatch (a same-frame catch jump, no C++ throw - the 42 win) or
             * C++-throws to the caller's boundary (cross-frame, like the
             * tree-walker) - the SAME path a runtime error takes. */
            Loc ls, le;
            chunk.loc_at(pc, ls, le);
            const EvalValue &tv = ctx.frame->at(in.a.slot).get();
            vm_raise(chunk, pc, handlers, vm_exc,
                     vm_make_thrown_exc(tv, ls, le));
            continue;                          /* re-dispatch (or unreached) */
        }

        case OpCode::PushHandler:
            handlers.push_back({ static_cast<uint32_t>(in.target) });
            pc++;
            break;

        case OpCode::PopHandler:
            handlers.pop_back();      /* the try body exited normally */
            pc++;
            break;

        case OpCode::CatchTest: {
            /* vm_exc holds the caught exception. Match its type name against
             * this clause (a.lit = catch_types idx, or -1 = catch-all). On a
             * match: bind `catch (T as e)` (target2 = slot, -1 if none), jump to
             * the catch body (target). Else fall to the next CatchTest /
             * Reraise. vm_exc is KEPT (not cleared) so a `rethrow` in the catch
             * body can re-raise it; a new throw overwrites it. */
            bool match;
            if (in.a.lit < 0) {
                match = true;
            } else {
                const std::vector<std::string> &names =
                    chunk.catch_types[in.a.lit];
                const std::string_view en = vm_exc_name(vm_exc.get());
                match = false;
                for (const std::string &nm : names)
                    if (nm == en) { match = true; break; }
            }
            if (match) {
                if (in.target2 >= 0)
                    ctx.frame->at(in.target2).put(
                        vm_catch_bind_val(vm_exc.get()));
                pc = static_cast<size_t>(in.target);
            } else {
                pc++;
            }
            break;
        }

        case OpCode::Reraise:
            /* No clause matched: re-raise vm_exc to the OUTER handler (native
             * jump), or C++-throw to propagate. Mirrors saved_ex->rethrow(). */
            if (vm_dispatch_exc(handlers, pc))
                break;
            vm_exc->rethrow();
            break;                    /* unreachable ([[noreturn]]) */

        case OpCode::Rethrow:
            /* `rethrow` in a catch body: re-raise the being-handled vm_exc with
             * the rethrow-site loc (like do_catch's RethrowEx handling), to the
             * OUTER handler (native) or C++-propagate. */
            {
                Loc ls, le;
                chunk.loc_at(pc, ls, le);
                vm_exc->loc_start = ls;
                vm_exc->loc_end = le;
            }
            if (vm_dispatch_exc(handlers, pc))
                break;
            vm_exc->rethrow();
            break;                    /* unreachable ([[noreturn]]) */

        case OpCode::SetPend:
            /* Record what the shared `finally` must resume - normal or reraise
             * (Inc 2b). Flow ops inline their finally, so ret/brk/cont never
             * reach the shared finally. */
            vm_pend = static_cast<Pend>(in.target);
            pc++;
            break;

        case OpCode::EndFinally:
            /* End of the SHARED finally block: resume the pending action. Only
             * the NORMAL and RERAISE exits reach here - a return/break/continue
             * crossing this try INLINES its own copy of the finally (Inc 2c),
             * so `vm_pend` is only ever normal or reraise here. */
            if (vm_pend == Pend::reraise) {
                /* finally didn't handle the exception → re-raise it. */
                if (vm_dispatch_exc(handlers, pc))
                    break;
                vm_exc->rethrow();
            } else {
                pc++;                                 /* fall through to Lend */
            }
            break;

        case OpCode::Halt:
            return;
        }
    }

    } catch (RuntimeException &e) {
        /* An exception reached the boundary: a fallback `throw`, a runtime-
         * library error (div0/OOB/KeyNotFound/…), or a callee's C++-thrown
         * uncaught throw. With an active try, route it to the innermost
         * handler's catch-dispatch (like TryCatchStmt catching a
         * RuntimeException). Else (Inc v2) CONVERT it to the pending-exception
         * SIGNAL and RETURN - do_func_call captures this frame + propagates it
         * WITHOUT a C++ throw per frame; only this one landing-pad ran. */
        /* Inc 4: a library error thrown FROM an inlined op flushes its virtual
         * frames here (the raise happened in C++, not via vm_raise). */
        vm_flush_inline(chunk, pc, e);
        if (!vm_dispatch_exc(handlers, pc)) {
            g_vm_exc_pending.reset(e.clone());
            return;
        }
        vm_exc.reset(e.clone());               /* like saved_ex */
        goto vm_resume;                        /* re-enter at the CatchTest chain */
    }
}
