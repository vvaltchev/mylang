/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/92_recursion_with_helper.my: a depth-~500
 * recursion whose every level also calls a helper. BENCH-FAIR (class B, as
 * 10_recursion_deep): MyLang performs a real call per level, so walk is
 * noinline and its sibling-call optimisation is off - each level is a real
 * call. weight stays inlinable (08_func_call's rule: inlining a leaf helper
 * IS the C++ advantage). The depth varies with k, which the compiler cannot
 * hoist, and bench_sink keeps every result live. */
#include "bench.h"

static long weight(long n)
{
    long x = n * 7 + 3;
    x = x ^ (x >> 3);
    if (x < 0)
        x = 0 - x;
    return x % 1009;
}

__attribute__((noinline, optimize("no-optimize-sibling-calls")))
static long walk(long n)
{
    if (n <= 0)
        return 0;
    return (weight(n) + walk(n - 1)) % 1000000007;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long K = 2000L * scale;
    long s = 0;
    for (long k = 0; k < K; k++) {
        s = (s + walk(500 + k % 7)) % 1000000007;
        bench_sink(s);
    }
    printf("result: %ld\n", s);
    return 0;
}
