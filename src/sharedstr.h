/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include <string>

class SharedStr final {

public:
    typedef std::string inner_type;

private:
    shared_ptr<inner_type> obj;
    size_type off = 0;
    size_type len = 0;
    bool slice = false;

public:

    SharedStr() = default;

    /*
     * That's not really necessary, just it helps knowing that we won't
     * copy strings, but just move them.
     */
    SharedStr(const inner_type &s) = delete;

    SharedStr(inner_type &&s)
        : obj(make_shared<inner_type>(move(s)))
        , off(0)
        , len(obj->size())
        , slice(false)
    { }

    SharedStr(const SharedStr &s, size_type off, size_type len)
        : obj(s.obj)
        , off(off)
        , len(len)
        , slice(true)
    { }

    int_type use_count() const { return obj.use_count(); }
    inner_type &get_ref() { return *obj.get(); }
    const inner_type &get_ref() const { return *obj.get(); }

    std::string_view get_view() const {
        return std::string_view(obj->data() + offset(), size());
    }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }
    size_type size() const { return slice ? len : obj->size(); }
};
