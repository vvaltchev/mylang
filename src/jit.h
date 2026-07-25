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

/* Approach A: a native fragment that hits a proven EXCEPTION condition
 * (a[i] out of bounds, a negative shift count) does NOT re-interpret the
 * op - it stores a raise KIND here and returns the op's pc; the EnterNative
 * handler then raises the matching exception via vm_raise (exact caret from
 * the loc table, no re-run). JR_NONE (0) on a normal fragment exit. */
enum JitRaiseKind { JR_NONE = 0, JR_OOB = 1, JR_NEG_SHIFT = 2 };
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

/* model-flip (nativize-ops): LoadBuiltinV natively - `slots[dst] =
 * builtin_slot[idx].get()` (the program-wide builtin table; a trivial value,
 * never throws). `slots` = the frame base (rdi). */
extern "C" void jit_load_builtin(LValue *slots, int_type dst,
                                 int_type idx) noexcept;

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
extern "C" int jit_dict_load(int_type dst, int_type base_slot,
                             const EvalValue *key, int is_int) noexcept;

/* model-flip (nativize-ops): MakeClosureV natively - create a closure +
 * snapshot captures from the running ctx. `def` is the closure's
 * program-lifetime FuncDescriptor* (baked as a value). Never throws. */
extern "C" void jit_make_closure(int_type dst, const void *def) noexcept;

/* model-flip (nativize-ops): the BOXED-ARITH ops BinOpV / CmpV / CompoundV -
 * the interpreter's exact boxed_operand + vm_num_binop bodies. `bop` is a baked
 * `&chunk.boxed_ops[idx]` (the op's operand data - target/a/b/aop - copied into
 * a stable serializable pool, since baking &code[pc] is unsafe). A num_bin_op
 * throw (div0 / type) catches into g_vm_jit_exc + returns 1 (EnterNative
 * re-raises with the loc from the side table); returns 0 otherwise. */
extern "C" int jit_boxed_binop(const void *bop) noexcept;
extern "C" int jit_boxed_cmp(const void *bop) noexcept;
extern "C" int jit_boxed_compound(const void *bop) noexcept;

/* model-flip (nativize-ops): PER-OP runtime coverage - g_jit_op_run[op] is
 * bumped by that op's nativized helper (jit_move/jit_subscript/...), PROVING
 * the native code for each op actually RAN (not merely that the op is
 * jit_op_eligible / classified native in the hypothetical container view - the
 * "prove the code ran" rule). Read per-op by a `jit:` test. Bumps are gated to
 * TESTS via ML_JIT_OP_RAN so a release build pays nothing. */
extern unsigned long g_jit_op_run[];
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
