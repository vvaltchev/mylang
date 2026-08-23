/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <cstddef>
#include <new>

/*
 * A single-threaded NODE POOL allocator for std::unordered_map (H2 v2,
 * plans/archived/vm-performance-roadmap.md - the maintainer-approved alternative to
 * a flat open-addressing map, which was REJECTED: it would break the
 * element-pointer stability the runtime relies on - dict values are LValues
 * written through held LValue* - for a modest lookup win).
 *
 * The measured cost it removes: a chained unordered_map heap-allocates a
 * ~96-byte node PER INSERT (hash + the 32-byte EvalValue key + 48-byte
 * LValue + next) and frees it on erase - the dominant cost of
 * 23_dict_insert. Single-element allocations are served from per-SIZE-CLASS
 * free lists backed by chunked arenas (never returned to malloc - the
 * interpreter is single-threaded and the pool is program-lifetime, so a
 * freed node is immediately reusable by ANY dict); multi-element
 * allocations (the map's bucket-pointer arrays) pass through to operator
 * new - they are few (rehash-only) and variable-sized.
 *
 * Node-pointer STABILITY is what makes this a pure drop-in: rehash moves
 * only the bucket array, never the nodes, so every held LValue* / iterator
 * invariant is untouched.
 *
 * UNDER ASAN THE POOL IS DISABLED (pass-through to operator new): pooled
 * reuse would mask a node use-after-free from AddressSanitizer, and the
 * debug lanes' bug-finding power outranks their speed (the same philosophy
 * as the RECYCLE lane, from the other direction). Release/plain builds get
 * the pool.
 */

#if defined(__SANITIZE_ADDRESS__)
#  define ML_POOLALLOC_PASSTHROUGH 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ML_POOLALLOC_PASSTHROUGH 1
#  endif
#endif

/* The pool STATE lives in types.cpp (the global-mutable-state home); the
 * single-element FAST PATHS are inline here (#74's per-throw-allocation
 * increment): every ML_POOL_NEW_DELETE call site passes a COMPILE-TIME
 * size, so the size class folds to a constant and the hot alloc/free is
 * 4-5 instructions (freelist pop/push) instead of an out-of-line call +
 * a runtime class computation (~45 Ir round trip, measured on the
 * per-throw exception objects; the dict-node and PureCache paths gain
 * identically). The refill and the out-of-range sizes stay out-of-line
 * (pool_alloc_slow / pool_free_slow, types.cpp). */
struct PoolFreeNode { PoolFreeNode *next; };
constexpr size_t ML_POOL_CLASS_STEP = 16;
constexpr size_t ML_POOL_NCLASSES = 16;   /* classes: 16..256 bytes */
extern PoolFreeNode *g_pool_free[ML_POOL_NCLASSES];   /* types.cpp */
void *pool_alloc_slow(size_t size);            /* refill / out-of-range */
void pool_free_slow(void *p, size_t size) noexcept;   /* out-of-range */

inline void *pool_alloc_one(size_t size)
{
#ifdef ML_POOLALLOC_PASSTHROUGH
    return ::operator new(size);     /* ASan: keep full UAF coverage */
#else
    const size_t cls = (size + ML_POOL_CLASS_STEP - 1) / ML_POOL_CLASS_STEP;
    if (cls == 0 || cls > ML_POOL_NCLASSES)
        return pool_alloc_slow(size);     /* out-of-range: plain heap */
    PoolFreeNode *n = g_pool_free[cls - 1];
    if (!n)
        return pool_alloc_slow(size);     /* refill (cold) */
    g_pool_free[cls - 1] = n->next;
    return n;
#endif
}

inline void pool_free_one(void *p, size_t size) noexcept
{
#ifdef ML_POOLALLOC_PASSTHROUGH
    ::operator delete(p);
#else
    const size_t cls = (size + ML_POOL_CLASS_STEP - 1) / ML_POOL_CLASS_STEP;
    if (cls == 0 || cls > ML_POOL_NCLASSES) {
        pool_free_slow(p, size);
        return;
    }
    auto *n = static_cast<PoolFreeNode *>(p);
    n->next = g_pool_free[cls - 1];
    g_pool_free[cls - 1] = n;
#endif
}

/*
 * Class-scope pooled allocation (top-10 #6): the runtime's small, fixed-size
 * intrusive HEAP OBJECTS (StrObj, SharedObject, DictObject, StructObject,
 * FuncObject, ExceptionObject) churn per value - a StrObj per str(i), a
 * StructObject per standalone construction, a FuncObject per closure. Sized
 * class operator new/delete route them through the same program-lifetime
 * free lists as the map nodes (an oversized class falls back to plain new
 * inside pool_alloc_one automatically; the ASan pass-through gate lives in
 * the pool core, so sanitizer lanes keep full UAF coverage). The explicit
 * placement forms are re-declared because a class operator new hides them.
 */
#define ML_POOL_NEW_DELETE                                                      static void *operator new(size_t sz) { return pool_alloc_one(sz); }         static void operator delete(void *p, size_t sz) noexcept {                      pool_free_one(p, sz);                                                   }                                                                           static void *operator new(size_t, void *p) noexcept { return p; }           static void operator delete(void *, void *) noexcept {}

template <typename T>
struct PoolAlloc {

    using value_type = T;

    PoolAlloc() = default;

    template <typename U>
    PoolAlloc(const PoolAlloc<U> &) noexcept {}

    T *allocate(size_t n)
    {
#ifndef ML_POOLALLOC_PASSTHROUGH
        if (n == 1)
            return static_cast<T *>(pool_alloc_one(sizeof(T)));
#endif
        return static_cast<T *>(::operator new(n * sizeof(T)));
    }

    void deallocate(T *p, size_t n) noexcept
    {
#ifndef ML_POOLALLOC_PASSTHROUGH
        if (n == 1) {
            pool_free_one(p, sizeof(T));
            return;
        }
#endif
        ::operator delete(p);
    }

    template <typename U>
    bool operator==(const PoolAlloc<U> &) const noexcept { return true; }

    template <typename U>
    bool operator!=(const PoolAlloc<U> &) const noexcept { return false; }
};
