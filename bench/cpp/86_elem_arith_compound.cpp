/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/86_elem_arith_compound.my: compound arith stores
 * on array elements in a hot loop. Values stay non-negative, so C's
 * truncating / and % match MyLang's (and Python's flooring twin). */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 400000L * scale;
    long a[64];

    for (long i = 0; i < 64; i++)
        a[i] = i * 17 + 3;

    for (long t = 0; t < N; t++) {
        long j = t & 63;
        a[j] += (t & 1023) + 256;
        a[j] -= (t >> 3) & 255;
        a[j] *= 3;
        a[j] /= 2;
        a[j] %= 65536;
    }

    long s = 0;
    for (long i = 0; i < 64; i++)
        s += a[i];
    printf("result: %ld\n", s);
    return 0;
}
