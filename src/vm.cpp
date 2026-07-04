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
 * Execute the optimized program via the bytecode VM. It builds the program's
 * root EvalContext exactly as Block::do_eval does for the root block
 * (ctx == nullptr): the implicit "main" Frame for slotted top-level vars, and
 * the program-wide GlobalFuncTable for top-level functions / escaped globals.
 * Both live for the whole run. The dispatch loop then drives the chunk; in
 * Phase 0 every instruction is a fallback EvalStmt, so this is byte-identical
 * to root->eval(nullptr) - which the differential harness enforces.
 */
void
vm_execute(const Construct *root_c)
{
    const Block *root = static_cast<const Block *>(root_c);

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

    for (size_t pc = 0; ; ) {

        const Instr &in = chunk.code[pc];

        switch (in.op) {

        case OpCode::EvalStmt: {

            EvalValue &&tmp = in.node->eval(&ctx);

            /* An unresolved name surfaces as an UndefinedId sentinel, which
             * Block::do_eval turns into UndefinedVariableEx. FlowState is NOT
             * acted on here: a break/continue set by a loop body is consumed by
             * the following LoopBackEdge; main has no top-level flow signal. */
            if (tmp.is<UndefinedId>())
                throw UndefinedVariableEx(tmp.get<UndefinedId>().id,
                                          in.node->start, in.node->end);

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
                pc = in.target2;    /* exit loop; leave flow set (propagates) */
                break;

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
                if (b == 0) throw DivisionByZeroEx(in.node->start, in.node->end);
                r = a / b; break;
            case Op::mod:
                if (b == 0) throw DivisionByZeroEx(in.node->start, in.node->end);
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

        case OpCode::Halt:
            return;
        }
    }
}
