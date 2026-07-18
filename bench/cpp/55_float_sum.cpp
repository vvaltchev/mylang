/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/55_float_sum.my: a floating-point numeric series
 * reduced into `total`. MyLang float_type == double, so a plain double sum in
 * the same order reproduces the value bit-for-bit. The accumulator is a serial
 * dependency chain (no reassociation without -ffast-math), so it stays scalar
 * and in-order - no in-loop barrier needed; `total` reaches print. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 2000000L * scale;
    double total = 0.0;

    for (long i = 1; i < N; i++) {
        double x = i * 1.0;
        total += x / (x + 1.0) - 0.5 / x + 1.0 / (x * x);
    }

    printf("result: %f\n", total);
    return 0;
}
