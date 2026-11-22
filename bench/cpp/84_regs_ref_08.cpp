/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/84_regs_ref_08.my - 8 reference (array)
 * parameters, each read once per iteration. The C++ side holds each
 * base pointer in a register until it runs out and spills; MyLang
 * re-derives every one from its 48-byte frame slot. */
#include "bench.h"

__attribute__((noinline)) static long work(const long *b0, const long *b1, const long *b2, const long *b3, const long *b4, const long *b5, const long *b6, const long *b7, long n)
{
    long s = 0;
    for (long i = 0; i < n; i++) {
        long j = i & 3;
        s = s + b0[j];
        s = s + b1[j];
        s = s + b2[j];
        s = s + b3[j];
        s = s + b4[j];
        s = s + b5[j];
        s = s + b6[j];
        s = s + b7[j];
        if ((i & 1) == 0)
            s = s + 1;
    }
    return s;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    static const long v0[4] = { 1, 2, 3, 4 };
    static const long v1[4] = { 2, 3, 4, 5 };
    static const long v2[4] = { 3, 4, 5, 6 };
    static const long v3[4] = { 4, 5, 6, 7 };
    static const long v4[4] = { 5, 6, 7, 8 };
    static const long v5[4] = { 6, 7, 8, 9 };
    static const long v6[4] = { 7, 8, 9, 10 };
    static const long v7[4] = { 8, 9, 10, 11 };
    long N = 2000000L * scale;
    printf("result: %ld\n", work(v0, v1, v2, v3, v4, v5, v6, v7, N));
    return 0;
}
