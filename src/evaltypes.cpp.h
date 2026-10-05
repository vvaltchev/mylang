/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include <cstring>

using std::string;
using std::string_view;

template <class T>
class TypeImpl : public Type {

public:

    TypeImpl(Type::TypeE e) : Type(e) { }

    void default_ctor(void *obj) override {
        new (obj) T;
    }

    void dtor(void *obj) override {
        reinterpret_cast<T *>(obj)->~T();
    }

    void copy_ctor(void *obj, const void *other) override {
        new (obj) T(*reinterpret_cast<const T *>(other));
    }

    void move_ctor(void *obj, void *other) override {
        new (obj) T(std::move(*reinterpret_cast<T *>(other)));
    }

    void copy_assign(void *obj, const void *other) override {
        *reinterpret_cast<T *>(obj) = *reinterpret_cast<const T *>(other);
    }

    void move_assign(void *obj, void *other) override {
        *reinterpret_cast<T *>(obj) = std::move(*reinterpret_cast<T *>(other));
    }
};

class TypeNone : public Type {

public:

    TypeNone() : Type(Type::t_none) { }

    bool is_true(const EvalValue &a) override {
        return false;
    }

    string to_string(const EvalValue &a) override {
        return "<none>";
    }

    void eq(EvalValue &a, const EvalValue &b) override {
        a = b.is<NoneVal>();
    }

    void noteq(EvalValue &a, const EvalValue &b) override {
        a = !b.is<NoneVal>();
    }

    size_t hash(const EvalValue &a) override {
        return hash_salt_none;   /* `none` is hashable: a fixed value */
    }
};

string_view
find_builtin_name(const Builtin &b)
{
    for (const auto &[k, v]: EvalContext::const_builtins) {
        if (v.is<Builtin>() && v.getval<Builtin>().func == b.func)
            return k->val;
    }

    for (const auto &[k, v]: EvalContext::builtins) {
        if (v.is<Builtin>() && v.getval<Builtin>().func == b.func)
            return k->val;
    }

    throw InternalErrorEx();
}

class TypeBuiltin : public Type {

public:
    TypeBuiltin() : Type(Type::t_builtin) { }
    string to_string(const EvalValue &a) override {
        return "<Builtin(" + string(find_builtin_name(a.get<Builtin>())) + ")>";
    }
};

/*
 * A container a builtin builds to a SIZE the program gives (range, array,
 * make_array, lpad / rpad) is reserved whole before it is filled: a size no
 * container can hold, or a request the allocator refuses, is OutOfMemoryEx
 * (README) with the size argument's caret - and nothing was half-built.
 */
#if defined(__SANITIZE_ADDRESS__)
#  define ML_RESERVE_PROBE 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ML_RESERVE_PROBE 1
#  endif
#endif

template <class Container>
static void reserve_or_oom(Container &c, uint64_t n, const ArgLoc *at)
{
    if (n > static_cast<uint64_t>(c.max_size()))
        throw OutOfMemoryEx("cannot allocate a result that large",
                            at->start, at->end);
#ifdef ML_RESERVE_PROBE
    /* ASan's THROWING operator new aborts on a refusal - it cannot call
     * the new_handler - whatever its options say; the NOTHROW form returns
     * null (allocator_may_return_null, mylang.cpp). So a sanitized build
     * asks that form first and gives the block back: the same outcome a
     * plain build reaches through the new_handler. Past ASan's largest
     * allocation (1 TiB on 64-bit) it is not even asked: ASan prints a
     * warning before refusing such a request, with its pid - stderr two
     * engines' runs could never agree on. */
    const uint64_t bytes = n * sizeof(typename Container::value_type);
    void *probe = bytes > (uint64_t(1) << 40)
        ? nullptr
        : ::operator new(static_cast<size_t>(bytes), std::nothrow);
    if (!probe)
        throw OutOfMemoryEx("cannot allocate a result that large",
                            at->start, at->end);
    ::operator delete(probe);
#endif
    try {
        c.reserve(static_cast<size_t>(n));
    } catch (const std::bad_alloc &) {  /* INT-COV-EXEMPT: no ASan only */
        /* an ASan build (the coverage build is one) has refused already,
         * in ML_RESERVE_PROBE; a plain build arrives here (66_out_of_memory
         * on the release lanes) */
        throw OutOfMemoryEx("cannot allocate a result that large",
                            at->start, at->end);
    }
}
