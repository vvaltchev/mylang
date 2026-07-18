/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/67_make_dict.my: build a fresh 400-entry dict per
 * rep from an array of keys via a value callback (the dict-comprehension form
 * { k: k*k + r for k in ks }). Values depend on the loop counter r. */
#include "bench.h"
#include <unordered_map>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long nkeys = 400;
    long reps = 3000L * scale;

    long total = 0;
    for (long r = 0; r < reps; r++) {
        std::unordered_map<long, long> d;
        for (long k = 0; k < nkeys; k++)
            d[k] = k * k + r;
        total += d[r % nkeys];
    }

    printf("result: %ld\n", total);
    return 0;
}
