/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/52_cse_dedup.my: two heavy constant expressions,
 * `sum(sort(base))` and `sum(base + base)`, each appear TWICE in the loop body.
 * MyLang evaluates each once at parse time (CSE shares the sorted/concatenated
 * array) and folds the whole `sum(...)` to an integer literal, so its runtime
 * loop is a bare modular add. This C++ reproduces the SAME work literally - two
 * full std::sort+accumulate and two concat+accumulate per iteration over the
 * 3000-element `base` = reverse(range(3000)) - so the printed result matches.
 * (Unlike the pure-arithmetic fold benches, -O3 does NOT fold a 3000-element
 * std::sort, so this ceiling reflects the real cost of doing the literal work;
 * that is exactly the compile-time-folding advantage the .my demonstrates.) */
#include "bench.h"
#include <algorithm>
#include <numeric>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long SZ = 3000;
    std::vector<long> base(SZ);
    for (long i = 0; i < SZ; i++)
        base[i] = SZ - 1 - i;               /* reverse(range(3000)) */

    long N = 1500L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        /* sum(sort(base)) - twice */
        std::vector<long> t1 = base;
        std::sort(t1.begin(), t1.end());
        long a = std::accumulate(t1.begin(), t1.end(), 0L);
        std::vector<long> t2 = base;
        std::sort(t2.begin(), t2.end());
        long b = std::accumulate(t2.begin(), t2.end(), 0L);

        /* sum(base + base) - twice (base + base is concatenation) */
        std::vector<long> c1;
        c1.reserve(2 * SZ);
        c1.insert(c1.end(), base.begin(), base.end());
        c1.insert(c1.end(), base.begin(), base.end());
        long c = std::accumulate(c1.begin(), c1.end(), 0L);
        std::vector<long> c2;
        c2.reserve(2 * SZ);
        c2.insert(c2.end(), base.begin(), base.end());
        c2.insert(c2.end(), base.begin(), base.end());
        long d = std::accumulate(c2.begin(), c2.end(), 0L);

        s = (s + a + b + c + d) % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
