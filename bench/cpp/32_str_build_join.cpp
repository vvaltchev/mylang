/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/32_str_build_join.my: collect parts in an array,
 * join() once (the idiomatic fast string build). join() is the shared builtin
 * being measured; the joined length is printed (a real use). */
#include "bench.h"
#include <string>
#include <vector>

static std::string join(const std::vector<std::string> &v, char sep)
{
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) out.push_back(sep);
        out += v[i];
    }
    return out;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    long N = 200000L * scale;
    std::vector<std::string> parts(N);
    for (long i = 0; i < N; i++)
        parts[i] = std::to_string(i);

    std::string joined = join(parts, ',');
    printf("result: %ld\n", (long)joined.size());
    return 0;
}
