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
 * EVERY op's data comes from a SERIALIZABLE pool: a builtin call
 * (`call.blt.v` and the LV family) reads `builtin_calls` (name + arg carets),
 * a struct-append `emplace_sites`, an indirect call `call_sites`, and a caret
 * with nowhere else to live rides the `locs` side table. There is no AST
 * pointer to render: the no-fail codegen removed the fallback ops that needed
 * one, and with them `Chunk::ast_nodes` / `node_table` (see CLAUDE.md's
 * ZERO-AST rule). So the whole dump is what a `.myv` file stores.
 *
 * `cap_names` is a CLOSURE's capture list - its anonymous capture-struct field
 * names, in `cN` slot order: the header prints them (`; captures (anon struct):
 * { ... }`) and the capture ops read a `cN` slot as its field name. Empty for a
 * non-capturing chunk.
 */
struct JitCtx;
std::string disassemble(const Chunk &chunk, const std::string &title,
                        const std::vector<std::string> &cap_names = {},
                        const JitCtx *jc = nullptr);

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

/* The LOADED-IMAGE twin (plans/archived/myv-serializer.md): dump a VmProgram exactly
 * as disassemble_program dumps a fresh compile - the ROUND-TRIP ORACLE
 * (`-vd file.my` vs `-vd file.myv` must be byte-identical) and the everyday
 * "what is in this file" answer. */
struct VmProgram;
std::string disassemble_image(const VmProgram &prog);

/*
 * Colorize a plain disassembly (from `disassemble`/`disassemble_program`) with
 * 256-color ANSI syntax highlighting - a post-pass that tokenizes each line by
 * the disassembler's own shape (`<pc> <mnemonic> <operands> ; comment`) and
 * colors the pc / mnemonic-by-category / registers / immediates / labels /
 * comments / section headers. The `-vd` driver applies it only on a TTY (not
 * piped, honoring NO_COLOR / --no-color), so a redirected dump stays plain.
 */
std::string highlight_disasm(const std::string &plain);

/*
 * ⛔ THE JIT PROFILE MAP (`MYLANG_JIT_MAP=<path>`) - the instrument that
 * answers "which EMITTED INSTRUCTION does the JIT spend its cycles on".
 *
 * Nothing in the repo could answer that. `-vdj` says what was emitted,
 * `bench/run.py` says how long the whole program took, callgrind says
 * how many instructions ran and which C++ FUNCTION they were in - but
 * JIT code lives in an anonymous mapping with no symbols, so every
 * emitted instruction lands in one nameless blob. The whole call-protocol
 * arc was costed by COUNTING the emitted sequence by hand and hoping the
 * hot path was the one being counted.
 *
 * This appends, per placed fragment, a header and one line per decoded
 * instruction at its RUNTIME ADDRESS:
 *
 *     frag <abs-hex> <len> <chunk-name>#<frag-index>
 *     i <abs-hex> <len> <mnemonic>
 *
 * `scripts/jitprofile.py` joins that against a callgrind
 * `--dump-instr=yes` run, which records costs by absolute address, and
 * prints per-instruction Ir. Written only when the env var is set (the
 * decode is not free), and it FORCES `g_jit_annotate` on, because the
 * per-fragment table it walks is built only under that flag.
 */
void jit_write_map(const Chunk &chunk, const std::string &name);
