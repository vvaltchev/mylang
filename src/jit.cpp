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
#include "codegen.h"
#include "bytecode.h"
#include "evalvalue.h"
#include "funcdesc.h"   /* FuncDescriptor::vm_chunk (native-call gate, STEP 2.1) */
#include "structtype.h" /* StructObject layout probe (baked member/ctor) */
#include "eval.h"       /* builtin_slot (the LoadBuiltinV emit-time bytes) */

#include <algorithm>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <cstring>
#include <cmath>
#include <vector>

#ifdef TESTS
/* struct baked-fast-path execution proof (see jit.h) */
extern "C" {
unsigned long g_jit_member_fast = 0, g_jit_ctor_fast = 0;
unsigned long g_jit_boxed_fast = 0;    /* #60: inline int-int boxed-op runs */
unsigned long g_jit_store_fast = 0;    /* #92: inline element-STORE runs */
unsigned long g_jit_hoist_rmw = 0;     /* C1e: hoisted-compound stores */
unsigned long g_jit_fcache = 0;        /* C2a: float-pinned fragment entries */
unsigned long g_jit_store_prep = 0;    /* #92: prep (COW-clone) slow calls */
unsigned long g_jit_elem2_fast = 0;    /* #93: inline nested-READ runs */
unsigned long g_jit_store2_fast = 0;   /* #95: inline nested-STORE runs */
unsigned long g_jit_elem_slice_fast = 0; /* #95: inline slice-READ runs */
unsigned long g_jit_fwd = 0;           /* lever A: forwarded consumers RUN */
unsigned long g_jit_hoist = 0;         /* C1: hoisted-nav loop ENTRIES */
unsigned long g_jit_sync_inline = 0;   /* fragment-inline sync calls run */
unsigned long g_jit_entry_resume = 0;  /* post-call entry stubs entered */
}
#endif

/* ML_JIT_SUPPORTED is the POLICY, defined once in jit.h - see the comment
 * there for which platforms and why. */
#if ML_JIT_SUPPORTED
#  include <sys/mman.h>
#  include <unistd.h>
#  include <cstdlib>
#else
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
/*
 * M5a - THE DEDICATED NATIVE STACK (plans/native-gap-roadmap.md /
 * plans/model-flip.md M5). Each native call level (the sync-inline
 * `call rdx`, the #55 direct call) nests a machine frame; on the C stack
 * that forced the tiny sync depth cap (200). Fragments now execute on a
 * RESERVED 1GB MAP_NORESERVE region (virtual only - lazy commit means RSS
 * = pages actually touched; a PROT_NONE guard page at the LOW end
 * backstops the checked cap), switched to at the OUTERMOST jit_enter
 * (g_on_nstack: helper-called C++ and its jit_enter re-entries stay on
 * the native stack; the dispatch loop and everything outside run on the
 * C stack as before). With the stack armed the sync depth cap is raised
 * to ~500k (the per-level cost is a few hundred bytes: the caller's
 * prologue pushes + the call ra + the push-helper's transient frame; the
 * 32MB slack absorbs one full interpreted excursion's C++ frames), so
 * deep recursion stays native instead of falling interpreted at 200 -
 * and the CallV SELF-gate lifts (op_run_eligible), since its rationale
 * was exactly the cap round-trip pathology.
 *
 * OFF under ASan (running C++ on a custom stack trips its stack
 * machinery - the PoolAlloc pass-through philosophy: the debug lanes
 * keep their bug-finding power) and via MYLANG_NATIVE_STACK=0 (the
 * same-binary A/B lever, like MYLANG_JIT=0). Off means the old cap and
 * the old self-gate - behavior identical to pre-M5a.
 */
#if ML_JIT_SUPPORTED
#if defined(__SANITIZE_ADDRESS__)
#  define ML_NSTACK_OFF 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ML_NSTACK_OFF 1
#  endif
#endif
/* ONE variable carries both states (the per-element VmInvoker re-entry
 * pays this check per callback, so it is kept to one load + two stores):
 * null = not armed OR currently ACTIVE (either way: plain call on the
 * current stack); else = the armed, inactive stack's top. */
static char *g_nstack_cur = nullptr;
static char *g_nstack_top = nullptr;    /* the armed top (a baked imm in
                                         * emitted site switches) */
static void *g_nstack_saved_rsp = nullptr;  /* the OUTERMOST emitted site's
                                             * C rsp (single-threaded; nested
                                             * sites are plain by cur==null) */

/* M5a: PAUSE the native stack for a builtin->callback ELEMENT LOOP
 * (VmInvoker): the per-fragment-entry stack switch measured ~1% on the
 * map/filter/sort benches, and a per-element callee never needs the deep
 * stack. Paused entries run plainly on the C stack; correctness of the
 * baked 500k guard is restored by the AUTHORITATIVE runtime cap check in
 * jit_sync_push_common + jit_call_sync* (the cap is lowered to the
 * C-stack-safe 200 for the pause's duration - VmInvoker's ctor/dtor
 * bracket the whole loop, ONE flip per loop). */
void jit_native_stack_init()
{
    static bool done = false;
    if (done)
        return;
    done = true;
#ifndef ML_NSTACK_OFF
    const char *e = getenv("MYLANG_NATIVE_STACK");
    if (e && e[0] == '0' && e[1] == 0)
        return;
    const size_t RESERVE = 1ull << 30;                    /* 1GB virtual */
    void *m = mmap(nullptr, RESERVE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED)
        return;                       /* no stack -> old cap, old gate */
    mprotect(m, 4096, PROT_NONE);     /* low guard (the stack grows DOWN) */
    g_nstack_top = static_cast<char *>(m) + RESERVE;
    g_nstack_cur = g_nstack_top;
    jit_set_sync_depth_cap(500000);
#else
    /* SANITIZED build (the stack is pass-through): every sync level below
     * the cap is a C-stack frame whose vm_dispatch alone is ~77KB under
     * clang ASan (per-case locals + redzones, no scoped-local overlap) -
     * the historical 200 was ~15MB of frames and overflowed the 8MB
     * default stack in the pinned-cap recursion test. 32 levels (~4MB
     * worst-case) leaves real margin; past the cap a sync call falls
     * interpreted (in-VM, no C recursion), so this is a perf knob the
     * correctness lanes don't care about. */
    jit_set_sync_depth_cap(32);
#endif
}
#else
void jit_native_stack_init() { }
#endif

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#elif defined(__GNUC__)
__attribute__((no_sanitize_undefined))
#endif
size_t jit_enter(const void *frag, void *slots)
{
    /* PLAIN, byte-identical to pre-M5a - this runs per FRAGMENT ENTRY
     * (millions per second in a callback loop; even a 3-instruction
     * conditional here measured ~1% on map/filter/sort). The native-
     * stack switch lives where the NESTING happens instead: the emitted
     * sync call site (outermost-only, see emit_sync_call_inline) and
     * jit_enter_deep (the helper path's direct entry, below). */
    typedef size_t (*NativeFrag)(void *);
    return reinterpret_cast<NativeFrag>(const_cast<void *>(frag))(slots);
}

/* The switching entry for jit_call_sync_core's DIRECT callee entry (the
 * helper path - a coerced-bind/cached callee): each core level brackets
 * its own switch; nested levels see the nulled g_nstack_cur and stay
 * plain (already on the native stack). r12 carries the C rsp across the
 * switch. It is CALLEE-SAVED, which is what makes that safe - every C++
 * helper preserves it, and so does a fragment: r12 is in the emitter's
 * N5 cache pool now, so a fragment that pins it pushes it at frag_entry
 * and pops it at every exit. (Before the pool moved to the callee-saved
 * registers this comment said "never used by the emitter"; the guarantee
 * is now save/restore rather than avoidance.) mmap page alignment + the
 * `call`'s ra push give the fragment the rsp % 16 == 8 entry contract. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#elif defined(__GNUC__)
__attribute__((no_sanitize_undefined))
#endif
size_t jit_enter_deep(const void *frag, void *slots)
{
    typedef size_t (*NativeFrag)(void *);
#if ML_JIT_SUPPORTED
    if (char *top = g_nstack_cur) {
        g_nstack_cur = nullptr;
        size_t r;
        asm volatile(
            "movq %%rsp, %%r12\n\t"
            "movq %2, %%rsp\n\t"
            "callq *%1\n\t"
            "movq %%r12, %%rsp\n\t"
            : "=a"(r)
            : "r"(frag), "r"(top), "D"(slots)
            : "r12", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11",
              "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
              "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
              "xmm14", "xmm15", "memory", "cc");
        g_nstack_cur = top;
        return r;
    }
#endif
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
    int arr_off_off;      /* SharedArrayObj: offset of `off` (u32; #95 slice
                           * reads - elements live at data + (off + i)) */
    int arr_len_off;      /* SharedArrayObj: offset of `len` (u32; a slice's
                           * element count - its bounds, NOT the vector's) */
    int kind_off;         /* SharedObject: &kind - shobj */
    int data_off;         /* SharedObject: &elem_vec - shobj (the vector's
                           * _M_start is at +0, _M_finish at +8) */
    unsigned char kind_ints, kind_floats, kind_bools;  /* Storage values */
    unsigned char kind_general;   /* #93: the nested read's OUTER array */
    /* #92 the inline STORE tier (see SharedArrayObj::JitProbe) */
    int ro_off;        /* SharedObject: &readonly   - shobj */
    int hashv_off;     /* SharedObject: &hash_valid - shobj */
    int slices_off;    /* SharedObject: &has_slices - shobj */
    int lv_const_off;  /* LValue: &is_const - &slot */
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
    int act_top_rec;      /* VmActivation::top_rec (the cached &back_rec -
                           * maintained by every push/pop incl. the emitted
                           * M5b push, ML_VM_CHECK-verified) */
    int act_pends;        /* VmActivation::pends (vector<VmPendState>) */
    int rec_pend_base;    /* VmCallRec::pend_base (u32) */
    int pend_state_size;  /* sizeof(VmPendState) */
    int pend_state_pend;  /* VmPendState::pend (a byte enum) */
    /* #55 STEP 2.1 native-call member offsets (via the vm.cpp probes) */
    int desc_vm_chunk;    /* FuncDescriptor::vm_chunk */
    int chunk_native_base;/* Chunk::native.base */
    int chunk_native_entry;/* Chunk::native_entry_off */
    /* Struct baked layout (the 64_struct_create fix): the member-read /
     * planned-ctor fast paths navigate a StructObject directly. */
    const void *t_struct; /* the struct-instance Type singleton */
    int sobj_def;         /* StructObject::def offset */
    int sobj_ro;          /* StructObject::readonly offset */
    int sobj_bytes;       /* StructObject::bytes (vector; data ptr at +0) */
    int sobj_rc;          /* RefCounted::intr_refcount offset (H1 use_count) */
    bool sobj_ok;         /* the vector-data-at-+0 probe held */
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
        l.arr_off_off = static_cast<int>(
            reinterpret_cast<const char *>(&arr.off) -
            reinterpret_cast<const char *>(&arr));
        l.arr_len_off = static_cast<int>(
            reinterpret_cast<const char *>(&arr.len) -
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
        l.kind_general = static_cast<unsigned char>(
            SharedArrayObj::Storage::general);
        l.ro_off = static_cast<int>(
            static_cast<const char *>(jp.readonly) - so);
        l.hashv_off = static_cast<int>(
            static_cast<const char *>(jp.hash_valid) - so);
        l.slices_off = static_cast<int>(
            static_cast<const char *>(jp.has_slices) - so);
        l.lv_const_off = static_cast<int>(
            reinterpret_cast<const char *>(&alv.jit_const_probe()) -
            reinterpret_cast<const char *>(&alv));
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
        l.act_top_rec = static_cast<int>(jit_off_act_top_rec());
        l.act_pends = static_cast<int>(jit_off_act_pends());
        l.rec_pend_base = static_cast<int>(jit_off_rec_pend_base());
        l.pend_state_size = static_cast<int>(jit_sizeof_pend_state());
        l.pend_state_pend = static_cast<int>(jit_off_pend_state_pend());
        l.desc_vm_chunk = static_cast<int>(jit_off_desc_vm_chunk());
        l.chunk_native_base = static_cast<int>(jit_off_chunk_native_base());
        l.chunk_native_entry = static_cast<int>(jit_off_chunk_native_entry());
        /* Struct baked layout: direct member offsets (public members, so a
         * co-located JitProbe isn't needed - a layout change relocates these
         * live members and the probe below still measures them). The
         * vector-data-at-+0 check guards the libstdc++/libc++ assumption the
         * bytes-pointer load bakes; if it ever fails, both fast paths are
         * disabled (helper fallbacks only). */
        {
            auto sp = make_intrusive<StructObject>();
            LValue slv(EvalValue(intrusive_ptr<StructObject>(sp)), false);
            l.t_struct = slv.get().get_type();
            const char *sb = reinterpret_cast<const char *>(sp.get());
            l.sobj_def = static_cast<int>(
                reinterpret_cast<const char *>(&sp->def) - sb);
            l.sobj_ro = static_cast<int>(
                reinterpret_cast<const char *>(&sp->readonly) - sb);
            l.sobj_bytes = static_cast<int>(
                reinterpret_cast<const char *>(&sp->bytes) - sb);
            l.sobj_rc = static_cast<int>(
                reinterpret_cast<const char *>(&sp->intr_refcount) - sb);
            std::vector<char> vp(3, 'x');
            l.sobj_ok = *reinterpret_cast<char *const *>(&vp) == vp.data();
        }
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
 * Register plan (System V):
 *   rbx = the slot window base - CALLEE-SAVED, so a helper call preserves
 *         it and no save/restore is needed around one. A fragment is
 *         entered with the window in rdi (the ABI argument) and does
 *         `push rbx; mov rbx, rdi` (frag_entry); every exit pops it
 *         (frag_ret / exit_pc).
 *   rdi = free scratch / the first helper ARGUMENT. It used to be the
 *         base, which is why every helper call had to push it: forming an
 *         argument (`lea rdi, [rdi+off]`) DESTROYED the base.
 *   rsi = the int Type singleton (loaded once per fragment)
 *   rax = the accumulator (op result; also the returned resume pc)
 *   rcx = the second operand / shift count / idiv divisor
 *   rdx = idiv's remainder (clobbered by cqo/idiv only)
 *
 * STACK ALIGNMENT. A fragment is entered with rsp % 16 == 8 (the caller's
 * `call` pushed a return address); frag_entry's single push makes it 0,
 * which IS the call-ready state, so an emitted call site needs an EVEN
 * number of pushes. That is why emit_call_prologue pads on an odd cache
 * size, and why the sites that spill by hand below all push in pairs.
 */

/* modrm addressing byte for `[rbx + disp32]`: mod = 10 (disp32), rm = 011
 * (rbx), reg field OR'd in by the caller. The old base, rdi, was rm = 111
 * (0x87) - every slot access in the emitter went through one of these. */
static constexpr uint8_t MODRM_SLOT = 0x83;
static constexpr uint8_t REG_SLOTS_BASE = 3;    /* rbx */
static constexpr uint8_t REG_ARG0 = 7;          /* rdi */

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
     * exit/bail). reg is a CALLEE-saved GP (r12-r15), so a helper call
     * preserves it: the save is one push at frag_entry instead of a
     * push/pop around every call. */
    struct CacheEnt { int slot; int32_t payload, type; uint8_t reg; };
    std::vector<CacheEnt> cache;
    /* C2a: the FLOAT half of the pool - hot float slots pinned in
     * xmm4-xmm7 (xmm0/1 stay the per-op scratch). xmm registers are ALL
     * caller-saved, so unlike the GP pins these are spilled to their
     * slot's payload around every helper call (emit_call_prologue) and
     * reloaded after (emit_call_epilogue) - the pre-step-2a discipline,
     * acceptable because the float-loop shapes that matter make few or
     * no helper calls. reg = the xmm index. */
    std::vector<CacheEnt> fcache;
    int fcreg(int slot) const
    {
        for (const CacheEnt &c : fcache)
            if (c.slot == slot)
                return c.reg;
        return -1;
    }

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

    /* mov <reg64>, [rbx + disp32]  (reg 0-15; REX.R for r8-r15) */
    void load(uint8_t reg, int32_t disp)
    {
        u8(static_cast<uint8_t>(0x48 | (reg >= 8 ? 0x04 : 0)));
        u8(0x8B);
        u8(static_cast<uint8_t>(MODRM_SLOT | ((reg & 7) << 3)));
        u32(static_cast<uint32_t>(disp));
    }
    /* mov [rbx + disp32], <reg64>  (reg 0-15) */
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
        u8(static_cast<uint8_t>(MODRM_SLOT | ((reg & 7) << 3)));
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
        /* the float pins: t_float rides in r8, which is live in any
         * fragment that HAS float pins (they only arise from float ops,
         * so run_has_float set it at entry and every helper-call
         * epilogue re-materialises it) */
        for (const CacheEnt &c : fcache) {
            store(8 /*r8 = t_float*/, c.type);
            fstore(c.reg, c.payload);
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
        for (const CacheEnt &c : fcache)
            fload(c.reg, c.payload);
    }
    /* movabs <reg64>, imm64 */
    void movabs(uint8_t reg, uint64_t imm)
    {
        u8(0x48); u8(static_cast<uint8_t>(0xB8 | reg)); u64(imm);
    }
    /* The CALLEE-SAVED registers this fragment took over, beyond the base:
     * the N5 cache registers. Set once at the entry, replayed in reverse
     * at every exit. Empty for a fragment that caches nothing. */
    std::vector<uint8_t> saved;

    /* Total pushes at entry, including the base and any 8-byte pad. A
     * fragment is entered at rsp % 16 == 8, so an ODD count lands the body
     * at 0 - which IS the call-ready state every emitted call site assumes.
     * Hence the pad when 1 + saved.size() is even. */
    bool entry_pad() const { return (1 + saved.size()) % 2 == 0; }

    /* THE FRAGMENT ENTRY: the window arrives in rdi (from jit_enter, an
     * emitted sync `call rdx`, or a native_leaf direct call). rbx and the
     * cache registers are callee-saved, so save the caller's and take them
     * over. Called AFTER `saved` is filled (the cache pick decides it). */
    void frag_entry()
    {
        push_reg(REG_SLOTS_BASE);
        for (const uint8_t r : saved)
            push_reg(r);
        if (entry_pad())
            { u8(0x48); u8(0x83); u8(0xEC); u8(0x08); }       /* sub rsp,8 */
        mov_rr(REG_SLOTS_BASE, REG_ARG0);
    }
    /* THE FRAGMENT RETURN: give the caller its registers back, then ret.
     * rax (the resume pc / sentinel) is already set by the caller of this,
     * and neither pop nor the pad adjustment touches it. */
    void frag_ret()
    {
        if (entry_pad())
            { u8(0x48); u8(0x83); u8(0xC4); u8(0x08); }       /* add rsp,8 */
        for (size_t i = saved.size(); i-- > 0; )
            pop_reg(saved[i]);
        pop_reg(REG_SLOTS_BASE);
        u8(0xC3);
    }
    /* Pending `jmp <epilogue>` sites, split by whether the exit must
     * FLUSH the register cache. Patched (and cleared) per fragment by
     * emit_epilogues. */
    std::vector<size_t> epi_flush, epi_bare;

    /* THE EXIT/BAIL: `mov eax, pc; jmp <epilogue>` - a CONSTANT 10 bytes.
     * It used to inline the whole tail (flush the N5 cache, restore, ret),
     * which grew with the cache: at four pinned slots the flush alone is
     * 56 bytes, and the short jcc that several guards use to hop OVER an
     * exit ran out of its 8-bit displacement. Sharing one epilogue per
     * fragment makes every exit the same small size again, and stops the
     * flush being duplicated at ~100 sites.
     *
     * TWO epilogues, because a barrier'd op EMPTIES the cache across its
     * emission on purpose: such an op's exit must NOT flush (the helper
     * already wrote those slots, and flushing would clobber its writes
     * with the stale pre-call register values). The choice is exactly
     * "is the cache live right now", which is what `cache` already says. */
    void exit_pc(uint32_t pc)
    {
        u8(0xB8); u32(pc);                                /* mov eax, pc */
        u8(0xE9);
        (cache.empty() && fcache.empty() ? epi_bare : epi_flush)
            .push_back(pos());
        u32(0);                                           /* jmp rel32 */
    }

    /* Emit this fragment's epilogue(s) and patch the exits that need them.
     * Called once per fragment, after its body and entry stubs. */
    void emit_epilogues()
    {
        if (!epi_flush.empty()) {
            const size_t at = pos();
            flush_cache();
            frag_ret();
            for (const size_t s : epi_flush)
                patch32(s, static_cast<uint32_t>(at - (s + 4)));
            epi_flush.clear();
        }
        if (!epi_bare.empty()) {
            const size_t at = pos();
            frag_ret();
            for (const size_t s : epi_bare)
                patch32(s, static_cast<uint32_t>(at - (s + 4)));
            epi_bare.clear();
        }
    }

    /* ---- N3 SSE float ---- (xmm0=a/acc, xmm1=b; r8 = t_float) */
    /* movsd xmm<r>, [rbx+disp] */
    void fload(uint8_t r, int32_t d)
    {
        u8(0xF2); u8(0x0F); u8(0x10);
        u8(static_cast<uint8_t>(MODRM_SLOT | (r << 3))); u32(uint32_t(d));
    }
    /* movsd [rbx+disp], xmm<r> */
    void fstore(uint8_t r, int32_t d)
    {
        u8(0xF2); u8(0x0F); u8(0x11);
        u8(static_cast<uint8_t>(MODRM_SLOT | (r << 3))); u32(uint32_t(d));
    }
    /* addsd/subsd/mulsd xmm0, xmm1 (op = 0x58/0x5C/0x59) */
    void farith(uint8_t op) { u8(0xF2); u8(0x0F); u8(op); u8(0xC1); }
    /* movsd xmm<dst>, xmm<src> (reg-reg; both < 8, no REX needed) */
    void fmov_rr(uint8_t dst, uint8_t src)
    { u8(0xF2); u8(0x0F); u8(0x10);
      u8(static_cast<uint8_t>(0xC0 | (dst << 3) | src)); }
    /* sqrtsd xmm<d>, xmm<s>  (SSE2) */
    void sqrtsd(uint8_t d, uint8_t s)
    { u8(0xF2); u8(0x0F); u8(0x51);
      u8(static_cast<uint8_t>(0xC0 | (d << 3) | s)); }
    /* push/pop a GP reg (0x50+r / 0x58+r; REX.B for r8..r15) */
    void push_reg(uint8_t r) { if (r >= 8) u8(0x41); u8(0x50 | (r & 7)); }
    void pop_reg(uint8_t r)  { if (r >= 8) u8(0x41); u8(0x58 | (r & 7)); }
    void call_rax() { u8(0xFF); u8(0xD0); }   /* call rax (indirect) */
    /* lea reg, [rbx + disp32]  (an EvalValue-ptr / LValue-ptr helper arg;
     * rm = rbx = slots base). reg is a raw GP number (the Reg enum is
     * declared after this struct, so lea_rdi passes REG_ARG0). */
    void lea(uint8_t reg, int32_t d)
    { u8(0x48); u8(0x8D); u8(static_cast<uint8_t>(MODRM_SLOT | (reg << 3)));
      u32(uint32_t(d)); }
    /* lea rdi, [rbx + disp32]  (rdi = &frame->slots[slot], a helper arg).
     * With the base in rbx this no longer destroys anything - which is
     * exactly why the helper-call prologue no longer saves rdi. */
    void lea_rdi(int32_t d) { lea(REG_ARG0, d); }
    /* mov rdi, rbx - hand the whole slot WINDOW to a helper whose first
     * parameter is `LValue *slots`. Before the base moved to rbx this was
     * implicit (rdi already held it); it is explicit now so that the
     * majority of call sites, which load rdi with something else, pay
     * nothing. */
    void slots_to_arg0() { mov_rr(REG_ARG0, REG_SLOTS_BASE); }
    /* cvtsi2sd xmm<r>, qword [rbx+disp]  (int -> double) */
    void cvt(uint8_t r, int32_t d)
    {
        u8(0xF2); u8(0x48); u8(0x0F); u8(0x2A);
        u8(static_cast<uint8_t>(MODRM_SLOT | (r << 3))); u32(uint32_t(d));
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
    /* mov rax, [rbx+disp]  (a slot's type ptr) */
    void load_type(int32_t d)
    {
        u8(0x48); u8(0x8B); u8(MODRM_SLOT); u32(uint32_t(d));
    }
    /* cmp rax, r8 (t_float) / cmp rax, rsi (t_int) */
    void cmp_rax_r8()  { u8(0x4C); u8(0x39); u8(0xC0); }
    void cmp_rax_rsi() { u8(0x48); u8(0x39); u8(0xF0); }
    /* mov [rbx+disp], r8  (store t_float as a slot's type) */
    void store_r8_type(int32_t d)
    {
        u8(0x4C); u8(0x89); u8(MODRM_SLOT); u32(uint32_t(d));
    }
    /* movabs r8, imm64 (REX.WB) */
    void movabs_r8(uint64_t imm)
    {
        u8(0x49); u8(0xB8); u64(imm);
    }
    /* ---- N4 array element access ---- */
    /* mov r9, [rbx+disp] (a slot: shobj ptr or the index) */
    void mov_r9_slot(int32_t d)
    { u8(0x4C); u8(0x8B); u8(MODRM_SLOT | (1 << 3)); u32(uint32_t(d)); }
    /* movabs r9, imm64 (t_arr singleton or an index literal) */
    void movabs_r9(uint64_t imm) { u8(0x49); u8(0xB9); u64(imm); }
    /* cmp rax, r9 */
    void cmp_rax_r9() { u8(0x4C); u8(0x39); u8(0xC8); }
    /* cmp byte [rbx+disp], imm8  (the slice flag) */
    void cmp_byte_slot(int32_t d, uint8_t imm)
    { u8(0x80); u8(MODRM_SLOT | (7 << 3)); u32(uint32_t(d)); u8(imm); }
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

    /* ---- #92, the inline element-STORE tier ----
     * The value lives in RDI, and that is load-bearing: rax/rcx/rdx/r9 are
     * all live (shobj/data/count/index) and RSI IS RESERVED - it carries
     * the fragment's t_int singleton, so clobbering it makes every later
     * slot write stamp a garbage type (an immediate SEGV, found that way).
     * rdi is free once frag_entry has moved the slots base into rbx. */
    void store_elem_int_rdi()          /* mov [rcx + r9*8], rdi */
    { u8(0x4A); u8(0x89); u8(0x3C); u8(0xC9); }
    void store_elem_byte_dil()         /* mov [rcx + r9], dil */
    { u8(0x42); u8(0x88); u8(0x3C); u8(0x09); }
    void mov_byte_rax_imm(int32_t d, uint8_t imm)   /* mov byte [rax+d],i */
    { u8(0xC6); u8(0x80); u32(uint32_t(d)); u8(imm); }
    size_t jmp32()                     /* jmp rel32 -> patch32_here */
    { u8(0xE9); const size_t at = pos(); u32(0); return at; }
    /* jmp rel32 to a KNOWN (earlier) position - the prep retry loop */
    void jmp32_to(size_t target)
    { u8(0xE9); u32(static_cast<uint32_t>(target - (pos() + 4))); }
    void mov_rsi_r9() { u8(0x4C); u8(0x89); u8(0xCE); }
    /* ---- #93, the inline nested-READ tier (rcx = a row LValue*) ---- */
    void mov_rax_rcx_d(int32_t d)      /* mov rax, [rcx+disp] */
    { u8(0x48); u8(0x8B); u8(0x81); u32(uint32_t(d)); }
    void cmp_byte_rcx(int32_t d, uint8_t imm)  /* cmp byte [rcx+d], imm */
    { u8(0x80); u8(0xB9); u32(uint32_t(d)); u8(imm); }
    void imul_r9_imm8(uint8_t k)       /* imul r9, r9, imm8 (row stride) */
    { u8(0x4D); u8(0x6B); u8(0xC9); u8(k); }
    void add_rcx_r9() { u8(0x4C); u8(0x01); u8(0xC9); }
    /* movsd [rcx + r9*8], xmm0  (#94: the float element STORE) */
    void store_elem_float_x0()
    { u8(0xF2); u8(0x4A); u8(0x0F); u8(0x11); u8(0x04); u8(0xC9); }
    /* ---- #95, the COMPOUND element store (read-modify-write) ----
     * The element rides RAX (the shobj is done with it once the hash byte
     * is invalidated - which happens FIRST, since past the guards nothing
     * can fault); the rhs stays in RDI. */
    void store_elem_int_rax()          /* mov [rcx + r9*8], rax */
    { u8(0x4A); u8(0x89); u8(0x04); u8(0xC9); }
    void store_elem_int_rdx()          /* mov [rcx + r9*8], rdx (mod) */
    { u8(0x4A); u8(0x89); u8(0x14); u8(0xC9); }
    void add_rax_rdi()  { u8(0x48); u8(0x01); u8(0xF8); }
    void sub_rax_rdi()  { u8(0x48); u8(0x29); u8(0xF8); }
    void imul_rax_rdi() { u8(0x48); u8(0x0F); u8(0xAF); u8(0xC7); }
    void cqo()          { u8(0x48); u8(0x99); }
    void idiv_rdi()     { u8(0x48); u8(0xF7); u8(0xFF); }
    void cmp_rdi_imm8(int8_t v)        /* cmp rdi, imm8 (sign-extended) */
    { u8(0x48); u8(0x83); u8(0xFF); u8(static_cast<uint8_t>(v)); }
    /* movsd xmm1, [rcx + r9*8] / movsd [rcx + r9*8], xmm1 */
    void load_elem_float_x1()
    { u8(0xF2); u8(0x4A); u8(0x0F); u8(0x10); u8(0x0C); u8(0xC9); }
    void store_elem_float_x1()
    { u8(0xF2); u8(0x4A); u8(0x0F); u8(0x11); u8(0x0C); u8(0xC9); }
    /* addsd/subsd/mulsd/divsd xmm1, xmm0 (op = 0x58/0x5C/0x59/0x5E) -
     * the compound direction: el = el OP rhs, el in xmm1, rhs in xmm0 */
    void farith_x1_x0(uint8_t op) { u8(0xF2); u8(0x0F); u8(op); u8(0xC8); }
    void pxor_x1() { u8(0x66); u8(0x0F); u8(0xEF); u8(0xC9); }
    /* ---- #95, the nested-STORE tier (boxed slot type guards in rdx) ---- */
    void cmp_rdx_rsi() { u8(0x48); u8(0x39); u8(0xF2); }
    void cmp_rdx_r8()  { u8(0x4C); u8(0x39); u8(0xC2); }
    void cmp_rdx_r9()  { u8(0x4C); u8(0x39); u8(0xCA); }
    void mov_rdi_rcx() { u8(0x48); u8(0x89); u8(0xCF); }  /* prep: &row */
    /* ---- C1, the hoisted-base navigation (r12-r15 operands) ---- */
    /* mov rH, [rax+disp]  (the vector's start/finish into a hoist reg) */
    void mov_hr_rax(uint8_t hr, int32_t d)
    { u8(0x4C); u8(0x8B); u8(static_cast<uint8_t>(0x80 | ((hr & 7) << 3)));
      u32(uint32_t(d)); }
    void mov_hr_rcx(uint8_t hr, int32_t d)       /* mov rH, [rcx+d] */
    { u8(0x4C); u8(0x8B);
      u8(static_cast<uint8_t>(0x81 | ((hr & 7) << 3))); u32(uint32_t(d)); }
    void sub_hr_hr(uint8_t dst, uint8_t src)     /* sub rHd, rHs */
    { u8(0x4D); u8(0x29);
      u8(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7))); }
    void sar_hr_3(uint8_t hr)                    /* sar rH, 3 */
    { u8(0x49); u8(0xC1); u8(static_cast<uint8_t>(0xF8 | (hr & 7)));
      u8(0x03); }
    void cmp_r9_hr(uint8_t hr)                   /* cmp r9, rH */
    { u8(0x4D); u8(0x39);
      u8(static_cast<uint8_t>(0xC1 | ((hr & 7) << 3))); }
    /* mov rax, [rH + r9*8] / movsd xmm0, [rH + r9*8]. SIB base = r13
     * (101b) with mod 00 means disp32-no-base - that case takes mod 01
     * with a zero disp8 instead. */
    void load_elem_int_hr(uint8_t hr)
    {
        u8(0x4B); u8(0x8B);
        if ((hr & 7) == 5) { u8(0x44); u8(0xCD); u8(0x00); }
        else { u8(0x04); u8(static_cast<uint8_t>(0xC8 | (hr & 7))); }
    }
    void load_elem_float_hr(uint8_t hr)
    {
        u8(0xF2); u8(0x4B); u8(0x0F); u8(0x10);
        if ((hr & 7) == 5) { u8(0x44); u8(0xCD); u8(0x00); }
        else { u8(0x04); u8(static_cast<uint8_t>(0xC8 | (hr & 7))); }
    }
    /* C1b: mov [rH + r9*8], rdi / movsd [rH + r9*8], xmm0 */
    void store_elem_int_hr(uint8_t hr)
    {
        u8(0x4B); u8(0x89);
        if ((hr & 7) == 5) { u8(0x7C); u8(0xCD); u8(0x00); }
        else { u8(0x3C); u8(static_cast<uint8_t>(0xC8 | (hr & 7))); }
    }
    void store_elem_float_hr(uint8_t hr)
    {
        u8(0xF2); u8(0x4B); u8(0x0F); u8(0x11);
        if ((hr & 7) == 5) { u8(0x44); u8(0xCD); u8(0x00); }
        else { u8(0x04); u8(static_cast<uint8_t>(0xC8 | (hr & 7))); }
    }
    /* C1c: movzx eax, byte [rH + r9]  (a bool element read - a clean
     * 0/1 in rax, matching the ordinary bool tail's int semantics) */
    void load_elem_byte_hr(uint8_t hr)
    {
        u8(0x43); u8(0x0F); u8(0xB6);
        if ((hr & 7) == 5) { u8(0x44); u8(0x0D); u8(0x00); }
        else { u8(0x04); u8(static_cast<uint8_t>(0x08 | (hr & 7))); }
    }
    /* C1c: mov [rH + r9], dil  (a bool element - 1 byte, scale 1) */
    void store_elem_byte_hr(uint8_t hr)
    {
        u8(0x43); u8(0x88);
        if ((hr & 7) == 5) { u8(0x7C); u8(0x0D); u8(0x00); }
        else { u8(0x3C); u8(static_cast<uint8_t>(0x08 | (hr & 7))); }
    }
    /* ---- #95, the SLICE read arms + the elem2 promote arm ---- */
    /* cvtsi2sd xmm0, qword [rcx + r9*8]  (an int element promotes) */
    void cvtsi2sd_x0_elem()
    { u8(0xF2); u8(0x4A); u8(0x0F); u8(0x2A); u8(0x04); u8(0xC9); }
    /* mov e<dx|ax>, dword [rbx+disp]  (a slice handle's u32 off/len,
     * zero-extended - the handle lives in the slot's payload) */
    void mov_edx_slot(int32_t d)
    { u8(0x8B); u8(MODRM_SLOT | (2 << 3)); u32(uint32_t(d)); }
    void mov_eax_slot(int32_t d)
    { u8(0x8B); u8(MODRM_SLOT); u32(uint32_t(d)); }
    /* mov e<dx|ax>, dword [rcx+disp]  (the elem2 ROW handle's off/len) */
    void mov_edx_rcx(int32_t d)
    { u8(0x8B); u8(0x91); u32(uint32_t(d)); }
    void mov_eax_rcx(int32_t d)
    { u8(0x8B); u8(0x81); u32(uint32_t(d)); }
    void add_r9_rax() { u8(0x49); u8(0x01); u8(0xC1); }
    void add_r9_rdx() { u8(0x49); u8(0x01); u8(0xD1); }
    /* cmp rax, rcx / test rax, rax */
    void cmp_rax_rcx() { u8(0x48); u8(0x39); u8(0xC8); }
    void test_rax_rax() { u8(0x48); u8(0x85); u8(0xC0); }
    /* mov [rbx+disp], rax  (a raw payload store - the type word is written
     * separately, as the two-store write_slot does). */
    void store_rax_slot(int32_t d)
    { u8(0x48); u8(0x89); u8(MODRM_SLOT); u32(uint32_t(d)); }
    /* mov [rbx+disp], rcx  (the Type* word of a slot) */
    void store_rcx_slot(int32_t d)
    { u8(0x48); u8(0x89); u8(MODRM_SLOT | (1 << 3)); u32(uint32_t(d)); }
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

