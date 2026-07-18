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
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cmath>
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
    int type_t_off;       /* offset of Type::t (the TypeE enum) within a Type */
    int t_str_val;        /* Type::t_str: types >= this hold a REFERENCE */
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
        /* Type::t offset + the t_str boundary: a slot whose current type has
         * t >= t_str holds a REFERENCE (needs a C++ release before overwrite);
         * < t_str is a trivial value (overwrite in place). */
        l.type_t_off = static_cast<int>(
            reinterpret_cast<const char *>(
                &static_cast<const Type *>(l.t_int)->t)
            - reinterpret_cast<const char *>(l.t_int));
        l.t_str_val = static_cast<int>(Type::t_str);
        return l;
    }();
    return L;
}

/* -vdj: the Type-tag singletons the emitter bakes (see jit.h). */
void jit_type_singletons(const void *&ti, const void *&tf, const void *&ta)
{
    const JitLayout &L = jit_layout();
    ti = L.t_int; tf = L.t_float; ta = L.t_arr;
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

    /* N5 register cache: a hot int slot pinned in a GP register for the
     * fragment's life (load once at entry, flush type+payload at EVERY
     * exit/bail). reg is a caller-saved GP (r10/r11) - free, so no
     * prologue. */
    struct CacheEnt { int slot; int32_t payload, type; uint8_t reg; };
    std::vector<CacheEnt> cache;

    /* N6a: a CALL-site relocation. A libm call reserves a fixed 12-byte
     * slot (offset `off` in `b`) + records the target; after the buffer is
     * placed (mmap gives the final base), patch_call rewrites the slot as a
     * 5-byte `E8 rel32` DIRECT call (+ nop padding) when the target is in
     * rel32 range - the default anon mmap lands next to libm, so it is - or
     * the 12-byte indirect `movabs rax,imm; call rax` fallback otherwise. */
    struct CallReloc { size_t off; const void *fn; };
    std::vector<CallReloc> call_relocs;
    int creg(int slot) const
    {
        for (const CacheEnt &c : cache)
            if (c.slot == slot)
                return c.reg;
        return -1;
    }

    /* mov <reg64>, [rdi + disp32]  (reg 0-15; REX.R for r8-r15) */
    void load(uint8_t reg, int32_t disp)
    {
        u8(static_cast<uint8_t>(0x48 | (reg >= 8 ? 0x04 : 0)));
        u8(0x8B);
        u8(static_cast<uint8_t>(0x87 | ((reg & 7) << 3)));
        u32(static_cast<uint32_t>(disp));
    }
    /* mov [rdi + disp32], <reg64>  (reg 0-15) */
    void store(uint8_t reg, int32_t disp)
    {
        u8(static_cast<uint8_t>(0x48 | (reg >= 8 ? 0x04 : 0)));
        u8(0x89);
        u8(static_cast<uint8_t>(0x87 | ((reg & 7) << 3)));
        u32(static_cast<uint32_t>(disp));
    }
    /* mov <dst>, <src>  (GP reg-reg, both 0-15) */
    void mov_rr(uint8_t dst, uint8_t src)
    {
        u8(static_cast<uint8_t>(0x48 | (src >= 8 ? 0x04 : 0)
                                | (dst >= 8 ? 0x01 : 0)));
        u8(0x89);
        u8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }
    /* Write every cached slot back to memory (type = t_int in rsi, then
     * the payload). Emitted before each exit/bail so the interpreter
     * resumes with the correct slot values. */
    void flush_cache()
    {
        for (const CacheEnt &c : cache) {
            store(6 /*rsi = t_int*/, c.type);
            store(c.reg, c.payload);
        }
    }
    /* movabs <reg64>, imm64 */
    void movabs(uint8_t reg, uint64_t imm)
    {
        u8(0x48); u8(static_cast<uint8_t>(0xB8 | reg)); u64(imm);
    }
    /* mov eax, imm32; ret - the exit/bail: flush the register cache
     * (N5) so the interpreter sees the up-to-date slots, then return
     * the resume pc. */
    void exit_pc(uint32_t pc)
    { flush_cache(); u8(0xB8); u32(pc); u8(0xC3); }

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
    /* sqrtsd xmm<d>, xmm<s>  (SSE2) */
    void sqrtsd(uint8_t d, uint8_t s)
    { u8(0xF2); u8(0x0F); u8(0x51);
      u8(static_cast<uint8_t>(0xC0 | (d << 3) | s)); }
    /* push/pop a GP reg (0x50+r / 0x58+r; REX.B for r8..r15) */
    void push_reg(uint8_t r) { if (r >= 8) u8(0x41); u8(0x50 | (r & 7)); }
    void pop_reg(uint8_t r)  { if (r >= 8) u8(0x41); u8(0x58 | (r & 7)); }
    void call_rax() { u8(0xFF); u8(0xD0); }   /* call rax (indirect) */
    /* lea rdi, [rdi + disp32]  (rdi = &frame->slots[slot], a helper arg) */
    void lea_rdi(int32_t d) { u8(0x48); u8(0x8D); u8(0xBF); u32(uint32_t(d)); }
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
    {
        const size_t sk = j8(short_pass);   /* jcc + rel8, skip exit_pc */
        exit_pc(pc);                        /* may be > 6 bytes (N5 flush) */
        patch8(sk, pos());
    }

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

enum Reg : uint8_t { RAX = 0, RCX = 1, RDX = 2, RSI = 6, RDI = 7 };

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
        const size_t sk = e.j8(short_neg_op);  /* jcc + rel8, skip the
                                                * exit_pc (grows past 6
                                                * bytes with the N5 flush) */
        e.exit_pc(static_cast<uint32_t>(remap[target_pc]));
        e.patch8(sk, e.pos());
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

/* N6a native math builtins (MathFnV). Classify a selector by how it lowers:
 *   MK_NONE = leave interpreted; MK_SSE = a pure SSE2 op (no call);
 *   MK_CALL = a libm call (sin/cos/.../pow) - the reg-save/align path. */
enum MathKind { MK_NONE = 0, MK_SSE, MK_CALL };

static MathKind mathfn_kind(MathFn fn)
{
    switch (fn) {
    case MathFn::sqrt_:                /* sqrtsd */
    case MathFn::tofloat_:             /* float(x): the float read promotes */
        return MK_SSE;
    case MathFn::cbrt_:  case MathFn::sin_:   case MathFn::cos_:
    case MathFn::tan_:   case MathFn::asin_:  case MathFn::acos_:
    case MathFn::atan_:  case MathFn::exp_:   case MathFn::exp2_:
    case MathFn::log_:   case MathFn::log2_:  case MathFn::log10_:
    case MathFn::pow_:                  /* a libm CALL (2-arg for pow) */
        return MK_CALL;
    default:
        return MK_NONE;   /* ceil/floor/trunc/fabs (roundsd = SSE4.1): later */
    }
}

/* The libm entry point for a MK_CALL selector. The DOUBLE overloads (the
 * C `::name(double)` functions) - baked as an immediate + `call rax`. pow
 * is the only 2-arg one (x in xmm0, y in xmm1). */
static const void *jit_math_fn_ptr(MathFn fn)
{
    switch (fn) {
    case MathFn::cbrt_:  return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::cbrt));
    case MathFn::sin_:   return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::sin));
    case MathFn::cos_:   return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::cos));
    case MathFn::tan_:   return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::tan));
    case MathFn::asin_:  return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::asin));
    case MathFn::acos_:  return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::acos));
    case MathFn::atan_:  return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::atan));
    case MathFn::exp_:   return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::exp));
    case MathFn::exp2_:  return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::exp2));
    case MathFn::log_:   return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::log));
    case MathFn::log2_:  return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::log2));
    case MathFn::log10_: return reinterpret_cast<const void *>(
                                    static_cast<double (*)(double)>(std::log10));
    case MathFn::pow_:   return reinterpret_cast<const void *>(
                            static_cast<double (*)(double, double)>(std::pow));
    default:             return nullptr;   /* not a MK_CALL selector */
    }
}

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
    /* Approach A: a flat-array element STORE `a[i] = v` / `a[i] OP= v`.
     * Marshals base/index/value and CALLS jit_store_elem_int/float (the
     * interpreter's EXACT store body - COW, bounds, universal fallback);
     * keeps the loop native instead of splitting at the store. A global /
     * capture base (in.target != 0) isn't in the slot window -> interpreted.*/
    case OpCode::StoreElemInt: case OpCode::StoreElemFloat:
        return in.target == 0;
    case OpCode::IntShlRI: case OpCode::IntShrRI:
        /* a negative imm count THROWS - leave the op interpreted */
        return imm_shift_ok(in.b_lit());
    case OpCode::IntModRI:
        /* imm nonzero by selection; -1 would make idiv trap on
         * INT64_MIN % -1 (the interpreter's -fwrapv path defines it) */
        return in.b_lit() != 0 && in.b_lit() != -1;
    case OpCode::IntAddModRI:
        return in.target2 != 0 && in.target2 != -1;
    /* N6a: a typed math builtin (MathFnV). A pure-SSE selector (sqrt /
     * float-cast) lowers with no call; the libm-call selectors are added
     * in increment B. An unsupported selector stays interpreted. */
    case OpCode::MathFnV:
        return mathfn_kind(static_cast<MathFn>(in.target2)) != MK_NONE;

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
/* Read a slot's int payload into `dst` - from its CACHE register (N5)
 * if pinned, else from memory. */
