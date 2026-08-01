/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/56_sieve_bool.my: a Sieve of Eratosthenes over a
 * flat array<bool> (a byte per element, std::vector<char>), counting primes
 * below N. The sieve is fully read to produce `count` - no barrier. */
#include "bench.h"

/* BENCH-FAIR (plans/archived/bench-fairness.md, class C): g++ -O3 auto-VECTORIZED
 * the hot loop (asm-verified packed ops) - 2-16 lanes per instruction vs
 * MyLang's scalar native loop. The comparison is meant to be scalar
 * shape-vs-shape; revisit if MyLang ever vectorizes. */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")

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
