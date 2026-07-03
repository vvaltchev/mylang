/* SPDX-License-Identifier: BSD-2-Clause */

#include "codegen.h"
#include "syntax.h"

#include <vector>

namespace {

/*
 * Lowers a statement list to a Chunk. Phase 1: `if` and `while` become native
 * jump structures; their conditions and bodies stay AST fallbacks (one EvalStmt
 * per condition-evaluation / body). `for` is NOT flattened - ForStmt::do_eval
 * wraps init/cond/inc/body in a child EvalContext (loop-variable scope) that a
 * naive flatten would drop - so it (and foreach / ForRangeStmt / every leaf
 * statement) stays a single fallback EvalStmt. `while`/`if` need no such scope
 * (they run in the passed ctx and delegate body scoping to the body Block), so
 * flattening them is sound. Bodies are kept as single fallbacks, so a nested
 * loop's break/continue is handled by the body's own do_eval + FlowState, and
 * the enclosing native loop's LoopBackEdge consumes the top-level one.
 */
struct Codegen {

    Chunk chunk;

    int here() const { return static_cast<int>(chunk.code.size()); }

    size_t emit(OpCode op, const Construct *node = nullptr,
                int target = -1, int target2 = -1)
    {
        chunk.code.push_back({ op, node, target, target2 });
        return chunk.code.size() - 1;
    }

    void gen_stmts(const std::vector<unique_ptr<Construct>> &elems)
    {
        for (const auto &e : elems)
            gen_stmt(e.get());
    }

    void gen_stmt(const Construct *s)
    {
        if (const IfStmt *f = dynamic_cast<const IfStmt *>(s)) {
            gen_if(f);
            return;
        }

        if (const WhileStmt *w = dynamic_cast<const WhileStmt *>(s)) {
            gen_while(w);
            return;
        }

        /* Everything else (leaf statement, for, foreach, ForRangeStmt, a bare
         * nested block, ...) runs tree-walked as one fallback. */
        emit(OpCode::EvalStmt, s);
    }

    void gen_if(const IfStmt *f)
    {
        /* JumpIfFalse cond -> Lelse ; then ; Jump Lend ; Lelse: else ; Lend: */
        const size_t jf = emit(OpCode::JumpIfFalse, f->condExpr.get());

        if (f->thenBlock)
            emit(OpCode::EvalStmt, f->thenBlock.get());

        if (f->elseBlock) {
            const size_t j = emit(OpCode::Jump);
            chunk.code[jf].target = here();          /* Lelse */
            emit(OpCode::EvalStmt, f->elseBlock.get());
            chunk.code[j].target = here();           /* Lend */
        } else {
            chunk.code[jf].target = here();          /* Lend */
        }
    }

    void gen_while(const WhileStmt *w)
    {
        /* Lstart: JumpIfFalse cond -> Lend ; body ; LoopBackEdge ; Lend: */
        const int lstart = here();
        const size_t jf = emit(OpCode::JumpIfFalse, w->condExpr.get());

        if (w->body)
            emit(OpCode::EvalStmt, w->body.get());

        /* continue dest = Lstart (re-test); exit dest (target2) = Lend. */
        const size_t be = emit(OpCode::LoopBackEdge, nullptr, lstart);

        const int lend = here();
        chunk.code[jf].target = lend;
        chunk.code[be].target2 = lend;
    }
};

}  /* namespace */

Chunk
codegen_program(const Block *root)
{
    Codegen cg;
    cg.gen_stmts(root->elems);
    cg.emit(OpCode::Halt);
    return std::move(cg.chunk);
}
