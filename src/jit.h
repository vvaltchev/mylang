/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Native x86-64 AOT - the incremental baseline tier (plans/native-aot.md).
 *
 * jit_compile_chunk runs LAST in codegen_chunk (after the peephole,
 * extract_locs, ref_slots and specialize_arith_ops - and, later, after a
 * `.myv` load): it finds maximal straight-line RUNS of proven-scalar int
 * ops, compiles each into a frameless x86-64 fragment in a per-chunk
 * mmap'd W^X buffer (Chunk::native), INSERTS an EnterNative op at each
 * run head (remapping every pc field + the pc-keyed side tables with the
 * original ops left in place, so any BAIL pc resumes interpreted), and
 * flips the buffer executable.
 *
 * On unsupported platforms (non-x86-64, Windows) or under -nj /
 * MYLANG_JIT=0 it is a no-op and the interpreter runs everything - the
 * fallback story is inherent, not bolted on.
 */

#pragma once

#include <cstddef>   /* size_t (jit_enter) */
#include <cstdint>   /* uint32_t (the store helpers' pc) */
#include <vector>    /* JitCtx's slot tables (#55 STEP 2.1) */

#include "defs.h"    /* int_type */

struct Chunk;
class LValue;

/* The kill switch: -nj / MYLANG_JIT=0; always false off-platform. */
extern bool g_jit_enabled;

/* -vdj: record per-fragment op-boundary marks so the disassembler can
 * interleave the native code with the VM ops. Off (zero cost) normally. */
extern bool g_jit_annotate;

/* The Type-tag singletons the emitter bakes into `movabs` immediates
 * (rsi = int, r8 = float, r9 = array). The -vdj disassembler compares an
 * immediate against these to label it `<int-tag>` etc. rather than a raw
 * address. All null off-platform (no fragments there). */
void jit_type_singletons(const void *&t_int, const void *&t_float,
                         const void *&t_arr);

/* Fragments compiled process-wide (tests / -vd audit). */
extern unsigned long g_jit_frags;

struct FuncDescriptor;   /* #55 STEP 2.1 - native call (pointers only here) */
class LValue;

/*
 * #55 STEP 2.1: program context a caller's jit needs to decide+emit a NATIVE
 * CallV (a compile-time decision - jit_op_eligible(Instr) alone can't see it).
 *   slot_desc[global slot]      -> the callee FuncDescriptor* (null if that
 *                                  slot isn't a native-callable function),
 *   slot_reassigned[global slot]-> 1 if the slot is reassigned (NOT write-once),
 *   caller_desc                 -> THIS chunk's own function descriptor (its
 *                                  vm_chunk is the record's ret_chunk); null for
 *                                  main -> no native call from main in v1.
 * Null (the disasm / -rt / default path) -> no native calls emitted.
 */
struct JitCtx {
    const std::vector<const FuncDescriptor *> *slot_desc = nullptr;
    const std::vector<char> *slot_reassigned = nullptr;
    const FuncDescriptor *caller_desc = nullptr;
};

void jit_compile_chunk(Chunk &chunk, const JitCtx *jc = nullptr);

/*
 * plans/model-flip.md M1: the CONTAINER PLAN - a compile-time view of how a
 * chunk partitions into NATIVE segments (maximal runs of native-eligible ops,
 * incl. an already-inserted EnterNative) and ISLAND segments (maximal runs of
 * un-nativizable ops). This is the analysis surface for the "native containers
 * with bytecode islands" model flip: `container_ready` == "the whole body could
 * be ONE native container" (every op native-eligible - a stronger bar than
 * native_leaf, which also needs a single fully-native run ending in ReturnV).
 * M1 is DUMP-ONLY (surfaced in -vd); later milestones consume the segments for
 * whole-function emission. Segments are maximal runs (no basic-block splitting
 * yet - that arrives when emission needs it). Empty (JIT unsupported) off
 * platform. Pass the same JitCtx a caller's jit uses so a native CallV counts
 * as native; null -> a CallV counts as an island (conservative).
 */
struct ContainerSeg {
    size_t begin, end;   /* [begin, end) in the chunk's current pc space */
    bool native;         /* true = native-eligible run (or EnterNative) */
};
struct ContainerPlan {
    std::vector<ContainerSeg> segs;    /* ordered, cover [0,n); {} if JIT off */
    bool container_ready = false;      /* >=1 seg AND every seg native */
    int island_count = 0;
    int island_op_count = 0;
    int native_op_count = 0;
};
ContainerPlan jit_container_plan(const Chunk &chunk, const JitCtx *jc = nullptr);

/* #55 STEP 2.1 layout offsets (probed in vm.cpp, which has the full defs; baked
 * as immediates by the emitter). */
ptrdiff_t jit_off_desc_vm_chunk();      /* FuncDescriptor::vm_chunk */
ptrdiff_t jit_off_chunk_native_base();  /* Chunk::native.base */
ptrdiff_t jit_off_chunk_native_entry(); /* Chunk::native_entry_off */

/* Step 7a (the INLINE exception ops): the activation-side layout (probed in
 * vm.cpp) + the cold grow path of the inline PushHandler. */
struct VmActivation;
VmActivation **jit_addr_vm_act();       /* &g_vm_act (file-static) */
ptrdiff_t jit_off_act_handlers();       /* VmActivation::handlers */
ptrdiff_t jit_off_act_records();        /* VmActivation::records */
ptrdiff_t jit_off_act_rec_n();          /* VmActivation::rec_n */
ptrdiff_t jit_sizeof_vm_rec();          /* sizeof(VmCallRec) */
ptrdiff_t jit_off_rec_pend();           /* VmCallRec::pend */
extern "C" void jit_push_handler_grow(int_type catch_pc) noexcept;

