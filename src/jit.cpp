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
#include "funcdesc.h"   /* FuncDescriptor::vm_chunk (native-call gate, STEP 2.1) */
#include "eval.h"       /* builtin_slot (the LoadBuiltinV emit-time bytes) */

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
    const void *t_bool;   /* the bool Type singleton (inline is_true / bool
                           * element loads) */
    const void *t_none;   /* the none Type singleton (JumpIfNotNoneV) */
    const void *t_arr;    /* the array Type singleton (N4) */
    int slice_off;        /* SharedArrayObj: offset of `slice` (from payload) */
    int kind_off;         /* SharedObject: &kind - shobj */
    int data_off;         /* SharedObject: &elem_vec - shobj (the vector's
                           * _M_start is at +0, _M_finish at +8) */
    unsigned char kind_ints, kind_floats, kind_bools;  /* Storage values */
    int type_t_off;       /* offset of Type::t (the TypeE enum) within a Type */
    int t_str_val;        /* Type::t_str: types >= this hold a REFERENCE */
    /* De-helperize 6b: the ctx-indirect chain (via the vm.cpp probes) */
    const void *addr_ctx; /* &g_current_ctx */
    int ctx_captures;     /* EvalContext::captures (a vector<LValue>*) */
    int ctx_gfuncs;       /* EvalContext::gfuncs (a GlobalFuncTable*) */
    int gft_slots;        /* GlobalFuncTable::slots (vector; data at +0) */
    int gft_defined;      /* GlobalFuncTable::defined (vector<char>) */
    /* Step 7a (inline exception ops): the activation-side layout */
    const void *addr_act; /* &g_vm_act */
    int act_handlers;     /* VmActivation::handlers (vector<VmHandler>) */
    int act_records;      /* VmActivation::records (vector<VmCallRec>) */
    int act_rec_n;        /* VmActivation::rec_n (size_t) */
    int rec_size;         /* sizeof(VmCallRec) */
    int rec_pend;         /* VmCallRec::pend (a byte enum) */
    /* #55 STEP 2.1 native-call member offsets (via the vm.cpp probes) */
    int desc_vm_chunk;    /* FuncDescriptor::vm_chunk */
    int chunk_native_base;/* Chunk::native.base */
    int chunk_native_entry;/* Chunk::native_entry_off */
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
        LValue bprobe(EvalValue(true), false);
        l.t_bool = bprobe.get().get_type();
        l.t_none = none.get_type();   /* the global none singleton value
                                       * (JumpIfNotNoneV's `??` test) */

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
        l.kind_bools = static_cast<unsigned char>(
            SharedArrayObj::Storage::bools);
        /* Type::t offset + the t_str boundary: a slot whose current type has
         * t >= t_str holds a REFERENCE (needs a C++ release before overwrite);
         * < t_str is a trivial value (overwrite in place). */
        l.type_t_off = static_cast<int>(
            reinterpret_cast<const char *>(
                &static_cast<const Type *>(l.t_int)->t)
            - reinterpret_cast<const char *>(l.t_int));
        l.t_str_val = static_cast<int>(Type::t_str);
        /* #55 STEP 2.1: native-call member offsets (vm.cpp probes) */
        l.addr_ctx = jit_addr_current_ctx();
        l.ctx_captures = static_cast<int>(jit_off_ctx_captures());
        l.ctx_gfuncs = static_cast<int>(jit_off_ctx_gfuncs());
        l.gft_slots = static_cast<int>(jit_off_gft_slots());
        l.gft_defined = static_cast<int>(jit_off_gft_defined());
        l.addr_act = jit_addr_vm_act();
        l.act_handlers = static_cast<int>(jit_off_act_handlers());
        l.act_records = static_cast<int>(jit_off_act_records());
        l.act_rec_n = static_cast<int>(jit_off_act_rec_n());
        l.rec_size = static_cast<int>(jit_sizeof_vm_rec());
        l.rec_pend = static_cast<int>(jit_off_rec_pend());
        l.desc_vm_chunk = static_cast<int>(jit_off_desc_vm_chunk());
        l.chunk_native_base = static_cast<int>(jit_off_chunk_native_base());
        l.chunk_native_entry = static_cast<int>(jit_off_chunk_native_entry());
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
    /* mov reg, [r9 + d] / mov [r9 + d], reg (de-helperize 6b: the
     * ctx-indirect chains walk through r9 as the scratch base; rm=001 with
     * REX.B is r9, no SIB needed). */
    void load_r9b(uint8_t reg, int32_t d)
    { u8(reg >= 8 ? 0x4D : 0x49); u8(0x8B);
      u8(static_cast<uint8_t>(0x81 | ((reg & 7) << 3))); u32(
          static_cast<uint32_t>(d)); }
    void store_r9b(uint8_t reg, int32_t d)
    { u8(reg >= 8 ? 0x4D : 0x49); u8(0x89);
      u8(static_cast<uint8_t>(0x81 | ((reg & 7) << 3))); u32(
          static_cast<uint32_t>(d)); }
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
    /* The inverse: re-load every pinned slot's payload from memory. Used with
     * flush_cache to BRACKET an op that reads or writes frame slots the emitter
     * cannot enumerate (a builtin call that can re-enter the VM, a closure
     * capture snapshot, a pooled-length arg run). Flushing first makes the
     * memory copy current for the op to read; reloading after picks up anything
     * the op wrote. This is the ordinary compiler treatment of a call - spill
     * the live registers around it - and it replaces the old all-or-nothing
     * rule, which disabled pinning for the WHOLE fragment when any such op
     * appeared (so a hot loop merged into a fragment containing one builtin
     * call silently lost its pinned counters). */
    void reload_cache()
    {
        for (const CacheEnt &c : cache)
            load(c.reg, c.payload);
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
    /* lea reg, [rdi + disp32]  (an EvalValue-ptr / LValue-ptr helper arg;
     * rm = rdi = slots base). reg is a raw GP number (the Reg enum is
     * declared after this struct, so lea_rdi passes 7). */
    void lea(uint8_t reg, int32_t d)
    { u8(0x48); u8(0x8D); u8(static_cast<uint8_t>(0x87 | (reg << 3)));
      u32(uint32_t(d)); }
    /* lea rdi, [rdi + disp32]  (rdi = &frame->slots[slot], a helper arg) */
    void lea_rdi(int32_t d) { lea(7, d); }
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
    /*
     * The per-op EXECUTION counter for an INLINED op (model-flip verify rule).
     * An op emitted inline never calls its jit_* helper, so it would never bump
     * g_jit_op_run and the `jit_op_nativized` test could no longer prove it ran
     * - losing exactly the evidence the standing rule demands. Emit the bump in
     * the fragment instead: `movabs rax, &g_jit_op_run[op]; inc qword [rax]`.
     * TESTS-only (the counter itself is TESTS-only), so a release fragment is
     * byte-identical to one with no instrumentation. Call it FIRST in an op's
     * emit, while rax is still dead.
     */
    void bump_op(OpCode op)
    {
#ifdef TESTS
        movabs(0 /* rax; the Reg enum is declared below */,
               reinterpret_cast<uint64_t>(
                   &g_jit_op_run[static_cast<size_t>(op)]));
        u8(0x48); u8(0xFF); u8(0x00);          /* inc qword [rax] */
#else
        (void)op;
#endif
    }
    /* movzx eax, byte [rcx + r9]  (a flat BOOL element - 1 byte, scale 1;
     * writing eax zeroes rax's high half, so rax is a clean 0/1). */
    void load_elem_byte()
    { u8(0x42); u8(0x0F); u8(0xB6); u8(0x04); u8(0x09); }
    /* cmp rax, rcx / test rax, rax */
    void cmp_rax_rcx() { u8(0x48); u8(0x39); u8(0xC8); }
    void test_rax_rax() { u8(0x48); u8(0x85); u8(0xC0); }
    /* mov [rdi+disp], rax  (a raw payload store - the type word is written
     * separately, as the two-store write_slot does). */
    void store_rax_slot(int32_t d)
    { u8(0x48); u8(0x89); u8(0x87); u32(uint32_t(d)); }
    /* mov [rdi+disp], rcx  (the Type* word of a slot) */
    void store_rcx_slot(int32_t d)
    { u8(0x48); u8(0x89); u8(0x8F); u32(uint32_t(d)); }
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
        /* A rel8 displacement is a SIGNED BYTE. Truncating an over-127 jump
         * silently lands in the middle of an instruction - it shows up only as
         * a SEGV in generated code, with no hint where it came from. Assert
         * instead, and use j32 for any span that can grow (an exit_pc carries
         * an N5 flush, a helper call its whole prologue/epilogue). */
        const long d = static_cast<long>(target) - static_cast<long>(at + 1);
        ML_CHECK(d >= -128 && d <= 127);
        b[at] = static_cast<uint8_t>(d);
    }
    /* A NEAR (rel32) jmp/jcc, patched to here later. `short_op` is the SHORT
     * opcode (0xEB jmp, 0x7x jcc); the near forms are 0xE9 and 0x0F 0x8x. */
    size_t j32(uint8_t short_op)
    {
        if (short_op == 0xEB) {
            u8(0xE9);
        } else {
            u8(0x0F);
            u8(static_cast<uint8_t>(short_op + 0x10));
        }
        const size_t at = pos();
        u32(0);
        return at;
    }
    void patch32_here(size_t at)
    { patch32(at, static_cast<uint32_t>(pos() - (at + 4))); }
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
    /* #55: a native ReturnV - a fully-native leaf body's return runs in the
     * fragment (calls jit_ret: pop/leave, or boundary-flow; rets a resume
     * sentinel EnterNative applies). A run TERMINATOR, never a branch. */
    case OpCode::ReturnV:
        return true;
    /* model-flip (nativize-ops): a native Halt - a fall-through body's implicit
     * `return none` runs in the fragment (calls jit_halt: like jit_ret with a
     * none result). A run TERMINATOR, never a branch. Never throws. */
    case OpCode::Halt:
        return true;
    /* N3: the SSE float tier. add/sub/mul/div (div RAISES DivisionByZeroEx
     * on a +-0.0 divisor via JR_DIV0 - a sign-stripped bits test, exactly
     * TypeFloat::div's fpclassify FP_ZERO check - then divsd); mod is the
     * same zero check + an fmod LIBM call (the exact function
     * TypeFloat::mod runs, so NaN/inf semantics are byte-identical). A
     * float READ handles an int-in-a-float-slot via cvtsi2sd (matching
     * read_float_slot) and BAILS on bool/other. */
    case OpCode::FloatBin:
        return in.aop == Op::plus || in.aop == Op::minus
            || in.aop == Op::times || in.aop == Op::div
            || in.aop == Op::mod;
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
    /* Approach A: a DICT element store d[k] = v / d[k] OP= v. The fragment
     * leas the base dict LValue* + the boxed key/value slot EvalValue*s and
     * CALLS jit_dict_store (the interpreter's exact vm_subscript_store), so
     * the counter loop no longer splits at the store. LOCAL base only.
     * Measured ~6.5% wall / 12% fewer instrs on 23_dict_insert. */
    case OpCode::DictStore:
        return in.target == 0;
    /* model-flip (nativize-ops): the UNIVERSAL subscript store a[i] = v /
     * a[i] OP= v via jit_store_elem_value (the interpreter's exact
     * vm_subscript_store; ANY base type). Unlike StoreElemInt/DictStore, it
     * handles a GLOBAL/CAPTURE base too (kind = target passed to the helper). An
     * undefined-global base bails; a subscript throw (OOB/KeyNotFound/type/
     * NotLValue) re-raises. Not op_fully_native (throws / bails). */
    case OpCode::StoreElemValue:
        return true;
    /* model-flip (nativize-ops): a struct field store s.f = v via
     * jit_store_member (the interpreter's exact vm_member_store; the member key
     * + carets come from the baked member_keys pool). GLOBAL/CAPTURE base too.
     * Undefined-global bail; a member-store throw re-raises. Not
     * op_fully_native. */
    case OpCode::StoreMemberV:
        return true;
    /* model-flip (nativize-ops): the NESTED-CHAIN stores via jit_store_elem2 /
     * jit_store_elem_chain / jit_store_lvalue_chain (the interpreter's exact
     * vm_nested_subscript_store / vm_chain_store_op / vm_chain_lvalue_store_op;
     * per-step carets from the baked chain_locs/chain_steps + member_keys pools).
     * StoreElem2V's base is LOCAL; the other two take a kind (global/capture ->
     * undefined bail). A store throw re-raises. Not op_fully_native. */
    case OpCode::StoreElem2V:
    case OpCode::StoreElemChainV:
    case OpCode::StoreLValueChainV:
        return true;
    /* model-flip (nativize-ops): a boxed slot copy `dst = src.get()`. Calls
     * jit_move (the interpreter's exact MoveV, ref-aware, never throws) so a
     * run no longer splits at a MoveV - one island op fewer, bigger fragments.*/
    case OpCode::MoveV:
        return true;
    /* model-flip (nativize-ops): a general subscript READ base[idx] via
     * jit_subscript (the interpreter's exact Type::subscript; throws -> exit ->
     * EnterNative re-raises). base/idx/dst are frame slots. Not op_fully_native
     * (it can bail on a throw), so a run holding it keeps its interpreted
     * originals - like the store ops. */
    case OpCode::SubscriptV:
        return true;
    /* model-flip (nativize-ops): a member READ base.member via jit_member (the
     * shared member_read_core; throws -> exit -> EnterNative re-raises). Not
     * op_fully_native (it can bail on a throw). */
    case OpCode::MemberV:
        return true;
    /* model-flip (nativize-ops): the H1 typed standalone struct-member read
     * `p.x` (th==i/f) via jit_load_member - the shared vm_load_member_scalar
     * body (POD byte fast path + the member_read_core fallback, which can
     * throw with the member caret -> re-raise). NOT op_fully_native. */
    case OpCode::LoadMemberInt:
    case OpCode::LoadMemberFloat:
        return true;
    /* model-flip (nativize-ops): the #9 fusion `dst = other + a[i].f` - the
     * proven no-fault field read via jit_struct_field_add_int, the add + the
     * dst write in the FRAGMENT (the dst is the reduction's hot accumulator,
     * kept cache-aware). Never throws -> op_fully_native. */
    case OpCode::StructFieldAddInt:
        return true;
    /* model-flip (nativize-ops): the #9 back-edge fusion `for i: ... a[i]` -
     * step + test + the element load, emitted inline (emit_branch). The
     * BASE GATE (array/non-slice/ints-or-bools) runs BEFORE the counter
     * step, because a bail re-runs the WHOLE op and a post-step bail would
     * double-step; an OOB RAISES (JR_OOB, this pc's caret). NOT
     * op_fully_native (gate bail + raise). */
    case OpCode::ForStepElemInt:
        return true;
    /* model-flip (nativize-ops): append(struct_arr, Ctor(args)) via
     * jit_emplace_struct (the shared vm_do_emplace over the field-value run;
     * bakes &emplace_sites[idx]). A coerce/const throw re-raises. NOT
     * op_fully_native. */
    case OpCode::EmplaceStruct:
        return true;
    /* model-flip (nativize-ops): the `??` short-circuit branch - an inline
     * type-tag compare against the none singleton (never throws ->
     * op_fully_native; see emit_branch). */
    case OpCode::JumpIfNotNoneV:
        return true;
    /* model-flip (nativize-ops): the typed float compare-to-BOOL VALUE -
     * inline (swapped) ucomisd + setcc + the bool store, the CmpIntV shape
     * with JumpUnlessFloatCmp's NaN-correct swap trick. The ordering
     * compares only (eq/noteq have the unordered-ZF pitfall and stay
     * interpreted, like the branch form). A float operand read can BAIL
     * (bool/other tag) -> NOT op_fully_native. */
    case OpCode::CmpFloatV:
        return float_cmp_eligible(in.aop);
    /* model-flip (nativize-ops): a const arr/dict/func decl bind via
     * jit_decl_const (local or global; never throws -> op_fully_native). */
    case OpCode::DeclConstV:
        return true;
    /* model-flip (nativize-ops): `defined(g)` via jit_defined_global (the
     * slot's defined-flag; never throws -> op_fully_native). */
    case OpCode::DefinedGlobalV:
        return true;
    /* model-flip (nativize-ops): the STRICT-unpack ops via jit_unpack_elem /
     * jit_multi_unpack (the shared bodies; pool-baked target/coerce lists).
     * The strict-length/non-array throws re-raise with side-table carets ->
     * NOT op_fully_native. */
    case OpCode::UnpackElemInt:
    case OpCode::UnpackElemFloat:
    case OpCode::UnpackElemValue:
    case OpCode::UnpackElemTargets:
    case OpCode::MultiUnpackV:
        return true;
    /* model-flip (nativize-ops): the CHECKED INC-DEC ops via jit_incdec_*
     * (the shared bodies / pooled-caret runtime cores; an undefined-GLOBAL
     * base BAILS - UndefinedVariableEx is not conveyable). NOT
     * op_fully_native (bail + throws). */
    case OpCode::IncDecCheckedV:
    case OpCode::IncDecElemCheckedV:
    case OpCode::IncDecMemberCheckedV:
    case OpCode::IncDecChainV:
        return true;
    /* Step 7a: the SIMPLE exception ops, INLINE (the helper-call form was
     * measured +8% on the no-throw try path and reverted - the call
     * protocol exceeded the dispatch it replaced for a 4-byte vector
     * push/pop). PushHandler = a capacity check + store + bump (the cold
     * grow falls to jit_push_handler_grow); PopHandler = `finish -= 4`;
     * SetPend = a byte store into records[rec_n-1].pend. None can throw ->
     * op_fully_native. PushHandler routes through emit_branch (its pushed
     * catch_pc is a PC needing remap[]). The raise-side ops (Throw/
     * CatchTest/Reraise/EndFinally) need the dynamic-resume design and
     * stay interpreted. */
    case OpCode::PushHandler:
    case OpCode::PopHandler:
    case OpCode::SetPend:
        return true;
    /* Step 7a: EndFinally - the hot NORMAL path is a byte compare + fall
     * through; the cold RERAISE path BAILS to the interpreter (the raise
     * needs vm_raise's dynamic resume). Eligibility matters beyond its own
     * dispatch: EndFinally was the run SPLITTER that left a try-loop's back
     * edge landing on an INTERIOR pc (interpreted body every iteration -
     * the fragment-head defect class); with it eligible the whole loop is
     * one run and the back edge is a fragment-local jump. NOT
     * op_fully_native (the bail re-runs the op). */
    case OpCode::EndFinally:
        return true;
    /* Step 7a: the COLD catch-region ops. CatchTest/Reraise are only ever
     * ENTERED via vm_raise's handler dispatch (the interpreter is already
     * driving there), and Throw always raises - so their native form is an
     * unconditional EXIT at the op (the ThrowRuntimeV pattern; the kept
     * originals run interpreted). Eligibility is the point: they were the
     * run SPLITTERS that left a try/catch loop's back edge crossing
     * fragments (the interior-entry defect class); merged, the whole loop
     * is one run and the back edge is a fragment-local jump. NOT
     * op_fully_native. */
    case OpCode::CatchTest:
    case OpCode::Reraise:
    case OpCode::Throw:
        return true;
    /* model-flip (nativize-ops): an ALWAYS-THROWING construct. Its exception
     * mix includes NON-Runtime ones (UndefinedVariableEx/CannotRebindBuiltin)
     * that cannot ride g_vm_jit_exc, so the native form is simply an
     * unconditional exit at the op - the interpreter re-runs the
     * side-effect-free throw op and raises with exact semantics for every
     * kind. The value is run-shape: the ops BEFORE it fragment together
     * instead of splitting at the throw. NOT op_fully_native. */
    case OpCode::ThrowRuntimeV:
        return true;
    /* model-flip (nativize-ops): a slice READ base[start:end] via jit_slice (the
     * runtime Type::slice; a non-int bound / non-sliceable base throws
     * TypeErrorEx -> exit -> EnterNative re-raises). base/start/end/dst are frame
     * slots. Not op_fully_native (it can bail on a throw; caret in the loc side
     * table). */
    case OpCode::SliceV:
        return true;
    /* model-flip (nativize-ops): load a builtin value into a slot via
     * jit_load_builtin (a trivial value, never throws -> op_fully_native). */
    case OpCode::LoadBuiltinV:
        return true;
    /* model-flip (nativize-ops): copy a const-pool value into a slot via
     * jit_load_const (the emitter bakes the pool BUFFER address; never
     * throws). */
    case OpCode::LoadConstV:
        return true;
    /* model-flip (nativize-ops): read a captured value into a slot via
     * jit_load_capture ((*ctx->captures)[idx].get() - a capture is snapshot at
     * closure creation, always defined, so never throws -> op_fully_native). */
    case OpCode::LoadCaptureV:
        return true;
    /* model-flip (nativize-ops): a global store `g = expr` (PLAIN, aop invalid,
     * via jit_store_global - never throws) OR `g OP=`/`g++` (COMPOUND, via
     * jit_store_global_compound - reads the rhs from the boxed_ops pool, runs
     * num_bin_op; an undefined slot BAILS, a num_bin_op throw re-raises). Both
     * eligible; op_fully_native gates deletability on aop. */
    case OpCode::StoreGlobalV:
        return true;
    /* model-flip (nativize-ops): a capture store `cap = expr` (PLAIN, via
     * jit_store_capture - always defined, never throws) OR `cap OP=`/`cap++`
     * (COMPOUND, via jit_store_capture_compound - num_bin_op; re-raises, no bail
     * since captures are always defined). Both eligible. */
    case OpCode::StoreCaptureV:
        return true;
    /* model-flip (nativize-ops): a global READ `dst = g` via jit_load_global.
     * The common (defined) case runs native; an undefined global BAILS (the
     * interpreter re-runs + throws UndefinedVariableEx). NOT op_fully_native
     * (the original is kept for the bail-to-reinterpret). */
    case OpCode::LoadGlobalV:
        return true;
    /* model-flip (nativize-ops): materialize a baked literal via
     * jit_load_literal_obj (bakes the literal-objs pool BUFFER addr; a clone,
     * never throws). */
    case OpCode::LoadLiteralObjV:
        return true;
    /* model-flip (nativize-ops): the foreach snapshot bound n = size(array)
     * via jit_arr_len - a proven flat array, never throws. */
    case OpCode::ArrLen:
        return true;
    /* model-flip (nativize-ops): typed dict scalar read d.k / d[k] via
     * jit_dict_load. A missing key CAN throw (catch -> g_vm_jit_exc, exit_pc),
     * so it is NOT op_fully_native. */
    case OpCode::DictLoadInt:
    case OpCode::DictLoadFloat:
        return true;
    /* model-flip (nativize-ops): closure create via jit_make_closure (bakes the
     * program-lifetime FuncDescriptor* value; snapshots captures). Never
     * throws. */
    case OpCode::MakeClosureV:
        return true;
    /* model-flip (nativize-ops): an array LITERAL `[a, b, ...]` via jit_make_array
     * (build_array_from_values over the element run, per the ArrHint in target2).
     * The build has NO error path (a mixed literal just goes general), so it
     * never throws -> op_fully_native. */
    case OpCode::MakeArrayV:
        return true;
    /* model-flip (nativize-ops): a dict LITERAL `{k: v, ...}` via jit_make_dict
     * (build_dict_from_pairs over the interleaved key/value run). An UNHASHABLE
     * key (a dyn-laundered func) CAN throw -> re-raise, so NOT op_fully_native
     * (its caret comes from the pc-keyed loc side table). */
    case OpCode::MakeDictV:
        return true;
    /* model-flip (nativize-ops): the STRUCT BUILDS - a POD ctor `P(x,y)`
     * (jit_struct_ctor), a BOXED ctor `B(a,x)` (jit_struct_ctor_boxed), and the
     * fused flat array<PodStruct> literal (jit_make_struct_array). Each can
     * throw a TypeErrorEx from a field coerce -> re-raise, so NONE is
     * op_fully_native (the POD/array pair carry a side-table caret; the boxed
     * ctor carries its own POOLED per-arg caret). */
    case OpCode::StructCtorV:
    case OpCode::StructCtorBoxedV:
    case OpCode::MakeStructArrayV:
        return true;
    /* model-flip (nativize-ops): the FOREACH element/field LOADS. The index is
     * materialized cache-aware into a register before the call, so an N5-pinned
     * loop counter stays usable. All non-throwing (the index is loop-bounded by
     * the ArrLen/StrLen that produced it, the base kind is proven) EXCEPT
     * LoadElemValue, which bounds-checks (it also serves 2-D `a[i][k]`): it
     * re-raises an OOB / bails on an unproven base, so only IT is excluded from
     * op_fully_native. */
    case OpCode::LoadElemBool:
    case OpCode::StrLen:
    case OpCode::LoadStrChar:
    case OpCode::LoadStructFieldInt:
    case OpCode::LoadStructFieldFloat:
    case OpCode::LoadStructElemV:
    case OpCode::LoadElemValue:
        return true;
    /* model-flip (nativize-ops): the typed int compare-to-BOOL VALUE
     * (`(a<b)+(a>b)`, a predicate's return, a sort comparator's `a<b`). Pure
     * int compare + setcc + a bool store - no call, and it CANNOT fault, so it
     * is op_fully_native. It was islanding real loops (57_bool_reduce). */
    case OpCode::CmpIntV:
        return true;
    /* model-flip (nativize-ops): the E4 `if (arr[i])` fusion - the hot island
     * in the sieve/bool-reduce loops. Emitted inline as the shared element
     * read + a test/jcc (see emit_branch). */
    case OpCode::JumpUnlessElemInt:
        return true;
    /* model-flip (nativize-ops): the ITERATOR ops - a dict foreach's live
     * iterator (DictIter*) and the dyn foreach's runtime-dispatching one
     * (ForeachDyn*). The Init pair are plain helper calls; the Next pair
     * BRANCH (the helper binds + advances and returns the verdict, the
     * fragment jumps to end_pc on exhaustion - the JumpUnlessTrueV shape), so
     * a dict/dyn foreach loop's back edge stays inside the fragment. DictIter*
     * never throw (the dict is proven) -> op_fully_native; ForeachDyn* throw
     * (a non-container init / the strict N-var unpack) -> re-raise, NOT
     * op_fully_native (side-table carets). */
    case OpCode::DictIterInit:
    case OpCode::DictIterNext:
    case OpCode::ForeachDynInit:
    case OpCode::ForeachDynNext:
        return true;
    /* model-flip (nativize-ops): the BOXED condition BRANCH (`if (dynvalue)`,
     * `while (flag)`, a &&/|| conjunct, the boxed ternary) - jit_is_true
     * evaluates it and the FRAGMENT jumps (emit_branch). This was the top
     * blocker for foreach/if bodies: an un-nativized branch SPLIT the run, so
     * the loop's back edge fell outside the fragment and every iteration after
     * the first ran interpreted. is_true CAN throw -> re-raise, so NOT
     * op_fully_native (the caret comes from the pc-keyed loc side table). */
    case OpCode::JumpUnlessTrueV:
        return true;
    /* model-flip (nativize-ops): the boxed-arith ops (dyn/string operands) via
     * jit_boxed_* + the boxed_ops pool (target2 = the pool index, set by
     * build_boxed_ops). vm_num_binop CAN throw (div0/type) -> NOT
     * op_fully_native. Guard target2 (a valid pool index must exist - it always
     * does post-build_boxed_ops, but be defensive against an un-pooled op). */
    case OpCode::BinOpV:
    case OpCode::CmpV:
    case OpCode::CompoundV:
    /* UnaryV: boxed unary (`-str`/`~str` throw). Same boxed_ops pool. */
    case OpCode::UnaryV:
        return in.target2 >= 0;
    /* LogV (eager && / ||): is_true's BASE Type op CAN throw (an operand with
     * no bool conversion), so it re-raises like the rest - NOT op_fully_native.
     * Same boxed_ops pool (target2 = the index). */
    case OpCode::LogV:
        return in.target2 >= 0;
    /* CoerceNumV: the typed numeric coerce of a dyn value. Register-passed
     * (target2 is the int/float FLAG, not free for a pool index). CAN throw
     * (bad narrow) -> NOT op_fully_native. */
    case OpCode::CoerceNumV:
        return true;
    /* CallBuiltinV: a value-ABI read-only builtin call via jit_call_builtin
     * (bakes the builtin_calls pool entry). Every reachable builtin throw is a
     * RuntimeException now, conveyed via g_vm_jit_exc. */
    case OpCode::CallBuiltinV:
        return true;
    /* AppendV: `append(a, x)`/`push(a, x)` via jit_append (the never-throwing
     * arr_append_fast fast path + the vm_call_builtin_lv_rest fallback; every
     * fallback throw is a RuntimeException now -> g_vm_jit_exc). Keeps an
     * append-in-a-loop native. NOT op_fully_native (the fallback can throw). */
    case OpCode::AppendV:
        return true;
    /* CheckFuncV/MapFilterV (the map/filter pair): the guard conveys a
     * loc-less TypeErrorEx (re-raise stamps the side-table caret at the op
     * pc -> NOT op_fully_native), the map/filter runs the shared
     * vm_map_filter (callbacks re-enter vm_dispatch; a plain callback throw
     * rides g_vm_jit_eptr). The last non-call island pair. */
    case OpCode::CheckFuncV:
    case OpCode::MapFilterV:
        return true;
    /* CallBuiltinLV: a mutating lvalue-ABI builtin (pop/insert/erase/sort/
     * reverse/intptr) via jit_call_builtin_lv - forms arg0 from kind+slot, calls
     * vm_call_builtin_lv_rest (rest-native) or func_lv (no value args). Every
     * throw is a RuntimeException now -> g_vm_jit_exc. NOT op_fully_native. */
    case OpCode::CallBuiltinLV:
        return true;
    /* CallBuiltinLVElem/LVMember: a mutating lvalue builtin with a subscript
     * (`append(a[i], x)`) or struct-member (`append(s.f, x)`) arg0 - derive the
     * element/field LValue* in the helper, then func_lv. Rest-native (run always
     * a lit), every throw a RuntimeException. NOT op_fully_native. */
    case OpCode::CallBuiltinLVElem:
    case OpCode::CallBuiltinLVMember:
        return true;
    /* GENERIC IntBin (the residual after specialize_arith_ops - a lit-first
     * NON-commutative op like `0 - i` that has no imm-reg specialized shape,
     * div/mod-by-reg which have no specialized shape at all, `>>>`, or any op
     * the specializer doesn't cover). ALL arms are now eligible: the
     * non-throwing ones (plus/minus/times/band/bor/bxor) emit as before;
     * div/mod emit a zero-check that RAISES DivisionByZeroEx (JR_DIV0, the
     * jit_raise path - caret from the loc side table, byte-identical to the
     * interpreted throw) then cqo+idiv; the shift arms reuse the reg-count
     * shift core (negative count -> JR_NEG_SHIFT raise, >= 64 saturates).
     * Unlike the RR/RI forms, both operands may be slot OR imm (that's why
     * `0 - i` is generic - operand a is the imm), so the emit uses
     * load_operand on both. Only the non-throwing arms are op_fully_native. */
    case OpCode::IntBin:
        switch (in.aop) {
        case Op::plus: case Op::minus: case Op::times:
        case Op::band: case Op::bor:  case Op::bxor:
        case Op::div:  case Op::mod:
        case Op::shl:  case Op::shr:  case Op::ushr:
            return true;
        default:
            return false;
        }
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

