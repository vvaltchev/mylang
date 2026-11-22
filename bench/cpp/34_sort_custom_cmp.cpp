/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/34_sort_custom_cmp.my: fill with an LCG, sort with a
 * custom comparator (p < q, ascending), checksum min+max, R times. The same LCG
 * seed each pass regenerates identical data, so the p<q comparator matches
 * MyLang's ascending sort exactly. */
/* BENCH-FAIR (class B, 2026-08-14 - maintainer REVERSED the 2026-07-26
 * verdict that left this bench inlined). `std::sort` with a raw lambda
 * INLINES the comparison into the sort - the asm has no call at all, and
 * one `cmp` answers p<q. MyLang cannot do that: `sort(a, func(p,q) => p<q)`
 * passes a first-class FuncObject VALUE, and the builtin invokes it per
 * comparison. A raw lambda is therefore the counterpart of an INLINED
 * MyLang call, which this program does not have.
 *
 * The honest counterpart is the one the fairness toolbox already names:
 * a std::function (type-erased, indirect call) handed to a NOINLINE sort
 * wrapper, so the comparator stays opaque to the sort. Verified by asm:
 * the loop now contains `call *%r..` per comparison. */
#include "bench.h"
#include <algorithm>
#include <functional>

/* noinline + a type-erased comparator: the sort cannot see the body, so
 * every comparison is a real indirect call - MyLang's shape. */
__attribute__((noinline))
static void sort_with(std::vector<long> &v,
                      const std::function<bool(long, long)> &cmp)
{
    std::sort(v.begin(), v.end(),
              [&cmp](long p, long q) { return cmp(p, q); });
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    const long SZ = 50000;
    long R = 2 * scale;
    long checksum = 0;

    std::function<bool(long, long)> cmp =
        [](long p, long q) { return p < q; };

    for (long k = 0; k < R; k++) {
        std::vector<long> a(SZ, 0);
        long x = 987654321;
        for (long i = 0; i < SZ; i++) {
            x = (x * 1103515245 + 12345) % 2147483647;
            a[i] = x;
        }
        sort_with(a, cmp);
        checksum += a[0] + a[SZ - 1];
    }

    printf("result: %ld\n", checksum);
    return 0;
}