static void read_slot(Emitter &e, uint8_t dst, int slot)
{
    const int cr = e.creg(slot);
    if (cr >= 0)
        e.mov_rr(dst, static_cast<uint8_t>(cr));
    else
        e.load(dst, slot_addr(slot).payload);
}

static void load_operand(Emitter &e, uint8_t reg, bool is_lit,
                         int_type lit, int slot)
{
    if (is_lit)
        e.movabs(reg, static_cast<uint64_t>(lit));
    else
        read_slot(e, reg, slot);
}

/* Ref-listed store guard (approach A): a reused temp on the chunk's ref
 * list may CURRENTLY hold a reference (`type->t >= t_str`), whose release
 * needs C++. The store is therefore: TEST the current type; a REFERENCE ->
 * call a noexcept put helper (release + store, stay native); a TRIVIAL
 * current value (none/int/float/bool, `< t_str`) -> the fast two-store. It
 * NEVER bails - a bail on the trivial case dropped the whole loop to the
 * interpreter (the bench-40 bug: iteration 1 finds the temp holding a
 * stale ref). emit_ref_check emits the test and returns the `jb` to be
 * patched to the fast path. Uses rcx as scratch (dead at every store). */
static size_t emit_ref_check(Emitter &e, int32_t type_off)
{
    const JitLayout &L = jit_layout();
    e.load(RCX, type_off);                    /* rcx = current Type* */
    e.u8(0x8B); e.u8(0x89);                    /* mov ecx, [rcx + type_t_off] */
    e.u32(static_cast<uint32_t>(L.type_t_off));
    e.u8(0x81); e.u8(0xF9);                    /* cmp ecx, t_str_val */
    e.u32(static_cast<uint32_t>(L.t_str_val));
    return e.j8(0x72);                         /* jb -> fast (trivial value) */
}

