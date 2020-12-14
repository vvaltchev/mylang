/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "sharedval.h"
#include <string>

class SharedStr {

public:
    typedef string inner_type;

private:

    SharedVal<inner_type> shval;
    unsigned off = 0;   /* NOTE: cannot be const because we're using this in a union */
    unsigned len = 0;   /* NOTE: cannot be const because we're using this in a union */
    bool slice = false;

public:

    inner_type &get_ref() { return shval.get(); }
    const inner_type &get_ref() const { return shval.get(); }
    SharedVal<inner_type> &get_shval() { return shval; }
    const SharedVal<inner_type> &get_shval() const { return shval; }
    long use_count() const { return shval.use_count(); }

    SharedStr() = default;

    /*
     * That's not really necessary, just it helps knowing that we won't
     * copy strings, but just move them.
     */
    SharedStr(const inner_type &s) = delete;

    SharedStr(inner_type &&s);

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
