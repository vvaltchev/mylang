/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <cstdint>
#include <utility>
#include <cstddef>

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

template <class T>
class intrusive_ptr final {

    T *ptr;

    void retain() { if (ptr) ++ptr->intr_refcount; }
    void release() { if (ptr && --ptr->intr_refcount == 0) delete ptr; }

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
