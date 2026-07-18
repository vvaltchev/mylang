/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/29_str_slice_readonly.my: read-only string slicing
 * in a loop. In MyLang base[1:999] is an O(1) copy-on-write VIEW (SharedStr
 * off/len - no copy until a write), so the fair C++ ceiling is an O(1)
 * std::string_view, NOT a substr() that copies 998 bytes every iteration (that
 * would time copy work MyLang never does). Mirrors the O(1)-view choice in
 * 15_array_slice_readonly.cpp. Only the two ends are read. */
#include "bench.h"
#include <string>
#include <string_view>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::string base;
    for (int i = 0; i < 100; i++)
        base += "0123456789";           /* 1000-char string */

    long N = 200000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        std::string_view sub(base.data() + 1, 998);   /* base[1:999], O(1) */
        s += (long)(unsigned char)sub[0]
           + (long)(unsigned char)sub[sub.size() - 1];
        s = s % 1000000007L;
    }

    printf("result: %ld\n", s);
    return 0;
}
