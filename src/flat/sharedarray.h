/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "flatval.h"
#include <vector>
#include <unordered_set>
#include <cassert>

using namespace std;

/*
 * These classes are templates simply because otherwise this header wouldn't be
 * able to compile if included independently (it requires LValue, which requires
 * EvalValue which requires FlatSharedArray). In this case, it wouldn't be a big
 * deal, but in general it's an anti-pattern to have headers requiring a specific
 * include order.
 */

template <class LValueT>
class ArrayConstViewTempl {

    const vector<LValueT> &vec;
    const unsigned off;
    const unsigned len;

public:

    ArrayConstViewTempl(const vector<LValueT> &vec, unsigned off, unsigned len)
        : vec(vec), off(off), len(len)
    { }

    const LValueT &operator[](size_t index) const {
        return vec[off + index];
    }

    unsigned size() const {
        return len;
    }
};

template <class LValueType>
class FlatSharedArrayTempl final {

    template <class LValueT>
    class SharedArrayObj final {

    public:
        typedef vector<LValueT> vec_type;

    private:
        static const unsigned all_slices = static_cast<unsigned>(-1);

        struct SharedObject final {

            vec_type vec;
            unordered_set<SharedArrayObj *> slices;

            SharedObject() = default;
            SharedObject(vec_type &&arr)
                : vec(move(arr))
            { }
        };

        shared_ptr<SharedObject> shobj;

    public:
        unsigned off;
        unsigned len;
        bool slice;

        void clone_internal_vec()
        {
            vec_type new_vec(
                shobj->vec.cbegin() + off,
                shobj->vec.cbegin() + off + len
            );

            this->~SharedArrayObj();
            new (this) SharedArrayObj(move(new_vec));
        }

        void clone_aliased_slices(unsigned index)
        {
            auto &slices = shobj->slices;

            for (auto it = slices.begin(); it != slices.end(); /* no inc */) {

                auto obj = *it;

                if (index == all_slices ||
                    (obj->off <= index && index < obj->off + obj->len))
                {
                    /*
                     * Erase the object from here and get an iterator for the next
                     * object in the container.
                     */
                    it = shobj->slices.erase(it);
                    assert(obj->slice);

                    /* Prevent the dtor from trying to erase the object */
                    obj->slice = false;

                    /* Do clone the internal vector */
                    obj->clone_internal_vec();

                } else {

                    /* Just move to the next slice */
                    ++it;
                }
            }
        }

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

        SharedArrayObj(const SharedArrayObj &obj, unsigned off, unsigned len)
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
        long use_count() const { return shobj.use_count(); }
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
    FlatSharedArrayTempl(const FlatSharedArrayTempl &flatWrapper, unsigned off, unsigned len)
        : flat(flatWrapper.flat.get(), off, len)
    { }

    vec_type &get_ref() { return flat->get_vec(); }
    const vec_type &get_ref() const { return flat->get_vec(); }
    long use_count() const { return flat->use_count(); }

    bool is_slice() const { return flat->slice; }
    unsigned offset() const { return flat->slice ? flat->off : 0; }
    unsigned size() const { return flat->slice ? flat->len : get_ref().size(); }

    void clone_internal_vec() { flat->clone_internal_vec(); }
    void clone_aliased_slices(unsigned index) { flat->clone_aliased_slices(index); }
    void clone_all_slices() { flat->clone_all_slices(); }

    ArrayConstViewTempl<LValueType> get_view() const {
        return ArrayConstViewTempl<LValueType>(get_ref(), offset(), size());
    };
};
