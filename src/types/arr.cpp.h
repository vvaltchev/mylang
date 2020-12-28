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

template <>
template <>
void FlatSharedArray::inner_type::clone_internal_vec()
{
    vec_type new_vec(
        shobj->vec.cbegin() + off,
        shobj->vec.cbegin() + off + len
    );

    *this = SharedArrayObj(move(new_vec));
}

template <>
template <>
void FlatSharedArray::inner_type::clone_aliased_slices(size_type index)
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


class TypeArr : public NonTrivialType<FlatSharedArray::inner_type> {

public:

    TypeArr()
        : NonTrivialType<FlatSharedArray::inner_type>(Type::t_arr)
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
    return a.get<FlatSharedArray>().size();
}

int_type TypeArr::use_count(const EvalValue &a)
{
    return a.get<FlatSharedArray>().use_count();
}

bool TypeArr::is_slice(const EvalValue &a)
{
    return a.get<FlatSharedArray>().is_slice();
}

EvalValue TypeArr::clone(const EvalValue &a)
{
    /* The ONLY way to get a copy of the internal SharedArrayObj */
    EvalValue new_val = a;
    new_val.get<FlatSharedArray>().clone_internal_vec();
    return new_val;
}

EvalValue TypeArr::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(&a.get<FlatSharedArray>().get_ref());
}

void TypeArr::add(EvalValue &a, const EvalValue &b)
{
    FlatSharedArray &lval = a.get<FlatSharedArray>();

    if (!b.is<FlatSharedArray>())
        throw TypeErrorEx("Expected array on the right side of +");

    const FlatSharedArray &rhs = b.get<FlatSharedArray>();

    if (!lval.is_slice()) {

        lval.get_ref().reserve(lval.size() + rhs.size());

        lval.get_ref().insert(
            lval.get_ref().end(),
            rhs.get_ref().cbegin() + rhs.offset(),
            rhs.get_ref().cbegin() + rhs.offset() + rhs.size()
        );

    } else {

        FlatSharedArray::vec_type new_arr;
        new_arr.reserve(lval.size() + rhs.size());

        new_arr.insert(
            new_arr.end(),
            lval.get_ref().begin() + lval.offset(),
            lval.get_ref().begin() + lval.offset() + lval.size()
        );

        new_arr.insert(
            new_arr.end(),
            rhs.get_ref().cbegin() + rhs.offset(),
            rhs.get_ref().cbegin() + rhs.offset() + rhs.size()
        );

        dtor(&lval); /* We have to manually destroy our fake "trivial" object */
        new (&lval) FlatSharedArray(move(new_arr));
    }
}

void TypeArr::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedArray>()) {
        a = false;
        return;
    }

    const FlatSharedArray &lhs = a.get<FlatSharedArray>();
    const FlatSharedArray &rhs = b.get<FlatSharedArray>();
    const ArrayConstView &lhs_view = lhs.get_view();
    const ArrayConstView &rhs_view = rhs.get_view();

    if (lhs_view.size() != rhs_view.size()) {
        a = false;
        return;
    }

    if (&lhs.get_ref() == &rhs.get_ref()) {
        /* Same vector, now just check the offsets */
        a = lhs.offset() == rhs.offset();
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
    const FlatSharedArray &arr = a.get<FlatSharedArray>();
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
    return a.get<FlatSharedArray>().size() > 0;
}

EvalValue TypeArr::subscript(const EvalValue &what_lval, const EvalValue &idx_val)
{
    if (!idx_val.is<int_type>())
        throw TypeErrorEx("Expected integer as subscript");

    const EvalValue &what = RValue(what_lval);
    FlatSharedArray &&arr = what.get<FlatSharedArray>();
    FlatSharedArray::vec_type &vec = arr.get_ref();
    int_type idx = idx_val.get<int_type>();

    if (idx < 0)
        idx += arr.size();

    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx();

    LValue *ret = &vec[arr.offset() + idx];

    if (!what_lval.is<LValue *>()) {
        /* The input array was not an LValue, so return a simple RValue */
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
    const FlatSharedArray &arr = what.get<FlatSharedArray>();
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

    return FlatSharedArray(arr, arr.offset() + start, end - start);
}
