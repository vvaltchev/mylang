/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/101_float_call.my: four float recurrences plus
 * one call per iteration. BENCH-FAIR (as 97_regs_int_call): MyLang
 * performs a real call, so blend is noinline; g++ passes its arguments in
 * xmm registers, which is exactly the comparison. */
#include "bench.h"

__attribute__((noinline))
static double blend(double a, double b)
{
    double t = a * 0.75 + b * 0.25;
    t = t + a * b * 0.0001;
    if (t > 1000.0)
        t = t - 1000.0;
    return t * 0.999 + a * 0.0005;
}

__attribute__((noinline))
static long drive(long n)
{
    double x0 = 1.0, x1 = 2.0, x2 = 3.0, x3 = 4.0;
    double s = 0.0;
    for (long i = 0; i < n; i++) {
        x0 = x0 * 0.5 + 1.0;
        x1 = x1 * 0.25 + x0;
        x2 = x2 * 0.125 + x1;
        x3 = x3 * 0.5 + x2 * 0.5;
        s = blend(s, x3);
        bench_sink(static_cast<long>(s));
    }
    return static_cast<long>(s * 1000.0)
         + static_cast<long>(x0 + x1 + x2 + x3);
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    printf("result: %ld\n", drive(500000L * scale));
    return 0;
}
