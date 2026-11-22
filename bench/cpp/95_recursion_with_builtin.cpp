/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/95_recursion_with_builtin.my: a depth-~500
 * recursion calling std::max at every level. BENCH-FAIR (class B, as
 * 10_recursion_deep): MyLang performs a real call per level, so climb is
 * noinline with the sibling-call optimisation off - g++ would otherwise
 * turn the tail call into a loop. The depth varies with k and bench_sink
 * keeps every result live. */
#include "bench.h"
#include <algorithm>

__attribute__((noinline, optimize("no-optimize-sibling-calls")))
static long climb(long n, long h)
{
    if (n <= 0)
        return h;
    return climb(n - 1, std::max(h, (n * 37) % 1009) - 1);
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long K = 2000L * scale;
    long s = 0;
    for (long k = 0; k < K; k++) {
        s = (s + climb(500 + k % 7, k % 11)) % 1000000007;
        bench_sink(s);
    }
    printf("result: %ld\n", s);
    return 0;
}
