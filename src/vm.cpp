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

            /*
             * Same as Block::do_eval's root loop: an unresolved name surfaces
             * as an UndefinedId sentinel, and an in-flight return/break/cont
             * stops the program (a top-level `return` in "main").
             */
            if (tmp.is<UndefinedId>())
                throw UndefinedVariableEx(tmp.get<UndefinedId>().id,
                                          in.node->start, in.node->end);

            if (ctx.flow->type != FlowState::none)
                return;

            pc++;
            break;
        }

        case OpCode::Halt:
            return;
        }
    }
}
