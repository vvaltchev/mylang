/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include "intrusiveptr.h"
#include <string>

class SharedStr final {

public:
    typedef std::string inner_type;

private:
    /*
     * std::string can't carry the intrusive refcount itself, so the shared
     * payload is this thin wrapper. The public inner_type stays std::string;
     * get_ref() hands out the wrapped string.
     */
    struct StrObj final : RefCounted {
        inner_type s;
        StrObj(inner_type &&str) : s(move(str)) { }
    };

    intrusive_ptr<StrObj> obj;
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
        : obj(make_intrusive<StrObj>(move(s)))
        , off(0)
        , len(obj->s.size())
        , slice(false)
    { }

    SharedStr(const SharedStr &s, size_type off, size_type len)
        : obj(s.obj)
        , off(off)
        , len(len)
        , slice(true)
    { }

    int_type use_count() const { return obj.use_count(); }
    inner_type &get_ref() { return obj->s; }
    const inner_type &get_ref() const { return obj->s; }

    std::string_view get_view() const {
        return std::string_view(obj->s.data() + offset(), size());
    }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }
    size_type size() const { return slice ? len : obj->s.size(); }
};
