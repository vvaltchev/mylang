/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/29_str_slice_readonly.my: read-only string slicing
 * in a loop. In MyLang base[1:999] is an O(1) copy-on-write VIEW (SharedStr
 * off/len - no copy until a write), so the fair C++ ceiling is an O(1)
 * std::string_view, NOT a substr() that copies 998 bytes every iteration (that
 * would time copy work MyLang never does). Mirrors the O(1)-view choice in
 * 15_array_slice_readonly.cpp. Only the two ends are read. */
#include "bench.h"
#include <memory>

/* BENCH-FAIR (class E): a MyLang string slice is a managed VALUE - an
 * intrusive refcount bump on the shared immutable body + an off/len view
 * (SharedStr; strings are immutable so no slice REGISTRATION, unlike
 * arrays). And `sub[0]` yields a fresh 1-CHAR STRING (an allocation),
 * which ord() then reads - the managed-semantics cost the old
 * string_view twin skipped entirely. */
struct StrBody {
    std::string s;
    long refcnt = 1;
};
struct StrSlice {
    StrBody *b;
    size_t off, len;
    StrSlice(StrBody *body, size_t o, size_t l) : b(body), off(o), len(l)
    {
        b->refcnt++;
    }
    ~StrSlice() { b->refcnt--; }
    /* sub[i]: a fresh 1-char string, like TypeStr::subscript /
     * SharedStr(string(&view[i], 1)). */
    std::string at(size_t i) const
    {
        return std::string(1, b->s[off + i]);
    }
    long size() const { return (long)len; }
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    StrBody base;
    base.s.reserve(1000);
    for (int r = 0; r < 100; r++)
        base.s += "0123456789";         /* "0123456789" * 100 */
    long N = 200000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        StrSlice sub(&base, 1, 998);    /* base[1:999] */
        s += (long)(unsigned char)sub.at(0)[0]
           + (long)(unsigned char)sub.at(sub.size() - 1)[0];
        s = s % 1000000007;
    }
    printf("result: %ld\n", s);
    return 0;
}