/* De-helperize 6b: the ctx-indirect address chain (probed in vm.cpp). */
class EvalContext;
EvalContext **jit_addr_current_ctx();   /* &g_current_ctx (file-static) */
/* Re-raise deletability: the cold-side caret stamp - a conveying fragment's
 * failure branch writes the op's baked start/end Locs DIRECTLY into the
 * exception object in g_vm_jit_exc (null-checked: a bail conveys nothing,
 * so nothing is written - no stale side-state is possible), making the
 * caret pc-independent. The emitter needs the unique_ptr's storage address
 * and the Loc offsets inside the object (probed, so they cannot drift). */
void **jit_addr_exc();
ptrdiff_t jit_off_exc_loc_start();
ptrdiff_t jit_off_exc_loc_end();
ptrdiff_t jit_off_ctx_captures();       /* EvalContext::captures */
ptrdiff_t jit_off_ctx_gfuncs();         /* EvalContext::gfuncs */
ptrdiff_t jit_off_gft_slots();          /* GlobalFuncTable::slots */
ptrdiff_t jit_off_gft_defined();        /* GlobalFuncTable::defined */

/*
 * #55 STEP 2.1: the native CallV setup helper (vm.cpp, extern "C" noexcept,
 * baked as a call target). A caller fragment, at a native CallV, calls this to
 * push the callee frame: resolve the FuncObject from the callee global slot
 * (write-once => always a defined FuncObject, the gate proved it), then
 * vm_frame_setup (ret_chunk = caller_desc->vm_chunk, ret_pc = callv_pc+1).
 * Returns the callee window's slots ptr, which the caller loads into rdi before
 * `call`ing the callee fragment; on StackOverflow (or any bind throw) it stashes
 * the exception in g_vm_jit_exc and returns null, and the caller exits to
 * callv_pc so EnterNative re-raises (caret from the loc table). noexcept: any
 * RuntimeException is caught here, never a C++ throw out of native code.
 */
extern "C" LValue *jit_call_setup(int_type callee_slot, int_type argbase,
                                  size_t nargs, int_type dst,
                                  const FuncDescriptor *caller_desc,
                                  size_t callv_pc) noexcept;

/* M5: the SYNCHRONOUS native call - run a CallV's callee to completion
 * inside the helper (the LEAN SYNC ENTER: a non-boundary frame whose
 * ret_chunk is the sentinel stop chunk - see vm.cpp), so the CALLER
 * fragment continues natively across the call. 0 = done (dst written);
 * 1 = bail pre-side-effect (the interpreter re-runs the op); 2 = the callee
 * threw (g_vm_jit_exc, or g_vm_jit_eptr for a plain exception).
 * `site_packed` = the call-site loc (line << 32 | col) for the backtrace's
 * innermost frame. jit_call_sync_cached is CachedCallV (probes the caller's
 * per-frame pure cache first; a miss's key rides the callee record and is
 * stored by the normal return pop); jit_call_sync_value is CallValueV (the
 * callee VALUE sits in a frame temp, not a global slot). */
/* Lever 1 step 5 + M5b: the sync call is fully emitted at the site (the
 * push inline via emit_sync_push_native/JitPushLayout); jit_sync_postexit
 * handles a direct-entered callee's non-sentinel exit (shared with the
 * helper path's direct branch). */
extern "C" int jit_sync_postexit(size_t r, int_type site_packed) noexcept;
extern "C" int jit_cached_probe(const void *desc, int_type argbase,
                                int_type nargs, int_type dst) noexcept;
void *jit_addr_pending_key();
void *jit_addr_sync_depth();
int jit_sync_depth_cap();
void jit_set_sync_depth_cap(int cap);   /* M5a: raised when the native
                                         * stack arms; tests pin it low */
void jit_native_stack_init();

/* M5b - the FULLY-INLINE record push: every offset/size the emitted push
 * needs, probed from REAL objects in vm.cpp (the TU that owns
 * VmActivation/VmCallRec/VmStackSeg) - the SharedArrayObj::jit_probe
 * philosophy: measured from live members, so a layout change cannot
 * silently drift. Filled once by jit_fill_push_layout. */
struct JitPushLayout {
    /* VmActivation */
    ptrdiff_t act_segs, act_cur_seg, act_recs_high, act_diters_n,
              act_dyiters_n, act_used, act_cap, act_top_rec, act_vframe,
              act_handlers2;
    /* Frame (the view) */
    ptrdiff_t frame_slots, frame_size, frame_pure_cache;
    /* VmStackSeg */
    ptrdiff_t seg_slots, seg_top;
    /* VmCallRec */
    ptrdiff_t rec_window, rec_nslots, rec_seg, rec_seg_top_before,
              rec_run_chunk, rec_ret_chunk, rec_ret_pc, rec_dst, rec_desc,
              rec_caller_caps, rec_handler_base, rec_diter_base,
              rec_dyiter_base, rec_boundary, rec_sync_stop,
              rec_cache_key, rec_caller_cache;
    /* FuncDescriptor */
    ptrdiff_t desc_params, desc_frame_size, desc_fast_bind;
    size_t param_desc_size;
    /* Chunk */
    ptrdiff_t ck_n_temps, ck_n_dict_iters, ck_n_dyn_iters, ck_sync_entry;
    /* FuncObject */
    ptrdiff_t fo_func, fo_capture_slots;
    /* singletons/constants */
    const void *t_func;
    const void *stop_chunk;           /* &vm_sync_stop_chunk() */
};
void jit_fill_push_layout(JitPushLayout *out);

