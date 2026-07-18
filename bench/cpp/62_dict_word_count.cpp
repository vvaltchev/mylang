/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/62_dict_word_count.my: word-frequency counting
 * with STRING keys (std::unordered_map, the C++ hashmap MyLang's dict wraps).
 * The result is an order-independent checksum, so map order doesn't matter. */
#include "bench.h"
#include <unordered_map>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    const std::vector<std::string> words = {
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
        "a", "an", "and", "of", "to", "in", "is", "it", "that", "was"};
    long nw = (long)words.size();
    long n = 2000000L * scale;

    std::unordered_map<std::string, long> counts;   /* default 0 == dict(0) */
    for (long i = 0; i < n; i++)
        counts[words[i % nw]] += 1;

    long total = 0;
    for (const auto &kv : counts)
        total += kv.second * (long)kv.first.size();

    printf("result: %ld\n", total);
    return 0;
}