enum Reg : uint8_t { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSI = 6, RDI = 7 };

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
    /* The fused nested read `a[i][j]` - a straight call to
     * jit_load_elem2_int/float, which BORROWS the row instead of boxing it
     * into a slot. No inline fast path (yet): the win is the deleted
     * LoadElemValue materialisation, not the call. */
    case OpCode::LoadElem2Int: case OpCode::LoadElem2Float:
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
    /* lever 4b: the fused ord(s[i]) via jit_ord_char (the proven-string byte
     * read; its only throw - OOB - conveys with the op's exc-stamped caret). */
    case OpCode::OrdCharV:
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
     * SetPend = a byte store into the try region's pend slot. None can
     * throw -> op_fully_native. (#78 step D: PushHandler carries only the
     * region id now, so it emits from emit_op like any other non-branch
     * op.) The raise-side ops need the dynamic-resume design and stay
     * interpreted. */
    case OpCode::PushHandler:
    case OpCode::PopHandler:
    case OpCode::SetPend:
        return true;
    /* Step 7a: EndFinally - the hot NORMAL path is a byte compare + fall
     * through. Eligibility matters beyond its own dispatch: EndFinally was
     * the run SPLITTER that left a try-loop's back edge landing on an
     * INTERIOR pc (interpreted body every iteration - the fragment-head
     * defect class); with it eligible the whole loop is one run and the
     * back edge is a fragment-local jump. #78 step E made its cold RERAISE
     * arm CONVEY (jit_end_finally) instead of bail, so it is
     * op_fully_native too. */
    case OpCode::EndFinally:
        return true;
    /* Step 7a: the COLD catch-region ops. CatchTest/Reraise are only ever
     * ENTERED via vm_raise's handler dispatch (the interpreter is already
     * driving there), and Throw always raises - so their native form is an
     * unconditional EXIT at the op (the ThrowRuntimeV pattern; the kept
     * originals run interpreted). Eligibility is the point: Throw was a
     * run SPLITTER that left a try/catch loop's back edge crossing
     * fragments (the interior-entry defect class); merged, the whole loop
     * is one run and the back edge is a fragment-local jump. (#78 step D
     * removed its two companions - CatchTest and Reraise are gone, the
     * raise path matches the handler table directly.) #80 gave `rethrow`
     * the same treatment, so it no longer splits a run either. */
    case OpCode::Throw:
    case OpCode::Rethrow:
        return true;
    /* model-flip (nativize-ops): an ALWAYS-THROWING construct - the helper
     * builds the POOLED exception natively (Runtime kinds via g_vm_jit_exc,
     * plain kinds - UndefinedVariableEx/CannotRebind* - via the M5
     * g_vm_jit_eptr channel, which postdates the op's old re-run-to-throw
     * exit form) with its pooled caret. Conveys, never re-executes ->
     * op_fully_native (deletable); never leaf-safe (it always exits). */
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
     * the first ran interpreted. is_true CAN throw -> the slow path conveys,
     * exc-stamped with the condition caret (deletable; the inline int/bool
     * fast path cannot fault and the op never bails). */
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
     * rides g_vm_jit_eptr). */
    case OpCode::CheckFuncV:
    case OpCode::MapFilterV:
        return true;
    /* The dyn-callee generic call pair - the LAST formerly-boxed sequential
     * ops. CheckCallableV conveys a loc-less NotCallableEx (exc-stamped
     * with the callee caret -> deletable); CallValueGenericV runs the full
     * by-Kind dispatch over the baked CallSite pool (a FuncObject callee
     * via the lean sync core) - it can BAIL (depth cap / chunkless callee /
     * undefined-global arg0 base), so NOT op_fully_native. */
    case OpCode::CheckCallableV:
    case OpCode::CallValueGenericV:
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
 * the slots base, the N5 cache registers (which hold a hot slot's LIVE
 * in-register value) and rsi/r8 (the type singletons). The first two now
 * live in CALLEE-SAVED registers - rbx for the base, r12-r15 for the cache
 * - so the callee preserves them and the prologue emits NOTHING; only
 * rsi/r8 are re-materialised, and they are constants. (rax/rcx/rdx are
 * per-op scratch - the store is the last thing in an op, so the next op
 * reloads them.)
 *
 * The registers are saved ONCE per fragment, at frag_entry, instead of
 * around each of its calls. SPILLING THEM AT ALL was once a correctness
 * bug: the int-store helper clobbered a cached accumulator/counter in an
 * int run (a float/libm run caches nothing, so it was masked), a
 * nested_fuzz find. Moving them callee-side keeps that guarantee by
 * construction rather than by remembering to bracket each call. */
static void emit_call_prologue(Emitter &e)
{
    /* C2a: the FLOAT pins are in xmm registers, which are ALL
     * caller-saved - spill each to its slot's PAYLOAD before the call
     * (the type word stays stale in memory, which is fine: a pinned
     * slot is never memory-read by any op in the run - the same bad()
     * rules as the GP pool - and an exceptional exit reloads in the
     * epilogue and then flushes type+payload properly). */
    for (const Emitter::CacheEnt &c : e.fcache)
        e.fstore(c.reg, c.payload);
    /* Otherwise NOTHING. Both things this used to do are gone: the slots base
     * is in callee-saved rbx and the N5 cache registers are callee-saved too,
     * so a helper call preserves all of them, and rsp is already call-ready
     * (frag_entry made the body rsp % 16 == 0). Kept as a named no-op
     * because it MARKS the call sites - a future pin in a caller-saved
     * register would spill here, and the epilogue below is still real.
     *
     * NOTE it does NOT materialise rdi either. A helper that takes the
     * slot window as its first argument used to get it for free, because
     * the base WAS rdi; such a site now says so explicitly with
     * slots_to_arg0(). Measured on this file: 64 of 73 call sites load rdi
     * with something else immediately after, so putting the move here
     * would be dead code at seven sites out of eight. */
    (void)e;
}

/* C1 (see the runs loop): the hoisted base's (data ptr, count) live in
 * CALLER-saved r10/r11 while a loop region emits, so any helper call
 * inside the region clobbers them; emit_call_epilogue below is the
 * single choke point every helper-call emission pairs through, and it
 * re-derives both. The base's PROPERTIES (type/non-slice/kind, its slot
 * binding) are region-stable - only the derived pointers need refresh. */
struct JitHoist {
    int base = -1;        /* the hoisted base's frame slot (-1 = none) */
    int kind = 0;         /* 0 ints, 1 floats, 2 general (elem2 outer) */
    uint8_t rdata = 0;    /* r10: the flat vector's data pointer */
    uint8_t rcount = 0;   /* r11: element count (ints/floats) / BYTE
                           * length (general - the elem2 outer compare) */
    bool active = false;  /* emitting the HOT region now */
    bool store_ok = false;/* C1b: the preheader ALSO proved the store
                           * guards (const/readonly/no live views) and
                           * invalidated the hash ONCE - plain stores
                           * may write raw off the pinned registers */
};
static JitHoist g_hoist;

static void emit_call_epilogue(Emitter &e)
{
    /* C2a: reload the caller-saved float pins the callee clobbered
     * (spilled to their slot payloads by emit_call_prologue). */
    for (const Emitter::CacheEnt &c : e.fcache)
        e.fload(c.reg, c.payload);
    /* The two type singletons - compile-time constants held in
     * CALLER-saved rsi/r8, so re-materialising is cheaper than a
     * register pair the allocator could otherwise use. */
    e.movabs(RSI, reinterpret_cast<uint64_t>(jit_layout().t_int));
    e.movabs_r8(reinterpret_cast<uint64_t>(jit_layout().t_float));
    if (g_hoist.active) {
        /* re-derive via RCX - RAX carries the helper's status, which
         * every call site tests right after this epilogue */
        const JitLayout &L = jit_layout();
        e.load(RCX, slot_addr(g_hoist.base).payload);   /* the shobj */
        e.mov_hr_rcx(g_hoist.rdata, L.data_off);
        e.mov_hr_rcx(g_hoist.rcount, L.data_off + 8);
        e.sub_hr_hr(g_hoist.rcount, g_hoist.rdata);
        if (g_hoist.kind == 0 || g_hoist.kind == 1)
            e.sar_hr_3(g_hoist.rcount);       /* 2/3 count BYTES */
    }
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
    /* #56: ... and the op's INLINED-AT chain, so a raise from a DELETED run
     * does not have to resolve one from its collapsed pc (see
     * Exception::jit_inline_frame). -1 = this op is not inlined code. */
    const int32_t chain = ck.inline_frame_at(old_pc);

    /*
     * #88: the absence of a chain is stamped too (-2, below) - but ONLY
     * where it can matter. The marker exists to stop `vm_flush_inline_walk`
     * guessing via `inline_frame_at(pc)`; in a chunk with NO inlined ops
     * that lookup returns -1 anyway, so there is nothing to prevent and the
     * whole block is dead weight on a cold path every conveying op carries.
     * Skipping it there measured back the +1.9% this cost 69_exc_crossframe.
     */
    const bool need_no_chain_marker = !ck.inline_ctxs.empty();
    if (!le && chain < 0 && !need_no_chain_marker)
        return;                    /* nothing to stamp - stays loc-less */

    const auto pack = [](const Loc &l) {
        return static_cast<uint64_t>(static_cast<uint32_t>(l.line))
             | (static_cast<uint64_t>(static_cast<uint32_t>(l.col)) << 32);
    };
    const uint32_t off_s = static_cast<uint32_t>(jit_off_exc_loc_start());
    const uint32_t off_e = static_cast<uint32_t>(jit_off_exc_loc_end());
    const uint32_t off_if =
        static_cast<uint32_t>(jit_off_exc_inline_frame());

    e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_exc()));
    e.u8(0x48); e.u8(0x8B); e.u8(0x00);      /* mov rax, [rax] (the object) */
    e.u8(0x48); e.u8(0x85); e.u8(0xC0);      /* test rax, rax */
    const size_t j_null = e.j8(0x74);        /* jz end: bail/eptr - no exc */

    size_t j_has = 0;
    if (le) {
        e.u8(0x83); e.u8(0xB8);              /* cmp dword [rax+off], 0 */
        e.u32(off_s + 4);                    /*   ... loc_start.col */
        e.u8(0x00);
        j_has = e.j8(0x75);                  /* jnz: caret already set */
        e.movabs(RCX, pack(le->start));
        e.u8(0x48); e.u8(0x89); e.u8(0x88);  /* mov [rax+off_s], rcx */
        e.u32(off_s);
        e.movabs(RCX, pack(le->end));
        e.u8(0x48); e.u8(0x89); e.u8(0x88);  /* mov [rax+off_e], rcx */
        e.u32(off_e);
        e.patch8(j_has, e.pos());            /* the CARET block only: the
                                              * chain stamp below still runs
                                              * for an exception that already
                                              * carries a caret but whose
                                              * frames are not yet emitted */
    }

    /*
     * #88: ALWAYS stamp, using -2 for "this op is NOT inlined code".
     *
     * -1 used to mean both "no fragment baked anything" and "a fragment
     * baked: there is no chain", and the flush could not tell them apart -
     * so on the second reading it fell back to a pc lookup, which on a
     * DELETED run resolves against collapsed pcs and invents a chain that
     * belongs to some other op. That is a phantom virtual frame in the
     * backtrace (visible at recursion depth 3), the mirror of the missing
     * ones. Recording the absence removes the guess.
     */
    if (chain >= 0 || need_no_chain_marker) {
        size_t j_set;
        if (chain >= 0) {
            /* A REAL chain outranks the -2 marker: stamp whenever the field
             * is still negative (unset -1 or "no chain" -2). Unchanged for
             * -1, which is what keeps first-real-conveyor-wins intact. */
            e.u8(0x83); e.u8(0xB8);          /* cmp dword [rax+off_if], 0 */
            e.u32(off_if);
            e.u8(0x00);
            j_set = e.j8(0x7D);              /* jge end: a chain is set */
        } else {
            /* The marker only fills a VACANCY - it must never overwrite a
             * chain, and must not block one either: an exception can be
             * conveyed by an op that is not the one that raised (a C++
             * throw crossing a fragment exit), and that conveyor's "I am
             * not inlined" says nothing about the raise site. */
            e.u8(0x83); e.u8(0xB8);          /* cmp dword [rax+off_if], -1 */
            e.u32(off_if);
            e.u8(0xFF);
            j_set = e.j8(0x75);              /* jne end: anything is set */
        }
        e.u8(0xC7); e.u8(0x80);              /* mov dword [rax+off_if], imm */
        e.u32(off_if);
        e.u32(static_cast<uint32_t>(chain >= 0 ? chain : -2));
        e.patch8(j_set, e.pos());
    }

    e.patch8(j_null, e.pos());
}


/*
 * #88: hand this call site's INLINED-AT chain to the sync helpers through
 * the side-channel globals (vm.cpp), which they claim on entry. Baked from
 * the PRE-COLLAPSE old_pc for the same reason the caret is: once the run's
 * interpreted originals are deleted every pc lands on the head EnterNative,
 * where a lookup cannot tell two inlined bodies apart. The POOL BASE, never
 * a Chunk * - the chunk is moved out of codegen_chunk and its address
 * dangles, while the vector's heap buffer survives (as &ck.locs[i] does).
 *
 * Emitted ONLY when this site HAS a chain. Nothing needs to be written to
 * say "no chain": every helper CLAIMS the pair at entry, resetting it, and
 * a store always sits immediately before its own call with nothing running
 * in between - so a site without a chain can only ever observe the cleared
 * value. That keeps two stores off every call in a chunk with no inlining,
 * which is nearly all of them.
 *
 * Clobbers rax/rcx - emit only where both are dead.
 */
static void emit_bake_call_site(Emitter &e, const Chunk &ck, size_t old_pc)
{
    const int32_t chain = ck.inline_frame_at(old_pc);
    if (chain < 0)
        return;
    const uint64_t pool =
        ck.inline_frames.empty()
            ? 0
            : reinterpret_cast<uint64_t>(ck.inline_frames.data());
    e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_call_inline_chain()));
    e.u8(0xC7); e.u8(0x00);                  /* mov dword [rax], imm32 */
    e.u32(static_cast<uint32_t>(chain));
    e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_call_inline_pool()));
    e.movabs(RCX, pool);
    e.u8(0x48); e.u8(0x89); e.u8(0x08);      /* mov [rax], rcx */
}

/*
 * Lever 1 step 5 - the fragment-INLINE sync call. The hot shape emits:
 * a depth guard, the flat noexcept push (jit_sync_push_slot/_value ->
 * {window, entry} in rax:rdx), a DIRECT `call rdx` into the callee
 * fragment (no jit_enter layer - machine code calling machine code, so
 * the UBSan CFI concern does not apply), an inline sentinel compare, and
 * only then the cold tails: jit_sync_postexit on a non-sentinel exit, or
 * the FULL jit_call_sync* helper when any guard/push declines (depth cap,
 * undefined/non-func callee, non-fast_bind, dispatch-path body, arity/
 * overflow - the helper re-runs everything and its interpreted-op
 * fallback produces the byte-identical throw). The depth counter is
 * bumped inline ONLY around the direct callee call; the slow helper owns
 * its own bookkeeping. Everything sits in ONE prologue/epilogue bracket
 * (three epilogue copies - one per exit - because exit_pc's flush_cache
 * must see the RELOADED cache regs, the bracket discipline the plain
 * helper emit established).
 */
/*
 * M5b - THE FULLY-INLINE RECORD PUSH (plans/native-gap-roadmap.md lever 1
 * endgame). Emits the sync call's push - callee resolve, the gate checks,
 * push_window's hot shape (segment fit + record REUSE), the record fill,
 * the unrolled fast_bind arg copies, the captures switch - as machine
 * code at the call site, leaving rdi = the callee window and rdx = the
 * callee fragment entry. EVERY guard precedes EVERY mutation, so any
 * decline jumps to the slow tier (the full jit_call_sync* helper), which
 * re-runs everything idempotently: the cold shapes (undefined/non-func
 * callee, non-fast_bind, dispatch-path body, arity mismatch, overflow,
 * segment advance, record high-water growth, iter-bearing chunks, a
 * pending pure-cache stash, a REFERENCE argument - the inline copy is
 * trivial-payload-only) all land there. Offsets come from the
 * JitPushLayout probe (vm.cpp - real members, cannot drift silently).
 *
 * Register plan (the bracket frees everything but rdi = caller slots):
 * r9 = ctx, r8 = act, rdx = fo, rax = desc, rcx = cck, rsi = total,
 * r10/r11 scratch; fo/desc spill to the stack across the fill.
 */
static const JitPushLayout &jit_push_layout()
{
    static JitPushLayout L = [] {
        JitPushLayout l{};
        jit_fill_push_layout(&l);
        return l;
    }();
    return L;
}

static void emit_sync_push_native(Emitter &e, const Instr &in, bool is_value,
                                  bool cached, int callee_arg,
                                  std::vector<size_t> &j_slow,
                                  std::vector<size_t> &j_done)
{
    const JitPushLayout &P = jit_push_layout();
    const JitLayout &L = jit_layout();
    const int NARGS = static_cast<int>(in.b_lit());
    const int ARGBASE = static_cast<int>(in.a_lit());

    /* [base + disp32] micro-encoders (base/reg 0-15, never rsp/r12) */
    const auto modrm = [&](uint8_t op, uint8_t reg, uint8_t base,
                           int32_t d, bool w) {
        uint8_t rex = static_cast<uint8_t>(
            (w ? 0x48 : 0x40) | (reg >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
        if (rex != 0x40)
            e.u8(rex);
        e.u8(op);
        e.u8(static_cast<uint8_t>(0x80 | ((reg & 7) << 3) | (base & 7)));
        e.u32(static_cast<uint32_t>(d));
    };
    const auto ld = [&](uint8_t r, uint8_t b, int32_t d) {   /* mov r,[b+d] */
        modrm(0x8B, r, b, d, true);
    };
    const auto st = [&](uint8_t b, int32_t d, uint8_t r) {   /* mov [b+d],r */
        modrm(0x89, r, b, d, true);
    };
    const auto st32 = [&](uint8_t b, int32_t d, uint8_t r) { /* dword */
        modrm(0x89, r, b, d, false);
    };
    const auto ld32sx = [&](uint8_t r, uint8_t b, int32_t d) {
        modrm(0x63, r, b, d, true);          /* movsxd r, dword [b+d] */
    };
    const auto ld32 = [&](uint8_t r, uint8_t b, int32_t d) {
        modrm(0x8B, r, b, d, false);         /* mov r32, [b+d] */
    };
    const auto cmp_b_imm8 = [&](uint8_t b, int32_t d, uint8_t imm) {
        if (b >= 8) e.u8(0x41);
        e.u8(0x80);                           /* cmp byte [b+d], imm8 */
        e.u8(static_cast<uint8_t>(0xB8 | (b & 7)));
        e.u32(static_cast<uint32_t>(d));
        e.u8(imm);
    };
    const auto cmp_q_imm8 = [&](uint8_t b, int32_t d, int8_t imm) {
        e.u8(static_cast<uint8_t>(0x48 | (b >= 8 ? 1 : 0)));
        e.u8(0x83);                           /* cmp qword [b+d], imm8 */
        e.u8(static_cast<uint8_t>(0xB8 | (b & 7)));
        e.u32(static_cast<uint32_t>(d));
        e.u8(static_cast<uint8_t>(imm));
    };
    const auto cmp_d_imm8 = [&](uint8_t b, int32_t d, int8_t imm) {
        if (b >= 8) e.u8(0x41);
        e.u8(0x83);                           /* cmp dword [b+d], imm8 */
        e.u8(static_cast<uint8_t>(0xB8 | (b & 7)));
        e.u32(static_cast<uint32_t>(d));
        e.u8(static_cast<uint8_t>(imm));
    };
    const auto st_q_imm32 = [&](uint8_t b, int32_t d, int32_t imm) {
        e.u8(static_cast<uint8_t>(0x48 | (b >= 8 ? 1 : 0)));
        e.u8(0xC7);                           /* mov qword [b+d], imm32 */
        e.u8(static_cast<uint8_t>(0x80 | (b & 7)));
        e.u32(static_cast<uint32_t>(d));
        e.u32(static_cast<uint32_t>(imm));
    };
    const auto st_b_imm8 = [&](uint8_t b, int32_t d, uint8_t imm) {
        if (b >= 8) e.u8(0x41);
        e.u8(0xC6);                           /* mov byte [b+d], imm8 */
        e.u8(static_cast<uint8_t>(0x80 | (b & 7)));
        e.u32(static_cast<uint32_t>(d));
        e.u8(imm);
    };
    const auto add_m_r = [&](uint8_t b, int32_t d, uint8_t r) {
        modrm(0x01, r, b, d, true);           /* add [b+d], r */
    };
    const auto inc_q = [&](uint8_t b, int32_t d) {
        e.u8(static_cast<uint8_t>(0x48 | (b >= 8 ? 1 : 0)));
        e.u8(0xFF);                           /* inc qword [b+d] */
        e.u8(static_cast<uint8_t>(0x80 | (b & 7)));
        e.u32(static_cast<uint32_t>(d));
    };
    const auto imul_imm = [&](uint8_t r, int32_t imm) { /* imul r, r, imm */
        e.u8(static_cast<uint8_t>(0x48 | (r >= 8 ? 5 : 0)));
        e.u8(0x69);
        e.u8(static_cast<uint8_t>(0xC0 | ((r & 7) << 3) | (r & 7)));
        e.u32(static_cast<uint32_t>(imm));
    };
    const uint8_t R10 = 10, R11 = 11, R8R = 8, R9R = 9;

    /* ---------------- GUARDS (no mutation before these pass) ----------- */
    /* NOTE there is no arg-triviality GATE here any more. A reference
     * argument used to decline the whole inline push to the C++ slow tier -
     * which meant EVERY call passing an array/string/dict/struct, i.e. most
     * real code. It is now bound per-argument at the copy loop below, so the
     * decision moved to where the copy happens and stopped being a decline. */
    e.movabs(RCX, reinterpret_cast<uint64_t>(L.addr_ctx));
    ld(R9R, RCX, 0);                                  /* r9 = ctx */
    e.movabs(RCX, reinterpret_cast<uint64_t>(L.addr_act));
    ld(R8R, RCX, 0);                                  /* r8 = act */
    if (!is_value) {
        ld(RAX, R9R, static_cast<int32_t>(L.ctx_gfuncs));
        ld(RCX, RAX, static_cast<int32_t>(L.gft_defined));
        cmp_b_imm8(RCX, callee_arg, 0);
        j_slow.push_back(e.j32(0x74));                /* je slow */
        ld(RCX, RAX, static_cast<int32_t>(L.gft_slots));
        e.movabs(RAX, reinterpret_cast<uint64_t>(P.t_func));
        modrm(0x39, RAX, RCX, callee_arg * 48 + 24, true); /* cmp [..],rax */
        j_slow.push_back(e.j32(0x75));                /* jne slow */
        ld(RDX, RCX, callee_arg * 48);                /* rdx = fo */
    } else {
        e.movabs(RAX, reinterpret_cast<uint64_t>(P.t_func));
        modrm(0x39, RAX, RBX, callee_arg * 48 + 24, true);
        j_slow.push_back(e.j32(0x75));                /* jne slow */
        ld(RDX, RBX, callee_arg * 48);                /* rdx = fo */
    }
    ld(RAX, RDX, static_cast<int32_t>(P.fo_func));    /* rax = desc */
    if (cached) {
        /* M5c: probe the caller's per-frame cache (a map lookup - C++).
         * HIT -> dst written, jump to the site's done label; MISS -> the
         * key is PARKED in g_jit_pending_key for the record store below
         * (any decline in between jumps to the slow tier, which consumes
         * it). fo is spilled around the call (the probe clobbers the
         * resolve registers); desc re-derives from it. */
        e.push_reg(RDX);                  /* fo */
        e.u8(0x48); e.u8(0x83); e.u8(0xEC); e.u8(0x08);  /* sub rsp,8 (pad:
                                           * an even count stays call-ready) */
        e.mov_rr(RDI, RAX);               /* rdi = desc */
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit()));
        e.movabs(RCX,
                 static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_cached_probe) });
        e.u8(0xE8); e.u32(0);
        e.u8(0x48); e.u8(0x83); e.u8(0xC4); e.u8(0x08);  /* add rsp,8 (pad) */
        e.pop_reg(RDX);                   /* fo back */
        e.u8(0x85); e.u8(0xC0);           /* test eax, eax */
        j_done.push_back(e.j32(0x75));    /* jnz done (hit) */
        /* the C++ call clobbered EVERY caller-saved register - rebuild
         * r9 = ctx and r8 = act too, not just fo/desc (the miss path's
         * remaining guards and the whole mutation phase read them; the
         * un-rebuilt r8 was a release-only SEGV - dbg helpers happened
         * to preserve it) */
        e.movabs(RCX, reinterpret_cast<uint64_t>(L.addr_ctx));
        ld(R9R, RCX, 0);
        e.movabs(RCX, reinterpret_cast<uint64_t>(L.addr_act));
        ld(R8R, RCX, 0);
        ld(RAX, RDX, static_cast<int32_t>(P.fo_func));  /* rax = desc */
    }
    cmp_b_imm8(RAX, static_cast<int32_t>(P.desc_fast_bind), 0);
    j_slow.push_back(e.j32(0x74));                    /* je slow */
    /* nparams == NARGS via the vector's byte length */
    ld(RCX, RAX, static_cast<int32_t>(P.desc_params) + 8);
    modrm(0x2B, RCX, RAX, static_cast<int32_t>(P.desc_params), true);
                                                      /* sub rcx,[params] */
    e.u8(0x48); e.u8(0x81); e.u8(0xF9);               /* cmp rcx, imm32 */
    e.u32(static_cast<uint32_t>(NARGS
              * static_cast<int>(P.param_desc_size)));
    j_slow.push_back(e.j32(0x75));                    /* jne slow */
    ld(RCX, RAX, static_cast<int32_t>(L.desc_vm_chunk)); /* rcx = cck */
    e.u8(0x48); e.u8(0x85); e.u8(0xC9);               /* test rcx, rcx */
    j_slow.push_back(e.j32(0x74));                    /* jz slow */
    cmp_q_imm8(RCX, static_cast<int32_t>(P.ck_sync_entry), 0);
    j_slow.push_back(e.j32(0x7C));                    /* jl slow */
    cmp_d_imm8(RCX, static_cast<int32_t>(P.ck_n_dict_iters), 0);
    j_slow.push_back(e.j32(0x75));                    /* jne slow */
    cmp_d_imm8(RCX, static_cast<int32_t>(P.ck_n_dyn_iters), 0);
    j_slow.push_back(e.j32(0x75));                    /* jne slow */
    /* #78: a callee with TRY regions needs rec.pend_base + a pends slice -
     * push_window's job; the inline push declines (like the iter pools). */
    cmp_d_imm8(RCX, static_cast<int32_t>(P.ck_n_trys), 0);
    j_slow.push_back(e.j32(0x75));                    /* jne slow */
    /* rsi = total = frame_size + n_temps */
    if (!cached) {
        /* a PLAIN call from a cache-carrying caller declines (the stash
         * costs ~2 Ir on every push and only cached-call chains carry
         * caches as a rule - measured -0.3..-0.5% on 10/11/63 when
         * unconditional); a CACHED site stashes inline below instead
         * (its caller holds a live cache by definition). */
        cmp_q_imm8(R8R,
                   static_cast<int32_t>(P.act_vframe + P.frame_pure_cache),
                   0);
        j_slow.push_back(e.j32(0x75));                /* jne slow */
    }
    ld32sx(RSI, RAX, static_cast<int32_t>(P.desc_frame_size));
    ld32sx(R10, RCX, static_cast<int32_t>(P.ck_n_temps));
    e.u8(0x4C); e.u8(0x01); e.u8(0xD6);               /* add rsi, r10 */
    /* stack-cap overflow: used + total > cap -> slow */
    ld(R10, R8R, static_cast<int32_t>(P.act_used));
    e.u8(0x49); e.u8(0x01); e.u8(0xF2);               /* add r10, rsi */
    modrm(0x3B, R10, R8R, static_cast<int32_t>(P.act_cap), true);
                                                      /* cmp r10,[act+cap] */
    j_slow.push_back(e.j32(0x7F));                    /* jg slow */
    /* record REUSE available: rec_n != recs_high (else the cold grow) */
    ld(R10, R8R, static_cast<int32_t>(L.act_rec_n));
    ld32(R11, R8R, static_cast<int32_t>(P.act_recs_high));
    e.u8(0x4D); e.u8(0x39); e.u8(0xDA);               /* cmp r10, r11 */
    j_slow.push_back(e.j32(0x74));                    /* je slow */
    /* segment + fit: (top + total) * 48 <= slots byte length */
    ld32sx(R10, R8R, static_cast<int32_t>(P.act_cur_seg));
    e.u8(0x4D); e.u8(0x85); e.u8(0xD2);               /* test r10, r10 */
    j_slow.push_back(e.j32(0x78));                    /* js slow */
    ld(R11, R8R, static_cast<int32_t>(P.act_segs));   /* segs._M_start */
    /* mov r10, [r11 + r10*8] */
    e.u8(0x4F); e.u8(0x8B); e.u8(0x14); e.u8(0xD3);   /* r10 = seg* */
    ld(R11, R10, static_cast<int32_t>(P.seg_top));    /* r11 = top */
    e.push_reg(RAX);
    /* lea rax, [r11 + rsi]; imul rax, rax, 48 */
    e.u8(0x49); e.u8(0x8D); e.u8(0x04); e.u8(0x33);   /* lea rax,[r11+rsi] */
    imul_imm(RAX, 48);
    e.push_reg(RCX);
    ld(RCX, R10, static_cast<int32_t>(P.seg_slots) + 8);
    modrm(0x2B, RCX, R10, static_cast<int32_t>(P.seg_slots), true);
                                                      /* sub rcx, [start] */
    e.u8(0x48); e.u8(0x39); e.u8(0xC8);               /* cmp rax, rcx */
    e.pop_reg(RCX);
    e.pop_reg(RAX);
    j_slow.push_back(e.j32(0x7F));                    /* jg slow */

    /* -------------- MUTATIONS (control reaches the call) --------------- */
    e.push_reg(RDX);                                  /* fo   [rsp+8] */
    e.push_reg(RAX);                                  /* desc [rsp]   */
    /* window rdx = seg->slots.data() + top*48 (r11 = top KEPT - it is
     * seg_top_before, stored into the record below) */
    ld(RDX, R10, static_cast<int32_t>(P.seg_slots));
    e.u8(0x4C); e.u8(0x89); e.u8(0xD8);               /* mov rax, r11 */
    imul_imm(RAX, 48);
    e.u8(0x48); e.u8(0x01); e.u8(0xC2);               /* add rdx, rax */
    /* seg->top = top + total; used += total (top itself stays in r11) */
    e.u8(0x49); e.u8(0x8D); e.u8(0x04); e.u8(0x33);   /* lea rax,[r11+rsi] */
    st(R10, static_cast<int32_t>(P.seg_top), RAX);
    add_m_r(R8R, static_cast<int32_t>(P.act_used), RSI);
    /* rec r10 = records_start + rec_n * RECSZ; rec_n++; top_rec = rec */
    ld(R10, R8R, static_cast<int32_t>(L.act_records));
    ld(RAX, R8R, static_cast<int32_t>(L.act_rec_n));
    imul_imm(RAX, static_cast<int32_t>(L.rec_size));
    e.u8(0x49); e.u8(0x01); e.u8(0xC2);               /* add r10, rax */
    inc_q(R8R, static_cast<int32_t>(L.act_rec_n));
    st(R8R, static_cast<int32_t>(P.act_top_rec), R10);
    /* the record fill */
    st(R10, static_cast<int32_t>(P.rec_window), RDX);
    st(R10, static_cast<int32_t>(P.rec_nslots), RSI);
    st(R10, static_cast<int32_t>(P.rec_seg_top_before), R11);
    ld32(RAX, R8R, static_cast<int32_t>(P.act_cur_seg));
    st32(R10, static_cast<int32_t>(P.rec_seg), RAX);
    st(R10, static_cast<int32_t>(P.rec_run_chunk), RCX);
    e.movabs(RAX, reinterpret_cast<uint64_t>(P.stop_chunk));
    st(R10, static_cast<int32_t>(P.rec_ret_chunk), RAX);
    st_q_imm32(R10, static_cast<int32_t>(P.rec_ret_pc), 1);
    st_q_imm32(R10, static_cast<int32_t>(P.rec_dst),
               static_cast<int32_t>(in.target));
    e.u8(0x48); e.u8(0x8B); e.u8(0x04); e.u8(0x24);  /* mov rax, [rsp]
                                                      * = the desc spill */
    st(R10, static_cast<int32_t>(P.rec_desc), RAX);
    ld(RAX, R9R, static_cast<int32_t>(L.ctx_captures));
    st(R10, static_cast<int32_t>(P.rec_caller_caps), RAX);
    ld(RAX, R8R, static_cast<int32_t>(P.act_handlers2) + 8);
    modrm(0x2B, RAX, R8R, static_cast<int32_t>(P.act_handlers2), true);
    e.u8(0x48); e.u8(0xC1); e.u8(0xE8); e.u8(0x02);   /* shr rax, 2
                                                       * (4-byte VmHandler) */
    st32(R10, static_cast<int32_t>(P.rec_handler_base), RAX);
    ld32(RAX, R8R, static_cast<int32_t>(P.act_diters_n));
    st32(R10, static_cast<int32_t>(P.rec_diter_base), RAX);
    ld32(RAX, R8R, static_cast<int32_t>(P.act_dyiters_n));
    st32(R10, static_cast<int32_t>(P.rec_dyiter_base), RAX);
    st_b_imm8(R10, static_cast<int32_t>(P.rec_boundary), 0);
    st_b_imm8(R10, static_cast<int32_t>(P.rec_sync_stop), 1);
    if (cached) {
        /* the caller's pure-cache STASH (per-frame scoping):
         * rec.caller_cache = move(view_frame.pure_cache) - a raw pointer
         * move (rec's field is null on a reused record: pop moved it
         * out). Cached sites only: a caching caller holds a live cache
         * by definition, and M5b's decline guard would kill their fast
         * path entirely; plain sites keep the guard (above). */
        ld(RAX, R8R,
           static_cast<int32_t>(P.act_vframe + P.frame_pure_cache));
        st(R10, static_cast<int32_t>(P.rec_caller_cache), RAX);
        st_q_imm32(
            R8R, static_cast<int32_t>(P.act_vframe + P.frame_pure_cache),
            0);
    }
    if (cached) {
        /* rec.cache_key = the probe's parked key (ownership transfer) */
        e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_pending_key()));
        ld(R11, RAX, 0);
        st(R10, static_cast<int32_t>(P.rec_cache_key), R11);
        st_q_imm32(RAX, 0, 0);
    }
    /* the view frame */
    st(R8R, static_cast<int32_t>(P.act_vframe + P.frame_slots), RDX);
    st32(R8R, static_cast<int32_t>(P.act_vframe + P.frame_size), RSI);
    /* fast_bind arg copies (unrolled; trivial payloads - guarded above):
     * 24B payload + 8B type copied, container/idx/flags zeroed */
    for (int i = 0; i < NARGS; i++) {
        const int32_t s = (ARGBASE + i) * 48, d = i * 48;
        ld(RAX, RBX, s + 24);                         /* rax = type* */
        cmp_d_imm8(RAX, static_cast<int32_t>(L.type_t_off),
                   static_cast<int8_t>(L.t_str_val));
        const size_t j_ref = e.j32(0x7D);             /* jge -> reference */
        for (int32_t o = 0; o <= 24; o += 8) {        /* scalar: raw copy */
            ld(R11, RBX, s + o);
            st(RDX, d + o, R11);
        }
        const size_t j_done = e.j32(0xEB);
        e.patch32_here(j_ref);
        /*
         * A REFERENCE: defer to the real C++ copy (jit_bind_ref_arg). It
         * cannot be inlined as a refcount bump - a SLICE registers itself in
         * its parent's `slices` set on copy - so the helper runs fast_bind's
         * exact per-argument step. FOUR pushes: rcx/rdx/r9 are read after the
         * bind and the 4th is a PAD, because an even count preserves the
         * 16-byte alignment this site already has (emit_call_prologue made it
         * call-ready and the two live pushes here - fo, desc - keep it so).
         * r8 is not read after; rsi/rax are dead (emit_call_epilogue reloads
         * the type singletons anyway); the base is in rbx, which the callee
         * preserves - it used to be rdi, which is why this pushed four LIVE
         * registers rather than three and a pad.
         */
        e.push_reg(RCX); e.push_reg(R9R);
        e.push_reg(RDX);
        e.u8(0x48); e.u8(0x83); e.u8(0xEC); e.u8(0x08);   /* sub rsp,8 (pad) */
        modrm(0x8D, RSI, RBX, s, true);               /* rsi = &src (arg 2) */
        modrm(0x8D, RDI, RDX, d, true);               /* rdi = &dst (arg 1) */
        e.movabs(RAX, reinterpret_cast<uint64_t>(&jit_bind_ref_arg));
        e.call_rax();
        e.u8(0x48); e.u8(0x83); e.u8(0xC4); e.u8(0x08);   /* add rsp,8 (pad) */
        e.pop_reg(RDX);
        e.pop_reg(R9R); e.pop_reg(RCX);
        e.patch32_here(j_done);
    }
    e.u8(0x45); e.u8(0x31); e.u8(0xDB);               /* xor r11d, r11d */
    for (int i = 0; i < NARGS; i++) {
        st(RDX, i * 48 + 32, R11);
        st(RDX, i * 48 + 40, R11);
    }
    /* ctx.captures = &fo.capture_slots; restore the spills */
    e.pop_reg(RAX);                                   /* desc (done) */
    e.pop_reg(R11);                                   /* fo */
    /* lea rax, [r11 + fo_caps] */
    e.u8(0x49); e.u8(0x8D); e.u8(0x83);
    e.u32(static_cast<uint32_t>(P.fo_capture_slots));
    st(R9R, static_cast<int32_t>(L.ctx_captures), RAX);
    /* rdi = the callee window; rdx = the fragment entry */
    e.mov_rr(RDI, RDX);
    ld(RDX, RCX, static_cast<int32_t>(L.chunk_native_base));
    modrm(0x03, RDX, RCX, static_cast<int32_t>(P.ck_sync_entry), true);
                                                      /* add rdx,[entry] */
}

