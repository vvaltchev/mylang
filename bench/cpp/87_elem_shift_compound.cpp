/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/87_elem_shift_compound.my: compound shift/bitwise
 * stores on array elements. Values stay non-negative below 2^61 (the &= MASK
 * bound), so a plain >> equals MyLang's >>> and << never overflows. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 400000L * scale;
    const long MASK = (1L << 60) - 1;
    long a[64];

    for (long i = 0; i < 64; i++)
        a[i] = i * 2654435761L + 1;

    for (long t = 0; t < N; t++) {
        long j = t & 63;
        a[j] <<= 1;
        a[j] |= t & 1;
        a[j] &= MASK;
        a[j] ^= 40503L * (j + 1);
        a[j] >>= 2;
        a[j] >>= 1;
    }

    long s = 0;
    for (long i = 0; i < 64; i++)
        s += a[i];
    printf("result: %ld\n", s);
    return 0;
}
