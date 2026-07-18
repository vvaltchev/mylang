/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/47_wordcount.my: build text from a deterministic
 * word stream, join() it, split() into tokens, count word frequencies in a
 * default dict (std::unordered_map<std::string,long>). Checksum = total token
 * count (order-independent); distinct = number of distinct words seen.
 * The RNG mods by 8, and 2^64 is divisible by 8, so 64-bit wrapping and
 * Python's bignum give the identical sequence. */
#include "bench.h"
#include <unordered_map>
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

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; ; i++) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            if (i == s.size()) break;
            start = i + 1;
        }
    }
    return out;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const std::vector<std::string> words = {
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog"};

    long n = 200000L * scale;
    std::vector<std::string> sb(n);
    long x = 7;
    for (long i = 0; i < n; i++) {
        x = (x * 1103515245L + 12345L) % 8;
        sb[i] = words[x];
    }
    std::string text = join(sb, ' ');

    std::unordered_map<std::string, long> counts;   /* default 0 == dict(0) */
    std::vector<std::string> toks = split(text, ' ');

    for (const std::string &w : toks)
        counts[w]++;

    long total = 0;
    for (const auto &kv : counts)
        total += kv.second;

    printf("result: %ld distinct: %ld\n", total, (long)counts.size());
    return 0;
}
