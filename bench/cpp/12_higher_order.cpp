/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/12_higher_order.my: a function value (sq) passed as
 * an argument to apply(f, x) and called indirectly. A C++ function pointer
 * models the passed-as-value callable; -O3 devirtualizes/inlines the known
 * target - that IS the C++ advantage over the interpreter's indirect dispatch,
 * so we leave it inlinable. The accumulator is a serial dependency through `%`,
 * so it can't be vectorized or close-formed - no in-loop barrier needed. */
#include "bench.h"

static long sq(long x)
{
    return x * x;
}

static long apply(long (*f)(long), long x)
{
    return f(x);
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        s += apply(sq, i);
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
