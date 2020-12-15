/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include <memory>
using namespace std;

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
class FlatVal {

public:

    typedef T inner_type;

    alignas(T) char data[sizeof(T)];

    FlatVal() = default;

    FlatVal(const T &s) {
        new ((void *)data) T(s);
    }

    FlatVal(T &&s) {
        new ((void *)data) T(move(s));
    }

    T &get() {
        return *reinterpret_cast<T *>(data);
    }

    const T &get() const {
        return *reinterpret_cast<T *>(const_cast<FlatVal<T> *>(this)->data);
    }
};

template <class T>
class FlatSharedVal {

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

    T &get() { return *flat.get().get(); }
    const T &get() const { return *flat.get().get(); }
    long use_count() const { return flat.get().use_count(); }
};
