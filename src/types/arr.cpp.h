/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "evaltypes.cpp.h"

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

    long use_count(const EvalValue &a) override;
    bool is_slice(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;

    long len(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    bool is_true(const EvalValue &a) override;
};

long TypeArr::len(const EvalValue &a)
{
    return a.get<FlatSharedArray>().size();
}

long TypeArr::use_count(const EvalValue &a)
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
    return reinterpret_cast<long>(&a.get<FlatSharedArray>().get_ref());
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

    if (lhs.size() != rhs.size()) {
        a = false;
        return;
    }

    if (&lhs.get_ref() == &rhs.get_ref()) {
        /* Same vector, now just check the offsets */
        a = lhs.offset() == rhs.offset();
        return;
    }

    for (unsigned i = 0; i < lhs.size(); i++) {

        EvalValue &&obj_a = lhs.get_ref()[lhs.offset() + i].get_rval();
        const EvalValue &obj_b = rhs.get_ref()[rhs.offset() + i].get();

        obj_a.get_type()->eq(obj_a, obj_b);

        if (!obj_a.get<long>()) {
            a = false;
            return;
        }
    }

    a = true;
}

void TypeArr::noteq(EvalValue &a, const EvalValue &b)
{
    eq(a, b);
    a = !a.get<long>();
}

string TypeArr::to_string(const EvalValue &a)
{
    FlatSharedArray &&arr = a.get<FlatSharedArray>();
    string res;

    res.reserve(arr.size() * 32);
    res += "[";

    const ArrayConstView &arr_view = arr.get_view();

    for (unsigned i = 0; i < arr_view.size(); i++) {

        const EvalValue &val = arr_view[i].get();
        res += val.get_type()->to_string(val);

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
    if (!idx_val.is<long>())
        throw TypeErrorEx("Expected integer as subscript");

    const EvalValue &what = RValue(what_lval);
    FlatSharedArray &&arr = what.get<FlatSharedArray>();
    FlatSharedArray::vec_type &vec = arr.get_ref();
    long idx = idx_val.get<long>();

    if (idx < 0)
        idx += arr.size();

    if (idx < 0 || idx >= arr.size())
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
    long start = 0, end = arr.size();

    if (start_val.is<long>()) {

        start = start_val.get<long>();

        if (start < 0) {

            start += arr.size();

            if (start < 0)
                start = 0;
        }

        if (start >= arr.size())
            return empty_arr;

    } else if (!start_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range start");
    }

    if (end_val.is<long>()) {

        end = end_val.get<long>();

        if (end < 0)
            end += arr.size();

        if (end <= start)
            return empty_arr;

        if (end > arr.size())
            end = arr.size();

    } else if (!end_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range end");
    }

    return FlatSharedArray(arr, arr.offset() + start, end - start);
}
