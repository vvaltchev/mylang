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

struct Chunk;

/* The kill switch: -nj / MYLANG_JIT=0; always false off-platform. */
extern bool g_jit_enabled;

/* Fragments compiled process-wide (tests / -vd audit). */
extern unsigned long g_jit_frags;

void jit_compile_chunk(Chunk &chunk);
