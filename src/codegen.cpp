/* SPDX-License-Identifier: BSD-2-Clause */

#include "codegen.h"
#include "syntax.h"

Chunk
codegen_program(const Block *root)
{
    Chunk chunk;

    /*
     * Phase 0: emit a fallback EvalStmt for every top-level statement, then a
     * Halt. This alone is a complete, correct executor of the whole language -
     * each statement runs tree-walked. Native lowering comes phase by phase
     * (see plans/bytecode-vm.md).
     */
    for (const auto &stmt : root->elems)
        chunk.code.push_back({ OpCode::EvalStmt, stmt.get() });

    chunk.code.push_back({ OpCode::Halt, nullptr });
    return chunk;
}
