/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/15_array_slice_readonly.my: a read-only slice in a
 * loop. In MyLang base[1:999] is an O(1) copy-on-write VIEW (no copy until a
 * write), so the faithful C++ ceiling is an O(1) sub-view (pointer + length),
 * not Python's eager 998-element copy. Only the two ends are read. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::vector<long> base(1000);       /* range(1000) -> [0..999] */
    for (long i = 0; i < 1000; i++)
        base[i] = i;

    long N = 200000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        /* base[1:999]: an O(1) view over indices 1..998 (length 998). */
        const long *sl = base.data() + 1;
        long sl_len = 998;
        s += sl[0] + sl[sl_len - 1];
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
