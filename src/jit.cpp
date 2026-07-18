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
bool g_jit_annotate = false;

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
    const void *t_int;    /* the int Type singleton */
    const void *t_float;  /* the float Type singleton (N3) */
    const void *t_arr;    /* the array Type singleton (N4) */
    int slice_off;        /* SharedArrayObj: offset of `slice` (from payload) */
    int kind_off;         /* SharedObject: &kind - shobj */
    int data_off;         /* SharedObject: &elem_vec - shobj (the vector's
                           * _M_start is at +0, _M_finish at +8) */
    unsigned char kind_ints, kind_floats;   /* Storage enum values */
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
        LValue fprobe(EvalValue(static_cast<float_type>(1.0)), false);
        l.t_float = fprobe.get().get_type();

        /* N4: the flat-array layout, via the co-located jit_probe (which
         * reads real members, so it can't drift). Non-slice, flat-int/
         * float arrays only; everything else BAILS at runtime. */
        SharedArrayObj aprobe(SharedArrayObj::ivec_type{ 1, 2, 3 });
        LValue alv(EvalValue(std::move(aprobe)), false);
        const SharedArrayObj &arr = alv.get().get<SharedArrayObj>();
        l.t_arr = alv.get().get_type();
        /* `slice` is a public member; SharedArrayObj sits at payload+0 in
         * the EvalValue union, so &arr.slice - &arr is the byte offset. */
        l.slice_off = static_cast<int>(
            reinterpret_cast<const char *>(&arr.slice) -
            reinterpret_cast<const char *>(&arr));
        const SharedArrayObj::JitProbe jp = arr.jit_probe();
        const char *so = static_cast<const char *>(jp.shobj);
        l.kind_off = static_cast<int>(
            static_cast<const char *>(jp.kind) - so);
        l.data_off = static_cast<int>(
            static_cast<const char *>(jp.elem_vec) - so);
        l.kind_ints = static_cast<unsigned char>(
            SharedArrayObj::Storage::ints);
        l.kind_floats = static_cast<unsigned char>(
            SharedArrayObj::Storage::floats);
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

    /* ---- N3 SSE float ---- (xmm0=a/acc, xmm1=b; r8 = t_float) */
    /* movsd xmm<r>, [rdi+disp] */
    void fload(uint8_t r, int32_t d)
    {
        u8(0xF2); u8(0x0F); u8(0x10);
        u8(static_cast<uint8_t>(0x87 | (r << 3))); u32(uint32_t(d));
    }
    /* movsd [rdi+disp], xmm<r> */
    void fstore(uint8_t r, int32_t d)
    {
        u8(0xF2); u8(0x0F); u8(0x11);
        u8(static_cast<uint8_t>(0x87 | (r << 3))); u32(uint32_t(d));
    }
    /* addsd/subsd/mulsd xmm0, xmm1 (op = 0x58/0x5C/0x59) */
    void farith(uint8_t op) { u8(0xF2); u8(0x0F); u8(op); u8(0xC1); }
    /* cvtsi2sd xmm<r>, qword [rdi+disp]  (int -> double) */
    void cvt(uint8_t r, int32_t d)
    {
        u8(0xF2); u8(0x48); u8(0x0F); u8(0x2A);
        u8(static_cast<uint8_t>(0x87 | (r << 3))); u32(uint32_t(d));
    }
    /* movq xmm<r>, rax  (double bits GP -> xmm) */
    void movq_xmm(uint8_t r)
    {
        u8(0x66); u8(0x48); u8(0x0F); u8(0x6E);
        u8(static_cast<uint8_t>(0xC0 | (r << 3)));
    }
    /* ucomisd xmm<d>, xmm<s> */
    void ucomisd(uint8_t d, uint8_t s)
    {
        u8(0x66); u8(0x0F); u8(0x2E);
        u8(static_cast<uint8_t>(0xC0 | (d << 3) | s));
    }
    /* mov rax, [rdi+disp]  (a slot's type ptr) */
    void load_type(int32_t d)
    {
        u8(0x48); u8(0x8B); u8(0x87); u32(uint32_t(d));
    }
    /* cmp rax, r8 (t_float) / cmp rax, rsi (t_int) */
    void cmp_rax_r8()  { u8(0x4C); u8(0x39); u8(0xC0); }
    void cmp_rax_rsi() { u8(0x48); u8(0x39); u8(0xF0); }
    /* mov [rdi+disp], r8  (store t_float as a slot's type) */
    void store_r8_type(int32_t d)
    {
        u8(0x4C); u8(0x89); u8(0x87); u32(uint32_t(d));
    }
    /* movabs r8, imm64 (REX.WB) */
    void movabs_r8(uint64_t imm)
    {
        u8(0x49); u8(0xB8); u64(imm);
    }
    /* ---- N4 array element access ---- */
    /* mov r9, [rdi+disp] (a slot: shobj ptr or the index) */
    void mov_r9_slot(int32_t d)
    { u8(0x4C); u8(0x8B); u8(0x8F); u32(uint32_t(d)); }
    /* movabs r9, imm64 (t_arr singleton or an index literal) */
    void movabs_r9(uint64_t imm) { u8(0x49); u8(0xB9); u64(imm); }
    /* cmp rax, r9 */
    void cmp_rax_r9() { u8(0x4C); u8(0x39); u8(0xC8); }
    /* cmp byte [rdi+disp], imm8  (the slice flag) */
    void cmp_byte_rdi(int32_t d, uint8_t imm)
    { u8(0x80); u8(0xBF); u32(uint32_t(d)); u8(imm); }
    /* cmp byte [rax+disp], imm8  (the SharedObject kind) */
    void cmp_byte_rax(int32_t d, uint8_t imm)
    { u8(0x80); u8(0xB8); u32(uint32_t(d)); u8(imm); }
    /* mov <rcx|rdx>, [rax+disp]  (the flat vector's start/finish ptr) */
    void mov_rcx_rax(int32_t d)
    { u8(0x48); u8(0x8B); u8(0x88); u32(uint32_t(d)); }
    void mov_rdx_rax(int32_t d)
    { u8(0x48); u8(0x8B); u8(0x90); u32(uint32_t(d)); }
    void sub_rdx_rcx() { u8(0x48); u8(0x29); u8(0xCA); }
    void sar_rdx_3()   { u8(0x48); u8(0xC1); u8(0xFA); u8(0x03); }
    void cmp_r9_rdx()  { u8(0x49); u8(0x39); u8(0xD1); }
    /* mov rax, [rcx + r9*8]   (int element) */
    void load_elem_int()  { u8(0x4A); u8(0x8B); u8(0x04); u8(0xC9); }
    /* movsd xmm0, [rcx + r9*8]  (float element) */
    void load_elem_float()
    { u8(0xF2); u8(0x4A); u8(0x0F); u8(0x10); u8(0x04); u8(0xC9); }
    /* `<short jcc> +6; exit_pc(pc)` - bail unless the PASS condition. */
    void bail_unless(uint8_t short_pass, uint32_t pc)
    { u8(short_pass); u8(0x06); exit_pc(pc); }

    /* a short rel8 jcc/jmp with the rel patched to here later */
    size_t j8(uint8_t op) { u8(op); const size_t at = pos(); u8(0); return at; }
    void patch8(size_t at, size_t target)
    {
        b[at] = static_cast<uint8_t>(target - (at + 1));
    }
};

