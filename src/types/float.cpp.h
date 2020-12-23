/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include <cmath>

class TypeFloat : public Type {

public:

    TypeFloat() : Type(Type::t_float) { }

    void add(EvalValue &a, const EvalValue &b) override;
    void sub(EvalValue &a, const EvalValue &b) override;
    void mul(EvalValue &a, const EvalValue &b) override;
    void div(EvalValue &a, const EvalValue &b) override;
    void mod(EvalValue &a, const EvalValue &b) override;
    void lt(EvalValue &a, const EvalValue &b) override;
    void gt(EvalValue &a, const EvalValue &b) override;
    void le(EvalValue &a, const EvalValue &b) override;
    void ge(EvalValue &a, const EvalValue &b) override;
    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    void opneg(EvalValue &a) override;

    bool is_true(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    size_t hash(const EvalValue &a) override;
};

inline float_type internal_val_to_float(const EvalValue &b)
{
    if (b.is<float_type>())
        return b.get<float_type>();

    if (b.is<int_type>())
        return b.get<int_type>();

    throw TypeErrorEx("Cannot convert right-side value to float");
}

void TypeFloat::add(EvalValue &a, const EvalValue &b)
{
    a.get<float_type>() += internal_val_to_float(b);
}

void TypeFloat::sub(EvalValue &a, const EvalValue &b)
{
    a.get<float_type>() -= internal_val_to_float(b);
}

void TypeFloat::mul(EvalValue &a, const EvalValue &b)
{
    a.get<float_type>() *= internal_val_to_float(b);
}

void TypeFloat::div(EvalValue &a, const EvalValue &b)
{
    float_type rhs = internal_val_to_float(b);

    if (iszero(rhs))
        throw DivisionByZeroEx();

    a.get<float_type>() /= rhs;
}

void TypeFloat::mod(EvalValue &a, const EvalValue &b)
{
    float_type rhs = internal_val_to_float(b);

    if (iszero(rhs))
        throw DivisionByZeroEx();

    a = fmodl(a.get<float_type>(), rhs);
}

void TypeFloat::lt(EvalValue &a, const EvalValue &b)
{
    a = a.get<float_type>() < internal_val_to_float(b);
}

void TypeFloat::gt(EvalValue &a, const EvalValue &b)
{
    a = a.get<float_type>() > internal_val_to_float(b);
}

void TypeFloat::le(EvalValue &a, const EvalValue &b)
{
    a = a.get<float_type>() <= internal_val_to_float(b);
}

void TypeFloat::ge(EvalValue &a, const EvalValue &b)
{
    a = a.get<float_type>() >= internal_val_to_float(b);
}

void TypeFloat::eq(EvalValue &a, const EvalValue &b)
{
    if (b.is<float_type>()) {

        a = a.get<float_type>() == b.get<float_type>();

    } else if (b.is<int_type>()) {

        a = a.get<float_type>() == b.get<int_type>();

    } else {

        a = false;
    }
}

void TypeFloat::noteq(EvalValue &a, const EvalValue &b)
{
    if (b.is<float_type>()) {

        a = a.get<float_type>() != b.get<float_type>();

    } else if (b.is<int_type>()) {

        a = a.get<float_type>() != b.get<int_type>();

    } else {

        a = true;
    }
}

void TypeFloat::opneg(EvalValue &a)
{
    a.get<float_type>() = -a.get<float_type>();
}

bool TypeFloat::is_true(const EvalValue &a)
{
    return a.get<float_type>() != 0.0;
}

string TypeFloat::to_string(const EvalValue &a) {
    return std::to_string(a.get<float_type>());
}

size_t TypeFloat::hash(const EvalValue &a)
{
    return std::hash<float_type>()(a.get<float_type>());
}
