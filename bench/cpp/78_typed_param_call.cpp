/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/78_typed_param_call.my: two closures - one built
 * over an int, one over a double - called once each per iteration, with the
 * second receiving an int argument that widens to its double parameter.
 *
 * A capturing lambda held in an `auto` variable is the faithful translation
 * of `var add = make_adder(7)`: a value that carries its captured state and
 * is invoked through the variable. C++ knows the target statically and will
 * inline both bodies - and that is exactly the CEILING this column exists to
 * report (see bench.h): the same work with the call protocol gone. bench_sink
 * on each accumulator is what stops -O3 from close-forming the two sums to
 * O(1) once the calls are inlined (rule a).
 */
#include "bench.h"

static auto make_adder(long base)
{
    return [base](long k) { return base + k; };
}

static auto make_scaler(double f)
{
    return [f](double x) { return f * x; };
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    auto add = make_adder(7);
    auto scale_it = make_scaler(0.5);

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
