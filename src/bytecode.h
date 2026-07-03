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
 * Phase 0: EvalStmt + Halt only - a flat list of one fallback per top-level
 * statement, i.e. the tree-walker driven by an instruction stream.
 *
 * (Named OpCode, not Op: `Op` is already the operator enum in operators.h.)
 */
enum class OpCode : unsigned char {

    /*
     * AST fallback (the incremental-safety pillar): evaluate `node` with the
     * tree-walker. EvalStmt runs a top-level statement - its value is
     * discarded, and an UndefinedId result / in-flight FlowState is handled
     * exactly as Block::do_eval's root loop does.
     */
    EvalStmt,

    /* Stop the program. */
    Halt,
};

struct Instr {
    OpCode op;
    const Construct *node;   /* the AST node for a fallback op; null else */
};

struct Chunk {
    std::vector<Instr> code;
};
