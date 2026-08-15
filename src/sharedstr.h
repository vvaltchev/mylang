/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "poolalloc.h"
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

        ML_POOL_NEW_DELETE

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
        StrObj(inner_type &&str) : s(std::move(str)) { }
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
        : obj(make_intrusive<StrObj>(std::move(s)))
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

    /*
     * THE WINDOW MODEL (2026-08-06). EVERY SharedStr is a window
     * [offset(), offset()+size()) over a shared, APPEND-ONLY buffer - what
     * a slice always was; a "full" string is just the window that happens
     * to cover the whole buffer. Because the buffer only ever GROWS (the
     * one in-place mutation is TypeStr::append, below), a window taken at
     * any moment keeps denoting the same characters forever. That single
     * fact is the whole soundness argument, and it is what gives strings
     * their documented VALUE semantics with no copy-on-write clone at all:
     * after `var b = a; a += "!"`, `a`'s window grew and `b`'s did not, so
     * b still reads "hi" while sharing the same buffer.
     *
     * `len` is therefore AUTHORITATIVE for both forms - hence size() below
     * is a plain field read rather than a dependent load through `obj`.
     *
     * owns_whole_buffer() is the precondition for appending IN PLACE: this
     * window must end where the buffer ends, or growing the buffer would
     * silently extend a window that is only a prefix of it.
     */
    bool owns_whole_buffer() const {
        return !slice && obj && len == obj->s.size();
    }

    /*
     * Fix up this window after the ONE in-place mutation there is: append
     * growing a buffer this window wholly owns. Every other path builds a
     * fresh SharedStr, whose ctor sets `len`.
     *
     * BOTH steps are load-bearing. Re-deriving `len` keeps the window on
     * the new end. Dropping `hash_valid` is what the DICT depends on: the
     * cache is the hash of the WHOLE buffer, and the buffer just changed,
     * so every window that covers it - including this one - would
     * otherwise keep answering with the PRE-append hash. Without this, a
     * string used as a key, appended to, and inserted into a second dict
     * lands under the old hash and is unfindable by its own value (it
     * still prints and compares equal - the map is simply corrupt).
     */
    void after_inplace_append() {
        ML_CHECK(!slice);
        len = static_cast<size_type>(obj->s.size());
        obj->hash_valid = false;
    }

    std::string_view get_view() const {
        ML_CHECK(offset() <= obj->s.size() &&
                 size() <= obj->s.size() - offset());
        return std::string_view(obj->s.data() + offset(), size());
    }

    /*
     * The JIT's layout probe (the co-located-probe rule, as
     * SharedArrayObj::jit_probe): hand out the ADDRESS of the window
     * length so jit.cpp can bake its byte offset from a REAL object
     * instead of growing a second copy of this class's layout that could
     * drift. `len` is private, hence the accessor.
     */
    struct JitProbe { const void *len; };
    JitProbe jit_probe() const { return { &len }; }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }
    /* `len` is authoritative for BOTH forms - see THE WINDOW MODEL above.
     * (A default-constructed SharedStr has no `obj`; returning `len` == 0
     * is also what makes that case safe rather than a null deref.) */
    size_type size() const { return len; }

    /*
     * Hash of the string's value. A full (non-slice) string caches it on the
     * shared StrObj (computed once); a slice hashes its sub-view on demand.
     */
    size_t hash() const {
        /* The StrObj's cache is the hash of the WHOLE buffer, so it may
         * only be used by a window that covers it - a prefix window must
         * hash its own view. (Before the window model a "non-slice" always
         * covered the buffer, so `!slice` alone was the same test.) */
        if (owns_whole_buffer()) {
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
