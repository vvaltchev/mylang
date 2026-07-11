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
 * A runtime-node op - a fallback (`eval.stmt` / `eval.slot` / `jmp.if.not`), a
 * builtin call, or a flat-store caret - holds its AST node as an INDEX into
 * `Chunk::ast_nodes` (the `Instr` itself has NO `Construct*` - that is what lets
 * the bytecode serialize). The disassembler renders that node via the SHARED AST
 * decompiler (`render_construct_code`, coderender.h) so it reads as the AST
 * dump, not a duplicate, and dumps the `ast_nodes` pool itself (labelled NOT
 * serializable). As those ops become native (the no-fallback end-goal) the pool
 * shrinks toward EMPTY - a fully-native chunk has an empty `ast_nodes`, the
 * accurate "the AST can be dropped" signal for a stored-bytecode `.myv`.
 *
 * `cap_names` is a CLOSURE's capture list - its anonymous capture-struct field
 * names, in `cN` slot order: the header prints them (`; captures (anon struct):
 * { ... }`) and the capture ops read a `cN` slot as its field name. Empty for a
 * non-capturing chunk.
 */
std::string disassemble(const Chunk &chunk, const std::string &title,
                        const std::vector<std::string> &cap_names = {});

/*
 * Disassemble a whole program - 100% of what a serialized `.myv` file would
 * hold, so `-vd` is the audit surface for the stored-bytecode endgame:
 *   1. the program's CUSTOM TYPES (every `struct` def - name, POD layout with
 *      per-field byte offsets / boxed slots, and folded consts);
 *   2. the "main" chunk, then every block-bodied function AND CLOSURE reachable
 *      (a complete AST walk - a lambda in a `return func[..]{..}` / a var-init
 *      / a call arg is found too, not only top-level funcs), each labelled
 *      (`func <name>` / `closure#N` / `lambda#N`) with its captures;
 *   3. after each chunk's code, its serializable POOLS + side tables (consts,
 *      member_keys, catch_types, literal_objs, closure_defs, struct_defs, the
 *      loc table, the inline-frame table) - non-empty ones only.
 * Compiles the chunks fresh (like a `-vm` run) without executing. NB: a func
 * body that ends in a return has NO trailing `halt` (ReturnV already stops the
 * chunk; only a fall-through body - `main`, a void function - keeps `halt`).
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
