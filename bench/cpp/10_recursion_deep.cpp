/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/10_recursion_deep.my: deep linear recursion (depth
 * 900) called in an outer loop. sumto(900) is loop-invariant, and worse, the
 * compiler CLOSE-FORMS sumto(constant) to a single value (verified: the plain
 * form runs in O(1) regardless of scale). Passing the argument through opaque()
 * hides its value, so the compiler must actually run the depth-900 recursion
 * each iteration (the fair ceiling), and bench_sink(r) keeps each call's result
 * live so none are dead-code-eliminated. */
#include "bench.h"

/* BENCH-FAIR (plans/archived/bench-fairness.md, class B): MyLang executes a REAL
 * depth-900 call chain here; g++ -O3 converted this accumulator recursion
 * into a plain loop (zero calls - asm-verified), which benches loop adds
 * against a call protocol. noinline keeps the entry a call, and
 * no-optimize-sibling-calls gates GCC's tail-recursion(+accumulator)
 * elimination so each level performs a real call. */
__attribute__((noinline, optimize("no-optimize-sibling-calls")))
static long sumto(long n)
{
    if (n == 0)
        return 0;
    return n + sumto(n - 1);
}

/* Force x opaque to the optimizer: no output, so the value is unchanged, but the
 * compiler can no longer prove it is the constant 900 - defeating close-form
 * and loop-invariant hoisting of the sumto() call. */
static inline long opaque(long x)
{
    asm volatile("" : "+r"(x));
    return x;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long r = 0;
    for (long k = 0; k < 3000L * scale; k++) {
        r = sumto(opaque(900));
        bench_sink(r);
    }
    printf("result: %ld\n", r);
    return 0;
}