/* Approach-A slow-path helpers: release the slot's current value and store
 * a scalar (int / float). Called from native ONLY on the ref path (cold,
 * once per reused temp). noexcept: a scalar put never throws. */
static void jit_put_int(LValue *lv, int_type v) noexcept
{
    lv->put(EvalValue(v));
}

/* A helper CALL clobbers every caller-saved GP reg. The fragment relies on
 * rdi (slots base), rsi (t_int), r8 (t_float) AND the N5 cache regs
 * r10/r11 (which hold a hot slot's LIVE in-register value). rdi + the cache
 * regs are PUSHED across the call and popped after; rsi/r8 are constant
 * singletons, re-materialised. (rax/rcx/rdx are per-op scratch - the store
 * is the last thing in an op, so the next op reloads them.) 16-alignment:
 * entry rsp % 16 == 8, so an ODD number of pushes lands aligned; 1 rdi +
 * ncache pushes need a pad iff ncache is odd.
 *
 * MISSING THIS was a correctness bug: the int-store helper clobbered a
 * cached accumulator/counter (r10/r11) in an int run (a float/libm run
 * caches nothing, so it was masked), a nested_fuzz find. */
static void emit_call_prologue(Emitter &e)
{
    e.push_reg(RDI);
    for (const Emitter::CacheEnt &c : e.cache)
        e.push_reg(c.reg);
    if (e.cache.size() % 2 == 1)               /* pad: 1+ncache even */
        { e.u8(0x48); e.u8(0x83); e.u8(0xEC); e.u8(0x08); }   /* sub rsp,8 */
}

static void emit_call_epilogue(Emitter &e)
{
    if (e.cache.size() % 2 == 1)
        { e.u8(0x48); e.u8(0x83); e.u8(0xC4); e.u8(0x08); }   /* add rsp,8 */
    for (size_t i = e.cache.size(); i-- > 0; )
        e.pop_reg(e.cache[i].reg);
    e.pop_reg(RDI);
    e.movabs(RSI, reinterpret_cast<uint64_t>(jit_layout().t_int));
    e.movabs_r8(reinterpret_cast<uint64_t>(jit_layout().t_float));
}

/* Emit a call to the INT put helper: rdi = &frame->slots[slot], rsi = the
 * int value (src_reg). rdi still = the slots base after the prologue's
 * pushes, so lea computes &slot; src_reg (rax/rdx) is not among the saved
 * regs, so it survives. */
static void emit_put_int_call(Emitter &e, const void *fn, int slot,
                              uint8_t src_reg)
{
    emit_call_prologue(e);
    e.mov_rr(RSI, src_reg);              /* rsi = the int value (2nd arg) */
    e.lea_rdi(static_cast<int32_t>(static_cast<long>(slot)
                                   * static_cast<long>(sizeof(LValue))));
    e.call_relocs.push_back({ e.pos(), fn });
    e.u8(0xE8); e.u32(0);                /* call rel32 (patched later) */
    emit_call_epilogue(e);
}

/*
 * Store rax (or rdx for the idiv remainder) into the dst slot. A ref-listed
 * dst that currently holds a reference goes through jit_put_int (release +
 * store); everything else is the branchless two-store (contract 3).
 */
static void store_dst(Emitter &e, const Chunk &ck, uint8_t src_reg,
                      int dst, uint32_t bail_pc)
{
    (void)bail_pc;                       /* no bail: helper on the ref path */
    const SlotAddr a = slot_addr(dst);
    if (std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                           static_cast<int32_t>(dst))) {
        const size_t jb_fast = emit_ref_check(e, a.type);
        emit_put_int_call(e, reinterpret_cast<const void *>(jit_put_int),
                          dst, src_reg);
        const size_t jmp_done = e.j8(0xEB);   /* jmp done */
        e.patch8(jb_fast, e.pos());           /* fast: */
        e.store(RSI, a.type);                 /* the int Type singleton */
        e.store(src_reg, a.payload);
        e.patch8(jmp_done, e.pos());          /* done */
        return;
    }
    e.store(RSI, a.type);        /* the int Type singleton */
    e.store(src_reg, a.payload);
}

/* Write an int result into a slot - into its CACHE register (N5,
 * payload only; the type flushes at exit) if pinned, else store_dst's
 * memory two-store / ref-bail. */
static void write_slot(Emitter &e, const Chunk &ck, uint8_t src, int slot,
                       uint32_t bail_pc)
{
    const int cr = e.creg(slot);
    if (cr >= 0) {
        e.mov_rr(static_cast<uint8_t>(cr), src);
        return;
    }
    store_dst(e, ck, src, slot, bail_pc);
}

/* N5: pick up to 2 hot INT-scalar slots to pin in registers for a run.
 * A slot qualifies iff it is a RESOLVED LOCAL (< slot_count) and EVERY use
 * in [begin,end) is an int-scalar read/write (the int-arith / loop ops);
 * any float / array / member touch DISQUALIFIES it. Ranked by use count
 * (>= 3), top 2 chosen.
 *
 * TEMPS (>= slot_count) are EXCLUDED: a temp is scratch the VM REUSES for
 * different roles across run boundaries - an int scratch inside one JIT
 * run, a foreach array-snapshot / dict-iterator base / slice temp between
 * runs. The cache's eager entry-load + exit-flush assumes the register
 * OWNS the slot for the whole fragment; that is false for a temp still
 * live as an array across the boundary (the flush would overwrite the live
 * array with the int register + t_int tag -> a corrupted snapshot -> a
 * later LoadElemValue InternalErrorEx). A resolved local has a stable
 * identity and (being counted here only via proven-int ops) a stable int
 * type, so it is safely owned. */
