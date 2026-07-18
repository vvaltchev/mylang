/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/69_exc_crossframe.my: an exception raised DEPTH
 * frames deep, caught at the top - measures the per-frame unwind cost of a
 * throw that crosses many frames. `deep` recurses (a NON-INLINED real call,
 * matching the .my's intent) and throws Err at the bottom; the top catches.
 * noinline forces the real call+throw+unwind each iteration (no EH elision). */
#include "bench.h"

struct Err { long v; };

__attribute__((noinline)) static void deep(long n, long i)
{
    if (n <= 0)
        throw Err{i};
    deep(n - 1, i);
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 20000L * scale;
    long DEPTH = 16;
    long caught = 0;

    for (long i = 0; i < N; i++) {
        try {
            deep(DEPTH, i);
        } catch (const Err &) {
            caught++;
        }
    }

    printf("result: %ld\n", caught);
    return 0;
}
