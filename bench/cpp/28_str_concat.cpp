/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/28_str_concat.my: string building by repeated `+=`
 * (std::string, appended in place like MyLang's unaliased-string fast path). */
#include "bench.h"
#include <string>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    long N = 50000L * scale;
    std::string s;
    for (long i = 0; i < N; i++) {
        s += std::to_string(i);
        s += ",";
    }

    printf("result: %ld\n", (long)s.size());
    return 0;
}
