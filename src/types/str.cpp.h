/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "evaltypes.cpp.h"

class TypeStr : public SharedType<FlatSharedStr> {

    void append(FlatSharedStr &lval, const string_view &s);

public:

    TypeStr() : SharedType<FlatSharedStr>(Type::t_str) { }

    virtual void add(EvalValue &a, const EvalValue &b);
    virtual void mul(EvalValue &a, const EvalValue &b);
    virtual void lt(EvalValue &a, const EvalValue &b);
    virtual void gt(EvalValue &a, const EvalValue &b);
    virtual void le(EvalValue &a, const EvalValue &b);
    virtual void ge(EvalValue &a, const EvalValue &b);
    virtual void eq(EvalValue &a, const EvalValue &b);
    virtual void noteq(EvalValue &a, const EvalValue &b);
    virtual EvalValue subscript(const EvalValue &what, const EvalValue &idx);
    virtual EvalValue slice(const EvalValue &what,
                            const EvalValue &start,
                            const EvalValue &end);

    virtual long use_count(const EvalValue &a);
    virtual EvalValue clone(const EvalValue &a);
    virtual bool is_slice(const EvalValue &a);
    virtual EvalValue intptr(const EvalValue &a);

    virtual long len(const EvalValue &a) {
        return a.get<FlatSharedStr>().size();
    }

    virtual string to_string(const EvalValue &a) {
        return string(a.get<FlatSharedStr>().get_view());
    }
};

EvalValue TypeStr::clone(const EvalValue &a)
{
    /*
     * Don't implement a real clone() for strings, simply because at the
     * moment, we're following Python's model in which strings are immutable.
     * In other words, `str[i]` returns just a slice, NOT an LValue to the N-th
     * character in the string.
     */
    return a;
}

long TypeStr::use_count(const EvalValue &a)
{
    return a.get<FlatSharedStr>().use_count();
}

bool TypeStr::is_slice(const EvalValue &a)
{
    return a.get<FlatSharedStr>().is_slice();
}

EvalValue TypeStr::intptr(const EvalValue &a)
{
    return reinterpret_cast<long>(&a.get<FlatSharedStr>().get_ref());
}

void TypeStr::append(FlatSharedStr &lval, const string_view &s)
{
    if (!lval.is_slice()) {

        lval.get_ref() += s;

    } else {

        string new_str;
        new_str.reserve(lval.size() + s.size());
        new_str += lval.get_view();
        new_str += s;

        dtor(&lval); /* We have to manually destroy our fake "trivial" object */
        new (&lval) FlatSharedStr(move(new_str));
    }
}

void TypeStr::add(EvalValue &a, const EvalValue &b)
{
    FlatSharedStr &lval = a.get<FlatSharedStr>();

    if (b.is<FlatSharedStr>())
        append(lval, b.get<FlatSharedStr>().get_view());
    else
        append(lval, b.get_type()->to_string(b));
}

void TypeStr::mul(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    string new_str;
    const string_view &s = a.get<FlatSharedStr>().get_view();
    const long n = b.get<long>();

    if (n >= 0) {
        new_str.reserve(s.size() * n);

        for (long i = 0; i < n; i++)
            new_str += s;
    }

    a = FlatSharedStr(move(new_str));
}

void TypeStr::lt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<FlatSharedStr>().get_view() < b.get<FlatSharedStr>().get_view()
    );
}

void TypeStr::gt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<FlatSharedStr>().get_view() > b.get<FlatSharedStr>().get_view()
    );
}

void TypeStr::le(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<FlatSharedStr>().get_view() <= b.get<FlatSharedStr>().get_view()
    );
}

void TypeStr::ge(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<FlatSharedStr>().get_view() >= b.get<FlatSharedStr>().get_view()
    );
}

void TypeStr::eq(EvalValue &a, const EvalValue &b)
{
    if (b.is<FlatSharedStr>()) {

        a = a.get<FlatSharedStr>().get_view() == b.get<FlatSharedStr>().get_view();

    } else {

        a = false;
    }
}

void TypeStr::noteq(EvalValue &a, const EvalValue &b)
{
    if (b.is<FlatSharedStr>()) {

        a = a.get<FlatSharedStr>().get_view() != b.get<FlatSharedStr>().get_view();

    } else {

        a = true;
    }
}

EvalValue TypeStr::subscript(const EvalValue &what_lval, const EvalValue &idx_val)
{
    if (!idx_val.is<long>())
        throw TypeErrorEx();

    const EvalValue &what = RValue(what_lval);
    const FlatSharedStr &s = what.get<FlatSharedStr>();
    long idx = idx_val.get<long>();

    if (idx < 0)
        idx += s.size();

    if (idx < 0 || idx >= s.size())
        throw OutOfBoundsEx();

    return FlatSharedStr(s, s.offset() + idx, 1);
}

EvalValue TypeStr::slice(const EvalValue &what_lval,
                         const EvalValue &start_val,
                         const EvalValue &end_val)
{
    const EvalValue &what = RValue(what_lval);
    const FlatSharedStr &s = what.get<FlatSharedStr>();
    long start = 0, end = s.size();

    if (start_val.is<long>()) {

        start = start_val.get<long>();

        if (start < 0) {

            start += s.size();

            if (start < 0)
                start = 0;
        }

        if (start >= s.size())
            return EvalValue::empty_str;

    } else if (!start_val.is<NoneVal>()) {

        throw TypeErrorEx();
    }

    if (end_val.is<long>()) {

        end = end_val.get<long>();

        if (end < 0)
            end += s.size();

        if (end <= start)
            return EvalValue::empty_str;

        if (end > s.size())
            end = s.size();

    } else if (!end_val.is<NoneVal>()) {

        throw TypeErrorEx();
    }

    return FlatSharedStr(s, s.offset() + start, end - start);
}
