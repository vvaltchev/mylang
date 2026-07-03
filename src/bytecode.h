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
 * JumpIfFalse / LoopBackEdge) so `if`/`while` become jumps; conditions and
 * bodies still fall back to the tree-walker (see plans/bytecode-vm.md).
 *
 * (Named OpCode, not Op: `Op` is already the operator enum in operators.h.)
 */
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

    /* Stop the program. */
    Halt,
};

struct Instr {
    OpCode op;
    const Construct *node = nullptr;  /* fallback op: the AST node; null else */
    int target = -1;    /* Jump/JumpIfFalse dest; LoopBackEdge continue dest */
    int target2 = -1;   /* LoopBackEdge exit dest */
};

struct Chunk {
    std::vector<Instr> code;
};
