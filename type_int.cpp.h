/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#include "evalvalue.h"

class TypeInt : public Type {

public:

    TypeInt() : Type(Type::t_int) { }

    virtual void add(EvalValue &a, EvalValue b);
    virtual void sub(EvalValue &a, EvalValue b);
    virtual void mul(EvalValue &a, EvalValue b);
    virtual void div(EvalValue &a, EvalValue b);
    virtual void mod(EvalValue &a, EvalValue b);
    virtual void lt(EvalValue &a, EvalValue b);
    virtual void gt(EvalValue &a, EvalValue b);
    virtual void le(EvalValue &a, EvalValue b);
    virtual void ge(EvalValue &a, EvalValue b);
    virtual void eq(EvalValue &a, EvalValue b);
    virtual void noteq(EvalValue &a, EvalValue b);
    virtual void opneg(EvalValue &a);
    virtual void lnot(EvalValue &a);
    virtual void land(EvalValue &a, EvalValue b);
    virtual void lor(EvalValue &a, EvalValue b);

    virtual bool is_true(const EvalValue &a);
    virtual string to_string(const EvalValue &a);
};

void TypeInt::add(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival += b.val.ival;
}

void TypeInt::sub(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival -= b.val.ival;
}

void TypeInt::mul(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival *= b.val.ival;
}

void TypeInt::div(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    if (b.val.ival == 0)
        throw DivisionByZeroEx();

    a.val.ival /= b.val.ival;
}

void TypeInt::mod(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival %= b.val.ival;
}

void TypeInt::lt(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival < b.val.ival;
}

void TypeInt::gt(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival > b.val.ival;
}

void TypeInt::le(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival <= b.val.ival;
}

void TypeInt::ge(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival >= b.val.ival;
}

void TypeInt::eq(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival == b.val.ival;
}

void TypeInt::noteq(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival != b.val.ival;
}

void TypeInt::opneg(EvalValue &a)
{
    a.val.ival = -a.val.ival;
}

void TypeInt::lnot(EvalValue &a)
{
    a.val.ival = !a.val.ival;
}

void TypeInt::land(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival && b.val.ival;
}

void TypeInt::lor(EvalValue &a, EvalValue b)
{
    if (!b.is<long>())
        throw TypeErrorEx();

    a.val.ival = a.val.ival || b.val.ival;
}

bool TypeInt::is_true(const EvalValue &a)
{
    return a.val.ival != 0;
}

string TypeInt::to_string(const EvalValue &a) {
    return std::to_string(a.val.ival);
}
