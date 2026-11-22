/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/78_typed_param_call.my: two closures - one built
 * over an int, one over a double - called once each per iteration, with the
 * second receiving an int argument that widens to its double parameter.
 *
 * std::function is the flexibility-matched translation of
 * `var add = make_adder(7)`: in MyLang the variable is RUNTIME state that
 * may hold ANY callable, so every call is an indirect dispatch. A capturing
 * lambda in an `auto` variable is NOT that - its type pins the one target,
 * -O3 inlines both bodies, and the loop compiles to 8 instructions with
 * ZERO calls (measured: ~8.5ms at scale 16 vs ~35.4ms for this version) -
 * a ceiling on "the same arithmetic with the call protocol gone", not a
 * fair race for a call-protocol bench. bench_sink_ptr on the two callables
 * keeps their targets opaque (runtime state, rule a); bench_sink on each
 * accumulator stops -O3 from close-forming the sums to O(1).
 */
#include "bench.h"
#include <functional>

static std::function<long(long)> make_adder(long base)
{
    return [base](long k) { return base + k; };
}

static std::function<double(double)> make_scaler(double f)
{
    return [f](double x) { return f * x; };
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    auto add = make_adder(7);
    auto scale_it = make_scaler(0.5);
    bench_sink_ptr(&add);          /* the targets are runtime state */
    bench_sink_ptr(&scale_it);

    long s = 0;
    double t = 0.0;

    for (long i = 0; i < N; i++) {
        s = s + add(i);           /* exact:    int argument, int parameter */
        t = t + scale_it(i);      /* widening: int argument, double param  */
        bench_sink(s);
        bench_sink(t);
    }

    printf("result: %ld %f\n", s, t);
    return 0;
}
