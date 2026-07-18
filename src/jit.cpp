/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Native x86-64 AOT (N0/N1) - see jit.h and plans/native-aot.md.
 *
 * N1 scope: STRAIGHT-LINE runs of the never-throwing int tier (the B1/B2
 * specialized arithmetic + IntModRI/IntAddModRI/LoadImmInt). No branches
 * inside a run yet (N2 adds them + the native back edge); a branch TARGET
 * splits a run, so a fragment has exactly one entry (its head) and every
 * interior pc's original Instr stays valid for a bail resume.
 *
 * THE THREE CONTRACTS (the correctness core - plans/native-aot.md):
 *  1. Fragments NEVER throw and never call anything that can: they are
 *     frameless leaves with no unwind tables. Every exceptional condition
 *     (a negative shift count, an excluded idiv combination) BAILS -
 *     `return pc` - and the interpreter re-executes that op, throwing
 *     with the exact loc/caret.
 *  2. Reads are the release interpreter's own raw proven-type loads
 *     (a th==i operand holds int - or bool, whose ctor zeroes the full
 *     payload word, so the raw int read is 0/1 exactly like the
 *     interpreter's readers).
 *  3. Writes: a dst OUTSIDE Chunk::ref_slots can only ever hold TRIVIAL
 *     values (the audited scalar-writer set + the never-reused-slot
 *     invariant), so the write is TWO UNCONDITIONAL STORES (type
 *     singleton + payload) - branchless, and exactly what LValue::put's
 *     trivial fast path does. A dst ON the ref list (a reused temp that
 *     may CURRENTLY hold a reference - releasing it needs the C++ path)
 *     gets a type-check + bail instead.
 */

#include "jit.h"
#include "bytecode.h"
#include "evalvalue.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__x86_64__) && !defined(_WIN32)
#  define ML_JIT_SUPPORTED 1
#  include <sys/mman.h>
#  include <unistd.h>
#  include <cstdlib>
#else
#  define ML_JIT_SUPPORTED 0
#endif

unsigned long g_jit_frags = 0;

#if ML_JIT_SUPPORTED
static bool jit_default_enabled()
{
    const char *e = getenv("MYLANG_JIT");
    return !(e && e[0] == '0' && e[1] == 0);
}
bool g_jit_enabled = jit_default_enabled();
#else
bool g_jit_enabled = false;
#endif

/* Exempt the indirect call to JIT code from UBSan's function-type
 * check (-fsanitize=function), which reads a CFI signature from before
 * the target - absent in a raw fragment, so the read faults on the
 * guard page below `base`. clang spells it no_sanitize("function");
 * gcc has no such value, so disable all of UBSan for this one-line
 * helper (no_sanitize_undefined). Both are no-ops without the
 * sanitizer. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#elif defined(__GNUC__)
__attribute__((no_sanitize_undefined))
#endif
size_t jit_enter(const void *frag, void *slots)
{
    typedef size_t (*NativeFrag)(void *);
    return reinterpret_cast<NativeFrag>(const_cast<void *>(frag))(slots);
}

void NativeCode::release() noexcept
{
#if ML_JIT_SUPPORTED
    if (base)
        munmap(base, len);
#endif
    base = nullptr;
    len = 0;
}

#if ML_JIT_SUPPORTED

/* ---------------- layout probes (baked as immediates) ---------------- */

static_assert(sizeof(LValue) == 48, "the emitter's slot stride");

struct JitLayout {
    long off_payload;   /* &slot.val.<union> - &slot  (ival/fval at +0) */
    long off_type;      /* &slot.val.type    - &slot */
    const void *t_int;  /* the int Type singleton */
};

/* Runtime-computed via public accessors (LValue mixes access specifiers,
 * so offsetof on it is not portable): getval<T>() returns a reference to
 * the union payload; EvalValue's own offsets are class-internal facts
 * exposed by jit_payload_off/jit_type_off. */
static const JitLayout &jit_layout()
{
    static JitLayout L = [] {
        JitLayout l{};
        LValue probe(EvalValue(static_cast<int_type>(1)), false);
        const long val_off =
            reinterpret_cast<const char *>(&probe.getval<int_type>())
            - reinterpret_cast<const char *>(&probe)
            - static_cast<long>(EvalValue::jit_payload_off());
        l.off_payload = val_off
            + static_cast<long>(EvalValue::jit_payload_off());
        l.off_type = val_off + static_cast<long>(EvalValue::jit_type_off());
        l.t_int = probe.get().get_type();
        return l;
    }();
    return L;
}

/* ---------------------------- the emitter ---------------------------- */

/*
 * Register plan (System V, caller-saved only - no prologue/epilogue):
 *   rdi = the slot window base (the ABI argument; never clobbered)
 *   rsi = the int Type singleton (loaded once per fragment)
 *   rax = the accumulator (op result; also the returned resume pc)
 *   rcx = the second operand / shift count / idiv divisor
 *   rdx = idiv's remainder (clobbered by cqo/idiv only)
 */
struct Emitter {
    std::vector<uint8_t> b;

    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v)
    {
        for (int i = 0; i < 4; i++)
            b.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void u64(uint64_t v)
    {
        for (int i = 0; i < 8; i++)
            b.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    size_t pos() const { return b.size(); }
    void patch32(size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; i++)
            b[at + i] = static_cast<uint8_t>(v >> (i * 8));
    }

    /* mov <reg64>, [rdi + disp32]   (reg encoded in ModRM.reg) */
    void load(uint8_t reg, int32_t disp)
    {
        u8(0x48); u8(0x8B); u8(static_cast<uint8_t>(0x87 | (reg << 3)));
        u32(static_cast<uint32_t>(disp));
    }
    /* mov [rdi + disp32], <reg64> */
    void store(uint8_t reg, int32_t disp)
    {
        u8(0x48); u8(0x89); u8(static_cast<uint8_t>(0x87 | (reg << 3)));
        u32(static_cast<uint32_t>(disp));
    }
    /* movabs <reg64>, imm64 */
    void movabs(uint8_t reg, uint64_t imm)
    {
        u8(0x48); u8(static_cast<uint8_t>(0xB8 | reg)); u64(imm);
    }
    /* mov eax, imm32; ret   (the exit/bail sequence: resume pc out) */
    void exit_pc(uint32_t pc) { u8(0xB8); u32(pc); u8(0xC3); }
};

enum Reg : uint8_t { RAX = 0, RCX = 1, RDX = 2, RSI = 6 };

/* rax OP= rcx (reg-reg forms; 0x48 REX.W + opcode + ModRM(rcx->rax)) */
static void op_rr(Emitter &e, Op aop)
{
    switch (aop) {
    case Op::plus:  e.u8(0x48); e.u8(0x01); e.u8(0xC8); break;   /* add  */
    case Op::minus: e.u8(0x48); e.u8(0x29); e.u8(0xC8); break;   /* sub  */
    case Op::times: e.u8(0x48); e.u8(0x0F); e.u8(0xAF);
                    e.u8(0xC1);                         break;   /* imul */
    case Op::band:  e.u8(0x48); e.u8(0x21); e.u8(0xC8); break;   /* and  */
    case Op::bor:   e.u8(0x48); e.u8(0x09); e.u8(0xC8); break;   /* or   */
    case Op::bxor:  e.u8(0x48); e.u8(0x31); e.u8(0xC8); break;   /* xor  */
    default:        e.u8(0xCC); /* unreachable by selection */   break;
    }
}

/* -------------------------- run selection --------------------------- */

static bool imm_shift_ok(int_type v) { return v >= 0; }

static bool jit_op_eligible(const Instr &in)
{
    switch (in.op) {
    case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
    case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
    case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
    case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI:
    case OpCode::IntShlRR: case OpCode::IntShrRR:
    case OpCode::LoadImmInt:
        return true;
    case OpCode::IntShlRI: case OpCode::IntShrRI:
        /* a negative imm count THROWS - leave the op interpreted */
        return imm_shift_ok(in.b_lit());
    case OpCode::IntModRI:
        /* imm nonzero by selection; -1 would make idiv trap on
         * INT64_MIN % -1 (the interpreter's -fwrapv path defines it) */
        return in.b_lit() != 0 && in.b_lit() != -1;
    case OpCode::IntAddModRI:
        return in.target2 != 0 && in.target2 != -1;
    default:
        return false;
    }
}

/* ------------------------ fragment compiler ------------------------- */

struct SlotAddr {
    int32_t payload;
    int32_t type;
};

static SlotAddr slot_addr(int slot)
{
    const JitLayout &L = jit_layout();
    const long base = static_cast<long>(slot) * sizeof(LValue);
    return { static_cast<int32_t>(base + L.off_payload),
             static_cast<int32_t>(base + L.off_type) };
}

/* Load operand a/b of `in` into `reg` (slot load or immediate). */
static void load_operand(Emitter &e, uint8_t reg, bool is_lit,
                         int_type lit, int slot)
{
    if (is_lit)
        e.movabs(reg, static_cast<uint64_t>(lit));
    else
        e.load(reg, slot_addr(slot).payload);
}

/*
 * Store rax (or rdx for the idiv remainder) into the dst slot. A dst on
 * the chunk's ref list may CURRENTLY hold a reference (a reused temp) -
 * overwriting it raw would leak the handle / dangle a slice
 * registration, so those get: cmp [type], rsi; jne bail(pc). Everything
 * else is the branchless two-store (contract 3).
 */
static void store_dst(Emitter &e, const Chunk &ck, uint8_t src_reg,
                      int dst, uint32_t bail_pc)
{
    const SlotAddr a = slot_addr(dst);
    const bool may_ref =
        std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                           static_cast<int32_t>(dst));
    if (may_ref) {
        /* cmp [rdi + type], rsi ; jne bail */
        e.u8(0x48); e.u8(0x39); e.u8(0xB7);
        e.u32(static_cast<uint32_t>(a.type));
        e.u8(0x0F); e.u8(0x85);
        const size_t rel = e.pos();
        e.u32(0);
        /* stores, then skip the bail */
        e.store(src_reg, a.payload);
        e.u8(0xE9);                       /* jmp past the bail stub */
        const size_t skip = e.pos();
        e.u32(0);
        e.patch32(rel, static_cast<uint32_t>(e.pos() - (rel + 4)));
        e.exit_pc(bail_pc);
        e.patch32(skip, static_cast<uint32_t>(e.pos() - (skip + 4)));
        return;
    }
    e.store(RSI, a.type);        /* the int Type singleton */
    e.store(src_reg, a.payload);
}

