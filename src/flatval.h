/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "defs.h"

/*
 * This special class allows us to store a non-trivial type in a union.
 * Of course, in order to everything to work, the owner class (EvalValue)
 * containing an instance of FlatSharedVal<T> inside a union, has to manually
 * call: the copy ctor, the move ctor, the copy assign and the move assign
 * methods, specific for this type. This happens in part in EvalValue and
 * in part in the specific Type instance. See TypeImpl<T> in evaltypes.cpp.h.
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

    template<typename... Args>
    FlatVal(Args&&... args) {
        new ((void *)data) T(forward<Args>(args)...);
    }
};
