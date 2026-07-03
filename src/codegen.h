/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"

class Block;

/*
 * Lower the optimized program (its root Block) to a bytecode Chunk. Phase 0:
 * one EvalStmt per top-level statement, then Halt - i.e. the tree-walker driven
 * by a flat list. Later phases replace the fallbacks with native opcodes. See
 * plans/bytecode-vm.md.
 */
Chunk codegen_program(const Block *root);