static std::vector<int>
pick_cached_slots(const std::vector<Instr> &code, size_t begin,
                  size_t end, int slot_count)
{
    std::unordered_map<int, int> use;    /* slot -> int-scalar use count */
    std::unordered_set<int> disq;
    const auto usei = [&](int s) { if (s >= 0) use[s]++; };
    const auto bad  = [&](int s) { if (s >= 0) disq.insert(s); };

    for (size_t pc = begin; pc < end; pc++) {
        const Instr &in = code[pc];
        switch (in.op) {
        case OpCode::IntBin:
        case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
        case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
        case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
        case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI:
        case OpCode::IntShlRR: case OpCode::IntShrRR:
        case OpCode::IntShlRI: case OpCode::IntShrRI:
        case OpCode::IntAddModRI:
            if (!in.a_is_lit()) usei(in.a_slot());
            if (!in.b_is_lit()) usei(in.b_slot());
            usei(in.target);
            break;
        case OpCode::IntModRI:
            usei(in.a_slot()); usei(in.target);
            break;
        case OpCode::JumpUnlessIntCmp:
            if (!in.a_is_lit()) usei(in.a_slot());
            if (!in.b_is_lit()) usei(in.b_slot());
            break;
        case OpCode::ForLoopStep:
            usei(in.target2);
            if (!in.a_is_lit()) usei(in.a_slot());
            if (!in.b_is_lit()) usei(in.b_slot());
            break;
        case OpCode::IntAddStep:
            /* operand layout MUST match the emitter (emit_branch): accum =
             * a_dual_lo, counter = target2, rhs = b (b_slot unless b_is_lit),
             * bound = a_dual_hi (slot unless the private a-lit flag, read as
             * a_is_lit). Counting the wrong field - e.g. a bare `in.pb`,
             * which for a LITERAL rhs is the literal VALUE - would treat that
             * value as a slot index and cache/corrupt whatever slot it
             * collides with (an array slot -> LoadElem InternalErrorEx). */
            usei(in.a_dual_lo());      /* accumulator */
            usei(in.target2);          /* counter */
            if (!in.b_is_lit()) usei(in.b_slot());      /* rhs slot */
            if (!in.a_is_lit()) usei(in.a_dual_hi());    /* bound slot */
            break;
        case OpCode::LoadImmInt:
            usei(in.target);
            break;
        /* float / array touches disqualify the slots (not int-scalar) */
        case OpCode::FloatBin:
        case OpCode::FloatAddRR: case OpCode::FloatSubRR:
        case OpCode::FloatMulRR: case OpCode::FloatAddRI:
        case OpCode::FloatSubRI: case OpCode::FloatMulRI:
        case OpCode::JumpUnlessFloatCmp:
            bad(in.a_slot()); bad(in.b_slot()); bad(in.target);
            break;
        case OpCode::LoadImmFloat:
            bad(in.target);
            break;
        case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
            bad(in.target2); bad(in.a_slot()); bad(in.target);
            break;
        case OpCode::StoreElemInt:
            bad(in.target2);             /* base slot holds an array */
            if (!in.a_is_lit()) usei(in.a_slot());   /* index (int) */
            if (!in.b_is_lit()) usei(in.b_slot());   /* value (int) */
            break;
        case OpCode::StoreElemFloat:
            bad(in.target2);             /* base slot holds an array */
            if (!in.b_is_lit()) bad(in.b_slot());    /* value is a FLOAT */
            if (!in.a_is_lit()) usei(in.a_slot());   /* index (int) */
            break;
        case OpCode::Jump:
            break;                       /* no slots */
        default:
            return {};                   /* an unclassified op - be SAFE and
                                          * cache nothing (a new eligible op
                                          * that isn't handled here just
                                          * turns caching off, never
                                          * corrupts a slot) */
        }
    }

    std::vector<std::pair<int, int>> cand;   /* (count, slot) */
    for (const auto &kv : use)
        if (kv.first < slot_count && !disq.count(kv.first)
                && kv.second >= 3)
            cand.push_back({ kv.second, kv.first });
    std::sort(cand.begin(), cand.end(),
              [](const std::pair<int,int> &a, const std::pair<int,int> &b) {
                  return a.first != b.first ? a.first > b.first
                                            : a.second < b.second;
              });
    std::vector<int> out;
    for (size_t i = 0; i < cand.size() && i < 2; i++)
        out.push_back(cand[i].second);
    return out;
}

/* Approach-A slow-path helper: release the slot's CURRENT value (whatever
 * reference it holds) and store a scalar float. Called from native ONLY on
 * the cold path where the store target is proven to hold a reference (once
 * per reused temp - the fast two-store handles every subsequent write).
 * noexcept: a scalar put never throws. */
static void jit_put_float(LValue *lv, double v) noexcept
{
    lv->put(EvalValue(v));
}

/* Emit a call to the FLOAT put helper: rdi = &frame->slots[slot] (the arg),
 * the value already in xmm0 (a GP-reg save can't touch it). */
