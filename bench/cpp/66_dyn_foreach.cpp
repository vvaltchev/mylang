/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/66_dyn_foreach.my: a foreach over a `dyn` container
 * that always holds an array<int> at runtime (range(1000)) -> a range-for over
 * a std::vector<long>. `s` is a serial modular-sum dependency carried across
 * every element of every rep (the mod through `s` makes the inner loop
 * non-invariant), so no barrier is needed. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    std::vector<long> a;
    a.reserve(1000);
    for (long j = 0; j < 1000; j++)
        a.push_back(j);

    long s = 0;
    long reps = 20000L * scale;

    for (long r = 0; r < reps; r++) {
        for (long e : a)
            s = (s + e) % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
