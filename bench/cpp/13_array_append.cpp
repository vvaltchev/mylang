/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/13_array_append.my: grow an array element-by-element
 * via push() (amortized O(1), like std::vector::push_back). Result is len(a). */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    std::vector<long> a;
    for (long i = 0; i < N; i++)
        a.push_back(i);

    /* MyLang actually builds the whole array; force the growth to materialize
     * so the compiler can't skip it and just print the (trivially N) length. */
    bench_sink_ptr(a.data());
    printf("result: %ld\n", (long)a.size());
    return 0;
}