static void emit_put_scalar_call(Emitter &e, const void *fn, int slot)
{
    emit_call_prologue(e);               /* save rdi + cache regs, align */
    e.lea_rdi(static_cast<int32_t>(static_cast<long>(slot)
                                   * static_cast<long>(sizeof(LValue))));
    e.call_relocs.push_back({ e.pos(), fn });
    e.u8(0xE8); e.u32(0);                /* call rel32 (patched later) */
    emit_call_epilogue(e);
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

/* Store xmm<r> (a float result) into a scalar dst: t_float type + the
 * double payload. A ref-listed dst that currently holds a reference goes
 * through jit_put_float (release + store); a trivial current value takes
 * the fast two-store. NEVER bails (see emit_ref_check). */
static void emit_float_store(Emitter &e, const Chunk &ck, uint8_t xr,
                             int dst, uint32_t bail_pc)
{
    (void)bail_pc;                        /* no bail: helper on the ref path */
    const SlotAddr a = slot_addr(dst);
    if (std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                           static_cast<int32_t>(dst))) {
        const size_t jb_fast = emit_ref_check(e, a.type);
        emit_put_scalar_call(e, reinterpret_cast<const void *>(jit_put_float),
                             dst);            /* xmm0 holds the value */
        const size_t jmp_done = e.j8(0xEB);   /* jmp done */
        e.patch8(jb_fast, e.pos());           /* fast: */
        e.store_r8_type(a.type);
        e.fstore(xr, a.payload);
        e.patch8(jmp_done, e.pos());          /* done */
        return;
    }
    e.store_r8_type(a.type);              /* type = t_float */
    e.fstore(xr, a.payload);              /* payload = the double */
}

/* N6a: call a libm function `fn` (arg(s) already in xmm0[/xmm1], result
 * comes back in xmm0). The libm double routines are NOEXCEPT (NaN/inf, no
 * throw), so a call from a fragment keeps the never-unwind contract. What
 * the call clobbers that the fragment relies on: rdi (slots base) - SAVED
 * across the call; rsi (t_int) and r8 (t_float) - constant singletons,
 * RE-materialised after; the xmm regs are all caller-saved but the JIT
 * never keeps a float LIVE in a register across ops (floats are slot-
 * backed - only the INT cache uses GP r10/r11, and a MathFnV run caches
 * NOTHING per pick_cached_slots' default), so only xmm0 (arg->result)
 * matters and it survives. Stack: at fragment entry rsp % 16 == 8 (the
 * jit_enter call's return address) and the fragment makes no other pushes,
 * so a single `push rdi` both saves the base AND 16-aligns rsp for the
 * call. */
/* Write `n` bytes of NOP at `dst` as the FEWEST canonical multi-byte NOPs
 * (Intel/AMD recommended encodings, 1..9 bytes; chained above 9). A single
 * wide NOP decodes to ONE (eliminated) uop - a run of `0x90`s would burn a
 * decode/rename slot EACH. A general JIT utility (padding / a patched-out
 * instruction / future alignment); `maybe_unused` since it has no caller
 * today - the libm call is a bare 5-byte rel32 and loop alignment was
 * removed as measured net-negative. */
[[maybe_unused]] static void fill_nop(uint8_t *dst, size_t n)
{
    static const uint8_t nop[10][9] = {
        {}, {0x90}, {0x66,0x90}, {0x0F,0x1F,0x00},
        {0x0F,0x1F,0x40,0x00}, {0x0F,0x1F,0x44,0x00,0x00},
        {0x66,0x0F,0x1F,0x44,0x00,0x00}, {0x0F,0x1F,0x80,0,0,0,0},
        {0x0F,0x1F,0x84,0,0,0,0,0}, {0x66,0x0F,0x1F,0x84,0,0,0,0,0},
    };
    while (n) { const size_t k = n > 9 ? 9 : n;
                std::memcpy(dst, nop[k], k); dst += k; n -= k; }
}

/* N6a: a libm call site is a bare 5-byte `E8 rel32` - NO padding. The rel32
 * is patched once the buffer's address is known (jit_compile_chunk): to libm
 * DIRECTLY when in +-2GB (the anon-mmap-next-to-libm case, ~always), else to
 * an out-of-line TRAMPOLINE in the same buffer (`movabs rax, fn; jmp rax`,
 * always rel32-reachable). The trampoline path is the arm64-style veneer the
 * short-range branch will need there too. */
static void emit_libm_call(Emitter &e, const void *fn)
{
    /* args in xmm0[/xmm1] (GP-reg saves don't touch them). A MathFnV run
     * caches nothing today, so the prologue is usually just `push rdi` -
     * but going through it keeps the call correct if that ever changes. */
    emit_call_prologue(e);
    e.call_relocs.push_back({ e.pos(), fn });   /* off = the E8 opcode */
    e.u8(0xE8); e.u32(0);                 /* call rel32 (patched later) */
    emit_call_epilogue(e);
}

/* Approach A: a proven EXCEPTION in a fragment (OOB / negative shift).
 * Store the raise KIND to g_vm_jit_raise, then exit to the op's pc - the
 * EnterNative handler raises the matching exception (exact caret from the
 * loc table), NEVER re-interpreting the op. rax is dead on the exit path
 * (exit_pc flushes the cache, not rax). */
static void emit_raise(Emitter &e, int kind, uint32_t pc)
{
    e.movabs(RAX, reinterpret_cast<uint64_t>(&g_vm_jit_raise));
    e.u8(0xC7); e.u8(0x00);              /* mov dword [rax], kind */
    e.u32(static_cast<uint32_t>(kind));
    e.exit_pc(pc);
}

