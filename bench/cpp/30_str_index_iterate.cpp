/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/30_str_index_iterate.my: index into a string
 * char-by-char (s[i] yields the byte; ord() is its code point). */
#include "bench.h"

/* BENCH-FAIR (plans/bench-fairness.md, class C): g++ -O3 auto-VECTORIZED
 * the hot loop (asm-verified packed ops) - 2-16 lanes per instruction vs
 * MyLang's scalar native loop. The comparison is meant to be scalar
 * shape-vs-shape; revisit if MyLang ever vectorizes. */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")
#include <string>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::string s2;
    for (int i = 0; i < 500; i++)
        s2 += "0123456789";             /* 5000 chars */

    long R = 200L * scale;
    long total = 0;
    for (long k = 0; k < R; k++) {
        for (size_t i = 0; i < s2.size(); i++)
            total += (long)(unsigned char)s2[i];
        total = total % 1000000007L;
    }

    printf("result: %ld\n", total);
    return 0;
}
