/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/14_array_subscript.my: preallocated array, a
 * random-access write pass then a read/reduce pass (subscript hot path). */
#include "bench.h"

/* BENCH-FAIR (plans/bench-fairness.md, class C): g++ -O3 auto-VECTORIZED
 * the hot loop (asm-verified packed ops) - 2-16 lanes per instruction vs
 * MyLang's scalar native loop. The comparison is meant to be scalar
 * shape-vs-shape; revisit if MyLang ever vectorizes. */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    std::vector<long> a(N, 0);          /* array(N, 0) */
    for (long i = 0; i < N; i++)
        a[i] = i * 2;

    /* Materialize the writes before the read pass so the two loops can't be
     * fused + closed-formed; the read reduction then reads real memory (and is
     * free to vectorize - the legitimate C++ win we measure). */
    bench_sink_ptr(a.data());

    long s = 0;
    for (long i = 0; i < N; i++)
        s += a[i];

    printf("result: %ld\n", s);
    return 0;
}
