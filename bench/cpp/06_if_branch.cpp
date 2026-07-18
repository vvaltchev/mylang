/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/06_if_branch.my: if / else-if / else dispatch on
 * i % 3 each iteration. The data-dependent branch counters do not close-form
 * (verified: runtime scales linearly with N), so the loop runs for real - no
 * barrier needed; keeping it clean lets -O3 apply the branch work it would to
 * any real program, which is exactly the C++ ceiling we measure. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1500000L * scale;
    long a = 0, b = 0, c = 0;
    for (long i = 0; i < N; i++) {
        if (i % 3 == 0)
            a++;
        else if (i % 3 == 1)
            b++;
        else
            c++;
    }
    printf("result: %ld\n", a + b * 2 + c * 3);
    return 0;
}
