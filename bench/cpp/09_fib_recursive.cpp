/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/09_fib_recursive.my: naive tree recursion.
 * bench_sink on the result each rep so the compiler can't hoist the call
 * out of the loop (fib(29) is loop-invariant). */
#include "bench.h"

static long fib(long n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long r = 0;
    for (long k = 0; k < scale; k++) {
        r = fib(29);
        bench_sink(r);
    }
    printf("result: %ld\n", r);
    return 0;
}
