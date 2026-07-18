/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/07_nested_loops.my: O(OUT*IN) inner body. The
 * accumulator is a serial dependency chain through `%` (s = s % 1000000007
 * every iteration), so the compiler can neither vectorize nor close-form it -
 * no in-loop barrier needed. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long OUT = 1500L * scale;
    long IN = 1000;
    long s = 0;
    for (long i = 0; i < OUT; i++) {
        for (long j = 0; j < IN; j++) {
            s += i * j;
            s = s % 1000000007;
        }
    }
    printf("result: %ld\n", s);
    return 0;
}
