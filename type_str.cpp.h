/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "evaltypes.cpp.h"

SharedStr::SharedStr(string &&s)
    : str(make_shared<string>(move(s)))
    , off(0)
    , len(get_ref().size())
{
}

class TypeStr : public SharedType<SharedStr> {

    void append(SharedStr &lval, const string_view &s);

public:

    TypeStr() : SharedType<SharedStr>(Type::t_str) { }

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

    virtual long len(const EvalValue &a) {
        return a.get<SharedStr>().size();
    }

    virtual string to_string(const EvalValue &a) {
        return string(a.get<SharedStr>().get_view());
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
    return a.get<SharedStr>().use_count();
}

void TypeStr::append(SharedStr &lval, const string_view &s)
{
    if (lval.off == 0 && lval.len == lval.get_ref().size()) {

        lval.get_ref() += s;
        lval.len += s.length();

    } else {

        string new_str;
        new_str.reserve(lval.len + s.size());
        new_str += lval.get_view();
        new_str += s;

        dtor(&lval.str); /* We have to manually destroy our fake "trivial" object */
        lval.str = make_shared<string>(move(new_str));
        lval.off = 0;
        lval.len = lval.get_ref().size();
    }
}

void TypeStr::add(EvalValue &a, const EvalValue &b)
{
    SharedStr &lval = a.get<SharedStr>();

    if (b.is<SharedStr>())
        append(lval, b.get<SharedStr>().get_view());
    else
        append(lval, b.get_type()->to_string(b));
}

void TypeStr::mul(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    string new_str;
    const string_view &s = a.get<SharedStr>().get_view();
    const long n = b.get<long>();

    if (n >= 0) {
        new_str.reserve(s.size() * n);

        for (long i = 0; i < n; i++)
            new_str += s;
    }

    a = SharedStr(move(new_str));
}

void TypeStr::lt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStr>().get_view() < b.get<SharedStr>().get_view()
    );
}

void TypeStr::gt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStr>().get_view() > b.get<SharedStr>().get_view()
    );
}

void TypeStr::le(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStr>().get_view() <= b.get<SharedStr>().get_view()
    );
}

void TypeStr::ge(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStr>().get_view() >= b.get<SharedStr>().get_view()
    );
}

void TypeStr::eq(EvalValue &a, const EvalValue &b)
{
    if (b.is<SharedStr>()) {

        a = a.get<SharedStr>().get_view() == b.get<SharedStr>().get_view();

    } else {

        a = false;
    }
}

void TypeStr::noteq(EvalValue &a, const EvalValue &b)
{
    if (b.is<SharedStr>()) {

        a = a.get<SharedStr>().get_view() != b.get<SharedStr>().get_view();

    } else {

        a = true;
    }
}

EvalValue TypeStr::subscript(const EvalValue &what, const EvalValue &idx_val)
{
    if (!idx_val.is<long>())
        throw TypeErrorEx();

    const SharedStr &s = what.get<SharedStr>();
    long idx = idx_val.get<long>();

    if (idx < 0)
        idx += s.size();

    if (idx < 0 || idx >= s.size())
        throw OutOfBoundsEx();

    SharedStr s2;

    /*
     * Of course, we have to manually call the copy ctor because SharedStr is
     * a POD type, contaning the data of a non-trivial C++ type and, we cannot
     * implement the copy ctor, move ctor etc. in SharedStr, otherwise it won't
     * be a POD type anymore, so it won't be accepted in EvalValue's union.
     */
    copy_ctor(&s2, &s);

    s2.off += idx;
    s2.len = 1;
    return s2;
}

EvalValue TypeStr::slice(const EvalValue &what,
                         const EvalValue &start_val,
                         const EvalValue &end_val)
{
    const SharedStr &s = what.get<SharedStr>();
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

    SharedStr s2;
    copy_ctor(&s2, &s); /* See TypeStr::subscript */
    s2.off += start;
    s2.len = end - start;
    return s2;
}