/* M5a site-switch halves (see the use in emit_sync_call_inline). PRE:
 * rax/rdx hold the push's {win, entry} and rdi the callee window - only
 * rcx/r9 are free. cur == null -> jump to the plain path (returned fixup);
 * else mark active, save rsp, switch to the baked top. POST: restore rsp
 * + re-arm cur (runs on the outermost path only). */
static size_t emit_nstack_switch_pre(Emitter &e)
{
    e.movabs(RCX, reinterpret_cast<uint64_t>(&g_nstack_cur));
    e.u8(0x48); e.u8(0x83); e.u8(0x39); e.u8(0x00);   /* cmp qword [rcx],0 */
    const size_t j_plain = e.j32(0x74);               /* je plain */
    /* mov qword [rcx], 0  (cur = null: active) */
    e.u8(0x48); e.u8(0xC7); e.u8(0x01); e.u32(0);
    e.movabs(RCX, reinterpret_cast<uint64_t>(&g_nstack_saved_rsp));
    e.u8(0x48); e.u8(0x89); e.u8(0x21);               /* mov [rcx], rsp */
    e.movabs(RCX, reinterpret_cast<uint64_t>(g_nstack_top));
    e.u8(0x48); e.u8(0x89); e.u8(0xCC);               /* mov rsp, rcx */
    return j_plain;
}

static void emit_nstack_switch_post(Emitter &e)
{
    e.movabs(RCX, reinterpret_cast<uint64_t>(&g_nstack_saved_rsp));
    e.u8(0x48); e.u8(0x8B); e.u8(0x21);               /* mov rsp, [rcx] */
    e.movabs(RCX, reinterpret_cast<uint64_t>(&g_nstack_cur));
    /* cur = top (re-arm): movabs r9, top; mov [rcx], r9 */
    e.movabs_r9(reinterpret_cast<uint64_t>(g_nstack_top));
    e.u8(0x4C); e.u8(0x89); e.u8(0x09);               /* mov [rcx], r9 */
}

/* #56 step 3: the CURRENT chunk's entry_remap, visible to
 * emit_sync_call_inline so it can bake the call's POST-CALL entry-stub pc
 * (the SWITCH record's resume). Set around the fragment-emission loop in
 * jit_compile_chunk; the compiler is single-threaded and non-reentrant
 * (the disasm replay runs sequentially), so a file-static is safe. */
static const std::vector<int> *g_cur_entry_remap = nullptr;

static void emit_sync_call_inline(Emitter &e, const Chunk &ck,
                                  const Instr &in, uint32_t pc,
                                  size_t old_pc, bool is_value,
                                  const void *slow_helper,
                                  int_type callee_arg)
{
    Loc ls, le;
    ck.loc_at(old_pc, ls, le);
    const uint64_t site =
        (static_cast<uint64_t>(static_cast<uint32_t>(ls.line)) << 32)
        | static_cast<uint32_t>(ls.col);
    const uint64_t depth_addr =
        reinterpret_cast<uint64_t>(jit_addr_sync_depth());

    emit_call_prologue(e);
    /* depth guard: cmp dword [&g_jit_sync_depth], CAP; jge slow */
    e.movabs(RAX, depth_addr);
    e.u8(0x81); e.u8(0x38);
    e.u32(static_cast<uint32_t>(jit_sync_depth_cap()));
    std::vector<size_t> j_slows, j_dones;
    j_slows.push_back(e.j32(0x7D));                /* jge slow */
    /* M5b/M5c: THE FULLY-INLINE PUSH - resolve/gates/(cached: probe)/
     * push_window's hot shape/record fill/unrolled bind/captures, ending
     * with rdi = the callee window and rdx = the fragment entry; every
     * decline jumped to slow BEFORE any mutation; a cache HIT jumped to
     * done (dst already written by the probe). */
    emit_sync_push_native(e, in, is_value,
                          in.op == OpCode::CachedCallV,
                          static_cast<int>(callee_arg), j_slows, j_dones);
    /* depth++ (the callee is committed) */
    e.movabs(RCX, depth_addr);
    e.u8(0xFF); e.u8(0x01);                        /* inc dword [rcx] */
#ifdef TESTS
    e.movabs(RCX, reinterpret_cast<uint64_t>(&g_jit_sync_inline));
    e.u8(0x48); e.u8(0xFF); e.u8(0x01);            /* inc qword [rcx] */
    e.movabs(RCX, reinterpret_cast<uint64_t>(
                      &g_jit_op_run[static_cast<size_t>(in.op)]));
    e.u8(0x48); e.u8(0xFF); e.u8(0x01);            /* inc qword [rcx] */
#endif
    /* THE OUTERMOST-ONLY STACK SWITCH (M5a): nesting happens HERE (the
     * direct callee call), so the native-stack switch lives here - not in
     * jit_enter, whose per-fragment-entry conditional measured ~1% on the
     * callback benches. cur == null (not armed, or already active) ->
     * plain call; else switch rsp to the baked top around the call
     * (nested sites see null and stay plain; the restore runs BEFORE the
     * sentinel branch, so the exception path unwinds it too). */
    {
        const size_t j_plain = emit_nstack_switch_pre(e);
        e.u8(0xFF); e.u8(0xD2);                    /* call rdx (switched) */
        emit_nstack_switch_post(e);
        const size_t j_over = e.j32(0xEB);
        e.patch32_here(j_plain);
        e.u8(0xFF); e.u8(0xD2);                    /* call rdx (plain) */
        e.patch32_here(j_over);
    }
    /* sentinel? (JIT_RET_SENTINEL == (size_t)-1). The depth DEC runs on
     * each path AFTER it completes - decrementing before the postexit
     * (as this site originally did) let the postexit's INTERPRETED
     * continuation (a fat vm_dispatch C frame) run depth-UNCOUNTED, so
     * a deep recursion whose bodies exit to the interpreter mid-body
     * stacked one un-capped C frame per level - a stack overflow the
     * clang-ASan lane caught (sanitized vm_dispatch frames are ~77KB). */
    e.u8(0x48); e.u8(0x83); e.u8(0xF8); e.u8(0xFF); /* cmp rax, -1 */
    const size_t j_notsent = e.j32(0x75);          /* jne notsent */
    e.movabs(RCX, depth_addr);
    e.u8(0xFF); e.u8(0x09);                        /* dec dword [rcx] */
    const size_t j_done1 = e.j32(0xEB);            /* jmp done */
    e.patch32_here(j_notsent);                     /* notsent: */
    /* #56 step 3: the callee returned JIT_RET_SWITCH (a deeper capped sync
     * call was pushed interpreted-flat). Our C frame dies too; balance the
     * depth (the flat continuation is uncounted by design) and propagate -
     * the record chain re-enters this fragment at OUR post-call stub. */
    e.u8(0x48); e.u8(0x83); e.u8(0xF8); e.u8(0xFD); /* cmp rax, -3 */
    {
        const size_t j_nsw = e.j32(0x75);          /* jne next */
        e.movabs(RCX, depth_addr);
        e.u8(0xFF); e.u8(0x09);                    /* dec dword [rcx] */
        emit_call_epilogue(e);
        e.movabs(RAX, static_cast<uint64_t>(-3));
        e.frag_ret();                                /* ret (propagate) */
        e.patch32_here(j_nsw);
    }
    /* #56 (native Throw): the callee raised past THIS sync frame - the
     * walk stopped at our sync_stop record and set the pending signal.
     * Each sync site is such a stop boundary, so it CONVERTS (rather than
     * propagating): fall into the shared postexit below, whose tail does
     * exactly that (pending -> g_vm_jit_exc + the baked site stamp). The
     * resume-pc argument is irrelevant on that path - the conversion
     * happens before any dispatch - so -2 needs no special case here. */
    /* cold: the shared post-exit (raise/continuation/pending) */
    e.mov_rr(RDI, RAX);
    /* #88: AFTER the callee ran - it may have made calls of its own and
     * overwritten the globals, so this site re-claims them. rax is dead
     * once the exit pc has moved to rdi. */
    emit_bake_call_site(e, ck, old_pc);
    e.movabs(RSI, site);
    e.call_relocs.push_back(
        { e.pos(), reinterpret_cast<const void *>(jit_sync_postexit) });
    e.u8(0xE8); e.u32(0);
    e.movabs(RCX, depth_addr);
    e.u8(0xFF); e.u8(0x09);                        /* dec dword [rcx] */
    e.u8(0x85); e.u8(0xC0);                        /* test eax, eax */
    const size_t j_done2 = e.j32(0x74);            /* jz done */
    emit_call_epilogue(e);
    e.exit_pc(pc);                                 /* exception -> re-raise */
    /* slow: the full helper (identical to the plain emit_sync_call tail).
     * r9 = the baked &locs[i] for THIS call op (#56 step 1: the
     * undefined-callee UndefinedVariableEx is constructed WITH the
     * callee-identifier caret; the Runtime conveys get the same caret from
     * the exc-stamp below). */
    for (const size_t j : j_slows)
        e.patch32_here(j);
    /* #88: the slow tier reads the same side channel. Reached only from a
     * GUARD decline, i.e. before the callee runs, so nothing can have
     * clobbered the globals between here and the helper. */
    emit_bake_call_site(e, ck, old_pc);
    e.movabs(RDI, static_cast<uint64_t>(callee_arg));
    /* #56 step 3: argbase|nargs<<32 packed into ONE arg; the freed reg
     * carries the POST-CALL entry-stub pc (the SWITCH record's resume). */
    e.movabs(RSI, static_cast<uint64_t>(
                      static_cast<uint64_t>(in.a_lit())
                      | (static_cast<uint64_t>(in.b_lit()) << 32)));
    e.movabs(RDX, static_cast<uint64_t>(static_cast<int_type>(
                      g_cur_entry_remap
                          ? (*g_cur_entry_remap)[old_pc + 1]
                          : static_cast<int>(pc) + 1)));
    e.movabs(RCX, static_cast<uint64_t>(static_cast<int_type>(in.target)));
    e.movabs_r8(site);
    {
        const Chunk::LocEntry *lep = nullptr;
        for (const auto &l : ck.locs)
            if (l.pc == old_pc) { lep = &l; break; }
        e.movabs_r9(reinterpret_cast<uint64_t>(lep));
    }
    e.call_relocs.push_back({ e.pos(), slow_helper });
    e.u8(0xE8); e.u32(0);
    e.u8(0x85); e.u8(0xC0);                        /* test eax, eax */
    const size_t j_done3 = e.j32(0x74);            /* jz done */
    /* status 3 = SWITCH: the callee was pushed interpreted-flat; this
     * fragment returns JIT_RET_SWITCH - its consumer drives the callee and
     * the record's baked resume re-enters us at the post-call stub. */
    e.u8(0x83); e.u8(0xF8); e.u8(0x03);            /* cmp eax, 3 */
    {
        const size_t j_sw = e.j32(0x75);           /* jne exc_path */
        emit_call_epilogue(e);
        e.movabs(RAX, static_cast<uint64_t>(-3));  /* JIT_RET_SWITCH */
        e.frag_ret();                                /* ret */
        e.patch32_here(j_sw);
    }
    emit_exc_stamp(e, ck, old_pc);    /* collapse-safe caret (#56 step 1) */
    emit_call_epilogue(e);
    e.exit_pc(pc);
    /* done: */
    e.patch32_here(j_done1);
    e.patch32_here(j_done2);
    e.patch32_here(j_done3);
    for (const size_t j : j_dones)
        e.patch32_here(j);                /* the cached probe's HIT path */
    emit_call_epilogue(e);
}

/* Emit a call to the INT put helper: rdi = &frame->slots[slot], rsi = the
 * int value (src_reg). The base is in rbx (callee-saved), so the lea
 * computes &slot with nothing to restore; src_reg (rax/rdx) is not among
 * the saved regs, so it survives. */
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
                      int dst, uint32_t bail_pc, bool keep_rax = false)
{
    (void)bail_pc;                       /* no bail: helper on the ref path */
    const SlotAddr a = slot_addr(dst);
    if (std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                           static_cast<int32_t>(dst))) {
        const size_t jb_fast = emit_ref_check(e, a.type);
        emit_put_int_call(e, reinterpret_cast<const void *>(jit_put_int),
                          dst, src_reg);
        if (keep_rax)                         /* lever A: the put call
                                               * clobbered RAX - COLD-arm
                                               * reload (the hot two-store
                                               * preserves it for free) */
            e.load(RAX, a.payload);
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
                       uint32_t bail_pc, bool keep_rax = false)
{
    const int cr = e.creg(slot);
    if (cr >= 0) {
        e.mov_rr(static_cast<uint8_t>(cr), src);
        return;
    }
    store_dst(e, ck, src, slot, bail_pc, keep_rax);
}

/*
 * ---- Lever A: ADJACENT DEAD-TEMP FORWARDING (plans/unboxing.md) ----
 *
 * The measured shape: each op is emitted independently, so a value flows
 * producer -> temp slot (two stores, plus a ref check when the temp is
 * ref-listed) -> consumer (a load) even when the temp is alive for
 * exactly one instruction (elem2 -> mul -> addstep round-trips ~7 of
 * 46_matrix_mult's 91.5 Ir/iter). When a whitelisted PRODUCER (ends with
 * its int result in RAX) is immediately followed by a whitelisted
 * CONSUMER reading that temp, the value is handed over IN RAX: the
 * consumer skips the slot load, and when the temp is provably DEAD after
 * the consumer (jit_fwd_info's liveness - throw-resume paths included)
 * and not ref-listed, the producer skips the slot write entirely.
 *
 * The guards, each load-bearing:
 *  - SAME RUN, adjacent pcs; neither op cache-barrier'd.
 *  - the consumer pc is NOT a branch/handler target (jit_fwd_info's
 *    is_tgt) and NOT a post-call resume entry - a jump-in would arrive
 *    with garbage RAX. (A resume entry after a non-call producer is
 *    structurally impossible; the check is a belt.)
 *  - TEMPS only (>= slot_count): a local is observable state and may be
 *    N5-cached; a temp is neither.
 *  - the consumer may read the temp ONLY at forwardable operand
 *    positions (jit_fwd_consumer enumerates them per op) - any other
 *    field naming it (a counter, a bound) reads the SLOT, which
 *    skip_write may have left stale.
 *  - a producer with a SLOW tier reloads RAX from the slot on the
 *    slow-path rejoin (the helper wrote the slot but returns its status
 *    in eax); write-skip stays fast-path-only, which is consistent -
 *    the slot is stale only where nothing reads it.
 *  - a REF-LISTED producer dst keeps its write (the release semantics);
 *    store_dst's COLD ref arm - the jit_put_int call, which clobbers
 *    RAX - reloads it there, so the hot two-store path pays nothing
 *    (an unconditional post-write reload measured the whole yield away:
 *    +2 Ir/iter on 46_matrix_mult against the predicted -2).
 *
 * HONESTY NOTE on the deadness net: forcing skip_write unconditionally
 * survives the full suite AND a 200-program fuzz round - for the
 * CURRENT whitelists, a temp consumed by the adjacent op is always dead
 * (codegen's expression temps are consumed exactly once). The net is
 * what makes GROWING the consumer whitelist safe: admitting, say,
 * JumpUnlessIntCmp would pair a counted loop's BOUND temp, which is
 * read every iteration - exactly the shape the liveness refuses. Keep
 * the net; it is the invariant, not the reach.
 *
 * THE CONTRACT jit_fwd_consumer PINS: for every op it returns true for,
 * that op's emit (emit_op / emit_branch) honors g_fwd.in_rax for exactly
 * the operand positions the predicate accepted. Growing the whitelist
 * means growing BOTH sides in the same change.
 */
struct JitFwd {
    int in_rax = -1;    /* consumer side: RAX holds this TEMP's value */
    int prod = -1;      /* producer side: this op's dst temp qualifies */
    bool skip_write = false;
    bool armed = false; /* the producer's emit confirmed RAX at its exit */
};
static JitFwd g_fwd;

static bool jit_slot_ref_listed(const Chunk &ck, int slot)
{
    return std::binary_search(ck.ref_slots.begin(), ck.ref_slots.end(),
                              static_cast<int32_t>(slot));
}

static bool jit_fwd_producer(const Instr &in, int &dst)
{
    switch (in.op) {
    case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
    case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
    case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
    case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI:
    case OpCode::LoadElemInt:
    case OpCode::LoadElem2Int:
        dst = in.target;
        return true;
    default:
        return false;
    }
}

static bool jit_fwd_consumer(const Instr &nx, int t)
{
    switch (nx.op) {
    case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
    case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
        return nx.a_slot() == t
            || (!nx.b_is_lit() && nx.b_slot() == t);
    case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
    case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI:
        return nx.a_slot() == t;
    case OpCode::IntAddStep:
        /* the accumulate VALUE only; the accumulator, the counter and a
         * slot bound all read their SLOTS */
        return !nx.b_is_lit() && nx.b_slot() == t
            && nx.a_dual_lo() != t && nx.target2 != t
            && (nx.a_is_lit() || nx.a_dual_hi() != t);
    default:
        return false;
    }
}

/* TESTS: the execution proof - forwarded consumers bump at RUNTIME (the
 * emitted-code counter rule; rdx is dead at every bump site). */
static void emit_fwd_bump(Emitter &e)
{
#ifdef TESTS
    e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_fwd));
    e.u8(0x48); e.u8(0xFF); e.u8(0x02);          /* inc qword [rdx] */
#endif
}

/*
 * ---- C1: PER-LOOP GUARD + NAVIGATION HOISTING ----
 * (plans/typed-invariant-arrays.md, the staircase's first step)
 *
 * A loop's element ops re-derive the base's navigation EVERY element:
 * type tag, slice flag, SharedObject, kind, data/finish, count - for a
 * base that provably cannot change inside the loop. C1 verifies the
 * guards ONCE at the fragment entry (the loop's preheader - the native
 * back edge jumps past it) and pins (data pointer, count) in two
 * callee-saved registers the N5 pick left free; the per-element form is
 * bounds-check + read. A failed entry guard jumps to a COLD TWIN of the
 * whole body - the unhoisted emission - so there is never a mid-loop
 * bail with derived registers (approach A at loop granularity), and a
 * DELETED run needs no interpreted original to fall back on.
 *
 * SOUNDNESS is run-shape, not dataflow: hoisting happens only when
 * EVERY op in the run is on a READ-ONLY whitelist - no calls (arbitrary
 * code), no stores (COW detach moves the data pointer), no boxed PMF
 * ops (TypeArr::add MUTATES the left operand) - and no op DEFINES the
 * base slot (a MoveV onto it would rebind it mid-loop). Within one
 * run's execution nothing else runs (single-threaded, calls excluded),
 * so memory the registers were derived from cannot move. The element
 * ops' slow tiers (negative wrap, OOB) read MEMORY, which is never
 * stale - only the registers are derived state, and the cold twin
 * re-derives nothing (it is the ordinary emission).
 *
 * Entry-pc note: a hoistable run can contain no sync calls and no
 * handler ops, so no post-call resume stub or handler resume can land
 * inside it - jumping past the entry navigation is structurally
 * impossible; the pick still scans `entries` as a belt.
 */
static bool op_is_branch(OpCode op);           /* defined below */

/* The read-only whitelist: ops that cannot run user code, cannot mutate
 * any array (so no COW detach can move a hoisted data pointer), and
 * cannot rebind slots beyond their own scalar dsts. */
static bool jit_hoist_op_ok(const Instr &in)
{
    switch (in.op) {
    case OpCode::LoadImmInt: case OpCode::LoadImmFloat:
    case OpCode::MoveV:
    case OpCode::IntBin:
    case OpCode::IntAddRR: case OpCode::IntSubRR: case OpCode::IntMulRR:
    case OpCode::IntAndRR: case OpCode::IntOrRR:  case OpCode::IntXorRR:
    case OpCode::IntShlRR: case OpCode::IntShrRR:
    case OpCode::IntAddRI: case OpCode::IntSubRI: case OpCode::IntMulRI:
    case OpCode::IntAndRI: case OpCode::IntOrRI:  case OpCode::IntXorRI:
    case OpCode::IntShlRI: case OpCode::IntShrRI:
    case OpCode::IntModRI: case OpCode::IntAddModRI:
    case OpCode::FloatBin:
    case OpCode::FloatAddRR: case OpCode::FloatSubRR:
    case OpCode::FloatMulRR: case OpCode::FloatAddRI:
    case OpCode::FloatSubRI: case OpCode::FloatMulRI:
    case OpCode::CmpIntV: case OpCode::CmpFloatV:
    case OpCode::Jump:
    case OpCode::JumpUnlessIntCmp: case OpCode::JumpUnlessFloatCmp:
    case OpCode::JumpUnlessTrueV:  /* is_true: reads, throws, never
                                    * mutates a container */
    case OpCode::ForLoopStep: case OpCode::IntAddStep:
    case OpCode::JumpUnlessElemInt: case OpCode::ForStepElemInt:
    case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
    case OpCode::LoadElem2Int: case OpCode::LoadElem2Float:
    case OpCode::ArrLen: case OpCode::StrLen: case OpCode::OrdCharV:
    case OpCode::ReturnV:
    /*
     * The PLAIN store family is read-only FOR A HOISTED BASE'S STORAGE,
     * which is the property that matters: flat_store_core /
     * vm_subscript_store move a base's data ONLY via clone_internal_vec
     * (the store's own base is a SLICE - our entry guard excludes that
     * for the hoisted slot, and a slice ALIAS reseats its own handle,
     * never our shobj) or a strs-kind promote (our kind guard excludes
     * strs); clone_aliased_slices detaches the VIEWS and leaves the
     * base's vector in place. What genuinely moves storage is GROWTH -
     * append/insert/emplace - and those are builtin-call ops, refused
     * above. The first version excluded stores wholesale and C1 fired
     * on ZERO benches: real loops store somewhere (46's row[j] = s).
     */
    case OpCode::StoreElemInt: case OpCode::StoreElemFloat:
    case OpCode::StoreElemValue: case OpCode::StoreElem2V:
    case OpCode::DictStore:
    /* C1d: the typed dict READS - strictly weaker than DictStore
     * (admitted above): a default-dict vivify mutates the DICT's own
     * map nodes, never any array's element vector, and a KeyNotFound
     * conveys out (a region is never resumed after a throw). Helper
     * calls, so the epilogue re-derives r10/r11. Unlocks 68_nested's
     * ForStepElemInt loops, which carry one. */
    case OpCode::DictLoadInt: case OpCode::DictLoadFloat:
        return true;
    default:
        return false;
    }
}

/* The slots an op in the whitelist above can WRITE (its scalar dsts +
 * the fusions' counters). Enumerated HERE, for the whitelist only - a
 * new whitelisted op must be added to BOTH switches. */
template <typename D>
static void jit_hoist_op_defs(const Instr &in, D d)
{
    switch (in.op) {
    case OpCode::MoveV:
        d(in.target); break;
    case OpCode::ForLoopStep:
        d(in.target2); break;                    /* the counter */
    case OpCode::IntAddStep:
        d(in.a_dual_lo()); d(in.target2); break; /* acc + counter */
    case OpCode::ForStepElemInt:
        /* the counter + the elem dst; `target` is this op's BRANCH pc,
         * not a slot (the first version marked it a def - a pc-numbered
         * slot was spuriously killed as a candidate) */
        d(in.target2); d(in.b_dual_hi()); break;
    case OpCode::Jump:
    case OpCode::JumpUnlessIntCmp: case OpCode::JumpUnlessFloatCmp:
    case OpCode::JumpUnlessElemInt: case OpCode::JumpUnlessTrueV:
    case OpCode::StoreElemInt: case OpCode::StoreElemFloat:
    case OpCode::StoreElemValue: case OpCode::StoreElem2V:
    case OpCode::DictStore:
        break;                                   /* no frame-slot writes */
    default:
        d(in.target);                            /* the scalar dst */
        break;
    }
}

/*
 * Pick the LOOP REGIONS to hoist for the run [begin, end). A region is
 * a backward branch's span [T, L]; candidates are tried INNERMOST-first
 * (smallest span), each accepted region excludes overlapping ones
 * (multi-region: 43_sieve has three hot loops - a one-region pick
 * served only the tiny fill loop). Per region, one base with one
 * consistent kind, from the element READERS and (C1b) the plain STORE
 * ops; `has_store` records that the region writes the base, which is
 * what makes the preheader emit the store guards + the one-shot hash
 * invalidation (emitting those unconditionally would send a read-only
 * loop over a CONST base to the cold twin - losing the read hoisting).
 * The returned list is sorted by T for the emission walk.
 */
struct HoistRegion {
    size_t T, L;
    int base, kind;
    bool has_store;
};

static std::vector<HoistRegion>
jit_hoist_pick(const Chunk &chunk, size_t begin, size_t end,
               const std::vector<std::pair<size_t, size_t>> &entries)
{
    static const bool dbg = getenv("MYLANG_HOISTDBG") != nullptr;

    std::vector<std::pair<size_t, size_t>> regions;   /* (T, L) */
    for (size_t p = begin; p < end; p++) {
        const Instr &in = chunk.code[p];
        if (op_is_branch(in.op)
                && in.target >= static_cast<int>(begin)
                && in.target <= static_cast<int>(p))
            regions.push_back({ static_cast<size_t>(in.target), p });
    }
    std::sort(regions.begin(), regions.end(),
              [](const std::pair<size_t, size_t> &a,
                 const std::pair<size_t, size_t> &b) {
                  return (a.second - a.first) < (b.second - b.first);
              });

    std::vector<HoistRegion> out;
    for (const auto &rg : regions) {
        const size_t T = rg.first, L = rg.second;
        bool ok = true;
        for (const HoistRegion &acc : out)       /* no overlap/nesting */
            if (T <= acc.L && acc.T <= L)
                ok = false;
        for (size_t p = T; p <= L && ok; p++)
            if (!jit_hoist_op_ok(chunk.code[p])) {
                if (dbg) fprintf(stderr,
                                 "hoist[%zu,%zu): op %d at %zu\n",
                                 T, L, (int)chunk.code[p].op, p);
                ok = false;
            }
        if (!ok)
            continue;
        /* every jump INTO the region originates inside it */
        for (size_t p = begin; p < end && ok; p++) {
            const Instr &in = chunk.code[p];
            if (op_is_branch(in.op)
                    && in.target >= static_cast<int>(T)
                    && in.target <= static_cast<int>(L)
                    && (p < T || p > L))
                ok = false;
        }
        for (const auto &pe : entries)
            if (pe.first >= T && pe.first <= L)
                ok = false;
        for (const Chunk::HandlerSite &hs : chunk.handler_sites) {
            for (const Chunk::HandlerClause &cl : hs.clauses)
                if (cl.body_pc >= static_cast<int>(T)
                        && cl.body_pc <= static_cast<int>(L))
                    ok = false;
            if (hs.fin_pc >= static_cast<int>(T)
                    && hs.fin_pc <= static_cast<int>(L))
                ok = false;
        }
        if (!ok)
            continue;

        /* candidates: base slot -> {kind, uses, store}; kind conflicts
         * drop. Stores are candidates too (C1b) - a PLAIN store's
         * hoisted form is bounds + raw write. */
        struct Cand { int kind; int uses; bool dead; bool store; };
        std::map<int, Cand> cands;
        const auto add = [&](int slot, int kind, bool store) {
            /* C1d: TEMPS are admitted as bases too (a foreach over an
             * array snapshots the container into a TEMP, so every
             * foreach loop - and 68's ForStepElemInt shapes - has a
             * temp base). N5's temp hazard (eager entry-load +
             * exit-FLUSH overwriting a live temp) does not apply: the
             * base slot is only READ, at the preheader, and a def
             * inside the region kills the candidate below. A store
             * through another alias cannot move the storage either -
             * element stores never detach a plain alias (#92's
             * has_slices rule); growth is a builtin call, refused by
             * the whitelist. */
            if (slot < 0
                    || slot >= chunk.slot_count + chunk.n_temps)
                return;
            auto it = cands.find(slot);
            if (it == cands.end())
                cands[slot] = { kind, 1, false, store };
            else if (it->second.kind != kind)
                it->second.dead = true;
            else {
                it->second.uses++;
                it->second.store |= store;
            }
        };
        for (size_t p = T; p <= L; p++) {
            const Instr &in = chunk.code[p];
            switch (in.op) {
            case OpCode::LoadElemInt:
            case OpCode::JumpUnlessElemInt:  /* C1d: the fused sieve test
                                              * (base/idx ride the load's
                                              * own fields + hint) */
                add(in.target2, in.elem_bool_hint() ? 3 : 0, false);
                break;
            case OpCode::ForStepElemInt:     /* C1d: the back-edge load;
                                              * base = b_dual_lo, the hint
                                              * TRANSFERRED by the fusion */
                add(in.b_dual_lo(), in.elem_bool_hint() ? 3 : 0, false);
                break;
            case OpCode::LoadElemFloat: add(in.target2, 1, false); break;
            case OpCode::LoadElem2Int:
            case OpCode::LoadElem2Float: add(in.target2, 2, false); break;
            /* store candidates. StoreElemInt in a RUN always has a
             * LOCAL base (jit_op_eligible admits target == 0 only), so
             * target2 is a frame slot like the readers'. C1c: the
             * compile-time ELEM-BOOL hint picks the bools kind (3) -
             * without it a bool array (43/56_sieve) failed the ints
             * guard every entry and ran its cold twin. */
            case OpCode::StoreElemInt:
                add(in.target2, in.elem_bool_hint() ? 3 : 0, true);
                break;
            case OpCode::StoreElemFloat: add(in.target2, 1, true); break;
            default: break;
            }
        }
        for (size_t p = T; p <= L; p++)
            jit_hoist_op_defs(chunk.code[p], [&](int s) {
                auto it = cands.find(s);
                if (it != cands.end())
                    it->second.dead = true;      /* redefined in the loop */
            });

        int best = -1, best_uses = 0, best_kind = 0;
        bool best_store = false;
        for (const auto &kv : cands) {
            if (kv.second.dead)
                continue;
            /* a STORE candidate with kind 2 cannot exist (no general
             * store op feeds cands) - belt below anyway */
            if (kv.second.uses > best_uses) {
                best = kv.first;
                best_uses = kv.second.uses;
                best_kind = kv.second.kind;
                best_store = kv.second.store;
            }
        }
        if (best < 0)
            continue;
        out.push_back({ T, L, best, best_kind, best_store });
        if (dbg) fprintf(stderr,
                         "hoist[%zu,%zu): PICKED slot %d kind %d%s\n",
                         T, L, best, best_kind,
                         best_store ? " +store" : "");
    }
    std::sort(out.begin(), out.end(),
              [](const HoistRegion &a, const HoistRegion &b) {
                  return a.T < b.T;
              });
    return out;
}

/* THE REGISTER-CACHE AUDIT (env MYLANG_CACHEAUDIT=1). Which opcode
 * DISQUALIFIED a slot that would otherwise have been pinned - the "what to
 * make cache-aware next" surface. A bad() site only matters in proportion
 * to the candidates it actually kills, and that is not readable off the
 * source: it depends on which shapes real programs compile to. Off by
 * default, so a normal compile pays one already-loaded bool test per
 * bad(). Printed at exit by cache_audit_report (mylang.cpp / the harness).
 */
bool g_cache_audit = getenv("MYLANG_CACHEAUDIT") != nullptr;
static std::unordered_map<int, long> g_cache_killed;   /* opcode -> kills */
static long g_cache_pinned = 0, g_cache_lost = 0;

