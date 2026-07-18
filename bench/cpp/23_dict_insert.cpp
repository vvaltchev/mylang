/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/23_dict_insert.my: insert N distinct integer keys
 * into a dict (std::unordered_map, the C++ hashmap MyLang's dict wraps), to
 * isolate hashing/insert cost. Result is the final map size (== N, all keys
 * distinct). The map fill is real work, so the loop can't be elided. */
#include "bench.h"
#include <unordered_map>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 300000L * scale;

    std::unordered_map<long, long> d;
    for (long i = 0; i < N; i++)
        d[i] = i * 2;

    printf("result: %ld\n", (long)d.size());
    return 0;
}
