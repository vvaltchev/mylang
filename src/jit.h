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

void jit_compile_chunk(Chunk &chunk);

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

/* #55: native ReturnVs executed process-wide (a `jit:` coverage counter that
 * PROVES the native return path actually ran, not the interpreter). */
extern unsigned long g_jit_native_returns;