extern "C" int jit_call_sync(int_type callee_slot, int_type argbase,
                             int_type nargs, int_type dst,
                             int_type site_packed) noexcept;
extern "C" int jit_call_sync_cached(int_type callee_slot, int_type argbase,
                                    int_type nargs, int_type dst,
                                    int_type site_packed) noexcept;
extern "C" int jit_call_sync_value(int_type callee_temp, int_type argbase,
                                   int_type nargs, int_type dst,
                                   int_type site_packed) noexcept;

/* #55 STEP 2.1: native CallVs SET UP process-wide (a `jit:` coverage counter -
 * proves the native call path actually ran). */
extern unsigned long g_jit_native_calls;

/* model-flip M3: native-container island calls (jit_exec_block) run process-wide
 * - a coverage counter proving the container path executed. */
extern unsigned long g_jit_container_calls;

/*
 * #55 STEP 2: is this chunk's WHOLE body a single fully-native run ending in
 * ReturnV (a `native_leaf` a caller fragment can `call` directly)? Computed
 * from the OPS ALONE - no fragment emit - so codegen_chunk can set the flag
 * BEFORE jit_compile_chunk, which lets the precompile codegen ALL bodies (flags
 * set) then jit ALL bodies (a caller's native-call gate sees every callee's
 * flag). Off-platform / -nj -> false. Matches jit_compile_chunk's native_leaf
 * exactly (it sets native_entry_off when this holds).
 */
bool jit_chunk_is_native_leaf(const Chunk &chunk);

/*
 * Call a compiled fragment (frameless: slots base in, resume pc out).
 * Marked no_sanitize("function") - the JIT fragment has no clang/UBSan
 * CFI type header, so the -fsanitize=function check would read the
 * (unmapped) word before the fragment and fault. class LValue is opaque
 * here, so the arg is void* (the real ABI is size_t(LValue*)).
 */
size_t jit_enter(const void *frag, void *slots);
/* the SWITCHING entry (M5a): jit_call_sync_core's direct callee entry -
 * brackets its level's native-stack switch; plain when already active */
size_t jit_enter_deep(const void *frag, void *slots);

/* Approach A: a native fragment that hits a proven EXCEPTION condition
 * (a[i] out of bounds, a negative shift count) does NOT re-interpret the
 * op - it stores a raise KIND here and returns the op's pc; the EnterNative
 * handler then raises the matching exception via vm_raise (exact caret from
 * the loc table, no re-run). JR_NONE (0) on a normal fragment exit. */
enum JitRaiseKind { JR_NONE = 0, JR_OOB = 1, JR_NEG_SHIFT = 2, JR_DIV0 = 3 };
extern int g_vm_jit_raise;

/* Approach A (container-store helper ops, plans/native-aot.md): a native
 * a[i]=v / a[i] OP= v fragment marshals the base LValue*, the index and the
 * value and CALLS one of these instead of splitting the run at the store, so
 * the enclosing loop stays native. They run the interpreter's EXACT store
 * body (vm.cpp), noexcept: a raised exception is thrown LOC-LESS, caught,
 * stashed, and reported by a non-0 return (eax), which the fragment turns
 * into an exit; EnterNative re-raises it (stamping the caret from the live
 * chunk at the op's pc). `aop` is the base arith op (Op) as an int. No chunk
 * arg: the fragment can't hold a chunk pointer (stack-built, moved out). */
extern "C" int jit_store_elem_int(LValue *base, int_type idx, int_type rhs,
                                  int aop) noexcept;
extern "C" int jit_store_elem_float(LValue *base, int_type idx,
                                    double rhs, int aop) noexcept;
/* d[k] = v / d[k] OP= v: base dict LValue* + the key/value slot EvalValue*s
 * (boxed - the fragment leas the slot addresses) + the Expr14 op. */
class EvalValue;
extern "C" int jit_dict_store(LValue *base, const EvalValue *key,
                              const EvalValue *val, int op) noexcept;

/* The UNIVERSAL subscript store a[i] = v / a[i] OP= v (flat/general/dict). The
 * base may be a global/capture container, so `kind` (0 local/1 global/2 capture)
 * + base_slot form it (not an lea'd frame slot); idx/val are frame slots, aop
 * the Expr14 op. Returns 0 (ok), or 1 to exit (an undefined-global base bails to
 * the interpreter with NO g_vm_jit_exc; a vm_subscript_store RuntimeException
 * sets g_vm_jit_exc for EnterNative to re-raise). */
extern "C" int jit_store_elem_value(int_type kind, int_type base_slot,
                                    int_type idx_slot, int_type val_slot,
                                    int_type aop) noexcept;

/* A struct field store s.f = v / s.f OP= v. Like jit_store_elem_value but the
 * key is a MEMBER: `mk` = &chunk.member_keys[idx] (baked pool addr; memUid + the
 * carets). Same return convention (0 ok / 1 exit: undefined-global bail or a
 * vm_member_store RuntimeException in g_vm_jit_exc). */
extern "C" int jit_store_member(int_type kind, int_type base_slot,
                                int_type val_slot, int_type aop,
                                const void *mk) noexcept;

