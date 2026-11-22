/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/25_dict_member.my: string-keyed dictionary member
 * access (d.key == d["key"]) in a loop (std::unordered_map<std::string,long>). */
#include "bench.h"
#include <unordered_map>
#include <string>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::unordered_map<std::string, long> d = {
        {"alpha", 1}, {"beta", 2}, {"gamma", 3}, {"delta", 4}};

    long N = 500000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        s += d["alpha"] + d["beta"] + d["gamma"] + d["delta"];
        s = s % 1000000007L;
    }

    printf("result: %ld\n", s);
    return 0;
}
