/* SPDX-License-Identifier: BSD-2-Clause */

#include "vm.h"
#include "codegen.h"
#include "bytecode.h"
#include "syntax.h"
#include "eval.h"
#include "errors.h"
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
static inline int_type
read_int_operand(const Operand &o, EvalContext *ctx)
{
    if (o.is_lit)
        return o.lit;
    const LValue &lv = ctx->frame->slots[o.slot];
    if (lv.is<bool>())
        return lv.getval<bool>() ? 1 : 0;
    return lv.getval<int_type>();
}

/* Write an int result into a frame slot: overwrite the int in place when the
 * slot already holds one (the common, hot case), else set value + int type. */
static inline void
write_int_slot(EvalContext *ctx, int slot, int_type v)
{
    LValue &lv = ctx->frame->slots[slot];
    if (lv.is<int_type>())
        lv.getval<int_type>() = v;
    else
        lv.put(EvalValue(v));
}

/* The float analogues: read an operand as float (an int/bool slot promotes,
 * mirroring Identifier::eval_float) and write a float result into a slot. */
static inline float_type
read_float_operand(const Operand &o, EvalContext *ctx)
{
    if (o.is_lit)
        return o.flit;
    const LValue &lv = ctx->frame->slots[o.slot];
    if (lv.is<int_type>())
        return static_cast<float_type>(lv.getval<int_type>());
    if (lv.is<bool>())
        return lv.getval<bool>() ? 1.0 : 0.0;
    return lv.getval<float_type>();
}

static inline void
write_float_slot(EvalContext *ctx, int slot, float_type v)
{
    LValue &lv = ctx->frame->slots[slot];
    if (lv.is<float_type>())
        lv.getval<float_type>() = v;
    else
        lv.put(EvalValue(v));
}

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

    /* Fresh per run: drop any prior program's function chunks (and their stale
     * FuncDeclStmt::vm_chunk pointers, whose functions are long freed). */
    g_func_chunks.clear();

    const Chunk chunk = codegen_program(root);

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
    if (!fdecl->body || !fdecl->body->is_block())
        return nullptr;

    const Block *body = static_cast<const Block *>(fdecl->body.get());

    /* vm_run_chunk runs the body's statements directly in the call's args
     * context (no per-block child EvalContext), which is correct only for a
     * SCOPE-FREE body (every decl is a frame slot - no capture / nested func).
     * A non-scope-free body needs its own child context, so tree-walk it. */
    if (!body->scope_free)
        return nullptr;

    auto it = g_func_chunks.find(fdecl);
    if (it != g_func_chunks.end())
        return &it->second;
    Chunk ck = codegen_chunk(body, fdecl->frame_size);

    bool has_native = false;
    for (const Instr &in : ck.code) {
        if (in.op == OpCode::IntBin || in.op == OpCode::FloatBin
            || in.op == OpCode::JumpUnlessIntCmp
            || in.op == OpCode::JumpUnlessFloatCmp
            || in.op == OpCode::ForLoopStep) {
            has_native = true;
            break;
        }
    }
    if (!has_native)
        return nullptr;

    return &g_func_chunks.emplace(fdecl, std::move(ck)).first->second;
}

