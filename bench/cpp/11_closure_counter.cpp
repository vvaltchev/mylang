/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/11_closure_counter.my: a factory mk(start) returns a
 * closure capturing MUTABLE state; the closure is called N times, each call
 * increments its captured counter and returns it. A C++ mutable lambda IS a
 * closure (a struct with operator() + a captured member), so this models the
 * MyLang closure directly. The captured counter walks 1,2,3,... - a plain sum
 * the compiler would close-form to N*(N+1)/2, so bench_sink(s) per iteration
 * (rule a) keeps the per-call work real and the loop O(N). */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    /* mk(0): a closure over a mutable captured `start` (initially 0). */
    auto c = [start = 0L]() mutable { start++; return start; };

    long N = 1000000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        s += c();
        bench_sink(s);
    }

    printf("result: %ld\n", s);
    return 0;
}
