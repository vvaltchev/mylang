/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/19_foreach_indexed.my: `indexed` foreach (enumerate)
 * over a 1M-element array, repeated R times; accumulates i*e, mod per pass. */
#include "bench.h"

/* BENCH-FAIR (plans/bench-fairness.md, class C): g++ -O3 auto-VECTORIZED
 * the hot loop (asm-verified packed ops) - 2-16 lanes per instruction vs
 * MyLang's scalar native loop. The comparison is meant to be scalar
 * shape-vs-shape; revisit if MyLang ever vectorizes. */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::vector<long> a(1000000);       /* range(1000000) */
    for (long i = 0; i < 1000000; i++)
        a[i] = i;

    long R = 3 * scale;
    long s = 0;

    for (long k = 0; k < R; k++) {
        /* Block hoisting the invariant reduction out of the outer loop. */
        bench_sink_ptr(a.data());
        long idx = 0;
        for (long e : a) {
            s += idx * e;
            idx++;
        }
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
