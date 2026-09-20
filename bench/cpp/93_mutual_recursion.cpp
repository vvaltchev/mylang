/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/93_mutual_recursion.my: two functions that call
 * each other to depth ~600. BENCH-FAIR (class B, as 10_recursion_deep):
 * MyLang performs a real call per level, so both are noinline with the
 * sibling-call optimisation off - otherwise g++ turns the mutual TAIL
 * calls into jumps (a loop), and a loop is not what is being compared.
 * The depth varies with k, and bench_sink keeps every result live. */
#include "bench.h"

static long od(long n, long acc);

__attribute__((noinline, optimize("no-optimize-sibling-calls")))
static long ev(long n, long acc)
{
    if (n <= 0)
        return acc;
    return od(n - 1, (acc * 3 + n) % 1000003);
}

__attribute__((noinline, optimize("no-optimize-sibling-calls")))
static long od(long n, long acc)
{
    if (n <= 0)
        return acc + 1;
    return ev(n - 1, (acc + n * 7) % 1000003);
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long K = 2000L * scale;
    long s = 0;
    for (long k = 0; k < K; k++) {
        s = (s + ev(600 + k % 5, k)) % 1000000007;
        bench_sink(s);
    }
    printf("result: %ld\n", s);
    return 0;
}
