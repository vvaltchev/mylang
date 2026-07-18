/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/74_dyn_foreach_kv.my: a 2-var foreach (k, v) over a
 * `dyn` container that always holds a dict at runtime -> a range-for over a
 * std::unordered_map<long,long> (the hashmap MyLang's dict wraps). `total`
 * sums k+v over every entry each rep with no per-element dependency, so the
 * inner map-iteration is loop-invariant; a per-rep memory barrier stops the
 * compiler hoisting/closed-forming it, forcing the map to be iterated each rep
 * (the honest ceiling for "iterate this map N times"). The k+v sum is
 * order-independent, so the unordered iteration order is irrelevant. */
#include "bench.h"
#include <unordered_map>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 200000L * scale;

    std::unordered_map<long, long> d;
    for (long i = 0; i < 100; i++)
        d[i] = i * 2;

    long total = 0;

    for (long r = 0; r < N; r++) {
        /* Break the compiler's proof that d is unchanged, so the inner
         * foreach is actually re-iterated each rep (not hoisted to C*N). */
        bench_sink_ptr(&d);
        for (const auto &kv : d)
            total += kv.first + kv.second;
    }

    printf("result: %ld\n", total % 1000000007);
    return 0;
}