static const char *jit_op_name(OpCode op)
{
#define ML_OPCODE_NAMEV(N) #N,
    static const char *const names[] = { ML_FOR_EACH_OPCODE(ML_OPCODE_NAMEV) };
#undef ML_OPCODE_NAMEV
    const size_t i = static_cast<size_t>(op);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

void jit_cache_audit_report()
{
    if (!g_cache_audit)
        return;
    std::vector<std::pair<long, int>> v;
    for (const auto &kv : g_cache_killed)
        v.push_back({ kv.second, kv.first });
    std::sort(v.begin(), v.end(),
              [](const std::pair<long,int> &x, const std::pair<long,int> &y)
              { return x.first != y.first ? x.first > y.first
                                          : x.second < y.second; });
    fprintf(stderr, "CACHEAUDIT pinned %ld lost %ld\n",
            g_cache_pinned, g_cache_lost);
    for (const auto &pr : v)
        fprintf(stderr, "CACHEAUDIT %-26s %ld\n",
                jit_op_name(static_cast<OpCode>(pr.second)), pr.first);
}

/* How many hot slots a fragment may pin: one per available CALLEE-SAVED
 * register (r12-r15). They survive a helper call for free, so the pool
 * costs one push/pop per fragment ENTRY instead of one per CALL - which
 * is what let it grow from two registers to four at the same time. */
static const uint8_t CACHE_REGS[] = { 12, 13, 14, 15 };
static const size_t MAX_CACHED = sizeof(CACHE_REGS) / sizeof(CACHE_REGS[0]);
/* C2a: the float pool - xmm4-7 (xmm0/1 are the per-op scratch) */
static const uint8_t FCACHE_REGS[] = { 4, 5, 6, 7 };
static const size_t MAX_FCACHED =
    sizeof(FCACHE_REGS) / sizeof(FCACHE_REGS[0]);

/* N5: pick up to MAX_CACHED hot INT-scalar slots to pin for a run.
 * A slot qualifies iff it is a RESOLVED LOCAL (< slot_count) and EVERY use
 * in [begin,end) is an int-scalar read/write (the int-arith / loop ops);
 * any float / array / member touch DISQUALIFIES it. Ranked by use count
 * (>= 3), the top MAX_CACHED chosen.
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
/* WHICH bad() SITES ARE WORTH REMOVING - the rule, from measurement.
 *
 * There are two kinds. An op that can read the REGISTER (MoveV: the copy
 * is just an int store) becomes cache-aware for free, and removing its
 * bad() is a clear win - 01_while_loop -5.92%, 07_nested_loops -5.43%.
 * An op whose helper must see MEMORY - anything taking &slot, or reading
 * the frame through g_current_ctx (the container stores, the boxed
 * ladder, the subscript read) - cannot: keeping the value in a register
 * means writing it back before EVERY execution of that op.
 *
 * That second case was BUILT AND MEASURED (flush the one operand at the
 * emit, two stores, no reload - much cheaper than marking the op a
 * barrier, which flushes and reloads the whole pool). It does not pay:
 * 23_dict_insert -0.42% but 46_matrix_mult +0.27%, 43_sieve +0.04%,
 * 14_array_subscript +0.03%, 56_sieve_bool +0.01%, 62_dict_word_count
 * +0.00%. The flush costs 2 stores per ITERATION while the register saves
 * ~1 load per other use of the counter, so it lands on zero. The
 * disqualification these sites do is the RIGHT trade, not an oversight.
 *
 * So: a bad() site is worth removing only when the op can be taught to
 * read the register itself. MYLANG_CACHEAUDIT=1 ranks the sites by
 * candidacies killed; check that FIRST, then check which kind it is.
 */
static std::vector<int>
pick_cached_slots(const Chunk &ck, size_t begin,
                  size_t end, int slot_count,
                  std::vector<char> *barrier = nullptr,
                  std::vector<int> *fhot = nullptr)
{
    const std::vector<Instr> &code = ck.code;
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
    /* C2a: the FLOAT pool's parallel accounting. The two pools are
     * DISJOINT by construction: an int use disqualifies the float side
     * (an int-cache-aware read goes to memory, stale for a float pin)
     * and a float use disqualifies the int side (the pre-C2a bad()).
     * A slot QUALIFIES for the float pool only when some float op WROTE
     * it in the run (`fdst`): a float op can legitimately READ a
     * definitely-int slot through the promote arm, and pinning such a
     * slot would movsd its int payload bits as a double. A float-WRITTEN
     * slot is t_float from its first write, every other writer in the
     * run is a float op (any non-float writer disqualified it), and the
     * pre-first-write window holds either an earlier float value or the
     * pre-decl none, whose garbage entry-load is dead by def-before-use
     * - the exact soundness shape the int pool established. */
    std::unordered_map<int, int> use_f;
    std::unordered_set<int> disq_f, fdst;
    const auto badf = [&](int s) { if (s >= 0) disq_f.insert(s); };
    const auto usei = [&](int s) {
        if (s >= 0) { use[s]++; disq_f.insert(s); }
    };
    const auto usef = [&](int s) { if (s >= 0) { use_f[s]++; } };
    /* CACHE AUDIT (env MYLANG_CACHEAUDIT=1): which opcode DISQUALIFIED a
     * slot that would otherwise have been pinned. This is the "what to make
     * cache-aware next" surface - a bad() site only matters in proportion
     * to the candidates it actually kills, which is not guessable from the
     * source (MoveV looked obvious and was; the rest is an open question).
     * Recorded only when the audit is on, so a normal compile pays one
     * null test per bad(). */
    std::unordered_map<int, std::vector<OpCode>> killed_by;
    const bool audit = g_cache_audit;
    OpCode cur_op = OpCode::Halt;
    const auto bad  = [&](int s) {
        if (s < 0)
            return;
        disq_f.insert(s);        /* memory-touched: no float pin either */
        disq.insert(s);
        if (audit)
            killed_by[s].push_back(cur_op);
    };
    /* C2a: the INT-side-only disqualifier, for the FLOAT ops' slots -
     * they leave the int pool (a float value can't live in a GP pin)
     * but stay float candidates. Everything else keeps the both-pool
     * bad() above, which is the safe default: a slot only escapes the
     * float pool's disqualification where a case explicitly says its
     * reads/writes go through the cache-aware float choke points. */
    const auto badi = [&](int s) {
        if (s < 0)
            return;
        disq.insert(s);
        if (audit)
            killed_by[s].push_back(cur_op);
    };

    for (size_t pc = begin; pc < end; pc++) {
        const Instr &in = code[pc];
        cur_op = in.op;
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
             * the float op that produced it (badi()), and a slot reachable
             * only via ReturnV is used once (< the 3-use cache threshold) - so
             * no float slot is ever cached as an int here. C2a: it must NOT
             * disqualify the FLOAT side (usei normally badf's) - the emit
             * runs flush_cache BEFORE jit_ret, so a float-pinned result
             * slot is current in memory when the helper reads it. */
            if (in.a_slot() >= 0)
                use[in.a_slot()]++;
            break;
        /* float ops: no int-pin (bad), but C2a float-pool USES - the
         * operand reads go through emit_float_load and the dst writes
         * through emit_float_store, both cache-aware. Only the DST
         * qualifies a slot (`fdst`); an operand may be a definitely-int
         * slot served by the promote arm. FloatBin's div/mod arms are
         * fine: the div0 raise exits through the flushing epilogue and
         * the fmod libm call spills the pins via the shared prologue. */
        case OpCode::FloatBin:
        case OpCode::FloatAddRR: case OpCode::FloatSubRR:
        case OpCode::FloatMulRR: case OpCode::FloatAddRI:
        case OpCode::FloatSubRI: case OpCode::FloatMulRI:
            if (!in.a_is_lit()) { badi(in.a_slot()); usef(in.a_slot()); }
            if (!in.b_is_lit()) { badi(in.b_slot()); usef(in.b_slot()); }
            badi(in.target); usef(in.target); fdst.insert(in.target);
            break;
        case OpCode::JumpUnlessFloatCmp:
            if (!in.a_is_lit()) { badi(in.a_slot()); usef(in.a_slot()); }
            if (!in.b_is_lit()) { badi(in.b_slot()); usef(in.b_slot()); }
            break;
        case OpCode::LoadImmFloat:
            badi(in.target); usef(in.target); fdst.insert(in.target);
            break;
        case OpCode::MathFnV:
            /* C2a: previously UNLISTED - one math builtin disabled
             * pinning for its whole run. Arg(s) via emit_float_load,
             * dst via emit_float_store (cache-aware); an MK_CALL
             * selector's libm call spills the float pins via the shared
             * prologue/epilogue. The int side stays memory (bad). */
            if (!in.a_is_lit()) { badi(in.a_slot()); usef(in.a_slot()); }
            if (!in.b_is_lit()) { badi(in.b_slot()); usef(in.b_slot()); }
            badi(in.target); usef(in.target); fdst.insert(in.target);
            break;
        case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
        case OpCode::OrdCharV:      /* same shape: idx cache-aware (a VALUE
                                     * arg to jit_ord_char), base/dst leas */
            /* the INDEX is read cache-aware (load_index_r9), so it stays a
             * countable int use - it is the loop counter, the slot most worth
             * pinning. base is read in memory. LoadElemFloat's dst is
             * written via emit_float_store -> a C2a float candidate; the
             * int/ord dsts stay memory (the two-store / store_dst). */
            bad(in.target2);
            if (in.op == OpCode::LoadElemFloat) {
                badi(in.target);
                usef(in.target); fdst.insert(in.target);
            } else {
                bad(in.target);
            }
            if (!in.a_is_lit()) usei(in.a_slot());
            break;
        case OpCode::LoadElem2Int: case OpCode::LoadElem2Float:
            /* BOTH indices are VALUE args to jit_load_elem2_* (read
             * cache-aware), so they stay countable int uses - in a matrix
             * loop they are the two loop counters, the slots most worth
             * pinning. base is a lea -> memory. The FLOAT dst is written
             * via emit_float_store -> a C2a float candidate. */
            bad(in.target2);
            if (in.op == OpCode::LoadElem2Float) {
                badi(in.target);
                usef(in.target); fdst.insert(in.target);
            } else {
                bad(in.target);
            }
            usei(in.a_dual_lo());
            if (!in.b_is_lit()) usei(in.b_slot());
            break;
        case OpCode::StoreElemInt:
            bad(in.target2);             /* base slot holds an array */
            if (!in.a_is_lit()) usei(in.a_slot());   /* index (int) */
            if (!in.b_is_lit()) usei(in.b_slot());   /* value (int) */
            break;
        case OpCode::StoreElemFloat:
            bad(in.target2);             /* base slot holds an array */
            if (!in.b_is_lit()) {        /* value: a FLOAT, read via
                                          * emit_float_load (cache-aware -
                                          * a C2a float use, no fdst) */
                badi(in.b_slot()); usef(in.b_slot());
            }
            if (!in.a_is_lit()) usei(in.a_slot());   /* index (int) */
            break;
        case OpCode::DictStore:
            /* the fragment passes &slot for base/key/value, so those slots
             * must hold CURRENT EvalValues - a cached int key (a counter used
             * as d[i]) would leave its slot stale. Disqualify all three -
             * flushing them at the emit instead was measured and REJECTED,
             * see the note above pick_cached_slots. */
            bad(in.target2); bad(in.a_slot()); bad(in.b_slot());
            break;
        case OpCode::StoreElemValue:
            /* jit_store_elem_value reads idx (a_slot) + val (b_slot) - and a
             * LOCAL base (target2) - from MEMORY via g_current_ctx. The idx and
             * index (a counter used as a[i]) would be stale. FLUSHING it at the
             * emit instead was measured and REJECTED - see the note above
             * pick_cached_slots. The base is a frame slot only for a LOCAL
             * base (kind == target == 0). */
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
            /* C2a: the source side is float-cache-aware too (the emit
             * reads the pin via emit_float_store), and like the int side
             * it contributes NO WEIGHT - a boxed move must never be the
             * evidence a slot holds a float. The DEST stays memory-only
             * (a MoveV can write ANY type). */
            badf(in.target);
            /* CACHE-AWARE ON THE SOURCE SIDE (see the emit). A pinned source
             * is a proven int, so the move is just the ordinary int store and
             * the register is read directly - which is why target2 is NOT
             * disqualified here.
             *
             * It is NOT counted either (no usei), and that omission is the
             * SOUNDNESS ANCHOR. A MoveV is the BOXED move: the bytecode says
             * nothing about the value's type, so it can never be the evidence
             * that a slot holds an int. Contributing zero weight means a slot
             * reaches the cache only if some genuine int op qualified it - and
             * once it has, every write to it in this run is an int write, so
             * the value this MoveV reads really is an int.
             *
             * The DEST stays memory-only: a MoveV can write ANY type, so a
             * pinned dst could silently stop holding an int. */
            bad(in.target);
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
        case OpCode::CheckCallableV:
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
            /* PLANNED (b_dual_hi >= 0): an act-0 (int) plan src is read
             * CACHE-AWARE by the emit (a direct-local src can be the loop
             * counter - `P(i, ..)`), so it stays a countable int use; an
             * act-1/2 src reads memory (emit_float_load / byte load) ->
             * bad. dst: the H1 guards read/write it from memory -> bad.
             * UNPLANNED: the whole run is read from memory by the helper. */
            if (in.b_dual_hi() >= 0) {
                for (const Chunk::CtorPlanField &pf :
                         ck.ctor_plans[in.b_dual_hi()].f) {
                    if (pf.act == 0)
                        usei(pf.src);
                    else
                        bad(pf.src);
                }
            } else {
                for (int i = 0; i < in.b_dual_lo(); i++)
                    bad(static_cast<int>(in.a_lit()) + i);
            }
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
            /* float operands (cache-aware via emit_float_load - C2a
             * float uses) + a BOOL dst written to memory (bad both). */
            if (!in.a_is_lit()) { badi(in.a_slot()); usef(in.a_slot()); }
            if (!in.b_is_lit()) { badi(in.b_slot()); usef(in.b_slot()); }
            bad(in.target); badf(in.target);
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
        case OpCode::Throw:
        case OpCode::Rethrow:
            break;                       /* an unconditional exit - no slots */
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
    for (size_t i = 0; i < cand.size() && i < MAX_CACHED; i++)
        out.push_back(cand[i].second);

    /* C2a: the float pool's picks - local, undisqualified, >= 3 uses,
     * and WRITTEN by a float op in the run (see the soundness note at
     * the accounting above). Temps are excluded for the pool's usual
     * reason (scratch reused across run boundaries). */
    if (fhot) {
        std::vector<std::pair<int, int>> fc;
        for (const auto &kv : use_f)
            if (kv.first < slot_count && !disq_f.count(kv.first)
                    && kv.second >= 3 && fdst.count(kv.first))
                fc.push_back({ kv.second, kv.first });
        std::sort(fc.begin(), fc.end(),
                  [](const std::pair<int,int> &a,
                     const std::pair<int,int> &b) {
                      return a.first != b.first ? a.first > b.first
                                                : a.second < b.second;
                  });
        for (size_t i = 0; i < fc.size() && i < MAX_FCACHED; i++)
            fhot->push_back(fc[i].second);
    }

    /* AUDIT: a slot LOST to disqualification is one that is a resolved
     * local and used often enough to have cleared the threshold - i.e. it
     * would have been a candidate, and only bad() stopped it. Attribute it
     * to EVERY opcode that disqualified it (they share the blame; making
     * just one cache-aware may not free the slot on its own, which is
     * itself worth seeing in the numbers). */
    if (audit) {
        g_cache_pinned += static_cast<long>(out.size());
        for (const auto &kv : use) {
            if (kv.first >= slot_count || kv.second < 3
                    || !disq.count(kv.first))
                continue;
            g_cache_lost++;
            std::unordered_set<int> seen;
            for (const OpCode o : killed_by[kv.first])
                if (seen.insert(static_cast<int>(o)).second)
                    g_cache_killed[static_cast<int>(o)]++;
        }
    }
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
    emit_call_prologue(e);               /* save the cache regs, align */
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
/* `no_bail`: the 2-way form (float -> movsd, ELSE -> cvtsi2sd) for an op
 * that must be exit-free (deletability). Sound for every state the release
 * interpreter accepts: read_float_slot promotes int AND bool (a bool
 * payload is 0/1 fully zero-extended, so cvtsi2sd yields the same 1.0/0.0)
 * and ML_VM_CHECKs anything else - a state inference already excludes, so
 * the raw convert matches the release interpreter's own trust of the
 * proven tag. The default 3-way form keeps the bail for ops whose
 * originals survive. */
static void emit_float_load(Emitter &e, uint8_t xr, bool is_lit,
                            float_type flit, int slot, uint32_t bail_pc,
                            bool no_bail = false)
{
    if (is_lit) {
        uint64_t bits;
        std::memcpy(&bits, &flit, sizeof bits);
        e.movabs(RAX, bits);
        e.movq_xmm(xr);
        return;
    }
    /* C2a: a float-PINNED slot reads register-to-register - no type
     * dispatch (the pool qualifies only float-WRITTEN slots, proven
     * t_float; see pick_cached_slots). */
    if (const int fr = e.fcreg(slot); fr >= 0) {
        e.fmov_rr(xr, static_cast<uint8_t>(fr));
        return;
    }
    const SlotAddr a = slot_addr(slot);
    e.load_type(a.type);                 /* rax = slot type */
    e.cmp_rax_r8();                       /* == t_float ? */
    const size_t j_notf = e.j8(0x75);     /* jne -> not float */
    e.fload(xr, a.payload);               /* FAST: movsd xmm, [payload] */
    const size_t j_done1 = e.j8(0xEB);    /* jmp done */
    e.patch8(j_notf, e.pos());
    if (!no_bail) {
        e.cmp_rax_rsi();                  /* == t_int ? */
        const size_t j_int = e.j8(0x74);  /* je -> promote */
        e.exit_pc(bail_pc);               /* neither -> bail */
        e.patch8(j_int, e.pos());
    }
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
    /* C2a: a float-PINNED dst is a register write - no type store, no
     * ref check (every writer of a pinned slot in the run is a float
     * op, so it can never hold a reference; the epilogue flush restores
     * type+payload to memory). */
    if (const int fr = e.fcreg(dst); fr >= 0) {
        e.fmov_rr(static_cast<uint8_t>(fr), xr);
        return;
    }
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
 * the call clobbers that the fragment relies on: rsi (t_int) and r8
 * (t_float) - constant singletons, RE-materialised after. The slots base
 * is in rbx, which libm preserves for us. The xmm regs are all caller-
 * saved but the JIT never keeps a float LIVE in a register across ops
 * (floats are slot-backed - only the INT cache uses GP r10/r11, and a
 * MathFnV run caches NOTHING per pick_cached_slots' default), so only
 * xmm0 (arg->result) matters and it survives. Stack: frag_entry's push
 * left rsp % 16 == 0 and a MathFnV run caches nothing, so the prologue
 * emits nothing and the call is already aligned. */
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
     * caches nothing today, so the prologue is usually EMPTY - but going
     * through it keeps the call correct if that ever changes. */
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

/* The CONVEY form of emit_raise (re-raise deletability): a cold call to
 * jit_raise_kind_exc builds the exception into g_vm_jit_exc, the exc-stamp
 * adds the op's own caret, and the exit lands in EnterNative's
 * g_vm_jit_exc branch - pc-independent, so the op can be deleted. Used by
 * the INT div/mod/shift arms; the float div/mod and the elem-OOB sites
 * keep the g_vm_jit_raise signal (their ops are non-deletable anyway -
 * bailing operand loads / base gates). The prologue/epilogue bracket
 * preserves rdi + any N5-pinned regs for the exit flush. */
static void emit_raise_convey(Emitter &e, const Chunk &ck, int kind,
                              uint32_t pc, size_t old_pc)
{
    emit_call_prologue(e);
    e.movabs(RDI, static_cast<uint64_t>(kind));
    e.call_relocs.push_back(
        { e.pos(), reinterpret_cast<const void *>(jit_raise_kind_exc) });
    e.u8(0xE8); e.u32(0);
    emit_call_epilogue(e);
    emit_exc_stamp(e, ck, old_pc);
    e.exit_pc(pc);
}

static void raise_convey_unless(Emitter &e, const Chunk &ck,
                                uint8_t pass_cond, int kind, uint32_t pc,
                                size_t old_pc)
{
    /* NEAR jcc: the convey sequence (helper bracket + exc-stamp + exit) is
     * ~100 bytes - far past a short jump's +127 (a patch8 assert caught the
     * first build). */
    const size_t sk = e.j32(pass_cond);
    emit_raise_convey(e, ck, kind, pc, old_pc);
    e.patch32_here(sk);
}

/* The shared REG-COUNT shift core (rax = value, rcx = count): a negative
 * count RAISES InvalidValueEx (JR_NEG_SHIFT), a count >= 64 SATURATES (0 for
 * shl/ushr, a full sign-fill for the arithmetic shr), else the machine shift
 * by cl - exactly bit_shl/bit_shr/bit_ushr (bitops.h). Used by the IntShlRR/
 * IntShrRR reg branch AND the generic-IntBin shift arms, so the two cannot
 * drift. Result in rax. */
static void emit_reg_shift(Emitter &e, const Chunk &ck, Op aop, uint32_t pc,
                           size_t old_pc)
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
    emit_raise_convey(e, ck, JR_NEG_SHIFT, pc, old_pc);  /* negative count:
                                                          * CONVEY InvalidValue
                                                          * with the op's own
                                                          * caret (deletable) */
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
 * The int index (and int rhs) are resolved CACHE-AWARE first; THEN the
 * prologue saves the cache regs and rdi is pointed at &slots[base] (the
 * base itself lives in rbx and is never disturbed). On a non-0 return (the
 * helper caught + stashed
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
static void emit_elem_bounds_or_wrap(Emitter &e, uint32_t pc,
                                    std::vector<size_t> *slows = nullptr)
{
    if (slows) {
        /* #56: DECLINE out-of-range (a negative wrap or a genuine OOB) to
         * the caller's slow tier - the interpreter core does the wrap and
         * throws CONVEYED (the raise path's loc_at would resolve against a
         * DELETED run's collapsed pcs). */
        e.cmp_r9_rdx();
        slows->push_back(e.j32(0x73));       /* jae -> slow */
        return;
    }
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
                               const Instr *idx_in, int idx_slot,
                               std::vector<size_t> *slows = nullptr)
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
    emit_elem_bounds_or_wrap(e, pc, slows);
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
static void emit_elem_int_read(Emitter &e, const Instr &in, uint32_t pc,
                               std::vector<size_t> *slows = nullptr)
{
    const JitLayout &L = jit_layout();
    const SlotAddr base = slot_addr(in.target2);
    /* C1d: the hoisted form - the region preheader proved the base and
     * pinned (data, count); bounds vs r11, read off r10 (byte for a
     * hinted bools base). Declines land on the caller's slow tier,
     * which reads MEMORY - never stale. Only emitted when a slow list
     * exists (the bail-mode callers keep the full nav). */
    if (slows && g_hoist.active && in.target2 == g_hoist.base
            && g_hoist.kind == (in.elem_bool_hint() ? 3 : 0)) {
        load_index_r9(e, in);
        e.cmp_r9_hr(g_hoist.rcount);
        slows->push_back(e.j32(0x73));           /* jae -> slow */
        if (g_hoist.kind == 3)
            e.load_elem_byte_hr(g_hoist.rdata);
        else
            e.load_elem_int_hr(g_hoist.rdata);
        return;                                  /* rax = the element */
    }
    const auto decline = [&](uint8_t pass_short, uint8_t fail_near) {
        /* #56: with a slow tier, a declined guard JUMPS there (the helper
         * runs the interpreter core); without one, the old bail. */
        if (slows)
            slows->push_back(e.j32(fail_near));
        else
            e.bail_unless(pass_short, pc);
    };
    e.load(RAX, base.type);                  /* base an array? */
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    decline(0x74, 0x75);                     /* je (== t_arr) */
    e.cmp_byte_slot(base.payload + L.slice_off, 0);   /* not a slice? */
    decline(0x74, 0x75);                     /* je (slice==0) */
    e.load(RAX, base.payload);               /* rax = shobj ptr */
    e.cmp_byte_rax(L.kind_off, L.kind_ints);
    const size_t j_ints = e.j32(0x74);       /* je -> the 8-byte path */
    e.cmp_byte_rax(L.kind_off, L.kind_bools);
    decline(0x74, 0x75);                     /* je (bools) else decline */
    /* flat bools: 1-byte elements, so the count is the raw pointer difference
     * (no sar) and the load is a movzx. */
    emit_flat_int_tail(e, pc, /*bools=*/true, &in, -1, slows);
    const size_t j_done = e.j32(0xEB);
    /* flat ints */
    e.patch32_here(j_ints);
    emit_flat_int_tail(e, pc, /*bools=*/false, &in, -1, slows);
    e.patch32_here(j_done);
}

/* The BASE GATE alone (no read): bail unless the slot holds a NON-slice flat
 * int/bool array. ForStepElemInt must run every bail-able check BEFORE it
 * steps the counter - a bail re-runs the WHOLE op, and a post-step bail would
 * DOUBLE-STEP. Clobbers rax/r9. */
static void emit_elem_base_gate(Emitter &e, int base_slot, uint32_t pc,
                                std::vector<size_t> *slows = nullptr)
{
    const JitLayout &L = jit_layout();
    const SlotAddr base = slot_addr(base_slot);
    const auto decline = [&](uint8_t pass_short, uint8_t fail_near) {
        if (slows)
            slows->push_back(e.j32(fail_near));   /* #56: -> the slow tier */
        else
            e.bail_unless(pass_short, pc);
    };
    e.load(RAX, base.type);
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    decline(0x74, 0x75);
    e.cmp_byte_slot(base.payload + L.slice_off, 0);
    decline(0x74, 0x75);
    e.load(RAX, base.payload);
    e.cmp_byte_rax(L.kind_off, L.kind_ints);
    const size_t j_ok = e.j8(0x74);
    e.cmp_byte_rax(L.kind_off, L.kind_bools);
    decline(0x74, 0x75);
    e.patch8(j_ok, e.pos());
}

/*
 * #92 - the INLINE tier for a flat int/bool element STORE.
 *
 * `a[i] = v` was a bare call to jit_store_elem_int, and that helper redoes
 * the whole managed model per store: type tag, storage kind, const +
 * readonly, negative wrap, size() TWICE, slice check, use_count(), the
 * store, invalidate_hash(). Measured at 85 Ir PER ELEMENT STORE - 66% of
 * 43_sieve, the suite's worst bench at 27.2x the C++ that writes one byte.
 *
 * So emit the guards and the store inline, as the READ side and the boxed
 * int-int arithmetic tier already do, and keep the helper as the SLOW
 * TIER. Every guard DECLINES to it rather than raising, so everything this
 * tier does not take stays byte-identical: the negative-index wrap, the
 * OOB caret, the COW clone, a readonly/const target, compound ops, floats.
 *
 * THE GUARD THAT IS NOT OBVIOUS: not `use_count() == 1`. `var a =
 * array(32)` compiles to `call.blt.v r4 = array(..)` + `move a = r4`, and
 * MoveV COPIES the handle - so a dead temp keeps the count at 2 for the
 * rest of the function and a sole-owner guard declines essentially all
 * array code (the first version of this tier was dead for exactly that
 * reason). The helper's own condition shows the real one: it calls
 * clone_aliased_slices, which ITERATES shobj->slices, so it is a NO-OP
 * when no slice VIEWS exist. A refcount > 1 with no slices is harmless -
 * two variables sharing an array is MyLang's reference semantics and a
 * plain store is correct. Hence the `has_slices` mirror.
 *
 * Registers: rax = shobj, rcx = data, rdx = count, r9 = index, rdi = the
 * value (NOT rsi - see the encoders).
 */
static bool emit_store_elem_inline(Emitter &e, const Instr &in,
                                   std::vector<size_t> &slows,
                                   std::vector<size_t> &dones,
                                   bool is_float)
{
    /*
     * #95 case 1 - COMPOUND stores `a[i] OP= v` inline too, as a
     * read-modify-write on the element. The op set is the interpreter
     * body's own (plus/minus/times/div/mod); anything else was
     * unreachable there (its default is InternalErrorEx). Refusals that
     * stay on the helper, decided at EMIT time:
     *   - float `%=` (an fmod libm call - not worth a call-bearing tail);
     *   - a LITERAL 0 / -1 divisor: the helper runs the interpreter's
     *     exact C++ (GCC may fold an imm-divisor division differently
     *     than an emitted idiv would trap - the IntModRI exclusion);
     * and at RUNTIME (a slot divisor): rdi == 0 / -1 declines, emitted
     * BEFORE the prep jumps - a div-by-zero store throws WITHOUT cloning
     * in the interpreter, so prep must not run first (`intptr`-observable).
     * A compound on a BOOL array declines too (the interpreter's fast
     * path excludes it: bool + int -> int does not fit the storage).
     */
    const Op aop = in.aop;
    const bool compound = aop != Op::invalid;
    const bool divmod = aop == Op::div || aop == Op::mod;

    /* The compound emit-time refusals FIRST (they hold for the hoisted
     * arm too - a refused shape takes the full helper either way). */
    if (compound) {
        if (aop != Op::plus && aop != Op::minus && aop != Op::times
            && !divmod)
            return false;
        if (is_float && aop == Op::mod)
            return false;                       /* fmod: helper */
        if (divmod && !is_float && in.b_is_lit()
            && (in.b_lit() == 0 || in.b_lit() == -1))
            return false;
        if (aop == Op::div && is_float && in.b_is_lit()
            && in.b_flit() == 0.0)
            return false;
    }

    /*
     * C1b: the HOISTED store - the preheader proved the base AND the
     * store guards (const/readonly/no live views) and invalidated the
     * hash once, so a plain store is a bounds check + a raw write off
     * the pinned registers, and (C1e) a COMPOUND is bounds + an RMW
     * off them: `mov rcx, r10` lets the ordinary tier's [rcx+r9*8]
     * tails serve unchanged, minus the per-element hash store (done
     * once at the preheader) and minus the whole nav. The runtime
     * divisor guards precede the bounds check exactly as the ordinary
     * tier's precede prep: a div-by-zero store must throw (in the
     * helper) without storing. Negative/OOB declines to the full
     * helper (which re-derives everything from memory - never stale).
     * A hint-3 compound never hoists: a compound on BOOL storage is
     * compile-unreachable (the store pins the base to array<int>), and
     * the ordinary tier's ints kind guard raises the exact error.
     */
    const int hoist_want = is_float ? 1
                         : in.elem_bool_hint() ? 3 : 0;
    if (g_hoist.active && g_hoist.store_ok
            && in.target2 == g_hoist.base
            && g_hoist.kind == hoist_want
            && !(compound && hoist_want == 3)) {
        if (is_float)
            emit_float_load(e, X0, in.b_is_lit(), in.b_flit(),
                            in.b_slot(), 0, /*no_bail=*/true);
        else
            load_operand(e, RDI, in.b_is_lit(), in.b_lit(), in.b_slot());
        if (compound && divmod && !in.b_is_lit() && is_float) {
            e.pxor_x1();
            e.ucomisd(X0, X1);
            const size_t j_nan = e.j8(0x7A);     /* jp -> not zero */
            slows.push_back(e.j32(0x74));        /* je (== 0.0) */
            e.patch8(j_nan, e.pos());
        }
        load_index_r9(e, in);
        e.cmp_r9_hr(g_hoist.rcount);
        slows.push_back(e.j32(0x73));            /* jae -> the helper */
        if (compound && divmod && !in.b_is_lit() && !is_float) {
            /* the int divisor gate (#103 refinement), AFTER the bounds
             * check so the cold side can read the element off the
             * pinned registers: 0 declines, and of the -1 divisors only
             * the INT_MIN dividend does - an ordinary x / -1 falls
             * through to the native RMW (which reloads the element;
             * rax is free here). */
            e.u8(0x48); e.u8(0x8D); e.u8(0x57); e.u8(0x01);
                                                 /* lea rdx,[rdi+1] */
            e.u8(0x48); e.u8(0x83); e.u8(0xFA); e.u8(0x01);
            const size_t j_ok = e.j32(0x77);     /* ja .ok (hot) */
            e.u8(0x48); e.u8(0x85); e.u8(0xFF);  /* test rdi,rdi */
            slows.push_back(e.j32(0x74));        /* 0 -> the helper */
            e.load_elem_int_hr(g_hoist.rdata);   /* rax = the element */
            e.movabs(RDX, 0x8000000000000000ull);
            e.u8(0x48); e.u8(0x39); e.u8(0xD0);  /* cmp rax,rdx */
            slows.push_back(e.j32(0x74));        /* INT_MIN -> helper */
            e.patch32_here(j_ok);
        }
#ifdef TESTS
        e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_store_fast));
        e.u8(0x48); e.u8(0xFF); e.u8(0x02);      /* the tier's counter */
#endif
        if (!compound) {
            if (is_float)
                e.store_elem_float_hr(g_hoist.rdata);
            else if (hoist_want == 3)
                e.store_elem_byte_hr(g_hoist.rdata); /* 0/1 lit -> dil */
            else
                e.store_elem_int_hr(g_hoist.rdata);
        } else {
#ifdef TESTS
            e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_hoist_rmw));
            e.u8(0x48); e.u8(0xFF); e.u8(0x02);  /* the ARM's own counter
                                                  * (g_jit_store_fast also
                                                  * counts the ordinary
                                                  * tier - it cannot prove
                                                  * THIS arm ran) */
#endif
            e.mov_rr(RCX, g_hoist.rdata);        /* the [rcx+r9*8] tails */
            if (is_float) {
                e.load_elem_float_x1();          /* xmm1 = elem */
                e.farith_x1_x0(aop == Op::plus  ? 0x58
                             : aop == Op::minus ? 0x5C
                             : aop == Op::times ? 0x59 : 0x5E);
                e.store_elem_float_x1();
            } else if (divmod) {
                e.load_elem_int();               /* rax = elem */
                e.cqo();
                e.idiv_rdi();
                if (aop == Op::div)
                    e.store_elem_int_rax();
                else
                    e.store_elem_int_rdx();      /* remainder */
            } else {
                e.load_elem_int();
                if (aop == Op::plus)       e.add_rax_rdi();
                else if (aop == Op::minus) e.sub_rax_rdi();
                else                       e.imul_rax_rdi();
                e.store_elem_int_rax();
            }
        }
        dones.push_back(e.jmp32());
        return true;
    }

    const JitLayout &L = jit_layout();
    const SlotAddr base = slot_addr(in.target2);
    const int32_t base_off = static_cast<int32_t>(
        static_cast<long>(in.target2) * static_cast<long>(sizeof(LValue)));
    const auto decline_ne = [&]() { slows.push_back(e.j32(0x75)); };
    const auto decline_if = [&](uint8_t cc) { slows.push_back(e.j32(cc)); };
    std::vector<size_t> preps;

    /*
     * The RETRY loop head. The two COW guards below jump to a PREP stub
     * (jit_store_elem_prep, the clone alone) and come back HERE, so a
     * store that needed the clone still finishes on the fast path - the
     * interpreter pays the clone once and stores raw ever after, and now
     * so does the emitted code. The loop cannot spin: prep returns 0 only
     * when jit_cow_clean() holds, which makes both COW guards pass.
     * The head re-loads the VALUE because the stub's call clobbers rdi.
     */
    const size_t retry = e.pos();
    /* the VALUE: rdi for ints (rsi is the t_int singleton), xmm0 for
     * floats (compile-proven numeric, so no_bail - int/bool promote
     * exactly as read_float_slot does). Loaded at the retry head because
     * the prep stub's call clobbers both. */
    if (is_float)
        emit_float_load(e, X0, in.b_is_lit(), in.b_flit(), in.b_slot(), 0,
                        /*no_bail=*/true);
    else
        load_operand(e, RDI, in.b_is_lit(), in.b_lit(), in.b_slot());

    e.load(RAX, base.type);                        /* an array? */
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    decline_ne();
    e.cmp_byte_slot(base.type + (L.lv_const_off - L.off_type), 0);
    decline_ne();                                            /* const slot */

    e.load(RAX, base.payload);                     /* rax = the SharedObject */
    e.cmp_byte_rax(L.ro_off, 0);
    decline_ne();                                            /* readonly */

    /*
     * The RUNTIME float divisor guard (compound div, slot rhs) - BEFORE
     * the prep jumps for the same reason prep sits after const/readonly:
     * a div-by-zero store throws WITHOUT cloning in the interpreter, so
     * the clone side effect must not run first. A decline itself is
     * always safe (the helper re-derives everything in the right order).
     * The INT guards moved INTO the compound-ints arm (below): deciding
     * the -1 case needs the ELEMENT, whose read needs the kind proven.
     */
    if (divmod && !in.b_is_lit()) {
        if (!is_float) {
            /* moved into the ints arm */
        } else {
            /* xmm0 == 0.0 declines; NaN (unordered sets ZF too) must NOT
             * - jp hops the decline first. -0.0 compares equal, matching
             * the interpreter's `rhs == 0.0`. */
            e.pxor_x1();
            e.ucomisd(X0, X1);
            const size_t j_nan = e.j8(0x7A);          /* jp -> not zero */
            decline_if(0x74);                         /* je (== 0.0) */
            e.patch8(j_nan, e.pos());
        }
    }

    /*
     * Per-kind arms. The KIND check now precedes the COW/prep guards
     * (defense in depth: prep must never clone a base the interpreter
     * would fault on without cloning - unreachable for this op's proven
     * bases today, but the ordering is the invariant, not the reach).
     * The COW guards themselves are per-arm and deliberately AFTER
     * const/readonly: the interpreter throws on those WITHOUT cloning,
     * so prepping first would detach slices for a store that never
     * happens (observable via `intptr`, which the COW tests pin as spec).
     */
    const auto cow_guards = [&]() {
        e.cmp_byte_slot(base.payload + L.slice_off, 0);  /* base a slice */
        preps.push_back(e.j32(0x75));
        e.cmp_byte_rax(L.slices_off, 0);                 /* live views */
        preps.push_back(e.j32(0x75));
    };
    const auto bump = [&]() {
#ifdef TESTS
        e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_store_fast));
        e.u8(0x48); e.u8(0xFF); e.u8(0x02);              /* inc qword */
#endif
    };
    /* The int RMW tail: the hash byte is invalidated FIRST (rax = shobj is
     * then free for the element) - safe, because past the guards nothing
     * can fault (div 0/-1 declined, bounds checked). */
    const auto int_rmw = [&]() {
        e.mov_byte_rax_imm(L.hashv_off, 0);              /* invalidate */
        switch (aop) {
        case Op::invalid:
            e.store_elem_int_rdi();
            return;
        case Op::div: case Op::mod:
            e.load_elem_int();                           /* rax = elem */
            e.cqo();
            e.idiv_rdi();
            if (aop == Op::div)
                e.store_elem_int_rax();
            else
                e.store_elem_int_rdx();                  /* remainder */
            return;
        default:
            e.load_elem_int();
            if (aop == Op::plus)       e.add_rax_rdi();
            else if (aop == Op::minus) e.sub_rax_rdi();
            else                       e.imul_rax_rdi();
            e.store_elem_int_rax();
            return;
        }
    };

    if (is_float) {
        /* --- FLOATS: one kind, one 8-byte tail --- */
        e.cmp_byte_rax(L.kind_off, L.kind_floats);
        decline_ne();
        cow_guards();
        e.mov_rcx_rax(L.data_off);
        e.mov_rdx_rax(L.data_off + 8);
        e.sub_rdx_rcx();
        e.sar_rdx_3();
        load_index_r9(e, in);
        e.cmp_r9_rdx();
        slows.push_back(e.j32(0x73));
        bump();
        e.mov_byte_rax_imm(L.hashv_off, 0);
        if (!compound) {
            e.store_elem_float_x0();           /* movsd [rcx+r9*8], xmm0 */
        } else {
            e.load_elem_float_x1();            /* xmm1 = elem */
            e.farith_x1_x0(aop == Op::plus  ? 0x58
                         : aop == Op::minus ? 0x5C
                         : aop == Op::times ? 0x59 : 0x5E);
            e.store_elem_float_x1();
        }
        dones.push_back(e.jmp32());
        /* fall through to the shared PREP stub below */
    } else if (compound) {
        /* --- compound INTS only (bools decline: bool+int -> int does
         * not fit the storage; the helper raises the exact error) --- */
        e.cmp_byte_rax(L.kind_off, L.kind_ints);
        decline_ne();
        /*
         * The int divisor gate (#103 refinement) - AFTER the kind proof
         * (the cold side reads the ELEMENT) and BEFORE the prep jumps
         * (a div-by-zero/overflow store throws WITHOUT cloning). ONE
         * hot branch: rdi+1 unsigned <= 1 catches 0 and -1; the cold
         * side declines 0, then derives data/count/index (the same nav
         * the arm re-derives later - rcx/rdx/r9 are all re-computed),
         * declines OOB (the helper wraps negatives), reads the element
         * and declines ONLY the INT_MIN dividend - an ordinary x / -1
         * jumps back and stores natively.
         */
        if (divmod && !in.b_is_lit()) {
            e.u8(0x48); e.u8(0x8D); e.u8(0x57); e.u8(0x01);
                                                 /* lea rdx,[rdi+1] */
            e.u8(0x48); e.u8(0x83); e.u8(0xFA); e.u8(0x01);
            const size_t j_ok = e.j32(0x77);     /* ja .ok (hot) */
            e.u8(0x48); e.u8(0x85); e.u8(0xFF);  /* test rdi,rdi */
            decline_if(0x74);                    /* 0 -> the helper */
            e.mov_rcx_rax(L.data_off);
            e.mov_rdx_rax(L.data_off + 8);
            e.sub_rdx_rcx();
            e.sar_rdx_3();
            load_index_r9(e, in);
            e.cmp_r9_rdx();
            decline_if(0x73);                    /* OOB/neg -> helper */
            e.u8(0x4A); e.u8(0x8B); e.u8(0x14); e.u8(0xC9);
                                                 /* mov rdx,[rcx+r9*8] */
            e.movabs_r9(0x8000000000000000ull);
            e.u8(0x4C); e.u8(0x39); e.u8(0xCA);  /* cmp rdx,r9 */
            decline_if(0x74);                    /* INT_MIN -> helper */
            e.patch32_here(j_ok);
        }
        cow_guards();
        e.mov_rcx_rax(L.data_off);
        e.mov_rdx_rax(L.data_off + 8);
        e.sub_rdx_rcx();
        e.sar_rdx_3();
        load_index_r9(e, in);
        e.cmp_r9_rdx();
        slows.push_back(e.j32(0x73));
        bump();
        int_rmw();
        dones.push_back(e.jmp32());
    } else {
    e.cmp_byte_rax(L.kind_off, L.kind_ints);
    const size_t j_ints = e.j32(0x74);
    e.cmp_byte_rax(L.kind_off, L.kind_bools);
    decline_ne();

    /* --- BOOLS: 1-byte elements, count = finish - start --- */
    cow_guards();
    e.mov_rcx_rax(L.data_off);
    e.mov_rdx_rax(L.data_off + 8);
    e.sub_rdx_rcx();
    load_index_r9(e, in);
    e.cmp_r9_rdx();
    slows.push_back(e.j32(0x73));        /* jae: negative OR >= count */
    bump();
    e.store_elem_byte_dil();
    e.mov_byte_rax_imm(L.hashv_off, 0);             /* invalidate_hash() */
    dones.push_back(e.jmp32());

    /* --- INTS: 8-byte elements, count = (finish - start) / 8 --- */
    e.patch32_here(j_ints);
    cow_guards();
    e.mov_rcx_rax(L.data_off);
    e.mov_rdx_rax(L.data_off + 8);
    e.sub_rdx_rcx();
    e.sar_rdx_3();
    load_index_r9(e, in);
    e.cmp_r9_rdx();
    slows.push_back(e.j32(0x73));
    bump();
    e.store_elem_int_rdi();
    e.mov_byte_rax_imm(L.hashv_off, 0);
    dones.push_back(e.jmp32());
    }                                          /* end of the int/bool arm */

    /*
     * The PREP stub: rdi = &slots[base], rsi = the index (cache-aware -
     * a pinned loop counter reads its register; the prologue pushes the
     * cache regs but they still hold their values inside the bracket).
     * Nonzero -> the full helper; zero -> normalized, RETRY the fast path.
     */
    for (const size_t j : preps)
        e.patch32_here(j);
    emit_call_prologue(e);
    e.lea_rdi(base_off);
    load_index_r9(e, in);
    e.mov_rsi_r9();
    e.call_relocs.push_back(
        { e.pos(), reinterpret_cast<const void *>(jit_store_elem_prep) });
    e.u8(0xE8); e.u32(0);
    emit_call_epilogue(e);
    e.u8(0x85); e.u8(0xC0);                    /* test eax, eax */
    slows.push_back(e.j32(0x75));              /* jnz -> the full helper */
    e.jmp32_to(retry);
    return true;
}

/*
 * #93 - the INLINE tier for the fused nested READ `dst = base[i][j]`
 * (LoadElem2Int; the float twin stays helper-only for now).
 *
 * The helper redoes both levels of the managed model per read - ~87 Ir
 * where -O3 C++ uses one mov (profiled at 22%+ of 46_matrix_mult's total,
 * with EvalValue::operator= adding another 11% for the dst boxing). The
 * fast shape emits the two navigations directly:
 *
 *   OUTER  an array, NOT a slice, GENERAL storage (a matrix's rows are
 *          boxed LValues at stride sizeof(LValue)); index in range by an
 *          unsigned BYTE-length compare (idx*48 < finish-start), which
 *          also catches a negative index.
 *   ROW    the LValue at data + idx*48: an array, flat INT storage
 *          (strs/general decline; the helper serves them
 *          byte-identically, including the per-level OOB carets from
 *          the baked chain_locs pair). #95 grew the arms: a SLICE
 *          outer/row takes its offset-aware arm (bounds = the handle's
 *          LEN, elements at data + off + i), and an INT row under the
 *          FLOAT op takes the cvtsi2sd promote arm.
 *   INNER  bounds via the shared count compare, then mov rax,[rcx+r9*8],
 *          then the ref-aware store_dst (the same dst path every int
 *          producer uses).
 *
 * Registers: rax scratch/element, rcx walks outer-data -> row -> inner
 * data, rdx byte-length/count, r9 the two indexes then the t_arr
 * immediate. rsi (t_int) and r8 (t_float) are RESERVED; no call on the
 * fast path, so nothing needs saving.
 */
static void emit_load_elem2_inline(Emitter &e, const Chunk &ck,
                                   const Instr &in, uint32_t pc,
                                   std::vector<size_t> &slows,
                                   std::vector<size_t> &dones,
                                   bool is_float)
{
    const JitLayout &L = jit_layout();
    const SlotAddr base = slot_addr(in.target2);
    const auto decline_ne = [&]() { slows.push_back(e.j32(0x75)); };
    /* lever A producer (the int tails; the CALLER reloads RAX on the
     * slow-path rejoin and arms the state - see the LoadElem2 case) */
    const bool fw = !is_float && g_fwd.prod == in.target;
    const auto dst_write = [&]() {
        if (fw && g_fwd.skip_write)
            return;
        store_dst(e, ck, RAX, in.target, pc, fw);
    };

    /* OUTER: array, general storage; a SLICE outer takes its arm (#95).
     * C1: a HOISTED outer skips the whole navigation - the entry
     * proved array/non-slice/general and pinned (data, BYTE length). */
    size_t j_oslice = SIZE_MAX;
    const bool hoisted = g_hoist.active && in.target2 == g_hoist.base
        && g_hoist.kind == 2;
    if (hoisted) {
        load_slot_r9(e, in.a_dual_lo());
        e.imul_r9_imm8(static_cast<uint8_t>(sizeof(LValue)));
        e.cmp_r9_hr(g_hoist.rcount);           /* vs the BYTE length */
        slows.push_back(e.j32(0x73));          /* jae: negative OR OOB */
        e.mov_rr(RCX, g_hoist.rdata);
        e.add_rcx_r9();                        /* rcx = &row (LValue) */
    } else {
    e.load(RAX, base.type);
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    decline_ne();
    e.cmp_byte_slot(base.payload + L.slice_off, 0);
    j_oslice = e.j32(0x75);                    /* -> the outer-slice arm */
    e.load(RAX, base.payload);                 /* outer SharedObject */
    e.cmp_byte_rax(L.kind_off, L.kind_general);
    decline_ne();

    /* the row: data + oidx * sizeof(LValue), bounds by byte length */
    e.mov_rcx_rax(L.data_off);
    e.mov_rdx_rax(L.data_off + 8);
    e.sub_rdx_rcx();                           /* rdx = byte length */
    load_slot_r9(e, in.a_dual_lo());           /* the outer index (slot) */
    e.imul_r9_imm8(static_cast<uint8_t>(sizeof(LValue)));
    e.cmp_r9_rdx();
    slows.push_back(e.j32(0x73));              /* jae: negative OR OOB */
    e.add_rcx_r9();                            /* rcx = &row (LValue) */
    }

    /* ROW: an array; a SLICE row takes its arm (#95); the arms below
     * rejoin here, so `row_sec` is the common &row entry. */
    const size_t row_sec = e.pos();
    e.mov_rax_rcx_d(static_cast<int32_t>(L.off_type));
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    decline_ne();
    e.cmp_byte_rcx(static_cast<int32_t>(L.off_payload) + L.slice_off, 0);
    const size_t j_rslice = e.j32(0x75);       /* -> the row-slice arm */
    e.mov_rax_rcx_d(static_cast<int32_t>(L.off_payload));  /* row shobj */

    const auto inner_idx_r9 = [&]() {
        if (in.b_is_lit())
            e.movabs_r9(static_cast<uint64_t>(in.b_lit()));
        else
            load_slot_r9(e, in.b_slot());
    };
    const auto count_and_idx = [&](bool bytes) {
        e.mov_rcx_rax(L.data_off);
        e.mov_rdx_rax(L.data_off + 8);
        e.sub_rdx_rcx();
        if (!bytes)
            e.sar_rdx_3();
        inner_idx_r9();
        e.cmp_r9_rdx();
        slows.push_back(e.j32(0x73));
    };
    const auto bump = [&]() {
#ifdef TESTS
        e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_elem2_fast));
        e.u8(0x48); e.u8(0xFF); e.u8(0x02);    /* inc qword [rdx] */
#endif
    };

    if (is_float) {
        /* FLOAT rows, and (#95 case 3) an INT row PROMOTES via cvtsi2sd
         * - the helper's own behaviour (the mixed-rows shape:
         * `[[1.0,2.0],[3,4]]` joins to float while one row's storage
         * stays flat INT). */
        e.cmp_byte_rax(L.kind_off, L.kind_floats);
        const size_t j_frow = e.j32(0x74);
        e.cmp_byte_rax(L.kind_off, L.kind_ints);
        decline_ne();
        count_and_idx(/*bytes=*/false);
        e.cvtsi2sd_x0_elem();                  /* xmm0 = (double)elem */
        bump();
        emit_float_store(e, ck, X0, in.target, pc);
        dones.push_back(e.jmp32());
        e.patch32_here(j_frow);
        count_and_idx(/*bytes=*/false);
        e.load_elem_float();                   /* xmm0 = [rcx + r9*8] */
        bump();
        emit_float_store(e, ck, X0, in.target, pc);
        dones.push_back(e.jmp32());
        return;
    }

    /* INT semantics accept flat ints AND flat bools, exactly as the
     * single-level emit_elem_int_read does (a bool node is stamped `i`).
     * A bool row stores ONE byte per element - byte count, byte read. */
    e.cmp_byte_rax(L.kind_off, L.kind_ints);
    const size_t j_ints = e.j32(0x74);
    e.cmp_byte_rax(L.kind_off, L.kind_bools);
    decline_ne();
    count_and_idx(/*bytes=*/true);
    e.load_elem_byte();                        /* movzx eax, [rcx + r9] */
    bump();
    dst_write();
    dones.push_back(e.jmp32());

    e.patch32_here(j_ints);
    count_and_idx(/*bytes=*/false);
    e.load_elem_int();                         /* rax = [rcx + r9*8] */
    bump();
    dst_write();
    dones.push_back(e.jmp32());

    /*
     * #95 case 4 - the OUTER-SLICE arm: a slice of a general array (its
     * rows live at data + (off + oidx) * 48, bounds are the slice's
     * LEN). Rejoins the common row section, so a slice-of-slices shape
     * (slice outer AND slice row) composes with the row arm below.
     */
    if (j_oslice != SIZE_MAX) {
    e.patch32_here(j_oslice);
    e.load(RAX, base.payload);
    e.cmp_byte_rax(L.kind_off, L.kind_general);
    decline_ne();
    e.mov_rcx_rax(L.data_off);                 /* data, before rax dies */
    e.mov_edx_slot(base.payload + L.arr_len_off);
    load_slot_r9(e, in.a_dual_lo());
    e.cmp_r9_rdx();
    slows.push_back(e.j32(0x73));              /* jae: negative OR OOB */
    e.mov_eax_slot(base.payload + L.arr_off_off);
    e.add_r9_rax();                            /* oidx += off */
    e.imul_r9_imm8(static_cast<uint8_t>(sizeof(LValue)));
    e.add_rcx_r9();                            /* rcx = &row */
    e.jmp32_to(row_sec);
    }

    /*
     * The ROW-SLICE arm: the row's elements live at its shobj's data +
     * (off + iidx), bounds its LEN. Strict kinds only (float op: float
     * rows; int op: int rows - no bool tail, no promote in the slice
     * arm; those decline to the helper).
     */
    e.patch32_here(j_rslice);
    e.mov_rax_rcx_d(static_cast<int32_t>(L.off_payload));  /* row shobj */
    e.cmp_byte_rax(L.kind_off,
                   is_float ? L.kind_floats : L.kind_ints);
    decline_ne();
    e.mov_edx_rcx(static_cast<int32_t>(L.off_payload) + L.arr_len_off);
    inner_idx_r9();
    e.cmp_r9_rdx();
    slows.push_back(e.j32(0x73));
    e.mov_edx_rcx(static_cast<int32_t>(L.off_payload) + L.arr_off_off);
    e.add_r9_rdx();                            /* iidx += off */
    e.mov_rcx_rax(L.data_off);                 /* rcx = row data */
