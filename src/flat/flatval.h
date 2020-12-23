/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include <memory>


/*
 * This special class allows us to store a non-trivial type in a union.
 * Of course, in order to everything to work, the owner class (EvalValue)
 * containing an instance of FlatSharedVal<T> inside a union, has to manually
 * call: the copy ctor, the move ctor, the copy assign and the move assign
 * methods, specific for this type. This happens in part in EvalValue and
 * in part in the specific Type instance. See NonTrivialType<T> and SharedType<T>
 * in evaltypes.cpp.h.
 *
 * Q: Why not just using std::variant?
 * A: Because it's much slower. By just switching to std::variant, even with the
 *    maximum level of optimization, the hole interpreter gets about 50% slower
 *    when performing a simple loop. That means that std::variant per se is much
 *    more slower than +50%. That's unacceptable.
 */

template <class T>
class FlatVal final {

public:

    typedef T inner_type;

    alignas(T) char data[sizeof(T)];

    FlatVal() = default;

    template<typename... Args>
    FlatVal(const T &s, Args&&... args) {
        new ((void *)data) T(s, forward<Args>(args)...);
    }

    template<typename... Args>
    FlatVal(T &&s, Args&&... args) {
        new ((void *)data) T(move(s), forward<Args>(args)...);
    }

    T &get() {
        return *reinterpret_cast<T *>(data);
    }

    const T &get() const {
        return *reinterpret_cast<T *>(const_cast<FlatVal<T> *>(this)->data);
    }

    T *operator->() { return &get(); }

    const T *operator->() const { return &get(); }
};

template <class T>
class FlatSharedVal final {

    FlatVal<shared_ptr<T>> flat;

public:

    typedef T inner_type;

    FlatSharedVal() = default;

    FlatSharedVal(const shared_ptr<T> &s)
        : flat(s)
    { }

    FlatSharedVal(shared_ptr<T> &&s)
        : flat(move(s))
    { }

    shared_ptr<T> &get_shared_ptr() { return flat.get(); }
    const shared_ptr<T> &get_shared_ptr() const { return flat.get(); }
    T &get() { return *flat->get(); }
    const T &get() const { return *flat->get(); }
    long use_count() const { return flat->use_count(); }
    T *operator->() { return &get(); }
    const T *operator->() const { return &get(); }
};
