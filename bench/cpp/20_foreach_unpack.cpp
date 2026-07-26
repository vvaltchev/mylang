/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/20_foreach_unpack.my: build an array of [x, y] pairs
 * (array<array<int>>), then foreach with tuple unpacking, accumulating (x + y)
 * mod 1e9+7. Reproduced as a vector of 2-element rows iterated by range-for. */
#include "bench.h"

/* BENCH-FAIR (plans/bench-fairness.md, class C): g++ -O3 auto-VECTORIZED
 * the hot loop (asm-verified packed ops) - 2-16 lanes per instruction vs
 * MyLang's scalar native loop. The comparison is meant to be scalar
 * shape-vs-shape; revisit if MyLang ever vectorizes. */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long n = 500000L * scale;

    /* MyLang's array<array<int>> is an outer array of n SEPARATELY
     * heap-allocated 2-int sub-arrays - match it with a nested heap vector
     * (like 46_matrix_mult), not one contiguous buffer. */
    std::vector<std::vector<long>> pairs(n);
    for (long i = 0; i < n; i++)
        pairs[i] = {i, i * 2};

    long s = 0;
    for (const std::vector<long> &p : pairs) {
        long x = p[0], y = p[1];
        s += x + y;
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
