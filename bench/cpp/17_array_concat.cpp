/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/17_array_concat.my: array concatenation with `+`
 * builds a fresh 1000-element array each iteration; only its length is used. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::vector<long> a(500), b(500);   /* range(500), range(500) */
    for (long i = 0; i < 500; i++) { a[i] = i; b[i] = i; }

    long N = 100000L * scale;
    long total = 0;

    for (long i = 0; i < N; i++) {
        std::vector<long> c;
        c.reserve(a.size() + b.size());
        c.insert(c.end(), a.begin(), a.end());
        c.insert(c.end(), b.begin(), b.end());
        /* MyLang materializes all 1000 elements of the fresh array; force the
         * build so the compiler can't reduce it to the constant length. */
        bench_sink_ptr(c.data());
        total += (long)c.size();
        total = total % 1000000007;
    }

    printf("result: %ld\n", total);
    return 0;
}
