/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/40_math_builtins.my: sqrt/sin/cos/log summed over a
 * range, printed to 2 decimals (MyLang `str(s, 2)` / Python `round(s, 2)`). The
 * transcendentals cannot be closed-formed; -O3 may vectorize the libm calls, a
 * legitimate C++ win. `s` reaches print. Uses double libm (like CPython); the
 * 2-decimal rounding agrees with MyLang at these scales. */
#include "bench.h"
#include <cmath>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 300000L * scale;
    double s = 0.0;

    for (long i = 1; i < N; i++) {
        double x = (double)i;
        s += std::sqrt(x) + std::sin(x) + std::cos(x) + std::log(x);
    }

    printf("result: %.2f\n", s);
    return 0;
}
