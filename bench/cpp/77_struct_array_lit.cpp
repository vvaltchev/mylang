/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/77_struct_array_lit.my: a flat array<PodStruct>
 * LITERAL `[P(i, i+1), P(i+2, i+3)]` built PER ITERATION. MyLang packs the two
 * POD structs into a fresh flat (heap) array each iteration; std::vector<P>
 * mirrors that per-iteration allocation + build. Only row[0].x and row[1].y are
 * read for the checksum, so bench_sink_ptr(row.data()) forces the WHOLE row to
 * be materialized (rule 3). acc carries a `%` serial dependency - no close-form. */
#include "bench.h"

struct P {
    long x;
    long y;
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 500000L * scale;
    long acc = 0;

    for (long i = 0; i < N; i++) {
        std::vector<P> row = {P{i, i + 1}, P{i + 2, i + 3}};
        bench_sink_ptr(row.data());
        acc += row[0].x + row[1].y;
        acc = acc % 1000000007;
    }

    printf("result: %ld\n", acc);
    return 0;
}