/* Emit one op; returns false if (unexpectedly) unhandled. */
static bool emit_op(Emitter &e, const Chunk &ck, const Instr &in,
                    uint32_t pc)
{
    switch (in.op) {

    case OpCode::LoadImmInt:
        e.movabs(RAX, static_cast<uint64_t>(in.a_lit()));
        store_dst(e, ck, RAX, in.target, pc);
        return true;

    case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
    case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
    case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
    case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI: {
        Op aop;
        switch (in.op) {
        case OpCode::IntAddRR: case OpCode::IntAddRI: aop = Op::plus;  break;
        case OpCode::IntSubRR: case OpCode::IntSubRI: aop = Op::minus; break;
        case OpCode::IntMulRR: case OpCode::IntMulRI: aop = Op::times; break;
        case OpCode::IntAndRR: case OpCode::IntAndRI: aop = Op::band;  break;
        case OpCode::IntOrRR:  case OpCode::IntOrRI:  aop = Op::bor;   break;
        default:                                      aop = Op::bxor;  break;
        }
        e.load(RAX, slot_addr(in.a_slot()).payload);
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, aop);
        store_dst(e, ck, RAX, in.target, pc);
        return true;
    }

    case OpCode::IntShlRR: case OpCode::IntShrRR:
    case OpCode::IntShlRI: case OpCode::IntShrRI: {
        const bool shl =
            in.op == OpCode::IntShlRR || in.op == OpCode::IntShlRI;
        e.load(RAX, slot_addr(in.a_slot()).payload);
        if (in.b_is_lit()) {
            /* imm count: >= 0 by selection; saturate at compile time */
            const int_type c = in.b_lit();
            if (c >= 64) {
                if (shl) {
                    e.u8(0x31); e.u8(0xC0);              /* xor eax,eax */
                } else {
                    e.u8(0x48); e.u8(0xC1); e.u8(0xF8);
                    e.u8(63);                            /* sar rax,63 */
                }
            } else if (c > 0) {
                e.u8(0x48); e.u8(0xC1);
                e.u8(shl ? 0xE0 : 0xF8);                 /* shl/sar rax,c */
                e.u8(static_cast<uint8_t>(c));
            }
        } else {
            e.load(RCX, slot_addr(in.b_slot()).payload);
            /* test rcx,rcx; js bail (negative count throws) */
            e.u8(0x48); e.u8(0x85); e.u8(0xC9);
            e.u8(0x0F); e.u8(0x88);
            const size_t js = e.pos(); e.u32(0);
            /* cmp rcx,64; jl Lnorm */
            e.u8(0x48); e.u8(0x83); e.u8(0xF9); e.u8(64);
            e.u8(0x0F); e.u8(0x8C);
            const size_t jl = e.pos(); e.u32(0);
            if (shl) {
                e.u8(0x31); e.u8(0xC0);                  /* xor eax,eax */
            } else {
                e.u8(0x48); e.u8(0xC1); e.u8(0xF8); e.u8(63);
            }
            e.u8(0xE9);
            const size_t jdone = e.pos(); e.u32(0);
            e.patch32(js, static_cast<uint32_t>(e.pos() - (js + 4)));
            e.exit_pc(pc);                               /* the bail */
            e.patch32(jl, static_cast<uint32_t>(e.pos() - (jl + 4)));
            e.u8(0x48); e.u8(0xD3);
            e.u8(shl ? 0xE0 : 0xF8);                     /* shl/sar rax,cl */
            e.patch32(jdone, static_cast<uint32_t>(e.pos() - (jdone + 4)));
        }
        store_dst(e, ck, RAX, in.target, pc);
        return true;
    }

    case OpCode::IntModRI: {
        e.load(RAX, slot_addr(in.a_slot()).payload);
        e.movabs(RCX, static_cast<uint64_t>(in.b_lit()));
        e.u8(0x48); e.u8(0x99);                          /* cqo */
        e.u8(0x48); e.u8(0xF7); e.u8(0xF9);              /* idiv rcx */
        store_dst(e, ck, RDX, in.target, pc);            /* remainder */
        return true;
    }

    case OpCode::IntAddModRI: {
        e.load(RAX, slot_addr(in.a_slot()).payload);
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, Op::plus);
        e.movabs(RCX, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.u8(0x48); e.u8(0x99);                          /* cqo */
        e.u8(0x48); e.u8(0xF7); e.u8(0xF9);              /* idiv rcx */
        store_dst(e, ck, RDX, in.target, pc);
        return true;
    }

    default:
        return false;
    }
}

