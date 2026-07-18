/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/24_dict_lookup.my: repeated dictionary lookups by
 * integer key (std::unordered_map, the C++ hashmap MyLang's dict wraps). */
#include "bench.h"
#include <unordered_map>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long M = 10000;
    std::unordered_map<long, long> d;
    for (long i = 0; i < M; i++)
        d[i] = i;

    long N = 1000000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        s += d[i % M];
        s = s % 1000000007L;
    }

    printf("result: %ld\n", s);
    return 0;
}
