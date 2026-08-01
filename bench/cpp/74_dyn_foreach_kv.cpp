/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/74_dyn_foreach_kv.my: a 2-var foreach (k, v) over a
 * `dyn` container that always holds a dict at runtime -> a range-for over a
 * std::unordered_map<long,long> (the hashmap MyLang's dict wraps). `total`
 * sums k+v over every entry each rep with no per-element dependency, so the
 * inner map-iteration is loop-invariant; a per-rep memory barrier stops the
 * compiler hoisting/closed-forming it, forcing the map to be iterated each rep
 * (the honest ceiling for "iterate this map N times"). The k+v sum is
 * order-independent, so the unordered iteration order is irrelevant. */
#include "bench.h"
#include "bench_value.h"

/* BENCH-FAIR (class D): the .my dict is laundered through `dyn` (a
 * runtime array-vs-dict dispatch per foreach entry) and the accumulator
 * is dyn - every k + v runs on runtime-tagged fat values, and the dict
 * itself is Value-keyed (MyLang dict keys are EvalValues hashed by
 * runtime tag). The old twin iterated a static unordered_map<long,long>:
 * a different program. */

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 200000L * scale;

    VDict d = std::make_shared<VDictObj>();
    for (long i = 0; i < 100; i++)
        d->m[Value(i)] = Value(i * 2);
    Value dd(d);                        /* the dyn-laundered container */

    Value total((long)0);               /* dyn accumulator */

    for (long r = 0; r < N; r++) {
        bench_sink_ptr(d.get());        /* keep the re-iteration honest */
        /* the per-entry runtime shape dispatch (array vs dict) */
        if (auto *dict = std::get_if<VDict>(&dd.v)) {
            for (const auto &kv : (*dict)->m)
                total = vadd(total, vadd(kv.first, kv.second));
        } else {
            value_type_error();
        }
    }

    printf("result: %ld\n", vlong(total) % 1000000007);
    return 0;
}