/* The r9-based sibling (the value lives at [r9 + ...], a capture/global
 * slot): jump NEAR to the helper when it is a REFERENCE. Clobbers rcx. */
static size_t emit_ref_check_jae_r9(Emitter &e, int32_t type_off)
{
    const JitLayout &L = jit_layout();
    e.load_r9b(RCX, type_off);
    e.u8(0x8B); e.u8(0x89);                    /* mov ecx, [rcx + type_t_off] */
    e.u32(static_cast<uint32_t>(L.type_t_off));
    e.u8(0x81); e.u8(0xF9);                    /* cmp ecx, t_str_val */
    e.u32(static_cast<uint32_t>(L.t_str_val));
    return e.j32(0x73);                        /* jae -> the helper (a ref) */
}

/* The STORE-source gate (StoreGlobalV/StoreCaptureV): their helper runs
 * RValue(*src), so besides a reference (>= t_str) the PSEUDO types t_lval/
 * t_undefid (1, 2 - collapse/throw) and the rare t_none (0) also take the
 * helper; the inline handles the 3..t_str-1 range (int/builtin/float/bool/
 * structtype). Returns the TWO jump sites to patch to the helper. */
static std::pair<size_t, size_t>
emit_store_src_gate(Emitter &e, int32_t type_off)
{
    const JitLayout &L = jit_layout();
    e.load(RCX, type_off);
    e.u8(0x8B); e.u8(0x89);                    /* mov ecx, [rcx + type_t_off] */
    e.u32(static_cast<uint32_t>(L.type_t_off));
    e.u8(0x83); e.u8(0xF9); e.u8(3);           /* cmp ecx, 3 (t_int) */
    const size_t j_lo = e.j32(0x72);           /* jb -> helper (none/pseudo) */
    e.u8(0x81); e.u8(0xF9);                    /* cmp ecx, t_str_val */
    e.u32(static_cast<uint32_t>(L.t_str_val));
    const size_t j_hi = e.j32(0x73);           /* jae -> helper (a ref) */
    return { j_lo, j_hi };
}

/* Walk `r9 = ctx->captures->data()` (cap=true) or, with the GlobalFuncTable
 * kept in RDX for the caller's `defined` write, `r9 = gfuncs->slots.data()`
 * (cap=false). Clobbers rax (and rdx for the global chain). */
static void emit_ctx_chain_r9(Emitter &e, bool cap)
{
    const JitLayout &L = jit_layout();
    e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_ctx));
    e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] (the ctx) */
    if (cap) {
        e.u8(0x48); e.u8(0x8B); e.u8(0x80);    /* mov rax,[rax+captures] */
        e.u32(static_cast<uint32_t>(L.ctx_captures));
        e.u8(0x4C); e.u8(0x8B); e.u8(0x88);    /* mov r9,[rax+0] (data) */
        e.u32(0);
    } else {
        e.u8(0x48); e.u8(0x8B); e.u8(0x90);    /* mov rdx,[rax+gfuncs] */
        e.u32(static_cast<uint32_t>(L.ctx_gfuncs));
        e.u8(0x4C); e.u8(0x8B); e.u8(0x8A);    /* mov r9,[rdx+slots+0] */
        e.u32(static_cast<uint32_t>(L.gft_slots));
    }
}

/* The INVERTED form for the de-helperize inline paths: jump NEAR to the
 * HELPER fallback when the value at `type_off` is a REFERENCE (type->t >=
 * t_str); fall through for a trivial value. Returns the jae rel32 site to
 * patch to the helper label. Clobbers rcx. */
static size_t emit_ref_check_jae(Emitter &e, int32_t type_off)
{
    const JitLayout &L = jit_layout();
    e.load(RCX, type_off);
    e.u8(0x8B); e.u8(0x89);                    /* mov ecx, [rcx + type_t_off] */
    e.u32(static_cast<uint32_t>(L.type_t_off));
    e.u8(0x81); e.u8(0xF9);                    /* cmp ecx, t_str_val */
    e.u32(static_cast<uint32_t>(L.t_str_val));
    return e.j32(0x73);                        /* jae -> the helper (a ref) */
}

/* Approach-A slow-path helpers: release the slot's current value and store
 * a scalar (int / float). Called from native ONLY on the ref path (cold,
 * once per reused temp). noexcept: a scalar put never throws. */
static void jit_put_int(LValue *lv, int_type v) noexcept
{
    lv->put(EvalValue(v));
}

/* The BOOL sibling (CmpIntV's dst is a real bool, not 0/1). Same cold ref
 * path: the slot currently holds a reference, whose release needs C++. */
static void jit_put_bool(LValue *lv, int_type v) noexcept
{
    lv->put(EvalValue(v != 0));
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

/* Re-raise DELETABILITY: the op's own LocEntry (`&ck.locs[i]`, a stable
 * pool-buffer address) baked as a helper arg, so a conveying helper stamps
 * the caret itself - pc-independent, which is what lets the op's interpreted
 * original be DELETED (a deleted run collapses its pcs onto the EnterNative,
 * where a table lookup would be wrong). The loc table is OLD-pc-keyed at
 * emission (the side tables are remapped after). Null if the op recorded no
 * loc (then nothing anywhere holds a caret for it - both engines loc-less). */
static const void *loc_entry_addr(const Chunk &ck, size_t old_pc)
{
    size_t lo = 0, hi = ck.locs.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (ck.locs[mid].pc < old_pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < ck.locs.size() && ck.locs[lo].pc == old_pc)
        return &ck.locs[lo];
    return nullptr;
}

/* The COLD-side caret stamp: on a conveying helper's failure branch (taken
 * only when the helper returned non-zero), write the op's baked start/end
 * Locs DIRECTLY into the exception object in g_vm_jit_exc - zero
 * instructions on the success path, unlike a helper ARG (which stays live
 * across the helper's try and cost a measured +4 Ir per CALL in an extra
 * callee-saved spill). GUARDS: skip when g_vm_jit_exc is NULL (a BAIL or
 * an eptr conveyance - nothing to stamp; this is what makes a stale-state
 * bug structurally impossible on ops whose failure branch serves both a
 * bail and a convey), and when the exception already carries a caret
 * (Loc::operator bool == col != 0 - a nested throw keeps its own). A Loc
 * is {int line; int col} = one qword store per Loc, little-endian
 * line | col<<32. rax/rcx are dead here (exit_pc's cache flush uses
 * rdi/rsi/r8/r10/r11; eax is set after). */
static void emit_exc_stamp(Emitter &e, const Chunk &ck, size_t old_pc)
{
    const Chunk::LocEntry *le = static_cast<const Chunk::LocEntry *>(
        loc_entry_addr(ck, old_pc));
    if (!le)
        return;                    /* no loc recorded - stays loc-less */
    const auto pack = [](const Loc &l) {
        return static_cast<uint64_t>(static_cast<uint32_t>(l.line))
             | (static_cast<uint64_t>(static_cast<uint32_t>(l.col)) << 32);
    };
    const uint32_t off_s = static_cast<uint32_t>(jit_off_exc_loc_start());
    const uint32_t off_e = static_cast<uint32_t>(jit_off_exc_loc_end());

    e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_exc()));
    e.u8(0x48); e.u8(0x8B); e.u8(0x00);      /* mov rax, [rax] (the object) */
    e.u8(0x48); e.u8(0x85); e.u8(0xC0);      /* test rax, rax */
    const size_t j_null = e.j8(0x74);        /* jz skip: bail/eptr - no exc */
    e.u8(0x83); e.u8(0xB8);                  /* cmp dword [rax+off], 0 */
    e.u32(off_s + 4);                        /*   ... loc_start.col */
    e.u8(0x00);
    const size_t j_has = e.j8(0x75);         /* jnz skip: caret already set */
    e.movabs(RCX, pack(le->start));
    e.u8(0x48); e.u8(0x89); e.u8(0x88);      /* mov [rax+off_s], rcx */
    e.u32(off_s);
    e.movabs(RCX, pack(le->end));
    e.u8(0x48); e.u8(0x89); e.u8(0x88);      /* mov [rax+off_e], rcx */
    e.u32(off_e);
    e.patch8(j_null, e.pos());
    e.patch8(j_has, e.pos());
}

/* M5: emit a SYNC call op (CallV / CachedCallV / CallValueV - one emit
 * shape, only the helper differs): jit_call_sync*(target2, argbase, nargs,
 * dst, site) runs the callee to completion via the lean sync enter (vm.cpp)
 * and the caller continues natively on 0. Non-0 -> exit (a bail re-runs the
 * interpreted op; a throw rides g_vm_jit_exc / g_vm_jit_eptr). The baked
 * site = the op's side-table loc start (the lazy in-VM backtrace capture's
 * loc); the loc table is still OLD-pc-keyed at emission (the side tables
 * are remapped AFTER), so look up with old_pc, not the remapped pc (a
 * remapped lookup silently found nothing and baked site 0, which showed as
 * `at line 0` in a backtrace). */
