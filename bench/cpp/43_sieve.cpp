/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/43_sieve.my: Sieve of Eratosthenes, count primes
 * below n. Mirrors the .my structure (a bool array of size n, nested while). */
#include "bench.h"

/* BENCH-FAIR (plans/archived/bench-fairness.md, class C): g++ -O3 auto-VECTORIZED
 * the hot loop (asm-verified packed ops) - 2-16 lanes per instruction vs
 * MyLang's scalar native loop. The comparison is meant to be scalar
 * shape-vs-shape; revisit if MyLang ever vectorizes. */
#pragma GCC optimize ("no-tree-vectorize", "no-tree-slp-vectorize")

static long compute_primes(long n)
{
    std::vector<char> primes(n, 1);   /* MyLang array(n) of true */
    if (n > 0) primes[0] = 0;
    if (n > 1) primes[1] = 0;

    long i = 2;
    while (i * i < n) {
        if (primes[i]) {
            long j = i * i;
            while (j < n) {
                primes[j] = 0;
                j += i;
            }
        }
        i++;
    }
    long count = 0;
    for (long k = 2; k < n; k++)
        if (primes[k])
            count++;
    return count;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long n = 1000000L * scale;
    printf("result: %ld\n", compute_primes(n));
    return 0;
}