enum XReg : uint8_t { X0 = 0, X1 = 1 };   /* GP r8 = t_float (float
                                           * fragments); baked in the
                                           * encodings above/below */

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

/* Condition-code opcodes for `a <cmp> b` (near 0F 8x / short 7x forms)
 * and the negation. int_type is SIGNED, so the signed jcc set. */
struct CC { uint8_t near_op, short_op; };

static CC cc_for(Op o)
{
    switch (o) {
    case Op::lt:    return { 0x8C, 0x7C };   /* jl  */
    case Op::le:    return { 0x8E, 0x7E };   /* jle */
    case Op::gt:    return { 0x8F, 0x7F };   /* jg  */
    case Op::ge:    return { 0x8D, 0x7D };   /* jge */
    case Op::eq:    return { 0x84, 0x74 };   /* je  */
    default:        return { 0x85, 0x75 };   /* jne (noteq) */
    }
}

static Op cc_negate(Op o)
{
    switch (o) {
    case Op::lt:    return Op::ge;
    case Op::le:    return Op::gt;
    case Op::gt:    return Op::le;
    case Op::ge:    return Op::lt;
    case Op::eq:    return Op::noteq;
    default:        return Op::eq;           /* noteq */
    }
}

/* A branch that jumps to `target_pc` when `cc` holds (else falls
 * through). INTERNAL target (inside [begin,end)) -> a fragment-local
 * near jcc whose rel32 is patched from `label[]` afterwards (recorded in
 * `fixups`). EXTERNAL target -> `j<negated cc> +6` over a 6-byte
 * exit_pc(remap[target]) (skip the exit when NOT jumping). */
