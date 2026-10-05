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
#include <limits>

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
        return static_cast<float_type>(b.get<int_type>());

    throw TypeErrorEx("Cannot convert right-side value to float");
}

void TypeFloat::add(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(int_vc_float_op(a.get<float_type>(), internal_val_to_float(b),
                                a.get<float_type>() +
                                internal_val_to_float(b));)
    a.get<float_type>() += internal_val_to_float(b);
}

void TypeFloat::sub(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(int_vc_float_op(a.get<float_type>(), internal_val_to_float(b),
                                a.get<float_type>() -
                                internal_val_to_float(b));)
    a.get<float_type>() -= internal_val_to_float(b);
}

void TypeFloat::mul(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(int_vc_float_op(a.get<float_type>(), internal_val_to_float(b),
                                a.get<float_type>() *
                                internal_val_to_float(b));)
    a.get<float_type>() *= internal_val_to_float(b);
}

void TypeFloat::div(EvalValue &a, const EvalValue &b)
{
    float_type rhs = internal_val_to_float(b);

    ML_INT_ONLY(int_vc_float_div0(false, rhs);)
    if (std::fpclassify(rhs) == FP_ZERO)
        throw DivisionByZeroEx();

    ML_INT_ONLY(int_vc_float_op(a.get<float_type>(), rhs,
                                a.get<float_type>() / rhs);)
    a.get<float_type>() /= rhs;
}

void TypeFloat::mod(EvalValue &a, const EvalValue &b)
{
    float_type rhs = internal_val_to_float(b);

    ML_INT_ONLY(int_vc_float_div0(true, rhs);)
    if (std::fpclassify(rhs) == FP_ZERO)
        throw DivisionByZeroEx();

    ML_INT_ONLY(int_vc_float_op(a.get<float_type>(), rhs,
                                std::fmod(a.get<float_type>(), rhs));)
    a = std::fmod(a.get<float_type>(), rhs);
}

void TypeFloat::lt(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(int_vc_float_cmp(a.get<float_type>(),
                                 internal_val_to_float(b));)
    a = a.get<float_type>() < internal_val_to_float(b);
}

void TypeFloat::gt(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(int_vc_float_cmp(a.get<float_type>(),
                                 internal_val_to_float(b));)
    a = a.get<float_type>() > internal_val_to_float(b);
}

void TypeFloat::le(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(int_vc_float_cmp(a.get<float_type>(),
                                 internal_val_to_float(b));)
    a = a.get<float_type>() <= internal_val_to_float(b);
}

void TypeFloat::ge(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(int_vc_float_cmp(a.get<float_type>(),
                                 internal_val_to_float(b));)
    a = a.get<float_type>() >= internal_val_to_float(b);
}

void TypeFloat::eq(EvalValue &a, const EvalValue &b)
{
    ML_INT_ONLY(if (b.is<float_type>() || b.is<int_type>())
                    int_vc_float_cmp(a.get<float_type>(),
                                     internal_val_to_float(b));)
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
    ML_INT_ONLY(if (b.is<float_type>() || b.is<int_type>())
                    int_vc_float_cmp(a.get<float_type>(),
                                     internal_val_to_float(b));)
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
    ML_INT_ONLY(int_vc_float_neg(a.get<float_type>());)
    a.get<float_type>() = -a.get<float_type>();
}

bool TypeFloat::is_true(const EvalValue &a)
{
    return a.get<float_type>() != 0.0;
}

/* A float as text, `precision` digits after the point (printf's %f, which
 * std::to_string used: 6). A NaN is `nan` whatever its sign bit - C
 * prints `-nan` for the x86 default NaN (inf - inf) and `nan` on ARM, and
 * the language must not depend on the platform. -0.0 and -inf keep their
 * sign: they are ordinary values. */
static string float_text(float_type v, int precision)
{
    ML_INT_ONLY(int_vc_float_text(v);)
    if (std::isnan(v))
        return "nan";
    const int n = snprintf(nullptr, 0, "%.*f", precision, v);
    if (n < 1)
        throw InternalErrorEx();
    std::vector<char> buf(static_cast<size_t>(n) + 1);
    snprintf(buf.data(), buf.size(), "%.*f", precision, v);
    return string(buf.data(), static_cast<size_t>(n));
}

string TypeFloat::to_string(const EvalValue &a) {
    return float_text(a.get<float_type>(), 6);
}

size_t TypeFloat::hash(const EvalValue &a)
{
    const float_type v = a.get<float_type>();
    const float_type int_min = static_cast<float_type>(
        std::numeric_limits<int_type>::min()
    );

    /*
     * An integer-valued float compares equal to the corresponding int
     * (e.g. 1.0 == 1), so it must hash the same; otherwise the two would
     * behave as distinct dictionary keys. int_min is -2^N (exactly
     * representable in floating point), so [int_min, -int_min) is exactly
     * the set of integral floats that fit in int_type.
     */
    if (std::isfinite(v) && v == std::trunc(v) && v >= int_min && v < -int_min)
        return std::hash<int_type>()(static_cast<int_type>(v));

    return std::hash<float_type>()(v);
}