#ifdef TESTS
    e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_elem_slice_fast));
    e.u8(0x48); e.u8(0xFF); e.u8(0x02);        /* inc qword [rdx] */
#endif
    if (is_float) {
        e.load_elem_float();
        emit_float_store(e, ck, X0, in.target, pc);
    } else {
        e.load_elem_int();
        dst_write();
    }
    dones.push_back(e.jmp32());
}

/*
 * #95 case 2 - the INLINE tier for the nested STORE `a[i][j] = v` /
 * `a[i][j] OP= v` (StoreElem2V). The read side (#93) got its tier; the
 * store never did - every nested store paid the full helper, which
 * redoes both levels of the managed model per element.
 *
 * The fast shape: k1/k2 INT slots, OUTER a non-slice non-readonly
 * GENERAL array (rows are boxed LValues at stride 48), ROW a non-const
 * non-readonly flat int/bool/float array, the VALUE fitting the row's
 * kind (int row: t_int; bool row: t_bool; float row: t_float, or t_int
 * which PROMOTES via cvtsi2sd - the interpreter's own float-arm cast).
 * A BOOL value on an int/float row declines to the helper, which
 * #96-widens it (0/1) - the inline arms keep the narrow guards.
 * The row's COW pair (slice flag / live views) goes to the SHARED prep
 * (jit_store_elem_prep on &row - the row LValue address is stable
 * across the clone, but the retry re-derives it anyway).
 *
 * GUARD ORDER is the load-bearing part, same invariant as the
 * single-level tier: every condition the interpreter throws on WITHOUT
 * cloning must be checked (or declined) BEFORE the prep jumps - row
 * const/readonly, the row KIND (a structs row throws its type error
 * before any clone), the value-FIT (flat_store_core checks `fits`
 * before COW), and a compound div/mod ZERO divisor (apply_compound_op
 * throws before the clone). OOB is safe by construction: prep
 * bounds-checks in C++ before cloning.
 *
 * Compound: the Expr14 op maps to the base op; the int arm does the
 * same RMW as the single-level tier with the same 0/-1 runtime divisor
 * declines; the FLOAT arm refuses div/mod at emit time (the zero test
 * on a maybe-promoted boxed value is not worth the code - the helper
 * serves it); a BOOL row refuses any compound (bool + int -> int does
 * not fit, the interpreter's own exclusion).
 *
 * Registers: rax outer-type/outer-shobj/row-type/row-shobj, rcx
 * outer-data -> &row (alive until the COW guards are done - prep needs
 * it) -> inner data, rdx val-type guard then byte-length/count, r9
 * t_arr/t_bool immediates + both indexes, rdi the int/bool value,
 * xmm0/xmm1 the float value/element. rsi (t_int) and r8 (t_float) are
 * RESERVED reads; the prep stub's epilogue re-materialises them.
 */
static bool emit_store_elem2_inline(Emitter &e, const Instr &in,
                                    std::vector<size_t> &slows,
                                    std::vector<size_t> &dones)
{
    /* the Expr14 op -> the base op (Op::invalid == plain assign) */
    Op bop;
    switch (in.aop) {
    case Op::assign: bop = Op::invalid; break;
    case Op::addeq:  bop = Op::plus;    break;
    case Op::subeq:  bop = Op::minus;   break;
    case Op::muleq:  bop = Op::times;   break;
    case Op::diveq:  bop = Op::div;     break;
    case Op::modeq:  bop = Op::mod;     break;
    default: return false;                     /* helper-only */
    }
    const bool compound = bop != Op::invalid;
    const bool divmod = bop == Op::div || bop == Op::mod;

    const JitLayout &L = jit_layout();
    const SlotAddr base = slot_addr(in.target2);
    const SlotAddr k1 = slot_addr(in.a_dual_lo());
    const SlotAddr k2 = slot_addr(in.b_slot());
    const SlotAddr val = slot_addr(in.target);
    const auto decline_ne = [&]() { slows.push_back(e.j32(0x75)); };
    const auto decline_if = [&](uint8_t cc) { slows.push_back(e.j32(cc)); };
    std::vector<size_t> preps;

    const size_t retry = e.pos();
    /* both keys must be plain ints (boxed slots - the interpreter's
     * "Expected integer as subscript" declines to the helper) */
    e.load(RAX, k1.type);
    e.cmp_rax_rsi();
    decline_ne();
    e.load(RAX, k2.type);
    e.cmp_rax_rsi();
    decline_ne();

    /* OUTER: an array, not a slice, not readonly, GENERAL storage */
    e.load(RAX, base.type);
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    decline_ne();
    e.cmp_byte_slot(base.payload + L.slice_off, 0);
    decline_ne();
    e.load(RAX, base.payload);                 /* outer SharedObject */
    e.cmp_byte_rax(L.ro_off, 0);
    decline_ne();                              /* readonly outer: rvalue */
    e.cmp_byte_rax(L.kind_off, L.kind_general);
    decline_ne();

    /* the row: data + k1 * sizeof(LValue), bounds by byte length */
    e.mov_rcx_rax(L.data_off);
    e.mov_rdx_rax(L.data_off + 8);
    e.sub_rdx_rcx();
    load_slot_r9(e, in.a_dual_lo());
    e.imul_r9_imm8(static_cast<uint8_t>(sizeof(LValue)));
    e.cmp_r9_rdx();
    slows.push_back(e.j32(0x73));              /* jae: negative OR OOB */
    e.add_rcx_r9();                            /* rcx = &row (LValue) */

    /* ROW: an array, not const, not readonly */
    e.mov_rax_rcx_d(static_cast<int32_t>(L.off_type));
    e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
    e.cmp_rax_r9();
    decline_ne();
    e.cmp_byte_rcx(L.lv_const_off, 0);
    decline_ne();
    e.mov_rax_rcx_d(static_cast<int32_t>(L.off_payload));  /* row shobj */
    e.cmp_byte_rax(L.ro_off, 0);
    decline_ne();

    /* the row's COW pair -> prep; every arm shares one stub */
    const auto cow_guards = [&]() {
        e.cmp_byte_rcx(static_cast<int32_t>(L.off_payload) + L.slice_off, 0);
        preps.push_back(e.j32(0x75));
        e.cmp_byte_rax(L.slices_off, 0);
        preps.push_back(e.j32(0x75));
    };
    const auto bump = [&]() {
#ifdef TESTS
        e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_store2_fast));
        e.u8(0x48); e.u8(0xFF); e.u8(0x02);
#endif
    };
    /* inner data/count/index/bounds (clobbers rcx - after cow_guards) */
    const auto inner_bounds = [&](bool bytes) {
        e.mov_rcx_rax(L.data_off);
        e.mov_rdx_rax(L.data_off + 8);
        e.sub_rdx_rcx();
        if (!bytes)
            e.sar_rdx_3();
        load_slot_r9(e, in.b_slot());
        e.cmp_r9_rdx();
        slows.push_back(e.j32(0x73));
    };

    /* per-kind arms; kind dispatch BEFORE the COW guards (see above) */
    e.cmp_byte_rax(L.kind_off, L.kind_ints);
    const size_t j_ints = e.j32(0x74);
    if (!compound) {
        e.cmp_byte_rax(L.kind_off, L.kind_bools);
        const size_t j_bools = e.j32(0x74);
        e.cmp_byte_rax(L.kind_off, L.kind_floats);
        decline_ne();

        /* --- FLOAT row, plain: value promotes like the interpreter --- */
        e.load(RDX, val.type);
        e.cmp_rdx_r8();                        /* t_float? */
        const size_t j_vf = e.j8(0x74);
        e.cmp_rdx_rsi();                       /* t_int? (promote) */
        decline_ne();
        e.cvt(X0, val.payload);                /* cvtsi2sd xmm0, [val] */
        const size_t j_vgot = e.j8(0xEB);
        e.patch8(j_vf, e.pos());
        e.fload(X0, val.payload);              /* movsd xmm0, [val] */
        e.patch8(j_vgot, e.pos());
        cow_guards();
        inner_bounds(/*bytes=*/false);
        bump();
        e.store_elem_float_x0();
        e.mov_byte_rax_imm(L.hashv_off, 0);
        dones.push_back(e.jmp32());

        /* --- BOOL row, plain: value must BE a bool (an int does not
         * fit - the interpreter's own rule); payload is already 0/1 --- */
        e.patch32_here(j_bools);
        e.load(RDX, val.type);
        e.movabs_r9(reinterpret_cast<uint64_t>(L.t_bool));
        e.cmp_rdx_r9();
        decline_ne();
        e.load(RDI, val.payload);
        cow_guards();
        inner_bounds(/*bytes=*/true);
        bump();
        e.store_elem_byte_dil();
        e.mov_byte_rax_imm(L.hashv_off, 0);
        dones.push_back(e.jmp32());
    } else if (!divmod) {
        /* compound float rows: add/sub/mul only (div/mod: the zero test
         * on a maybe-promoted boxed value stays on the helper) */
        e.cmp_byte_rax(L.kind_off, L.kind_floats);
        const size_t j_floats = e.j32(0x74);
        decline_if(0xEB);                      /* other kinds: helper */
        e.patch32_here(j_floats);
        e.load(RDX, val.type);
        e.cmp_rdx_r8();
        const size_t j_vf = e.j8(0x74);
        e.cmp_rdx_rsi();
        decline_ne();
        e.cvt(X0, val.payload);
        const size_t j_vgot = e.j8(0xEB);
        e.patch8(j_vf, e.pos());
        e.fload(X0, val.payload);
        e.patch8(j_vgot, e.pos());
        cow_guards();
        inner_bounds(/*bytes=*/false);
        bump();
        e.mov_byte_rax_imm(L.hashv_off, 0);
        e.load_elem_float_x1();                /* xmm1 = elem */
        e.farith_x1_x0(bop == Op::plus  ? 0x58
                     : bop == Op::minus ? 0x5C : 0x59);
        e.store_elem_float_x1();
        dones.push_back(e.jmp32());
    } else {
        decline_if(0xEB);                      /* div/mod: ints only */
    }

    /* --- INT row (plain or compound) --- */
    e.patch32_here(j_ints);
    e.load(RDX, val.type);
    e.cmp_rdx_rsi();
    decline_ne();
    e.load(RDI, val.payload);
    if (divmod) {
        /* the int divisor gate (#103 refinement): the row's kind is
         * proven ints here and rax = the row's shobj, so the cold side
         * derives data/count/index (inner_bounds' own sequence -
         * rcx/rdx/r9 are re-derived by the arm anyway), declines OOB,
         * reads the element and declines ONLY the INT_MIN dividend;
         * an ordinary x / -1 jumps back and stores natively. */
        /* NOTE rcx holds &row here and must SURVIVE until the cow
         * guards - the first version clobbered it via the shared nav
         * shape (a misaligned-Type* crash in -rt). This cold path uses
         * rdx/r9 only: &elem formed by lea, bounds as a TWO-sided
         * pointer compare (a negative index wraps below data). */
        e.u8(0x48); e.u8(0x8D); e.u8(0x57); e.u8(0x01);
                                                 /* lea rdx,[rdi+1] */
        e.u8(0x48); e.u8(0x83); e.u8(0xFA); e.u8(0x01);
        const size_t j_ok = e.j32(0x77);         /* ja .ok (hot) */
        e.u8(0x48); e.u8(0x85); e.u8(0xFF);      /* test rdi,rdi */
        decline_if(0x74);                        /* 0 -> the helper */
        e.mov_rdx_rax(L.data_off);               /* rdx = data */
        load_slot_r9(e, in.b_slot());
        e.u8(0x4A); e.u8(0x8D); e.u8(0x14); e.u8(0xCA);
                                                 /* lea rdx,[rdx+r9*8] */
        e.u8(0x48); e.u8(0x3B); e.u8(0x50);
        e.u8(static_cast<uint8_t>(L.data_off));  /* cmp rdx,[rax+data] */
        decline_if(0x72);                        /* jb: negative idx */
        e.u8(0x48); e.u8(0x3B); e.u8(0x50);
        e.u8(static_cast<uint8_t>(L.data_off + 8));
        decline_if(0x73);                        /* jae: OOB */
        e.u8(0x48); e.u8(0x8B); e.u8(0x12);      /* mov rdx,[rdx] */
        e.movabs_r9(0x8000000000000000ull);
        e.u8(0x4C); e.u8(0x39); e.u8(0xCA);      /* cmp rdx,r9 */
        decline_if(0x74);                        /* INT_MIN -> helper */
        e.patch32_here(j_ok);
    }
    cow_guards();
    inner_bounds(/*bytes=*/false);
    bump();
    e.mov_byte_rax_imm(L.hashv_off, 0);
    switch (bop) {
    case Op::invalid:
        e.store_elem_int_rdi();
        break;
    case Op::div: case Op::mod:
        e.load_elem_int();
        e.cqo();
        e.idiv_rdi();
        if (bop == Op::div)
            e.store_elem_int_rax();
        else
            e.store_elem_int_rdx();
        break;
    default:
        e.load_elem_int();
        if (bop == Op::plus)       e.add_rax_rdi();
        else if (bop == Op::minus) e.sub_rax_rdi();
        else                       e.imul_rax_rdi();
        e.store_elem_int_rax();
        break;
    }
    dones.push_back(e.jmp32());

    /* the shared PREP stub: rdi = &row, rsi = the inner index */
    for (const size_t j : preps)
        e.patch32_here(j);
    emit_call_prologue(e);
    e.mov_rdi_rcx();                           /* &row (still live here) */
    load_slot_r9(e, in.b_slot());
    e.mov_rsi_r9();
    e.call_relocs.push_back(
        { e.pos(), reinterpret_cast<const void *>(jit_store_elem_prep) });
    e.u8(0xE8); e.u32(0);
    emit_call_epilogue(e);
    e.u8(0x85); e.u8(0xC0);                    /* test eax, eax */
    slows.push_back(e.j32(0x75));              /* jnz -> the full helper */
    e.jmp32_to(retry);
    return true;
}

static void emit_store_elem(Emitter &e, const Chunk &ck, const Instr &in,
                            uint32_t pc, size_t old_pc, bool is_float)
{
    const void *fn = is_float
        ? reinterpret_cast<const void *>(jit_store_elem_float)
        : reinterpret_cast<const void *>(jit_store_elem_int);
    const int32_t base_off = static_cast<int32_t>(
        static_cast<long>(in.target2) * static_cast<long>(sizeof(LValue)));

    /* #92/#94: the INLINE fast tier first; every guard it fails jumps
     * here, to the helper, which is the exact interpreter semantics. */
    std::vector<size_t> slows, dones;
    if (emit_store_elem_inline(e, in, slows, dones, is_float))
        for (const size_t j : slows)
            e.patch32_here(j);

    /* idx -> rsi (an int operand), read before the cache regs spill */
    load_operand(e, RSI, in.a_is_lit(), in.a_lit(), in.a_slot());
    /* value: int -> rdx; float -> xmm0 (may BAIL on a non-numeric tag, as
     * everywhere in the float tier - the value is proven numeric, so it
     * won't in practice) */
    if (is_float)
        /* no_bail: the value is compile-proven float (int/bool promote in
         * the 2-way form exactly as read_float_slot does) - the store op's
         * ONLY exit was this load's bail, which kept it non-deletable. */
        emit_float_load(e, X0, in.b_is_lit(), in.b_flit(), in.b_slot(), pc,
                        /*no_bail=*/true);
    else
        load_operand(e, RDX, in.b_is_lit(), in.b_lit(), in.b_slot());

    emit_call_prologue(e);               /* save the cache, 16-align */
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
    for (const size_t j : dones)          /* #92: the inline tails rejoin */
        e.patch32_here(j);
}

