/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/88_elem_float_compound.my: compound float stores
 * on array elements. All values positive, so fmod agrees with Python's
 * floored % and MyLang's. The sum accumulates in source order. */
#include "bench.h"
#include <cmath>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 400000L * scale;
    double f[64];

    for (long i = 0; i < 64; i++)
        f[i] = i * 0.75 + 0.5;

    for (long t = 0; t < N; t++) {
        long j = t & 63;
        f[j] += 1.25 + (double)(t & 7) * 0.375;
        f[j] *= 0.5;
        f[j] -= 0.125;
        f[j] /= 1.5;
        f[j] = fmod(f[j], 1.0);
    }

    double s = 0.0;
    for (long i = 0; i < 64; i++)
        s += f[i];
    printf("result: %.4f\n", s);
    return 0;
}
