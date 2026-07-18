/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/26_dict_iterate.my: foreach over a dict's <key,
 * value> pairs, summing values (order-independent checksum). */
#include "bench.h"
#include <unordered_map>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long M = 100000;
    std::unordered_map<long, long> d;
    for (long i = 0; i < M; i++)
        d[i] = i * 2;

    long R = 5L * scale;
    long s = 0;
    for (long k = 0; k < R; k++) {
        for (const auto &kv : d)
            s += kv.second;
        s = s % 1000000007L;
    }

    printf("result: %ld\n", s);
    return 0;
}
