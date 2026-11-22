/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/39_find_builtin.my: find() = a linear scan in an
 * array (to the last element) + a substring search in a string, R times. */
#include "bench.h"
#include <string>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long SZ = 20000;
    std::vector<long> a(SZ);           /* range(SZ) -> [0..SZ-1] */
    for (long i = 0; i < SZ; i++)
        a[i] = i;

    std::string hay;                    /* "abcdefghi." * 1000 -> 10000 chars */
    hay.reserve(10000);
    for (long i = 0; i < 1000; i++)
        hay += "abcdefghi.";

    long R = 1000 * scale;
    long s = 0;

    for (long k = 0; k < R; k++) {
        /* MyLang re-scans the array every iteration; keep the scan live. */
        bench_sink_ptr(a.data());
        /* array find: linear scan for value SZ-1 (the last element). */
        long ai = -1;
        for (long j = 0; j < SZ; j++)
            if (a[j] == SZ - 1) { ai = j; break; }
        /* string find: substring search (returns npos if absent). */
        std::string::size_type pos = hay.find("i.");
        long hi = (pos == std::string::npos) ? -1 : (long)pos;

        if (ai != -1) s += ai;
        if (hi != -1) s += hi;
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
