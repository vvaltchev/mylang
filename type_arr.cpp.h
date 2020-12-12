/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "evaltypes.cpp.h"

SharedArray::SharedArray(vector<LValue> &&arr)
    : vec(make_shared<vector<LValue>>(move(arr)))
    , off(0)
    , len(get_ref().size())
{
}


class TypeArr : public SharedType<SharedArray> {

public:

    TypeArr() : SharedType<SharedArray>(Type::t_arr) { }

    // virtual void add(EvalValue &a, const EvalValue &b);
    // virtual void eq(EvalValue &a, const EvalValue &b);
    // virtual void noteq(EvalValue &a, const EvalValue &b);
    virtual EvalValue subscript(const EvalValue &what, const EvalValue &idx);
    virtual EvalValue slice(const EvalValue &what,
                             const EvalValue &start,
                             const EvalValue &end);

    virtual long len(const EvalValue &a) {
        return a.get<SharedArray>().size();
    }

    virtual string to_string(const EvalValue &a);
};

string TypeArr::to_string(const EvalValue &a)
{
    SharedArray &&arr = a.get<SharedArray>();
    string res;

    res.reserve(arr.size() * 32);
    res += "[";

    const SharedArray::inner_type &vec = arr.vec.get();

    for (unsigned i = 0; i < arr.len; i++) {

        const EvalValue &val = vec[arr.off + i].get();
        res += val.get_type()->to_string(val);

        if (i != arr.len - 1)
            res += ", ";
    }

    res += "]";
    return res;
}

EvalValue TypeArr::subscript(const EvalValue &what, const EvalValue &idx_val)
{
    if (!idx_val.is<long>())
        throw TypeErrorEx();

    SharedArray &&arr = what.get<SharedArray>();
    SharedArray::inner_type &vec = arr.vec.get();
    long idx = idx_val.get<long>();

    if (idx < 0)
        idx += arr.size();

    if (idx < 0 || idx >= arr.size())
        throw OutOfBoundsEx();

    return &vec[arr.off + idx];
}

EvalValue TypeArr::slice(const EvalValue &what,
                         const EvalValue &start_val,
                         const EvalValue &end_val)
{
    const SharedArray &arr = what.get<SharedArray>();
    long start = 0, end = arr.size();

    if (start_val.is<long>()) {

        start = start_val.get<long>();

        if (start < 0) {

            start += arr.size();

            if (start < 0)
                start = 0;
        }

        if (start >= arr.size())
            return EvalValue::empty_str;

    } else if (!start_val.is<NoneVal>()) {

        throw TypeErrorEx();
    }

    if (end_val.is<long>()) {

        end = end_val.get<long>();

        if (end < 0)
            end += arr.size();

        if (end <= start)
            return EvalValue::empty_str;

        if (end > arr.size())
            end = arr.size();

    } else if (!end_val.is<NoneVal>()) {

        throw TypeErrorEx();
    }

    SharedArray arr2;
    copy_ctor(&arr2, &arr); /* See TypeStr::subscript */
    arr2.off += start;
    arr2.len = end - start;
    return arr2;
}
