/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"

#include <string>

class Block;

/*
 * Human-readable text disassembly of the bytecode (the `-vd` CLI flag), the
 * bytecode analogue of `-s` for the AST. It is "smart assembly": the operands
 * are register slots (`rN`, unbounded - the VM's registers ARE the frame slots)
 * and immediates (`#N`), and the instructions are the fused superinstructions
 * (a for-loop counter is ONE `for.step`, not three ops).
 *
 * A fallback op that still carries a `Construct*` - `eval.stmt` / `eval.slot` /
 * `jmp.if.not` / a non-array element op - renders that node via the SHARED AST
 * decompiler (`render_construct_code`, coderender.h), so the AST constructs
 * still embedded in the bytecode are represented with the same code as the AST
 * dump, not a duplicate. As those fallbacks become native ops (the no-fallback
 * end-goal), the disassembly loses its `Construct*` rows and becomes pure
 * bytecode - the disassembler is how we track that progress and audit each
 * bench for wasted cycles.
 */
std::string disassemble(const Chunk &chunk, const std::string &title);

/*
 * Disassemble a whole program: the "main" chunk, then every block-bodied
 * function's chunk (each labelled). Compiles the chunks fresh (like a `-vm`
 * run would) without executing. Used by the `-vd` driver.
 */
std::string disassemble_program(const Block *root);
