/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/15_array_slice_readonly.my: a read-only slice in a
 * loop. In MyLang base[1:999] is an O(1) copy-on-write VIEW (no copy until a
 * write), so the faithful C++ ceiling is an O(1) sub-view (pointer + length),
 * not Python's eager 998-element copy. Only the two ends are read. */
#include "bench.h"
#include <set>

/* BENCH-FAIR (plans/bench-fairness.md, class E): a MyLang slice is NOT a
 * raw pointer pair - it is a managed VALUE with CoW semantics: an
 * intrusive refcount bump on the shared body plus REGISTRATION of the
 * live slice in the parent's slices set (a node-allocating std::set
 * insert; erased + refcount dropped on scope exit) - that registration
 * is what lets a later write to the parent COW-detach correctly. C++ has
 * no CoW out of the box, so this twin mirrors SharedArrayObj's exact
 * slice mechanics (embedding the whole runtime was rejected: the types
 * TU drags the full interpreter). The old raw-pointer twin benched a
 * borrowed view - a different, weaker semantic. */
struct CowBody {
    std::vector<long> v;
    std::set<struct CowSlice *> slices;
    long refcnt = 1;
};
struct CowSlice {
    CowBody *b;
    size_t off, len;
    CowSlice(CowBody *body, size_t o, size_t l) : b(body), off(o), len(l)
    {
        b->refcnt++;
        b->slices.insert(this);
    }
    ~CowSlice()
    {
        b->slices.erase(this);
        b->refcnt--;
    }
    long operator[](size_t i) const { return b->v[off + i]; }
    long size() const { return (long)len; }
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    CowBody base;
    base.v.resize(1000);                /* range(1000) -> [0..999] */
    for (long i = 0; i < 1000; i++)
        base.v[i] = i;
    long N = 200000L * scale;
    long s = 0;
    for (long i = 0; i < N; i++) {
        CowSlice sl(&base, 1, 998);     /* base[1:999]: a REGISTERED view */
        s += sl[0] + sl[sl.size() - 1];
        s = s % 1000000007;
    }
    printf("result: %ld\n", s);
    return 0;
}