/* Approach A: a dict element store d[k] = v as a CALL to jit_dict_store. The
 * key/value are BOXED EvalValues in frame slots, so the fragment just leas
 * their addresses (EvalValue is the first LValue member). SysV: rdi=base
 * LValue*, rsi=key EvalValue*, rdx=val EvalValue*, rcx=op. The key/val/base
 * slots are disqualified
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
    e.lea(RSI, off(in.a_slot()));         /* rsi = &slot[key]  (rbx=slots) */
    e.lea(RDX, off(in.b_slot()));         /* rdx = &slot[val]  (rbx=slots) */
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
        /* lever A, the CONSUMER side: a forwarded operand is already in
         * RAX. A commutative op with the `b` operand forwarded SWAPS the
         * roles (rax = b OP a == a OP b) instead of moving RAX aside -
         * the swap costs nothing where the move-aside cost the saved
         * load back (measured +2 Ir/iter on 46_matrix_mult, the first
         * version's whole yield inverted). Only sub - non-commutative -
         * pays the move. */
        const int fin = g_fwd.in_rax;
        const bool fa = fin >= 0 && in.a_slot() == fin;
        const bool fb = fin >= 0 && !in.b_is_lit() && in.b_slot() == fin;
        if (fa || fb)
            emit_fwd_bump(e);
        if (fa && fb) {
            e.mov_rr(RCX, RAX);            /* t OP t */
        } else if (fb && aop == Op::minus) {
            e.mov_rr(RCX, RAX);
            read_slot(e, RAX, in.a_slot());
        } else if (fb) {
            read_slot(e, RCX, in.a_slot());   /* commutative swap */
        } else if (fa) {
            load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        } else {
            read_slot(e, RAX, in.a_slot());
            load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        }
        op_rr(e, aop);
        /* lever A, the PRODUCER side: elide a dead temp's write; a kept
         * (ref-listed) write reloads RAX in store_dst's COLD arm only. */
        const bool fw = g_fwd.prod == in.target;
        if (!(fw && g_fwd.skip_write))
            write_slot(e, ck, RAX, in.target, pc, fw);
        if (fw)
            g_fwd.armed = true;
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
         * interpreted throw), then the #103 INT_MIN/-1 pre-check (RAISES
         * InvalidValueEx via JR_DIV_OVF - the interpreter throws too, so
         * idiv can never trap), then cqo+idiv; the
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
            /*
             * The edge-divisor checks (#103): 0 raises DivisionByZeroEx,
             * -1 with an INT_MIN dividend raises InvalidValueEx (idiv
             * would trap with a hardware #DE - the language THROWS via
             * EXPLICIT pre-checks, never a signal handler). A LITERAL
             * divisor decides at COMPILE time: an ordinary one emits NO
             * runtime checks at all (the first #103 version - and the
             * pre-#103 code - ran the zero test even on literals); a
             * SLOT divisor pays ONE hot-path gate for both edges:
             * rcx+1 unsigned <= 1 catches 0 and -1 in one cmp+ja, and
             * the cold block (0 -> DIV0; -1: the INT_MIN compare)
             * FALLS THROUGH into the division for a legitimate x / -1.
             * Hot cost vs the pre-#103 zero-only test: ONE instruction
             * (the lea). Measured: the first version's second
             * compare+branch read +3.4%/+4.8% on 03/44; this shape
             * halves it.
             */
            if (in.b_is_lit()) {
                if (in.b_lit() == 0) {
                    /* always-throws: test on the loaded literal */
                    e.u8(0x48); e.u8(0x85); e.u8(0xC9);  /* test rcx,rcx */
                    raise_convey_unless(e, ck, 0x75, JR_DIV0, pc, old_pc);
                } else if (in.b_lit() == -1) {
                    e.movabs(RDX, 0x8000000000000000ull);
                    e.u8(0x48); e.u8(0x39); e.u8(0xD0);  /* cmp rax,rdx */
                    raise_convey_unless(e, ck, 0x75 /* jne */, JR_DIV_OVF,
                                        pc, old_pc);
                }
                /* any other literal: nothing can fault */
            } else {
                e.u8(0x48); e.u8(0x8D); e.u8(0x51); e.u8(0x01);
                                                     /* lea rdx,[rcx+1] */
                e.u8(0x48); e.u8(0x83); e.u8(0xFA); e.u8(0x01);
                                                     /* cmp rdx,1 */
                const size_t j_div = e.j32(0x77);    /* ja .div (hot) */
                /* cold: rcx is 0 or -1 */
                e.u8(0x48); e.u8(0x85); e.u8(0xC9);  /* test rcx,rcx */
                raise_convey_unless(e, ck, 0x75, JR_DIV0, pc, old_pc);
                e.movabs(RDX, 0x8000000000000000ull);
                e.u8(0x48); e.u8(0x39); e.u8(0xD0);  /* cmp rax,rdx */
                raise_convey_unless(e, ck, 0x75 /* jne */, JR_DIV_OVF,
                                    pc, old_pc);
                /* rcx == -1, rax != INT_MIN: fall into the division */
                e.patch32_here(j_div);
            }
            e.u8(0x48); e.u8(0x99);              /* cqo */
            e.u8(0x48); e.u8(0xF7); e.u8(0xF9);  /* idiv rcx */
            write_slot(e, ck, in.aop == Op::div ? RAX : RDX, in.target, pc);
            return true;
        case Op::shl: case Op::shr: case Op::ushr:
            emit_reg_shift(e, ck, in.aop, pc, old_pc);
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
            emit_reg_shift(e, ck, shl ? Op::shl : Op::shr, pc, old_pc);
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
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc,
                        /*no_bail=*/true);
        emit_float_load(e, X1, in.b_is_lit(), in.b_flit(), in.b_slot(), pc,
                        /*no_bail=*/true);
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
            raise_convey_unless(e, ck, 0x75 /* jnz */, JR_DIV0, pc, old_pc);
        }
        if (is_mod)
            /* the exact libm call TypeFloat::mod makes - x in xmm0, y in
             * xmm1 (the SysV float args); the prologue/epilogue inside
             * save any pinned cache regs. */
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
                            in.a_slot(), pc, /*no_bail=*/true);
            if (fn == MathFn::pow_)
                emit_float_load(e, X1, in.b_is_lit(), in.b_flit(),
                                in.b_slot(), pc, /*no_bail=*/true);
            emit_libm_call(e, jit_math_fn_ptr(fn));
            emit_float_store(e, ck, X0, in.target, pc);
            return true;
        }
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc,
                        /*no_bail=*/true);
        switch (fn) {
        case MathFn::sqrt_:    e.sqrtsd(X0, X0); break;
        case MathFn::tofloat_: break;   /* float(x): the read already widened */
        default:               e.u8(0xCC); break;   /* unreachable: MK_SSE */
        }
        emit_float_store(e, ck, X0, in.target, pc);
        return true;
    }

    case OpCode::LoadElemInt: case OpCode::LoadElemFloat: {
        /* a[i] from a flat int/float array (N4; #56 delete-originals form).
         * The inline fast path serves the proven flat non-slice in-range
         * shape; EVERY declined precondition - non-array, a slice,
         * general/strs/wrong-kind storage, a negative index, OOB - jumps
         * to the SLOW TIER: a call to jit_load_elem_int/float (the
         * interpreter's exact core), whose OOB CONVEYS with the op's
         * exc-stamped caret. The op never re-interprets, so its run's
         * originals are deletable. target2 = the array slot, a() = the
         * int index, target = the dst. */
        const JitLayout &L = jit_layout();
        const bool is_float = in.op == OpCode::LoadElemFloat;
        const SlotAddr base = slot_addr(in.target2);
        std::vector<size_t> j_slows, j_dones;
        /* lever A producer: elide a dead temp's write on the fast arms
         * (the SLOW tier's helper always writes the slot; its rejoin
         * reloads RAX below, so the contract holds on both paths). */
        const bool fw = !is_float && g_fwd.prod == in.target;
        const auto dst_write = [&]() {
            if (fw && g_fwd.skip_write)
                return;
            write_slot(e, ck, RAX, in.target, pc, fw);
        };

        /* C1: the HOISTED form - the entry navigation proved the base
         * and pinned (data, count); the element is a bounds check + a
         * read. Negative/OOB declines to the slow tier, which reads
         * MEMORY (never stale - the registers are the only derived
         * state). */
        const int hoist_want = is_float ? 1
                             : in.elem_bool_hint() ? 3 : 0;
        const bool hoisted = g_hoist.active && in.target2 == g_hoist.base
            && g_hoist.kind == hoist_want;
        if (hoisted) {
            load_index_r9(e, in);
            e.cmp_r9_hr(g_hoist.rcount);
            j_slows.push_back(e.j32(0x73));      /* jae -> slow */
            if (is_float) {
                e.load_elem_float_hr(g_hoist.rdata);
                emit_float_store(e, ck, X0, in.target, pc);
            } else {
                if (hoist_want == 3)             /* C1c: byte read, 0/1 */
                    e.load_elem_byte_hr(g_hoist.rdata);
                else
                    e.load_elem_int_hr(g_hoist.rdata);
                dst_write();
            }
            j_dones.push_back(e.j32(0xEB));
        } else {
        e.load(RAX, base.type);                  /* base an array? */
        e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
        e.cmp_rax_r9();
        j_slows.push_back(e.j32(0x75));          /* jne -> slow */
        e.cmp_byte_slot(base.payload + L.slice_off, 0);
        const size_t j_slice = e.j32(0x75);      /* #95: -> the slice arm */
        e.load(RAX, base.payload);               /* rax = shobj ptr */
        if (is_float) {
            e.cmp_byte_rax(L.kind_off, L.kind_floats);
            j_slows.push_back(e.j32(0x75));
            e.mov_rcx_rax(L.data_off);           /* rcx = _M_start */
            e.mov_rdx_rax(L.data_off + 8);       /* rdx = _M_finish */
            e.sub_rdx_rcx();
            e.sar_rdx_3();                        /* rdx = element count */
            load_index_r9(e, in);                 /* cache-aware index */
            e.cmp_r9_rdx();
            j_slows.push_back(e.j32(0x73));      /* jae (wrap/OOB) -> slow */
            e.load_elem_float();                 /* movsd xmm0,[rcx+r9*8] */
            emit_float_store(e, ck, X0, in.target, pc);
            j_dones.push_back(e.j32(0xEB));
        } else {
            e.cmp_byte_rax(L.kind_off, L.kind_ints);
            const size_t j_ints = e.j32(0x74);
            e.cmp_byte_rax(L.kind_off, L.kind_bools);
            j_slows.push_back(e.j32(0x75));
            /* flat bools: byte elements (no sar), movzx load */
            e.mov_rcx_rax(L.data_off);
            e.mov_rdx_rax(L.data_off + 8);
            e.sub_rdx_rcx();
            load_index_r9(e, in);
            e.cmp_r9_rdx();
            j_slows.push_back(e.j32(0x73));
            e.load_elem_byte();
            const size_t j_store = e.j32(0xEB);
            e.patch32_here(j_ints);              /* flat ints */
            e.mov_rcx_rax(L.data_off);
            e.mov_rdx_rax(L.data_off + 8);
            e.sub_rdx_rcx();
            e.sar_rdx_3();
            load_index_r9(e, in);
            e.cmp_r9_rdx();
            j_slows.push_back(e.j32(0x73));
            e.load_elem_int();
            e.patch32_here(j_store);
            dst_write();
            j_dones.push_back(e.j32(0xEB));
        }
        /*
         * #95 case 4 - the SLICE arm: elements live at data + (off + i),
         * bounds are the slice's LEN (a u32 in the handle, NOT the
         * vector's size - the sabotage that reads past the slice's end
         * is exactly what the len bound refuses). A negative index
         * declines (the helper wraps); a bool/other-kind slice declines
         * (int/float slices only - the reach case, 15_array_slice).
         */
        e.patch32_here(j_slice);
        e.load(RAX, base.payload);               /* rax = shobj ptr */
        e.cmp_byte_rax(L.kind_off,
                       is_float ? L.kind_floats : L.kind_ints);
        j_slows.push_back(e.j32(0x75));
        e.mov_edx_slot(base.payload + L.arr_len_off);   /* count = len */
        load_index_r9(e, in);
        e.cmp_r9_rdx();
        j_slows.push_back(e.j32(0x73));          /* jae: negative OR OOB */
        e.mov_rcx_rax(L.data_off);               /* rcx = vector data */
        e.mov_eax_slot(base.payload + L.arr_off_off);   /* rax = off */
        e.add_r9_rax();                          /* idx += off */
#ifdef TESTS
        e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_elem_slice_fast));
        e.u8(0x48); e.u8(0xFF); e.u8(0x02);      /* inc qword [rdx] */
#endif
        if (is_float) {
            e.load_elem_float();
            emit_float_store(e, ck, X0, in.target, pc);
        } else {
            e.load_elem_int();
            dst_write();
        }
        j_dones.push_back(e.j32(0xEB));
        }                                        /* end of !hoisted */
        /* slow: the interpreter-core helper; OOB/type conveys */
        for (const size_t j : j_slows)
            e.patch32_here(j);
        emit_call_prologue(e);
        load_operand(e, RSI, in.a_is_lit(), in.a_lit(), in.a_slot());
        e.lea(RDX, static_cast<int32_t>(
                       static_cast<long>(in.target)
                       * static_cast<long>(sizeof(LValue))));
        e.lea_rdi(static_cast<int32_t>(
                      static_cast<long>(in.target2)
                      * static_cast<long>(sizeof(LValue))));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(
                           is_float
                               ? jit_load_elem_float
                               : jit_load_elem_int) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);                  /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            emit_exc_stamp(e, ck, old_pc);       /* the op's own caret */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        /* lever A: the helper's status clobbered RAX - slow-path-only
         * reload (the fast dones jump PAST it, keeping their RAX) */
        if (fw)
            e.load(RAX, slot_addr(in.target).payload);
        for (const size_t j : j_dones)
            e.patch32_here(j);
        if (fw)
            g_fwd.armed = true;
        return true;
    }

    case OpCode::LoadElem2Int: case OpCode::LoadElem2Float:
        /* The fused nested read: jit_load_elem2_*(base_lv=&slot[target2],
         * oidx=a_dual_lo (cache-aware), iidx=b(), dst=&slot[target],
         * locs=chain_locs[a_dual_hi].data()). The helper BORROWS the row -
         * the point of the op - and throws WITH the per-level caret, so
         * the stamp below only supplies the inlined-at chain.
         * #93: the INT form gets an INLINE fast tier first; every guard
         * it fails lands here, on the helper. */
        {
        std::vector<size_t> e2_slows, e2_dones;
        emit_load_elem2_inline(e, ck, in, pc, e2_slows, e2_dones,
                               in.op == OpCode::LoadElem2Float);
        for (const size_t j : e2_slows)
            e.patch32_here(j);
        emit_call_prologue(e);
        e.lea_rdi(static_cast<int32_t>(
                      static_cast<long>(in.target2)
                      * static_cast<long>(sizeof(LValue))));
        read_slot(e, RSI, in.a_dual_lo());
        load_operand(e, RDX, in.b_is_lit(), in.b_lit(), in.b_slot());
        e.lea(RCX, static_cast<int32_t>(
                       static_cast<long>(in.target)
                       * static_cast<long>(sizeof(LValue))));
        e.movabs_r8(reinterpret_cast<uint64_t>(
                        ck.chain_locs[in.a_dual_hi()].data()));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(
                           in.op == OpCode::LoadElem2Int
                               ? jit_load_elem2_int
                               : jit_load_elem2_float) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);                  /* test eax, eax */
        {
            const size_t j_ok = e.j8(0x74);
            emit_exc_stamp(e, ck, old_pc);
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        /* lever A: slow-path-only RAX reload (fast dones jump past it) */
        const bool fw2 = in.op == OpCode::LoadElem2Int
            && g_fwd.prod == in.target;
        if (fw2)
            e.load(RAX, slot_addr(in.target).payload);
        for (const size_t j : e2_dones)      /* #93: fast tail rejoins */
            e.patch32_here(j);
        if (fw2)
            g_fwd.armed = true;
        }
        return true;

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
         * base (no kind). r8=aop, r9=the per-step caret buffer (baked).
         * #95: the INLINE fast tier first; every declined guard lands on
         * the helper, whose per-level OOB carets stay byte-identical. */
        {
        std::vector<size_t> s2_slows, s2_dones;
        emit_store_elem2_inline(e, in, s2_slows, s2_dones);
        for (const size_t j : s2_slows)
            e.patch32_here(j);
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
        for (const size_t j : s2_dones)       /* #95: fast tails rejoin */
            e.patch32_here(j);
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
        /* CACHE-AWARE SOURCE: when the source slot is pinned in an N5
         * register its LIVE value is there (memory is stale until the next
         * flush), so reading memory here would move the WRONG value - the
         * op had to disqualify the slot before this existed, and one
         * trailing `move r5 = a` then cost `a` its register for the whole
         * fragment. A pinned slot is a proven int (only a genuine int op
         * can qualify one - a MoveV contributes no weight), so this is the
         * ordinary int store: the two-store fast path, or the release
         * helper when the dst is ref-listed. No reference check is needed
         * on either side - the source cannot be a reference. */
        if (const int sreg = e.creg(in.target2); sreg >= 0) {
            store_dst(e, ck, static_cast<uint8_t>(sreg), in.target, pc);
            return true;
        }
        /* C2a: the FLOAT twin - a float-PINNED source is a proven float
         * (only genuine float ops qualify a pin; the MoveV itself adds
         * no weight), so the move is the ordinary float store from the
         * register (emit_float_store handles a ref-listed dst via
         * jit_put_float, which wants the value in xmm0). Without this,
         * an arg-staging `move rN = x` cost x its register for the
         * whole fragment - 04_float_arith's accumulator never pinned
         * because of its final str(x, 4). */
        if (const int fr = e.fcreg(in.target2); fr >= 0) {
            e.fmov_rr(X0, static_cast<uint8_t>(fr));
            emit_float_store(e, ck, X0, in.target, pc);
            return true;
        }
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
        e.slots_to_arg0();          /* rdi = the slot window */
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
            e.slots_to_arg0();          /* rdi = the slot window */
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
        e.slots_to_arg0();          /* rdi = the slot window */
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
            e.slots_to_arg0();          /* rdi = the slot window */
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
        e.slots_to_arg0();          /* rdi = the slot window */
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
         * (rdi=slots, rsi=dst, rdx=lo; rdi is loaded from rbx by the
         * helper's arg setup). lo = &ck.literal_objs[idx] (pool BUFFER
         * addr, stable across the chunk move). Never throws. */
        emit_call_prologue(e);
        e.slots_to_arg0();          /* rdi = the slot window */
        e.movabs(RSI, static_cast<uint64_t>(static_cast<int_type>(in.target)));
        e.movabs(RDX,
                 reinterpret_cast<uint64_t>(&ck.literal_objs[in.target2]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_load_literal_obj) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        return true;

    case OpCode::ArrLen:
        /* n = size(frame[base]) via jit_arr_len (rdi=slots from rbx,
         * rsi=dst=target,
         * rdx=base=target2). Base is a proven flat array -> never throws. */
        emit_call_prologue(e);
        e.slots_to_arg0();          /* rdi = the slot window */
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
         * throws -> test eax + the collapse-safe exc-stamp + exit_pc, so the
         * re-raise carries the literal's caret WITHOUT a pc lookup: that is
         * what makes the op CONVEY rather than bail, hence deletable (#56 -
         * the same treatment its MakeArrayV/MakeStructArrayV siblings have). */
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;

    case OpCode::StructCtorV: {
        const int32_t plan = in.b_dual_hi();
        if (plan >= 0) {
            /* THE PLANNED CTOR (the 64_struct_create fix): every field act
             * is a compile-proven {offset, act}, so the hot path is the H1
             * reuse guards + direct byte stores into the instance - the C++
             * shape. Guards (any miss -> the never-throwing planned helper,
             * which allocates fresh / handles an aliased dst): dst holds a
             * struct, same def, use_count == 1, not readonly. NEVER exits
             * (op_never_exits), so the run stays leaf-safe/deletable. */
            const JitLayout &L = jit_layout();
            const Chunk::CtorPlan &cp = ck.ctor_plans[plan];
            size_t j_done = 0;
            bool have_fast = false;
            if (L.sobj_ok) {
                have_fast = true;
                const SlotAddr d = slot_addr(in.target);
                e.load(RAX, d.type);
                e.movabs(RCX, reinterpret_cast<uint64_t>(L.t_struct));
                e.cmp_rax_rcx();
                const size_t js1 = e.j32(0x75);
                e.load(RAX, d.payload);           /* rax = StructObject* */
                e.movabs(RCX, reinterpret_cast<uint64_t>(
                                  ck.struct_defs[in.target2]));
                /* cmp [rax + sobj_def], rcx */
                e.u8(0x48); e.u8(0x39); e.u8(0x88);
                e.u32(static_cast<uint32_t>(L.sobj_def));
                const size_t js2 = e.j32(0x75);
                /* cmp dword [rax + sobj_rc], 1  (use_count == 1) */
                e.u8(0x83); e.u8(0xB8);
                e.u32(static_cast<uint32_t>(L.sobj_rc)); e.u8(0x01);
                const size_t js3 = e.j32(0x75);
                /* cmp byte [rax + sobj_ro], 0  (not readonly) */
                e.u8(0x80); e.u8(0xB8);
                e.u32(static_cast<uint32_t>(L.sobj_ro)); e.u8(0x00);
                const size_t js4 = e.j32(0x75);
#ifdef TESTS
                e.movabs(RCX, reinterpret_cast<uint64_t>(&g_jit_ctor_fast));
                e.u8(0x48); e.u8(0xFF); e.u8(0x01);   /* inc qword [rcx] */
#endif
                /* r9 = bytes data (vector _M_start at +0, probed) */
                e.u8(0x4C); e.u8(0x8B); e.u8(0x88);
                e.u32(static_cast<uint32_t>(L.sobj_bytes));
                for (size_t fi = 0; fi < cp.f.size(); fi++) {
                    const Chunk::CtorPlanField &pf = cp.f[fi];
                    const SlotAddr s = slot_addr(pf.src);
                    switch (pf.act) {
                    case 0: {                      /* raw int (bool = 0/1) */
                        /* cache-aware: a direct-local src can be the
                         * N5-pinned loop counter */
                        const int cr = e.creg(pf.src);
                        if (cr >= 0)
                            e.mov_rr(RAX, static_cast<uint8_t>(cr));
                        else
                            e.load(RAX, s.payload);
                        /* mov [r9 + off], rax */
                        e.u8(0x49); e.u8(0x89); e.u8(0x81);
                        e.u32(static_cast<uint32_t>(pf.off));
                        break;
                    }
                    case 1:               /* float (int/bool promote, r8) */
                        emit_float_load(e, X0, false, 0, pf.src, pc,
                                        /*no_bail=*/true);
                        /* movsd [r9 + off], xmm0 */
                        e.u8(0xF2); e.u8(0x41); e.u8(0x0F); e.u8(0x11);
                        e.u8(0x81); e.u32(static_cast<uint32_t>(pf.off));
                        break;
                    default:              /* bool byte (payload is 0/1) */
                        /* movzx eax, byte [rdi + payload] */
                        e.u8(0x0F); e.u8(0xB6); e.u8(MODRM_SLOT);
                        e.u32(static_cast<uint32_t>(s.payload));
                        /* mov [r9 + off], al */
                        e.u8(0x41); e.u8(0x88); e.u8(0x81);
                        e.u32(static_cast<uint32_t>(pf.off));
                        break;
                    }
                }
                j_done = e.j32(0xEB);
                e.patch32_here(js1);
                e.patch32_here(js2);
                e.patch32_here(js3);
                e.patch32_here(js4);
            }
            /* slow: jit_struct_ctor_planned(def, plan, dst) - the
             * interpreter's planned body (fresh alloc / aliased dst),
             * which reads the plan srcs from MEMORY - flush the cache
             * first (an act-0 src may be register-pinned). NEVER throws
             * -> no test/exit; dst is never cached (bad) -> no reload. */
            e.flush_cache();
            emit_call_prologue(e);
            e.movabs(RDI,
                     reinterpret_cast<uint64_t>(ck.struct_defs[in.target2]));
            e.movabs(RSI, reinterpret_cast<uint64_t>(&ck.ctor_plans[plan]));
            e.movabs(RDX,
                     static_cast<uint64_t>(static_cast<int_type>(in.target)));
            e.call_relocs.push_back(
                { e.pos(), reinterpret_cast<const void *>(
                               jit_struct_ctor_planned) });
            e.u8(0xE8); e.u32(0);
            emit_call_epilogue(e);
            if (have_fast)
                e.patch32_here(j_done);
            return true;
        }
    }
        [[fallthrough]];
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
        e.movabs(RDX, static_cast<uint64_t>(
                          in.op == OpCode::StructCtorV
                              ? static_cast<int_type>(in.b_dual_lo())
                              : in.b_lit()));
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
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
         * operand reads are no_bail (int/bool promote like read_float_slot),
         * so the op has NO exit - never-exits, leaf-safe. */
        const FCmp fc = float_cmp(in.aop);
        e.bump_op(OpCode::CmpFloatV);        /* before loads (rax) */
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc,
                        /*no_bail=*/true);
        emit_float_load(e, X1, in.b_is_lit(), in.b_flit(), in.b_slot(), pc,
                        /*no_bail=*/true);
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
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
            emit_exc_stamp(e, ck, old_pc);   /* cold; null-checked, so the
                                              * undefined-global BAIL path is
                                              * safe; a pool-loc'd throw keeps
                                              * its own caret (stamp-if-empty) */
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
            emit_exc_stamp(e, ck, old_pc);   /* cold; null-checked, so the
                                              * undefined-global BAIL path is
                                              * safe; a pool-loc'd throw keeps
                                              * its own caret (stamp-if-empty) */
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
            emit_exc_stamp(e, ck, old_pc);   /* cold; null-checked, so the
                                              * undefined-global BAIL path is
                                              * safe; a pool-loc'd throw keeps
                                              * its own caret (stamp-if-empty) */
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
            emit_exc_stamp(e, ck, old_pc);   /* cold; null-checked, so the
                                              * undefined-global BAIL path is
                                              * safe; a pool-loc'd throw keeps
                                              * its own caret (stamp-if-empty) */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        return true;
    }

    case OpCode::PushHandler: {
        /* INLINE try-handler push (step 7a): load the handlers vector's
         * finish; at capacity -> the cold jit_push_handler_grow; else store
         * the REGION ID (a 4-byte VmHandler) and bump finish. Never throws.
         * #78 step D: the pushed handler is JUST the region now - the
         * dispatch reads the clause pcs off Chunk::handler_sites - so this
         * op has no pc operand and no longer needs the branch emitter's
         * remap[] (it moved here from emit_branch, and left op_is_branch). */
        const JitLayout &L = jit_layout();
        const uint32_t region = static_cast<uint32_t>(in.a_lit());
        e.bump_op(OpCode::PushHandler);
        e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_act));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] (act) */
        e.u8(0x48); e.u8(0x8B); e.u8(0x88);        /* mov rcx, [rax+h+8] */
        e.u32(static_cast<uint32_t>(L.act_handlers + 8));    /* finish */
        e.u8(0x48); e.u8(0x3B); e.u8(0x88);        /* cmp rcx, [rax+h+16] */
        e.u32(static_cast<uint32_t>(L.act_handlers + 16));   /* end cap */
        const size_t j_grow = e.j32(0x74);         /* je -> the cold grow */
        e.u8(0xC7); e.u8(0x01);                    /* mov dword [rcx], rg */
        e.u32(region);
        e.u8(0x48); e.u8(0x83); e.u8(0xC1); e.u8(4);   /* add rcx, 4 */
        e.u8(0x48); e.u8(0x89); e.u8(0x88);        /* mov [rax+h+8], rcx */
        e.u32(static_cast<uint32_t>(L.act_handlers + 8));
        const size_t j_done = e.j32(0xEB);
        e.patch32_here(j_grow);
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(region)));
        e.call_relocs.push_back(
            { e.pos(),
              reinterpret_cast<const void *>(jit_push_handler_grow) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.patch32_here(j_done);
        return true;
    }

    case OpCode::PopHandler: {
        /* INLINE handler pop (step 7a): `finish -= 8` - a VmHandler is a
         * trivial 4-byte {region} struct (#78 step D), so vector
         * pop_back is exactly the finish decrement (never empty at a
         * PopHandler by codegen construction). Never throws. */
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
        /* INLINE finally-pend store (step 7a; #78 re-addressed): a byte
         * store into pends[rec.pend_base + region].pend, the try's
         * per-REGION slot (a = the region id, target = the Pend ENUM
         * value - neither is a pc). Never throws. */
        const JitLayout &L = jit_layout();
        e.bump_op(OpCode::SetPend);
        e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_act));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] */
        e.u8(0x48); e.u8(0x8B); e.u8(0x88);        /* mov rcx, [rax+toprec] */
        e.u32(static_cast<uint32_t>(L.act_top_rec));   /* = &back_rec */
        e.u8(0x8B); e.u8(0x91);                    /* mov edx, [rcx+pbase] */
        e.u32(static_cast<uint32_t>(L.rec_pend_base));
        if (in.a_lit()) {
            e.u8(0x81); e.u8(0xC2);                /* add edx, region */
            e.u32(static_cast<uint32_t>(in.a_lit()));
        }
        ML_CHECK(L.pend_state_size == 16);
        e.u8(0x48); e.u8(0xC1); e.u8(0xE2); e.u8(4);   /* shl rdx, 4 */
        e.u8(0x48); e.u8(0x8B); e.u8(0x80);        /* mov rax, [rax+pends] */
        e.u32(static_cast<uint32_t>(L.act_pends)); /* _M_start */
        e.u8(0x48); e.u8(0x01); e.u8(0xD0);        /* add rax, rdx = entry */
        e.u8(0xC6); e.u8(0x80);                    /* mov byte [rax+off], v */
        e.u32(static_cast<uint32_t>(L.pend_state_pend));
        e.u8(static_cast<uint8_t>(in.target));
        return true;
    }

    case OpCode::EndFinally: {
        /* INLINE the NORMAL path: pends[rec.pend_base + region].pend ==
         * normal (0) -> fall through to Lend. #78 step E: the RERAISE path
         * no longer BAILS - it calls jit_end_finally, which runs the
         * interpreted op's exact body (the shared vm_raise) and reports
         * dispatched / boundary / conveyed exactly as the native `throw`
         * does. That removed the last bail in the exception ops, so a run
         * containing EndFinally is op_fully_native and deletable.
         * (a = the region id, #78.) */
        const JitLayout &L = jit_layout();
        e.bump_op(OpCode::EndFinally);
        e.movabs(RAX, reinterpret_cast<uint64_t>(L.addr_act));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);        /* mov rax, [rax] */
        e.u8(0x48); e.u8(0x8B); e.u8(0x88);        /* mov rcx, [rax+toprec] */
        e.u32(static_cast<uint32_t>(L.act_top_rec));   /* = &back_rec */
        e.u8(0x8B); e.u8(0x91);                    /* mov edx, [rcx+pbase] */
        e.u32(static_cast<uint32_t>(L.rec_pend_base));
        if (in.a_lit()) {
            e.u8(0x81); e.u8(0xC2);                /* add edx, region */
            e.u32(static_cast<uint32_t>(in.a_lit()));
        }
        ML_CHECK(L.pend_state_size == 16);
        e.u8(0x48); e.u8(0xC1); e.u8(0xE2); e.u8(4);   /* shl rdx, 4 */
        e.u8(0x48); e.u8(0x8B); e.u8(0x80);        /* mov rax, [rax+pends] */
        e.u32(static_cast<uint32_t>(L.act_pends));
        e.u8(0x48); e.u8(0x01); e.u8(0xD0);        /* add rax, rdx = entry */
        e.u8(0x80); e.u8(0xB8);                    /* cmp byte [rax+off], 0 */
        e.u32(static_cast<uint32_t>(L.pend_state_pend));
        e.u8(0);
        /* rel32: the cold arm below is far bigger than a short jump's
         * reach (emit_exc_stamp alone exceeds it). */
        const size_t j_norm = e.j32(0x74);  /* je -> the NORMAL fall-through */

        /* the COLD reraise arm */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_lit())));
        e.movabs(RSI, static_cast<uint64_t>(pc));
        e.movabs(RDX, static_cast<uint64_t>(
                          static_cast<int_type>(ck.inline_frame_at(old_pc))));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_end_finally) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);

        e.u8(0x83); e.u8(0xF8); e.u8(0x03);       /* cmp eax, 3 */
        const size_t j_none = e.j32(0x74);        /* je -> fall through */
        e.u8(0x83); e.u8(0xF8); e.u8(0x02);       /* cmp eax, 2 */
        {
            const size_t j_not2 = e.j8(0x75);
            emit_exc_stamp(e, ck, old_pc);        /* (already located) */
            e.exit_pc(pc);
            e.patch8(j_not2, e.pos());
        }
        e.u8(0x85); e.u8(0xC0);                   /* test eax, eax */
        {
            const size_t j_disp = e.j8(0x74);     /* jz -> dispatched */
            e.flush_cache();                      /* every raw ret must */
            e.movabs(RAX, static_cast<uint64_t>(-2));   /* JIT_RET_BOUNDARY */
            e.frag_ret();
            e.patch8(j_disp, e.pos());
        }
        e.flush_cache();
        e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_resume_pc()));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);       /* mov rax, [rax] */
        e.frag_ret();                               /* ret (the handler pc) */

        e.patch32_here(j_none);
        e.patch32_here(j_norm);
        return true;
    }

    case OpCode::Throw:
        /* #56: the NATIVE `throw` - jit_throw(val_slot, pc, &locs[i]) runs
         * the interpreted op's exact body (build + the shared vm_raise).
         * 0 = dispatched to a same-frame handler: return the parked
         * handler pc as an ordinary external exit (the op ALREADY ran -
         * this is a resume, not a re-run); 1 = boundary: return
         * JIT_RET_BOUNDARY so the invocation stops, as the interpreted
         * `if (!vm_raise(...)) return;`; 2 = the builder's TypeErrorEx,
         * conveyed. No re-interpret -> the original is deletable. */
        e.bump_op(in.op);
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.movabs(RSI, static_cast<uint64_t>(pc));
        e.movabs(RDX, reinterpret_cast<uint64_t>(loc_entry_addr(ck, old_pc)));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_throw) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x83); e.u8(0xF8); e.u8(0x02);       /* cmp eax, 2 */
        {
            const size_t j_not2 = e.j8(0x75);
            emit_exc_stamp(e, ck, old_pc);        /* (already located) */
            e.exit_pc(pc);
            e.patch8(j_not2, e.pos());
        }
        e.u8(0x85); e.u8(0xC0);                   /* test eax, eax */
        {
            const size_t j_disp = e.j8(0x74);     /* jz -> dispatched */
            /* the N5 register cache MUST be flushed before ANY return -
             * exit_pc does it implicitly; these raw rets must do it
             * explicitly or the interpreter reads stale pinned slots
             * (a cross-frame-throw SEGV caught it). */
            e.flush_cache();
            e.movabs(RAX, static_cast<uint64_t>(-2));   /* JIT_RET_BOUNDARY */
            e.frag_ret();                           /* ret */
            e.patch8(j_disp, e.pos());
        }
        e.flush_cache();
        e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_resume_pc()));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);       /* mov rax, [rax] */
        e.frag_ret();                               /* ret (the handler pc) */
        return true;

    case OpCode::Rethrow:
        /* #80: the NATIVE `rethrow` - jit_rethrow runs the interpreted op's
         * exact body (take the caught exception out of the enclosing
         * catch's region slot, restamp it with the RETHROW SITE's caret,
         * vm_raise). It never falls through, so the shape is Throw's
         * exactly: 0 = dispatched (return the parked handler pc as an
         * external exit - the op already ran), 1 = boundary, 2 = conveyed.
         * The LocEntry and the inlined-at chain are BAKED here because a
         * deleted run's pcs collapse onto the head EnterNative. */
        e.bump_op(in.op);
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_lit())));
        e.movabs(RSI, static_cast<uint64_t>(pc));
        e.movabs(RDX, reinterpret_cast<uint64_t>(loc_entry_addr(ck, old_pc)));
        e.movabs(RCX, static_cast<uint64_t>(
                          static_cast<int_type>(ck.inline_frame_at(old_pc))));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_rethrow) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x83); e.u8(0xF8); e.u8(0x02);       /* cmp eax, 2 */
        {
            const size_t j_not2 = e.j8(0x75);
            emit_exc_stamp(e, ck, old_pc);
            e.exit_pc(pc);
            e.patch8(j_not2, e.pos());
        }
        e.u8(0x85); e.u8(0xC0);                   /* test eax, eax */
        {
            const size_t j_disp = e.j8(0x74);     /* jz -> dispatched */
            e.flush_cache();                      /* every raw ret must */
            e.movabs(RAX, static_cast<uint64_t>(-2));   /* JIT_RET_BOUNDARY */
            e.frag_ret();
            e.patch8(j_disp, e.pos());
        }
        e.flush_cache();
        e.movabs(RAX, reinterpret_cast<uint64_t>(jit_addr_resume_pc()));
        e.u8(0x48); e.u8(0x8B); e.u8(0x00);       /* mov rax, [rax] */
        e.frag_ret();                               /* ret (the handler pc) */
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
        /* jit_throw_runtime(&ck.throws[target]) builds the pooled exception
         * (Runtime -> g_vm_jit_exc, plain -> g_vm_jit_eptr) with its exact
         * pooled caret, then the unconditional exit lands in EnterNative's
         * conveyance branches - never a re-run, so the op is deletable.
         * (The helper bumps the coverage counter; no emit-side bump.) */
        emit_call_prologue(e);
        e.movabs(RDI, reinterpret_cast<uint64_t>(&ck.throws[in.target]));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_throw_runtime) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
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
            e.cmp_byte_slot(base.payload + L.slice_off, 0);   /* not a slice? */
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
            emit_exc_stamp(e, ck, old_pc);    /* cold: the OOB caret (the
                                               * InternalErrorEx net rides
                                               * eptr, loc-less both ways) */
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
        /* #60 (the value-model campaign): the INT-INT FAST TIER, inline.
         * The dyn/boxed arithmetic ops paid a full helper call (~80 Ir:
         * marshal + EvalValue copies + vm_num_binop + put) even when both
         * runtime operands are plain ints - the overwhelmingly common case
         * in a dyn accumulator loop (66_dyn_foreach's `s = (s+e) % M`).
         * Emit type-tag guards + payload arithmetic + the ref-aware store
         * at the site; ANY other shape (float/bool/string/mixed operand, a
         * throwing aop - div/mod/shifts) falls to the EXACT helper path
         * below, byte-identical incl. carets. Guards precede every
         * mutation, so the decline is idempotent. CompoundV's fast store
         * writes the PAYLOAD only (the guard proved the dst already holds
         * an int, so the type word is already t_int and there is nothing
         * to release). CmpV yields a REAL bool (is_true of the 0/1 int),
         * the CmpIntV setcc shape. */
        const Chunk::BoxedOp &bo = ck.boxed_ops[in.target2];
        const bool comp = in.op == OpCode::CompoundV;
        const bool cmp = in.op == OpCode::CmpV;
        const auto arith_ok = [](Op o) {
            return o == Op::plus || o == Op::minus || o == Op::times
                || o == Op::band || o == Op::bor || o == Op::bxor;
        };
        /* div/mod join the fast tier when they provably cannot trap or
         * need the throw: an IMM divisor must be neither 0 nor -1 (the
         * IntModRI idiv-trap exclusion - INT64_MIN % -1); a REG divisor
         * gets runtime 0/-1 guards that DECLINE to the helper (which
         * throws DivisionByZeroEx / computes the -1 case exactly as the
         * interpreter's C++ does). */
        const auto divmod_ok = [](Op o, const Operand &b) {
            return (o == Op::div || o == Op::mod)
                && (!b.is_lit || (b.lit != 0 && b.lit != -1));
        };
        const auto cmp_ok = [](Op o) {
            return o == Op::lt || o == Op::gt || o == Op::le || o == Op::ge
                || o == Op::eq || o == Op::noteq;
        };
        const auto opnd_ok = [](const Operand &o) {
            return !o.is_lit || o.lit_kind == Operand::LitKind::i;
        };
        const bool dv = !cmp && divmod_ok(bo.aop, bo.b);
        const bool fast = in.op != OpCode::UnaryV
            && (cmp ? cmp_ok(bo.aop) : (arith_ok(bo.aop) || dv))
            && opnd_ok(bo.b) && (comp || opnd_ok(bo.a));
        std::vector<size_t> j_slows;
        size_t j_done = 0;
        if (fast) {
            e.movabs(RCX, reinterpret_cast<uint64_t>(jit_layout().t_int));
            if (comp) {
                e.load(RAX, slot_addr(in.target).type);
                e.cmp_rax_rcx();
                j_slows.push_back(e.j32(0x75));
            } else if (!bo.a.is_lit) {
                e.load(RAX, slot_addr(bo.a.slot).type);
                e.cmp_rax_rcx();
                j_slows.push_back(e.j32(0x75));
            }
            if (!bo.b.is_lit) {
                e.load(RAX, slot_addr(bo.b.slot).type);
                e.cmp_rax_rcx();
                j_slows.push_back(e.j32(0x75));
            }
#ifdef TESTS
            e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_boxed_fast));
            e.u8(0x48); e.u8(0xFF); e.u8(0x02);    /* inc qword [rdx] */