/* The nested-chain stores. StoreElem2V a[i][j]=v: LOCAL base + k1/k2/val slots +
 * `locs` = chunk.chain_locs[idx].data() (the per-step carets). StoreElemChainV
 * a[k0..kn]=v: `kind` base + kbase (key run) + `cl` = &chunk.chain_locs[idx].
 * StoreLValueChainV base.step..=v (mixed member/subscript): `kind` base +
 * `steps` = &chunk.chain_steps[idx] + `mkeys` = chunk.member_keys.data(). Same
 * return convention (0 ok / 1 exit: an undefined-global base bails, a store
 * RuntimeException in g_vm_jit_exc). */
extern "C" int jit_store_elem2(int_type base_slot, int_type k1_slot,
                               int_type k2_slot, int_type val_slot,
                               int_type aop, const void *locs) noexcept;
extern "C" int jit_store_elem_chain(int_type kind, int_type base_slot,
                                    int_type kbase, int_type val_slot,
                                    int_type aop, const void *cl) noexcept;
extern "C" int jit_store_lvalue_chain(int_type kind, int_type base_slot,
                                      int_type val_slot, int_type aop,
                                      const void *steps,
                                      const void *mkeys) noexcept;

/*
 * model-flip (nativize-ops): MoveV natively - `slots[dst] = slots[src].get()`,
 * the interpreter's EXACT MoveV (an alias-copy, ref-aware via LValue::put; never
 * throws). A boxed slot copy needs the type-erased copy path, so it CALLS this
 * helper (like the store ops) rather than inlining. `slots` is the frame base
 * (rdi). One island op fewer; the enclosing run no longer splits at a MoveV. */
extern "C" void jit_move(LValue *slots, int_type dst, int_type src) noexcept;

/*
 * model-flip (nativize-ops): SubscriptV natively - `dst = base[idx]` via the
 * runtime Type::subscript (any base: array/dict/string), the interpreter's
 * EXACT read. base_lv/dst are frame-slot LValue*s, idx the index slot's
 * EvalValue*. CAN throw (OOB / KeyNotFound / type) -> caught LOC-LESS into
 * g_vm_jit_exc, reported by a non-0 return; the fragment exits and EnterNative
 * re-raises (caret from the loc side table at the op's pc). Keeps a
 * subscript-in-a-loop native instead of splitting the run. */
extern "C" int jit_subscript(LValue *base_lv, const EvalValue *idx,
                             LValue *dst) noexcept;

/* Lever 4b - the fused ord(s[i]) body: TypeStr::subscript's wrap + bounds
 * (the OOB conveys loc-less -> the fragment's exc-stamp), then the raw byte
 * as int into dst. The base is compile-proven a non-opt string. */
extern "C" int jit_ord_char(LValue *base_lv, int_type idx,
                            LValue *dst) noexcept;

/* model-flip (nativize-ops): the native SliceV `dst = base[start:end]` via the
 * runtime Type::slice (COW-registered sub-view). base/start/end/dst are frame
 * slots (start/end == -1 -> none); frame via g_current_ctx. Only TypeErrorEx
 * (RuntimeException) is thrown -> caught loc-less into g_vm_jit_exc, non-0
 * return; EnterNative re-raises (caret from the loc side table). */
extern "C" int jit_slice(int_type base_slot, int_type start_slot,
                         int_type end_slot, int_type dst_slot) noexcept;

/* model-flip (nativize-ops): LoadBuiltinV natively - `slots[dst] =
 * builtin_slot[idx].get()` (the program-wide builtin table; a trivial value,
 * never throws). `slots` = the frame base (rdi). */
extern "C" void jit_load_builtin(LValue *slots, int_type dst,
                                 int_type idx) noexcept;

/* model-flip (nativize-ops): LoadCaptureV natively - `frame[dst] =
 * (*ctx->captures)[idx]`, a copy of a snapshot capture value (always defined ->
 * never throws). Uses g_current_ctx (captures + frame). */
extern "C" void jit_load_capture(int_type dst, int_type idx) noexcept;

/* model-flip (nativize-ops): LoadConstV natively - `slots[dst] = *src`, a copy
 * of a baked const-pool EvalValue (never throws; ref-aware for a non-trivial
 * const). `src` is `&chunk.consts[idx]` - the const-pool vector's heap BUFFER
 * address, which survives the chunk's std::move (unlike `&chunk` itself). */
extern "C" void jit_load_const(LValue *slots, int_type dst,
                               const EvalValue *src) noexcept;

/* model-flip (nativize-ops): MemberV natively - `dst = base.member` via the
 * shared member_read_core (struct field / const / dict key / optional). `base`
 * is the base slot's EvalValue*, `dst` the dst slot LValue*, `mk` a baked
 * `&chunk.member_keys[idx]` (the member-key pool BUFFER address, void* since
 * Chunk::MemberKey is a nested type). CAN throw (missing field/key) -> caught
 * loc-less (the caret is already on the exception, from the member key's
 * carets) into g_vm_jit_exc, reported by a non-0 return; EnterNative re-raises
 * (the existing loc is preserved). */
extern "C" int jit_member(const EvalValue *base, LValue *dst,
                          const void *mk) noexcept;

/* model-flip (nativize-ops): a PLAIN global store `g = <expr>` (aop invalid) -
 * `gfuncs->slots[gslot] = RValue(*src); defined[gslot]=1` (the interpreter's
 * exact decl/reassign; never throws). gfuncs via g_current_ctx. `src` is the
 * rhs value slot's EvalValue*. The COMPOUND case (g OP= / g++) stays interpreted
 * (it throws on an undefined slot + runs num_bin_op). */
extern "C" void jit_store_global(int_type gslot, const EvalValue *src) noexcept;

