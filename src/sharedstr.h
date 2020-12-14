/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "sharedval.h"
#include <string>

class SharedStr {

public:

    friend class TypeStr;
    typedef string inner_type;

private:

    /*
     * This method is `const` despite accesses the non-const
     * SharedVal<T>::get() method, simply because that method always
     * return a T&, for performance reasons. SharedVal<T> has no way
     * to know about string_view because it's generic and it cannot offer
     * a T get() const method, because that will require a string copy.
     *
     * Therefore, we always call a non-const method, even inside `const`
     * functions here but just allow only const operations like getting
     * a string_view, inside `const` methods. This way of breaking strict
     * constness is NOT a hack. It's more like the `@trusted` attribute
     * in Dlang which allows the programmer to "bless" a given unsafe
     * function and treat it as safe.
     */
    inner_type &get_ref() const {
        return const_cast<SharedStr *>(this)->shval.get();
    }

    long use_count() const {
        return const_cast<SharedStr *>(this)->shval.use_count();
    }

    SharedVal<inner_type> shval;
    unsigned off = 0;   /* NOTE: cannot be const because we're using this in a union */
    unsigned len = 0;   /* NOTE: cannot be const because we're using this in a union */
    bool slice = false;

public:

    SharedStr() = default;

    /*
     * That's not really necessary, just it helps knowing that we won't
     * copy strings, but just move them.
     */
    SharedStr(const inner_type &s) = delete;

    SharedStr(inner_type &&s);

    string_view get_view() const {
        return string_view(get_ref().data() + offset(), size());
    }

    SharedVal<inner_type> &get_shval() { return shval; }
    const SharedVal<inner_type> &get_shval() const { return shval; }

    void set_slice(unsigned off_val, unsigned len_val) {
        off = off_val;
        len = len_val;
        slice = true;
    }

    bool is_slice() const { return slice; }

    unsigned offset() const {
        return slice ? off : 0;
    }

    unsigned size() const {
        return slice ? const_cast<SharedStr *>(this)->len : get_ref().size();
    }
};
