/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "evaltypes.cpp.h"

template <>
SharedArrayTemplate<LValue>::SharedArrayTemplate(vector<LValue> &&arr)
    : shval(make_shared<vector<LValue>>(move(arr)))
    , off(0)
    , len(get_ref().size())
    , slice(false)
{
}

class TypeArr : public SharedType<SharedArray> {

public:

    TypeArr() : SharedType<SharedArray>(Type::t_arr) { }

    virtual void add(EvalValue &a, const EvalValue &b);
    virtual void eq(EvalValue &a, const EvalValue &b);
    virtual void noteq(EvalValue &a, const EvalValue &b);
    virtual EvalValue subscript(const EvalValue &what, const EvalValue &idx);
    virtual EvalValue slice(const EvalValue &what,
                            const EvalValue &start,
                            const EvalValue &end);

    virtual long use_count(const EvalValue &a);
    virtual bool is_slice(const EvalValue &a);
    virtual EvalValue clone(const EvalValue &a);
    virtual EvalValue intptr(const EvalValue &a);

    virtual long len(const EvalValue &a) {
        return a.get<SharedArray>().size();
    }

    virtual string to_string(const EvalValue &a);
};

long TypeArr::use_count(const EvalValue &a)
{
    return a.get<SharedArray>().use_count();
}

bool TypeArr::is_slice(const EvalValue &a)
{
    return a.get<SharedArray>().is_slice();
}

EvalValue TypeArr::clone(const EvalValue &a)
{
    const SharedArray &lval = a.get<SharedArray>();
    SharedArray::inner_type new_arr;
    new_arr.reserve(lval.len);

    new_arr.insert(
        new_arr.end(),
        lval.get_ref().begin() + lval.off,
        lval.get_ref().begin() + lval.off + lval.len
    );

    return SharedArray(move(new_arr));
}

EvalValue TypeArr::intptr(const EvalValue &a)
{
    return reinterpret_cast<long>(&a.get<SharedArray>().get_ref());
}

void TypeArr::add(EvalValue &a, const EvalValue &b)
{
    SharedArray &lval = a.get<SharedArray>();

    if (!b.is<SharedArray>())
        throw TypeErrorEx();

    const SharedArray &rhs = b.get<SharedArray>();

    if (!lval.is_slice()) {

        lval.get_ref().reserve(lval.size() + rhs.size());

        lval.get_ref().insert(
            lval.get_ref().end(),
            rhs.get_ref().cbegin() + rhs.offset(),
            rhs.get_ref().cbegin() + rhs.offset() + rhs.size()
        );

    } else {

        SharedArray::inner_type new_arr;
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

        dtor(&lval.get_shval()); /* We have to manually destroy our fake "trivial" object */
        new (&lval.get_shval()) SharedArray(move(new_arr));
    }
}

void TypeArr::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedArray>()) {
        a = false;
        return;
    }

    const SharedArray &lhs = a.get<SharedArray>();
    const SharedArray &rhs = b.get<SharedArray>();

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
    SharedArray &&arr = a.get<SharedArray>();
    string res;

    res.reserve(arr.size() * 32);
    res += "[";

    const SharedArray::inner_type &vec = arr.shval.get();

    for (unsigned i = 0; i < arr.size(); i++) {

        const EvalValue &val = vec[arr.offset() + i].get();
        res += val.get_type()->to_string(val);

        if (i != arr.size() - 1)
            res += ", ";
    }

    res += "]";
    return res;
}

EvalValue TypeArr::subscript(const EvalValue &what_lval, const EvalValue &idx_val)
{
    if (!idx_val.is<long>())
        throw TypeErrorEx();

    const EvalValue &what = RValue(what_lval);
    SharedArray &&arr = what.get<SharedArray>();
    SharedArray::inner_type &vec = arr.shval.get();
    long idx = idx_val.get<long>();

    if (idx < 0)
        idx += arr.size();

    if (idx < 0 || idx >= arr.size())
        throw OutOfBoundsEx();

    LValue *ret = &vec[arr.off + idx];

    if (!what_lval.is<LValue *>()) {
        /* The input array was not an LValue, so return a simple RValue */
        return ret->get();
    }

    /* We deferenced a LValue array, so return element's LValue */
    ret->container = what_lval.get<LValue *>();
    ret->container_idx = arr.offset() + idx;
    return ret;
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
    arr2.set_slice(arr.offset() + start, end - start);
    return arr2;
}
