/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "flatval.h"
#include <string>

class FlatSharedStr {

public:
    typedef string inner_type;

private:

    FlatSharedVal<inner_type> flat;
    unsigned off = 0;   /* NOTE: cannot be const because we're using this in a union */
    unsigned len = 0;   /* NOTE: cannot be const because we're using this in a union */
    bool slice = false;

public:

    inner_type &get_ref() { return flat.get(); }
    const inner_type &get_ref() const { return flat.get(); }
    FlatSharedVal<inner_type> &get_shval() { return flat; }
    const FlatSharedVal<inner_type> &get_shval() const { return flat; }
    long use_count() const { return flat.use_count(); }

    FlatSharedStr() = default;

    /*
     * That's not really necessary, just it helps knowing that we won't
     * copy strings, but just move them.
     */
    FlatSharedStr(const inner_type &s) = delete;

    FlatSharedStr(inner_type &&s)
        : flat(make_shared<string>(move(s)))
        , off(0)
        , len(get_ref().size())
        , slice(false)
    { }

    FlatSharedStr(const FlatSharedStr &s, unsigned off, unsigned len)
        : flat(s.flat.get_shared_ptr())
        , off(off)
        , len(len)
        , slice(true)
    { }

    string_view get_view() const {
        return string_view(flat->data() + offset(), size());
    }

    bool is_slice() const { return slice; }
    unsigned offset() const { return slice ? off : 0; }
    unsigned size() const { return slice ? len : get_ref().size(); }
};
