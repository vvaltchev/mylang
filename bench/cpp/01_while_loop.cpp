/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/01_while_loop.my: counter increment + accumulate. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 3000000L * scale;
    long i = 0, s = 0;
    while (i < N) {
        s += i;
        i++;
        /* barrier per iteration: without it the compiler closed-forms the
         * sum to N*(N-1)/2 (an O(1) "precompute the whole loop"), which the
         * anti-const-fold contract forbids - we measure a native SCALAR loop,
         * the fair ceiling for the interpreter's per-iteration cost. */
        bench_sink(s);
    }
    printf("result: %ld\n", s);
    return 0;
}
