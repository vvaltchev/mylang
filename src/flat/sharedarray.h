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
 * EvalValue which requires FlatSharedArray). In this case, it wouldn't be a big
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

template <class LValueType>
class FlatSharedArrayTempl final {

    template <class LValueT>
    class SharedArrayObj final {

    public:
        typedef std::vector<LValueT> vec_type;

    private:
        static const size_type all_slices = static_cast<size_type>(-1);

        struct SharedObject final {

            vec_type vec;
            std::unordered_set<SharedArrayObj *> slices;

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

        void clone_internal_vec();
        void clone_aliased_slices(size_type index);

        void clone_all_slices()
        {
            clone_aliased_slices(all_slices);
        }

        /* Special constructors */

        SharedArrayObj(const vec_type &arr) = delete;

        SharedArrayObj(vec_type &&arr)
            : shobj(make_shared<SharedObject>(move(arr)))
            , off(0)
            , len(shobj->vec.size())
            , slice(false)
        { }

        SharedArrayObj(const SharedArrayObj &obj, size_type off, size_type len)
            : shobj(obj.shobj)
            , off(off)
            , len(len)
            , slice(true)
        {
            shobj->slices.insert(this);
        }

        /* Regular constructors */

        SharedArrayObj() : off(0), len(0), slice(false) { }

        SharedArrayObj(const SharedArrayObj &obj)
            : shobj(obj.shobj)
            , off(obj.off)
            , len(obj.len)
            , slice(obj.slice)
        {
            if (slice)
                shobj->slices.insert(this);
        }

        SharedArrayObj(SharedArrayObj &&obj)
            : shobj(move(obj.shobj))
            , off(obj.off)
            , len(obj.len)
            , slice(obj.slice)
        {
            if (slice) {
                shobj->slices.erase(&obj);
                shobj->slices.insert(this);
            }
        }

        SharedArrayObj &operator=(const SharedArrayObj &obj)
        {
            shobj = obj.shobj;
            off = obj.off;
            len = obj.len;
            slice = obj.slice;

            if (slice)
                shobj->slices.insert(this);

            return *this;
        }

        SharedArrayObj &operator=(SharedArrayObj &&obj)
        {
            shobj = move(obj.shobj);
            off = obj.off;
            len = obj.len;
            slice = obj.slice;

            if (slice) {
                shobj->slices.erase(&obj);
                shobj->slices.insert(this);
            }

            return *this;
        }

        ~SharedArrayObj()
        {
            if (slice)
                shobj->slices.erase(this);
        }

        vec_type &get_vec() { return shobj->vec; }
        const vec_type &get_vec() const { return shobj->vec; }
        int_type use_count() const { return shobj.use_count(); }
    };

public:
    typedef SharedArrayObj<LValueType> inner_type;
    typedef typename inner_type::vec_type vec_type;

private:
    FlatVal<inner_type> flat;

public:
    FlatSharedArrayTempl() = default;
    FlatSharedArrayTempl(const vec_type &arr) = delete;
    FlatSharedArrayTempl(vec_type &&arr) : flat(move(arr)) { }
    FlatSharedArrayTempl(const FlatSharedArrayTempl &flatWrapper, size_type off, size_type len)
        : flat(flatWrapper.flat.get(), off, len)
    { }

    vec_type &get_ref() { return flat->get_vec(); }
    const vec_type &get_ref() const { return flat->get_vec(); }
    int_type use_count() const { return flat->use_count(); }

    bool is_slice() const { return flat->slice; }
    size_type offset() const { return flat->slice ? flat->off : 0; }
    size_type size() const { return flat->slice ? flat->len : get_ref().size(); }

    void clone_internal_vec() { flat->clone_internal_vec(); }
    void clone_aliased_slices(size_type index) { flat->clone_aliased_slices(index); }
    void clone_all_slices() { flat->clone_all_slices(); }

    ArrayConstViewTempl<LValueType> get_view() const {
        return ArrayConstViewTempl<LValueType>(get_ref(), offset(), size());
    };
};
