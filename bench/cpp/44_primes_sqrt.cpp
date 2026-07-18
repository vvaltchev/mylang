/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/44_primes_sqrt.my: count primes below LIMIT by trial
 * division up to sqrt(n). is_prime(n) has data-dependent early returns and a
 * nested loop over varying n, so nothing close-forms or hoists - no barrier
 * needed. -O3 inlines is_prime, the C++ advantage we measure. */
#include "bench.h"

static bool is_prime(long n)
{
    for (long f = 2; f * f <= n; f++)
        if (n % f == 0)
            return false;
    return true;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long LIMIT = 100000L * scale;
    long count = 0;
    for (long n = 2; n < LIMIT; n++)
        if (is_prime(n))
            count++;
    printf("result: %ld\n", count);
    return 0;
}
