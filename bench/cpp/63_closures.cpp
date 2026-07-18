/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/63_closures.my: the whole closure lifecycle in a
 * loop - a factory capturing MUTABLE state (make_counter: count++ inside), a
 * factory capturing IMMUTABLE state (make_adder: base read), created fresh each
 * iteration, called, and their captures mutated/read. C++ mutable lambdas model
 * both closures directly. The accumulator has a `%` serial dependency each
 * iteration, so it can't be close-formed - no in-loop barrier needed. */
#include "bench.h"

/* a factory whose returned closure captures MUTABLE state (count++). */
static auto make_counter(long start)
{
    return [count = start]() mutable { count++; return count; };
}

/* a factory whose returned closure captures IMMUTABLE state (base is read). */
static auto make_adder(long n)
{
    long base = n * 10;
    return [base](long x) { return base + x; };
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 200000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        auto c = make_counter(i);   /* create a counter closure */
        long a1 = c();              /* i+1 */
        long a2 = c();              /* i+2 (two capture-mutating calls) */
        s += a1 + a2;
        auto add = make_adder(i);   /* create an adder closure */
        s += add(i);                /* a capture-reading call (11*i) */
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
