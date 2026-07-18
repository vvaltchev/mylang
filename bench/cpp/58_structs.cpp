/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/58_structs.my: build a flat array<Struct> by
 * appending N POD structs, then reduce over their fields. MyLang's POD Point is
 * two int scalars laid out in bytes; std::vector<Point> mirrors the flat array.
 * The reduction reads EVERY element (both fields feed the printed sums), so the
 * build can't be DCE'd; bench_sink_ptr keeps the vector materialized so the
 * compiler can't fuse build+reduce and close-form the whole thing. */
#include "bench.h"

struct Point {
    long x;
    long y;
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 100000L * scale;

    std::vector<Point> pts;
    for (long i = 0; i < N; i++)
        pts.push_back(Point{i, i * 2});

    bench_sink_ptr(pts.data());

    long sx = 0, sy = 0;
    for (const Point &p : pts) {
        sx += p.x;
        sy += p.y;
    }

    printf("result: %ld %ld\n", sx, sy);
    return 0;
}