/* model-flip (nativize-ops): the native StoreCaptureV PLAIN `cap = <expr>` -
 * `(*ctx->captures)[cap_slot] = RValue(*src)`. A capture is always defined ->
 * never throws (op_fully_native). PLAIN only (like StoreGlobalV; a compound
 * `cap OP= v` runs num_bin_op, stays interpreted). */
extern "C" void jit_store_capture(int_type cap_slot,
                                  const EvalValue *src) noexcept;

/* model-flip (nativize-ops): the native LoadGlobalV read `frame[dst] =
 * gfuncs->slots[gslot]`. Returns 0 on success; 1 to BAIL (the global is
 * undefined - a use-before-def) so the emit exits to the op's pc and the
 * interpreter re-runs LoadGlobalV + throws UndefinedVariableEx (a plain
 * Exception, not conveyable via g_vm_jit_exc). NOT op_fully_native (the
 * original is kept for the bail). gfuncs/frame via g_current_ctx. */
extern "C" int jit_load_global(int_type dst_gslot,
                               const void *lep) noexcept;

/* model-flip (nativize-ops): LoadLiteralObjV natively - materialize a baked
 * const array/dict/struct literal via the shared eval_literal_obj (immutable
 * share vs a fresh mutable clone; never throws). `lo` is a baked
 * `&chunk.literal_objs[idx]` - the literal-objs pool BUFFER address (void* since
 * Chunk::LiteralObjEntry is nested), stable across the chunk's std::move. */
extern "C" void jit_load_literal_obj(LValue *slots, int_type dst,
                                     const void *lo) noexcept;

/* model-flip (nativize-ops): ArrLen natively - the foreach snapshot bound
 * `n = size(frame[base])`. `base` is a proven flat array (ForeachStmt::elem_th),
 * so size() never throws. */
extern "C" void jit_arr_len(LValue *slots, int_type dst,
                            int_type base) noexcept;

/* model-flip (nativize-ops): DictLoadInt/Float natively - the typed scalar dict
 * read. `key` is a baked const-pool value (member `d.k`) or a lea'd key-temp
 * slot (subscript `d[k]`). A missing key / non-dict base runs the shared
 * Type::subscript LOC-LESS and, on throw, catches into g_vm_jit_exc + returns 1
 * (EnterNative re-raises with the loc from the side table); returns 0 on the
 * hot present-key path. `is_int` selects the DictLoadInt vs Float result. */
extern "C" int jit_dict_load_int(int_type dst, int_type base_slot,
                                 const EvalValue *key) noexcept;
extern "C" int jit_dict_load_float(int_type dst, int_type base_slot,
                                   const EvalValue *key) noexcept;

/* model-flip (nativize-ops): MakeClosureV natively - create a closure +
 * snapshot captures from the running ctx. `def` is the closure's
 * program-lifetime FuncDescriptor* (baked as a value). Never throws. */
extern "C" void jit_make_closure(int_type dst, const void *def) noexcept;

/* model-flip (nativize-ops): MakeArrayV natively - build an array LITERAL from
 * the element run [base, base+n) via the shared build_array_from_values, honoring
 * `hint` (an ArrHint: flat int/float/bool/struct or general). Never throws
 * (op_fully_native). */
extern "C" void jit_make_array(int_type dst, int_type base, int_type n,
                               int_type hint) noexcept;

/* model-flip (nativize-ops): MakeDictV natively - build a dict LITERAL from the
 * interleaved key/value run [base, base + 2*npairs) via the shared
 * build_dict_from_pairs (freezing each key). An UNHASHABLE key (a dyn-laundered
 * function) throws TypeErrorEx -> g_vm_jit_exc + return 1 (EnterNative re-raises
 * with the literal's caret from the loc side table); 0 otherwise. */
extern "C" int jit_make_dict(int_type dst, int_type base,
                             int_type npairs) noexcept;

/* model-flip (nativize-ops): the STRUCT BUILDS. `def` is the program-lifetime
 * StructTypeDef* from the struct_defs pool (baked as a VALUE); `bc` is a baked
 * `&chunk.boxed_ctors[idx]` (def + per-arg carets). All three re-raise a
 * TypeErrorEx via g_vm_jit_exc + return 1:
 *  - jit_struct_ctor: a POD `P(x, y)` from its field run (H1 dst-slot reuse);
 *  - jit_struct_ctor_boxed: a non-POD `B(a, x)` - the throw carries the
 *    offending arg's POOLED caret, which vm_raise's empty-loc-only stamp keeps;
 *  - jit_make_struct_array: the fused flat array<PodStruct> literal (`n` is the
 *    ELEMENT count; the run holds n * nfields values). */
extern "C" int jit_struct_ctor(const void *def, int_type base, int_type nf,
                               int_type dst) noexcept;
extern "C" int jit_struct_ctor_boxed(int_type dst, int_type base,
                                     const void *bc) noexcept;
extern "C" int jit_make_struct_array(const void *def, int_type base,
                                     int_type n, int_type dst) noexcept;
/* the planned POD ctor's slow branch (never throws -> void) */
extern "C" void jit_struct_ctor_planned(const void *def, const void *plan,
                                        int_type dst) noexcept;

/* model-flip (nativize-ops): the FOREACH element/field LOADS. The `idx` arrives
 * as a VALUE (the emitter materializes the op's slot-or-literal operand with the
 * cache-aware load_operand before the call), so an N5-pinned foreach counter is
 * read from the register, not a stale slot. All non-throwing (index loop-bounded,
 * base kind proven) -> void + op_fully_native, except jit_load_elem_value, which
 * bounds-checks: an OOB sets g_vm_jit_exc LOC-LESS (EnterNative stamps from the
 * loc side table) and a non-general/non-str base BAILS to the interpreter. */