struct Fixup { size_t site; size_t target_pc; };

/* Emit "jump to target_pc when <near_op> holds". INTERNAL -> a local
 * near jcc (rel32 patched from label[]); EXTERNAL -> `<short_neg_op> +6`
 * over a 6-byte exit_pc (skip the exit when the condition does NOT hold).
 * near_op is the 2nd byte of `0F 8x`; short_neg_op the 1-byte short jcc
 * of the NEGATED condition. */
static void emit_cond_jump_raw(Emitter &e, uint8_t near_op,
                               uint8_t short_neg_op, size_t target_pc,
                               size_t begin, size_t end,
                               const std::vector<int> &remap,
                               std::vector<Fixup> &fixups)
{
    if (target_pc >= begin && target_pc < end) {
        e.u8(0x0F); e.u8(near_op);
        fixups.push_back({ e.pos(), target_pc });
        e.u32(0);
    } else {
        e.u8(short_neg_op);
        e.u8(0x06);                          /* skip the 6-byte exit */
        e.exit_pc(static_cast<uint32_t>(remap[target_pc]));
    }
}

/* Integer branch: jump to target when `cc` holds. */
static void emit_cond_jump(Emitter &e, Op cc, size_t target_pc,
                           size_t begin, size_t end,
                           const std::vector<int> &remap,
                           std::vector<Fixup> &fixups)
{
    emit_cond_jump_raw(e, cc_for(cc).near_op,
                       cc_for(cc_negate(cc)).short_op,
                       target_pc, begin, end, remap, fixups);
}

/* Float ORDERING compare via ucomisd (N3). Jump-to-target when
 * (a cmp b) is FALSE. The swap trick avoids the NaN trap: a<b is emitted
 * as b>a so an unordered (NaN) compare correctly does NOT satisfy it and
 * jumps. Returns swap (ucomisd operand order) + the jump-when-false near
 * opcode (jbe 0x86 / jb 0x82) and its negated short (ja 0x77 / jae 0x73).
 * eq/noteq are not eligible (NaN-fiddly, rare in float loops). */
struct FCmp { bool swap; uint8_t near_op, short_neg_op; };

static bool float_cmp_eligible(Op o)
{
    return o == Op::lt || o == Op::le || o == Op::gt || o == Op::ge;
}

