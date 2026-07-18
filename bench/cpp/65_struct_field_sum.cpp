/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/65_struct_field_sum.my: iterate a flat array<Struct>
 * (2000 Point3) and sum its scalar fields, amplified by an outer rep loop.
 * std::vector<Point3> mirrors the flat POD-struct array; the field reads match
 * MyLang's direct-from-bytes reads. s carries a `%` per element (a serial
 * dependency, no close-form); bench_sink_ptr(a.data()) at the top of each pass
 * blocks hoisting the loop-invariant reduction out of the outer loop (cf. 18). */
#include "bench.h"

struct Point3 {
    long x;
    long y;
    long z;
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::vector<Point3> a;
    for (long i = 0; i < 2000; i++)
        a.push_back(Point3{i, i * 2, i * 3});

    long s = 0;
    long reps = 2000L * scale;
    for (long r = 0; r < reps; r++) {
        bench_sink_ptr(a.data());
        for (const Point3 &p : a)
            s = (s + p.x + p.y + p.z) % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