/* model-flip (nativize-ops): the JumpUnlessTrueV CONDITION - the boxed
 * truthiness test. Returns 1 = true, 0 = false, -1 = THREW (is_true's base Type
 * op throws for a value with no bool conversion; the exception rides
 * g_vm_jit_exc LOC-LESS and EnterNative stamps the condition's caret from the
 * loc side table). The JUMP itself is emitted by the fragment, so a loop with a
 * boxed condition no longer splits the run at the branch. */
extern "C" int jit_is_true(int_type cond_slot) noexcept;

extern "C" void jit_load_elem_bool(int_type dst, int_type base,
                                   int_type idx) noexcept;
extern "C" void jit_str_len(int_type dst, int_type base) noexcept;
extern "C" void jit_load_str_char(int_type dst, int_type base,
                                  int_type idx) noexcept;
extern "C" void jit_load_struct_field(int_type dst, int_type base, int_type idx,
                                      int_type fidx, int is_float) noexcept;
extern "C" void jit_load_struct_elem(int_type dst, int_type base,
                                     int_type idx) noexcept;
extern "C" int jit_load_elem_value(int_type dst, int_type base,
                                   int_type idx) noexcept;

/* model-flip (nativize-ops): the ITERATOR ops. The per-loop state lives on the
 * activation (a watermarked slice indexed by the current record's base +
 * iter_id), reached via g_vm_act; each helper runs the SHARED body its
 * interpreter handler runs. The Next pair BRANCHES: the helper returns the
 * verdict and the FRAGMENT jumps (like JumpUnlessTrueV), so a dict/dyn foreach
 * loop's back edge stays inside the fragment.
 *  - jit_dict_iter_init: pin the proven dict + iterator=begin(); never throws.
 *  - jit_dict_iter_next: 1 = bound (fall through), 0 = end (jump to end_pc);
 *    never throws.
 *  - jit_foreach_dyn_init: dispatch the dyn container once (`targets` = the
 *    baked &chunk.unpack_targets[idx]); a non-container -> TypeErrorEx via
 *    g_vm_jit_exc + return 1 (loc side table), else 0.
 *  - jit_foreach_dyn_next: 1 = bound, 0 = end, -1 = THREW (the strict N-var
 *    unpack's TypeErrorEx via g_vm_jit_exc, loc side table). */
/* model-flip (nativize-ops): the CHECKED INC-DEC ops. Each forms its base /
 * root like the interpreter EXCEPT an undefined GLOBAL, which BAILS (return 1
 * with no exception - UndefinedVariableEx is not conveyable); every other
 * throw is a RuntimeException -> g_vm_jit_exc + return 1 (the Elem/Member/
 * Chain throws carry their POOLED carets). `site` / `chain` are baked
 * &chunk.incdec_sites[idx] / &chunk.incdec_chains[idx]; `mkeys` the
 * member_keys BUFFER. */
extern "C" int jit_incdec_checked(int_type slot, int_type kind,
                                  int_type is_inc) noexcept;
extern "C" int jit_incdec_elem(int_type kind, int_type base_slot,
                               int_type key_slot, int_type is_inc,
                               const void *site) noexcept;
extern "C" int jit_incdec_member(int_type kind, int_type base_slot,
                                 int_type is_inc, const void *site) noexcept;
extern "C" int jit_incdec_chain(int_type root_kind, int_type root_slot,
                                int_type dst, int_type is_inc,
                                const void *chain, const void *mkeys) noexcept;

/* model-flip (nativize-ops): the STRICT-unpack ops. jit_unpack_elem serves
 * all four UnpackElem* (n_kind = N | kind << 8, kind 0 int / 1 float / 2
 * value; `targets` = the baked &chunk.unpack_targets[idx] for the Targets
 * variant, else null and the consecutive run at dst_base). jit_multi_unpack
 * is MultiUnpackV (targets/coerce = baked pool entries; aop = the compound
 * base op or Op::invalid). Throws -> g_vm_jit_exc + return 1. */
extern "C" int jit_unpack_elem(int_type dst_base, int_type base_slot,
                               int_type idx, int_type n_kind,
                               const void *targets) noexcept;
extern "C" int jit_multi_unpack(int_type rval_slot, const void *targets,
                                const void *coerce, int_type aop) noexcept;

/* model-flip (nativize-ops): DeclConstV (bind a const decl's slot, local or
 * global; never throws) and DefinedGlobalV (`defined(g)` = the slot's
 * defined-flag as a bool; never throws). */
extern "C" void jit_decl_const(int_type dst, int_type is_global,
                               int_type src) noexcept;
extern "C" void jit_defined_global(int_type dst, int_type gslot) noexcept;

/* model-flip (nativize-ops): the StructFieldAddInt READ half (the #9 fusion
 * `dst = other + a[i].f`) - vm_struct_field_int, proven no-fault; the add +
 * dst write run in the fragment. And the EmplaceStruct body -
 * append(struct_arr, Ctor(args)): arg0 by kind (0 local / 1 global, nullptr
 * when undefined / 2 capture), the shared vm_do_emplace, dst written; a
 * throw -> g_vm_jit_exc + return 1. `site` = a baked
 * &chunk.emplace_sites[idx]. */
extern "C" int_type jit_struct_field_add_int(int_type base_slot, int_type idx,
                                             int_type fidx) noexcept;
