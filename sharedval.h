/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include <memory>
using namespace std;

template <class T>
struct SharedVal {

    typedef T type;

    char data[sizeof(shared_ptr<T>)] alignas(shared_ptr<T>);

    SharedVal() = default;

    SharedVal(const shared_ptr<T> &s) {
        new ((void *)data) shared_ptr<T>(s);
    }

    SharedVal(shared_ptr<T> &&s) {
        new ((void *)data) shared_ptr<T>(move(s));
    }

    shared_ptr<T> &to_shared_ptr() {
        return *reinterpret_cast<shared_ptr<T> *>(data);
    }

    T &get() {
        return *to_shared_ptr().get();
    }

    T get() const {
        /* Unfortunately, this const_cast is unavoidable */
        return *const_cast<SharedVal<T> *>(this).get();
    }

    long use_count() {
        return to_shared_ptr().use_count();
    }
};
