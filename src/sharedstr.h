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
        /*
         * Strings are immutable, so their hash never changes - cache it on the
         * shared object, computed lazily on first use (so a string never used
         * as a key / hash() arg costs nothing). `mutable` because hash() is
         * logically const; safe since `s` never changes. Only the FULL-string
         * hash is cached here; a slice hashes its sub-view on demand.
         */
        mutable size_t hash_cache = 0;
        mutable bool hash_valid = false;
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
    {
        /* the slice must lie within the underlying string (no overflow form) */
        ML_CHECK(off <= obj->s.size() && len <= obj->s.size() - off);
    }

    int_type use_count() const { return obj.use_count(); }
    inner_type &get_ref() { return obj->s; }
    const inner_type &get_ref() const { return obj->s; }

    std::string_view get_view() const {
        ML_CHECK(offset() <= obj->s.size() &&
                 size() <= obj->s.size() - offset());
        return std::string_view(obj->s.data() + offset(), size());
    }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }
    size_type size() const { return slice ? len : obj->s.size(); }

    /*
     * Hash of the string's value. A full (non-slice) string caches it on the
     * shared StrObj (computed once); a slice hashes its sub-view on demand.
     */
    size_t hash() const {
        if (!slice) {
            if (!obj->hash_valid) {
                obj->hash_cache = std::hash<std::string_view>()(
                    std::string_view(obj->s.data(), obj->s.size()));
                obj->hash_valid = true;
            }
            return obj->hash_cache;
        }
        return std::hash<std::string_view>()(get_view());
    }
};