static FCmp float_cmp(Op o)
{
    switch (o) {
    case Op::lt: return { true,  0x86, 0x77 };   /* a<b: b>a; !ja=jbe */
    case Op::le: return { true,  0x82, 0x73 };   /* a<=b: b>=a; !jae=jb */
    case Op::gt: return { false, 0x86, 0x77 };
    default:     return { false, 0x82, 0x73 };   /* ge */
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
    /* N2: control flow - intra-run branches become fragment-local
     * jumps, so a whole int loop iterates in machine code (the native
     * back edge). A branch to a pc OUTSIDE the run exits to the
     * interpreter (exit_pc). */
    case OpCode::Jump: case OpCode::JumpUnlessIntCmp:
    case OpCode::ForLoopStep: case OpCode::IntAddStep:
        return true;
    /* N3: the SSE float tier. add/sub/mul only (div/mod THROW on 0 /
     * are a libm call -> interpreted); a float READ handles an int-in-a-
     * float-slot via cvtsi2sd (matching read_float_slot) and BAILS on
     * bool/other. */
    case OpCode::FloatBin:
        return in.aop == Op::plus || in.aop == Op::minus
            || in.aop == Op::times;
    case OpCode::FloatAddRR: case OpCode::FloatSubRR:
    case OpCode::FloatMulRR: case OpCode::FloatAddRI:
    case OpCode::FloatSubRI: case OpCode::FloatMulRI:
    case OpCode::LoadImmFloat:
        return true;
    case OpCode::JumpUnlessFloatCmp:
        return float_cmp_eligible(in.aop);
    /* N4: flat int/float array element READ `a[i]`. A non-array, a
     * slice, a wrong-kind (bool/general/str) base, a negative or
     * out-of-range index all BAIL to the interpreter (exact throw/
     * caret). Unblocks the s += a[i] reduction loops. */
    case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
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

/* Load a float OPERAND into xmm<r>. A lit: movabs the double bits +
 * movq. A slot: type-dispatch matching read_float_slot - float -> movsd
 * (the fast path), int -> cvtsi2sd (promote), anything else (bool/str)
 * -> BAIL (the interpreter re-runs the op; rare in a float loop). r8
 * holds t_float, rsi holds t_int (both set once at fragment entry). */
static void emit_float_load(Emitter &e, uint8_t xr, bool is_lit,
                            float_type flit, int slot, uint32_t bail_pc)
{
    if (is_lit) {
        uint64_t bits;
        std::memcpy(&bits, &flit, sizeof bits);
        e.movabs(RAX, bits);
        e.movq_xmm(xr);
        return;
    }
    const SlotAddr a = slot_addr(slot);
    e.load_type(a.type);                 /* rax = slot type */
    e.cmp_rax_r8();                       /* == t_float ? */
    const size_t j_notf = e.j8(0x75);     /* jne -> not float */
    e.fload(xr, a.payload);               /* FAST: movsd xmm, [payload] */
    const size_t j_done1 = e.j8(0xEB);    /* jmp done */
    e.patch8(j_notf, e.pos());
    e.cmp_rax_rsi();                      /* == t_int ? */
    const size_t j_int = e.j8(0x74);      /* je -> promote */
    e.exit_pc(bail_pc);                   /* neither -> bail */
    e.patch8(j_int, e.pos());
    e.cvt(xr, a.payload);                 /* cvtsi2sd xmm, [payload] */
    e.patch8(j_done1, e.pos());
}

/* Store xmm<r> (a float result) into a scalar dst: t_float type +
 * the double payload. A ref-listed dst (should not happen for a th==f
 * scalar-writer) type-checks + bails, mirroring store_dst. */
static void emit_float_store(Emitter &e, const Chunk &ck, uint8_t xr,
                             int dst, uint32_t bail_pc)
{
    const SlotAddr a = slot_addr(dst);
    if (std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                           static_cast<int32_t>(dst))) {
        e.load_type(a.type);
        e.cmp_rax_r8();
        const size_t j_ok = e.j8(0x74);   /* je -> already float, store */
        e.exit_pc(bail_pc);
        e.patch8(j_ok, e.pos());
        e.fstore(xr, a.payload);
        return;
    }
    e.store_r8_type(a.type);              /* type = t_float */
    e.fstore(xr, a.payload);              /* payload = the double */
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

    case OpCode::LoadImmFloat:
        emit_float_load(e, X0, true, in.a_flit(), 0, pc);
        emit_float_store(e, ck, X0, in.target, pc);
        return true;

    case OpCode::FloatBin:
    case OpCode::FloatAddRR: case OpCode::FloatSubRR:
    case OpCode::FloatMulRR: case OpCode::FloatAddRI:
    case OpCode::FloatSubRI: case OpCode::FloatMulRI: {
        uint8_t fop;   /* 0x58 addsd / 0x5C subsd / 0x59 mulsd */
        switch (in.op) {
        case OpCode::FloatAddRR: case OpCode::FloatAddRI: fop = 0x58; break;
        case OpCode::FloatSubRR: case OpCode::FloatSubRI: fop = 0x5C; break;
        case OpCode::FloatMulRR: case OpCode::FloatMulRI: fop = 0x59; break;
        default:   /* FloatBin: by aop (add/sub/mul only - eligible) */
            fop = in.aop == Op::plus ? 0x58
                : in.aop == Op::minus ? 0x5C : 0x59;
            break;
        }
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc);
        emit_float_load(e, X1, in.b_is_lit(), in.b_flit(), in.b_slot(), pc);
        e.farith(fop);
        emit_float_store(e, ck, X0, in.target, pc);
        return true;
    }

    case OpCode::LoadElemInt: case OpCode::LoadElemFloat: {
        /* a[i] from a flat int/float array (N4). Navigate slot -> shobj
         * -> kind + data, bounds-check, read the raw scalar. Every
         * failing precondition BAILS (the interpreter re-runs the op with
         * its exact OutOfBounds/type handling). target2 = the array slot,
         * a() = the int index, target = the dst. r9 = t_arr then the
         * index; rcx = data start; rdx = element count. */
        const JitLayout &L = jit_layout();
        const bool is_float = in.op == OpCode::LoadElemFloat;
        const SlotAddr base = slot_addr(in.target2);

        e.load(RAX, base.type);                  /* base an array? */
        e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
        e.cmp_rax_r9();
        e.bail_unless(0x74, pc);                 /* je (== t_arr) */
        e.cmp_byte_rdi(base.payload + L.slice_off, 0);   /* not a slice? */
        e.bail_unless(0x74, pc);                 /* je (slice==0) */
        e.load(RAX, base.payload);               /* rax = shobj ptr */
        e.cmp_byte_rax(L.kind_off,               /* right flat kind? */
                       is_float ? L.kind_floats : L.kind_ints);
        e.bail_unless(0x74, pc);                 /* je (kind matches) */
        e.mov_rcx_rax(L.data_off);               /* rcx = _M_start */
        e.mov_rdx_rax(L.data_off + 8);           /* rdx = _M_finish */
        e.sub_rdx_rcx();
        e.sar_rdx_3();                            /* rdx = element count */
        if (in.a_is_lit())                        /* idx (unsigned bounds:
                                                   * a negative index is
                                                   * huge -> bails) */
            e.movabs_r9(static_cast<uint64_t>(in.a_lit()));
        else
            e.mov_r9_slot(slot_addr(in.a_slot()).payload);
        e.cmp_r9_rdx();
        e.bail_unless(0x72, pc);                 /* jb (idx < count) */
        if (is_float) {
            e.load_elem_float();                 /* movsd xmm0,[rcx+r9*8] */
            emit_float_store(e, ck, X0, in.target, pc);
        } else {
            e.load_elem_int();                   /* mov rax,[rcx+r9*8] */
            store_dst(e, ck, RAX, in.target, pc);
        }
        return true;
    }

    default:
        return false;
    }
}

