/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * Shared harness for the C++ reference benchmarks (bench/cpp/NN_name.cpp).
 *
 * PURPOSE. These are FAITHFUL C++ translations of bench/my/NN_name.my (and
 * the bench/py twin) - the SAME algorithm, the SAME data, the SAME printed
 * result. They answer one question: how much of MyLang's residual cost is
 * the interpreter/JIT dispatch + the boxed EvalValue/refcount model, versus
 * the underlying C++ std::library work (unordered_map, std::string, std::sort)
 * that MyLang and a hand-written C++ program share. The C++ time is the
 * REALISTIC CEILING for a perfect JIT: MyLang can never beat native C++ doing
 * the same work, so `my/cpp` is the multiple still on the table, and `cpp/py`
 * says how much a C++ rewrite already buys over CPython.
 *
 * ANTI-CONST-FOLD CONTRACT. We WANT the normal -O3 wins a real program gets
 * (inlining, const-folding of loop-invariants, autovectorization) - that is
 * exactly the C++ advantage we are measuring. We must ONLY stop the
 * degenerate optimizations that would defeat the test: (1) precomputing the
 * whole loop at compile time, and (2) dead-code-eliminating the computation
 * because its result is unused. Two rules make both impossible:
 *   - the workload size comes from argv (a RUNTIME value), so the loop trip
 *     count is unknown at compile time - the compiler cannot precompute it;
 *   - every result that flows to `print` is printed (a real use), and any
 *     intermediate the compiler might still drop is passed to bench_sink(),
 *     an empty `asm volatile` that both consumes the value and clobbers
 *     memory, so the producing loop cannot be elided or hoisted out.
 * Build with -O3 -fwrapv (MyLang's int64 wraps under -fwrapv; matching it
 * keeps the arithmetic byte-identical) and -std=c++17.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

/* Read the scale multiplier the way every .my/.py bench does: argv[1], or 1. */
static inline long bench_scale(int argc, char **argv)
{
    return argc > 1 ? std::atol(argv[1]) : 1;
}

/*
 * The optimization barrier. An empty `asm volatile` with `x` as an input and
 * a "memory" clobber: the compiler must have `x` in a register/memory at this
 * point (so the code computing it survives) and must assume memory changed
 * (so it can't reorder stores across it). It emits ZERO instructions - it
 * only constrains the optimizer. Use it on any value whose producing loop
 * would otherwise be dead. (Values that reach printf are already "used".)
 */
template <typename T>
static inline void bench_sink(const T &x)
{
    asm volatile("" : : "r,m"(x) : "memory");
}

/* Overload for a value we only need to keep "live" through memory. */
static inline void bench_sink_ptr(const void *p)
{
    asm volatile("" : : "r,m"(p) : "memory");
}
