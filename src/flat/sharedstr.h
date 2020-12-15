/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "flatval.h"
#include <string>

class FlatSharedStr {

public:
    typedef string inner_type;

private:

    FlatSharedVal<inner_type> shval;
    unsigned off = 0;   /* NOTE: cannot be const because we're using this in a union */
    unsigned len = 0;   /* NOTE: cannot be const because we're using this in a union */
    bool slice = false;

public:

    inner_type &get_ref() { return shval.get(); }
    const inner_type &get_ref() const { return shval.get(); }
    FlatSharedVal<inner_type> &get_shval() { return shval; }
    const FlatSharedVal<inner_type> &get_shval() const { return shval; }
    long use_count() const { return shval.use_count(); }

    FlatSharedStr() = default;

    /*
     * That's not really necessary, just it helps knowing that we won't
     * copy strings, but just move them.
     */
    FlatSharedStr(const inner_type &s) = delete;

    FlatSharedStr(inner_type &&s);

    string_view get_view() const {
        return string_view(shval.get().data() + offset(), size());
    }

    void set_slice(unsigned off_val, unsigned len_val) {
        off = off_val;
        len = len_val;
        slice = true;
    }

    bool is_slice() const { return slice; }
    unsigned offset() const { return slice ? off : 0; }
    unsigned size() const { return slice ? len : get_ref().size(); }
};
