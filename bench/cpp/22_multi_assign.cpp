/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/22_multi_assign.my: `a, b, c = [i, i+1, i+2]` each
 * iteration (MyLang elides the array literal into scalar stores), accumulating
 * (a + b + c) mod 1e9+7. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 600000L * scale;
    long a = 0, b = 0, c = 0;
    long s = 0;

    for (long i = 0; i < N; i++) {
        a = i;
        b = i + 1;
        c = i + 2;
        s += a + b + c;
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