/* ------------------------- the chunk pass --------------------------- */

/* Emit a control-flow op. `pc` is the OLD pc; label[]/fixups resolve
 * internal targets, remap[] the external exits. The counter/accumulator
 * stores use store_dst (their slots are scalar-writers -> not ref-listed
 * -> the branchless two-store; RSI holds t_int, set once at fragment
 * entry and preserved across the loop). */
static void emit_branch(Emitter &e, const Chunk &ck, const Instr &in,
                        uint32_t pc, size_t begin, size_t end,
                        const std::vector<int> &remap,
                        std::vector<Fixup> &fixups)
{
    switch (in.op) {

    case OpCode::Jump: {
        const size_t tgt = static_cast<size_t>(in.target);
        if (tgt >= begin && tgt < end) {
            e.u8(0xE9);
            fixups.push_back({ e.pos(), tgt });
            e.u32(0);
        } else {
            e.exit_pc(static_cast<uint32_t>(remap[in.target]));
        }
        return;
    }

    case OpCode::JumpUnlessIntCmp: {
        /* jump to target when (a cmp b) is FALSE == when negate(cmp). */
        load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        e.u8(0x48); e.u8(0x39); e.u8(0xC8);      /* cmp rax, rcx */
        emit_cond_jump(e, cc_negate(in.aop),
                       static_cast<size_t>(in.target), begin, end,
                       remap, fixups);
        return;
    }

    case OpCode::ForLoopStep: {
        /* i (target2) += step (b)  [lt/le]  or -= step  [ge/gt]; then
         * jump to target (the back edge) when (i aop bound(a)). */
        const bool up = in.aop == Op::lt || in.aop == Op::le;
        e.load(RAX, slot_addr(in.target2).payload);
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, up ? Op::plus : Op::minus);     /* rax += / -= rcx */
        store_dst(e, ck, RAX, in.target2, pc);
        load_operand(e, RCX, in.a_is_lit(), in.a_lit(), in.a_slot());
        e.u8(0x48); e.u8(0x39); e.u8(0xC8);      /* cmp rax, rcx */
        emit_cond_jump(e, in.aop, static_cast<size_t>(in.target),
                       begin, end, remap, fixups);
        return;
    }

    case OpCode::IntAddStep: {
        /* adst (a_dual_lo) += rhs (b); i (target2) ++/-- ; jump to
         * target when (i aop bound). bound = a_dual_hi (imm if a_is_lit
         * else slot). step is always 1. */
        const bool up = in.aop == Op::lt || in.aop == Op::le;
        const int adst = in.a_dual_lo();
        e.load(RAX, slot_addr(adst).payload);
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, Op::plus);
        store_dst(e, ck, RAX, adst, pc);
        e.load(RAX, slot_addr(in.target2).payload);
        e.u8(0x48); e.u8(0xFF); e.u8(up ? 0xC0 : 0xC8);   /* inc/dec rax */
        store_dst(e, ck, RAX, in.target2, pc);
        if (in.a_is_lit())
            e.movabs(RCX, static_cast<uint64_t>(
                              static_cast<int_type>(in.a_dual_hi())));
        else
            e.load(RCX, slot_addr(in.a_dual_hi()).payload);
        e.u8(0x48); e.u8(0x39); e.u8(0xC8);      /* cmp rax, rcx */
        emit_cond_jump(e, in.aop, static_cast<size_t>(in.target),
                       begin, end, remap, fixups);
        return;
    }

    case OpCode::JumpUnlessFloatCmp: {
        /* jump to target when (a cmp b) is FALSE (the ordering compares;
         * the swap trick makes NaN correctly jump - see float_cmp). */
        const FCmp fc = float_cmp(in.aop);
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc);
        emit_float_load(e, X1, in.b_is_lit(), in.b_flit(), in.b_slot(), pc);
        if (fc.swap) e.ucomisd(X1, X0); else e.ucomisd(X0, X1);
        emit_cond_jump_raw(e, fc.near_op, fc.short_neg_op,
                           static_cast<size_t>(in.target), begin, end,
                           remap, fixups);
        return;
    }

    default:
        e.u8(0xCC);   /* unreachable by selection */
        return;
    }
}

