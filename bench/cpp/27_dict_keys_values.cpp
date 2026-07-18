/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/27_dict_keys_values.my: keys()/values() builtins
 * materialize a key array and a value array each rep. The vectors are the work
 * being measured; bench_sink_ptr keeps their allocation from being elided. */
#include "bench.h"
#include <unordered_map>
#include <vector>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long M = 100000;
    std::unordered_map<long, long> d;
    for (long i = 0; i < M; i++)
        d[i] = i;

    long R = 20L * scale;
    long s = 0;
    for (long k = 0; k < R; k++) {
        /* MyLang's keys(d) then values(d) are TWO separate traversals of the
         * map, not one fused pass. Do the same: fill ks, then fill vs. The
         * bench_sink_ptr between them stops -O3 fusing the two traversals. */
        std::vector<long> ks;
        ks.reserve(d.size());
        for (const auto &kv : d)
            ks.push_back(kv.first);
        bench_sink_ptr(ks.data());
        std::vector<long> vs;
        vs.reserve(d.size());
        for (const auto &kv : d)
            vs.push_back(kv.second);
        bench_sink_ptr(ks.data());
        bench_sink_ptr(vs.data());
        s += (long)ks.size() + (long)vs.size();
    }

    printf("result: %ld\n", s);
    return 0;
}
