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

class TypeStr : public TypeImpl<SharedStr> {

    void append(SharedStr &lval, const string_view &s);

public:

    TypeStr() : TypeImpl<SharedStr>(Type::t_str) { }

    void add(EvalValue &a, const EvalValue &b) override;
    void mul(EvalValue &a, const EvalValue &b) override;
    void lt(EvalValue &a, const EvalValue &b) override;
    void gt(EvalValue &a, const EvalValue &b) override;
    void le(EvalValue &a, const EvalValue &b) override;
    void ge(EvalValue &a, const EvalValue &b) override;
    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    EvalValue subscript(const EvalValue &what, const EvalValue &idx) override;
    EvalValue slice(const EvalValue &what,
                    const EvalValue &start,
                    const EvalValue &end) override;

    int_type use_count(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    bool is_slice(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;

    bool is_true(const EvalValue &a) override {
        return a.get<SharedStr>().size() > 0;
    }

    int_type len(const EvalValue &a) override {
        return a.get<SharedStr>().size();
    }

    string to_string(const EvalValue &a) override {
        return string(a.get<SharedStr>().get_view());
    }

    size_t hash(const EvalValue &a) override;
};

EvalValue TypeStr::clone(const EvalValue &a)
{
    const SharedStr &s = a.get<SharedStr>();

    if (s.is_slice())
        return s;

    return SharedStr(string(s.get_view()));
}

int_type TypeStr::use_count(const EvalValue &a)
{
    return a.get<SharedStr>().use_count();
}

bool TypeStr::is_slice(const EvalValue &a)
{
    return a.get<SharedStr>().is_slice();
}

EvalValue TypeStr::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(&a.get<SharedStr>().get_ref());
}

void TypeStr::append(SharedStr &lval, const string_view &s)
{
    if (!lval.is_slice()) {

        lval.get_ref() += s;

    } else {

        string new_str;
        new_str.reserve(lval.size() + s.size());
        new_str += lval.get_view();
        new_str += s;

        dtor(&lval); /* We have to manually destroy our fake "trivial" object */
        new (&lval) SharedStr(move(new_str));
    }
}

void TypeStr::add(EvalValue &a, const EvalValue &b)
{
    SharedStr &lval = a.get<SharedStr>();

    if (b.is<SharedStr>())
        append(lval, b.get<SharedStr>().get_view());
    else
        append(lval, b.to_string());
}

void TypeStr::mul(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected an integer on the right side");

    string new_str;
    const string_view &s = a.get<SharedStr>().get_view();
    const int_type n = b.get<int_type>();

    if (n >= 0) {
        new_str.reserve(s.size() * n);

        for (int_type i = 0; i < n; i++)
            new_str += s;
    }

    a = SharedStr(move(new_str));
}

void TypeStr::lt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx("Expected a string on the right side");

    a = EvalValue(
        a.get<SharedStr>().get_view() < b.get<SharedStr>().get_view()
    );
}

void TypeStr::gt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx("Expected a string on the right side");

    a = EvalValue(
        a.get<SharedStr>().get_view() > b.get<SharedStr>().get_view()
    );
}

void TypeStr::le(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx("Expected a string on the right side");

    a = EvalValue(
        a.get<SharedStr>().get_view() <= b.get<SharedStr>().get_view()
    );
}

void TypeStr::ge(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedStr>())
        throw TypeErrorEx("Expected a string on the right side");

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

EvalValue TypeStr::subscript(const EvalValue &what_lval, const EvalValue &idx_val)
{
    if (!idx_val.is<int_type>())
        throw TypeErrorEx("Expected an integer as subscript");

    const EvalValue &what = RValue(what_lval);
    const SharedStr &s = what.get<SharedStr>();
    int_type idx = idx_val.get<int_type>();

    if (idx < 0)
        idx += s.size();

    if (idx < 0 || static_cast<size_t>(idx) >= s.size())
        throw OutOfBoundsEx();

    return SharedStr(s, s.offset() + idx, 1);
}

EvalValue TypeStr::slice(const EvalValue &what_lval,
                         const EvalValue &start_val,
                         const EvalValue &end_val)
{
    const EvalValue &what = RValue(what_lval);
    const SharedStr &s = what.get<SharedStr>();
    int_type start = 0, end = s.size();

    if (start_val.is<int_type>()) {

        start = start_val.get<int_type>();

        if (start < 0) {

            start += s.size();

            if (start < 0)
                start = 0;
        }

        if (static_cast<size_t>(start) >= s.size())
            return empty_str;

    } else if (!start_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range start");
    }

    if (end_val.is<int_type>()) {

        end = end_val.get<int_type>();

        if (end < 0)
            end += s.size();

        if (end <= start)
            return empty_str;

        if (static_cast<size_t>(end) > s.size())
            end = s.size();

    } else if (!end_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range end");
    }

    return SharedStr(s, s.offset() + start, end - start);
}

size_t TypeStr::hash(const EvalValue &a)
{
    return std::hash<string_view>()(a.get<SharedStr>().get_view());
}
