/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "flatval.h"
#include <vector>

using namespace std;

/*
 * This class is a template simply because otherwise this header wouldn't be
 * able to compile if included independently (it requires LValue, which requires
 * EvalValue which requires FlatSharedArray). In this case, it wouldn't be a big
 * deal, but in general it's an anti-pattern to have headers requiring a specific
 * include order.
 */

template <class LValueType>
class FlatSharedArrayTempl {

    template <class LValueT>
    class SharedArrayObj {

    public:
        typedef vector<LValueT> vec_type;

    private:
        struct SharedObject {

            vec_type vec;

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

        SharedArrayObj() : off(0), len(0), slice(false) { }
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
        { }

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
};
