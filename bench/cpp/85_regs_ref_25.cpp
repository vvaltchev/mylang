/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/85_regs_ref_25.my - 25 reference (array)
 * parameters, each read once per iteration. The C++ side holds each
 * base pointer in a register until it runs out and spills; MyLang
 * re-derives every one from its 48-byte frame slot. */
#include "bench.h"

__attribute__((noinline)) static long work(const long *b0, const long *b1, const long *b2, const long *b3, const long *b4, const long *b5, const long *b6, const long *b7, const long *b8, const long *b9, const long *b10, const long *b11, const long *b12, const long *b13, const long *b14, const long *b15, const long *b16, const long *b17, const long *b18, const long *b19, const long *b20, const long *b21, const long *b22, const long *b23, const long *b24, long n)
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
        s = s + b8[j];
        s = s + b9[j];
        s = s + b10[j];
        s = s + b11[j];
        s = s + b12[j];
        s = s + b13[j];
        s = s + b14[j];
        s = s + b15[j];
        s = s + b16[j];
        s = s + b17[j];
        s = s + b18[j];
        s = s + b19[j];
        s = s + b20[j];
        s = s + b21[j];
        s = s + b22[j];
        s = s + b23[j];
        s = s + b24[j];
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
    static const long v8[4] = { 9, 10, 11, 12 };
    static const long v9[4] = { 10, 11, 12, 13 };
    static const long v10[4] = { 11, 12, 13, 14 };
    static const long v11[4] = { 12, 13, 14, 15 };
    static const long v12[4] = { 13, 14, 15, 16 };
    static const long v13[4] = { 14, 15, 16, 17 };
    static const long v14[4] = { 15, 16, 17, 18 };
    static const long v15[4] = { 16, 17, 18, 19 };
    static const long v16[4] = { 17, 18, 19, 20 };
    static const long v17[4] = { 18, 19, 20, 21 };
    static const long v18[4] = { 19, 20, 21, 22 };
    static const long v19[4] = { 20, 21, 22, 23 };
    static const long v20[4] = { 21, 22, 23, 24 };
    static const long v21[4] = { 22, 23, 24, 25 };
    static const long v22[4] = { 23, 24, 25, 26 };
    static const long v23[4] = { 24, 25, 26, 27 };
    static const long v24[4] = { 25, 26, 27, 28 };
    long N = 2000000L * scale;
    printf("result: %ld\n", work(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, N));
    return 0;
}
