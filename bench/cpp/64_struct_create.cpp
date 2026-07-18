/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/64_struct_create.my: construct POD structs
 * STANDALONE (not into an array) in a loop, read their fields, accumulate. An
 * int-POD Point and a float-POD Vec3, one of each per iteration. -O3 inlines
 * the construction+field-read to arithmetic - that IS the C++ ceiling. The
 * float accumulation fs is non-associative, so the compiler keeps the loop
 * sequential (O(N)); bench_sink(sx) blocks close-forming the int sum. */
#include "bench.h"

struct Point {
    long x;
    long y;
};

struct Vec3 {
    double x;
    double y;
    double z;
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 500000L * scale;
    long sx = 0;
    double fs = 0.0;

    for (long i = 0; i < N; i++) {
        Point p{i, i * 2};                          /* int-POD construction */
        sx += p.x + p.y;
        bench_sink(sx);
        Vec3 v{i * 1.0, i * 2.0, i * 3.0};          /* float-POD construction */
        fs += v.x + v.y + v.z;
    }

    printf("result: %ld %ld\n", sx, (long)fs);
    return 0;
}
