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
 *
 * `cap_names` is a CLOSURE's capture list - its anonymous capture-struct field
 * names, in `cN` slot order: the header prints them (`; captures (anon struct):
 * { ... }`) and the capture ops read a `cN` slot as its field name. Empty for a
 * non-capturing chunk.
 */
std::string disassemble(const Chunk &chunk, const std::string &title,
                        const std::vector<std::string> &cap_names = {});

/*
 * Disassemble a whole program: the "main" chunk, then every block-bodied
 * function AND CLOSURE reachable (a complete AST walk - a lambda in a
 * `return func[..]{..}` / a var-init / a call arg is found too, not only
 * top-level funcs), each labelled (`func <name>` / `closure#N` / `lambda#N`)
 * with its captures. Compiles the chunks fresh (like a `-vm` run) without
 * executing. Used by the `-vd` driver. NB: a function body that ends in a
 * return has NO trailing `halt` (ReturnV already stops the chunk; only a
 * fall-through body - `main`, a void function - keeps the `halt` terminator).
 */
std::string disassemble_program(const Block *root);

/*
 * Colorize a plain disassembly (from `disassemble`/`disassemble_program`) with
 * 256-color ANSI syntax highlighting - a post-pass that tokenizes each line by
 * the disassembler's own shape (`<pc> <mnemonic> <operands> ; comment`) and
 * colors the pc / mnemonic-by-category / registers / immediates / labels /
 * comments / section headers. The `-vd` driver applies it only on a TTY (not
 * piped, honoring NO_COLOR / --no-color), so a redirected dump stays plain.
 */
std::string highlight_disasm(const std::string &plain);
