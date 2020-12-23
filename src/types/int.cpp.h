/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include <functional>

class TypeInt : public Type {

public:

    TypeInt() : Type(Type::t_int) { }

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
    void land(EvalValue &a, const EvalValue &b) override;
    void lor(EvalValue &a, const EvalValue &b) override;

    bool is_true(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    size_t hash(const EvalValue &a) override;
};

void TypeInt::add(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() += b.get<int_type>();
}

void TypeInt::sub(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() -= b.get<int_type>();
}

void TypeInt::mul(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() *= b.get<int_type>();
}

void TypeInt::div(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    if (b.get<int_type>() == 0)
        throw DivisionByZeroEx();

    a.get<int_type>() /= b.get<int_type>();
}

void TypeInt::mod(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    if (b.get<int_type>() == 0)
        throw DivisionByZeroEx();

    a.get<int_type>() %= b.get<int_type>();
}

void TypeInt::lt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() = a.get<int_type>() < b.get<int_type>();
}

void TypeInt::gt(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() = a.get<int_type>() > b.get<int_type>();
}

void TypeInt::le(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() = a.get<int_type>() <= b.get<int_type>();
}

void TypeInt::ge(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() = a.get<int_type>() >= b.get<int_type>();
}

void TypeInt::eq(EvalValue &a, const EvalValue &b)
{
    if (b.is<int_type>()) {

        a.get<int_type>() = a.get<int_type>() == b.get<int_type>();

    } else {

        a = false;
    }
}

void TypeInt::noteq(EvalValue &a, const EvalValue &b)
{
    if (b.is<int_type>()) {

        a.get<int_type>() = a.get<int_type>() != b.get<int_type>();

    } else {

        a = true;
    }
}

void TypeInt::opneg(EvalValue &a)
{
    a.get<int_type>() = -a.get<int_type>();
}

void TypeInt::land(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() = a.get<int_type>() && b.get<int_type>();
}

void TypeInt::lor(EvalValue &a, const EvalValue &b)
{
    if (!b.is<int_type>())
        throw TypeErrorEx("Expected integer on the right side");

    a.get<int_type>() = a.get<int_type>() || b.get<int_type>();
}

bool TypeInt::is_true(const EvalValue &a)
{
    return a.get<int_type>() != 0;
}

string TypeInt::to_string(const EvalValue &a)
{
    return std::to_string(a.get<int_type>());
}

size_t TypeInt::hash(const EvalValue &a)
{
    return std::hash<int_type>()(a.get<int_type>());
}
