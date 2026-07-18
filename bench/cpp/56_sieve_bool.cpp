/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/56_sieve_bool.my: a Sieve of Eratosthenes over a
 * flat array<bool> (a byte per element, std::vector<char>), counting primes
 * below N. The sieve is fully read to produce `count` - no barrier. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 2000000L * scale;

    std::vector<char> sieve(N, 1);   /* array<bool>, all true */
    sieve[0] = 0;
    sieve[1] = 0;

    for (long i = 2; i * i < N; i++) {
        if (sieve[i]) {
            for (long j = i * i; j < N; j += i)
                sieve[j] = 0;
        }
    }

    long count = 0;
    for (long i = 2; i < N; i++)
        if (sieve[i])
            count++;

    printf("result: %ld\n", count);
    return 0;
}
