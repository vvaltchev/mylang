/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/41_str_int_conv.my: str()/int() round-trip
 * conversions in a loop. str(i) -> std::to_string, int(s) -> std::stol.
 * back - i is always 0; bench_sink_ptr on the string keeps the round trip
 * (to_string + stol) from being optimized away. */
#include "bench.h"
#include <string>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    long N = 300000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        std::string str_i = std::to_string(i);
        long back = std::stol(str_i);
        bench_sink_ptr(str_i.data());
        s += back - i;                  /* always 0 */
    }

    printf("result: %ld\n", s);
    return 0;
}
