/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "evaltypes.cpp.h"

template <class LValueT>
void SharedArrayObjTempl<LValueT>::clone_internal_vec()
{
    /*
     * Use offset()/size(), not the raw off/len fields: for a non-slice, `len`
     * is only the size at construction and goes stale when `+=` appends to the
     * vector in place (a non-slice tracks its length via vec.size(), see
     * size()). offset()/size() are correct for both slices and non-slices.
     *
     * Kind-aware: a flat int/float array clones into a fresh *flat* array, so
     * the cheap representation survives clone() and copy-on-write (a const flat
     * array stays flat, `a.clone()` keeps it flat). Only a general array clones
     * into vector<LValue>.
     */
    switch (shobj->kind) {

        case Storage::ints: {
            ivec_type nv(
                shobj->ivec.cbegin() + offset(),
                shobj->ivec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(move(nv));
            return;
        }

        case Storage::floats: {
            fvec_type nv(
                shobj->fvec.cbegin() + offset(),
                shobj->fvec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(move(nv));
            return;
        }

        default:
            break;
    }

    vec_type new_vec(
        shobj->vec.cbegin() + offset(),
        shobj->vec.cbegin() + offset() + size()
    );

    *this = SharedArrayObjTempl(move(new_vec));
}

template <class LValueT>
void SharedArrayObjTempl<LValueT>::clone_aliased_slices(size_type index)
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

            /*
             * Clone the slice's range into its own vector. We leave obj->slice
             * set so clone_internal_vec's offset()/size() report the slice
             * range; the move-assign it does turns obj into a standalone
             * non-slice, and obj was already removed from the slices set above,
             * so nothing tries to erase it again.
             */
            obj->clone_internal_vec();

        } else {

            /* Just move to the next slice */
            ++it;
        }
    }
}

/*
 * Convert unboxed int/float storage into the general vector<LValue> form, in
 * place (destroy the flat union member, construct the general one). Value-
 * preserving, so it is sound even when the object is shared/sliced. Called by
 * get_vec() the first time something needs LValue access (see plans/
 * typed-arrays.md). Defined here, not in sharedarray.h, because it constructs
 * EvalValue/LValue.
 */
template <class LValueT>
void SharedArrayObjTempl<LValueT>::promote_to_general()
{
    if (shobj->kind == Storage::general)
        return;

    vec_type gv;

    if (shobj->kind == Storage::ints) {
        gv.reserve(shobj->ivec.size());
        for (int_type x : shobj->ivec)
            gv.emplace_back(EvalValue(x), false);
        shobj->ivec.~ivec_type();
    } else {
        gv.reserve(shobj->fvec.size());
        for (float_type x : shobj->fvec)
            gv.emplace_back(EvalValue(x), false);
        shobj->fvec.~fvec_type();
    }

    new (&shobj->vec) vec_type(move(gv));
    shobj->kind = Storage::general;
}


class TypeArr : public TypeImpl<SharedArrayObj> {

public:

    TypeArr()
        : TypeImpl<SharedArrayObj>(Type::t_arr)
    { }

    void add(EvalValue &a, const EvalValue &b) override;
    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    EvalValue subscript(const EvalValue &what, const EvalValue &idx) override;
    EvalValue slice(const EvalValue &what,
                    const EvalValue &start,
                    const EvalValue &end) override;

    int_type use_count(const EvalValue &a) override;
    bool is_slice(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;

    int_type len(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    bool is_true(const EvalValue &a) override;
};

int_type TypeArr::len(const EvalValue &a)
{
    return a.get<SharedArrayObj>().size();
}

int_type TypeArr::use_count(const EvalValue &a)
{
    return a.get<SharedArrayObj>().use_count();
}

bool TypeArr::is_slice(const EvalValue &a)
{
    return a.get<SharedArrayObj>().is_slice();
}

EvalValue TypeArr::clone(const EvalValue &a)
{
    /* The ONLY way to get a copy of the internal SharedArrayObj */
    EvalValue new_val = a;
    new_val.get<SharedArrayObj>().clone_internal_vec();
    return new_val;
}

EvalValue TypeArr::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(&a.get<SharedArrayObj>().get_vec());
}

void TypeArr::add(EvalValue &a, const EvalValue &b)
{
    SharedArrayObj &lval = a.get<SharedArrayObj>();

    if (lval.is_readonly())
        throw CannotChangeConstEx();

    if (!b.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array on the right side of +");

    const SharedArrayObj &rhs = b.get<SharedArrayObj>();

    /*
     * Flat homogeneous fast path: int+int / float+float concatenate in unboxed
     * storage, with no promotion (8-byte appends). A mismatched or general
     * operand falls through to the boxed path below, which promotes both via
     * get_vec(). A non-slice lhs appends in place (same alias semantics as the
     * general path); a slice lhs builds a fresh flat array.
     */
    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::ints)
    {
        const auto &rv = rhs.flat_ints();

        if (!lval.is_slice()) {

            auto &lv = lval.flat_ints();
            lv.reserve(lval.size() + rhs.size());
            lv.insert(lv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());

        } else {

            const auto &lv = lval.flat_ints();
            SharedArrayObj::ivec_type nv;
            nv.reserve(lval.size() + rhs.size());
            nv.insert(nv.end(),
                      lv.cbegin() + lval.offset(),
                      lv.cbegin() + lval.offset() + lval.size());
            nv.insert(nv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());
            lval = SharedArrayObj(move(nv));
        }

        return;
    }

    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::floats)
    {
        const auto &rv = rhs.flat_floats();

        if (!lval.is_slice()) {

            auto &lv = lval.flat_floats();
            lv.reserve(lval.size() + rhs.size());
            lv.insert(lv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());

        } else {

            const auto &lv = lval.flat_floats();
            SharedArrayObj::fvec_type nv;
            nv.reserve(lval.size() + rhs.size());
            nv.insert(nv.end(),
                      lv.cbegin() + lval.offset(),
                      lv.cbegin() + lval.offset() + lval.size());
            nv.insert(nv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());
            lval = SharedArrayObj(move(nv));
        }

        return;
    }