/* `<short jcc PASS> +skip; emit_raise` - raise UNLESS the pass condition
 * holds (the exception analogue of Emitter::bail_unless). */
static void raise_unless(Emitter &e, uint8_t pass_cond, int kind, uint32_t pc)
{
    const size_t sk = e.j8(pass_cond);
    emit_raise(e, kind, pc);
    e.patch8(sk, e.pos());
}

/* Approach A: a flat-array element STORE `a[i] = v` / `a[i] OP= v` as a CALL
 * to jit_store_elem_int/float (the interpreter's exact store body - COW /
 * bounds / universal fallback), keeping the surrounding loop native rather
 * than splitting the run at the store. The SysV args:
 *   int:   rdi=base, rsi=idx, rdx=rhs, rcx=aop
 *   float: rdi=base, rsi=idx, xmm0=rhs, rdx=aop
 * The int index (and int rhs) are resolved CACHE-AWARE while rdi is still the
 * slots base; THEN the prologue saves rdi + the cache regs and rdi is
 * re-pointed to &slots[base]. On a non-0 return (the helper caught + stashed
 * a LOC-LESS raise in g_vm_jit_exc) the fragment exits to the op's pc and
 * EnterNative raises it, stamping the caret from the live chunk - no chunk/pc
 * is passed (the fragment can't hold the stack-built, moved-out chunk). */
static void emit_store_elem(Emitter &e, const Chunk &ck, const Instr &in,
                            uint32_t pc, bool is_float)
{
    (void)ck;
    const void *fn = is_float
        ? reinterpret_cast<const void *>(jit_store_elem_float)
        : reinterpret_cast<const void *>(jit_store_elem_int);
    const int32_t base_off = static_cast<int32_t>(
        static_cast<long>(in.target2) * static_cast<long>(sizeof(LValue)));

    /* idx -> rsi (an int operand), read while rdi = the slots base */
    load_operand(e, RSI, in.a_is_lit(), in.a_lit(), in.a_slot());
    /* value: int -> rdx; float -> xmm0 (may BAIL on a non-numeric tag, as
     * everywhere in the float tier - the value is proven numeric, so it
     * won't in practice) */
    if (is_float)
        emit_float_load(e, X0, in.b_is_lit(), in.b_flit(), in.b_slot(), pc);
    else
        load_operand(e, RDX, in.b_is_lit(), in.b_lit(), in.b_slot());

    emit_call_prologue(e);               /* save rdi + cache, 16-align */
    e.lea_rdi(base_off);                  /* rdi = &slots[base] (arg 0) */
    /* aop is the last GP arg: rcx (int helper) / rdx (float helper) */
    e.movabs(is_float ? RDX : RCX,
             static_cast<uint64_t>(static_cast<int>(in.aop)));
    e.call_relocs.push_back({ e.pos(), fn });
    e.u8(0xE8); e.u32(0);                 /* call rel32 (patched later) */
    emit_call_epilogue(e);                /* restore rdi + cache; re-mat rsi/r8*/

    e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
    const size_t j_ok = e.j8(0x74);       /* jz -> continue (0 = no raise) */
    e.exit_pc(pc);                        /* raised: EnterNative raises exc */
    e.patch8(j_ok, e.pos());
}

