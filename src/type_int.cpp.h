/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"

class TypeInt : public Type {

public:

    TypeInt() : Type(Type::t_int) { }

    virtual void add(EvalValue &a, const EvalValue &b);
    virtual void sub(EvalValue &a, const EvalValue &b);
    virtual void mul(EvalValue &a, const EvalValue &b);
    virtual void div(EvalValue &a, const EvalValue &b);
    virtual void mod(EvalValue &a, const EvalValue &b);
    virtual void lt(EvalValue &a, const EvalValue &b);
    virtual void gt(EvalValue &a, const EvalValue &b);
    virtual void le(EvalValue &a, const EvalValue &b);
    virtual void ge(EvalValue &a, const EvalValue &b);
    virtual void eq(EvalValue &a, const EvalValue &b);
    virtual void noteq(EvalValue &a, const EvalValue &b);
    virtual void opneg(EvalValue &a);
    virtual void lnot(EvalValue &a);
    virtual void land(EvalValue &a, const EvalValue &b);
    virtual void lor(EvalValue &a, const EvalValue &b);

    virtual bool is_true(const EvalValue &a);
    virtual string to_string(const EvalValue &a);
};

void TypeInt::add(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() += b.get<long>();
}

void TypeInt::sub(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() -= b.get<long>();
}

void TypeInt::mul(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() *= b.get<long>();
}

void TypeInt::div(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    if (b.get<long>() == 0)
        throw DivisionByZeroEx();

    a.get<long>() /= b.get<long>();
}

void TypeInt::mod(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() %= b.get<long>();
}

void TypeInt::lt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() = a.get<long>() < b.get<long>();
}

void TypeInt::gt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() = a.get<long>() > b.get<long>();
}

void TypeInt::le(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() = a.get<long>() <= b.get<long>();
}

void TypeInt::ge(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() = a.get<long>() >= b.get<long>();
}

void TypeInt::eq(EvalValue &a, const EvalValue &b)
{
    if (b.is<long>()) {

        a.get<long>() = a.get<long>() == b.get<long>();

    } else {

        a = false;
    }
}

void TypeInt::noteq(EvalValue &a, const EvalValue &b)
{
    if (b.is<long>()) {

        a.get<long>() = a.get<long>() != b.get<long>();

    } else {

        a = true;
    }
}

void TypeInt::opneg(EvalValue &a)
{
    a.get<long>() = -a.get<long>();
}

void TypeInt::lnot(EvalValue &a)
{
    a.get<long>() = !a.get<long>();
}

void TypeInt::land(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() = a.get<long>() && b.get<long>();
}

void TypeInt::lor(EvalValue &a, const EvalValue &b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.get<long>() = a.get<long>() || b.get<long>();
}

bool TypeInt::is_true(const EvalValue &a)
{
    return a.get<long>() != 0;
}

string TypeInt::to_string(const EvalValue &a) {
    return std::to_string(a.get<long>());
}
