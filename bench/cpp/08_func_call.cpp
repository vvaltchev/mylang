/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/08_func_call.my: a small function invoked in a tight
 * loop (work(a,b) => a*b + a - b). -O3 inlines work - that IS the C++ advantage
 * over the interpreter's per-call dispatch, so we leave it inlinable. The
 * accumulator is a serial dependency chain through `%`, so it can't be
 * vectorized or close-formed - no in-loop barrier needed. */
#include "bench.h"

static long work(long a, long b)
{
    return a * b + a - b;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        s += work(i, 2);
        s = s % 1000000007;
    }
    printf("result: %ld\n", s);
    return 0;
}