void
vm_run_chunk(const Chunk &chunk, EvalContext &ctx)
{
    for (size_t pc = 0; ; ) {

        const Instr &in = chunk.code[pc];

        switch (in.op) {

        case OpCode::EvalStmt: {

            EvalValue &&tmp = in.node->eval(&ctx);

            /* An unresolved name surfaces as an UndefinedId sentinel, which
             * Block::do_eval turns into UndefinedVariableEx. */
            if (tmp.is<UndefinedId>())
                throw UndefinedVariableEx(tmp.get<UndefinedId>().id,
                                          in.node->start, in.node->end);

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
            if (vm_eval_cond(in.node, &ctx))
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

        case OpCode::IntBin: {

            const int_type a = read_int_operand(in.a, &ctx);
            const int_type b = read_int_operand(in.b, &ctx);
            int_type r;

            switch (in.aop) {
            case Op::plus:  r = a + b; break;
            case Op::minus: r = a - b; break;
            case Op::times: r = a * b; break;
            case Op::div:
                if (b == 0)
                    throw DivisionByZeroEx(in.node->start, in.node->end);
                r = a / b; break;
            case Op::mod:
                if (b == 0)
                    throw DivisionByZeroEx(in.node->start, in.node->end);
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
                if (b == 0.0)
                    throw DivisionByZeroEx(in.node->start, in.node->end);
                r = a / b; break;
            case Op::mod:
                if (b == 0.0)
                    throw DivisionByZeroEx(in.node->start, in.node->end);
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
            LValue &ilv = ctx.frame->slots[in.target2];
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
            const EvalValue &base = ctx.frame->slots[in.target2].get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = read_int_operand(in.a, &ctx);
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                    throw OutOfBoundsEx(in.node->start, in.node->end);
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
                write_int_slot(&ctx, in.target, in.node->eval_int(&ctx));
            }
            pc++;
            break;
        }

        case OpCode::LoadElemFloat: {

            const EvalValue &base = ctx.frame->slots[in.target2].get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                int_type idx = read_int_operand(in.a, &ctx);
                if (idx < 0)
                    idx += arr.size();
                if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                    throw OutOfBoundsEx(in.node->start, in.node->end);
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
                write_float_slot(&ctx, in.target, in.node->eval_float(&ctx));
            }
            pc++;
            break;
        }

        case OpCode::LoadElemValue: {

            /* a[i] (an array-valued element of a GENERAL array) into a temp
             * slot, so a 2-D read `a[i][k]` is native (both indices). A
             * non-array / non-general base falls back to the node. */
            const EvalValue &base = ctx.frame->slots[in.target2].get();
            if (base.is<SharedArrayObj>()) {
                const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
                if (arr.skind() == SharedArrayObj::Storage::general) {
                    int_type idx = read_int_operand(in.a, &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                        throw OutOfBoundsEx(in.node->start, in.node->end);
                    ctx.frame->slots[in.target].put(
                        arr.get_vec()[arr.offset() + idx].get());
                    pc++;
                    break;
                }
            }
            ctx.frame->slots[in.target].put(RValue(in.node->eval(&ctx)));
            pc++;
            break;
        }

        case OpCode::StoreElemInt: {

            /* a[i] = v / a[i] OP= v for a flat mutable int array (mirrors the
             * int path of try_flat_subscript_store, incl. COW); anything else
             * (const / read-only / general / bool / dyn) falls back to the node
             * - sound because a compiled rvalue is side-effect-free, so re-eval
             * is exact. */
            LValue &alv = ctx.frame->slots[in.target2];
            if (alv.is<SharedArrayObj>()) {
                SharedArrayObj &arr = alv.getval<SharedArrayObj>();
                if (arr.skind() == SharedArrayObj::Storage::ints
                    && !alv.is_const_var() && !arr.is_readonly()) {
                    int_type idx = read_int_operand(in.a, &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                        throw OutOfBoundsEx(in.node->start, in.node->end);
                    const int_type rhs = read_int_operand(in.b, &ctx);
                    /* div/mod by zero throws BEFORE any clone (tree-walker
                     * throws during the op eval, before the COW). */
                    if ((in.aop == Op::div || in.aop == Op::mod) && rhs == 0)
                        throw DivisionByZeroEx(in.node->start, in.node->end);
                    if (arr.is_slice())
                        arr.clone_internal_vec();
                    else if (arr.use_count() > 1)
                        arr.clone_aliased_slices(arr.offset() + idx);
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
            in.node->eval(&ctx);
            pc++;
            break;
        }

        case OpCode::StoreElemFloat: {

            LValue &alv = ctx.frame->slots[in.target2];
            if (alv.is<SharedArrayObj>()) {
                SharedArrayObj &arr = alv.getval<SharedArrayObj>();
                if (arr.skind() == SharedArrayObj::Storage::floats
                    && !alv.is_const_var() && !arr.is_readonly()) {
                    int_type idx = read_int_operand(in.a, &ctx);
                    if (idx < 0)
                        idx += arr.size();
                    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
                        throw OutOfBoundsEx(in.node->start, in.node->end);
                    const float_type rhs = read_float_operand(in.b, &ctx);
                    if ((in.aop == Op::div || in.aop == Op::mod) && rhs == 0.0)
                        throw DivisionByZeroEx(in.node->start, in.node->end);
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
            in.node->eval(&ctx);
            pc++;
            break;
        }

        case OpCode::EvalToSlot: {

            /* A scalar-result call evaluated into a temp (native builtin/call
             * dispatch): the result is then read as an int/float operand. */
            ctx.frame->slots[in.target].put(RValue(in.node->eval(&ctx)));
            pc++;
            break;
        }

        case OpCode::LoadImmInt:
            ctx.frame->slots[in.target].put(
                EvalValue(static_cast<int_type>(in.a.lit)));
            pc++;
            break;

        case OpCode::LoadImmFloat:
            ctx.frame->slots[in.target].put(
                EvalValue(static_cast<float_type>(in.a.flit)));
            pc++;
            break;

        case OpCode::Halt:
            return;
        }
    }
}