#endif
            /* payloads: rax = lhs, rcx = rhs (guard value dead now) */
            if (comp)
                e.load(RAX, slot_addr(in.target).payload);
            else if (bo.a.is_lit)
                e.movabs(RAX, static_cast<uint64_t>(bo.a.lit));
            else
                e.load(RAX, slot_addr(bo.a.slot).payload);
            if (bo.b.is_lit)
                e.movabs(RCX, static_cast<uint64_t>(bo.b.lit));
            else
                e.load(RCX, slot_addr(bo.b.slot).payload);
            if (cmp) {
                e.cmp_rax_rcx();
                e.u8(0x0F);
                e.u8(static_cast<uint8_t>(cc_for(bo.aop).near_op + 0x10));
                e.u8(0xC0);                           /* setcc al */
                e.u8(0x0F); e.u8(0xB6); e.u8(0xC0);   /* movzx eax, al */
                store_dst_bool(e, ck, RAX, in.target);
            } else {
                if (dv) {
                    if (!bo.b.is_lit) {
                        /* the edge divisors (#103): rcx+1 unsigned <= 1
                         * catches 0 and -1 in ONE hot branch; the cold
                         * side declines 0 (the helper throws div0) and
                         * ONLY the INT_MIN dividend (the helper throws
                         * the overflow) - an ordinary x / -1 falls back
                         * into the native idiv (rax = the dividend is
                         * already loaded; rdx is dead, cqo is next). */
                        e.u8(0x48); e.u8(0x8D); e.u8(0x51); e.u8(0x01);
                        e.u8(0x48); e.u8(0x83); e.u8(0xFA); e.u8(0x01);
                        const size_t j_div = e.j32(0x77);  /* ja .div */
                        e.u8(0x48); e.u8(0x85); e.u8(0xC9);
                        j_slows.push_back(e.j32(0x74));    /* 0 -> slow */
                        e.movabs(RDX, 0x8000000000000000ull);
                        e.u8(0x48); e.u8(0x39); e.u8(0xD0);
                        j_slows.push_back(e.j32(0x74));    /* ovf -> slow */
                        e.patch32_here(j_div);
                    }
                    e.u8(0x48); e.u8(0x99);              /* cqo */
                    e.u8(0x48); e.u8(0xF7); e.u8(0xF9);  /* idiv rcx */
                    if (bo.aop == Op::mod)
                        e.mov_rr(RAX, RDX);              /* the remainder */
                } else {
                    op_rr(e, bo.aop);
                }
                if (comp) {
                    e.store(RAX, slot_addr(in.target).payload);
                } else {
                    e.movabs(RSI,
                             reinterpret_cast<uint64_t>(jit_layout().t_int));
                    store_dst(e, ck, RAX, in.target, pc);
                }
            }
            j_done = e.j32(0xEB);
            for (const size_t j : j_slows)
                e.patch32_here(j);
        }
        /* the slow tier: the interpreter-exact helpers (any operand shape,
         * the throwing aops) */
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
        if (j_done) e.patch32_here(j_done);
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe (#56) */
        e.exit_pc(pc);
        e.patch8(j_ok_mf, e.pos());
        return true;
    }

    case OpCode::CheckCallableV: {
        /* jit_check_callable(slot) - rdi = a_slot. A non-callable conveys a
         * loc-less NotCallableEx -> exc-stamp (the callee caret) + exit. */
        emit_call_prologue(e);
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_check_callable) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok_cc = e.j8(0x74);
        emit_exc_stamp(e, ck, old_pc);        /* cold: the callee caret */
        e.exit_pc(pc);
        e.patch8(j_ok_cc, e.pos());
        return true;
    }

    case OpCode::CallValueGenericV: {
        /* jit_call_value_generic(dst|callee<<32, argbase, nargs, cs, mkeys,
         * site) - rdi=packed dst/callee, rsi=a_lit, rdx=nargs (b_lit's low
         * 12 bits), rcx=&ck.call_sites[b_lit>>12], r8=member_keys.data()
         * (an elem/member arg0 descriptor), r9=the baked call-site loc (a
         * FuncObject callee's backtrace frame). The helper self-stamps its
         * throws from the pool's args caret; a bail re-runs the interpreted
         * op (all pre-call work idempotent). */
        Loc ls, le;
        ck.loc_at(old_pc, ls, le);
        const uint64_t site =
            (static_cast<uint64_t>(static_cast<uint32_t>(ls.line)) << 32)
            | static_cast<uint32_t>(ls.col);
        const int site_i = static_cast<int>(in.b_lit() >> 12);
        emit_call_prologue(e);
        e.movabs(RDI,
                 (static_cast<uint64_t>(
                      static_cast<uint32_t>(in.target2)) << 32)
                 | static_cast<uint32_t>(in.target));
        e.movabs(RSI, static_cast<uint64_t>(in.a_lit()));
        e.movabs(RDX, static_cast<uint64_t>(in.b_lit() & 0xfff));
        e.movabs(RCX, reinterpret_cast<uint64_t>(&ck.call_sites[site_i]));
        e.movabs_r8(reinterpret_cast<uint64_t>(ck.member_keys.data()));
        e.movabs_r9(site);
        e.call_relocs.push_back(
            { e.pos(),
              reinterpret_cast<const void *>(jit_call_value_generic) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
        const size_t j_ok_cvg = e.j8(0x74);
        e.exit_pc(pc);
        e.patch8(j_ok_cvg, e.pos());
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe caret (#56) */
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe caret (#56) */
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
            emit_exc_stamp(e, ck, old_pc);    /* collapse-safe caret (#56) */
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
         * conveyed on throw -> deletable). leas from rbx (slots base). A
         * non-0 return = threw ->
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

    case OpCode::OrdCharV: {
        /* dst = ord(base_str[idx]) via jit_ord_char(base_lv, idx, dst_lv) -
         * SysV rdi = &slot[base], rsi = the int index VALUE (cache-aware
         * load_operand), rdx = &slot[dst]. The only throw (OOB) conveys ->
         * exc-stamp with the subscript's caret -> exit_pc re-raise. rdi is
         * set LAST (it overwrites the slots-base pointer). */
        const auto off = [](int slot) {
            return static_cast<int32_t>(static_cast<long>(slot)
                                        * static_cast<long>(sizeof(LValue)));
        };
        emit_call_prologue(e);
        load_operand(e, RSI, in.a_is_lit(), in.a_lit(), in.a_slot());
        e.lea(RDX, off(in.target));         /* rdx = &slot[dst] */
        e.lea_rdi(off(in.target2));         /* rdi = &slot[base] (LAST) */
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_ord_char) });
        e.u8(0xE8); e.u32(0);
        emit_call_epilogue(e);
        e.u8(0x85); e.u8(0xC0);             /* test eax, eax */
        const size_t j_ok = e.j8(0x74);     /* jz ok (0 = no throw) */
        emit_exc_stamp(e, ck, old_pc);      /* cold: the subscript's caret */
        e.exit_pc(pc);                      /* threw -> EnterNative re-raises */
        e.patch8(j_ok, e.pos());
        return true;
    }

    case OpCode::LoadMemberInt:
    case OpCode::LoadMemberFloat: {
        /* THE BAKED MEMBER READ (the 64_struct_create fix): try_member_
         * scalar resolved the field at COMPILE time (b dual: lo = byte
         * offset, hi = struct_defs idx << 2 | form) - the fast path is
         * type-tag + def-identity guards and ONE byte read, no name scan,
         * no helper call. Forms: 0 int, 1 float, 2 bool -> 0/1, 3 int
         * promoted to float. Any guard miss -> the generic helper below
         * (which can throw -> test eax + exit_pc re-raise). */
        const JitLayout &L = jit_layout();
        const int32_t moff = in.b_dual_lo();
        size_t j_done = 0;
        bool have_fast = false;
        if (moff >= 0 && L.sobj_ok) {
            have_fast = true;
            const int form = in.b_dual_hi() & 3;
            const SlotAddr b = slot_addr(in.target2);
            e.load(RAX, b.type);
            e.movabs(RCX, reinterpret_cast<uint64_t>(L.t_struct));
            e.cmp_rax_rcx();
            const size_t js1 = e.j32(0x75);
            e.load(RAX, b.payload);            /* rax = StructObject* */
            e.movabs(RCX, reinterpret_cast<uint64_t>(
                              ck.struct_defs[in.b_dual_hi() >> 2]));
            /* cmp [rax + sobj_def], rcx */
            e.u8(0x48); e.u8(0x39); e.u8(0x88);
            e.u32(static_cast<uint32_t>(L.sobj_def));
            const size_t js2 = e.j32(0x75);
#ifdef TESTS
            e.movabs(RCX, reinterpret_cast<uint64_t>(&g_jit_member_fast));
            e.u8(0x48); e.u8(0xFF); e.u8(0x01);    /* inc qword [rcx] */
#endif
            /* rax = bytes data (vector _M_start at +0, probed) */
            e.u8(0x48); e.u8(0x8B); e.u8(0x80);
            e.u32(static_cast<uint32_t>(L.sobj_bytes));
            switch (form) {
            case 0:                                   /* int field */
                /* mov rax, [rax + moff] */
                e.u8(0x48); e.u8(0x8B); e.u8(0x80);
                e.u32(static_cast<uint32_t>(moff));
                store_dst(e, ck, RAX, in.target, pc);
                break;
            case 1:                                   /* float field */
                /* movsd xmm0, [rax + moff] */
                e.u8(0xF2); e.u8(0x0F); e.u8(0x10); e.u8(0x80);
                e.u32(static_cast<uint32_t>(moff));
                emit_float_store(e, ck, X0, in.target, pc);
                break;
            case 2:                                   /* bool -> 0/1 int */
                /* movzx eax, byte [rax + moff] */
                e.u8(0x0F); e.u8(0xB6); e.u8(0x80);
                e.u32(static_cast<uint32_t>(moff));
                e.u8(0x85); e.u8(0xC0);               /* test eax, eax */
                e.u8(0x0F); e.u8(0x95); e.u8(0xC0);   /* setne al */
                e.u8(0x0F); e.u8(0xB6); e.u8(0xC0);   /* movzx eax, al */
                store_dst(e, ck, RAX, in.target, pc);
                break;
            default:                       /* int field read as float */
                /* mov rax, [rax + moff] */
                e.u8(0x48); e.u8(0x8B); e.u8(0x80);
                e.u32(static_cast<uint32_t>(moff));
                /* cvtsi2sd xmm0, rax */
                e.u8(0xF2); e.u8(0x48); e.u8(0x0F); e.u8(0x2A); e.u8(0xC0);
                emit_float_store(e, ck, X0, in.target, pc);
                break;
            }
            j_done = e.j32(0xEB);
            e.patch32_here(js1);
            e.patch32_here(js2);
        }
        /* the generic helper (slot_of + kind dispatch + member_read_core
         * fallback) - now the COLD path */
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
            emit_exc_stamp(e, ck, old_pc);    /* belt: pooled carets (#56) */
            e.exit_pc(pc);
            e.patch8(j_ok, e.pos());
        }
        if (have_fast)
            e.patch32_here(j_done);
        return true;
    }

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
         * then call jit_ret(res_slot) and RET its resume sentinel. rdi is
         * free scratch, so it just carries the arg; jit_ret uses
         * g_current_ctx->frame, not the base. No 16-alignment adjustment:
         * frag_entry's push already left rsp % 16 == 0, which is exactly
         * what the call needs. No prologue/epilogue: we ret, so only the
         * caller's rbx needs restoring - frag_ret does that. */
        e.flush_cache();
        e.movabs(RDI, static_cast<uint64_t>(
                          static_cast<int_type>(in.a_slot())));
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_ret) });
        e.u8(0xE8); e.u32(0);                             /* call jit_ret */
        e.frag_ret();                              /* ret (rax = sentinel) */
        return true;

    case OpCode::Halt:
        /* model-flip (nativize-ops): the native `return none`. flush_cache so a
         * later-bail path (there is none past a terminator, but stay uniform
         * with ReturnV) has memory consistent, then call jit_halt() (no arg -
         * the result is none) and RET its resume sentinel. Same stack
         * discipline as ReturnV: already call-aligned, frag_ret restores. */
        e.flush_cache();
        e.call_relocs.push_back(
            { e.pos(), reinterpret_cast<const void *>(jit_halt) });
        e.u8(0xE8); e.u32(0);                             /* call jit_halt */
        e.frag_ret();                              /* ret (rax = sentinel) */
        return true;

    case OpCode::CachedCallV:
        /* M5c: the cached recursive call, fully inline - the emitted site
         * calls the lean jit_cached_probe (hit -> dst written, continue),
         * and a miss rides the parked key through the M5b inline push
         * (rec.cache_key store); declines fall to jit_call_sync_cached,
         * which CONSUMES the parked key instead of re-probing. */
        emit_sync_call_inline(e, ck, in, pc, old_pc, /*is_value=*/false,
                       reinterpret_cast<const void *>(jit_call_sync_cached),
                       static_cast<int_type>(in.target2));
        return true;

    case OpCode::CallValueV:
        /* M5 inc 3 + lever 1 step 5: the indirect func-VALUE call (closure/
         * lambda/func var - the callee was evaluated into the target2 temp),
         * now the fragment-INLINE shape (direct push + `call rdx`). A
         * non-func value falls to the slow helper -> the interpreted op's
         * NotCallableEx. */
        emit_sync_call_inline(e, ck, in, pc, old_pc, /*is_value=*/true,
                       reinterpret_cast<const void *>(jit_call_sync_value),
                       static_cast<int_type>(in.target2));
        return true;

    case OpCode::CallV: {
        if (!callv_native_ok(in, jc)) {
            /* M5 + lever 1 step 5: the SYNC call, fragment-INLINE shape
             * (direct push + `call rdx`; the full jit_call_sync helper is
             * the cold fallback). */
            emit_sync_call_inline(e, ck, in, pc, old_pc, /*is_value=*/false,
                           reinterpret_cast<const void *>(jit_call_sync),
                           static_cast<int_type>(in.target2));
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

        emit_call_prologue(e);              /* empty cache -> nothing */
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
        emit_exc_stamp(e, ck, old_pc);      /* collapse-safe caret (#56) */
        emit_call_epilogue(e);              /* SO: re-mat rsi/r8 */
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
        emit_call_epilogue(e);              /* re-mat rsi/r8 */
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
                        std::vector<Fixup> &fixups, size_t old_pc)
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
        /* lever A consumer: the accumulate VALUE may arrive in RAX.
         * `+` commutes, so SWAP the roles - the accumulator loads into
         * RCX and rax = value + accumulator (no move-aside). */
        const bool fb = g_fwd.in_rax >= 0 && !in.b_is_lit()
            && in.b_slot() == g_fwd.in_rax;
        if (fb) {
            emit_fwd_bump(e);
            read_slot(e, RCX, adst);
        } else {
            read_slot(e, RAX, adst);
            load_operand(e, RCX, in.b_is_lit(), in.b_lit(), in.b_slot());
        }
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
            emit_exc_stamp(e, ck, pc);           /* cold: the condition caret
                                                  * (pc-independent -> the op
                                                  * is deletable) */
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
        const auto off = [](int slot) {
            return static_cast<int32_t>(static_cast<long>(slot)
                                        * static_cast<long>(sizeof(LValue)));
        };
        /* #56: TWO slow tiers, because the gate must precede the step (a
         * re-run would double-step): a GATE decline runs the FULL op in
         * jit_for_step_elem (step + test + read, the interpreted body), a
         * post-step READ decline (wrap/OOB/kind) runs jit_elem_int_value
         * on the already-stepped counter. Neither bails -> deletable. */
        std::vector<size_t> gate_slows, read_slows;
        e.bump_op(OpCode::ForStepElemInt);   /* before any loads (rax!) */
        /* C1d: in a hoisted region the preheader already proved the base
         * and its ONE kind - no gate (so no SLOW B and no double-step
         * hazard: nothing bail-able precedes the step) and no per-element
         * kind dispatch; the read is bounds-vs-r11 + a read off r10, with
         * the post-step value tier (SLOW A) serving negative/OOB exactly
         * as before. */
        const bool hoisted = g_hoist.active
            && in.b_dual_lo() == g_hoist.base
            && g_hoist.kind == (in.elem_bool_hint() ? 3 : 0);
        if (!hoisted)
            emit_elem_base_gate(e, in.b_dual_lo(), pc, &gate_slows);
        read_slot(e, RAX, in.target2);
        e.u8(0x48); e.u8(0xFF); e.u8(up ? 0xC0 : 0xC8);   /* inc/dec rax */
        write_slot(e, ck, RAX, in.target2, pc);
        load_operand(e, RCX, in.a_is_lit(), in.a_lit(), in.a_slot());
        e.u8(0x48); e.u8(0x39); e.u8(0xC8);               /* cmp rax, rcx */
        const size_t j_fall = e.j32(cc_for(cc_negate(in.aop)).short_op);
        if (hoisted) {
            load_slot_r9(e, in.target2);      /* the stepped counter */
            e.cmp_r9_hr(g_hoist.rcount);
            read_slows.push_back(e.j32(0x73));
            if (g_hoist.kind == 3)
                e.load_elem_byte_hr(g_hoist.rdata);
            else
                e.load_elem_int_hr(g_hoist.rdata);
        } else {
        /* the trusted read: the gate proved kind is ints or bools */
        e.load(RAX, base.payload);            /* rax = shobj */
        e.cmp_byte_rax(L.kind_off, L.kind_bools);
        const size_t j_bools = e.j32(0x74);
        emit_flat_int_tail(e, pc, /*bools=*/false, nullptr, in.target2,
                           &read_slows);
        const size_t j_done = e.j32(0xEB);
        e.patch32_here(j_bools);
        emit_flat_int_tail(e, pc, /*bools=*/true, nullptr, in.target2,
                           &read_slows);
        e.patch32_here(j_done);
        }
        write_slot(e, ck, RAX, in.b_dual_hi(), pc);
        std::vector<size_t> j_takens;
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
        /* SLOW A - the post-step READ decline: the counter is already
         * stepped and the branch is taken; read via the interpreter core,
         * write the elem slot, then take the same branch. */
        if (!read_slows.empty()) {
            for (const size_t j : read_slows)
                e.patch32_here(j);
            read_slot(e, RAX, in.target2);           /* the stepped counter */
            emit_call_prologue(e);
            e.mov_rr(RSI, RAX);
            e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_elem_tmp));
            e.lea_rdi(off(in.b_dual_lo()));
            e.call_relocs.push_back(
                { e.pos(),
                  reinterpret_cast<const void *>(jit_elem_int_value) });
            e.u8(0xE8); e.u32(0);
            emit_call_epilogue(e);
            e.u8(0x85); e.u8(0xC0);
            {
                const size_t j_ok = e.j8(0x74);
                emit_exc_stamp(e, ck, old_pc);
                e.exit_pc(pc);
                e.patch8(j_ok, e.pos());
            }
            e.movabs(RAX, reinterpret_cast<uint64_t>(&g_jit_elem_tmp));
            e.u8(0x48); e.u8(0x8B); e.u8(0x00);      /* mov rax,[rax] */
            write_slot(e, ck, RAX, in.b_dual_hi(), pc);
            const size_t tgt = static_cast<size_t>(in.target);
            if (tgt >= begin && tgt < end) {
                e.u8(0xE9);
                fixups.push_back({ e.pos(), tgt });
                e.u32(0);
            } else {
                e.exit_pc(static_cast<uint32_t>(remap[in.target]));
            }
        }
        /* SLOW B - the GATE decline (nothing stepped yet): the FULL op in
         * jit_for_step_elem. 0 = fell through, 1 = taken (elem written),
         * 2 = threw. */
        if (!gate_slows.empty()) {
            for (const size_t j : gate_slows)
                e.patch32_here(j);
            load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
            emit_call_prologue(e);
            e.mov_rr(RSI, RAX);                      /* the bound VALUE */
            e.movabs(RDI, static_cast<uint64_t>(
                              static_cast<int_type>(in.target2)));
            e.movabs(RDX, static_cast<uint64_t>(static_cast<int>(in.aop)));
            e.movabs(RCX, static_cast<uint64_t>(
                              static_cast<int_type>(in.b_dual_lo())));
            e.movabs_r8(static_cast<uint64_t>(
                            static_cast<int_type>(in.b_dual_hi())));
            e.call_relocs.push_back(
                { e.pos(),
                  reinterpret_cast<const void *>(jit_for_step_elem) });
            e.u8(0xE8); e.u32(0);
            emit_call_epilogue(e);
            e.u8(0x83); e.u8(0xF8); e.u8(0x02);      /* cmp eax, 2 */
            {
                const size_t j_nothrow = e.j8(0x75);
                emit_exc_stamp(e, ck, old_pc);
                e.exit_pc(pc);
                e.patch8(j_nothrow, e.pos());
            }
            e.u8(0x85); e.u8(0xC0);                  /* test eax, eax */
            j_takens.push_back(e.j32(0x75));         /* jnz -> taken */
            const size_t j_ff = e.j32(0xEB);         /* jmp fall-through */
            for (const size_t j : j_takens) {
                e.patch32_here(j);
                const size_t tgt = static_cast<size_t>(in.target);
                if (tgt >= begin && tgt < end) {
                    e.u8(0xE9);
                    fixups.push_back({ e.pos(), tgt });
                    e.u32(0);
                } else {
                    e.exit_pc(static_cast<uint32_t>(remap[in.target]));
                }
            }
            e.patch32_here(j_ff);
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
            emit_exc_stamp(e, ck, old_pc);       /* collapse-safe (#56) */
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
         * int-semantics path (flat ints or bools) and jump to target when
         * it is FALSE. Nothing is written - the fused temp was proven dead
         * on both paths. #56: every DECLINED shape (non-array/slice/
         * general/wrong kind, negative wrap, OOB) calls jit_elem_int_value
         * (the interpreter core, conveying) instead of bailing, so the op
         * never re-interprets and its run's originals delete. Both paths
         * CONVERGE with the value in rax before the branch. */
        std::vector<size_t> slows;
        emit_elem_int_read(e, in, pc, &slows);
        const size_t j_conv = e.j32(0xEB);          /* jmp converge */
        for (const size_t j : slows)
            e.patch32_here(j);
        {   /* slow: rdi = &slot[base], rsi = idx, rdx = &g_jit_elem_tmp
             * (a file-static scratch - no rsp juggling inside the
             * prologue's frame). The status/value convention mirrors the
             * other conveying helpers: the epilogue runs FIRST (rax + the
             * scratch survive it), then test / stamp / exit. */
            const auto off = [](int slot) {
                return static_cast<int32_t>(static_cast<long>(slot)
                                            * static_cast<long>(sizeof(LValue)));
            };
            load_operand(e, RAX, in.a_is_lit(), in.a_lit(), in.a_slot());
            emit_call_prologue(e);
            e.mov_rr(RSI, RAX);                     /* the index value */
            e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_elem_tmp));
            e.lea_rdi(off(in.target2));             /* rdi = &slot[base] */
            e.call_relocs.push_back(
                { e.pos(),
                  reinterpret_cast<const void *>(jit_elem_int_value) });
            e.u8(0xE8); e.u32(0);
            emit_call_epilogue(e);
            e.u8(0x85); e.u8(0xC0);                 /* test eax, eax */
            const size_t j_ok = e.j8(0x74);
            emit_exc_stamp(e, ck, old_pc);          /* the op's own caret */
            e.exit_pc(pc);                          /* threw -> re-raise */
            e.patch8(j_ok, e.pos());
            e.movabs(RAX, reinterpret_cast<uint64_t>(&g_jit_elem_tmp));
            e.u8(0x48); e.u8(0x8B); e.u8(0x00);     /* mov rax, [rax] */
        }
        e.patch32_here(j_conv);                     /* converge: */
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
        emit_float_load(e, X0, in.a_is_lit(), in.a_flit(), in.a_slot(), pc,
                        /*no_bail=*/true);
        emit_float_load(e, X1, in.b_is_lit(), in.b_flit(), in.b_slot(), pc,
                        /*no_bail=*/true);
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
        || op == OpCode::JumpIfNotNoneV;
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
        case OpCode::LoadElem2Float:     /* #94 inline tier: float dst */
        case OpCode::StoreElem2V:        /* #95 inline tier: the float-row
                                          * arm's val-type guard reads r8 */
        case OpCode::LoadMemberFloat:    /* baked fast path: float store */
            return true;
        case OpCode::StructCtorV:        /* a planned float field reads via
                                          * emit_float_load -> needs r8 */
            if (ck.code[pc].b_dual_hi() >= 0)
                for (const Chunk::CtorPlanField &pf :
                         ck.ctor_plans[ck.code[pc].b_dual_hi()].f)
                    if (pf.act == 1)
                        return true;
            break;
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
    /* generic IntBin: the NON-THROWING arms never return an interior pc.
     * The div/mod/shift arms EXIT (they convey JR_DIV0/JR_NEG_SHIFT via
     * jit_raise_kind_exc + the exc-stamp) - deletable, but NOT leaf-safe,
     * so they stay out of this predicate and live in op_fully_native's
     * convey section, like the reg-shift RR forms. (Own case, NOT part of
     * the fall-through chain below - the nested switch would swallow the
     * chain's earlier labels.) */
    case OpCode::IntBin:
        switch (in.aop) {
        case Op::plus: case Op::minus: case Op::times:
        case Op::band: case Op::bor:  case Op::bxor:
            return true;
        default:                       /* div/mod/shl/shr/ushr raise */
            return false;
        }
    /* A PLANNED StructCtorV (the 64_struct_create fix) NEVER exits: the
     * native fast path is guards + direct byte stores, and its slow branch
     * calls jit_struct_ctor_planned - vm_struct_ctor_planned never throws
     * (every field act was compile-proven). Leaf-safe AND deletable. An
     * UNPLANNED ctor (nested-struct field) keeps the defensive-throw exit. */
    case OpCode::StructCtorV:
        return in.b_dual_hi() >= 0;
    /* FLOAT arith (the no_bail tier, 2026-07-25): the operand reads
     * promote int/bool exactly like read_float_slot (the 2-way no_bail
     * form), so add/sub/mul cannot exit at all - never-exits, leaf-safe.
     * div/mod CONVEY a zero-divisor DivisionByZeroEx (raise_convey_unless)
     * - deletable (op_fully_native) but never leaf-safe. */
    case OpCode::FloatBin:
        switch (in.aop) {
        case Op::plus: case Op::minus: case Op::times:
            return true;
        default:                       /* div/mod convey */
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
    case OpCode::FloatAddRR: case OpCode::FloatSubRR:
    case OpCode::FloatMulRR: case OpCode::FloatAddRI:
    case OpCode::FloatSubRI: case OpCode::FloatMulRI:
    case OpCode::LoadImmFloat:
    case OpCode::JumpUnlessFloatCmp:
    case OpCode::CmpFloatV:            /* float compare -> bool; no_bail
                                        * reads, ucomisd cannot fault */
    case OpCode::MathFnV:              /* sqrtsd / a libm call - no exit
                                        * (arity/type compile-excluded) */
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
     * bail (LoadElemValue bounds-checks, so it lives in op_fully_native's
     * convey family instead - it exits, but only by conveying). */
    case OpCode::LoadElemBool:
    case OpCode::StrLen:
    case OpCode::LoadStrChar:
    case OpCode::LoadStructFieldInt:
    case OpCode::LoadStructFieldFloat:
    case OpCode::LoadStructElemV:
    /* #56: dst = other + a[i].f - the helper is never-throwing (the field
     * read is inference-proven no-fault) and the add/store run in the
     * fragment; no exit of any kind. */
    case OpCode::StructFieldAddInt:
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
    case OpCode::OrdCharV:          /* only OOB, conveyed + exc-stamped */
    case OpCode::SliceV:
    case OpCode::DictLoadInt:
    case OpCode::DictLoadFloat:
    case OpCode::CoerceNumV:
    case OpCode::MemberV:
    case OpCode::LoadGlobalV:
    case OpCode::CheckCallableV:    /* convey-only guard, exc-stamped caret */
    case OpCode::JumpUnlessTrueV:   /* inline int/bool test; the is_true slow
                                     * path conveys, exc-stamped - no bail */
        return true;
    /* The IncDec family: elem/member/chain throws already carry their
     * POOLED dual carets (incdec_sites/incdec_chains), the scalar's
     * loc-less TypeError gets the cold-side exc-stamp - all
     * pc-independent. Only an undefined-GLOBAL base/root BAILS, so the
     * global kind stays non-deletable (kind rides target2 for the scalar,
     * target for elem/member, a_lit for the chain; chain kind 3 is an
     * RVALUE root - no bail). */
    case OpCode::IncDecCheckedV:
        return in.target2 != 1;
    case OpCode::IncDecElemCheckedV:
    case OpCode::IncDecMemberCheckedV:
        return in.target != 1;
    case OpCode::IncDecChainV:
        return in.a_lit() != 1;
    /* LoadElemValue (the general/str-array element read incl. 2-D
     * `a[i][k]`): the OOB conveys (exc-stamped caret) and the
     * unreachable-by-inference non-array/wrong-kind tail conveys the
     * interpreted InternalErrorEx via eptr - no bail left. */
    case OpCode::LoadElemValue:
        return true;
    /* #56 delete-originals: LoadElemInt/Float - the inline fast path's
     * every decline (slice/kind/wrap/OOB) goes to the jit_load_elem_*
     * slow tier (the interpreter's exact core; OOB conveys, the
     * InternalErrorEx net rides eptr) - no bail, no re-interpret. */
    case OpCode::LoadElemInt:
    case OpCode::LoadElemFloat:
        return true;
    /* The fused nested read: one helper call, every shape handled inside
     * it (the interpreter's exact cores), OOB conveying with the BAKED
     * per-level caret and the InternalErrorEx net riding eptr. No bail. */
    case OpCode::LoadElem2Int:
    case OpCode::LoadElem2Float:
        return true;
    /* #56 inc 2: the LV-builtin family - the helpers run the full
     * interpreter path (fast append + the pooled-caret fallback / the
     * shared LV dispatch) and every throw now CONVEYS with the op's
     * exc-stamped caret (plus the eptr net for a plain Exception). No
     * bail, no re-interpret -> deletable. */
    case OpCode::AppendV:
    case OpCode::CallBuiltinLV:
    case OpCode::CallBuiltinLVElem:
    case OpCode::CallBuiltinLVMember:
        return true;
    /* #56 step 4: the sync CALLS - every decline is gone (errors convey
     * with baked/stamped carets; the chunk-less callee runs as a boundary
     * call IN the helper; the depth cap SWITCHES interpreted-flat with the
     * record resuming at the post-call stub). NEVER op_never_exits (they
     * exit by convey/switch - not leaf-safe). */
    case OpCode::CallV:
    case OpCode::CachedCallV:
    case OpCode::CallValueV:
        return true;
    /* #56 (the small-batch increment): throws convey with exc-stamped /
     * pooled carets; MapFilterV's plain callback throws ride eptr; no
     * bail in any of them. */
    case OpCode::MultiUnpackV:
    case OpCode::CheckFuncV:
    case OpCode::MapFilterV:
    case OpCode::LoadMemberInt:
    case OpCode::LoadMemberFloat:
        return true;
    /* #56: the dyn-foreach pair - Init's non-container TypeErrorEx and
     * Next's strict-unpack throws convey with the exc-stamped container
     * caret; the exhausted/bound paths are fragment-local branches. */
    case OpCode::ForeachDynInit:
    case OpCode::ForeachDynNext:
        return true;
    /* #56: the #9 fusions - both keep their inline flat fast paths and
     * route EVERY decline (base gate, kind, negative wrap, OOB) to a slow
     * tier running the interpreter core (jit_elem_int_value; the gate
     * decline of ForStepElemInt to the full-op jit_for_step_elem, since
     * the gate precedes the step). Throws convey exc-stamped; no bail. */
    case OpCode::JumpUnlessElemInt:
    case OpCode::ForStepElemInt:
        return true;
    /* #56: the native `throw` - jit_throw runs the interpreted body (build
     * + vm_raise) and the fragment either resumes at the handler pc, stops
     * the invocation (boundary), or conveys the builder's TypeErrorEx. It
     * never returns its OWN pc for a re-run -> deletable. (CatchTest and
     * Reraise stay kept: the handler dispatch JUMPS to their pcs.) */
    case OpCode::Throw:
        return true;
    /* #56: the struct/unpack builders - their helpers run the shared
     * interpreter cores (vm_do_emplace / construct_struct_from_values /
     * the strict unpack) and every throw conveys with the exc-stamped
     * caret (the per-field/arg carets ride their own pools); no bail. */
    case OpCode::EmplaceStruct:
    case OpCode::MakeStructArrayV:
    case OpCode::UnpackElemInt:
    case OpCode::UnpackElemFloat:
    case OpCode::UnpackElemValue:
    case OpCode::UnpackElemTargets:
    /* ... and the dict LITERAL, the same shape (jit_make_dict runs the shared
     * build_dict_from_pairs; an UNHASHABLE key is its only throw and conveys
     * with the exc-stamped caret).  Its MakeArrayV twin is never-exits - a
     * dict differs only in that it FREEZES + HASHES each key.  This one was
     * invisible until samples/phonebook started compiling again (the
     * value-template inference fix), which is why it missed the batch. */
    case OpCode::MakeDictV:
        return true;
    /* The raise-kind int arms: div/mod (a zero divisor) and the reg-count
     * shifts (a negative count) now CONVEY via jit_raise_kind_exc + the
     * exc-stamp instead of the g_vm_jit_raise signal (whose exception got
     * its caret from loc_at at the exit pc - wrong once collapsed). The
     * non-throwing IntBin arms were already never-exits; with the throwing
     * arms conveying, EVERY IntBin arm is deletable, as are the RR shifts
     * (the RI shifts were never-exits all along - a negative imm count is
     * compile-excluded). */
    case OpCode::IntBin:
    case OpCode::IntShlRR:
    case OpCode::IntShrRR:
    /* FloatBin div/mod convey their zero-divisor throw (the add/sub/mul
     * arms are already never-exits) - every FloatBin arm is deletable. */
    case OpCode::FloatBin:
    /* ThrowRuntimeV: builds its pooled exception natively (exc/eptr by
     * kind, pooled caret) - conveys, never re-executes. */
    case OpCode::ThrowRuntimeV:
    /* #78 step E: EndFinally's cold RERAISE arm calls jit_end_finally,
     * which runs the interpreted body's vm_raise and reports
     * dispatched / boundary / conveyed like the native `throw` - no bail
     * left, so a try/FINALLY region deletes like everything else.
     * #80: `rethrow` is the same shape (jit_rethrow), plus the site's
     * caret restamp. */
    case OpCode::EndFinally:
    case OpCode::Rethrow:
        return true;
    /* The STORE family (increment 2). StoreElemInt (local-only eligible) /
     * DictStore / StoreElem2V: convey-only helpers, cold-side caret.
     * StoreElemFloat joined once its emitted VALUE load moved to the
     * no-bail 2-way form (float -> movsd, else -> cvtsi2sd - the release
     * interpreter's exact int/bool promotion), removing its only exit. */
    case OpCode::StoreElemInt:
    case OpCode::StoreElemFloat:
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
            /* M5a: the exclusion's rationale was the CAP round-trip
             * pathology (every level past 200 paid enter->bail->exit->
             * re-dispatch, +14% on 10) - with the native stack armed the
             * cap is ~500k and unreachable in practice, so direct
             * self-recursion becomes a native fragment self-call. The
             * old gate stays when the stack is off (ASan / kill switch /
             * mmap failure). */
            return jit_sync_depth_cap() > 1000;
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
     * dyn-callee generic call pair (CheckCallableV + CallValueGenericV) is
     * op_run_eligible too now (2026-07-25) - the island-source hop chain is
     * EXHAUSTED: no sequential op remains that this list admits but
     * op_run_eligible rejects, so jit_try_container forms no containers on
     * real code and the jit_exec_block mechanism is covered by the
     * synthetic vm_exec_block_selftest (kept for future un-nativizable
     * ops). (The island-source hop chain: SliceV -> MakeArrayV -> MakeDictV
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
 * re-raises g_vm_jit_exc. The prologue/epilogue re-materialise rsi=t_int /
 * r8=t_float; the base is in callee-saved rbx and the cache is empty in a
 * container (no N5), so the prologue emits nothing and rsp is already
 * 16-aligned for the call. */
static void emit_island_call(Emitter &e, const FuncDescriptor *desc,
                             uint32_t island_pc)
{
    emit_call_prologue(e);                 /* empty cache -> nothing */
    e.movabs(RDI, reinterpret_cast<uint64_t>(desc));        /* arg1 = desc */
    e.movabs(RSI, island_pc);                               /* arg2 = from_pc */
    e.call_relocs.push_back(
        { e.pos(), reinterpret_cast<const void *>(jit_exec_block) });
    e.u8(0xE8); e.u32(0);                                    /* call rel32 */
    emit_call_epilogue(e);                 /* rsi=t_int; r8=t_float */
    e.u8(0x48); e.u8(0x85); e.u8(0xC0);                      /* test rax, rax */
    e.u8(0x79); const size_t jfix = e.pos(); e.u8(0);        /* jns +over (rel8)*/
    e.u8(0xB8); e.u32(island_pc);                    /* mov eax, island_pc */
    e.frag_ret();                       /* ret -> EnterNative re-raises */
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
    e.fcache.clear();                      /* C2a: nor a float one */
    std::vector<NativeCode::OpMark> marks;  /* -vdj: op-boundary annotations */
    std::vector<size_t> label(n, 0);        /* fragment offset of each body pc */
    std::vector<Fixup> fixups;              /* fragment-local branch fixups */
    e.frag_entry();                         /* push rbx; rbx = rdi (the base) */
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
                        static_cast<uint32_t>(remap[pc]), 0, n, remap, fixups,
                        pc);
        else if (!emit_op(e, chunk, chunk.code[pc],
                          static_cast<uint32_t>(remap[pc]), jc, pc))
            return false;                  /* selection miss: chunk pristine */
        pc++;
    }
    for (const Fixup &f : fixups)          /* patch each fragment-local branch */
        e.patch32(f.site,
                  static_cast<uint32_t>(label[f.target_pc] - (f.site + 4)));
    /* The body ends in a native ReturnV (emit_op emitted its `ret`); every
     * other exit is a branch to an in-body label, so no trailing exit_pc.
     * An island call CAN exit though (RAISED), so the shared tail follows. */
    e.emit_epilogues();

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
    /* #78: the handler table's pcs are resumes into ordinary code, remapped
     * exactly like the branch targets above (this path keeps every original,
     * so there is no entry map here). */
    for (Chunk::HandlerSite &hs : chunk.handler_sites) {
        for (Chunk::HandlerClause &cl : hs.clauses)
            if (cl.body_pc >= 0 && static_cast<size_t>(cl.body_pc) <= n)
                cl.body_pc = static_cast<int32_t>(remap[cl.body_pc]);
        if (hs.fin_pc >= 0 && static_cast<size_t>(hs.fin_pc) <= n)
            hs.fin_pc = static_cast<int32_t>(remap[hs.fin_pc]);
    }
    verify_handler_sites(chunk);

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
    jit_native_stack_init();   /* M5a: arm the stack + raise the cap BEFORE
                                * any guard bakes jit_sync_depth_cap() */
    if (!g_jit_enabled || chunk.code.empty())
        return;

    /* Lever A: neutral forwarding state BEFORE any emission - the
     * container path below shares emit_op but not the pairing protocol,
     * and a stale prod/in_rax from a previous chunk's last op would
     * otherwise leak into its cases. */
    g_fwd = JitFwd{};

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

    /* Lever A: temp live-out + branch/handler-target flags over the
     * FINAL code (post-splice - this runs at JIT time), from codegen's
     * audited enumerations. Without computable liveness the pairs still
     * forward READS; only the write elision needs deadness. */
    std::vector<uint64_t> fwd_lout;
    std::vector<char> fwd_tgt;
    const bool fwd_live_ok = jit_fwd_info(chunk, fwd_lout, fwd_tgt);
    g_fwd = JitFwd{};

    /* op -> enum NAME (the audit's label; from the X-macro, so it can
     * never drift from the opcode list) */
    static const auto jit_op_name = [](OpCode op) -> const char * {
        switch (op) {
#define X(o) case OpCode::o: return #o;
        ML_FOR_EACH_OPCODE(X)
#undef X
        default: return "?";
        }
    };

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
            /* #56 (2026-07-30): the guard is GONE - distinct chains are fine
             * now. A conveying fragment BAKES its op's chain into the
             * exception (emit_exc_stamp -> Exception::jit_inline_frame, which
             * vm_flush_inline prefers), so a raise no longer has to resolve
             * one from the collapsed pc; and an exception PROPAGATING through
             * a call resolves it from the call record's baked chain
             * (VmCallRec::inline_frame). Both are pc-independent, which is
             * exactly what deletion breaks. `can_raise` is kept only as the
             * cheap gate below. */
            (void)can_raise;
        }
        deletable[r] = ok;

        /* #56 delete-originals AUDIT (env MYLANG_DELAUDIT=1): print each
         * NON-deletable run's blocking reason + opcodes, so the corpus-wide
         * histogram says what to nativize/relax next (the M1-plan pattern:
         * an audit surface first, then the increments). Dev-only output;
         * zero cost when the env is unset. */
        if (!ok && getenv("MYLANG_DELAUDIT")) {
            std::string why, ops_s;
            bool full = true;
            for (size_t p = b; p < en; p++)
                if (!op_fully_native(chunk.code[p])) {
                    full = false;
                    ops_s += std::string(" ")
                        + jit_op_name(chunk.code[p].op);
                }
            if (!full)
                why = "bail-op:";
            else {
                bool multi = false;
                for (size_t p = 0; p < n && !multi; p++) {
                    const int t = branch_pc_target(chunk.code[p]);
                    if (t > static_cast<int>(b) && t < static_cast<int>(en)
                            && (p < b || p >= en))
                        multi = true;
                }
                why = multi ? "multi-entry" : "inline-raise";
            }
            fprintf(stderr, "DELAUDIT run[%zu,%zu) %s%s\n",
                    b, en, why.c_str(), ops_s.c_str());
        }
    }

    /* pc remap: every run head gains one inserted EnterNative; a DELETABLE
     * run's interior ops are removed, so every one of its pcs maps to the
     * EnterNative (only the head is ever a target - single-entry). */
    /* PER-PC ENTRY POINTS, increment 1 (post-call resume; plans/native-gap-
     * roadmap.md lever 1): every in-VM call op (CallV/CachedCallV/
     * CallValueV) inside a KEPT run gets an EnterNative inserted DIRECTLY
     * AFTER it, pointing at a per-entry STUB (tag + cache entry loads +
     * a jump to the following op's fragment offset). An interpreted
     * return's resume pc is computed at RUNTIME as call_pc + 1, so it
     * LANDS ON the inserted op and re-enters native - no runtime lookup,
     * no remap ambiguity (no old pc maps to the inserted op; bails of the
     * following op still reach its original; loc_at(ret_chunk, ret_pc-1)
     * still hits the call op, so backtraces are untouched). A call as a
     * run's LAST op needs none (the next pc is outside the run). Deletable
     * runs can't contain a call (not op_fully_native), so entries live in
     * kept runs only. */
    /* Increment 2 (branch-target re-entry) UNIFIES the entry set: an
     * entry pc is any KEPT-run pc where control RESUMES from interpreted
     * flow - (a) the pc after an in-VM call op (a runtime ret_pc lands
     * there), (b) an INTERPRETED branch's target (the post-exception /
     * post-bail loop back edge - after one bail the old code walked the
     * whole remaining loop interpreted, the 42/70-class gap), (c) a
     * NATIVE branch's external exit (a resume too - the target op has
     * not attempted execution, unlike a bail at its OWN pc). A RUN-HEAD
     * target needs no insertion (the head EnterNative exists); an
     * INTERIOR entry pc gets an EnterNative inserted DIRECTLY BEFORE its
     * op + a stub. PushHandler's target (the MATCHER pc) is EXCLUDED
     * from entry mapping: a handler's first op (CatchTest) is an
     * exit-at-op native, so entering there would be
     * enter->exit->reinterpret - pure overhead. A CatchTest's target
     * (the CATCH BODY) is an ordinary resume and IS entry-mapped
     * (#74 inc 2), so a matched catch runs its body natively instead
     * of interpreting until the back edge.
     *
     * THE DUAL REMAP: `remap` (bails, exit-at-own-pc, side tables) maps
     * an entry pc to its ORIGINAL op - a bailed op must re-run
     * interpreted, never re-enter (which would loop). `entry_remap`
     * (branch-target fields, native external exits) maps a run head to
     * its head EnterNative and an interior entry pc to its inserted
     * EnterNative; every other pc is identical to `remap`. */
    std::vector<std::pair<size_t, size_t>> entries;   /* entry pc, stub */
    {
        const auto interior_of_kept = [&](size_t t) -> bool {
            for (size_t r = 0; r < runs.size(); r++)
                if (!deletable[r] && t > runs[r].begin && t < runs[r].end)
                    return true;
            return false;
        };
        const auto interior_of_deleted = [&](size_t t) -> bool {
            for (size_t r = 0; r < runs.size(); r++)
                if (deletable[r] && t > runs[r].begin && t < runs[r].end)
                    return true;
            return false;
        };
        for (size_t r = 0; r < runs.size(); r++) {
            /* #56 step 4: DELETABLE runs get post-call stubs too - the
             * SWITCH protocol's records resume there (a stub is the ONLY
             * pc emitted inside a deleted span). */
            for (size_t p = runs[r].begin; p + 1 < runs[r].end; p++) {
                const OpCode op = chunk.code[p].op;
                if (op == OpCode::CallV || op == OpCode::CachedCallV
                        || op == OpCode::CallValueV)
                    entries.push_back({ p + 1, 0 });
            }
            /*
             * #78 step D: the step-1 hang fix (a stub for PushHandler's
             * target when it landed inside a deleted span) is GONE with the
             * target itself - a pushed handler is just a region id now, and
             * the table loop below gives EVERY handler pc (clause bodies
             * and the shared finally) its resume point, deleted span or
             * not. That loop is the general form of the same fix.
             */
        }

        /*
         * #78 step C: EVERY HANDLER-TABLE pc IS A RESUME. The dispatch now
         * jumps STRAIGHT to a clause's body_pc or to the region's fin_pc, so
         * both need a real entry point when they land inside a run - a stub
         * if the run is DELETED (else the pc collapses onto the head
         * EnterNative and the region re-runs: the step-1 hang, which
         * resurfaced through fin_pc the moment the table drove dispatch),
         * and an interior entry if the run is KEPT (what #74 inc 2 already
         * did for CatchTest's target, now sourced from the table instead).
         */
        for (const Chunk::HandlerSite &hs : chunk.handler_sites) {
            const auto add = [&](int32_t t) {
                if (t < 0)
                    return;
                const size_t tt = static_cast<size_t>(t);
                if (interior_of_kept(tt) || interior_of_deleted(tt))
                    entries.push_back({ tt, 0 });
            };
            for (const Chunk::HandlerClause &cl : hs.clauses)
                add(cl.body_pc);
            add(hs.fin_pc);
        }
        for (size_t p = 0; p < n; p++) {          /* branch targets */
            const Instr &in = chunk.code[p];
            switch (in.op) {
            case OpCode::Jump:
            case OpCode::JumpUnlessIntCmp:
            case OpCode::JumpUnlessFloatCmp:
            case OpCode::JumpUnlessTrueV:
            case OpCode::JumpIfNotNoneV:
            case OpCode::ForLoopStep:
            case OpCode::DictIterNext:
            case OpCode::ForeachDynNext:
            case OpCode::JumpUnlessElemInt:
            case OpCode::IntAddStep:
            case OpCode::ForStepElemInt:
            /* #74 inc 2 gave the CATCH BODY an entry via CatchTest's
             * target; #78 step D deleted that op, and the handler-table
             * loop below supplies the same pcs (plus every fin_pc). */
            default:
                break;
            }
        }
        std::sort(entries.begin(), entries.end());
        entries.erase(std::unique(entries.begin(), entries.end(),
                                  [](const auto &a, const auto &b) {
                                      return a.first == b.first;
                                  }),
                      entries.end());
    }

    std::vector<int> remap(n + 1);
    std::vector<int> entry_remap(n + 1);
    {
        size_t r = 0, pc = 0, pe = 0;
        int np = 0;                               /* next NEW pc */
        while (pc <= n) {
            if (r < runs.size() && pc == runs[r].begin) {
                const size_t b = runs[r].begin, en = runs[r].end;
                const int en_pc = np++;           /* the head EnterNative */
                if (deletable[r]) {
                    for (size_t p = b; p < en; p++) {
                        remap[p] = en_pc;         /* all -> EnterNative */
                        if (pe < entries.size()
                                && entries[pe].first == p) {
                            /* #56 step 4: a post-call resume STUB - the
                             * only pc materialized in a deleted span
                             * (bails don't exist here; only the SWITCH
                             * record's ret_pc lands on it). */
                            entry_remap[p] = np++;
                            pe++;
                        } else {
                            entry_remap[p] = en_pc;
                        }
                    }
                } else {
                    for (size_t p = b; p < en; p++) {
                        if (pe < entries.size()
                                && entries[pe].first == p) {
                            /* the inserted interior EnterNative sits
                             * BEFORE the original: branches/resumes land
                             * on it, bails land past it */
                            entry_remap[p] = np++;
                            pe++;
                        } else {
                            entry_remap[p] =
                                p == b ? en_pc : np;  /* head -> its
                                                       * EnterNative */
                        }
                        remap[p] = np++;          /* the original op */
                    }
                }
                pc = en;
                r++;
                continue;
            }
            remap[pc] = np++;
            entry_remap[pc] = remap[pc];
            pc++;
        }
    }

    g_cur_entry_remap = &entry_remap;   /* #56: the emits bake resume pcs */
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

        /* N5: pin up to MAX_CACHED hot int slots for this run. The PICK
         * runs BEFORE the entry is emitted, because it decides which
         * callee-saved registers this fragment takes over and therefore
         * what frag_entry must push (and what every exit must pop). */
        e.cache.clear();
        e.fcache.clear();       /* C2a: per-RUN state like the GP pool - a
                                 * stale entry from the previous fragment
                                 * would flush a never-loaded xmm into the
                                 * slot at this fragment's epilogue (caught
                                 * as a wrong 40_math-shape sum: floor/abs
                                 * split the run, and fragment 2 flushed
                                 * fragment 1's pin) */
        e.saved.clear();
        std::vector<char> cache_barrier(end - begin, 0);

        /* C1: the hoisted (data, count) live in r10/r11 - CALLER-saved
         * registers the emitter freed when the N5 cache moved to
         * r12-r15, so hoisting costs the N5 pick NOTHING (an earlier
         * version reserved two callee-saved regs and the fragment-wide
         * pin loss ate the region-local win: 43_sieve +3.3%). The price
         * is that any helper call INSIDE the region clobbers them -
         * emit_call_epilogue re-derives both when g_hoist.active, the
         * single choke point every helper-call emission goes through.
         * A sync CALL would not re-derive, but calls cannot be in a
         * region (the whitelist), which is also what keeps r10/r11
         * unused there (only the M5b push emitter touches them).
         */
        g_hoist = JitHoist{};
        const std::vector<HoistRegion> hregs =
            jit_hoist_pick(chunk, begin, end, entries);
        std::vector<int> fhot;                    /* C2a float picks */
        const std::vector<int> hot =
            pick_cached_slots(chunk, begin, end, chunk.slot_count,
                              &cache_barrier, &fhot);
        for (size_t h = 0; h < hot.size(); h++)
            e.saved.push_back(CACHE_REGS[h]);

        e.frag_entry();               /* push rbx + the cache regs; rbx=rdi */
        e.movabs(RSI, reinterpret_cast<uint64_t>(jit_layout().t_int));
        if (run_has_float(chunk, begin, end))     /* r8 = t_float (N3) */
            e.movabs_r8(reinterpret_cast<uint64_t>(jit_layout().t_float));

        /* Load each pinned slot ONCE here (the back edge jumps to the
         * first op below, so the loop keeps them in registers; every exit
         * flushes them back). */
        for (size_t h = 0; h < hot.size(); h++) {
            const SlotAddr a = slot_addr(hot[h]);
            e.cache.push_back({ hot[h], a.payload, a.type, CACHE_REGS[h] });
            e.load(CACHE_REGS[h], a.payload);     /* entry load */
        }
        /* C2a: the float pins - xmm needs no push (caller-saved; the
         * C++ caller expects nothing preserved), only the entry loads.
         * A non-empty fhot implies run_has_float (float pins only arise
         * from float ops), so r8 = t_float is live for the flushes. */
        for (size_t h = 0; h < fhot.size(); h++) {
            const SlotAddr a = slot_addr(fhot[h]);
            e.fcache.push_back({ fhot[h], a.payload, a.type,
                                 FCACHE_REGS[h] });
            e.fload(FCACHE_REGS[h], a.payload);   /* entry load */
        }
