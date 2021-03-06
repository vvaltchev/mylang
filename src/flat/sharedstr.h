/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include <string>

class FlatSharedStr final {

public:
    typedef std::string inner_type;

private:

    FlatSharedVal<inner_type> flat;
    size_type off = 0;   /* NOTE: cannot be const because we're using this in a union */
    size_type len = 0;   /* NOTE: cannot be const because we're using this in a union */
    bool slice = false;

public:

    inner_type &get_ref() { return flat.get(); }
    const inner_type &get_ref() const { return flat.get(); }
    FlatSharedVal<inner_type> &get_shval() { return flat; }
    const FlatSharedVal<inner_type> &get_shval() const { return flat; }
    int_type use_count() const { return flat.use_count(); }

    FlatSharedStr() = default;

    /*
     * That's not really necessary, just it helps knowing that we won't
     * copy strings, but just move them.
     */
    FlatSharedStr(const inner_type &s) = delete;

    FlatSharedStr(inner_type &&s)
        : flat(make_shared<inner_type>(move(s)))
        , off(0)
        , len(get_ref().size())
        , slice(false)
    { }

    FlatSharedStr(const FlatSharedStr &s, size_type off, size_type len)
        : flat(s.flat.get_shared_ptr())
        , off(off)
        , len(len)
        , slice(true)
    { }

    std::string_view get_view() const {
        return std::string_view(flat->data() + offset(), size());
    }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }
    size_type size() const { return slice ? len : get_ref().size(); }
};
