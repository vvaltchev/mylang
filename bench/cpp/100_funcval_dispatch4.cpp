/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/100_funcval_dispatch4.my: a function picked from
 * an array<func> of FOUR by index and called for its side effect. A C++
 * array of function pointers models the func-value array; the target
 * rotates by i%4, so the call is a genuine indirect dispatch.
 * bench_sink(st[0]) per iteration (rule a) keeps the compiler from
 * reasoning about the sequence as a whole. */
#include "bench.h"

static void add_op(long *st, long x) { st[0] = st[0] + x; }
static void sub_op(long *st, long x) { st[0] = st[0] - x; }
static void xor_op(long *st, long x) { st[0] = st[0] ^ x; }
static void mix_op(long *st, long x) { st[0] = (st[0] * 3 + x) % 1000000007L; }

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    void (*ops[4])(long *, long) = {add_op, sub_op, xor_op, mix_op};
    long st[1] = {0};

    for (long i = 0; i < N; i++) {
        void (*fn)(long *, long) = ops[i % 4];
        fn(st, i);            /* indirect call statement, result discarded */
        bench_sink(st[0]);
    }

    printf("result: %ld\n", st[0]);
    return 0;
}
