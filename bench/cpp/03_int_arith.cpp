/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/03_int_arith.my: + - * / % mix, int64 (-fwrapv).
 * The accumulator is a serial dependency chain through `%`, so the compiler
 * can neither vectorize nor close-form it - no in-loop barrier needed. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;
    long acc = 1;
    for (long i = 1; i < N; i++) {
        acc = (acc + i) * 3;
        acc = acc % 1000000007;
        acc = acc + i / 2 - i % 7;   /* truncating /, matching MyLang */
    }
    printf("result: %ld\n", acc);
    return 0;
}
