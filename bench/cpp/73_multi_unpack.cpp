/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/73_multi_unpack.my: destructure a runtime array
 * VALUE `a, b, c = arr` each iteration (a strict length-checked unpack in
 * MyLang), accumulating (a + b + c) mod 1e9+7. The array of 3 is kept real so
 * the unpack reads it, mirroring the .my. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 600000L * scale;
    long arr[3] = {0, 0, 0};
    long s = 0;

    for (long i = 0; i < N; i++) {
        arr[0] = i;
        arr[1] = i + 1;
        arr[2] = i + 2;
        long a = arr[0], b = arr[1], c = arr[2];
        s += a + b + c;
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