/* Emit one op; returns false if (unexpectedly) unhandled. */
static bool emit_op(Emitter &e, const Chunk &ck, const Instr &in,
                    uint32_t pc)
{
    switch (in.op) {

    case OpCode::LoadImmInt:
        e.movabs(RAX, static_cast<uint64_t>(in.a_lit()));
        write_slot(e, ck, RAX, in.target, pc);
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
        read_slot(e, RAX, in.a_slot());
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, aop);
        write_slot(e, ck, RAX, in.target, pc);
        return true;
    }

    case OpCode::IntShlRR: case OpCode::IntShrRR:
    case OpCode::IntShlRI: case OpCode::IntShrRI: {
        const bool shl =
            in.op == OpCode::IntShlRR || in.op == OpCode::IntShlRI;
        read_slot(e, RAX, in.a_slot());
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
            read_slot(e, RCX, in.b_slot());   /* cache-aware (the classifier
                                               * counts this shift count as a
                                               * cacheable int use) */
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
            emit_raise(e, JR_NEG_SHIFT, pc);             /* negative count:
                                                          * RAISE InvalidValue
                                                          * (no re-interpret) */
            e.patch32(jl, static_cast<uint32_t>(e.pos() - (jl + 4)));
            e.u8(0x48); e.u8(0xD3);
            e.u8(shl ? 0xE0 : 0xF8);                     /* shl/sar rax,cl */
            e.patch32(jdone, static_cast<uint32_t>(e.pos() - (jdone + 4)));
        }
        write_slot(e, ck, RAX, in.target, pc);
        return true;
    }

    case OpCode::IntModRI: {
        read_slot(e, RAX, in.a_slot());
        e.movabs(RCX, static_cast<uint64_t>(in.b_lit()));
        e.u8(0x48); e.u8(0x99);                          /* cqo */
        e.u8(0x48); e.u8(0xF7); e.u8(0xF9);              /* idiv rcx */
        write_slot(e, ck, RDX, in.target, pc);            /* remainder */
        return true;
    }

    case OpCode::IntAddModRI: {
        read_slot(e, RAX, in.a_slot());
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, Op::plus);
        e.movabs(RCX, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.u8(0x48); e.u8(0x99);                          /* cqo */
        e.u8(0x48); e.u8(0xF7); e.u8(0xF9);              /* idiv rcx */
        write_slot(e, ck, RDX, in.target, pc);
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

    case OpCode::MathFnV: {
        /* N6a: a typed math builtin. Read the (int-promoting) float arg into
         * xmm0, apply the SSE2 op, store. Only MK_SSE selectors reach here
         * (jit_op_eligible gates it); the libm-call selectors are increment
         * B. A MathFnV run caches nothing (pick_cached_slots' default), so
         * emit_float_load reads the operand from memory (no stale cache). */
        const MathFn fn = static_cast<MathFn>(in.target2);
        if (mathfn_kind(fn) == MK_CALL) {
            /* Load the arg(s) FIRST (a bail here re-runs the op cleanly),
             * THEN the call sequence. pow is the only 2-arg selector:
             * x in xmm0, y in xmm1 - the SysV order. */
            emit_float_load(e, X0, in.a_is_lit(), in.a_flit(),
                            in.a_slot(), pc);
            if (fn == MathFn::pow_)
                emit_float_load(e, X1, in.b_is_lit(), in.b_flit(),
                                in.b_slot(), pc);
            emit_libm_call(e, jit_math_fn_ptr(fn));
            emit_float_store(e, ck, X0, in.target, pc);
            return true;
        }
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc);
        switch (fn) {
        case MathFn::sqrt_:    e.sqrtsd(X0, X0); break;
        case MathFn::tofloat_: break;   /* float(x): the read already widened */
        default:               e.u8(0xCC); break;   /* unreachable: MK_SSE */
        }
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
        raise_unless(e, 0x72, JR_OOB, pc);       /* jb (idx < count) else
                                                  * RAISE OutOfBounds (approach
                                                  * A: no re-interpret) */
        if (is_float) {
            e.load_elem_float();                 /* movsd xmm0,[rcx+r9*8] */
            emit_float_store(e, ck, X0, in.target, pc);
        } else {
            e.load_elem_int();                   /* mov rax,[rcx+r9*8] */
            write_slot(e, ck, RAX, in.target, pc);
        }
        return true;
    }

    case OpCode::StoreElemInt:
        emit_store_elem(e, ck, in, pc, /*is_float=*/false);
        return true;
    case OpCode::StoreElemFloat:
        emit_store_elem(e, ck, in, pc, /*is_float=*/true);
        return true;

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
        read_slot(e, RAX, in.target2);
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, up ? Op::plus : Op::minus);     /* rax += / -= rcx */
        write_slot(e, ck, RAX, in.target2, pc);
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
        read_slot(e, RAX, adst);
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        op_rr(e, Op::plus);
        write_slot(e, ck, RAX, adst, pc);
        read_slot(e, RAX, in.target2);
        e.u8(0x48); e.u8(0xFF); e.u8(up ? 0xC0 : 0xC8);   /* inc/dec rax */
        write_slot(e, ck, RAX, in.target2, pc);
        if (in.a_is_lit())
            e.movabs(RCX, static_cast<uint64_t>(
                              static_cast<int_type>(in.a_dual_hi())));
        else
            read_slot(e, RCX, in.a_dual_hi());
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
        case OpCode::MathFnV:            /* N6a: writes a float -> needs r8 */
        case OpCode::StoreElemFloat:     /* reads a float rhs -> needs r8 */
            return true;
        default:
            break;
        }
    }
    return false;
}

/* Approach A: an op the fragment handles WITHOUT ever returning an interior
 * pc - a non-throwing int op (arith / imm-shift / mod-by-nonzero-imm /
 * loop / branch / imm-load). Such an op has no re-interpret bail AND no
 * jit_raise (so no per-op caret is needed). A run built ONLY of these can
 * have its interpreted originals DELETED - the interpreter never re-runs
 * them. EXCLUDED (they can bail-to-reinterpret or jit_raise, so their
 * originals must stay): reg-shift (negative-count raise), LoadElem (slice/
 * kind re-interpret + OOB raise), every float op (float_load's bool/other
 * safety-net bail), generic IntBin (div/mod can throw). */
static bool op_fully_native(OpCode op)
{
    switch (op) {
    case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
    case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
    case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
    case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI:
    case OpCode::IntShlRI: case OpCode::IntShrRI:
    case OpCode::IntModRI: case OpCode::IntAddModRI:
    case OpCode::LoadImmInt: case OpCode::Jump:
    case OpCode::JumpUnlessIntCmp: case OpCode::ForLoopStep:
    case OpCode::IntAddStep:
        return true;
    default:
        return false;
    }
}

/* The pc TARGET of a branch-family op (the audited list that the nc rebuild
 * remaps), or -1. Used by the single-entry check. */
