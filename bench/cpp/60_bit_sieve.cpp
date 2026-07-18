/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/60_bit_sieve.my: a bit-packed Sieve of Eratosthenes
 * (64 bits/word in a flat int array; a set bit = composite). `>>>` is a logical
 * shift and `1 << 63` sets the word's sign bit, so the mask is built and the
 * word read through UNSIGNED shifts to reproduce MyLang's int64 bit patterns
 * exactly. The sieve is fully read to produce `count` - no barrier. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 400000L * scale;
    long WORDS = (N >> 6) + 1;
    std::vector<long> bits(WORDS, 0);            /* flat array<int>, all zero */

    for (long i = 2; i * i < N; i++) {
        if ((((unsigned long)bits[i >> 6] >> (i & 63)) & 1L) == 0) {  /* prime */
            for (long j = i * i; j < N; j += i)
                bits[j >> 6] = bits[j >> 6] | (long)(1UL << (j & 63));
        }
    }

    long count = 0;
    for (long i = 2; i < N; i++)
        if ((((unsigned long)bits[i >> 6] >> (i & 63)) & 1L) == 0)
            count++;

    printf("result: %ld\n", count);
    return 0;
}
