/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/89_regs_float_08.my - the REGISTER-PRESSURE
 * family's PHASED float member: two loops, each hot on 4 accumulators
 * + its float index. Identical IEEE op order to the MyLang side, so
 * the printed 4-digit result matches exactly. */
#include "bench.h"

__attribute__((noinline)) static double work(long n)
{
    double f0 = 1.0;
    double f1 = 2.0;
    double f2 = 3.0;
    double f3 = 4.0;
    double f4 = 5.0;
    double f5 = 6.0;
    double f6 = 7.0;
    double f7 = 8.0;
    double fi = 0.0;
    double fj = 0.0;
    for (long i = 0; i < n; i++) {
        fi = fi + 1.0;
        f0 = (f0 + fi) * 0.5;
        f1 = (f1 * 0.5) + fi;
        f2 = (f2 + fi) * 0.625;
        f3 = (f3 * 0.375) + fi;
        if ((i & 1) == 0)
            f0 = f0 + 0.125;
    }
    for (long j = 0; j < n; j++) {
        fj = fj + 1.0;
        f4 = (f4 + fj) * 0.5625;
        f5 = (f5 * 0.4375) + fj;
        f6 = (f6 + fj) * 0.6875;
        f7 = (f7 * 0.3125) + fj;
        if ((j & 1) == 0)
            f4 = f4 + 0.125;
    }
    double s = 0.0;
    s = s + f0;
    s = s + f1;
    s = s + f2;
    s = s + f3;
    s = s + f4;
    s = s + f5;
    s = s + f6;
    s = s + f7;
    s = s + fi;
    s = s + fj;
    return s;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 2000000L * scale;
    printf("result: %.4f\n", work(N));
    return 0;
}
