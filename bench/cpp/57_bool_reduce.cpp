/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/57_bool_reduce.my: build a flat array<bool>
 * (flags[i] = i%2==0), then count the `true`s 50 times. array<bool> -> a byte
 * per element (std::vector<char>). The per-pass bench_sink(c) carries a
 * "memory" clobber so the compiler cannot collapse the 50 identical scans into
 * one (that would defeat the "50 real passes" the test measures). */
#include "bench.h"

/* BENCH-FAIR (plans/bench-fairness.md, class C): g++ -O3 auto-VECTORIZED
 * the hot loop (asm-verified packed ops) - 2-16 lanes per instruction vs
 * MyLang's scalar native loop. The comparison is meant to be scalar
 * shape-vs-shape; revisit if MyLang ever vectorizes. */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 200000L * scale;

    std::vector<char> flags(N);
    for (long i = 0; i < N; i++)
        flags[i] = (i % 2 == 0) ? 1 : 0;

    long total = 0;
    for (int r = 0; r < 50; r++) {
        long c = 0;
        for (long i = 0; i < N; i++)
            if (flags[i])
                c++;
        bench_sink(c);   /* stop the 50 identical passes collapsing to one */
        total += c;
    }

    printf("result: %ld\n", total);
    return 0;
}
