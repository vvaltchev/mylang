/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"       /* ML_CHECK */
#include <cstdint>
#include <utility>
#include <cstddef>
#include <vector>

/*
 * A minimal intrusive, NON-atomic reference-counted smart pointer.
 *
 * The interpreter is single-threaded, so we don't need std::shared_ptr's two
 * properties that make it expensive here:
 *   - its two-word layout (the object pointer PLUS a separate control-block
 *     pointer) - an intrusive_ptr is a single pointer, since the count lives in
 *     the pointee (which inherits RefCounted); and
 *   - its ATOMIC refcount ops - retain/release here are plain ++ / --.
 *
 * Used as the storage handle inside SharedArrayObj / SharedStr, shrinking each
 * from 32 to 24 bytes (which is what shrinks EvalValue/LValue) and removing the
 * atomic-refcount churn that showed up in the copy-heavy benchmarks.
 *
 * The interface mirrors the subset of shared_ptr these classes use (get,
 * operator->, operator*, bool, use_count, reset, ==), so it is a drop-in there.
 */

/*
 * Base for intrusively-counted objects. The count is part of the pointer
 * topology, NOT the object's value, so a *copied* or *moved* object (e.g. a
 * clone()) must start with its own fresh count of 0 - otherwise the clone would
 * inherit the original's count and never be freed. Assignment likewise leaves
 * the destination's count untouched.
 */
struct RefCounted {
    uint32_t intr_refcount = 0;

    RefCounted() = default;
    RefCounted(const RefCounted &) : intr_refcount(0) { }
    RefCounted(RefCounted &&) : intr_refcount(0) { }
    RefCounted &operator=(const RefCounted &) { return *this; }
    RefCounted &operator=(RefCounted &&) { return *this; }
};

/*
 * BOUNDED DESTRUCTION (2026-10-12). Freeing an object releases what it
 * holds, so a CHAIN of values - a linked list of class nodes, arrays or
 * dicts nested 100k deep - is freed by recursion, several C++ frames per
 * link: a 20000-node list overflowed the stack of a debug build and a
 * 1M-deep one a release build, wherever its head was dropped (program
 * exit included). Every last release comes here: up to DESTROY_MAX_DEPTH
 * deletions run nested as before, a deeper one is QUEUED, and the
 * outermost deletion empties the queue before it returns. The recursion
 * is bounded, everything is still freed before the release that started
 * it returns, and only the ORDER of destruction changes - no destructor
 * here runs program code or has an observable effect.
 */
struct DeferredDestroy {
    void *obj;
    void (*del)(void *) noexcept;
};

constexpr int DESTROY_MAX_DEPTH = 32;
inline int g_destroy_depth = 0;

/* never destroyed: a release during static destruction may still queue */
inline std::vector<DeferredDestroy> &destroy_queue()
{
    /* its branches are the static's initialization guard (INT-COV-EXEMPT
     * on the line): the C++ runtime's, taken once per process */
    static auto *q = new std::vector<DeferredDestroy>(); /* INT-COV-EXEMPT */
    return *q;
}

ML_NOINLINE inline void destroy_drain() noexcept
{
    std::vector<DeferredDestroy> &q = destroy_queue();
    while (!q.empty()) {
        const DeferredDestroy d = q.back();
        q.pop_back();
        ++g_destroy_depth;
        d.del(d.obj);
        --g_destroy_depth;
    }
}

/* the one typed step: run or queued, by destroy_any */
template <class T>
void destroy_delete(void *o) noexcept
{
    delete static_cast<T *>(o);    /* INT-COV-EXEMPT: the null test */
}

/*
 * The depth rule lives in ONE function, not in the template: a branch in
 * destroy_counted<T> is a separate coverage element per counted type, and
 * the queue arm of most of them is reached by no program (a chain of
 * strings does not exist). Inlined into destroy_counted<T>, `del` is a
 * constant, so an optimized build calls the typed delete directly.
 */
inline void destroy_any(void *p, void (*del)(void *) noexcept) noexcept
{
    if (g_destroy_depth >= DESTROY_MAX_DEPTH) {
        destroy_queue().push_back({p, del});
        return;
    }
    ++g_destroy_depth;
    del(p);
    --g_destroy_depth;
    if (g_destroy_depth == 0 && !destroy_queue().empty())
        destroy_drain();
}

/* delete a counted object whose count reached 0 (see above) */
template <class T>
void destroy_counted(T *p) noexcept
{
    destroy_any(p, &destroy_delete<T>);
}

template <class T>
class intrusive_ptr final {

    T *ptr;

    void retain() { if (ptr) ++ptr->intr_refcount; }

    /*
     * #109: the COLD half of release(), kept OUT OF LINE on purpose.
     *
     * `delete ptr` drags in T's whole destructor - for SharedStr::StrObj that
     * is ~std::string -> free - which makes release() big enough that GCC
     * out-lines the WHOLE function. Every reference drop in the interpreter
     * then pays a call frame to perform a three-instruction decrement: 90M Ir
     * on 75_indexed_unpack alone (callgrind, DEBUG_INFO=1, OPT=1 ASSERTS=0),
     * ~9 Ir per release where the inline form is 3-4.
     *
     * Splitting it lets the hot half inline at the call site and leaves the
     * destroy - which is a real free, and rare - behind one call. The
     * ML_CHECK stays in the INLINE half: it is the over-release tripwire and
     * must fire at the exact point the balance breaks, not one frame away.
     */
    ML_NOINLINE void destroy() { destroy_counted(ptr); }

    void release()
    {
        if (ptr) {
            /* Over-release (count already 0) would underflow this unsigned
             * counter to a huge value and silently leak / cause a UAF later;
             * catch it at the exact point the balance breaks. */
            ML_CHECK(ptr->intr_refcount > 0);
            if (--ptr->intr_refcount == 0)
                destroy();
        }
    }

public:

    intrusive_ptr() : ptr(nullptr) { }
    intrusive_ptr(std::nullptr_t) : ptr(nullptr) { }

    /* Adopt a freshly-new'd object (refcount 0 -> 1). */
    explicit intrusive_ptr(T *p) : ptr(p) { retain(); }

    intrusive_ptr(const intrusive_ptr &o) : ptr(o.ptr) { retain(); }

    intrusive_ptr(intrusive_ptr &&o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }

    intrusive_ptr &operator=(const intrusive_ptr &o)
    {
        if (ptr != o.ptr) {
            release();
            ptr = o.ptr;
            retain();
        }
        return *this;
    }

    intrusive_ptr &operator=(intrusive_ptr &&o) noexcept
    {
        if (this != &o) {
            release();
            ptr = o.ptr;
            o.ptr = nullptr;
        }
        return *this;
    }

    ~intrusive_ptr() { release(); }

    T *get() const { return ptr; }
    T *operator->() const { return ptr; }
    T &operator*() const { return *ptr; }
    explicit operator bool() const { return ptr != nullptr; }

    /* Matches shared_ptr::use_count()'s meaning: number of handles sharing the
     * pointee (the COW paths test `use_count() > 1`). */
    long use_count() const { return ptr ? ptr->intr_refcount : 0; }

    void reset() { release(); ptr = nullptr; }

    bool operator==(const intrusive_ptr &o) const { return ptr == o.ptr; }
    bool operator!=(const intrusive_ptr &o) const { return ptr != o.ptr; }
};

template <class T, class... Args>
intrusive_ptr<T> make_intrusive(Args &&... args)
{
    return intrusive_ptr<T>(new T(std::forward<Args>(args)...));
}