extern "C" int jit_emplace_struct(int_type dst, int_type base_slot,
                                  int_type kind, const void *site,
                                  int_type run_base) noexcept;

/* model-flip (nativize-ops): LoadMemberInt/Float natively - the H1 typed
 * standalone struct-member read `p.x` (POD byte fast path; boxed/dict/const
 * fallback via member_read_core). `mk` = a baked &chunk.member_keys[idx]; a
 * fallback throw carries the member caret -> g_vm_jit_exc + return 1. */
extern "C" int jit_load_member(int_type dst, int_type base_slot,
                               const void *mk, int is_int) noexcept;

extern "C" void jit_dict_iter_init(int_type iter_id,
                                   int_type dict_slot) noexcept;
extern "C" int jit_dict_iter_next(int_type iter_id, int_type k_slot,
                                  int_type v_slot) noexcept;
extern "C" int jit_foreach_dyn_init(int_type iter_id, int_type cont_slot,
                                    int_type shape,
                                    const void *targets) noexcept;
extern "C" int jit_foreach_dyn_next(int_type iter_id) noexcept;

/* model-flip (nativize-ops): the BOXED-ARITH ops BinOpV / CmpV / CompoundV -
 * the interpreter's exact boxed_operand + vm_num_binop bodies. `bop` is a baked
 * `&chunk.boxed_ops[idx]` (the op's operand data - target/a/b/aop - copied into
 * a stable serializable pool, since baking &code[pc] is unsafe). A num_bin_op
 * throw (div0 / type) catches into g_vm_jit_exc + returns 1 (EnterNative
 * re-raises with the loc from the side table); returns 0 otherwise. */
extern "C" int jit_boxed_binop(const void *bop) noexcept;
extern "C" int jit_boxed_cmp(const void *bop) noexcept;
extern "C" int jit_boxed_compound(const void *bop) noexcept;
/* Compound global/capture stores `g OP=`/`cap OP=` - reuse the boxed_ops pool
 * (bo->target = the GLOBAL/CAPTURE slot, bo->a = the rhs, bo->aop). Return 1 to
 * exit: an undefined-global BAIL (global only, no g_vm_jit_exc) or a num_bin_op
 * RuntimeException (g_vm_jit_exc, re-raised). */
extern "C" int jit_store_global_compound(const void *bop) noexcept;
extern "C" int jit_store_capture_compound(const void *bop) noexcept;
/* LogV (eager && / ||): is_true() never throws -> void (op_fully_native). */
extern "C" int jit_boxed_log(const void *bop) noexcept;

/* UnaryV (boxed unary -/~/!/+ over a dyn): reuses the boxed_ops pool. `-str`/
 * `~str` throw -> g_vm_jit_exc + returns 1 (re-raise); 0 otherwise. */
extern "C" int jit_unary(const void *bop) noexcept;

/* model-flip (nativize-ops): CoerceNumV - the typed-store numeric coerce of a
 * dyn value (widen / pass-none / TypeError-throw). Fits in registers (dst +
 * src_slot + is_float flag), no pool. A throw catches into g_vm_jit_exc +
 * returns 1 (EnterNative re-raises with the Expr14 caret); 0 otherwise. */
extern "C" int jit_coerce_num(int_type dst, int_type src_slot,
                              int is_float) noexcept;

/* model-flip (nativize-ops): CallBuiltinV - a value-ABI read-only builtin call.
 * `bc` is a baked `&chunk.builtin_calls[idx]` (func_v ptr + arg carets). Args
 * are copied from frame slots [base, base+n); a throw catches into g_vm_jit_exc
 * (loc from the pool) + returns 1. A callback builtin re-enters vm_dispatch. */
extern "C" int jit_call_builtin(int_type dst, int_type base, int_type n,
                                const void *bc) noexcept;

/* model-flip (nativize-ops): CheckFuncV (map/filter's arg0 guard - a non-func
 * conveys a loc-less TypeErrorEx, stamped at the op pc by the re-raise) and
 * MapFilterV (the shared vm_map_filter body; callback re-enters vm_dispatch). */
extern "C" int jit_check_func(int_type slot) noexcept;
extern "C" int jit_map_filter(int_type fn_slot, int_type cont_slot,
                              int_type dst, int_type is_map) noexcept;

/* model-flip (nativize-ops): the dyn-callee generic call pair. CheckCallableV
 * (the callable guard - conveys a loc-less NotCallableEx, exc-stamped with
 * the callee caret) and CallValueGenericV (the full by-Kind dispatch over
 * the baked CallSite pool; a FuncObject callee runs via the lean sync core;
 * dst_callee packs dst lo32 | callee-temp hi32). */
extern "C" int jit_check_callable(int_type slot) noexcept;

/* Re-raise deletability: builds the JR_DIV0 / JR_NEG_SHIFT exception
 * loc-less into g_vm_jit_exc (the emit's exc-stamp adds the caret), so the
 * int div/mod/shift arms convey instead of signalling g_vm_jit_raise. */
extern "C" void jit_raise_kind_exc(int kind) noexcept;

/* Re-raise deletability: ThrowRuntimeV builds its pooled exception natively
 * (`t` = a baked &chunk.throws[idx]) - Runtime kinds via g_vm_jit_exc,
 * plain kinds via g_vm_jit_eptr, each with its pooled caret. */
extern "C" int jit_throw_runtime(const void *t) noexcept;
extern "C" int jit_call_value_generic(int_type dst_callee, int_type argbase,
                                      int_type nargs, const void *cs,
                                      const void *mkeys,
                                      int_type site_packed) noexcept;

