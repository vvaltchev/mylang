/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/98_local_closure_call.my: two closures built in
 * main - a counter over a mutable capture and an adder over a read-only one
 * - called once each per iteration. std::function for the reason 78 gives:
 * the MyLang variables are runtime state that may hold any callable, so each
 * call is an indirect dispatch; bench_sink_ptr keeps the targets opaque and
 * bench_sink stops -O3 from closing the sum into a formula. */
#include "bench.h"
#include <functional>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    long start = 0;
    std::function<long()> tick = [start]() mutable { return ++start; };
    long base = 7;
    std::function<long(long)> add = [base](long k) { return base + k; };
    bench_sink_ptr(&tick);
    bench_sink_ptr(&add);

    long s = 0;
    for (long i = 0; i < N; i++) {
        s = s + tick() + add(i);
        bench_sink(s);
    }

    printf("result: %ld\n", s);
    return 0;
}
