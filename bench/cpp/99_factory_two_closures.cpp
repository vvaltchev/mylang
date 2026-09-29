/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/99_factory_two_closures.my: a factory that
 * returns one of two closures by its argument, called in a loop. The
 * factory returns a std::function - the MyLang closure is a runtime value
 * that may be either lambda - and is kept out of line (noinline) so -O3
 * cannot fold the choice into the loop; bench_sink stops the sum from
 * being computed in closed form. */
#include "bench.h"
#include <functional>

__attribute__((noinline))
static std::function<long(long)> make_op(long k)
{
    if (k % 3 == 0) {
        long base = k;
        return [base](long x) { return base + x; };
    }
    long factor = k % 7 + 1;
    return [factor](long x) { return factor * x; };
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 400000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        std::function<long(long)> f = make_op(i);
        s = (s + f(i)) % 1000000007L;
        bench_sink(s);
    }
    printf("result: %ld\n", s);
    return 0;
}