/* ------------------------- the chunk pass --------------------------- */

struct Run { size_t begin, end; };   /* [begin, end) in OLD pc space */

static constexpr size_t MIN_RUN = 4;

void jit_compile_chunk(Chunk &chunk)
{
    if (!g_jit_enabled || chunk.code.empty())
        return;

    const size_t n = chunk.code.size();

    /* Branch targets (any pc field) split runs: a fragment has exactly
     * one entry, its head. visit_pc_fields is the audited enumeration -
     * shared with the peephole via codegen.cpp; a local copy of the walk
     * would drift, so it lives in bytecode.h... it does not: re-walk the
     * few branching ops here against the SAME field list. */
    std::vector<char> is_tgt(n + 1, 0);
    for (const Instr &in : chunk.code) {
        switch (in.op) {
        case OpCode::Jump:
        case OpCode::JumpUnlessIntCmp:
        case OpCode::JumpUnlessFloatCmp:
        case OpCode::JumpUnlessTrueV:
        case OpCode::JumpIfNotNoneV:
        case OpCode::ForLoopStep:
        case OpCode::DictIterNext:
        case OpCode::ForeachDynNext:
        case OpCode::CatchTest:
        case OpCode::PushHandler:
        case OpCode::JumpUnlessElemInt:
        case OpCode::IntAddStep:
        case OpCode::ForStepElemInt:
            if (in.target >= 0 && static_cast<size_t>(in.target) <= n)
                is_tgt[in.target] = 1;
            break;
        default:
            break;
        }
    }

    /* Maximal straight-line runs of eligible ops; an interior branch
     * target truncates (the pc becomes a new run's head). */
    std::vector<Run> runs;
    size_t i = 0;
    while (i < n) {
        if (!jit_op_eligible(chunk.code[i])) { i++; continue; }
        size_t j = i + 1;
        while (j < n && jit_op_eligible(chunk.code[j]) && !is_tgt[j])
            j++;
        if (j - i >= MIN_RUN)
            runs.push_back({i, j});
        i = j;
    }
    if (runs.empty())
        return;

    /* pc remap: every run head gains one inserted EnterNative. */
    std::vector<int> remap(n + 1);
    {
        size_t r = 0;
        int shift = 0;
        for (size_t pc = 0; pc <= n; pc++) {
            if (r < runs.size() && pc == runs[r].begin) { shift++; r++; }
            remap[pc] = static_cast<int>(pc) + shift;
        }
    }

    /* Emit the fragments (exits/bails in NEW pc space). */
    Emitter e;
    std::vector<size_t> frag_off(runs.size());
    for (size_t r = 0; r < runs.size(); r++) {
        frag_off[r] = e.pos();
        e.movabs(RSI, reinterpret_cast<uint64_t>(jit_layout().t_int));
        for (size_t pc = runs[r].begin; pc < runs[r].end; pc++) {
            if (!emit_op(e, chunk, chunk.code[pc],
                         static_cast<uint32_t>(remap[pc])))
                { e.b.clear(); return; }   /* selection bug: give up */
        }
        e.exit_pc(static_cast<uint32_t>(remap[runs[r].end]));
        g_jit_frags++;
    }

    /* Rebuild the code vector with the EnterNative heads inserted and
     * every pc field remapped; then remap the pc-keyed side tables. */
    std::vector<Instr> nc;
    nc.reserve(n + runs.size());
    {
        size_t r = 0;
        for (size_t pc = 0; pc < n; pc++) {
            if (r < runs.size() && pc == runs[r].begin) {
                Instr en;
                en.op = OpCode::EnterNative;
                Operand off;
                off.is_lit = true;
                off.lit_kind = Operand::LitKind::i;
                off.lit = static_cast<int_type>(frag_off[r]);
                en.set_a(off);
                nc.push_back(en);
                r++;
            }
            Instr in = chunk.code[pc];
            switch (in.op) {
            case OpCode::Jump:
            case OpCode::JumpUnlessIntCmp:
            case OpCode::JumpUnlessFloatCmp:
            case OpCode::JumpUnlessTrueV:
            case OpCode::JumpIfNotNoneV:
            case OpCode::ForLoopStep:
            case OpCode::DictIterNext:
            case OpCode::ForeachDynNext:
            case OpCode::CatchTest:
            case OpCode::PushHandler:
            case OpCode::JumpUnlessElemInt:
            case OpCode::IntAddStep:
            case OpCode::ForStepElemInt:
                if (in.target >= 0 && static_cast<size_t>(in.target) <= n)
                    in.target = remap[in.target];
                break;
            default:
                break;
            }
            nc.push_back(in);
        }
    }
    chunk.code = std::move(nc);

    for (auto &l : chunk.locs)
        l.pc = static_cast<uint32_t>(remap[l.pc]);
    for (auto &ic : chunk.inline_ctxs)
        ic.pc = static_cast<uint32_t>(remap[ic.pc]);

    /* Map RW, copy, flip RX (strict W^X). */
    const size_t len = e.b.size();
    void *mem = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return;   /* out of memory: the EnterNative ops... must NOT stay */
    std::memcpy(mem, e.b.data(), len);
    if (mprotect(mem, len, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, len);
        return;
    }
    chunk.native.base = mem;
    chunk.native.len = len;
}

#else   /* !ML_JIT_SUPPORTED */

void jit_compile_chunk(Chunk &)
{
}

#endif