#ifdef TESTS
        if (!fhot.empty()) {
            /* the execution proof: bumped per ENTRY of a float-pinned
             * fragment (the g_jit_hoist pattern) */
            e.movabs(RCX, reinterpret_cast<uint64_t>(&g_jit_fcache));
            e.u8(0x48); e.u8(0xFF); e.u8(0x01);   /* inc qword [rcx] */
        }
#endif


        std::vector<size_t> label(end - begin, 0);
        std::vector<NativeCode::OpMark> marks;
        /*
         * C1 is LOOP VERSIONING: the loop region [h_T, h_L] is emitted
         * with the hoisted registers live, its PREHEADER (the bytes just
         * before label[h_T] - only the fall-through entry runs them, the
         * back edges target the label after) verifying the base and
         * deriving (data, count); a failed guard jumps to a COLD copy of
         * the region alone (emitted after the run), whose exits rejoin
         * the shared stream. `emit_one` is the single per-op emitter both
         * passes share, so the copies cannot drift; `fixups`/`label`
         * belong to the MAIN stream, the cold pass swaps its own in.
         */
        std::vector<Fixup> fixups;
        std::vector<Fixup> *cur_fix = &fixups;
        /* per REGION: the failed-guard jnes -> that region's cold copy */
        std::vector<std::vector<size_t>> h_cold(hregs.size());
        g_fwd = JitFwd{};
        bool emit_ok = true;
        const auto emit_one = [&](size_t pc, bool in_cold,
                                  size_t cold_end) {
            const Instr &in = chunk.code[pc];
            if (g_jit_annotate)
                marks.push_back({ static_cast<uint32_t>(e.pos() - frag_off[r]),
                                  static_cast<uint32_t>(remap[pc]) });

            /* Lever A protocol. in_rax is one-shot: it names the temp
             * whose value the JUST-EMITTED producer left in RAX (nothing
             * is emitted between two ops - labels and marks are
             * metadata - so RAX survives the boundary). Then decide
             * whether THIS op produces for the next one - refused when
             * the C1 preheader's navigation bytes would intervene. */
            g_fwd.in_rax = g_fwd.armed ? g_fwd.prod : -1;
            g_fwd.prod = -1;
            g_fwd.skip_write = false;
            g_fwd.armed = false;
            int fdst;
            bool next_is_preheader = false;
            for (const HoistRegion &hr : hregs)
                if (pc + 1 == hr.T)
                    next_is_preheader = true;
            if (pc + 1 < end
                    && !(!in_cold && next_is_preheader)
                    && !(in_cold && pc + 1 > cold_end)
                    && !cache_barrier[pc - begin]
                    && !cache_barrier[pc + 1 - begin]
                    && jit_fwd_producer(in, fdst)
                    && fdst >= chunk.slot_count          /* a TEMP only */
                    && jit_fwd_consumer(chunk.code[pc + 1], fdst)
                    && !fwd_tgt[pc + 1]
                    && !std::binary_search(
                           entries.begin(), entries.end(),
                           std::make_pair(pc + 1, size_t(0)),
                           [](const std::pair<size_t, size_t> &x,
                              const std::pair<size_t, size_t> &y) {
                               return x.first < y.first;
                           })) {
                g_fwd.prod = fdst;
                const int tb = fdst - chunk.slot_count;
                g_fwd.skip_write = fwd_live_ok && tb < 64
                    && !(fwd_lout[pc + 1] & (uint64_t(1) << tb))
                    && !jit_slot_ref_listed(chunk, fdst);
            }

            /* An op that touches slots the emitter cannot enumerate is
             * BRACKETED: flush the pinned registers so it reads current values,
             * reload after so anything it wrote is picked up (the ordinary
             * spill-around-a-call). Never a branch op, so the reload always
             * executes. */
            const bool brk = cache_barrier[pc - begin] && !e.cache.empty();
            std::vector<Emitter::CacheEnt> saved_cache, saved_fcache;
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
                saved_fcache = std::move(e.fcache);
                e.fcache.clear();
            }
            if (op_is_branch(in.op)) {
                /* targets get entry_remap (an external exit is a RESUME -
                 * enter the target run natively); the op's OWN pc (arg 4,
                 * its bail) stays ordinary remap */
                emit_branch(e, chunk, in, static_cast<uint32_t>(remap[pc]),
                            begin, end, entry_remap, *cur_fix, pc);
            } else if (!emit_op(e, chunk, in,
                                static_cast<uint32_t>(remap[pc]), jc, pc)) {
                emit_ok = false;           /* selection bug: give up */
            }
            if (brk) {
                e.cache = std::move(saved_cache);
                e.fcache = std::move(saved_fcache);
                e.reload_cache();
            }
        };

        size_t hri = 0;                       /* the next region to enter */
        for (size_t pc = begin; pc < end && emit_ok; pc++) {
            if (g_hoist.active && hri > 0
                    && pc == hregs[hri - 1].L + 1)
                g_hoist.active = false;
            if (hri < hregs.size() && pc == hregs[hri].T) {
                /* the PREHEADER: guards + (data, count) derivation. Back
                 * edges target label[T], recorded AFTER these bytes. */
                const HoistRegion &hr = hregs[hri];
                g_hoist.base = hr.base;
                g_hoist.kind = hr.kind;
                g_hoist.rdata = 10;
                g_hoist.rcount = 11;
                g_hoist.store_ok = hr.has_store;
                const JitLayout &L = jit_layout();
                const SlotAddr hb = slot_addr(hr.base);
                std::vector<size_t> &cold = h_cold[hri];
                e.load(RAX, hb.type);
                e.movabs_r9(reinterpret_cast<uint64_t>(L.t_arr));
                e.cmp_rax_r9();
                cold.push_back(e.j32(0x75));
                e.cmp_byte_slot(hb.payload + L.slice_off, 0);
                cold.push_back(e.j32(0x75));
                e.load(RAX, hb.payload);
                e.cmp_byte_rax(L.kind_off,
                               hr.kind == 0 ? L.kind_ints
                             : hr.kind == 1 ? L.kind_floats
                             : hr.kind == 3 ? L.kind_bools
                                            : L.kind_general);
                cold.push_back(e.j32(0x75));
                if (hr.has_store) {
                    /* C1b: the STORE guard set, all region-stable
                     * (nothing in a region can freeze, rebind, or
                     * create views), and the hash invalidated ONCE -
                     * setting hash_valid=0 early only means "recompute
                     * later", so the per-element store needs neither
                     * the shobj nor a third register. */
                    e.cmp_byte_slot(hb.type + (L.lv_const_off
                                               - L.off_type), 0);
                    cold.push_back(e.j32(0x75));  /* const slot */
                    e.cmp_byte_rax(L.ro_off, 0);
                    cold.push_back(e.j32(0x75));  /* readonly */
                    e.cmp_byte_rax(L.slices_off, 0);
                    cold.push_back(e.j32(0x75));  /* live views */
                    e.mov_byte_rax_imm(L.hashv_off, 0);
                }
                e.mov_hr_rax(g_hoist.rdata, L.data_off);
                e.mov_hr_rax(g_hoist.rcount, L.data_off + 8);
                e.sub_hr_hr(g_hoist.rcount, g_hoist.rdata);
                if (g_hoist.kind == 0 || g_hoist.kind == 1)
                    e.sar_hr_3(g_hoist.rcount);   /* 8-byte elements;
                                                   * general (2) and
                                                   * bools (3) count
                                                   * BYTES */
#ifdef TESTS
                e.movabs(RDX, reinterpret_cast<uint64_t>(&g_jit_hoist));
                e.u8(0x48); e.u8(0xFF); e.u8(0x02);
#endif
                g_fwd = JitFwd{};                 /* the nav clobbered rax */
                g_hoist.active = true;
                hri++;
            }
            label[pc - begin] = e.pos();
            emit_one(pc, /*in_cold=*/false, end);
        }
        g_hoist.active = false;
        if (!emit_ok) {
            g_hoist = JitHoist{};
            e.b.clear();
            return;
        }
        const size_t exit_pos = e.pos();
        e.exit_pc(static_cast<uint32_t>(remap[end]));   /* fall-through */

        for (const Fixup &f : fixups) {    /* patch internal jumps */
            const size_t dst = label[f.target_pc - begin];
            e.patch32(f.site,
                      static_cast<uint32_t>(dst - (f.site + 4)));
        }

        /* the COLD region copies: the ordinary emission of each region
         * alone; region-internal branches patch against the copy's own
         * labels, exits against the main stream's, and the fall-through
         * end rejoins after the region. */
        for (size_t ri = 0; ri < hregs.size() && emit_ok; ri++) {
            const size_t cT = hregs[ri].T, cL = hregs[ri].L;
            for (const size_t j : h_cold[ri])
                e.patch32_here(j);
            g_fwd = JitFwd{};
            std::vector<size_t> cold_label(cL - cT + 1, 0);
            std::vector<Fixup> cold_fix;
            cur_fix = &cold_fix;
            for (size_t pc = cT; pc <= cL && emit_ok; pc++) {
                cold_label[pc - cT] = e.pos();
                emit_one(pc, /*in_cold=*/true, cL + 1);
            }
            cur_fix = &fixups;
            if (!emit_ok)
                break;
            /* fall through off the region end -> the shared stream */
            e.jmp32_to(cL + 1 == end ? exit_pos
                                     : label[cL + 1 - begin]);
            for (const Fixup &f : cold_fix) {
                const size_t dst =
                    (f.target_pc >= cT && f.target_pc <= cL)
                        ? cold_label[f.target_pc - cT]
                        : label[f.target_pc - begin];
                e.patch32(f.site,
                          static_cast<uint32_t>(dst - (f.site + 4)));
            }
        }
        if (!emit_ok) {
            g_hoist = JitHoist{};
            e.b.clear();
            return;
        }
        g_hoist = JitHoist{};

        /* PER-PC ENTRY STUBS (post-call resume): an interior offset cannot
         * be entered raw - fragment code assumes the HEAD's register
         * contract (rsi = t_int, r8 = t_float on a float run, the N5 cache
         * regs loaded). Each post-call entry gets a stub replaying exactly
         * the head's establishment sequence, then jumping to the op AFTER
         * the call. Sound: cache slots are resolved locals and memory is
         * CURRENT at any resume (every native exit flushes; interpreted
         * ops write memory). Emitted after the run body (labels final, so
         * the jump is a direct backward rel32); unmarked in -vdj (the
         * decoder resyncs at the next fragment's marks). */
        for (auto &pe : entries) {
            if (pe.first <= begin || pe.first >= end)
                continue;                /* interior entries only (a head
                                          * uses its run's own entry) */
            pe.second = e.pos();
            e.frag_entry();                      /* a stub IS an entry too */
            e.movabs(RSI, reinterpret_cast<uint64_t>(jit_layout().t_int));
            if (run_has_float(chunk, begin, end))
                e.movabs_r8(reinterpret_cast<uint64_t>(jit_layout().t_float));
            for (size_t h = 0; h < hot.size(); h++)
                e.load(CACHE_REGS[h], e.cache[h].payload);
            for (const Emitter::CacheEnt &c : e.fcache)
                e.fload(c.reg, c.payload);        /* C2a float pins */
#ifdef TESTS
            e.movabs(RCX,
                     reinterpret_cast<uint64_t>(&g_jit_entry_resume));
            e.u8(0x48); e.u8(0xFF); e.u8(0x01);   /* inc qword [rcx] */
#endif
            const size_t tgt = label[pe.first - begin];
            e.u8(0xE9);
            e.u32(static_cast<uint32_t>(
                tgt - (e.pos() + 4)));            /* jmp (backward) */
        }
        e.emit_epilogues();      /* the shared exit tail(s) for THIS run */
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
    nc.reserve(n + runs.size() + entries.size());
    {
        size_t r = 0, pc = 0, pe = 0;
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
                if (del) {
                    /* #56 step 4: the originals drop, but any post-call
                     * resume STUBS inside the span materialize (the SWITCH
                     * records' ret_pcs land on them; nothing else does). */
                    const size_t rb = pc;
                    for (pc = rb; pc < rend; pc++) {
                        if (pe < entries.size() && pc == entries[pe].first) {
                            Instr sen;
                            sen.op = OpCode::EnterNative;
                            Operand soff;
                            soff.is_lit = true;
                            soff.lit_kind = Operand::LitKind::i;
                            soff.lit =
                                static_cast<int_type>(entries[pe].second);
                            sen.set_a(soff);
                            nc.push_back(sen);
                            pe++;
                        }
                    }
                    continue;
                }
            }
            /* an interior entry pc: the EnterNative (its stub) sits
             * BEFORE the original - branch targets/ret_pcs land on it,
             * bail exits land past it on the original (dual remap) */
            if (pe < entries.size() && pc == entries[pe].first) {
                Instr en;
                en.op = OpCode::EnterNative;
                Operand off;
                off.is_lit = true;
                off.lit_kind = Operand::LitKind::i;
                off.lit = static_cast<int_type>(entries[pe].second);
                en.set_a(off);
                nc.push_back(en);
                pe++;
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
            case OpCode::JumpUnlessElemInt:
            case OpCode::IntAddStep:
            case OpCode::ForStepElemInt:
                /*
                 * A control-flow target is a RESUME, so it takes the ENTRY
                 * map (a run head's EnterNative, or an interior entry's
                 * inserted stub) - not the bail map, which points at the
                 * surviving original.
                 *
                 * KEEP A BODY ATTACHED TO THESE LABELS. They carry no code
                 * of their own; until this commit they fell through into
                 * `case OpCode::CatchTest:`, which owned the only copy of
                 * it. Deleting an opcode whose body is shared by
                 * fall-through silently turns every label above it into a
                 * no-op - here that would leave a surviving branch pointing
                 * into the PRE-insertion pc space.
                 *
                 * #78 step D: the handler pcs now live in
                 * Chunk::handler_sites, remapped through entry_remap right
                 * after this loop.
                 */
                if (in.target >= 0 && static_cast<size_t>(in.target) <= n)
                    in.target = entry_remap[in.target];
                break;
            default:
                break;
            }
            nc.push_back(in);
            pc++;
        }
        ML_CHECK(pe == entries.size());
    }
    chunk.code = std::move(nc);

    /*
     * Every surviving branch target must be a pc of the NEW code. The
     * rebuild inserts EnterNative heads, so a target that was not remapped
     * points into the pre-insertion pc space - at best one op early, at
     * worst past the end, where the dispatch reads whatever follows and
     * jumps through a garbage opcode.
     *
     * The switch above is the only thing standing between those two, and
     * its remap body is shared by fall-through across eleven opcodes - a
     * shape that already lost that body once when an opcode above it was
     * deleted. Assert the outcome rather than trust the next edit to it.
     */
#ifndef NDEBUG
    for (const Instr &bi : chunk.code)
        if (op_is_branch(bi.op))
            ML_CHECK_MSG(bi.target >= 0
                             && static_cast<size_t>(bi.target)
                                    <= chunk.code.size(),
                         "jit rebuild: branch target outside the new code");
#endif

    for (auto &l : chunk.locs)
        l.pc = static_cast<uint32_t>(remap[l.pc]);
    for (auto &ic : chunk.inline_ctxs)
        ic.pc = static_cast<uint32_t>(remap[ic.pc]);
    /* #78: the handler table's pcs are RESUMES (a catch body / the shared
     * finally), so they take entry_remap - the same map CatchTest's target
     * takes (#74 inc 2), NOT the bail map. */
    for (Chunk::HandlerSite &hs : chunk.handler_sites) {
        for (Chunk::HandlerClause &cl : hs.clauses)
            if (cl.body_pc >= 0 && static_cast<size_t>(cl.body_pc) <= n)
                cl.body_pc = static_cast<int32_t>(entry_remap[cl.body_pc]);
        if (hs.fin_pc >= 0 && static_cast<size_t>(hs.fin_pc) <= n)
            hs.fin_pc = static_cast<int32_t>(entry_remap[hs.fin_pc]);
    }
    verify_handler_sites(chunk);

    g_cur_entry_remap = nullptr;        /* #56: emission done */

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
    /* lever 1 step 5: a body that STARTS native is direct-`call`able by a
     * sync caller fragment (jit_sync_push_* returns base + this). */
    if (!chunk.code.empty() && chunk.code[0].op == OpCode::EnterNative)
        chunk.sync_entry_off = static_cast<int64_t>(chunk.code[0].a_lit());
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

void jit_cache_audit_report()
{
}

#endif
