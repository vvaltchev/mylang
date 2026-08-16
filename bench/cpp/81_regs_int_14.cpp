/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/81_regs_int_14.my - the REGISTER-PRESSURE
 * family. 14 independent loop-carried accumulators: at this count the
 * compiler must SPILL some of them. -fwrapv matches MyLang's
 * wrapping int64. */
#include "bench.h"

__attribute__((noinline)) static long work(long n)
{
    long a0 = 1;
    long a1 = 2;
    long a2 = 3;
    long a3 = 4;
    long a4 = 5;
    long a5 = 6;
    long a6 = 7;
    long a7 = 8;
    long a8 = 9;
    long a9 = 10;
    long a10 = 11;
    long a11 = 12;
    long a12 = 13;
    long a13 = 14;
    for (long i = 0; i < n; i++) {
        a0 = a0 + i;
        a0 = a0 ^ (a0 >> 3);
        a1 = a1 + i;
        a1 = a1 ^ (a1 >> 4);
        a2 = a2 + i;
        a2 = a2 ^ (a2 >> 5);
        a3 = a3 + i;
        a3 = a3 ^ (a3 >> 6);
        a4 = a4 + i;
        a4 = a4 ^ (a4 >> 7);
        a5 = a5 + i;
        a5 = a5 ^ (a5 >> 3);
        a6 = a6 + i;
        a6 = a6 ^ (a6 >> 4);
        a7 = a7 + i;
        a7 = a7 ^ (a7 >> 5);
        a8 = a8 + i;
        a8 = a8 ^ (a8 >> 6);
        a9 = a9 + i;
        a9 = a9 ^ (a9 >> 7);
        a10 = a10 + i;
        a10 = a10 ^ (a10 >> 3);
        a11 = a11 + i;
        a11 = a11 ^ (a11 >> 4);
        a12 = a12 + i;
        a12 = a12 ^ (a12 >> 5);
        a13 = a13 + i;
        a13 = a13 ^ (a13 >> 6);
        if ((i & 1) == 0)
            a0 = a0 + 1;
    }
    long s = 0;
    s = s + a0;
    s = s + a1;
    s = s + a2;
    s = s + a3;
    s = s + a4;
    s = s + a5;
    s = s + a6;
    s = s + a7;
    s = s + a8;
    s = s + a9;
    s = s + a10;
    s = s + a11;
    s = s + a12;
    s = s + a13;
    return s;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 2000000L * scale;
    printf("result: %ld\n", work(N));
    return 0;
}
