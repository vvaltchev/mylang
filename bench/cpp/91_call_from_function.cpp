/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/91_call_from_function.my: a small helper called in
 * a tight loop that lives in a function. -O3 may inline mix - as in
 * 08_func_call, that IS the C++ advantage over a call protocol. The
 * accumulator is a serial chain through `%` (and feeds mix's argument), so
 * the loop cannot be vectorized or close-formed. drive's argument comes from
 * the scale, which the compiler cannot see. */
#include "bench.h"

static long mix(long a, long b)
{
    long x = a * 31 + b;
    x = x ^ (x >> 7);
    x = x * 5 + 3;
    if (x < 0)
        x = 0 - x;
    return x % 1000003;
}

static long drive(long n)
{
    long s = 0;
    for (long i = 0; i < n; i++)
        s = (s + mix(i, s)) % 1000000007;
    return s;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    printf("result: %ld\n", drive(1000000L * scale));
    return 0;
}
