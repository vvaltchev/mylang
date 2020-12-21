/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include <cmath>

class TypeFloat : public Type {

public:

    TypeFloat() : Type(Type::t_float) { }

    void add(EvalValue &a, const EvalValue &b) override;
    void sub(EvalValue &a, const EvalValue &b) override;
    void mul(EvalValue &a, const EvalValue &b) override;
    void div(EvalValue &a, const EvalValue &b) override;
    void lt(EvalValue &a, const EvalValue &b) override;
    void gt(EvalValue &a, const EvalValue &b) override;
    void le(EvalValue &a, const EvalValue &b) override;
    void ge(EvalValue &a, const EvalValue &b) override;
    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    void opneg(EvalValue &a) override;

    bool is_true(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
};

inline long double internal_val_to_float(const EvalValue &b)
{
    if (b.is<long double>())
        return b.get<long double>();

    if (b.is<long>())
        return b.get<long>();

    throw TypeErrorEx();
}

void TypeFloat::add(EvalValue &a, const EvalValue &b)
{
    a.get<long double>() += internal_val_to_float(b);
}

void TypeFloat::sub(EvalValue &a, const EvalValue &b)
{
    a.get<long double>() -= internal_val_to_float(b);
}

void TypeFloat::mul(EvalValue &a, const EvalValue &b)
{
    a.get<long double>() *= internal_val_to_float(b);
}

void TypeFloat::div(EvalValue &a, const EvalValue &b)
{
    long double rhs = internal_val_to_float(b);

    if (rhs == 0)
        throw DivisionByZeroEx();

    a.get<long double>() /= rhs;
}

void TypeFloat::lt(EvalValue &a, const EvalValue &b)
{
    a = a.get<long double>() < internal_val_to_float(b);
}

void TypeFloat::gt(EvalValue &a, const EvalValue &b)
{
    a = a.get<long double>() > internal_val_to_float(b);
}

void TypeFloat::le(EvalValue &a, const EvalValue &b)
{
    a = a.get<long double>() <= internal_val_to_float(b);
}

void TypeFloat::ge(EvalValue &a, const EvalValue &b)
{
    a = a.get<long double>() >= internal_val_to_float(b);
}

void TypeFloat::eq(EvalValue &a, const EvalValue &b)
{
    if (b.is<long double>()) {

        a = a.get<long double>() == b.get<long double>();

    } else if (b.is<long>()) {

        a = a.get<long double>() == b.get<long>();

    } else {

        a = false;
    }
}

void TypeFloat::noteq(EvalValue &a, const EvalValue &b)
{
    if (b.is<long double>()) {

        a = a.get<long double>() != b.get<long double>();

    } else if (b.is<long>()) {

        a = a.get<long double>() != b.get<long>();

    } else {

        a = true;
    }
}

void TypeFloat::opneg(EvalValue &a)
{
    a.get<long double>() = -a.get<long double>();
}

bool TypeFloat::is_true(const EvalValue &a)
{
    return a.get<long double>() != 0.0;
}

string TypeFloat::to_string(const EvalValue &a) {
    return std::to_string(a.get<long double>());
}
