/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/76_funcval_dispatch.my: a function picked from an
 * array<func> by index and called for its side effect (the result discarded).
 * A C++ array of function pointers models the func-value array; the target
 * alternates by i%2, so the call is a genuine indirect dispatch. st[0]
 * accumulates +i (even) / -i (odd); bench_sink(st[0]) per iteration (rule a)
 * blocks the compiler from close-forming the alternating sum to O(1). */
#include "bench.h"

static void add_op(long *st, long x)
{
    st[0] = st[0] + x;
}

static void sub_op(long *st, long x)
{
    st[0] = st[0] - x;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    void (*ops[2])(long *, long) = {add_op, sub_op};
    long st[1] = {0};

    for (long i = 0; i < N; i++) {
        void (*fn)(long *, long) = ops[i % 2];
        fn(st, i);            /* indirect call statement, result discarded */
        bench_sink(st[0]);
    }

    printf("result: %ld\n", st[0]);
    return 0;
}