static void emit_sync_call(Emitter &e, const Chunk &ck, const Instr &in,
                           uint32_t pc, size_t old_pc, const void *helper,
                           OpCode op)
{
    Loc ls, le;
    ck.loc_at(old_pc, ls, le);
    const uint64_t site =
        (static_cast<uint64_t>(static_cast<uint32_t>(ls.line)) << 32)
        | static_cast<uint32_t>(ls.col);
    emit_call_prologue(e);
    e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
    e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
    e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
    e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
    e.movabs_r8(site);
    e.call_relocs.push_back({ e.pos(), helper });
    e.u8(0xE8); e.u32(0);
    emit_call_epilogue(e);
    e.u8(0x85); e.u8(0xC0);                   /* test eax, eax */
    const size_t j_ok = e.j8(0x74);
    e.exit_pc(pc);
    e.patch8(j_ok, e.pos());
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

/* Store a BOOL result (0/1 in `src_reg`) into a slot: the two-store with the
 * t_bool singleton, or - when the slot is REF-LISTED, i.e. it may currently
 * hold a reference whose release needs C++ - the same ref-check + put-helper
 * shape store_dst uses for ints. A bool dst is never N5-cached (the cache is
 * int-only), so there is no register case. */
static void store_dst_bool(Emitter &e, const Chunk &ck, uint8_t src_reg, int dst)
{
    const SlotAddr a = slot_addr(dst);
    const uint64_t tb = reinterpret_cast<uint64_t>(jit_layout().t_bool);
    if (std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                           static_cast<int32_t>(dst))) {
        const size_t jb_fast = emit_ref_check(e, a.type);
        emit_put_int_call(e, reinterpret_cast<const void *>(jit_put_bool),
                          dst, src_reg);
        const size_t jmp_done = e.j8(0xEB);   /* jmp done */
        e.patch8(jb_fast, e.pos());           /* fast: */
        e.movabs(RCX, tb);
        e.store(RCX, a.type);
        e.store(src_reg, a.payload);
        e.patch8(jmp_done, e.pos());          /* done */
        return;
    }
    e.movabs(RCX, tb);
    e.store(RCX, a.type);
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
                  size_t end, int slot_count,
                  std::vector<char> *barrier = nullptr)
{
    /* `barrier[pc-begin] = 1` marks an op that touches frame slots the emitter
     * cannot enumerate. Such an op is BRACKETED by flush_cache/reload_cache
     * (the compiler's spill-around-a-call) instead of disabling pinning for the
     * whole fragment - only ops that are NOT branches may be bracketed, since a
     * taken branch would skip the reload. */
    const auto mark_barrier = [&](size_t pc) {
        if (barrier)
            (*barrier)[pc - begin] = 1;
    };
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
        case OpCode::ReturnV:
            /* the result value slot is read by the return. Counting it as an
             * int use is SAFE: a FLOAT result slot is always disqualified by
             * the float op that produced it (bad()), and a slot reachable
             * only via ReturnV is used once (< the 3-use cache threshold) - so
             * no float slot is ever cached as an int here. */
            usei(in.a_slot());
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
            /* the INDEX is read cache-aware (load_index_r9), so it stays a
             * countable int use - it is the loop counter, the slot most worth
             * pinning. dst/base are written/read in memory. */
            bad(in.target2); bad(in.target);
            if (!in.a_is_lit()) usei(in.a_slot());
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
        case OpCode::DictStore:
            /* the fragment passes &slot for base/key/value, so those slots
             * must hold CURRENT EvalValues - a cached int key (a counter used
             * as d[i]) would leave its slot stale. Disqualify all three. */
            bad(in.target2); bad(in.a_slot()); bad(in.b_slot());
            break;
        case OpCode::StoreElemValue:
            /* jit_store_elem_value reads idx (a_slot) + val (b_slot) - and a
             * LOCAL base (target2) - from MEMORY via g_current_ctx; a cached int
             * index (a counter used as a[i]) would be stale. The base is a frame
             * slot only for a LOCAL base (kind == target == 0). */
            if (in.target == 0) bad(in.target2);
            bad(in.a_slot()); bad(in.b_slot());
            break;
        case OpCode::StoreMemberV:
            /* jit_store_member reads val (b_slot) - and a LOCAL base (target2) -
             * from MEMORY; the member key is a_lit (a pool index, not a slot). */
            if (in.target == 0) bad(in.target2);
            bad(in.b_slot());
            break;
        case OpCode::StoreElem2V:
            /* jit_store_elem2 reads base (target2, always LOCAL), k1 (a_dual_lo),
             * k2 (b_slot), val (target) from MEMORY. (StoreElemChainV /
             * StoreLValueChainV read a key RUN of unknown-here size, so they hit
             * the default -> no caching for the run, which is safe.) */
            bad(in.target2); bad(static_cast<int>(in.a_dual_lo()));
            bad(in.b_slot()); bad(in.target);
            break;
        case OpCode::MoveV:
            /* jit_move reads/writes slots[src]/slots[dst] from MEMORY, so both
             * must be current (not pinned in a register). Disqualify them; the
             * rest of the run can still cache (the call saves/restores r10/r11).
             * A MoveV also copies any type - never an int-scalar cache anyway. */
            bad(in.target); bad(in.target2);
            break;
        case OpCode::SubscriptV:
            /* the fragment leas &slot for base/idx/dst - all must be current. */
            bad(in.target2); bad(in.a_slot()); bad(in.target);
            break;
        case OpCode::SliceV:
            /* jit_slice reads base/start/end + writes dst from MEMORY (via
             * g_current_ctx->frame); start/end are int bounds that COULD be
             * int-cached (a loop counter as a[i:j]), so disqualify them. */
            bad(in.target2); bad(in.target);
            if (in.a_slot() >= 0) bad(static_cast<int>(in.a_slot()));
            if (in.b_slot() >= 0) bad(static_cast<int>(in.b_slot()));
            break;
        case OpCode::MemberV:
        case OpCode::LoadMemberInt:
        case OpCode::LoadMemberFloat:
            /* base read + dst written from memory by the helper (the member
             * key is a baked pool entry, not a slot). */
            bad(in.target2); bad(in.target);
            break;
        case OpCode::ArrLen:
            /* dst written from memory; base (target2) holds an array reference,
             * never an int - both must stay in memory. */
            bad(in.target); bad(in.target2);
            break;
        case OpCode::DictLoadInt:
        case OpCode::DictLoadFloat:
            /* dst written from memory; base (target2) holds a dict reference;
             * a subscript key temp (a_slot when !a_is_lit) is lea'd, so it must
             * be current. A member key (a_is_lit) is a baked const, no slot. */
            bad(in.target); bad(in.target2);
            if (!in.a_is_lit()) bad(in.a_slot());
            break;
        case OpCode::MakeClosureV:
            /* A closure SNAPSHOTS its capture sources from frame MEMORY, but
             * the captured slots are the closure's (runtime) capture list - the
             * emitter can't enumerate them to disqualify individually. An N5
             * register-cached capture source (e.g. a hot loop accumulator the
             * closure captures) would be STALE in memory when jit_make_closure
             * reads it. BRACKET it (flush before / reload after) so the rest of
             * the fragment keeps its pinned registers. */
            mark_barrier(pc);
            break;
        case OpCode::CallBuiltinV:
            /* A callback builtin (make_array/make_dict/find) re-enters
             * vm_dispatch and can mutate ARBITRARY slots (globals, captures,
             * the accumulator) - the emitter can't enumerate them. BRACKET it:
             * flush the pinned registers to memory before (so the builtin and
             * any callback see current values) and reload after (so a callback
             * write is picked up). Disabling pinning for the whole fragment
             * instead - the old rule - cost a hot loop its registers whenever it
             * merged into a fragment containing ANY builtin call. */
            mark_barrier(pc);
            break;
        case OpCode::MapFilterV:
            /* map/filter's callback re-enters vm_dispatch (same as a
             * callback builtin) - bracket; the boxed operand/dst slots are
             * never int candidates, disqualify defensively. */
            mark_barrier(pc);
            bad(in.a_slot()); bad(in.b_slot()); bad(in.target);
            break;
        case OpCode::CheckFuncV:
            bad(in.a_slot());            /* a func-value slot - never int */
            break;
        case OpCode::BinOpV:
        case OpCode::CmpV:
        case OpCode::LogV:
            /* boxed operands/dst read/written from memory (dyn/string - not an
             * int-scalar cache candidate anyway, but disqualify defensively). */
            if (!in.a_is_lit()) bad(in.a_slot());
            if (!in.b_is_lit()) bad(in.b_slot());
            bad(in.target);
            break;
        case OpCode::UnaryV:         /* 1-operand boxed: a_slot + dst */
            if (!in.a_is_lit()) bad(in.a_slot());
            bad(in.target);
            break;
        case OpCode::CompoundV:
            bad(in.target);              /* read + written from memory */
            if (!in.b_is_lit()) bad(in.b_slot());
            break;
        case OpCode::CoerceNumV:
            /* dst (a typed int/float coerces_dyn accumulator - COULD be a hot
             * int slot) is written from memory by the helper; an N5-cached dst
             * would be overwritten stale by the flush. src (a_slot) holds a dyn
             * value (never int-cached). Disqualify both. */
            bad(in.target); bad(in.a_slot());
            break;
        case OpCode::StoreGlobalV:
        case OpCode::StoreCaptureV:
            /* reads the rhs operand from memory (target is a GLOBAL/CAPTURE slot,
             * not a frame slot). PLAIN: `a` is the src slot; COMPOUND: `a` is the
             * rhs (a slot OR a lit - skip a lit). */
            if (!in.a_is_lit()) bad(in.a_slot());
            break;
        case OpCode::AppendV:
            /* jit_append reads the VALUE (b_lit, a frame slot - could be a cached
             * int counter `append(a, i)`) and a LOCAL arg0 array (target2 when
             * kind == a_dual_hi() == 0) from MEMORY; the dst is written. */
            if (in.a_dual_hi() == 0) bad(in.target2);
            bad(static_cast<int>(in.b_lit()));
            if (in.target >= 0) bad(in.target);
            break;
        case OpCode::MakeArrayV:
            /* jit_make_array reads the whole ELEMENT run [a_lit, a_lit+b_lit)
             * from MEMORY (an element can be a cached int counter - `[i, i*2]`
             * in a loop), and writes dst. The run IS enumerable here (base + n
             * are both in the instruction), so disqualify it precisely instead
             * of turning caching off for the whole run. */
            for (int_type i = 0; i < in.b_lit(); i++)
                bad(static_cast<int>(in.a_lit() + i));
            bad(in.target);
            break;
        case OpCode::MakeDictV:
            /* same, over the INTERLEAVED key/value run [a_lit, a_lit+2*b_lit)
             * (b_lit is the PAIR count) - a key or value can be a cached int
             * counter (`{i: i * 2}` in a loop). */
            for (int_type i = 0; i < 2 * in.b_lit(); i++)
                bad(static_cast<int>(in.a_lit() + i));
            bad(in.target);
            break;
        case OpCode::StructCtorV:
            /* jit_struct_ctor reads the FIELD run [a_lit, a_lit+b_lit) from
             * MEMORY (a field arg can be a cached int counter - `P(i, i*2)`)
             * and reads+writes dst (the H1 reuse inspects the current value). */
            for (int_type i = 0; i < in.b_lit(); i++)
                bad(static_cast<int>(in.a_lit() + i));
            bad(in.target);
            break;
        case OpCode::LoadElemBool:
            /* INLINED (no helper); the index is read cache-aware
             * (load_index_r9), so it stays a countable int use like
             * LoadElemInt's. */
            bad(in.target); bad(in.target2);
            if (!in.a_is_lit()) usei(in.a_slot());
            break;
        case OpCode::StrLen:
        case OpCode::LoadStrChar:
        case OpCode::LoadStructFieldInt:
        case OpCode::LoadStructFieldFloat:
        case OpCode::LoadStructElemV:
        case OpCode::LoadElemValue:
            /* The helper WRITES dst and READS the base from MEMORY, so both
             * must stay in memory. The INDEX is different: the emitter
             * materializes it with the cache-aware load_operand BEFORE the
             * call, so it is read from its register when pinned - it stays a
             * countable int use (this is the foreach COUNTER, the slot N5 most
             * wants to cache; disqualifying it would lose the loop's caching). */
            bad(in.target); bad(in.target2);
            if (in.op != OpCode::StrLen && !in.a_is_lit())
                usei(in.a_slot());
            break;
        case OpCode::StructCtorBoxedV:
        case OpCode::MakeStructArrayV:
            /* Their run LENGTH is not derivable from the instruction alone -
             * the boxed ctor's arg count lives in the boxed_ctors pool, and the
             * struct-array literal's run is n * nfields (the field count is in
             * the DEF) - and pick_cached_slots has no chunk to resolve either.
             * Per the MakeClosureV rule (a helper reading slots the emit site
             * cannot name), BRACKET them with flush/reload. */
            mark_barrier(pc);
            break;
        case OpCode::LoadBuiltinV:
        case OpCode::LoadConstV:
        case OpCode::LoadLiteralObjV:
        case OpCode::LoadCaptureV:
        case OpCode::LoadGlobalV:
            /* writes dst from memory (a builtin / const / literal / capture /
             * global value, not an int); target2 is a compile-time index (the
             * global slot for LoadGlobalV), not a frame slot. */
            bad(in.target);
            break;
        case OpCode::JumpUnlessElemInt:
            /* reads the base (an array ref) from memory; the INDEX goes through
             * the cache-aware load_index_r9, so it stays a countable int use -
             * it is the loop counter. Nothing is written. */
            bad(in.target2);
            if (!in.a_is_lit()) usei(in.a_slot());
            break;
        case OpCode::CmpIntV:
            /* both operands are plain int reads (cache-aware load_operand), so
             * they stay countable int uses; the dst holds a BOOL, written to
             * memory - never an int-cache candidate. Without this case the op
             * hit the `default` and disabled pinning for the WHOLE run. */
            if (!in.a_is_lit()) usei(in.a_slot());
            if (!in.b_is_lit()) usei(in.b_slot());
            bad(in.target);
            break;
        case OpCode::JumpUnlessTrueV:
            /* jit_is_true reads the CONDITION slot (target2) from MEMORY via
             * g_current_ctx, so it must be current - and it holds a boxed value
             * (dyn/bool/string), never an int-scalar cache candidate anyway. */
            bad(in.target2);
            break;
        case OpCode::StructFieldAddInt:
            /* idx (a) is materialized cache-aware pre-call; `other`
             * (b_dual_hi) and the dst are read/written via the cache-aware
             * read_slot/write_slot in the FRAGMENT - all stay countable int
             * uses (the dst is the reduction's hot accumulator). Only the
             * base array slot must stay in memory. */
            bad(in.target2);
            if (!in.a_is_lit()) usei(in.a_slot());
            usei(static_cast<int>(in.b_dual_hi()));
            usei(in.target);
            break;
        case OpCode::ForStepElemInt:
            /* counter (target2) stepped + used as the index cache-aware;
             * bound (a) cache-aware; the elem dst (b_dual_hi) written via
             * write_slot. The base array slot stays in memory. */
            bad(static_cast<int>(in.b_dual_lo()));
            usei(in.target2);
            if (!in.a_is_lit()) usei(in.a_slot());
            usei(static_cast<int>(in.b_dual_hi()));
            break;
        case OpCode::EmplaceStruct:
            /* the field-value RUN length lives in the emplace_sites pool
             * (nfields), not in the instruction - the emitter cannot
             * enumerate the slots the helper reads. BRACKET it (the
             * StructCtorBoxedV rule; not a branch, so the reload always
             * runs). */
            mark_barrier(pc);
            break;
        case OpCode::DictIterInit:
            /* reads the dict slot (target2) from memory; target is the iter_id,
             * not a slot. The iterator state itself is activation-side. */
            bad(in.target2);
            break;
        case OpCode::DictIterNext:
            /* the helper WRITES the key/value slots (a/b; -1 == unbound) from
             * memory - a cached int value slot (a dict<str,int> loop's v used
             * in int arith) would be stale. target/target2 are end_pc/iter_id,
             * not slots. */
            bad(in.a_slot()); bad(in.b_slot());
            break;
        case OpCode::ForeachDynInit:
            /* reads the container slot (target2) from memory; a/b are lits
             * (the shape + the unpack_targets pool index). */
            bad(in.target2);
            break;
        case OpCode::ForeachDynNext:
            /* the helper writes the POOL-listed target slots - which the
             * emitter cannot enumerate (the slot list lives in unpack_targets,
             * and pick_cached_slots has no chunk). A barrier is not an option
             * either: this is a BRANCH (a taken end_pc would skip the reload).
             * Per the MakeClosureV can't-enumerate rule, cache nothing. */
            return {};
        case OpCode::CmpFloatV:
            /* float operands + a bool dst - none is an int-cache candidate;
             * all read/written from memory. */
            if (!in.a_is_lit()) bad(in.a_slot());
            if (!in.b_is_lit()) bad(in.b_slot());
            bad(in.target);
            break;
        case OpCode::JumpIfNotNoneV:
            /* reads the lhs slot's type tag from memory (a boxed value). */
            bad(in.a_slot());
            break;
        case OpCode::DeclConstV:
            /* the helper reads src (a) + writes the dst LValue from memory;
             * for a GLOBAL dst (target2==1) `target` is a global slot, not a
             * frame slot. */
            bad(in.a_slot());
            if (in.target2 == 0) bad(in.target);
            break;
        case OpCode::DefinedGlobalV:
            /* writes the bool dst from memory (target2 is the global slot). */
            bad(in.target);
            break;
        case OpCode::ThrowRuntimeV:
            break;                       /* no slots - it only exits */
        case OpCode::PushHandler:
        case OpCode::PopHandler:
        case OpCode::SetPend:
        case OpCode::EndFinally:
            break;                       /* pure activation state - no slots */
        case OpCode::CatchTest:
        case OpCode::Reraise:
        case OpCode::Throw:
            break;                       /* unconditional exits - no slots */
        case OpCode::UnpackElemInt:
        case OpCode::UnpackElemFloat:
        case OpCode::UnpackElemValue:
            /* the helper writes the CONSECUTIVE dst run [target, target+N)
             * (N = b_lit, enumerable) and reads the base from memory; the
             * index is materialized cache-aware pre-call (the foreach
             * counter stays a countable int use). */
            bad(in.target2);
            for (int_type k = 0; k < in.b_lit(); k++)
                bad(in.target + static_cast<int>(k));
            if (!in.a_is_lit()) usei(in.a_slot());
            break;
        case OpCode::UnpackElemTargets:
        case OpCode::MultiUnpackV:
            /* the target slots live in the unpack_targets pool - not
             * enumerable here (no chunk). BRACKET (neither is a branch). */
            mark_barrier(pc);
            break;
        case OpCode::IncDecCheckedV:
            /* the helper RMWs the slot in memory; a LOCAL (kind target2==0)
             * must not be register-pinned. */
            if (in.target2 == 0) bad(in.target);
            break;
        case OpCode::IncDecElemCheckedV:
            /* base (target2, when a LOCAL - kind is in target) + the key
             * temp (a) are read from memory by the helper. */
            if (in.target == 0) bad(in.target2);
            bad(in.a_slot());
            break;
        case OpCode::IncDecMemberCheckedV:
            if (in.target == 0) bad(in.target2);
            break;
        case OpCode::IncDecChainV:
            /* the chain's key temps live in the incdec_chains pool - not
             * enumerable here. BRACKET (not a branch). */
            mark_barrier(pc);
            break;
        case OpCode::Jump:
        case OpCode::Halt:               /* returns none - reads/writes no slot */
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

/* The shared REG-COUNT shift core (rax = value, rcx = count): a negative
 * count RAISES InvalidValueEx (JR_NEG_SHIFT), a count >= 64 SATURATES (0 for
 * shl/ushr, a full sign-fill for the arithmetic shr), else the machine shift
 * by cl - exactly bit_shl/bit_shr/bit_ushr (bitops.h). Used by the IntShlRR/
 * IntShrRR reg branch AND the generic-IntBin shift arms, so the two cannot
 * drift. Result in rax. */
static void emit_reg_shift(Emitter &e, Op aop, uint32_t pc)
{
    /* D3 /4 shl, /7 sar (signed shr), /5 shr (ushr); C1 imm forms same /r */
    const uint8_t modrm = aop == Op::shl ? 0xE0
                        : aop == Op::shr ? 0xF8 : 0xE8;
    /* test rcx,rcx; js Lraise (negative count throws) */
    e.u8(0x48); e.u8(0x85); e.u8(0xC9);
    e.u8(0x0F); e.u8(0x88);
    const size_t js = e.pos(); e.u32(0);
    /* cmp rcx,64; jl Lnorm */
    e.u8(0x48); e.u8(0x83); e.u8(0xF9); e.u8(64);
    e.u8(0x0F); e.u8(0x8C);
    const size_t jl = e.pos(); e.u32(0);
    if (aop == Op::shr) {
        e.u8(0x48); e.u8(0xC1); e.u8(0xF8); e.u8(63);    /* sar rax,63 */
    } else {
        e.u8(0x31); e.u8(0xC0);                          /* xor eax,eax */
    }
    e.u8(0xE9);
    const size_t jdone = e.pos(); e.u32(0);
    e.patch32(js, static_cast<uint32_t>(e.pos() - (js + 4)));
    emit_raise(e, JR_NEG_SHIFT, pc);                     /* negative count:
                                                          * RAISE InvalidValue
                                                          * (no re-interpret) */
    e.patch32(jl, static_cast<uint32_t>(e.pos() - (jl + 4)));
    e.u8(0x48); e.u8(0xD3); e.u8(modrm);                 /* shl/sar/shr rax,cl */
    e.patch32(jdone, static_cast<uint32_t>(e.pos() - (jdone + 4)));
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
/* Load an op's int INDEX operand into r9, CACHE-AWARE: if the slot is pinned in
 * an N5 register, move it from there rather than re-reading memory. Reading
 * memory would force the index to be disqualified from pinning for the whole
 * fragment - which is exactly how a hot loop counter lost its register once
 * runs began merging (mov_rr encodes r9 as a destination directly). */
/* Load a SLOT's int payload into r9, cache-aware. */
static void load_slot_r9(Emitter &e, int slot)
{
    const int cr = e.creg(slot);
    if (cr >= 0)
        e.mov_rr(9 /* r9 */, static_cast<uint8_t>(cr));
    else
        e.mov_r9_slot(slot_addr(slot).payload);
}

static void load_index_r9(Emitter &e, const Instr &in)
{
    if (in.a_is_lit()) {
        e.movabs_r9(static_cast<uint64_t>(in.a_lit()));
        return;
    }
    load_slot_r9(e, in.a_slot());
}

/* The index bounds check with the NEGATIVE-index WRAP on the COLD side
 * (r9 = idx, rdx = count): the HOT path is the ORIGINAL single unsigned
 * compare (a non-negative in-range index falls straight through to the load
 * - zero added cost; the first wrap version put a sign test on the hot path
 * and cost 18_foreach_array +3.3% Ir). The unsigned-out-of-range COLD side
 * distinguishes: negative -> `idx += size` (EXACTLY the interpreter - the
 * jit used to raise OOB for a[-1] where both interpreters returned the
 * wrapped element, a REAL pre-existing divergence) + a re-check; a
 * non-negative or still-negative-after-wrap index RAISES OutOfBounds (this
 * pc's caret), matching the interpreter's `idx < 0 ||` check. */
static void emit_elem_bounds_or_wrap(Emitter &e, uint32_t pc)
{
    e.cmp_r9_rdx();
    const size_t j_load = e.j32(0x72);       /* jb: in range -> the load */
    /* cold: unsigned out-of-range - negative (wrap) or genuine OOB */
    e.u8(0x4D); e.u8(0x85); e.u8(0xC9);      /* test r9, r9 */
    {
        const size_t j_wrap = e.j8(0x78);    /* js -> the wrap */
        emit_raise(e, JR_OOB, pc);           /* non-negative OOB */
        e.patch8(j_wrap, e.pos());
    }
    e.u8(0x49); e.u8(0x01); e.u8(0xD1);      /* add r9, rdx (idx += size) */
    e.cmp_r9_rdx();
    raise_unless(e, 0x72, JR_OOB, pc);       /* doubly-negative -> OOB */
    e.patch32_here(j_load);
}

/* The per-kind FLAT element read tail (rax = the shobj): data -> rcx,
 * count -> rdx, the index -> r9 (from the op's a-operand when `idx_in`, else
 * cache-aware from `idx_slot`), then emit_elem_bounds_or_wrap (hot unsigned
 * check; cold negative wrap / OOB raise), else the element lands in RAX.
 * Shared by every flat int/bool element reader so the semantics cannot
 * drift. */
static void emit_flat_int_tail(Emitter &e, uint32_t pc, bool bools,
                               const Instr *idx_in, int idx_slot)
{
    const JitLayout &L = jit_layout();
    e.mov_rcx_rax(L.data_off);
    e.mov_rdx_rax(L.data_off + 8);
    e.sub_rdx_rcx();
    if (!bools)
        e.sar_rdx_3();
    if (idx_in)
        load_index_r9(e, *idx_in);
    else
        load_slot_r9(e, idx_slot);
    emit_elem_bounds_or_wrap(e, pc);
    if (bools)
        e.load_elem_byte();                  /* movzx eax,[rcx+r9] */
    else
        e.load_elem_int();                   /* mov rax,[rcx+r9*8] */
}

/*
 * Emit the INT-semantics element read `arr[idx]` -> RAX, shared by LoadElemInt
 * and the JumpUnlessElemInt fusion (`if (arr[i])`). Navigates slot -> shobj ->
 * kind + data, unsigned bounds-checks, and reads the element - accepting BOTH
 * flat `ints` (8-byte) and flat `bools` (1-byte, 0/1), exactly what the
 * interpreter accepts for these ops. A non-array / SLICE / other-kind base or
 * a NEGATIVE index BAILS (the interpreter re-runs the op - and wraps the
 * negative); an out-of-range index RAISES OutOfBounds with the op's caret
 * (approach A - no re-interpret). Clobbers rax/rcx/rdx/r9.
 */
static void emit_elem_int_read(Emitter &e, const Instr &in, uint32_t pc)
{
    const JitLayout &L = jit_layout();
    const SlotAddr base = slot_addr(in.target2);
    e.load(RAX, base.type);                  /* base an array? */
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    e.bail_unless(0x74, pc);                 /* je (== t_arr) */
    e.cmp_byte_rdi(base.payload + L.slice_off, 0);   /* not a slice? */
    e.bail_unless(0x74, pc);                 /* je (slice==0) */
    e.load(RAX, base.payload);               /* rax = shobj ptr */
    e.cmp_byte_rax(L.kind_off, L.kind_ints);
    const size_t j_ints = e.j32(0x74);       /* je -> the 8-byte path */
    e.cmp_byte_rax(L.kind_off, L.kind_bools);
    e.bail_unless(0x74, pc);                 /* je (bools) else BAIL */
    /* flat bools: 1-byte elements, so the count is the raw pointer difference
     * (no sar) and the load is a movzx. */
    emit_flat_int_tail(e, pc, /*bools=*/true, &in, -1);
    const size_t j_done = e.j32(0xEB);
    /* flat ints */
    e.patch32_here(j_ints);
    emit_flat_int_tail(e, pc, /*bools=*/false, &in, -1);
    e.patch32_here(j_done);
}

/* The BASE GATE alone (no read): bail unless the slot holds a NON-slice flat
 * int/bool array. ForStepElemInt must run every bail-able check BEFORE it
 * steps the counter - a bail re-runs the WHOLE op, and a post-step bail would
 * DOUBLE-STEP. Clobbers rax/r9. */
static void emit_elem_base_gate(Emitter &e, int base_slot, uint32_t pc)
{
    const JitLayout &L = jit_layout();
    const SlotAddr base = slot_addr(base_slot);
    e.load(RAX, base.type);
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    e.bail_unless(0x74, pc);
    e.cmp_byte_rdi(base.payload + L.slice_off, 0);
    e.bail_unless(0x74, pc);
    e.load(RAX, base.payload);
    e.cmp_byte_rax(L.kind_off, L.kind_ints);
    const size_t j_ok = e.j8(0x74);
    e.cmp_byte_rax(L.kind_off, L.kind_bools);
    e.bail_unless(0x74, pc);
    e.patch8(j_ok, e.pos());
}

static void emit_store_elem(Emitter &e, const Chunk &ck, const Instr &in,
                            uint32_t pc, size_t old_pc, bool is_float)
{
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
    emit_exc_stamp(e, ck, old_pc);        /* cold: the op's own caret */
    e.exit_pc(pc);                        /* raised: EnterNative raises exc */
    e.patch8(j_ok, e.pos());
}

/* Approach A: a dict element store d[k] = v as a CALL to jit_dict_store. The
 * key/value are BOXED EvalValues in frame slots, so the fragment just leas
 * their addresses (EvalValue is the first LValue member). SysV: rdi=base
 * LValue*, rsi=key EvalValue*, rdx=val EvalValue*, rcx=op. rdi written LAST
 * (the key/val leas read rdi=slots). The key/val/base slots are disqualified
 * from register caching (pick_cached_slots) so their slots hold CURRENT
 * EvalValues - a cached int key (a counter used as d[i]) would be stale. */
static void emit_dict_store(Emitter &e, const Chunk &ck, const Instr &in,
                            uint32_t pc, size_t old_pc)
{
    const auto off = [](int slot) {
        return static_cast<int32_t>(static_cast<long>(slot)
                                    * static_cast<long>(sizeof(LValue)));
    };
    emit_call_prologue(e);
    e.lea(RSI, off(in.a_slot()));         /* rsi = &slot[key]  (rdi=slots) */
    e.lea(RDX, off(in.b_slot()));         /* rdx = &slot[val]  (rdi=slots) */
    e.lea_rdi(off(in.target2));           /* rdi = &slot[base] (LAST) */
    e.movabs(RCX, static_cast<uint64_t>(static_cast<int>(in.aop)));
    e.call_relocs.push_back(
        { e.pos(), reinterpret_cast<const void *>(jit_dict_store) });
    e.u8(0xE8); e.u32(0);
    emit_call_epilogue(e);
    e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
    const size_t j_ok = e.j8(0x74);
    emit_exc_stamp(e, ck, old_pc);        /* cold: the op's own caret */
    e.exit_pc(pc);
    e.patch8(j_ok, e.pos());
}

/* Emit one op; returns false if (unexpectedly) unhandled. */
/* fwd (defined with the run-building code below): the #55 native-call gate
 * the CallV emit uses to pick the direct-call vs the M5 sync form. */
static bool callv_native_ok(const Instr &in, const JitCtx *jc);

static bool emit_op(Emitter &e, const Chunk &ck, const Instr &in,
                    uint32_t pc, const JitCtx *jc, size_t old_pc)
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

    case OpCode::IntBin:
        /* generic IntBin - ALL arms (jit_op_eligible admits exactly the aops
         * the interpreter's switch handles). UNLIKE the RR/RI forms, BOTH
         * operands may be slot OR imm (that's exactly why `0 - i` stays
         * generic - operand a is the imm), so load_operand on both
         * (cache-aware for a slot). The non-throwing arms are a bare op_rr;
         * div/mod check the divisor and RAISE DivisionByZeroEx (JR_DIV0 -
         * caret from the loc side table at this pc, byte-identical to the
         * interpreted throw) then cqo+idiv (an INT64_MIN/-1 divisor traps in
         * idiv - EXACTLY the interpreter's own UB, see TypeInt::div); the
         * shift arms share emit_reg_shift with the RR forms (negative count
         * -> JR_NEG_SHIFT raise, >= 64 saturates - the bit_shl/bit_shr/
         * bit_ushr semantics). Only the non-throwing arms are
         * op_fully_native. */
        switch (in.aop) {
        case Op::div: case Op::mod: case Op::shl: case Op::shr: case Op::ushr:
            /* execution proof - BEFORE the operand loads: bump_op emits
             * `movabs rax, <counter>; inc [rax]`, i.e. it CLOBBERS rax (a
             * counter inc after load_operand wiped the dividend - a wrong
             * 16/3, caught by the differential + read in -vdj). */
            e.bump_op(OpCode::IntBin);
            break;
        default:
            break;
        }
        load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        switch (in.aop) {
        case Op::div: case Op::mod:
            /* test rcx,rcx; raise DIV0 unless nonzero */
            e.u8(0x48); e.u8(0x85); e.u8(0xC9);
            raise_unless(e, 0x75 /* jnz */, JR_DIV0, pc);
            e.u8(0x48); e.u8(0x99);              /* cqo */
            e.u8(0x48); e.u8(0xF7); e.u8(0xF9);  /* idiv rcx */
            write_slot(e, ck, in.aop == Op::div ? RAX : RDX, in.target, pc);
            return true;
        case Op::shl: case Op::shr: case Op::ushr:
            emit_reg_shift(e, in.aop, pc);
            break;
        default:
            op_rr(e, in.aop);
            break;
        }
        write_slot(e, ck, RAX, in.target, pc);
        return true;

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
            emit_reg_shift(e, shl ? Op::shl : Op::shr, pc);
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
        uint8_t fop;   /* 0x58 addsd / 0x5C subsd / 0x59 mulsd / 0x5E divsd */
        const bool is_mod =
            in.op == OpCode::FloatBin && in.aop == Op::mod;
        switch (in.op) {
        case OpCode::FloatAddRR: case OpCode::FloatAddRI: fop = 0x58; break;
        case OpCode::FloatSubRR: case OpCode::FloatSubRI: fop = 0x5C; break;
        case OpCode::FloatMulRR: case OpCode::FloatMulRI: fop = 0x59; break;
        default:   /* FloatBin: by aop (add/sub/mul/div/mod - eligible) */
            fop = in.aop == Op::plus ? 0x58
                : in.aop == Op::minus ? 0x5C
                : in.aop == Op::times ? 0x59 : 0x5E;
            break;
        }
        if (fop == 0x5E || is_mod)
            e.bump_op(OpCode::FloatBin);   /* execution proof for the div/mod
                                            * arms (BEFORE any load - bump_op
                                            * clobbers rax) */
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc);
        emit_float_load(e, X1, in.b_is_lit(), in.b_flit(), in.b_slot(), pc);
        if (fop == 0x5E || is_mod) {
            /* float DIV and MOD throw DivisionByZeroEx on a +-0.0 divisor
             * (TypeFloat::div/mod: fpclassify(rhs) == FP_ZERO). Test the
             * divisor's BITS with the sign stripped: movq rax, xmm1;
             * shl rax, 1 - zero iff the value is +-0.0 (NaN/inf/denormal
             * bits stay nonzero and divide, exactly fpclassify's answer).
             * A bits test avoids a ucomisd NaN pitfall (unordered sets ZF,
             * so a bare `je` would wrongly raise on a NaN divisor). */
            e.u8(0x66); e.u8(0x48); e.u8(0x0F); e.u8(0x7E);
            e.u8(0xC8);                            /* movq rax, xmm1 */
            e.u8(0x48); e.u8(0xD1); e.u8(0xE0);    /* shl rax, 1 */
            raise_unless(e, 0x75 /* jnz */, JR_DIV0, pc);
        }
        if (is_mod)
            /* the exact libm call TypeFloat::mod makes - x in xmm0, y in
             * xmm1 (the SysV float args); the prologue/epilogue inside
             * save rdi + any pinned cache regs. */
            emit_libm_call(e, reinterpret_cast<const void *>(
                static_cast<double (*)(double, double)>(&::fmod)));
        else
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
        if (is_float) {
            e.cmp_byte_rax(L.kind_off, L.kind_floats);
            e.bail_unless(0x74, pc);             /* je (kind matches) */
            e.mov_rcx_rax(L.data_off);           /* rcx = _M_start */
            e.mov_rdx_rax(L.data_off + 8);       /* rdx = _M_finish */
            e.sub_rdx_rcx();
            e.sar_rdx_3();                        /* rdx = element count */
            load_index_r9(e, in);                 /* cache-aware index */
            /* hot unsigned bounds check; cold negative wrap / OOB raise
             * (raising OOB for a[-1] was the same pre-existing divergence) */
            emit_elem_bounds_or_wrap(e, pc);
            e.load_elem_float();                 /* movsd xmm0,[rcx+r9*8] */
            emit_float_store(e, ck, X0, in.target, pc);
            return true;
        }
        /* The int-semantics read (ints + bools storage) is shared with the
         * JumpUnlessElemInt fusion - see emit_elem_int_read. */
        emit_elem_int_read(e, in, pc);
        write_slot(e, ck, RAX, in.target, pc);
        return true;
    }

    case OpCode::StoreElemInt:
        emit_store_elem(e, ck, in, pc, old_pc, /*is_float=*/false);
        return true;
    case OpCode::StoreElemFloat:
        emit_store_elem(e, ck, in, pc, old_pc, /*is_float=*/true);
        return true;
    case OpCode::DictStore:
        emit_dict_store(e, ck, in, pc, old_pc);
        return true;

    case OpCode::StoreElemValue:
        /* the UNIVERSAL store a[i] = v via jit_store_elem_value(kind=target,
         * base_slot=target2, idx_slot=a_slot, val_slot=b_slot, aop). The helper
         * forms the base (local/global/capture) from kind+slot + reads idx/val
         * from frame slots via g_current_ctx. Non-0 return -> exit_pc (an
         * undefined-global bail resumes interpreted; a subscript throw re-raises
         * via g_vm_jit_exc). aop -> r8 (re-materialised to t_float by the
         * epilogue AFTER the call). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.a_slot())));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.b_slot())));
        e.movabs_r8(static_cast<uint64_t>(static_cast<int>(in.aop)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_store_elem_value) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);   /* jz -> continue (0 = ok) */
            emit_exc_stamp(e, ck, old_pc); /* cold; null-checked,
                                   * so bail-safe too */
            e.exit_pc(pc);                    /* raise (exc set) or bail (unset) */
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StoreMemberV:
        /* s.f = v via jit_store_member(kind=target, base_slot=target2,
         * val_slot=b_slot, aop, mk=&member_keys[a_lit]). r8 = the pool entry
         * pointer (baked - stable across the chunk move). Non-0 -> exit_pc (bail
         * / re-raise). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.b_slot())));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int>(in.aop)));
        e.movabs_r8(reinterpret_cast<uint64_t>(&ck.member_keys[in.a_lit()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_store_member) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);   /* jz -> continue (0 = ok) */
            e.exit_pc(pc);                    /* raise (exc set) or bail (unset) */
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StoreElem2V:
        /* a[i][j] = v via jit_store_elem2(base_slot=target2, k1=a_dual_lo,
         * k2=b_slot, val=target, aop, locs=chain_locs[a_dual_hi].data()). LOCAL
         * base (no kind). r8=aop, r9=the per-step caret buffer (baked). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_dual_lo())));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.b_slot())));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs_r8(static_cast<uint64_t>(static_cast<int>(in.aop)));
        e.movabs_r9(reinterpret_cast<uint64_t>(
                        ck.chain_locs[in.a_dual_hi()].data()));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_store_elem2) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            emit_exc_stamp(e, ck, old_pc);  /* cold: own caret */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StoreElemChainV:
        /* a[k0..kn] = v via jit_store_elem_chain(kind=a_dual_hi, base=target2,
         * kbase=b_lit, val=target, aop, cl=&chain_locs[a_dual_lo]). r8=aop,
         * r9=the chain_locs entry (baked - data()/size() in the helper). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_dual_hi())));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.b_lit())));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs_r8(static_cast<uint64_t>(static_cast<int>(in.aop)));
        e.movabs_r9(reinterpret_cast<uint64_t>(&ck.chain_locs[in.a_dual_lo()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_store_elem_chain) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            emit_exc_stamp(e, ck, old_pc); /* cold; null-checked,
                                   * so bail-safe too */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StoreLValueChainV:
        /* base.step.. = v via jit_store_lvalue_chain(kind=a_dual_hi,
         * base=target2, val=target, aop, steps=&chain_steps[a_dual_lo],
         * mkeys=member_keys.data()). r8=steps entry, r9=member_keys buffer
         * (both baked). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_dual_hi())));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int>(in.aop)));
        e.movabs_r8(reinterpret_cast<uint64_t>(&ck.chain_steps[in.a_dual_lo()]));
        e.movabs_r9(reinterpret_cast<uint64_t>(ck.member_keys.data()));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_store_lvalue_chain) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            emit_exc_stamp(e, ck, old_pc); /* cold; null-checked,
                                   * so bail-safe too */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::MoveV: {
        /* De-helperize (roadmap step 6): dst = src.get() INLINE for the
         * trivial-x-trivial case - a TRIVIAL source (type->t < t_str) copied
         * over a TRIVIAL current dst is a bitwise 24-byte-payload + Type*
         * copy, no refcounts. A REFERENCE source (retain) or - when dst is
         * ref-listed - a reference current dst (release) falls to the
         * original jit_move helper. */
        const SlotAddr src = slot_addr(in.target2);
        const SlotAddr dst = slot_addr(in.target);
        e.bump_op(OpCode::MoveV);
        std::vector<size_t> jhelp;
        jhelp.push_back(emit_ref_check_jae(e, src.type));
        if (std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                               static_cast<int32_t>(in.target)))
            jhelp.push_back(emit_ref_check_jae(e, dst.type));
        e.load(RAX, src.type);
        e.load(RCX, src.payload);      e.store(RCX, dst.payload);
        e.load(RCX, src.payload + 8);  e.store(RCX, dst.payload + 8);
        e.load(RCX, src.payload + 16); e.store(RCX, dst.payload + 16);
        e.store(RAX, dst.type);
        const size_t j_done = e.j32(0xEB);
        for (const size_t s : jhelp)
            e.patch32_here(s);
        emit_call_prologue(e);
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_move) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.patch32_here(j_done);
        return true;
    }

    case OpCode::LoadBuiltinV: {
        /* De-helperize (roadmap step 6): a TRIVIAL builtin-table value (a
         * real `Builtin`, t_builtin < t_str - the immutable singleton table
         * is built before any codegen) has emit-time-constant bytes, exactly
         * the trivial LoadConstV shape (immediate stores; a ref-listed dst
         * checks its current value and falls to the helper for the release).
         * ⛔ NOT every builtin-table value is trivial: `argv` is an ARRAY in
         * that table - bitwise-copying it skipped the retain and the next
         * release FREED it under the static table (an ASan UAF at exit the
         * differential caught). A reference value keeps the helper. */
        const EvalValue &bv = builtin_slot(static_cast<int>(in.target2)).get();
        if (bv.get_type()->t < Type::t_str) {
            const SlotAddr dst = slot_addr(in.target);
            e.bump_op(OpCode::LoadBuiltinV);
            size_t j_help = 0;
            const bool reflisted = std::binary_search(
                ck.ref_slots.begin(), ck.ref_slots.end(),
                static_cast<int32_t>(in.target));
            if (reflisted)
                j_help = emit_ref_check_jae(e, dst.type);
            uint64_t q[3];
            std::memcpy(q, reinterpret_cast<const char *>(&bv)
                               + EvalValue::jit_payload_off(), sizeof q);
            e.movabs(RCX, q[0]); e.store(RCX, dst.payload);
            e.movabs(RCX, q[1]); e.store(RCX, dst.payload + 8);
            e.movabs(RCX, q[2]); e.store(RCX, dst.payload + 16);
            e.movabs(RAX, reinterpret_cast<uint64_t>(bv.get_type()));
            e.store(RAX, dst.type);
            if (!reflisted)
                return true;
            const size_t j_done = e.j32(0xEB);
            e.patch32_here(j_help);
            emit_call_prologue(e);
            e.movabs(RSI, static_cast<uint64_t>(
                              static_cast<int_type>(in.target)));
            e.movabs(RDX, static_cast<uint64_t>(
                              static_cast<int_type>(in.target2)));
            e.call_relocs.push_back(
                { e.pos(), reinterpret_cast<const void *>(jit_load_builtin) });
            e.u8(0xE8); e.u32(0);
            emit_call_epilogue(e);
            e.patch32_here(j_done);
            return true;
        }
        emit_call_prologue(e);
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_load_builtin) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;
    }

    case OpCode::LoadCaptureV: {
        /* De-helperize 6b: dst = (*ctx->captures)[idx] INLINE for a TRIVIAL
         * capture over a trivial dst - walk the ctx chain to the captures
         * data (r9), then the MoveV copy shape (src runtime ref check on
         * [r9+...]; dst current-value check only when ref-listed; the
         * 24-byte-payload + Type* copy). A reference either side falls to
         * the original jit_load_capture. */
        const SlotAddr dst = slot_addr(in.target);
        const int32_t coff = static_cast<int32_t>(
            in.target2 * static_cast<int_type>(sizeof(LValue)));
        const int32_t pv0 = slot_addr(0).payload, ty0 = slot_addr(0).type;
        e.bump_op(OpCode::LoadCaptureV);
        emit_ctx_chain_r9(e, /*cap=*/true);
        std::vector<size_t> jhelp;
        jhelp.push_back(emit_ref_check_jae_r9(e, coff + ty0));
        if (std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                               static_cast<int32_t>(in.target)))
            jhelp.push_back(emit_ref_check_jae(e, dst.type));
        e.load_r9b(RAX, coff + ty0);
        e.load_r9b(RCX, coff + pv0);      e.store(RCX, dst.payload);
        e.load_r9b(RCX, coff + pv0 + 8);  e.store(RCX, dst.payload + 8);
        e.load_r9b(RCX, coff + pv0 + 16); e.store(RCX, dst.payload + 16);
        e.store(RAX, dst.type);
        const size_t j_done = e.j32(0xEB);
        for (const size_t sj : jhelp)
            e.patch32_here(sj);
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_load_capture) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.patch32_here(j_done);
        return true;
    }

    case OpCode::LoadGlobalV:
        /* dst = gfuncs->slots[gslot] via jit_load_global (rdi=dst=target,
         * rsi=gslot=target2, rdx=&locs[i]). Returns 0 = ok, 1 = CONVEYED
         * (undefined global -> UndefinedVariableEx with the name + the baked
         * caret rides g_vm_jit_eptr; EnterNative rethrows - the exact
         * interpreted throw, pc-independent -> deletable). */
        emit_call_prologue(e);
        e.movabs(RDI, (static_cast<uint64_t>(static_cast<uint32_t>(in.target2))
                       << 32)
                      | static_cast<uint32_t>(in.target));
        e.movabs(RSI, reinterpret_cast<uint64_t>(loc_entry_addr(ck, old_pc)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_load_global) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);   /* jz -> continue (0 = ok) */
            e.exit_pc(pc);                    /* bail: interpreter re-throws */
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::SliceV:
        /* dst = base[start:end] via jit_slice (rdi=base_slot=target2,
         * rsi=start_slot=a_slot, rdx=end_slot=b_slot, rcx=dst_slot=target). The
         * helper reads the 4 frame slots via g_current_ctx (start/end == -1 ->
         * none). Throws TypeErrorEx -> g_vm_jit_exc + return 1 -> exit_pc so
         * EnterNative re-raises (caret from the loc side table). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.a_slot())));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.b_slot())));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_slice) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);   /* jz -> continue (0 = no raise) */
            emit_exc_stamp(e, ck, old_pc);    /* cold: the op's own caret */
            e.exit_pc(pc);                    /* raised: EnterNative re-raises */
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::LoadConstV: {
        /* De-helperize (roadmap step 6): a TRIVIAL const's BYTES are known at
         * EMIT time - store them as immediates (3 payload quads + the Type*),
         * zero loads, zero calls. Only a ref-listed dst needs the runtime
         * current-value check (a reference dst's release needs C++ -> the
         * helper). A REFERENCE const (a string - needs a retain) keeps the
         * helper call unconditionally. */
        const EvalValue &v = ck.consts[in.target2];
        if (v.get_type()->t < Type::t_str) {
            const SlotAddr dst = slot_addr(in.target);
            e.bump_op(OpCode::LoadConstV);
            size_t j_help = 0;
            const bool reflisted = std::binary_search(
                ck.ref_slots.begin(), ck.ref_slots.end(),
                static_cast<int32_t>(in.target));
            if (reflisted)
                j_help = emit_ref_check_jae(e, dst.type);
            uint64_t q[3];
            std::memcpy(q, reinterpret_cast<const char *>(&v)
                               + EvalValue::jit_payload_off(), sizeof q);
            e.movabs(RCX, q[0]); e.store(RCX, dst.payload);
            e.movabs(RCX, q[1]); e.store(RCX, dst.payload + 8);
            e.movabs(RCX, q[2]); e.store(RCX, dst.payload + 16);
            e.movabs(RAX, reinterpret_cast<uint64_t>(v.get_type()));
            e.store(RAX, dst.type);
            if (!reflisted)
                return true;
            const size_t j_done = e.j32(0xEB);
            e.patch32_here(j_help);
            emit_call_prologue(e);
            e.movabs(RSI, static_cast<uint64_t>(
                              static_cast<int_type>(in.target)));
            e.movabs(RDX, reinterpret_cast<uint64_t>(&ck.consts[in.target2]));
            e.call_relocs.push_back(
                { e.pos(), reinterpret_cast<const void *>(jit_load_const) });
            e.u8(0xE8); e.u32(0);
            emit_call_epilogue(e);
            e.patch32_here(j_done);
            return true;
        }
        emit_call_prologue(e);
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RDX, reinterpret_cast<uint64_t>(&ck.consts[in.target2]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_load_const) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;
    }

    case OpCode::LoadLiteralObjV:
        /* dst = eval_literal_obj(literal_objs[idx]) via jit_load_literal_obj
         * (rdi=slots, rsi=dst, rdx=lo). lo = &ck.literal_objs[idx] (pool BUFFER
         * addr, stable across the chunk move). Never throws. */
        emit_call_prologue(e);
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RDX,
                 reinterpret_cast<uint64_t>(&ck.literal_objs[in.target2]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_load_literal_obj) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::ArrLen:
        /* n = size(frame[base]) via jit_arr_len (rdi=slots, rsi=dst=target,
         * rdx=base=target2). Base is a proven flat array -> never throws. */
        emit_call_prologue(e);
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_arr_len) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::DictLoadInt:
    case OpCode::DictLoadFloat: {
        /* d.k / d[k] via jit_dict_load(dst, base_slot, key, is_int). The key
         * ptr (rdx) is computed FIRST while rdi still = slots: a member bakes
         * &ck.consts[idx] (a_is_lit), a subscript leas &slot[a_slot]. Then rdi
         * = dst (overwrites slots - OK now), rsi = base_slot, rcx = is_int.
         * Throws -> test eax + exit_pc so EnterNative re-raises with the loc. */
        emit_call_prologue(e);
        if (in.a_is_lit())
            e.movabs(RDX,
                     reinterpret_cast<uint64_t>(&ck.consts[in.a_lit()]));
        else
            e.lea(RDX, static_cast<int32_t>(static_cast<long>(in.a_slot())
                                            * static_cast<long>(sizeof(LValue))));
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), in.op == OpCode::DictLoadInt
                  ? reinterpret_cast<const void *>(jit_dict_load_int)
                  : reinterpret_cast<const void *>(jit_dict_load_float) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok = e.j8(0x74);       /* jz -> continue (0 = no raise) */
        emit_exc_stamp(e, ck, old_pc);        /* cold: the op's own caret */
        e.exit_pc(pc);                        /* raised: EnterNative re-raises */
        e.patch8(j_ok, e.pos());
        return true;
    }

    case OpCode::MakeClosureV:
        /* dst = FuncObject(def, ctx) via jit_make_closure (rdi=dst, rsi=def).
         * def = the closure_defs[idx] FuncDescriptor* baked as a VALUE (a
         * stable program-lifetime address, like CallV's callee). The helper
         * uses g_current_ctx->frame, not the slots arg. Never throws. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI,
                 reinterpret_cast<uint64_t>(ck.closure_defs[in.target2]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_make_closure) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::MakeArrayV:
        /* dst = build_array_from_values(run[a_lit .. +b_lit), hint=target2) via
         * jit_make_array (rdi=dst, rsi=run base, rdx=n, rcx=hint). The helper
         * reads the element run from MEMORY via g_current_ctx->frame (so those
         * slots are N5-disqualified). Never throws -> no status check. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
        e.movabs(RCX, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_make_array) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::MakeDictV:
        /* dst = build_dict_from_pairs(run[a_lit .. +2*b_lit), b_lit pairs) via
         * jit_make_dict (rdi=dst, rsi=run base, rdx=npairs). An unhashable key
         * throws -> test eax + exit_pc so EnterNative re-raises with the
         * literal's caret from the loc side table. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_make_dict) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::DictIterInit:
        /* pin the dict + iterator=begin() via jit_dict_iter_init (rdi=iter_id,
         * rsi=dict slot). The dict is proven -> never throws. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_dict_iter_init) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::ForeachDynInit:
        /* dispatch the dyn container once via jit_foreach_dyn_init
         * (rdi=iter_id, rsi=container slot, rdx=shape, rcx=&unpack_targets[i]
         * - a pool-element address, stable across the chunk's std::move). A
         * non-container throws -> test eax + exit_pc so EnterNative re-raises
         * with the container's caret from the loc side table. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RCX, reinterpret_cast<uint64_t>(
                          &ck.unpack_targets[in.b_lit()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_foreach_dyn_init) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StructFieldAddInt: {
        /* dst = other + a[i].f: the field read via the never-throwing
         * jit_struct_field_add_int (rdi=base slot, rsi=idx VALUE - loaded
         * cache-aware BEFORE the prologue, rdx=field idx), then the ADD and
         * the dst write IN the fragment (rax survives the epilogue), so the
         * reduction's accumulator dst stays cache-aware. */
        load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.mov_rr(RSI, RAX);
        e.movabs(RDX, static_cast<uint64_t>(
                          static_cast<int_type>(in.b_dual_lo())));
        e.call_relocs.push_back(
            { e.pos(),
              reinterpret_cast<const void *>(jit_struct_field_add_int) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        read_slot(e, RCX, in.b_dual_hi());        /* other */
        op_rr(e, Op::plus);                        /* rax += rcx */
        write_slot(e, ck, RAX, in.target, pc);
        return true;
    }

    case OpCode::EmplaceStruct:
        /* append(struct_arr, Ctor(args)) via jit_emplace_struct(dst,
         * base_slot, kind, &emplace_sites[idx], run base) - `a` packs
         * kind | idx << 2 (the interpreter's layout); r8 carries the 5th
         * arg (the epilogue re-materializes r8 = t_float after the call).
         * A throw -> test eax + exit_pc re-raise. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(in.a_lit() & 3));
        e.movabs(RCX, reinterpret_cast<uint64_t>(
                          &ck.emplace_sites[in.a_lit() >> 2]));
        e.movabs_r8(static_cast<uint64_t>(in.b_lit()));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_emplace_struct) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StructCtorV:
    case OpCode::MakeStructArrayV:
        /* jit_struct_ctor / jit_make_struct_array(def, run base, n, dst):
         * rdi = the struct_defs[target2] StructTypeDef* baked as a VALUE (a
         * program-lifetime address, like MakeClosureV's descriptor), rsi = the
         * field/element run base, rdx = the field count (ctor) or ELEMENT count
         * (array literal), rcx = dst. A defensive coerce throw -> test eax +
         * exit_pc so EnterNative re-raises with the loc side table's caret. */
        emit_call_prologue(e);
        e.movabs(RDI, reinterpret_cast<uint64_t>(ck.struct_defs[in.target2]));
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(
                           in.op == OpCode::StructCtorV
                               ? jit_struct_ctor : jit_make_struct_array) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::CmpIntV: {
        /* dst = (a <cmp> b) as a REAL bool. Fully inline: two cache-aware
         * operand reads, one `cmp`, a `setcc` for the op's condition (the
         * setcc opcode is the near-jcc opcode + 0x10, so the existing cc_for
         * table drives it), `movzx` to a clean 0/1, then the bool two-store.
         * Never faults -> op_fully_native. */
        load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
        load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        e.cmp_rax_rcx();                          /* cmp rax, rcx */
        e.u8(0x0F);
        e.u8(static_cast<uint8_t>(cc_for(in.aop).near_op + 0x10));
        e.u8(0xC0);                               /* setcc al */
        e.u8(0x0F); e.u8(0xB6); e.u8(0xC0);       /* movzx eax, al */
        store_dst_bool(e, ck, RAX, in.target);
        return true;
    }

    case OpCode::CmpFloatV: {
        /* dst = (a <cmp> b) over FLOATS as a REAL bool: the CmpIntV shape
         * with the ucomisd operand-SWAP trick (see float_cmp), so an
         * unordered (NaN) compare correctly yields FALSE for the ordering
         * compares. setcc condition = the INVERSE of the branch-if-false
         * near op (near_op ^ 1), + 0x10 for the setcc encoding. A float
         * operand read can BAIL -> not op_fully_native. */
        const FCmp fc = float_cmp(in.aop);
        e.bump_op(OpCode::CmpFloatV);        /* before loads (rax) */
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc);
        emit_float_load(e, X1, in.b_is_lit(), in.b_flit(), in.b_slot(), pc);
        if (fc.swap) e.ucomisd(X1, X0); else e.ucomisd(X0, X1);
        e.u8(0x0F);
        e.u8(static_cast<uint8_t>((fc.near_op ^ 1) + 0x10));
        e.u8(0xC0);                               /* setcc al */
        e.u8(0x0F); e.u8(0xB6); e.u8(0xC0);       /* movzx eax, al */
        store_dst_bool(e, ck, RAX, in.target);
        return true;
    }

    case OpCode::UnpackElemInt:
    case OpCode::UnpackElemFloat:
    case OpCode::UnpackElemValue:
    case OpCode::UnpackElemTargets: {
        /* jit_unpack_elem(dst_base, base_slot, idx, N|kind<<8, targets):
         * the index is materialized cache-aware BEFORE the prologue (the
         * foreach counter - rax survives the pushes); targets = the baked
         * pool entry for the Targets variant (whose `target` is the POOL
         * index, dst_base unused = -1), else null with the consecutive run
         * base in `target`. A strict-unpack throw -> test eax + exit_pc. */
        const int kind = in.op == OpCode::UnpackElemInt ? 0
                       : in.op == OpCode::UnpackElemFloat ? 1 : 2;
        const bool tg = in.op == OpCode::UnpackElemTargets;
        load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(tg ? -1 : in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.mov_rr(RDX, RAX);                    /* the index value */
        e.movabs(RCX, static_cast<uint64_t>(
                          in.b_lit() | (static_cast<int_type>(kind) << 8)));
        e.movabs_r8(tg ? reinterpret_cast<uint64_t>(
                             &ck.unpack_targets[in.target])
                       : 0);
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_unpack_elem) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;
    }

    case OpCode::MultiUnpackV:
        /* jit_multi_unpack(rval_slot, targets, coerce, aop) - pool-baked
         * target/coerce lists; a strict-length / coerce / compound throw ->
         * test eax + exit_pc re-raise. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.movabs(RSI, reinterpret_cast<uint64_t>(
                          &ck.unpack_targets[in.target]));
        e.movabs(RDX, in.b_is_lit()
                          ? reinterpret_cast<uint64_t>(
                                &ck.unpack_coerce[in.b_lit()])
                          : 0);
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int>(in.aop)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_multi_unpack) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::IncDecCheckedV:
        /* jit_incdec_checked(slot, kind, is_inc) - layout: target = slot,
         * target2 = kind, a_lit = is_inc. Bail or throw -> test + exit. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(in.a_lit()));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_incdec_checked) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::IncDecElemCheckedV:
        /* jit_incdec_elem(kind, base_slot, key_slot, is_inc, &site) -
         * layout: target = kind, target2 = base slot, a = the key temp,
         * aop = +/-, b = the incdec_sites index. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.movabs(RCX, static_cast<uint64_t>(in.aop == Op::plus ? 1 : 0));
        e.movabs_r8(reinterpret_cast<uint64_t>(&ck.incdec_sites[in.b_lit()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_incdec_elem) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::IncDecMemberCheckedV:
        /* jit_incdec_member(kind, base_slot, is_inc, &site) - same layout
         * minus the key temp (the member key rides the site). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(in.aop == Op::plus ? 1 : 0));
        e.movabs(RCX, reinterpret_cast<uint64_t>(
                          &ck.incdec_sites[in.b_lit()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_incdec_member) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::IncDecChainV: {
        /* jit_incdec_chain(root_kind, root_slot, dst, is_inc, &chain,
         * member_keys buffer) - layout: a_lit = root kind (3 = rvalue),
         * target2 = root slot, target = dst (-1 = statement), aop = +/-,
         * b = the incdec_chains index. r9 carries the 6th arg (not a
         * persistent tag reg, the nested-chain-store precedent). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RCX, static_cast<uint64_t>(in.aop == Op::plus ? 1 : 0));
        e.movabs_r8(reinterpret_cast<uint64_t>(
                        &ck.incdec_chains[in.b_lit()]));
        e.movabs_r9(reinterpret_cast<uint64_t>(ck.member_keys.data()));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_incdec_chain) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;
    }

    case OpCode::PopHandler: {
        /* INLINE handler pop (step 7a): `finish -= 4` - a VmHandler is a
         * trivial 4-byte struct, so vector pop_back is exactly the finish
         * decrement (never empty at a PopHandler by codegen construction).
         * Never throws. */
        const JitLayout &L = jit_layout();
        e.bump_op(OpCode::PopHandler);
        e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_act));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] */
        e.u8(0x48); e.u8(0x8B); e.u8(0x88);        /* mov rcx, [rax+h+8] */
        e.u32(static_cast<uint32_t>(L.act_handlers + 8));
        e.u8(0x48); e.u8(0x83); e.u8(0xE9); e.u8(4);   /* sub rcx, 4 */
        e.u8(0x48); e.u8(0x89); e.u8(0x88);        /* mov [rax+h+8], rcx */
        e.u32(static_cast<uint32_t>(L.act_handlers + 8));
        return true;
    }

    case OpCode::SetPend: {
        /* INLINE finally-pend store (step 7a): a byte store into
         * records[rec_n - 1].pend (target = the Pend ENUM value, not a
         * pc). Never throws. */
        const JitLayout &L = jit_layout();
        e.bump_op(OpCode::SetPend);
        e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_act));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] */
        e.u8(0x48); e.u8(0x8B); e.u8(0x88);        /* mov rcx, [rax+recs+0] */
        e.u32(static_cast<uint32_t>(L.act_records));   /* _M_start */
        e.u8(0x48); e.u8(0x8B); e.u8(0x90);        /* mov rdx, [rax+rec_n] */
        e.u32(static_cast<uint32_t>(L.act_rec_n));
        e.u8(0x48); e.u8(0xFF); e.u8(0xCA);        /* dec rdx */
        e.u8(0x48); e.u8(0x69); e.u8(0xD2);        /* imul rdx, rdx, size */
        e.u32(static_cast<uint32_t>(L.rec_size));
        e.u8(0x48); e.u8(0x01); e.u8(0xD1);        /* add rcx, rdx */
        e.u8(0xC6); e.u8(0x81);                    /* mov byte [rcx+pend], v */
        e.u32(static_cast<uint32_t>(L.rec_pend));
        e.u8(static_cast<uint8_t>(in.target));
        return true;
    }

    case OpCode::EndFinally: {
        /* INLINE the NORMAL path: records[rec_n-1].pend == normal (0) ->
         * fall through to Lend; RERAISE -> bail (the interpreter re-runs
         * EndFinally and raises via vm_raise). */
        const JitLayout &L = jit_layout();
        e.bump_op(OpCode::EndFinally);
        e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_act));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] */
        e.u8(0x48); e.u8(0x8B); e.u8(0x88);        /* mov rcx, [rax+recs+0] */
        e.u32(static_cast<uint32_t>(L.act_records));
        e.u8(0x48); e.u8(0x8B); e.u8(0x90);        /* mov rdx, [rax+rec_n] */
        e.u32(static_cast<uint32_t>(L.act_rec_n));
        e.u8(0x48); e.u8(0xFF); e.u8(0xCA);        /* dec rdx */
        e.u8(0x48); e.u8(0x69); e.u8(0xD2);        /* imul rdx, rdx, size */
        e.u32(static_cast<uint32_t>(L.rec_size));
        e.u8(0x48); e.u8(0x01); e.u8(0xD1);        /* add rcx, rdx */
        e.u8(0x80); e.u8(0xB9);                    /* cmp byte [rcx+pend], 0 */
        e.u32(static_cast<uint32_t>(L.rec_pend));
        e.u8(0);
        e.bail_unless(0x74, pc);      /* je (normal) else BAIL (reraise) */
        return true;
    }

    case OpCode::CatchTest:
    case OpCode::Reraise:
    case OpCode::Throw:
        /* cold catch-region / raise ops: an unconditional exit - the
         * interpreter re-runs the kept original (dispatch/raise machinery).
         * Reached natively only via a fall-through that in practice is the
         * handler-dispatch entry, already interpreted. */
        e.bump_op(in.op);
        e.exit_pc(pc);
        return true;

    case OpCode::DeclConstV:
        /* const decl bind via jit_decl_const(dst, is_global, src). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_decl_const) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::DefinedGlobalV:
        /* defined(g) via jit_defined_global(dst, gslot). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_defined_global) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::ThrowRuntimeV:
        /* An always-throwing construct whose exception mix includes
         * NON-Runtime ones (UndefinedVariableEx) that cannot ride
         * g_vm_jit_exc: exit unconditionally at the op - the interpreter
         * re-runs the side-effect-free throw op with exact semantics for
         * every kind. The point is run-shape: the ops before it fragment
         * together instead of splitting at the throw. */
        e.bump_op(OpCode::ThrowRuntimeV);
        e.exit_pc(pc);
        return true;

    case OpCode::LoadElemBool:
        /* INLINE (no helper call): the N4 flat-array navigation with a BYTE
         * element - slot -> shobj -> kind + data, unsigned bounds check, movzx
         * the byte, then the two-store with the t_bool singleton. A helper CALL
         * here cost more than the interpreter dispatch it replaced (measured:
         * +55% instructions on 56_sieve_bool, whose hot loop is exactly
         * `if (sieve[i])`), which is the whole reason this op is emitted inline.
         * Any failing precondition BAILS and the interpreter re-runs the op.
         * Falls back to the helper only when the dst is REF-LISTED (it may hold
         * a reference whose release needs C++) - rare for a bool loop var. */
        if (!std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                                static_cast<int32_t>(in.target))) {
            const JitLayout &L = jit_layout();
            const SlotAddr base = slot_addr(in.target2);
            const SlotAddr dst = slot_addr(in.target);
            e.bump_op(OpCode::LoadElemBool);         /* execution proof */
            e.load(RAX, base.type);                  /* base an array? */
            e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
            e.cmp_rax_r9();
            e.bail_unless(0x74, pc);
            e.cmp_byte_rdi(base.payload + L.slice_off, 0);   /* not a slice? */
            e.bail_unless(0x74, pc);
            e.load(RAX, base.payload);               /* rax = shobj */
            e.cmp_byte_rax(L.kind_off, L.kind_bools);/* flat bools? */
            e.bail_unless(0x74, pc);
            e.mov_rcx_rax(L.data_off);               /* rcx = _M_start */
            e.mov_rdx_rax(L.data_off + 8);           /* rdx = _M_finish */
            e.sub_rdx_rcx();                         /* rdx = count (1B elems,
                                                      * so NO sar - unlike the
                                                      * 8-byte int/float path) */
            load_index_r9(e, in);                    /* cache-aware index */
            e.cmp_r9_rdx();
            e.bail_unless(0x72, pc);                 /* jb: unsigned in-range */
            e.load_elem_byte();                      /* movzx eax,[rcx+r9] */
            e.movabs(RCX, reinterpret_cast<uint64_t>(L.t_bool));
            e.store_rcx_slot(dst.type);              /* a REAL bool, not 0/1 */
            e.store_rax_slot(dst.payload);
            return true;
        }
        goto foreach_load_helper;
    case OpCode::StrLen:
    case OpCode::LoadStrChar:
    case OpCode::LoadStructFieldInt:
    case OpCode::LoadStructFieldFloat:
    case OpCode::LoadStructElemV:
    case OpCode::LoadElemValue:
    foreach_load_helper: {
        /* The foreach element/field loads: rdi = dst, rsi = the base slot,
         * rdx = the INDEX VALUE (materialized cache-aware from the op's
         * slot-or-literal `a` operand BEFORE the prologue - rax survives the
         * pushes - so an N5-pinned loop counter is read from its REGISTER),
         * rcx/r8 = the field index + int/float selector for the struct-field
         * pair. StrLen has no index. Only LoadElemValue returns a status
         * (OOB re-raise / unproven-base bail). */
        const bool has_idx = in.op != OpCode::StrLen;
        const bool is_field = in.op == OpCode::LoadStructFieldInt
                           || in.op == OpCode::LoadStructFieldFloat;
        if (has_idx)
            load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        if (has_idx)
            e.mov_rr(RDX, RAX);               /* the index value */
        if (is_field) {
            e.movabs(RCX, static_cast<uint64_t>(in.b_lit()));
            e.movabs_r8(in.op == OpCode::LoadStructFieldFloat ? 1u : 0u);
        }
        const void *fn =
            in.op == OpCode::LoadElemBool
                ? reinterpret_cast<const void *>(jit_load_elem_bool)
          : in.op == OpCode::StrLen
                ? reinterpret_cast<const void *>(jit_str_len)
          : in.op == OpCode::LoadStrChar
                ? reinterpret_cast<const void *>(jit_load_str_char)
          : is_field
                ? reinterpret_cast<const void *>(jit_load_struct_field)
          : in.op == OpCode::LoadStructElemV
                ? reinterpret_cast<const void *>(jit_load_struct_elem)
                : reinterpret_cast<const void *>(jit_load_elem_value);
        e.call_relocs.push_back({ e.pos(), fn });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        if (in.op == OpCode::LoadElemValue) {
            e.u8(0x85); e.u8(0xC0);           /* test eax, eax */
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;
    }

    case OpCode::StructCtorBoxedV:
        /* jit_struct_ctor_boxed(dst, run base, &ck.boxed_ctors[target2]) - the
         * pool entry (def + the per-arg carets) is baked as a stable buffer
         * address. A field coerce throw carries the arg's POOLED caret, which
         * vm_raise leaves untouched (it stamps only an EMPTY loc). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX,
                 reinterpret_cast<uint64_t>(&ck.boxed_ctors[in.target2]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_struct_ctor_boxed) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::BinOpV:
    case OpCode::CmpV:
    case OpCode::CompoundV:
    case OpCode::UnaryV: {
        /* jit_boxed_* / jit_unary (bop) where bop = &ck.boxed_ops[in.target2]
         * (a stable pool-buffer address; build_boxed_ops stored the index in
         * target2). rdi = bop. Can throw -> test eax + exit_pc so EnterNative
         * re-raises with the op's caret from the loc side table. */
        const void *fn = in.op == OpCode::BinOpV
            ? reinterpret_cast<const void *>(jit_boxed_binop)
            : in.op == OpCode::CmpV
                ? reinterpret_cast<const void *>(jit_boxed_cmp)
            : in.op == OpCode::UnaryV
                ? reinterpret_cast<const void *>(jit_unary)
                : reinterpret_cast<const void *>(jit_boxed_compound);
        emit_call_prologue(e);
        e.movabs(RDI,
                 reinterpret_cast<uint64_t>(&ck.boxed_ops[in.target2]));
        e.call_relocs.push_back({ e.pos(), fn });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok = e.j8(0x74);       /* jz -> continue (0 = no raise) */
        e.exit_pc(pc);                        /* raised: EnterNative re-raises */
        e.patch8(j_ok, e.pos());
        return true;
    }

    case OpCode::LogV:
        /* jit_boxed_log(&ck.boxed_ops[target2]). is_true() CAN throw (its base
         * Type op, for an operand with no bool conversion) -> test eax +
         * exit_pc so EnterNative re-raises with the loc side table's caret.
         * This op was originally emitted with NO status check under a "never
         * throws" assumption, which made a throwing operand escape the noexcept
         * helper and std::terminate. */
        emit_call_prologue(e);
        e.movabs(RDI,
                 reinterpret_cast<uint64_t>(&ck.boxed_ops[in.target2]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_boxed_log) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::CoerceNumV: {
        /* jit_coerce_num(dst, src_slot, is_float, lep) - rdi=dst=target,
         * rsi=src=a_slot, rdx=is_float=(target2!=0), rcx=&locs[i]. Uses
         * g_current_ctx->frame, not the slots arg. CAN throw -> conveyed with
         * the baked caret; test eax + exit_pc. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.a_slot())));
        e.movabs(RDX, in.target2 != 0 ? 1u : 0u);
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_coerce_num) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok_cn = e.j8(0x74);
        emit_exc_stamp(e, ck, old_pc);        /* cold: the op's own caret */
        e.exit_pc(pc);
        e.patch8(j_ok_cn, e.pos());
        return true;
    }

    case OpCode::CallBuiltinV: {
        /* jit_call_builtin(dst, base, n, bc) - rdi=dst=target, rsi=base=a_lit,
         * rdx=n=b_lit, rcx=bc=&ck.builtin_calls[target2] (a stable pool-buffer
         * addr). Uses g_current_ctx->frame. The builtin CAN throw -> test eax +
         * exit_pc so EnterNative re-raises (the helper already stamped the loc
         * from the pool). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
        e.movabs(RCX,
                 reinterpret_cast<uint64_t>(&ck.builtin_calls[in.target2]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_call_builtin) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok_cb = e.j8(0x74);
        e.exit_pc(pc);
        e.patch8(j_ok_cb, e.pos());
        return true;
    }

    case OpCode::CheckFuncV: {
        /* jit_check_func(slot) - rdi = a_slot. A non-func conveys a loc-less
         * TypeErrorEx -> test eax + exit_pc (the re-raise stamps arg0's
         * caret from the loc table at this pc). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_check_func) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok_cf = e.j8(0x74);
        e.exit_pc(pc);
        e.patch8(j_ok_cf, e.pos());
        return true;
    }

    case OpCode::MapFilterV: {
        /* jit_map_filter(fn, cont, dst, is_map) - rdi=a_slot, rsi=b_slot,
         * rdx=target, rcx=target2. The callback re-enters vm_dispatch; a
         * throw conveys (exc/eptr) -> test eax + exit_pc. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.b_slot())));
        e.movabs(RDX, static_cast<uint64_t>(
                          static_cast<int_type>(in.target)));
        e.movabs(RCX, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_map_filter) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok_mf = e.j8(0x74);
        e.exit_pc(pc);
        e.patch8(j_ok_mf, e.pos());
        return true;
    }

    case OpCode::AppendV:
        /* jit_append(kind=a_dual_hi, arg0_slot=target2, val_slot=b_lit,
         * dst_slot=target, bc=&ck.builtin_calls[a_dual_lo]). r8 = the pool entry
         * (baked). The fallback CAN throw -> test eax + exit_pc (re-raise). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_dual_hi())));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs_r8(
            reinterpret_cast<uint64_t>(&ck.builtin_calls[in.a_dual_lo()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_append) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::CallBuiltinLV:
        /* jit_call_builtin_lv(kind=a_dual_hi, arg0_slot=target2, dst_slot=target,
         * rest_base=(b_is_lit ? b_lit : -1), bc=&ck.builtin_calls[a_dual_lo]).
         * rest_base -1 = a no-value-arg op. r8 = the pool entry. Throws ->
         * test eax + exit_pc (re-raise). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_dual_hi())));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(
                          in.b_is_lit() ? in.b_lit() : -1)));
        e.movabs_r8(
            reinterpret_cast<uint64_t>(&ck.builtin_calls[in.a_dual_lo()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_call_builtin_lv) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::CallBuiltinLVElem:
    case OpCode::CallBuiltinLVMember:
        /* jit_call_builtin_lv_{elem,member}(kind=a_dual_hi, base_slot=target2,
         * dst_slot=target, run_base=b_lit, bc=&ck.builtin_calls[a_dual_lo]).
         * `b` is always a lit here (the value-args run). Throws -> test eax +
         * exit_pc (re-raise). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_dual_hi())));
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.b_lit())));
        e.movabs_r8(
            reinterpret_cast<uint64_t>(&ck.builtin_calls[in.a_dual_lo()]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(
                           in.op == OpCode::CallBuiltinLVElem
                               ? jit_call_builtin_lv_elem
                               : jit_call_builtin_lv_member) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StoreGlobalV:
    case OpCode::StoreCaptureV: {
        const bool is_cap = in.op == OpCode::StoreCaptureV;
        if (in.aop != Op::invalid) {
            /* COMPOUND `g OP=`/`cap OP=` via jit_store_*_compound(&boxed_ops[
             * target2]) - the rhs operand + slot ride the pool (like CompoundV).
             * Non-0 -> exit_pc (an undefined-global bail / a num_bin_op
             * re-raise). rdi = the pool entry. */
            emit_call_prologue(e);
            e.movabs(RDI,
                     reinterpret_cast<uint64_t>(&ck.boxed_ops[in.target2]));
            e.call_relocs.push_back(
                { e.pos(), reinterpret_cast<const void *>(
                    is_cap ? jit_store_capture_compound
                           : jit_store_global_compound) });
            e.u8(0xE8); e.u32(0);
            emit_call_epilogue(e);
            e.u8(0x85); e.u8(0xC0);             /* test eax, eax */
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
            return true;
        }
        /* De-helperize 6b: the PLAIN `g = <expr>`/`cap = <expr>` INLINE for
         * the trivial-x-trivial case. The helper runs RValue(*src), so the
         * SRC gate excludes the pseudo types AND t_none besides references
         * (emit_store_src_gate, the 3..t_str-1 range); the dst slot lives
         * behind the ctx chain (r9 = the slots data; the GLOBAL chain keeps
         * the table in RDX for the `defined` byte) and its CURRENT value is
         * always ref-checked (globals/captures can hold anything). Any gate
         * failing falls to the original helper. */
        const SlotAddr src = slot_addr(in.a_slot());
        const int32_t soff = static_cast<int32_t>(
            in.target * static_cast<int_type>(sizeof(LValue)));
        const int32_t pv0 = slot_addr(0).payload, ty0 = slot_addr(0).type;
        e.bump_op(in.op);
        const auto g = emit_store_src_gate(e, src.type);
        emit_ctx_chain_r9(e, is_cap);
        const size_t j_dref = emit_ref_check_jae_r9(e, soff + ty0);
        e.load(RAX, src.type);
        e.load(RCX, src.payload);      e.store_r9b(RCX, soff + pv0);
        e.load(RCX, src.payload + 8);  e.store_r9b(RCX, soff + pv0 + 8);
        e.load(RCX, src.payload + 16); e.store_r9b(RCX, soff + pv0 + 16);
        e.store_r9b(RAX, soff + ty0);
        if (!is_cap) {
            /* defined[gslot] = 1: rcx = defined.data() (the table is still
             * in rdx), then the byte store. */
            e.u8(0x48); e.u8(0x8B); e.u8(0x8A);    /* mov rcx,[rdx+defined] */
            e.u32(static_cast<uint32_t>(jit_layout().gft_defined));
            e.u8(0xC6); e.u8(0x81);                /* mov byte [rcx+d], 1 */
            e.u32(static_cast<uint32_t>(in.target));
            e.u8(0x01);
        }
        const size_t j_done = e.j32(0xEB);
        e.patch32_here(g.first);
        e.patch32_here(g.second);
        e.patch32_here(j_dref);
        const auto off = [](int slot) {
            return static_cast<int32_t>(static_cast<long>(slot)
                                        * static_cast<long>(sizeof(LValue)));
        };
        emit_call_prologue(e);
        e.lea(RSI, off(in.a_slot()));       /* rsi = &slot[src] (uses rdi) */
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(
                is_cap ? jit_store_capture : jit_store_global) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.patch32_here(j_done);
        return true;
    }

    case OpCode::SubscriptV: {
        /* dst = base[idx] via jit_subscript(base_lv, idx*, dst*, lep) - SysV
         * rdi=base_lv, rsi=idx, rdx=dst, rcx=&locs[i] (the op's own caret,
         * conveyed on throw -> deletable). leas from rdi (slots base); rdi is
         * set LAST (it overwrites the base ptr). A non-0 return = threw ->
         * exit to pc so EnterNative re-raises g_vm_jit_exc. */
        const auto off = [](int slot) {
            return static_cast<int32_t>(static_cast<long>(slot)
                                        * static_cast<long>(sizeof(LValue)));
        };
        emit_call_prologue(e);
        e.lea(RSI, off(in.a_slot()));       /* rsi = &slot[idx] */
        e.lea(RDX, off(in.target));         /* rdx = &slot[dst] */
        e.lea_rdi(off(in.target2));         /* rdi = &slot[base] (LAST) */
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_subscript) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);             /* test eax, eax */
        const size_t j_ok = e.j8(0x74);     /* jz ok (0 = no throw) */
        emit_exc_stamp(e, ck, old_pc);      /* cold: the op's own caret */
        e.exit_pc(pc);                      /* threw -> EnterNative re-raises */
        e.patch8(j_ok, e.pos());
        return true;
    }

    case OpCode::LoadMemberInt:
    case OpCode::LoadMemberFloat:
        /* the H1 typed member read `p.x` via jit_load_member(dst, base_slot,
         * mk, is_int) - the POD byte fast path + the member_read_core
         * fallback (which can throw -> test eax + exit_pc re-raise; the
         * throw carries the member caret from the pool). */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RDX, reinterpret_cast<uint64_t>(&ck.member_keys[in.a_lit()]));
        e.movabs(RCX, static_cast<uint64_t>(
                          in.op == OpCode::LoadMemberInt ? 1 : 0));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_load_member) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::MemberV: {
        /* dst = base.member via jit_member(base*, dst*, mk) - SysV rdi=base,
         * rsi=dst, rdx=mk. mk = &ck.member_keys[a_lit] (the pool BUFFER addr,
         * stable across the chunk move). rdi (base) set LAST. */
        const auto off = [](int slot) {
            return static_cast<int32_t>(static_cast<long>(slot)
                                        * static_cast<long>(sizeof(LValue)));
        };
        emit_call_prologue(e);
        e.lea(RSI, off(in.target));         /* rsi = &slot[dst] */
        e.movabs(RDX, reinterpret_cast<uint64_t>(&ck.member_keys[in.a_lit()]));
        e.lea_rdi(off(in.target2));         /* rdi = &slot[base] (LAST) */
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_member) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);             /* test eax, eax */
        const size_t j_ok = e.j8(0x74);
        e.exit_pc(pc);                      /* threw -> EnterNative re-raises */
        e.patch8(j_ok, e.pos());
        return true;
    }

    case OpCode::ReturnV:
        /* #55: flush the cache (jit_ret reads the result slot from MEMORY),
         * then call jit_ret(res_slot) and RET its resume sentinel. rdi (the
         * slots base) is dead after - we ret - so it carries the arg; jit_ret
         * uses g_current_ctx->frame, not rdi. rsp must be 16-aligned at the
         * call (entry rsp%16==8, no pushes yet -> sub 8; restore before ret so
         * ret pops the real return address). No prologue/epilogue: we ret, so
         * nothing needs preserving. */
        e.flush_cache();
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.u8(0x48); e.u8(0x83); e.u8(0xEC); e.u8(0x08);   /* sub rsp, 8 */
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_ret) });
        e.u8(0xE8); e.u32(0);                             /* call jit_ret */
        e.u8(0x48); e.u8(0x83); e.u8(0xC4); e.u8(0x08);   /* add rsp, 8 */
        e.u8(0xC3);                                        /* ret (rax=sentinel)*/
        return true;

    case OpCode::Halt:
        /* model-flip (nativize-ops): the native `return none`. flush_cache so a
         * later-bail path (there is none past a terminator, but stay uniform
         * with ReturnV) has memory consistent, then call jit_halt() (no arg -
         * the result is none) and RET its resume sentinel. Same stack discipline
         * as ReturnV: sub 8 to 16-align, restore, ret. */
        e.flush_cache();
        e.u8(0x48); e.u8(0x83); e.u8(0xEC); e.u8(0x08);   /* sub rsp, 8 */
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_halt) });
        e.u8(0xE8); e.u32(0);                             /* call jit_halt */
        e.u8(0x48); e.u8(0x83); e.u8(0xC4); e.u8(0x08);   /* add rsp, 8 */
        e.u8(0xC3);                                        /* ret (rax=sentinel)*/
        return true;

    case OpCode::CachedCallV:
        /* M5 inc 3 (LEAN SYNC ENTER): the cached recursive call - the helper
         * probes the caller's per-frame pure cache, else pushes the callee
         * frame (the miss key riding its record) and runs it to completion;
         * the caller continues natively either way. */
        emit_sync_call(e, ck, in, pc, old_pc,
                       reinterpret_cast<const void *>(jit_call_sync_cached),
                       OpCode::CachedCallV);
        return true;

    case OpCode::CallValueV:
        /* M5 inc 3: the indirect func-VALUE call (closure/lambda/func var -
         * the callee was evaluated into the target2 temp). A non-func value
         * bails to the interpreted op's NotCallableEx. */
        emit_sync_call(e, ck, in, pc, old_pc,
                       reinterpret_cast<const void *>(jit_call_sync_value),
                       OpCode::CallValueV);
        return true;

    case OpCode::CallV: {
        if (!callv_native_ok(in, jc)) {
            /* M5: the SYNC call - jit_call_sync(callee_slot, argbase, nargs,
             * dst, site) runs the callee to completion (its own fragments
             * active inside); this fragment continues natively on 0. */
            emit_sync_call(e, ck, in, pc, old_pc,
                           reinterpret_cast<const void *>(jit_call_sync),
                           OpCode::CallV);
            return true;
        }
        /* #55 STEP 2.1: a NATIVE direct call (the run builder included it only
         * when callv_native_ok). Push the callee frame via jit_call_setup, then
         * `call` the callee's fragment directly; the callee's native ReturnV
         * (jit_ret) pops its window + writes OUR dst slot + rets a sentinel we
         * IGNORE (a native_leaf never bails post-setup). A call run is NOT
         * N5-cached (pick_cached_slots -> {}), so the args already sit in memory
         * and no cache flush/reload is needed. This fragment (holding a CallV)
         * is non-leaf, so it is only ever entered via jit_enter/EnterNative -
         * a StackOverflow exit therefore returns to EnterNative, which raises
         * g_vm_jit_exc. */
        const JitLayout &L = jit_layout();
        const FuncDescriptor *callee = (*jc->slot_desc)[in.target2];

        emit_call_prologue(e);              /* push rdi (empty cache: aligned) */
        /* jit_call_setup(callee_slot, argbase, nargs, dst, caller_desc, pc): */
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
        e.movabs(RCX, static_cast<uint64_t>(in.target));
        e.movabs_r8(reinterpret_cast<uint64_t>(jc->caller_desc));
        e.movabs_r9(static_cast<uint64_t>(pc));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_call_setup) });
        e.u8(0xE8); e.u32(0);               /* call jit_call_setup -> rax */
        e.u8(0x48); e.u8(0x85); e.u8(0xC0); /* test rax, rax */
        const size_t j_ok = e.j8(0x75);     /* jnz over_SO (rax != null) */
        emit_call_epilogue(e);              /* SO: restore rdi, re-mat rsi/r8 */
        e.exit_pc(pc);                      /* -> EnterNative raises g_vm_jit_exc*/
        e.patch8(j_ok, e.pos());            /* over_SO: */
        e.mov_rr(RDI, RAX);                 /* rdi = callee window slots */
        /* fragment entry = callee->vm_chunk->native.base + native_entry_off: */
        e.movabs(RAX, reinterpret_cast<uint64_t>(callee));
        e.u8(0x48); e.u8(0x8B); e.u8(0x80); /* mov rax, [rax + desc.vm_chunk] */
        e.u32(static_cast<uint32_t>(L.desc_vm_chunk));
        e.u8(0x48); e.u8(0x8B); e.u8(0x88); /* mov rcx, [rax + chunk.native.base]*/
        e.u32(static_cast<uint32_t>(L.chunk_native_base));
        e.u8(0x48); e.u8(0x8B); e.u8(0x90); /* mov rdx, [rax + native_entry_off]*/
        e.u32(static_cast<uint32_t>(L.chunk_native_entry));
        e.u8(0x48); e.u8(0x01); e.u8(0xD1); /* add rcx, rdx */
        e.u8(0xFF); e.u8(0xD1);             /* call rcx (the callee fragment) */
        emit_call_epilogue(e);              /* restore rdi, re-mat rsi/r8 */
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

    case OpCode::JumpUnlessTrueV: {
        /* The BOXED condition. INLINE FAST PATH: for an int or bool value
         * `is_true` is exactly `payload != 0` (TypeInt::is_true is `v != 0`, and
         * a bool payload is fully zeroed except the low byte), so the common
         * loop/if condition is a type-tag compare + a test - NO call. A helper
         * CALL on this op cost more than the interpreter dispatch it replaced
         * (measured: +55% instructions on 56_sieve_bool), which is why it is
         * inlined. Any other type (string/array/dict/none/...) takes the
         * jit_is_true call, whose tri-state also carries the THROW case
         * (is_true's base Type op throws for a value with no bool conversion).
         * Both paths leave rax = the truth value, then one shared test+jcc. */
        const JitLayout &L = jit_layout();
        const SlotAddr cond = slot_addr(in.target2);
        e.bump_op(OpCode::JumpUnlessTrueV);      /* execution proof (the inline
                                                  * fast path calls no helper) */
        e.load(RAX, cond.type);                  /* rax = the value's Type* */
        e.u8(0x48); e.u8(0x39); e.u8(0xF0);      /* cmp rax, rsi (t_int) */
        const size_t j_fast_int = e.j32(0x74);   /* je -> fast */
        e.movabs(RCX, reinterpret_cast<uint64_t>(L.t_bool));
        e.cmp_rax_rcx();
        const size_t j_fast_bool = e.j32(0x74);  /* je -> fast */
        /* --- slow path: any other type (may throw) --- */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_is_true) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);                  /* test eax, eax (32-bit: -1
                                                  * is negative here, while the
                                                  * 64-bit rax is zero-extended
                                                  * 0/1 on the success paths) */
        {
            const size_t j_ok = e.j8(0x79);      /* jns -> did not throw */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        const size_t j_join = e.j32(0xEB);       /* jmp -> join (rax = 0/1) */
        /* --- fast path: rax = the raw payload --- */
        e.patch32_here(j_fast_int);
        e.patch32_here(j_fast_bool);
        e.load(RAX, cond.payload);
        /* --- join: jump to target when the value is FALSE --- */
        e.patch32_here(j_join);
        e.test_rax_rax();                        /* 64-bit: a big int whose low
                                                  * 32 bits are 0 is still true */
        emit_cond_jump_raw(e, 0x84 /* jz near */, 0x75 /* jnz short */,
                           static_cast<size_t>(in.target), begin, end,
                           remap, fixups);
        return;
    }

    case OpCode::PushHandler: {
        /* INLINE try-handler push (step 7a): load the handlers vector's
         * finish; at capacity -> the cold jit_push_handler_grow; else store
         * the REMAPPED catch-dispatch pc (a 4-byte VmHandler) and bump
         * finish. Never throws. */
        const JitLayout &L = jit_layout();
        const uint32_t tgt = static_cast<uint32_t>(remap[in.target]);
        e.bump_op(OpCode::PushHandler);
        e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_act));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] (act) */
        e.u8(0x48); e.u8(0x8B); e.u8(0x88);        /* mov rcx, [rax+h+8] */
        e.u32(static_cast<uint32_t>(L.act_handlers + 8));    /* finish */
        e.u8(0x48); e.u8(0x3B); e.u8(0x88);        /* cmp rcx, [rax+h+16] */
        e.u32(static_cast<uint32_t>(L.act_handlers + 16));   /* end cap */
        const size_t j_grow = e.j32(0x74);         /* je -> the cold grow */
        e.u8(0xC7); e.u8(0x01);                    /* mov dword [rcx], tgt */
        e.u32(tgt);
        e.u8(0x48); e.u8(0x83); e.u8(0xC1); e.u8(4);   /* add rcx, 4 */
        e.u8(0x48); e.u8(0x89); e.u8(0x88);        /* mov [rax+h+8], rcx */
        e.u32(static_cast<uint32_t>(L.act_handlers + 8));
        const size_t j_done = e.j32(0xEB);
        e.patch32_here(j_grow);
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(static_cast<int_type>(tgt)));
        e.call_relocs.push_back(
            { e.pos(),
              reinterpret_cast<const void *>(jit_push_handler_grow) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.patch32_here(j_done);
        return;
    }

    case OpCode::JumpIfNotNoneV: {
        /* `a ?? b`: jump to target when the lhs slot is NOT none - one
         * type-tag compare against the none singleton. Never throws. */
        const SlotAddr a = slot_addr(in.a_slot());
        e.bump_op(OpCode::JumpIfNotNoneV);
        e.load(RAX, a.type);
        e.movabs(RCX, reinterpret_cast<uint64_t>(jit_layout().t_none));
        e.cmp_rax_rcx();
        emit_cond_jump_raw(e, 0x85 /* jne near */, 0x74 /* je short */,
                           static_cast<size_t>(in.target), begin, end,
                           remap, fixups);
        return;
    }

    case OpCode::ForStepElemInt: {
        /* #9 back-edge fusion: step + test + the a[i] element load in one op.
         * ORDER MATTERS: the base GATE (array / non-slice / ints-or-bools,
         * every check that can BAIL) runs FIRST - a bail re-runs the WHOLE
         * op, so a post-step bail would DOUBLE-STEP the counter. Then step
         * (cache-aware), test vs the bound; NOT-go falls through with no
         * load (the counter is out of range there, matching the
         * interpreter); GO reads a[counter] via the shared flat tails
         * (negative wraps, OOB raises with this pc's caret) into the elem
         * slot and jumps to target. */
        const JitLayout &L = jit_layout();
        const bool up = in.aop == Op::lt || in.aop == Op::le;
        const SlotAddr base = slot_addr(in.b_dual_lo());
        e.bump_op(OpCode::ForStepElemInt);   /* before any loads (rax!) */
        emit_elem_base_gate(e, in.b_dual_lo(), pc);
        read_slot(e, RAX, in.target2);
        e.u8(0x48); e.u8(0xFF); e.u8(up ? 0xC0 : 0xC8);   /* inc/dec rax */
        write_slot(e, ck, RAX, in.target2, pc);
        load_operand(e, RCX, in.a_is_lit(), in.a_lit(), in.a_slot());
        e.u8(0x48); e.u8(0x39); e.u8(0xC8);               /* cmp rax, rcx */
        const size_t j_fall = e.j32(cc_for(cc_negate(in.aop)).short_op);
        /* the trusted read: the gate proved kind is ints or bools */
        e.load(RAX, base.payload);            /* rax = shobj */
        e.cmp_byte_rax(L.kind_off, L.kind_bools);
        const size_t j_bools = e.j32(0x74);
        emit_flat_int_tail(e, pc, /*bools=*/false, nullptr, in.target2);
        const size_t j_done = e.j32(0xEB);
        e.patch32_here(j_bools);
        emit_flat_int_tail(e, pc, /*bools=*/true, nullptr, in.target2);
        e.patch32_here(j_done);
        write_slot(e, ck, RAX, in.b_dual_hi(), pc);
        {
            const size_t tgt = static_cast<size_t>(in.target);
            if (tgt >= begin && tgt < end) {
                e.u8(0xE9);
                fixups.push_back({ e.pos(), tgt });
                e.u32(0);
            } else {
                e.exit_pc(static_cast<uint32_t>(remap[in.target]));
            }
        }
        e.patch32_here(j_fall);
        return;
    }

    case OpCode::DictIterNext: {
        /* dict foreach advance: jit_dict_iter_next (rdi=iter_id, rsi=k slot,
         * rdx=v slot; -1 == unbound) binds + advances and returns 1, or 0 on
         * exhaustion - jump to end_pc (target) when ZERO. Never throws. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.movabs(RSI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.movabs(RDX, static_cast<uint64_t>(
                          static_cast<int_type>(in.b_slot())));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_dict_iter_next) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);                  /* test eax, eax */
        emit_cond_jump_raw(e, 0x84 /* jz near */, 0x75 /* jnz short */,
                           static_cast<size_t>(in.target), begin, end,
                           remap, fixups);
        return;
    }

    case OpCode::ForeachDynNext: {
        /* dyn foreach advance: jit_foreach_dyn_next (rdi=iter_id) binds from
         * the recorded state and returns 1, 0 on exhaustion (jump to end_pc),
         * or -1 = THREW (the strict N-var unpack) - exit so EnterNative
         * re-raises with the container's caret from the loc side table. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.target2)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_foreach_dyn_next) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);                  /* test eax, eax (32-bit:
                                                  * -1 is negative here) */
        {
            const size_t j_ok = e.j8(0x79);      /* jns -> did not throw */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        emit_cond_jump_raw(e, 0x84 /* jz near */, 0x75 /* jnz short */,
                           static_cast<size_t>(in.target), begin, end,
                           remap, fixups);
        return;
    }

    case OpCode::JumpUnlessElemInt: {
        /* E4 fusion `if (arr[i])`: read the element with the shared
         * int-semantics path (flat ints or bools; a bail re-runs the op, an
         * OOB raises with its caret) and jump to target when it is FALSE.
         * Nothing is written - the fused temp was proven dead on both paths. */
        emit_elem_int_read(e, in, pc);
        e.test_rax_rax();
        emit_cond_jump_raw(e, 0x84 /* jz near */, 0x75 /* jnz short */,
                           static_cast<size_t>(in.target), begin, end,
                           remap, fixups);
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
        || op == OpCode::JumpUnlessFloatCmp
        /* the BOXED condition branch: the helper evaluates is_true, the
         * fragment does the jump - so a loop body holding one no longer
         * splits the run and leaves the back edge interpreted. */
        || op == OpCode::JumpUnlessTrueV
        /* the E4 `if (arr[i])` fusion: the element read is emitted inline and
         * the fragment jumps when it is FALSE. */
        || op == OpCode::JumpUnlessElemInt
        /* the iterator advance ops: the helper binds + advances and returns
         * the verdict; the fragment jumps to end_pc on exhaustion. */
        || op == OpCode::DictIterNext
        || op == OpCode::ForeachDynNext
        /* the #9 back-edge fusion: step + test + the element load, jumping
         * to the loop target when the loop continues. */
        || op == OpCode::ForStepElemInt
        /* the `??` short-circuit: jump when the lhs is NOT none. */
        || op == OpCode::JumpIfNotNoneV
        /* NOT a real branch - routed here because its pushed catch_pc is a
         * PC needing remap[] (emit_op has no remap). */
        || op == OpCode::PushHandler;
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
/*
 * The op NEVER EXITS its fragment mid-run - no throw, no raise, no bail;
 * at most a terminal resume SENTINEL (ReturnV/Halt). This is the
 * NATIVE_LEAF bar: the #55 direct call IGNORES the callee fragment's
 * return value, so a leaf body must be provably exit-free - a
 * conveying-throw op inside a leaf DROPPED its exception with the callee
 * frame still pushed (a `-ni` non-inlined `len(dyn)` leaf printed a stale
 * dst and HUNG - found while widening op_fully_native for the re-raise
 * deletability pass, 2026-07-25; CallBuiltinV had been on the old combined
 * list, so the hole predates the pass). Deletability (op_fully_native
 * below) is the WEAKER bar: never-exits OR conveys-with-own-loc.
 */
static bool op_never_exits(const Instr &in)
{
    switch (in.op) {
    /* generic IntBin: the NON-THROWING arms never return an interior pc ->
     * deletable. The div/mod/shift arms RAISE (JR_DIV0/JR_NEG_SHIFT) with a
     * caret from the pc-keyed loc side table, so their originals must stay
     * (a deleted run collapses every pc onto the EnterNative - the caret
     * would be wrong), exactly like the reg-shift RR forms. (Own case, NOT
     * part of the fall-through chain below - the nested switch would
     * swallow the chain's earlier labels.) */
    case OpCode::IntBin:
        switch (in.aop) {
        case Op::plus: case Op::minus: case Op::times:
        case Op::band: case Op::bor:  case Op::bxor:
            return true;
        default:                       /* div/mod/shl/shr/ushr raise */
            return false;
        }
    case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
    case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
    case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
    case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI:
    case OpCode::IntShlRI: case OpCode::IntShrRI:
    case OpCode::IntModRI: case OpCode::IntAddModRI:
    case OpCode::LoadImmInt: case OpCode::Jump:
    case OpCode::JumpUnlessIntCmp: case OpCode::ForLoopStep:
    case OpCode::IntAddStep:
    /* model-flip (nativize-ops): a native Halt rets a resume SENTINEL (never an
     * interior pc), like ReturnV - its interpreted original is deletable. */
    case OpCode::Halt:
    /* #55: a native ReturnV never returns an interior pc (it rets a resume
     * SENTINEL, not a re-interpret pc), so the interpreted original can be
     * dropped from a deletable run - exactly the fully-native contract. */
    case OpCode::ReturnV:
    /* model-flip (nativize-ops): MoveV runs entirely in the fragment (the
     * jit_move helper never throws / never bails), so it never returns an
     * interior pc - deletable. Without this a MoveV adjacent to a fully-native
     * loop (e.g. the print-arg move after the loop) would extend the run and
     * make the LOOP non-deletable (its int originals kept). LoadBuiltinV /
     * LoadConstV are the same (a trivial / value-copy non-throwing load); a
     * PLAIN StoreGlobalV (the only eligible form) never throws either. */
    case OpCode::MoveV:
    case OpCode::LoadBuiltinV:
    case OpCode::LoadConstV:
    case OpCode::LoadCaptureV:      /* capture read (always defined), never throws */
    case OpCode::LoadLiteralObjV:   /* a clone, never throws */
    case OpCode::ArrLen:            /* size() of a proven array, never throws */
    case OpCode::MakeClosureV:      /* resolved-closure create, never throws */
    case OpCode::MakeArrayV:        /* array literal build, no error path */
    case OpCode::CmpIntV:           /* int compare -> bool; cannot fault */
    /* the foreach loads: the index is loop-bounded by the ArrLen/StrLen that
     * produced it and the base kind is proven, so none of these can throw or
     * bail (LoadElemValue, which bounds-checks, is deliberately NOT here). */
    case OpCode::LoadElemBool:
    case OpCode::StrLen:
    case OpCode::LoadStrChar:
    case OpCode::LoadStructFieldInt:
    case OpCode::LoadStructFieldFloat:
    case OpCode::LoadStructElemV:
    /* the dict foreach iterator pair: the dict is PROVEN, the frame-slot binds
     * have no COW path - neither op can throw or bail, so both are deletable
     * (the Next's end_pc branch exits via the remapped exit_pc like any
     * fully-native branch). The ForeachDyn pair are NOT here (they throw with
     * side-table carets). */
    case OpCode::DictIterInit:
    case OpCode::DictIterNext:
    /* the `??` branch (an inline none-tag compare), the const-decl bind and
     * defined(g) (trivial helpers) - none can throw or bail. */
    case OpCode::JumpIfNotNoneV:
    case OpCode::DeclConstV:
    case OpCode::DefinedGlobalV:
    /* the inline exception ops: pure activation state, never throw (the
     * PushHandler's pushed catch_pc is remapped at emit). */
    case OpCode::PushHandler:
    case OpCode::PopHandler:
    case OpCode::SetPend:
        return true;
    /* A PLAIN global/capture store (aop invalid) never throws -> deletable; a
     * COMPOUND one (`g OP=`/`cap OP=`) throws (num_bin_op) OR bails (undefined
     * global), so its interpreted original must be KEPT (the bail/re-raise). */
    case OpCode::StoreGlobalV:
    case OpCode::StoreCaptureV:
        return in.aop == Op::invalid;
    default:
        return false;
    }
}

/* DELETABLE (the interpreted original can be dropped from the rebuilt
 * bytecode): never-exits, OR a CONVEY-WITH-OWN-LOC throwing op. The latter's
 * contract (plans/model-flip.md, the re-raise deletability pass): the helper
 * (1) stamps its throw from a BAKED loc - the BoxedOp pool's start/end, a
 * baked &locs[i] LocEntry, the builtin_calls / member_keys pool carets - so
 * the caret is pc-INDEPENDENT (a deleted run collapses every pc onto its
 * EnterNative, where a table lookup would be wrong); (2) never re-executes
 * (a re-raise, not a re-interpret); and (3) never BAILS - a bail resumes AT
 * the exit pc, which in a deleted run is the EnterNative itself, so the
 * fragment would RE-RUN from the head (double execution). A conveyed exit
 * is SAFE despite the collapsed pc because EnterNative handles the raise
 * flags right after jit_enter returns - control never re-dispatches to the
 * exit pc. LoadGlobalV's old undefined-global bail became an eptr
 * conveyance of the exact interpreted UndefinedVariableEx (name + baked
 * caret) to qualify. NOTE these ops must NEVER enter a native_leaf
 * (op_never_exits above): the #55 direct caller ignores the fragment's
 * return, so a mid-body conveyed exit would be dropped. */
static bool op_fully_native(const Instr &in)
{
    if (op_never_exits(in))
        return true;
    switch (in.op) {
    /* CallBuiltinV conveys its OWN loc from the builtin_calls pool (stamped
     * in jit_call_builtin before stashing) - the family's model; see
     * plans/callbuiltinv-nativization.md #2. */
    case OpCode::CallBuiltinV:
    case OpCode::BinOpV:
    case OpCode::CmpV:
    case OpCode::LogV:
    case OpCode::UnaryV:
    case OpCode::CompoundV:
    case OpCode::SubscriptV:
    case OpCode::SliceV:
    case OpCode::DictLoadInt:
    case OpCode::DictLoadFloat:
    case OpCode::CoerceNumV:
    case OpCode::MemberV:
    case OpCode::LoadGlobalV:
        return true;
    /* The STORE family (increment 2). StoreElemInt (local-only eligible) /
     * DictStore / StoreElem2V: convey-only helpers, cold-side lep caret.
     * StoreElemFloat is EXCLUDED: its emitted VALUE load (emit_float_load)
     * can structurally BAIL on a non-numeric tag - unreachable when
     * inference is right, but a bail in a deleted run re-runs the fragment
     * head (the double-execution hazard), so the original stays. */
    case OpCode::StoreElemInt:
    case OpCode::DictStore:
    case OpCode::StoreElem2V:
        return true;
    /* A GLOBAL base (kind 1) BAILS on an undefined slot -> non-deletable
     * (and its emit skips the lep store for the same reason: the shared
     * failure branch would leave a STALE lep on the bail path). A local /
     * capture base is convey-only. StoreMemberV needs no lep at all (its
     * helper stamps every loc-less throw with the member_keys pool caret,
     * the interpreted catch's exact rule). */
    case OpCode::StoreElemValue:
    case OpCode::StoreMemberV:
        return in.target != 1;
    case OpCode::StoreElemChainV:
    case OpCode::StoreLValueChainV:
        return in.a_dual_hi() != 1;
    /* The COMPOUND global/capture stores (the PLAIN forms are never-exit,
     * gated in op_never_exits): both stamp their num_bin_op throw from the
     * BoxedOp pool's start/end, and the global's old undefined-slot bail
     * became an eptr conveyance of the exact UndefinedVariableEx (name +
     * pool caret) - convey-only now, so deletable. */
    case OpCode::StoreGlobalV:
    case OpCode::StoreCaptureV:
        return true;
    default:
        return false;
    }
}

/* #55 STEP 2.1: is THIS CallV a native-callable direct call? A COMPILE-TIME
 * gate (never a runtime bail): a plain CallV (not CachedCallV) to a WRITE-ONCE
 * global-slot function whose body is a `native_leaf`, from a FUNCTION caller
 * (caller_desc != null - main has no stable descriptor, so no native call from
 * main in v1). Needs the program context `jc`; null jc (disasm / -rt) -> no.
 * NOT op_fully_native (StackOverflow exit), so a run holding it is
 * non-deletable - the interpreted CallV survives, dead. */
static bool callv_native_ok(const Instr &in, const JitCtx *jc)
{
    if (!jc || !jc->caller_desc || in.op != OpCode::CallV)
        return false;
    if (!jc->slot_desc || !jc->slot_reassigned)
        return false;
    const int slot = in.target2;
    if (slot < 0 || static_cast<size_t>(slot) >= jc->slot_desc->size()
            || static_cast<size_t>(slot) >= jc->slot_reassigned->size())
        return false;
    if ((*jc->slot_reassigned)[slot])          /* not write-once */
        return false;
    const FuncDescriptor *callee = (*jc->slot_desc)[slot];
    if (!callee || !callee->vm_chunk)
        return false;
    return static_cast<const Chunk *>(callee->vm_chunk)->native_leaf;
}

/* An op that can be part of a native RUN: a plain eligible op, OR a CallV the
 * STEP-2.1 gate accepts (native call). */
static bool op_run_eligible(const Instr &in, const JitCtx *jc)
{
    /* M5: a plain CallV is eligible - the emit picks the #55 direct
     * fragment call for a native_leaf callee (callv_native_ok), else the
     * SYNCHRONOUS jit_call_sync (any callee with a chunk, main callers
     * included): the callee runs to completion via the vm_try_invoke
     * boundary machinery and the CALLER stays native across the call.
     * Undefined/non-callable/chunk-less/depth-capped shapes BAIL
     * pre-side-effect. EXCEPT direct SELF-recursion: past the depth cap
     * every level would pay a fragment-enter -> sync-bail -> exit ->
     * re-dispatch round-trip (a measured +14% on 10_recursion_deep), and
     * a self-recursive caller gains nothing from staying native across
     * the call - ineligible keeps the pre-M5 shape (the op splits the
     * run; the lean interpreted vm_enter_call runs it). NOT
     * op_fully_native.
     *
     * Inc 3 (lean sync enter): CachedCallV and CallValueV are eligible
     * too. CachedCallV is by definition SELF-recursive (a cacheable
     * recursive func), but its recursion is TREE-shaped and cache-bounded
     * (the per-frame PureCache dedups the frontier), so the effective
     * depth is small (fib-class) and the depth cap only nets pathology;
     * gating it like CallV's self-rule would exclude the flagship fib
     * shape entirely. CallValueV's callee is a runtime VALUE (unknown at
     * compile time) - a self-call through a value is rare and the depth
     * cap bails it. */
    if (in.op == OpCode::CallV) {
        if (callv_native_ok(in, jc))
            return true;
        if (jc && jc->caller_desc && jc->slot_desc
                && in.target2 >= 0
                && static_cast<size_t>(in.target2) < jc->slot_desc->size()
                && (*jc->slot_desc)[in.target2] == jc->caller_desc)
            return false;                  /* direct self-recursion */
        return true;
    }
    if (in.op == OpCode::CachedCallV || in.op == OpCode::CallValueV)
        return true;
    return jit_op_eligible(in) || callv_native_ok(in, jc);
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

/* #55 STEP 2: the native_leaf predicate (from ops; see jit.h). Matches
 * jit_compile_chunk's native_leaf condition exactly - the WHOLE body is a
 * single maximal run (every op jit_op_eligible, so nothing splits it) that is
 * deletable (every op op_fully_native; single-entry is trivial for [0,n)) and
 * ends in ReturnV. */
bool jit_chunk_is_native_leaf(const Chunk &chunk)
{
    if (!g_jit_enabled)
        return false;
    const size_t n = chunk.code.size();
    if (n < 1 || chunk.code[n - 1].op != OpCode::ReturnV)
        return false;
    for (size_t pc = 0; pc < n; pc++)
        if (!jit_op_eligible(chunk.code[pc])
                || !op_never_exits(chunk.code[pc]))
            return false;                     /* op_never_exits, NOT
                                               * op_fully_native: the #55
                                               * direct caller ignores the
                                               * fragment's return, so a leaf
                                               * must be exit-FREE (a
                                               * conveying throw would be
                                               * dropped - the -ni len(dyn)
                                               * hang) */
    return true;
}

/* plans/model-flip.md M1: partition the chunk into maximal NATIVE / ISLAND
 * segments. An op is NATIVE iff it is an inserted EnterNative (a compiled run)
 * or it is op_run_eligible (an op the container WOULD nativize - note this is
 * a looser bar than "has a fragment today": an op can be run-eligible yet sit
 * in a run whose compilation declined, while a whole-function container pays
 * no per-run EnterNative overhead). container_ready == every op native, i.e.
 * the whole body could be a single native container. DUMP-ONLY today. */
ContainerPlan jit_container_plan(const Chunk &chunk, const JitCtx *jc)
{
    ContainerPlan plan;
    const size_t n = chunk.code.size();
    for (size_t pc = 0; pc < n; pc++) {
        const Instr &in = chunk.code[pc];
        const bool native =
            in.op == OpCode::EnterNative || op_run_eligible(in, jc);
        if (native) plan.native_op_count++; else plan.island_op_count++;
        if (!plan.segs.empty() && plan.segs.back().native == native)
            plan.segs.back().end = pc + 1;
        else
            plan.segs.push_back({pc, pc + 1, native});
    }
    for (const ContainerSeg &s : plan.segs)
        if (!s.native)
            plan.island_count++;
    plan.container_ready = !plan.segs.empty() && plan.island_count == 0;
    return plan;
}

/* ---------------------------------------------------------------------------
 * model-flip M3 (plans/model-flip.md): the first NATIVE CONTAINER emission -
 * the inversion "bytecode with native islands" -> "native with bytecode
 * islands", on the simplest MIXED shape. A container fragment DRIVES the whole
 * body; its ONE interpreted ISLAND becomes a `call jit_exec_block`, and the
 * native ops (here just the trailing ReturnV, plus any straight-line native
 * arithmetic) are machine code. One EnterNative at pc 0.
 *
 * The M3 gate is deliberately narrow (M4 widens it - branches/loops, richer
 * island op sets; M5 - calls): a LEAF, STRAIGHT-LINE body (no branches, no
 * handlers, no calls) that is exactly ONE contiguous island of SIMPLE boxed
 * scalar ops surrounded by native non-branch ops, ending in ReturnV. Narrow
 * enough that no bench/sample matches it (verified -vd byte-identical), so the
 * existing per-run path is untouched; proven by a dedicated test + the
 * differential + the fuzzer.
 * ------------------------------------------------------------------------- */

/* The boxed island ops M3 admits: pure scalar arithmetic/compare/logic/move/
 * load/coerce - no control flow, no calls, no container builders. They run
 * interpreted inside vm_exec_block (a throw is handled via the RAISED bridge);
 * anything else declines the container (falls to the per-run path). */
/* The minimum island size for M3 to form a container (see the gate below). */
static constexpr size_t MIN_CONTAINER_ISLAND = 5;

static bool op_is_simple_island(OpCode op)
{
    switch (op) {
    /* The simple boxed SCALAR ops, SliceV, the CONTAINER/STRUCT BUILDS
     * (MakeArrayV/MakeDictV/StructCtorBoxedV), DeclConstV AND the map/filter
     * pair (CheckFuncV/MapFilterV) are ALL op_run_eligible now (the
     * nativize-ops path), so op_run_eligible (checked FIRST in the gate)
     * wins - they never reach here as islands. They stay listed for
     * documentation / a `-nj` build (where op_run_eligible is false). The
     * ONLY still-boxed sequential ops are the dyn-callee generic call pair
     * below (CheckCallableV + CallValueGenericV - fully AST-FREE since F1
     * step 2, reading the serializable call_sites pool; simply not yet
     * given an emit case, as its by-Kind dispatch is the most involved
     * remaining one) - the container gate's island source, so the
     * jit_exec_block mechanism (M2-M4) stays exercised on real dyn-dispatch
     * code. (The island-source hop chain: SliceV -> MakeArrayV -> MakeDictV
     * -> StructCtorBoxedV -> DeclConstV -> CheckFuncV/MapFilterV -> this
     * pair, each hop perf-checked by probing bench/ + samples/ for
     * containers formed.) */
    case OpCode::BinOpV: case OpCode::CmpV: case OpCode::LogV:
    case OpCode::UnaryV: case OpCode::MoveV: case OpCode::CompoundV:
    case OpCode::LoadConstV: case OpCode::CoerceNumV:
    case OpCode::SliceV:
    case OpCode::MakeArrayV:
    case OpCode::MakeDictV:
    case OpCode::StructCtorBoxedV:
    case OpCode::DeclConstV:
    case OpCode::CheckFuncV:
    case OpCode::MapFilterV:
    case OpCode::CheckCallableV:
    case OpCode::CallValueGenericV:
        return true;
    default:
        return false;
    }
}

/* Emit a container fragment's ISLAND CALL: `call jit_exec_block(desc,
 * island_pc)` (SysV rdi=desc, rsi=island_pc), then branch on the result -
 * FellThrough (a small resume pc) falls through to the next native op; RAISED
 * (a high-bit-set sentinel) exits to island_pc so the EnterNative handler
 * re-raises g_vm_jit_exc. The prologue/epilogue save+restore rdi (the slots
 * base) around the call and re-materialise rsi=t_int / r8=t_float; the cache is
 * empty in a container (no N5), so the prologue's single `push rdi` leaves rsp
 * 16-aligned for the call. */
static void emit_island_call(Emitter &e, const FuncDescriptor *desc,
                             uint32_t island_pc)
{
    emit_call_prologue(e);                 /* push rdi (empty cache -> aligned) */
    e.movabs(RDI, reinterpret_cast<uint64_t>(desc));        /* arg1 = desc */
    e.movabs(RSI, island_pc);                               /* arg2 = from_pc */
    e.call_relocs.push_back(
        { e.pos(), reinterpret_cast<const void *>(jit_exec_block) });
    e.u8(0xE8); e.u32(0);                                    /* call rel32 */
    emit_call_epilogue(e);                 /* pop rdi; rsi=t_int; r8=t_float */
    e.u8(0x48); e.u8(0x85); e.u8(0xC0);                      /* test rax, rax */
    e.u8(0x79); const size_t jfix = e.pos(); e.u8(0);        /* jns +over (rel8)*/
    e.u8(0xB8); e.u32(island_pc);                    /* mov eax, island_pc */
    e.u8(0xC3);                                       /* ret -> EnterNative raise*/
    e.b[jfix] = static_cast<uint8_t>(e.pos() - (jfix + 1)); /* patch the jns */
}

/* Try to compile `chunk` as an M3 native container. Returns true iff it
 * matched the gate AND emitted successfully (chunk.code + chunk.native
 * committed); false leaves `chunk` PRISTINE for the normal per-run path. */
static bool jit_try_container(Chunk &chunk, const JitCtx *jc)
{
    if (!jc || !jc->caller_desc)          /* need a descriptor to bake + reach */
        return false;
    const size_t n = chunk.code.size();
    if (n < 2 || chunk.code[n - 1].op != OpCode::ReturnV)
        return false;

    /* Gate: collect EVERY contiguous island of simple boxed ops (M4b: multiple
     * islands - the common real shape, e.g. an init MoveV + a body island);
     * every other op a native RUN-eligible op (M4: incl. NATIVE branches - the
     * loop control - handled by emit_branch); no calls; a boxed condition
     * (JumpUnlessTrueV etc. - not run-eligible) declines to the per-run path
     * (M4b: the BRANCHED island-exit); ReturnV only last. */
    std::vector<std::pair<size_t, size_t>> islands;   /* {begin, end} in order */
    bool in_island = false, has_branch = false;
    for (size_t p = 0; p < n; p++) {
        const OpCode op = chunk.code[p].op;
        if (op == OpCode::CallV || op == OpCode::CachedCallV
                || op == OpCode::CallValueV)
            return false;                  /* M5 territory (containers stay
                                            * call-free for now) */
        /* op_run_eligible FIRST (native) - an op that the JIT can emit is NATIVE
         * in the container, NOT an island. This ORDER is load-bearing: a
         * newly-nativized op (MoveV/LoadConstV/...) is ALSO in the stale
         * op_is_simple_island whitelist, and checking that first would classify
         * it an island (run interpreted via vm_exec_block, its jit_* helper
         * NEVER called) - a verified bug the g_jit_op_run counter caught. */
        if (op_run_eligible(chunk.code[p], jc)) {
            in_island = false;
            if (op_is_branch(op))
                has_branch = true;         /* M4: a native loop/if */
            if (op == OpCode::ReturnV && p != n - 1)
                return false;
        } else if (op_is_simple_island(op)) {   /* a boxed op -> vm_exec_block */
            if (!in_island) { islands.push_back({ p, p + 1 }); in_island = true; }
            else islands.back().second = p + 1;
        } else {
            return false;                  /* boxed branch / handler -> decline */
        }
    }
    if (islands.empty())
        return false;                      /* fully native -> the normal path */

    /* A native branch may target an island START (the `call jit_exec_block`, a
     * valid fragment label) but NOT an island INTERIOR (no fragment code there -
     * the island runs inside vm_exec_block); and it must stay in-body (a target
     * == n would exit the fragment mid-run). Well-formed loops target a block
     * leader in range, so this only ever declines a pathological shape. */
    for (size_t p = 0; p < n; p++) {
        const int t = branch_pc_target(chunk.code[p]);
        if (t < 0)
            continue;
        if (t >= static_cast<int>(n))
            return false;
        for (const auto &isl : islands)
            if (t > static_cast<int>(isl.first)
                    && t < static_cast<int>(isl.second))
                return false;
    }

    /* A container WITH a native loop (branches) is the M4 WIN: the loop control
     * (test + step + back edge) iterates in machine code around the island(s). A
     * STRAIGHT-LINE container (no branch) is only a mechanism proof - the islands
     * run interpreted either way, so the container merely ADDS overhead; gate it
     * OFF for small bodies (total island ops), matching NO tiny hot function. */
    if (!has_branch) {
        size_t total = 0;
        for (const auto &isl : islands)
            total += isl.second - isl.first;
        if (total < MIN_CONTAINER_ISLAND)
            return false;
    }

    /* remap: EnterNative@0 shifts everything +1; each island inserts an ExitBlock
     * after it, shifting every pc past that island's end +1 more. So
     * remap[p] = 1 + p + (# islands ending at or before p). A VECTOR (not a
     * lambda) - emit_branch consumes it for out-of-run exits (none here,
     * begin=0/end=n). */
    std::vector<int> remap(n + 1);
    for (size_t p = 0; p <= n; p++) {
        int shift = 1;
        for (const auto &isl : islands)
            if (isl.second <= p) shift++;
        remap[p] = static_cast<int>(p) + shift;
    }

    /* Emit the container fragment (over the ORIGINAL ops; the rebuild is after).
     * The whole body is one fragment at offset 0. */
    Emitter e;
    e.cache.clear();                       /* no N5 register cache in a container*/
    std::vector<NativeCode::OpMark> marks;  /* -vdj: op-boundary annotations */
    std::vector<size_t> label(n, 0);        /* fragment offset of each body pc */
    std::vector<Fixup> fixups;              /* fragment-local branch fixups */
    e.movabs(RSI, reinterpret_cast<uint64_t>(jit_layout().t_int));
    if (run_has_float(chunk, 0, n))
        e.movabs_r8(reinterpret_cast<uint64_t>(jit_layout().t_float));
    size_t isl_idx = 0;                     /* islands are in ascending order */
    for (size_t pc = 0; pc < n; ) {
        if (g_jit_annotate)
            marks.push_back({ static_cast<uint32_t>(e.pos()),
                              static_cast<uint32_t>(remap[pc]) });
        if (isl_idx < islands.size() && pc == islands[isl_idx].first) {
            const size_t ib = islands[isl_idx].first, ie = islands[isl_idx].second;
            label[pc] = e.pos();           /* a back edge may target this call */
            emit_island_call(e, jc->caller_desc,
                             static_cast<uint32_t>(remap[ib]));
            for (size_t p = ib + 1; p < ie; p++)
                label[p] = e.pos();        /* interiors (defensive; never a tgt) */
            pc = ie;
            isl_idx++;
            continue;
        }
        label[pc] = e.pos();
        if (op_is_branch(chunk.code[pc].op))
            emit_branch(e, chunk, chunk.code[pc],
                        static_cast<uint32_t>(remap[pc]), 0, n, remap, fixups);
        else if (!emit_op(e, chunk, chunk.code[pc],
                          static_cast<uint32_t>(remap[pc]), jc, pc))
            return false;                  /* selection miss: chunk pristine */
        pc++;
    }
    for (const Fixup &f : fixups)          /* patch each fragment-local branch */
        e.patch32(f.site,
                  static_cast<uint32_t>(label[f.target_pc] - (f.site + 4)));
    /* The body ends in a native ReturnV (emit_op emitted its `ret`); every
     * other exit is a branch to an in-body label, so no trailing exit_pc. */

    /* Finalize: trampolines for out-of-range calls, mmap W^X, patch call
     * rel32s, flip RX. (Self-contained - does NOT touch the per-run path.) */
    std::unordered_map<const void *, size_t> tramp;
    for (const Emitter::CallReloc &r : e.call_relocs) {
        if (tramp.count(r.fn)) continue;
        tramp[r.fn] = e.pos();
        e.movabs(RAX, reinterpret_cast<uint64_t>(r.fn));
        e.u8(0xFF); e.u8(0xE0);                             /* jmp rax */
    }
    const size_t len = e.b.size();
    void *mem = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return false;                      /* chunk still pristine */
    std::memcpy(mem, e.b.data(), len);
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
        return false;
    }

    /* COMMIT: rebuild the code (EnterNative@0 + an ExitBlock after EACH island +
     * branch-target/side-table remap), then adopt the native buffer. Only past
     * here does `chunk` change. */
    std::vector<Instr> nc;
    nc.reserve(n + 1 + islands.size());
    Instr en;
    en.op = OpCode::EnterNative;
    { Operand o; o.is_lit = true; o.lit_kind = Operand::LitKind::i; o.lit = 0;
      en.set_a(o); }
    nc.push_back(en);
    size_t rb_isl = 0;
    for (size_t pc = 0; pc < n; pc++) {
        Instr in = chunk.code[pc];
        /* M4: remap a NATIVE branch's target (in.target) - the interpreted
         * originals are kept, so a bail/resume in a non-deletable case still
         * dispatches correctly. Same audited branch-op list as the per-run
         * rebuild. */
        if (branch_pc_target(in) >= 0 && in.target >= 0
                && static_cast<size_t>(in.target) <= n)
            in.target = remap[in.target];
        nc.push_back(in);
        if (rb_isl < islands.size() && pc == islands[rb_isl].second - 1) {
            Instr eb;
            eb.op = OpCode::ExitBlock;
            Operand o; o.is_lit = true; o.lit_kind = Operand::LitKind::i;
            o.lit = static_cast<int_type>(remap[islands[rb_isl].second]);
            eb.set_a(o);
            nc.push_back(eb);
            rb_isl++;
        }
    }
    chunk.code = std::move(nc);
    for (auto &l : chunk.locs)
        l.pc = static_cast<uint32_t>(remap[l.pc]);
    for (auto &ic : chunk.inline_ctxs)
        ic.pc = static_cast<uint32_t>(remap[ic.pc]);

    chunk.native.base = mem;
    chunk.native.len = len;
    if (g_jit_annotate)
        chunk.native.frags.push_back(
            { 0, static_cast<uint32_t>(len), std::move(marks) });
    g_jit_frags++;
    return true;
}

void jit_compile_chunk(Chunk &chunk, const JitCtx *jc)
{
    if (!g_jit_enabled || chunk.code.empty())
        return;

    /* model-flip M3: try the native-container path first (a narrow gate); on a
     * match it emits the whole-function container and we're done. Otherwise the
     * chunk is pristine and the per-run path below runs unchanged. */
    if (jit_try_container(chunk, jc))
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
        if (!op_run_eligible(chunk.code[i], jc)) { i++; continue; }
        size_t j = i + 1;
        while (j < n && op_run_eligible(chunk.code[j], jc))
            j++;
        /* No minimum run length (MIN_RUN removed 2026-07-25): with most ops
         * nativized, the short runs a floor excluded were mostly whole TINY
         * bodies - a 2-op comparator `func(a,b) => a < b` - which become
         * native_leaf and get CALLED directly by a caller fragment, paying
         * no EnterNative at all. Measured (callgrind Ir, floor 4 -> none):
         * 34_sort_custom_cmp 0.929x, 35_map_filter 0.948x, 57_bool_reduce
         * 0.973x; loop/recursion benches neutral. */
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
            if (!op_fully_native(chunk.code[p]))
                ok = false;
        for (size_t p = 0; p < n && ok; p++) {   /* single-entry */
            const int t = branch_pc_target(chunk.code[p]);
            if (t > static_cast<int>(b) && t < static_cast<int>(en)
                    && (p < b || p >= en))
                ok = false;                       /* external -> interior */
        }
        /* Re-raise deletability guard: an op spliced from an INLINED body
         * carries an inline_ctxs entry (pc -> inlined-at chain). Deleting
         * the run collapses every such pc onto the EnterNative - DISTINCT
         * chains would merge onto one pc, so a RAISE's virtual-frame flush
         * (vm_raise's vm_flush_inline at the collapsed exit pc) could emit
         * the WRONG chain. Only a run that CAN raise cares (a conveying op:
         * fully-native but not never-exits); an all-never-exits run hosts
         * no raise, so its inline entries are never flushed from within. */
        if (ok) {
            bool can_raise = false;
            for (size_t p = b; p < en && !can_raise; p++)
                if (!op_never_exits(chunk.code[p]))
                    can_raise = true;
            if (can_raise) {
                for (const auto &ie : chunk.inline_ctxs) {
                    if (ie.pc >= b && ie.pc < en) {
                        ok = false;
                        break;
                    }
                }
            }
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
        std::vector<char> cache_barrier(end - begin, 0);
        {
            static const uint8_t cregs[2] = { 10, 11 };
            const std::vector<int> hot =
                pick_cached_slots(chunk.code, begin, end, chunk.slot_count,
                                  &cache_barrier);
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
            /* An op that touches slots the emitter cannot enumerate is
             * BRACKETED: flush the pinned registers so it reads current values,
             * reload after so anything it wrote is picked up (the ordinary
             * spill-around-a-call). Never a branch op, so the reload always
             * executes. */
            const bool brk = cache_barrier[pc - begin] && !e.cache.empty();
            std::vector<Emitter::CacheEnt> saved_cache;
            if (brk) {
                e.flush_cache();
                /* EMPTY the cache across the op's emission: a barrier'd op
                 * that THROWS exits via exit_pc BEFORE the reload, and
                 * exit_pc's flush would write the PRE-CALL register values
                 * over slots the helper already modified (a cached slot among
                 * a throwing MultiUnpackV/CallBuiltinV's written targets
                 * would be clobbered back - the interpreter keeps the partial
                 * write). With no cache entries, the op's exits flush nothing
                 * and memory keeps the helper's writes; any in-op operand
                 * read falls back to (current) memory. */
                saved_cache = std::move(e.cache);
                e.cache.clear();
            }
            if (op_is_branch(in.op)) {
                emit_branch(e, chunk, in, static_cast<uint32_t>(remap[pc]),
                            begin, end, remap, fixups);
            } else if (!emit_op(e, chunk, in,
                                static_cast<uint32_t>(remap[pc]), jc, pc)) {
                e.b.clear();
                return;                    /* selection bug: give up */
            }
            if (brk) {
                e.cache = std::move(saved_cache);
                e.reload_cache();
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

    /* #55 native calls: native_leaf (the FLAG) is set by codegen_chunk via
     * jit_chunk_is_native_leaf, so it is available BEFORE any jit (the STEP-2
     * ordering fix). Here we just record the fragment ENTRY offset for it - a
     * native_leaf is, by that predicate, exactly a single deletable run over
     * [0,n), so runs[0] is it. ML_CHECK the run analysis agrees (defense). */
    if (chunk.native_leaf) {
        ML_CHECK(runs.size() == 1 && runs[0].begin == 0
                 && runs[0].end == n && deletable[0]);
        chunk.native_entry_off = frag_off[0];
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

void jit_compile_chunk(Chunk &, const JitCtx *)
{
}

bool jit_chunk_is_native_leaf(const Chunk &)
{
    return false;
}

ContainerPlan jit_container_plan(const Chunk &, const JitCtx *)
{
    return {};   /* no native code off-platform -> no container plan */
}

void jit_type_singletons(const void *&ti, const void *&tf, const void *&ta)
{
    ti = tf = ta = nullptr;
}

#endif
