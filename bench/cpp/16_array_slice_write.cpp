/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/16_array_slice_write.my: slice-then-write. The write
 * through the slice forces MyLang's copy-on-write clone of the 1000-element
 * storage (O(k) per iteration) - so the faithful C++ copies the vector each
 * iteration, matching that O(k) work (and what Python pays up front). */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::vector<long> base(1000);       /* range(1000) -> [0..999] */
    for (long i = 0; i < 1000; i++)
        base[i] = i;

    long N = 100000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        std::vector<long> sl = base;    /* the COW clone the write triggers */
        sl[0] = i;
        /* Force the 1000-element clone to actually materialize (MyLang does the
         * full copy); without this the compiler drops the dead-but-copied
         * elements since only sl[0] is read afterwards. */
        bench_sink_ptr(sl.data());
        s += sl[0];
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
