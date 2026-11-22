/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/59_bit_hash.my: an FNV-style 32-bit hash mixed over
 * a range - xor, multiply, rotate-left-13, all masked back into 32 bits. `h`
 * stays in [0, 2^32), so a signed 64-bit `long` holds every intermediate with
 * no overflow (h*16777619 < 2^56). `>>>` is a logical shift; h is non-negative
 * so a plain `>>` is identical. The accumulator is a serial dependency chain
 * (no vectorization / closed form), and h reaches print - no barrier. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 800000L * scale;
    long MASK = (1L << 32) - 1;
    long h = 2166136261L;                        /* FNV-1a 32-bit offset basis */

    for (long i = 0; i < N; i++) {
        h = (h ^ i) & MASK;
        h = (h * 16777619L) & MASK;              /* FNV prime */
        h = ((h << 13) | (h >> 19)) & MASK;      /* rotate-left 13 in 32 bits */
    }

    printf("result: %ld\n", h);
    return 0;
}
