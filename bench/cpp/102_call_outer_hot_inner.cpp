/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/102_call_outer_hot_inner.my: an outer loop
 * with one real call per iteration around an inner loop of eight int
 * recurrences. BENCH-FAIR (as 97_regs_int_call): seed is noinline. */
#include "bench.h"

__attribute__((noinline))
static long seed(long k)
{
    long x = (long)((unsigned long)k * 2654435761UL + 12345UL);
    x = x ^ (x >> 13);
    if (x < 0)
        x = 0 - x;
    return x % 65521;
}

__attribute__((noinline))
static long drive(long n)
{
    long total = 0;
    for (long o = 0; o < n; o++) {
        long b = seed(o);
        long a0 = b, a1 = b + 1, a2 = b + 2, a3 = b + 3;
        long a4 = b + 4, a5 = b + 5, a6 = b + 6, a7 = b + 7;
        for (long i = 0; i < 100; i++) {
            a0 = (a0 * 3 + i) & 65535;
            a1 = (a1 ^ (i >> 1)) + 7;
            a2 = (a2 + a0) & 65535;
            a3 = (a3 * 5 + 1) & 65535;
            a4 = (a4 ^ a1) & 65535;
            a5 = (a5 + i * 2) & 65535;
            a6 = (a6 * 7 + a2) & 65535;
            a7 = (a7 + a3 + a5) & 65535;
        }
        total = (total + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7)
                % 1000000007;
        bench_sink(total);
    }
    return total;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    printf("result: %ld\n", drive(10000L * scale));
    return 0;
}
