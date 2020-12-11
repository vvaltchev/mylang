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

void SharedStr::append(const string_view &s)
{
    if (use_count() == 1 && off == 0 && len == get_ref().size()) {

        get_ref() += s;
        len += s.length();

    } else {

        string new_str;
        new_str.reserve(len + s.size());
        new_str += get_view();
        new_str += s;

        str = SharedVal<string>(make_shared<string>(move(new_str)));
        len = get_ref().size();
    }
}

class TypeStr : public SharedType<SharedStr> {

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

    virtual long len(const EvalValue &a) {
        return a.get<SharedStr>().size();
    }

    virtual string to_string(const EvalValue &a) {
        return string(a.get<SharedStr>().get_view());
    }
};

void TypeStr::add(EvalValue &a, const EvalValue &b)
{
    SharedStr &lval = a.get<SharedStr>();

    if (b.is<SharedStr>())
        lval.append(b.get<SharedStr>().get_view());
    else
        lval.append(b.get_type()->to_string(b));
}

void TypeStr::mul(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    string new_str;
    const string_view &s = a.get<SharedStr>().get_view();
    const long n = b.get<long>();
    new_str.reserve(s.size() * n);

    for (long i = 0; i < n; i++) {
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
     * POD type, contaning the data of a non-trivial C++ type.
     */
    copy_ctor(&s2, &s);

    s2.off = idx;
    s2.len = 1;
    return s2;
}
