/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/31_str_split_join.my: split() a CSV string into
 * fields and join() them back, repeatedly (std::string, the storage MyLang's
 * strings wrap). split/join written plainly to mirror the builtins. */
#include "bench.h"

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

    std::vector<std::string> parts(1000);
    for (int i = 0; i < 1000; i++)
        parts[i] = std::to_string(i);
    std::string csv = join(parts, ',');

    long R = 2000L * scale;
    long total = 0;
    for (long k = 0; k < R; k++) {
        std::vector<std::string> fields = split(csv, ',');
        std::string back = join(fields, ',');
        bench_sink_ptr(back.data());       /* keep `back` live (join's work) */
        total += (long)fields.size();
    }
    printf("result: %ld\n", total);
    return 0;
}
