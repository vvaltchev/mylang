/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/70_exc_runtime_error.my: a native runtime error
 * (division by zero) caught in a loop (EAFP). The divisor is read from a
 * mutable array so it is a genuine RUNTIME zero - a memory barrier on the
 * array stops the compiler proving it is 0, so the div-by-zero check and the
 * throw run every iteration (C++ has no throwing `/`, so we throw at d==0,
 * the same condition MyLang's IntBin detects). `caught` feeds print. */
#include "bench.h"

struct DivByZero {};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    std::vector<long> zeros = {0, 0, 0};
    /* Force a genuine runtime read: after this clobber the compiler cannot
     * assume zeros[] is all-0, so the d==0 test is a real runtime branch. */
    bench_sink_ptr(zeros.data());
    long N = 200000L * scale;
    long caught = 0;

    for (long i = 0; i < N; i++) {
        try {
            long d = zeros[i % 3];
            if (d == 0)
                throw DivByZero{};
            long x = 100 / d;
            caught += x;                 /* unreachable (always divides by 0) */
        } catch (const DivByZero &) {
            caught++;
        }
    }

    printf("result: %ld\n", caught);
    return 0;
}
