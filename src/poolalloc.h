/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <cstddef>
#include <new>

/*
 * A single-threaded NODE POOL allocator for std::unordered_map (H2 v2,
 * plans/vm-performance-roadmap.md - the maintainer-approved alternative to
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

/* The pool core (defined in types.cpp - the global-mutable-state home).
 * Size classes: 16-byte steps up to ML_POOL_MAX; larger sizes fall back to
 * operator new inside pool_alloc_one itself, so callers never branch. */
void *pool_alloc_one(size_t size);
void pool_free_one(void *p, size_t size) noexcept;

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