static bool op_is_branch(OpCode op)
{
    return op == OpCode::Jump || op == OpCode::JumpUnlessIntCmp
        || op == OpCode::ForLoopStep || op == OpCode::IntAddStep
        || op == OpCode::JumpUnlessFloatCmp;
}

static bool run_has_float(const Chunk &ck, size_t begin, size_t end)
{
    for (size_t pc = begin; pc < end; pc++) {
        switch (ck.code[pc].op) {
        case OpCode::FloatBin:
        case OpCode::FloatAddRR: case OpCode::FloatSubRR:
        case OpCode::FloatMulRR: case OpCode::FloatAddRI:
        case OpCode::FloatSubRI: case OpCode::FloatMulRI:
        case OpCode::LoadImmFloat: case OpCode::JumpUnlessFloatCmp:
        case OpCode::LoadElemFloat:      /* writes a float -> needs r8 */
            return true;
        default:
            break;
        }
    }
    return false;
}

struct Run { size_t begin, end; };   /* [begin, end) in OLD pc space */

static constexpr size_t MIN_RUN = 4;

void jit_compile_chunk(Chunk &chunk)
{
    if (!g_jit_enabled || chunk.code.empty())
        return;

    const size_t n = chunk.code.size();

    /* N2: maximal contiguous runs of eligible ops. Branch TARGETS inside
     * a run are fine now (a fragment-local jump); the run is entered
     * natively ONLY at its head (via the inserted EnterNative), and every
     * interior op ALSO survives as an interpreted original, so an external
     * branch to an interior pc (or a bail) simply resumes interpreted -
     * no single-entry constraint is needed for correctness. A branch to a
     * pc outside its run exits to the interpreter (exit_pc). */
    std::vector<Run> runs;
    size_t i = 0;
    while (i < n) {
        if (!jit_op_eligible(chunk.code[i])) { i++; continue; }
        size_t j = i + 1;
        while (j < n && jit_op_eligible(chunk.code[j]))
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

    /* Emit the fragments. Per run: RSI = t_int once at entry (preserved
     * across the loop - no op clobbers it - so the native back edge, a
     * jump to label[begin] AFTER this movabs, keeps it live); record each
     * op's fragment offset in label[]; branch ops emit local jumps
     * (patched from label[] after) or exit_pc; a trailing exit_pc handles
     * fall-through off the end. */
    Emitter e;
    std::vector<size_t> frag_off(runs.size());
    for (size_t r = 0; r < runs.size(); r++) {
        const size_t begin = runs[r].begin, end = runs[r].end;
        frag_off[r] = e.pos();
        e.movabs(RSI, reinterpret_cast<uint64_t>(jit_layout().t_int));
        if (run_has_float(chunk, begin, end))     /* r8 = t_float (N3) */
            e.movabs_r8(reinterpret_cast<uint64_t>(jit_layout().t_float));

        std::vector<size_t> label(end - begin, 0);
        std::vector<Fixup> fixups;
        std::vector<NativeCode::OpMark> marks;
        for (size_t pc = begin; pc < end; pc++) {
            label[pc - begin] = e.pos();
            if (g_jit_annotate)
                marks.push_back({ static_cast<uint32_t>(e.pos() - frag_off[r]),
                                  static_cast<uint32_t>(remap[pc]) });
            const Instr &in = chunk.code[pc];
            if (op_is_branch(in.op)) {
                emit_branch(e, chunk, in, static_cast<uint32_t>(remap[pc]),
                            begin, end, remap, fixups);
            } else if (!emit_op(e, chunk, in,
                                static_cast<uint32_t>(remap[pc]))) {
                e.b.clear();
                return;                    /* selection bug: give up */
            }
        }
        e.exit_pc(static_cast<uint32_t>(remap[end]));   /* fall-through */

        for (const Fixup &f : fixups) {    /* patch internal jumps */
            const size_t dst = label[f.target_pc - begin];
            e.patch32(f.site,
                      static_cast<uint32_t>(dst - (f.site + 4)));
        }
        if (g_jit_annotate)
            chunk.native.frags.push_back(
                { static_cast<uint32_t>(frag_off[r]), 0, std::move(marks) });
        g_jit_frags++;
    }

    /* fill each fragment's byte length (start of the next, or the end) */
    if (g_jit_annotate) {
        for (size_t r = 0; r < chunk.native.frags.size(); r++) {
            const uint32_t nxt = r + 1 < frag_off.size()
                ? static_cast<uint32_t>(frag_off[r + 1])
                : static_cast<uint32_t>(e.b.size());
            chunk.native.frags[r].len = nxt - chunk.native.frags[r].start;
        }
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
