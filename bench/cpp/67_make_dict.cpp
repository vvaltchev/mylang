/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/67_make_dict.my: build a fresh 400-entry dict per
 * rep from an array of keys via a value callback (the dict-comprehension form
 * { k: k*k + r for k in ks }). Values depend on the loop counter r. */
/* BENCH-FAIR (class B, 2026-08-14). The value expression used to be
 * written inline in the fill loop. MyLang's `make_dict(ks, func[r](k) =>
 * k*k+r)` passes a CAPTURING closure and the builtin invokes it per key,
 * so the honest counterpart is a std::function that captures `r` the same
 * way, called through a NOINLINE make_dict. Asm-verified: one indirect
 * call per key. */
#include "bench.h"
#include <unordered_map>
#include <functional>

__attribute__((noinline))
static std::unordered_map<long, long>
make_dict_with(const std::vector<long> &keys,
               const std::function<long(long)> &f)
{
    std::unordered_map<long, long> d;
    for (size_t i = 0; i < keys.size(); i++)
        d[keys[i]] = f(keys[i]);
    return d;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long nkeys = 400;
    long reps = 3000L * scale;

    std::vector<long> ks(nkeys);
    for (long k = 0; k < nkeys; k++)
        ks[k] = k;                   /* range(nkeys) */

    long total = 0;
    for (long r = 0; r < reps; r++) {
        /* the capture is fresh per rep, exactly as MyLang's func[r] is */
        std::function<long(long)> gen =
            [r](long k) { return k * k + r; };
        std::unordered_map<long, long> d = make_dict_with(ks, gen);
        total += d[r % nkeys];
    }

    printf("result: %ld\n", total);
    return 0;
}
