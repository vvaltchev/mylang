/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "evaltypes.cpp.h"

class TypeStr : public SharedType<SharedStrWrapper> {

public:

    TypeStr() : SharedType<SharedStrWrapper>(Type::t_str) { }

    virtual void add(EvalValue &a, EvalValue b);
    virtual void lt(EvalValue &a, EvalValue b);
    virtual void gt(EvalValue &a, EvalValue b);
    virtual void le(EvalValue &a, EvalValue b);
    virtual void ge(EvalValue &a, EvalValue b);
    virtual void eq(EvalValue &a, EvalValue b);
    virtual void noteq(EvalValue &a, EvalValue b);

    virtual string to_string(const EvalValue &a) {
        return a.get<SharedStrWrapper>().get();
    }
};

void TypeStr::add(EvalValue &a, EvalValue b)
{
    if (!a.is<SharedStrWrapper>())
        throw TypeErrorEx();

    SharedStrWrapper &lval = a.get<SharedStrWrapper>();

    if (lval.use_count() > 1) {

        string new_str;

        if (b.is<SharedStrWrapper>())
            new_str = lval.get() + b.get<SharedStrWrapper>().get();
        else
            new_str = lval.get() + b.get_type()->to_string(b);

        a = SharedStrWrapper(make_shared<string>(move(new_str)));

    } else {

        if (b.is<SharedStrWrapper>())
            lval.get() += b.get<SharedStrWrapper>().get();
        else
            lval.get() += b.get_type()->to_string(b);
    }
}

void TypeStr::lt(EvalValue &a, EvalValue b)
{
    if (!a.is<SharedStrWrapper>() || !b.is<SharedStrWrapper>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStrWrapper>().get() < b.get<SharedStrWrapper>().get()
    );
}

void TypeStr::gt(EvalValue &a, EvalValue b)
{
    if (!a.is<SharedStrWrapper>() || !b.is<SharedStrWrapper>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStrWrapper>().get() > b.get<SharedStrWrapper>().get()
    );
}

void TypeStr::le(EvalValue &a, EvalValue b)
{
    if (!a.is<SharedStrWrapper>() || !b.is<SharedStrWrapper>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStrWrapper>().get() <= b.get<SharedStrWrapper>().get()
    );
}

void TypeStr::ge(EvalValue &a, EvalValue b)
{
    if (!a.is<SharedStrWrapper>() || !b.is<SharedStrWrapper>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStrWrapper>().get() >= b.get<SharedStrWrapper>().get()
    );
}

void TypeStr::eq(EvalValue &a, EvalValue b)
{
    if (!a.is<SharedStrWrapper>() || !b.is<SharedStrWrapper>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStrWrapper>().get() == b.get<SharedStrWrapper>().get()
    );
}

void TypeStr::noteq(EvalValue &a, EvalValue b)
{
    if (!a.is<SharedStrWrapper>() || !b.is<SharedStrWrapper>())
        throw TypeErrorEx();

    a = EvalValue(
        a.get<SharedStrWrapper>().get() != b.get<SharedStrWrapper>().get()
    );
}
