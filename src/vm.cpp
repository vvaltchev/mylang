/* SPDX-License-Identifier: BSD-2-Clause */

#include "vm.h"
#include "codegen.h"
#include "bytecode.h"
#include "syntax.h"
#include "eval.h"
#include "errors.h"

#include <memory>

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

    std::unique_ptr<Frame> root_frame;
    if (root->slot_count) {
        root_frame = std::make_unique<Frame>();
        root_frame->init(root->slot_count);
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

        case OpCode::Halt:
            return;
        }
    }
}
