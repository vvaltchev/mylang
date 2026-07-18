/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/42_exceptions.my: throw/catch user exceptions in a
 * loop (exceptions as control flow). MyLang `throw Even(i)` / `catch (Even)`
 * -> C++ `throw`/`try/catch` with small exception types dispatched by type.
 * The throw/catch is the work; `caught` (the result) feeds print, and the
 * throw is opaque control flow the compiler cannot closed-form away. */
#include "bench.h"

struct Even { long i; };
struct Odd  { long i; };

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 200000L * scale;
    long caught = 0;

    for (long i = 0; i < N; i++) {
        try {
            if (i % 2 == 0)
                throw Even{i};
            else
                throw Odd{i};
        } catch (const Even &) {
            caught++;
        } catch (const Odd &) {
            caught += 2;
        }
    }

    printf("result: %ld\n", caught);
    return 0;
}
