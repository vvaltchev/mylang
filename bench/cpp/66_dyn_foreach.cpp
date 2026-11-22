/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/66_dyn_foreach.my: a foreach over a `dyn` container
 * that always holds an array<int> at runtime (range(1000)) -> a range-for over
 * a std::vector<long>. `s` is a serial modular-sum dependency carried across
 * every element of every rep (the mod through `s` makes the inner loop
 * non-invariant), so no barrier is needed. */
#include "bench.h"
#include "bench_value.h"

/* BENCH-FAIR (class D): the .my bench launders the array AND the
 * accumulator through `dyn` - every element read and every (s + e) % k
 * runs on a runtime-tagged fat value. The old twin iterated a static
 * vector<long>: a different program. This twin carries Value end to end
 * with the same per-op tag dispatch, and dispatches the container shape
 * ONCE per foreach entry (MyLang's ForeachDynInit does the same). */
int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    VArr a = std::make_shared<std::vector<Value>>();
    a->reserve(1000);
    for (long j = 0; j < 1000; j++)
        a->push_back(Value(j));
    Value dyn_a(a);                     /* the dyn-laundered container */

    Value s((long)0);                   /* dyn accumulator */
    const Value k((long)1000000007);
    long reps = 20000L * scale;

    for (long r = 0; r < reps; r++) {
        /* the per-entry runtime shape dispatch (array vs dict vs error) */
        if (auto *arr = std::get_if<VArr>(&dyn_a.v)) {
            for (const Value &e : **arr)
                s = vmod(vadd(s, e), k);
        } else {
            value_type_error();
        }
    }

    printf("result: %ld\n", vlong(s));
    return 0;
}
