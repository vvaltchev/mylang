/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/97_regs_int_call.my: eight int recurrences plus
 * one call per iteration. BENCH-FAIR (as 91_call_from_function): MyLang
 * performs a real call, so mix is noinline; g++ keeps the eight locals in
 * registers across it where it can, which is exactly the comparison. */
#include "bench.h"

__attribute__((noinline))
static long mix(long a, long b)
{
    long x = a * 31 + b;
    x = x ^ (x >> 7);
    x = x * 5 + 3;
    if (x < 0)
        x = 0 - x;
    return x % 1000003;
}

__attribute__((noinline))
static long drive(long n)
{
    long a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8;
    long s = 0;
    for (long i = 0; i < n; i++) {
        a0 = (a0 * 3 + i) & 65535;
        a1 = (a1 ^ (i >> 1)) + 7;
        a2 = (a2 + a0) & 65535;
        a3 = (a3 * 5 + 1) & 65535;
        a4 = (a4 ^ a1) & 65535;
        a5 = (a5 + i * 2) & 65535;
        a6 = (a6 * 7 + a2) & 65535;
        a7 = (a7 + a3 + a5) & 65535;
        s = (s + mix(i, a4)) % 1000000007;
        bench_sink(s);
    }
    return s + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    printf("result: %ld\n", drive(500000L * scale));
    return 0;
}
