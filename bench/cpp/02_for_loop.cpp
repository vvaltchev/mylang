/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/02_for_loop.my: C-style counted loop, sum of i.
 * The accumulator is a plain sum (s += i), which the compiler would close-form
 * to N*(N-1)/2 (an O(1) "precompute the whole loop") - forbidden by the
 * anti-const-fold contract. bench_sink(s) per iteration forces the fair native
 * SCALAR loop, the ceiling for the interpreter's per-iteration cost. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 3000000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        s += i;
        bench_sink(s);
    }
    printf("result: %ld\n", s);
    return 0;
}
