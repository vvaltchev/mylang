/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/45_gcd.my: Euclidean GCD over many varied operand
 * pairs. gcd() is inlined by -O3 (the C++ advantage). The accumulator is a
 * serial dependency chain through `%` (s = s % 1000000007 each iteration), and
 * gcd's operands vary with i, so nothing close-forms - no barrier needed. */
#include "bench.h"

static long gcd(long a, long b)
{
    while (b != 0) {
        long t = b;
        b = a % b;
        a = t;
    }
    return a;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 150000L * scale;
    long s = 0;
    for (long i = 1; i < N; i++) {
        s += gcd(i * 12345 % 999983, i * 54321 % 999983 + 1);
        s = s % 1000000007;
    }
    printf("result: %ld\n", s);
    return 0;
}
