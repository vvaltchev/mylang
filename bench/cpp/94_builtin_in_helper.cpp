/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/94_builtin_in_helper.my: a small helper using
 * abs/min/max, called in a tight loop. -O3 may inline dist and the
 * std:: calls - as in 08_func_call, that IS the C++ advantage. The
 * accumulator is a serial chain through `%` and feeds the argument, so the
 * loop cannot be vectorized or close-formed. */
#include "bench.h"
#include <algorithm>
#include <cstdlib>

static long dist(long a, long b)
{
    long d = std::labs(a - b);
    return std::min(d, 1000L) + std::max(a % 7, b % 5);
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++)
        s = (s + dist(i, s % 997)) % 1000000007;
    printf("result: %ld\n", s);
    return 0;
}
