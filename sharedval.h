/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include <memory>
using namespace std;

/*
 * This special class allows us to store a non-trivial type in a union.
 * Of course, in order to everything to work, the owner class (EvalValue)
 * containing an instance of SharedVal<T> inside a union, has to manually
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
class SharedVal {

    shared_ptr<T> &to_shared_ptr() {
        return *reinterpret_cast<shared_ptr<T> *>(data);
    }

public:

    typedef T inner_type;

    alignas(shared_ptr<T>) char data[sizeof(shared_ptr<T>)];

    SharedVal() = default;

    SharedVal(const shared_ptr<T> &s) {
        new ((void *)data) shared_ptr<T>(s);
    }

    SharedVal(shared_ptr<T> &&s) {
        new ((void *)data) shared_ptr<T>(move(s));
    }

    T &get() {
        return *to_shared_ptr().get();
    }

    long use_count() {
        return to_shared_ptr().use_count();
    }
};
