/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"

#include <string>
#include <vector>

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

/*
 * ====================  THE STRUCTURED DECODE  ====================
 *
 * ⛔ A TEST ASSERTS ON AN INSTRUCTION, NOT ON THE TEXT THAT RENDERS IT
 * (maintainer, 2026-09-22). `-vdj`'s decoder knows every field - the
 * base register, the index, the scale, the displacement, whether the
 * memory operand resolves to a FRAME SLOT - and then threw all of it
 * away into a string. A shape test that wanted one operand had to
 * parse the rendering back, which is a second decoder in all but name:
 * it re-derives what was already known, it is not the thing objdump
 * cross-checks, and it cannot recover what the rendering COLLAPSED.
 *
 * The collapse is real and it is the reason this exists: the dump
 * spells a scratch TEMP `rN`, which is also how it spells machine
 * registers r8..r15, so `mov r10, rax` is ambiguous IN THE TEXT and
 * not ambiguous at all in the decode. `DecOp::Slot` and `DecOp::Gpr`
 * are different kinds here.
 *
 * So `decode_one` FILLS one of these and the dump is `render(it)` -
 * one decoder still, the one `scripts/disasmcheck.py` verifies against
 * objdump, and the text cannot disagree with the structure because it
 * is derived from it.
 *
 * ⛔ WHAT THIS DOES NOT PROVE, and the sabotage that showed it:
 * dropping the SIB byte from an `[r12+d]` encoding leaves a DIFFERENT,
 * well-formed instruction - our decoder and objdump agreed on it
 * perfectly over 235,639 instructions while `-rt` aborted and
 * corpus_diff went 33/35. A decoder oracle proves the dump is honest
 * about the BYTES; only the engine differential proves the bytes mean
 * what the emitter intended.
 */
struct DecOp {
    enum Kind : unsigned char {
        None,
        Gpr,        /* a 64-bit general register (`reg`)             */
        Gpr32,      /* its 32-bit spelling, eNN                      */
        Gpr8,       /* its low-byte spelling (`rex8`: dil/sil/...)   */
        Xmm,        /* an SSE register (`reg`)                       */
        Cl,         /* the ISA-fixed shift count                     */
        Mem,        /* [base + index*scale + disp]; base < 0 = none  */
        Slot,       /* a FRAME SLOT - [rbx+disp] the namer resolved  */
        Imm,        /* `imm`, rendered through the tag symbolisation */
        ImmDec,     /* `imm`, rendered as a plain decimal            */
        Rel,        /* a branch target: `imm` = the FRAGMENT offset  */
        CallRel     /* a rel32 call: `imm` = the raw displacement,
                     * masked to <helper> unless MYLANG_VDJ_ADDRS     */
    };
    Kind kind = None;
    int reg = -1;          /* Gpr/Gpr32/Gpr8/Xmm; Mem: the base      */
    int index = -1;        /* Mem, -1 = none                         */
    int scale = 1;         /* Mem                                    */
    int disp = 0;          /* Mem                                    */
    long long imm = 0;     /* Imm/ImmDec/Rel/CallRel                 */
    int slot = -1;         /* Slot: the frame slot index             */
    bool slot_type = false;/* Slot: the `.type` half of the slot     */
    bool byte_ptr = false; /* Mem/Slot printed with a `byte` size    */
    bool rex8 = false;     /* Gpr8: the uniform low-byte set         */
    /* HOW the address was ENCODED, which the rendering differs on: a
     * SIB form omits a zero displacement and carries the no-base
     * absolute, a plain one always prints its disp. Recorded rather
     * than re-derived, so render() stays a pure function of this. */
    bool via_sib = false;  /* Mem                                    */
    bool rip = false;      /* Mem: RIP-relative                      */
};

struct DecodedIns {
    unsigned off = 0;      /* fragment-relative offset               */
    unsigned len = 0;
    std::string mn;        /* the mnemonic; empty when !ok           */
    DecOp ops[3];
    int n = 0;
    bool ok = false;       /* false: the decoder does not know it    */
};

/* one collected fragment: which chunk section it belongs to, and its
 * instructions in offset order */
struct DecodedFrag {
    std::string section;
    std::vector<DecodedIns> ins;
};

/*
 * When non-null, every native fragment `disassemble_program` renders is
 * ALSO appended here. A collector rather than a second entry point
 * because the dump walk already finds every fragment and names every
 * section; duplicating that walk is how two answers start to differ.
 */
extern std::vector<DecodedFrag> *g_jit_decode_sink;

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
