/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/75_indexed_unpack.my: build 50 rows of [name, val]
 * strings, then N times run an `indexed` foreach that unpacks (i, name, val)
 * and accumulates i + len(name) + len(val) (no intermediate modulo; mod only
 * at the end). Reproduced as a vector of (string,string) rows iterated by a
 * range-for with a running index. bench_sink_ptr blocks -O3 from hoisting the
 * loop-invariant inner reduction out of the outer loop (an O(1) collapse), so
 * the inner scan runs each of the N passes - matching the .my's per-pass work. */
#include "bench.h"
#include <string>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 100000L * scale;

    /* MyLang's array<array<str>> is an outer array of 2-element str sub-arrays
     * - match it with a nested vector, not a std::pair. */
    std::vector<std::vector<std::string>> rows;
    for (long j = 0; j < 50; j++)
        rows.push_back({"item" + std::to_string(j), std::to_string(j * 2)});

    long acc = 0;
    for (long r = 0; r < N; r++) {
        bench_sink_ptr(rows.data());        /* block hoisting the reduction */
        long i = 0;
        for (const auto &row : rows) {
            acc += i + (long)row[0].size() + (long)row[1].size();
            i++;
        }
    }

    printf("result: %ld\n", acc % 1000000007);
    return 0;
}