static int branch_pc_target(const Instr &in)
{
    switch (in.op) {
    case OpCode::Jump: case OpCode::JumpUnlessIntCmp:
    case OpCode::JumpUnlessFloatCmp: case OpCode::JumpUnlessTrueV:
    case OpCode::JumpIfNotNoneV: case OpCode::ForLoopStep:
    case OpCode::DictIterNext: case OpCode::ForeachDynNext:
    case OpCode::CatchTest: case OpCode::PushHandler:
    case OpCode::JumpUnlessElemInt: case OpCode::IntAddStep:
    case OpCode::ForStepElemInt:
        return in.target;
    default:
        return -1;
    }
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

    /* Approach A: a run is DELETABLE (its interpreted originals removed - no
     * double copy) iff the fragment NEVER returns an interior pc, i.e. every
     * op is `op_fully_native` (no re-interpret bail, no jit_raise) AND the
     * run is SINGLE-ENTRY (no branch from OUTSIDE it targets an INTERIOR pc;
     * an external branch to the HEAD is fine - it hits the EnterNative). A
     * deletable run's ops are dropped from the rebuilt bytecode. */
    std::vector<char> deletable(runs.size(), 0);
    for (size_t r = 0; r < runs.size(); r++) {
        const size_t b = runs[r].begin, en = runs[r].end;
        bool ok = true;
        for (size_t p = b; p < en && ok; p++)
            if (!op_fully_native(chunk.code[p].op))
                ok = false;
        for (size_t p = 0; p < n && ok; p++) {   /* single-entry */
            const int t = branch_pc_target(chunk.code[p]);
            if (t > static_cast<int>(b) && t < static_cast<int>(en)
                    && (p < b || p >= en))
                ok = false;                       /* external -> interior */
        }
        deletable[r] = ok;
    }

    /* pc remap: every run head gains one inserted EnterNative; a DELETABLE
     * run's interior ops are removed, so every one of its pcs maps to the
     * EnterNative (only the head is ever a target - single-entry). */
    std::vector<int> remap(n + 1);
    {
        size_t r = 0, pc = 0;
        int np = 0;                               /* next NEW pc */
        while (pc <= n) {
            if (r < runs.size() && pc == runs[r].begin) {
                const size_t b = runs[r].begin, en = runs[r].end;
                const int en_pc = np++;           /* the EnterNative */
                if (deletable[r]) {
                    for (size_t p = b; p < en; p++)
                        remap[p] = en_pc;         /* all -> EnterNative */
                } else {
                    for (size_t p = b; p < en; p++)
                        remap[p] = np++;          /* ops kept after it */
                }
                pc = en;
                r++;
                continue;
            }
            remap[pc] = np++;
            pc++;
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

        /* N5: pin up to 2 hot int slots in r10/r11 for this run - load
         * each ONCE here (the back edge jumps to the first op below, so
         * the loop keeps them in registers; every exit flushes them). */
        e.cache.clear();
        {
            static const uint8_t cregs[2] = { 10, 11 };
            const std::vector<int> hot =
                pick_cached_slots(chunk.code, begin, end, chunk.slot_count);
            for (size_t h = 0; h < hot.size(); h++) {
                const SlotAddr a = slot_addr(hot[h]);
                e.cache.push_back({ hot[h], a.payload, a.type, cregs[h] });
                e.load(cregs[h], a.payload);     /* entry load */
            }
        }

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
        size_t r = 0, pc = 0;
        while (pc < n) {
            if (r < runs.size() && pc == runs[r].begin) {
                Instr en;
                en.op = OpCode::EnterNative;
                Operand off;
                off.is_lit = true;
                off.lit_kind = Operand::LitKind::i;
                off.lit = static_cast<int_type>(frag_off[r]);
                en.set_a(off);
                nc.push_back(en);
                const bool del = deletable[r];
                const size_t rend = runs[r].end;
                r++;
                if (del) { pc = rend; continue; }  /* drop the originals */
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
            pc++;
        }
    }
    chunk.code = std::move(nc);

    for (auto &l : chunk.locs)
        l.pc = static_cast<uint32_t>(remap[l.pc]);
    for (auto &ic : chunk.inline_ctxs)
        ic.pc = static_cast<uint32_t>(remap[ic.pc]);

    /* Trampoline pool (out-of-line, one per DISTINCT libm fn): the rare
     * rel32-out-of-range fallback for a call (and the arm64-style veneer a
     * short-range branch will need there). Appended AFTER all fragments so
     * it never shifts a fragment offset; the patch below routes a call
     * through it only when libm is not directly reachable. */
    std::unordered_map<const void *, size_t> tramp;
    for (const Emitter::CallReloc &r : e.call_relocs) {
        if (tramp.count(r.fn))
            continue;
        tramp[r.fn] = e.pos();
        e.movabs(RAX, reinterpret_cast<uint64_t>(r.fn));   /* movabs rax, fn */
        e.u8(0xFF); e.u8(0xE0);                            /* jmp rax */
    }

    /* Map RW, copy, flip RX (strict W^X). */
    const size_t len = e.b.size();
    void *mem = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return;   /* out of memory: the EnterNative ops... must NOT stay */
    std::memcpy(mem, e.b.data(), len);
    /* Patch each call site's rel32 now the base is known: DIRECT to libm
     * when in +-2GB (~always), else through the in-buffer trampoline (which
     * is only KBs away, so it always fits). Only the 4-byte rel32 is
     * written; the E8 opcode is already in place. */
    for (const Emitter::CallReloc &r : e.call_relocs) {
        uint8_t *dst = static_cast<uint8_t *>(mem) + r.off;
        intptr_t rel = reinterpret_cast<intptr_t>(r.fn)
                     - reinterpret_cast<intptr_t>(dst + 5);
        if (rel < INT32_MIN || rel > INT32_MAX)
            rel = reinterpret_cast<intptr_t>(
                      static_cast<uint8_t *>(mem) + tramp[r.fn])
                - reinterpret_cast<intptr_t>(dst + 5);
        const int32_t r32 = static_cast<int32_t>(rel);
        for (int i = 0; i < 4; i++)
            dst[1 + i] = static_cast<uint8_t>(r32 >> (i * 8));
    }
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

void jit_type_singletons(const void *&ti, const void *&tf, const void *&ta)
{
    ti = tf = ta = nullptr;
}

#endif