    if (!lval.is_slice()) {

        lval.get_vec().reserve(lval.size() + rhs.size());

        lval.get_vec().insert(
            lval.get_vec().end(),
            rhs.get_vec().cbegin() + rhs.offset(),
            rhs.get_vec().cbegin() + rhs.offset() + rhs.size()
        );

    } else {

        SharedArrayObj::vec_type new_arr;
        new_arr.reserve(lval.size() + rhs.size());

        new_arr.insert(
            new_arr.end(),
            lval.get_vec().begin() + lval.offset(),
            lval.get_vec().begin() + lval.offset() + lval.size()
        );

        new_arr.insert(
            new_arr.end(),
            rhs.get_vec().cbegin() + rhs.offset(),
            rhs.get_vec().cbegin() + rhs.offset() + rhs.size()
        );

        lval = SharedArrayObj(move(new_arr));
    }
}

void TypeArr::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedArrayObj>()) {
        a = false;
        return;
    }

    const SharedArrayObj &lhs = a.get<SharedArrayObj>();
    const SharedArrayObj &rhs = b.get<SharedArrayObj>();
    const ArrayConstView &lhs_view = lhs.get_view();
    const ArrayConstView &rhs_view = rhs.get_view();

    if (lhs_view.size() != rhs_view.size()) {
        a = false;
        return;
    }

    if (&lhs.get_vec() == &rhs.get_vec() && lhs.offset() == rhs.offset()) {
        /*
         * Same vector AND same offset (sizes are already known equal): the two
         * views cover the very same region, so they are necessarily equal.
         * A different offset does NOT imply inequality, though: two slices of
         * the same array can hold equal elements at different offsets, so in
         * that case we must fall through to the element-by-element comparison.
         */
        a = true;
        return;
    }

    for (size_type i = 0; i < lhs.size(); i++) {

        if (lhs_view[i].get() != rhs_view[i].get()) {
            a = false;
            return;
        }
    }

    a = true;
}

void TypeArr::noteq(EvalValue &a, const EvalValue &b)
{
    eq(a, b);
    a = !a.get<int_type>();
}

string TypeArr::to_string(const EvalValue &a)
{
    const SharedArrayObj &arr = a.get<SharedArrayObj>();
    const ArrayConstView &arr_view = arr.get_view();
    string res;

    res.reserve(arr.size() * 32);
    res += "[";

    for (size_type i = 0; i < arr_view.size(); i++) {

        const EvalValue &val = arr_view[i].get();
        res += val.to_string();

        if (i != arr_view.size() - 1)
            res += ", ";
    }

    res += "]";
    return res;
}

bool TypeArr::is_true(const EvalValue &a)
{
    return a.get<SharedArrayObj>().size() > 0;
}

EvalValue TypeArr::subscript(const EvalValue &what_lval, const EvalValue &idx_val)
{
    if (!idx_val.is<int_type>())
        throw TypeErrorEx("Expected integer as subscript");

    const EvalValue &what = RValue(what_lval);
    SharedArrayObj &&arr = what.get<SharedArrayObj>();
    SharedArrayObj::vec_type &vec = arr.get_vec();
    int_type idx = idx_val.get<int_type>();

    if (idx < 0)
        idx += arr.size();

    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx();

    LValue *ret = &vec[arr.offset() + idx];

    if (!what_lval.is<LValue *>() || arr.is_readonly()) {
        /*
         * Return a simple RValue when the input array was not an LValue, or
         * when it is read-only (a `const` value): a read still works, but an
         * assignment target is an rvalue, so `a[i] = x` fails with NotLValueEx.
         */
        return ret->get();
    }

    /* We deferenced a LValue array, so return element's LValue */
    ret->container = what_lval.get<LValue *>();
    ret->container_idx = arr.offset() + idx;
    return ret;
}

EvalValue TypeArr::slice(const EvalValue &what_lval,
                         const EvalValue &start_val,
                         const EvalValue &end_val)
{
    const EvalValue &what = RValue(what_lval);
    const SharedArrayObj &arr = what.get<SharedArrayObj>();
    int_type start = 0, end = arr.size();

    if (start_val.is<int_type>()) {

        start = start_val.get<int_type>();

        if (start < 0) {

            start += arr.size();

            if (start < 0)
                start = 0;
        }

        if (static_cast<size_t>(start) >= arr.size())
            return empty_arr;

    } else if (!start_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range start");
    }

    if (end_val.is<int_type>()) {

        end = end_val.get<int_type>();

        if (end < 0)
            end += arr.size();

        if (end <= start)
            return empty_arr;

        if (static_cast<size_t>(end) > arr.size())
            end = arr.size();

    } else if (!end_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range end");
    }

    return SharedArrayObj(arr, arr.offset() + start, end - start);
}