/* model-flip (nativize-ops): AppendV - `append(a, x)`/`push(a, x)`. Forms arg0's
 * LValue* from kind (0 loc/1 gbl/2 cap) + arg0_slot, runs the never-throwing
 * arr_append_fast; a decline falls back to vm_call_builtin_lv_rest (builtin_
 * append, all RuntimeException throws now) -> g_vm_jit_exc + return 1. dst_slot
 * = the result dst (-1 = discarded). `bc` = &chunk.builtin_calls[idx]. */
extern "C" int jit_append(int_type kind, int_type arg0_slot, int_type val_slot,
                          int_type dst_slot, const void *bc) noexcept;

/* model-flip (nativize-ops): CallBuiltinLV - a mutating lvalue-ABI builtin
 * (pop/insert/erase/sort/reverse/intptr). Forms arg0 from kind + arg0_slot;
 * rest_base >= 0 = the value-args run base (vm_call_builtin_lv_rest), -1 = no
 * value args (func_lv, empty rest). All throws are RuntimeExceptions ->
 * g_vm_jit_exc + return 1. `bc` = &chunk.builtin_calls[idx]. */
extern "C" int jit_call_builtin_lv(int_type kind, int_type arg0_slot,
                                   int_type dst_slot, int_type rest_base,
                                   const void *bc) noexcept;

/* model-flip (nativize-ops): CallBuiltinLVElem / CallBuiltinLVMember - a mutating
 * lvalue builtin whose arg0 is a SUBSCRIPT (`append(a[i], x)`) or struct-MEMBER
 * (`append(s.f, x)`) target. Form the base by kind + base_slot, derive the
 * element/field LValue*, then func_lv. `run_base` = the value-args run (for
 * elem, run[0] is the index + run[1..] the values; for member, run[0..] the
 * values). All throws are RuntimeExceptions -> g_vm_jit_exc + return 1.
 * `bc` = &chunk.builtin_calls[idx]. */
extern "C" int jit_call_builtin_lv_elem(int_type kind, int_type base_slot,
                                        int_type dst_slot, int_type run_base,
                                        const void *bc) noexcept;
extern "C" int jit_call_builtin_lv_member(int_type kind, int_type base_slot,
                                          int_type dst_slot, int_type run_base,
                                          const void *bc) noexcept;

/* model-flip (nativize-ops): PER-OP runtime coverage - g_jit_op_run[op] is
 * bumped by that op's nativized helper (jit_move/jit_subscript/...), PROVING
 * the native code for each op actually RAN (not merely that the op is
 * jit_op_eligible / classified native in the hypothetical container view - the
 * "prove the code ran" rule). Read per-op by a `jit:` test. Bumps are gated to
 * TESTS via ML_JIT_OP_RAN so a release build pays nothing. */
extern unsigned long g_jit_op_run[];
#ifdef TESTS
/* Execution-proof counters for the struct BAKED fast paths - bumped by the
 * EMITTED inline code itself (the helpers bump g_jit_op_run), so a test can
 * prove the inline path ran, not just the slow helper. */
extern "C" unsigned long g_jit_member_fast, g_jit_ctor_fast;
extern "C" unsigned long g_jit_sync_inline;
extern "C" unsigned long g_jit_entry_resume;
#endif
#ifdef TESTS
#  define ML_JIT_OP_RAN(op) (g_jit_op_run[static_cast<size_t>(OpCode::op)]++)
#else
#  define ML_JIT_OP_RAN(op) ((void)0)
#endif

/*
 * #55 native calls (plans/native-call-impl.md): a fully-native LEAF body's
 * ReturnV runs IN the fragment. The fragment flushes its register cache and
 * calls this with the result value's frame slot; jit_ret reads that slot from
 * the CURRENT callee window, then either pops the frame (an in-VM call -
 * vm_frame_leave writes the parent's dst + sets the resume globals) or, at a
 * BOUNDARY frame, sets flow (the do_func_call / callback contract). It returns
 * a resume SENTINEL the EnterNative handler applies (switch to the parent, or
 * stop the invocation). Defined in vm.cpp (it needs the in-VM call stack) and
 * baked as a call target by the emitter. noexcept: a fully-native leaf body is
 * throw-free, so the pop/leave here cannot throw. */
extern "C" size_t jit_ret(int_type res_slot) noexcept;

/* model-flip (nativize-ops): the native Halt - a fall-through body's implicit
 * `return none`. Like jit_ret but the result is hard-wired to none (no slot).
 * IN-VM: vm_frame_leave (parent dst = none) -> JIT_RET_SENTINEL. BOUNDARY: bare
 * JIT_RET_BOUNDARY (flow untouched - a fall-through body's flow is none). */
extern "C" size_t jit_halt() noexcept;

/*
 * model-flip M3 (plans/model-flip.md): the native CONTAINER's ISLAND call. A
 * container fragment, at an interpreted island, `call`s this with its OWN
 * FuncDescriptor (so vm_exec_block can reach the container's chunk via
 * desc->vm_chunk) and the island's start pc. Runs the island via vm_exec_block
 * in the container frame; returns the fall-through resume pc, or a high-bit-set
 * RAISED sentinel (bridging the pending exception into g_vm_jit_exc) so the
 * caller fragment can `test rax; jns` and exit to re-raise. noexcept. */
extern "C" size_t jit_exec_block(const FuncDescriptor *desc,
                                 size_t from_pc) noexcept;

/* #55: native ReturnVs executed process-wide (a `jit:` coverage counter that
 * PROVES the native return path actually ran, not the interpreter). */
extern unsigned long g_jit_native_returns;
