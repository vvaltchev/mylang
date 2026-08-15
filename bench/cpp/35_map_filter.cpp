/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/35_map_filter.my: map(x=>x*2) then
 * filter(x=>x%3==0) over [0..SZ-1], printing len(c). Builds the real
 * intermediate arrays MyLang builds - a sized `b` for map, a grown `c`
 * (push_back) for filter - so the measured work matches. bench_sink keeps the
 * arrays materialized; c.size() reaches print. */
/* BENCH-FAIR (class B, 2026-08-14 - maintainer REVERSED the 2026-07-26
 * verdict that left the callback benches inlined). The map and filter
 * BODIES used to be written straight into the loops here, so the asm had
 * no call at all. MyLang passes a first-class FuncObject VALUE to
 * `map`/`filter` and the builtin invokes it PER ELEMENT; an inline body
 * is the counterpart of an inlined MyLang call, which this program does
 * not have. std::function (type-erased, indirect) through a NOINLINE
 * map/filter is the honest shape - asm-verified: one `call` per element
 * in each loop. */
#include "bench.h"
#include <functional>

__attribute__((noinline))
static std::vector<long> map_with(const std::vector<long> &src,
                                  const std::function<long(long)> &f)
{
    std::vector<long> out(src.size());
    for (size_t i = 0; i < src.size(); i++)
        out[i] = f(src[i]);
    return out;
}

__attribute__((noinline))
static std::vector<long> filter_with(const std::vector<long> &src,
                                     const std::function<bool(long)> &f)
{
    std::vector<long> out;
    for (size_t i = 0; i < src.size(); i++)
        if (f(src[i]))
            out.push_back(src[i]);
    return out;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long SZ = 500000L * scale;

    std::vector<long> a(SZ);
    for (long i = 0; i < SZ; i++)
        a[i] = i;                    /* range(SZ) == [0..SZ-1] */

    std::function<long(long)> dbl = [](long x) { return x * 2; };
    std::function<bool(long)> div3 = [](long x) { return x % 3 == 0; };

    std::vector<long> b = map_with(a, dbl);      /* map(x => x*2, a) */
    std::vector<long> c = filter_with(b, div3);  /* filter(x => x%3==0, b) */

    bench_sink_ptr(b.data());
    bench_sink_ptr(c.data());

    printf("result: %ld\n", (long)c.size());
    return 0;
}
