/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include <vector>
#include <unordered_set>
#include <cassert>


/*
 * These classes are templates simply because otherwise this header wouldn't be
 * able to compile if included independently (it requires LValue, which requires
 * EvalValue which requires SharedArrayObj). In this case, it wouldn't be a big
 * deal, but in general it's an anti-pattern to have headers requiring a specific
 * include order.
 */

template <class LValueT>
class ArrayConstViewTempl {

    const std::vector<LValueT> &vec;
    const size_type off;
    const size_type len;

public:

    ArrayConstViewTempl(const std::vector<LValueT> &vec, size_type off, size_type len)
        : vec(vec), off(off), len(len)
    { }

    const LValueT &operator[](size_t index) const {
        return vec[off + index];
    }

    size_type size() const {
        return len;
    }
};

template <class LValueT>
class SharedArrayObjTempl final {

public:
    typedef std::vector<LValueT> vec_type;

private:
    static constexpr size_type all_slices = static_cast<size_type>(-1);

    struct SharedObject final {

        vec_type vec;
        std::unordered_set<SharedArrayObjTempl *> slices;

        SharedObject() = default;
        SharedObject(vec_type &&arr)
            : vec(move(arr))
        { }
    };

    shared_ptr<SharedObject> shobj;

public:
    size_type off;
    size_type len;
    bool slice;

    /* Special constructors */

    SharedArrayObjTempl(const vec_type &arr) = delete;

    SharedArrayObjTempl(vec_type &&arr)
        : shobj(make_shared<SharedObject>(move(arr)))
        , off(0)
        , len(shobj->vec.size())
        , slice(false)
    { }

    SharedArrayObjTempl(const SharedArrayObjTempl &obj, size_type off, size_type len)
        : shobj(obj.shobj)
        , off(off)
        , len(len)
        , slice(true)
    {
        shobj->slices.insert(this);
    }

    /* Regular constructors */

    SharedArrayObjTempl() : off(0), len(0), slice(false) { }

    SharedArrayObjTempl(const SharedArrayObjTempl &obj)
        : shobj(obj.shobj)
        , off(obj.off)
        , len(obj.len)
        , slice(obj.slice)
    {
        if (slice)
            shobj->slices.insert(this);
    }

    SharedArrayObjTempl(SharedArrayObjTempl &&obj)
        : shobj(move(obj.shobj))
        , off(obj.off)
        , len(obj.len)
        , slice(obj.slice)
    {
        if (slice) {
            shobj->slices.erase(&obj);
            shobj->slices.insert(this);
            obj.slice = false;
        }
    }

    SharedArrayObjTempl &operator=(const SharedArrayObjTempl &obj)
    {
        shobj = obj.shobj;
        off = obj.off;
        len = obj.len;
        slice = obj.slice;

        if (slice)
            shobj->slices.insert(this);

        return *this;
    }

    SharedArrayObjTempl &operator=(SharedArrayObjTempl &&obj)
    {
        shobj = move(obj.shobj);
        off = obj.off;
        len = obj.len;
        slice = obj.slice;

        if (slice) {
            shobj->slices.erase(&obj);
            shobj->slices.insert(this);
            obj.slice = false;
        }

        return *this;
    }

    ~SharedArrayObjTempl()
    {
        if (slice)
            shobj->slices.erase(this);
    }

    void clone_internal_vec();
    void clone_aliased_slices(size_type index);
    void clone_all_slices() { clone_aliased_slices(all_slices); }

    vec_type &get_vec() { return shobj->vec; }
    const vec_type &get_vec() const { return shobj->vec; }
    int_type use_count() const { return shobj.use_count(); }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }
    size_type size() const { return slice ? len : get_vec().size(); }

    ArrayConstViewTempl<LValueT> get_view() const {
        return ArrayConstViewTempl<LValueT>(get_vec(), offset(), size());
    };
};
