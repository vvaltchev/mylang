/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "eval.h"
#include "evaltypes.cpp.h"
#include "syntax.h"

#include <random>
#include <cmath>

static std::random_device rdev;
static std::mt19937_64 mt_engine(rdev());


EvalValue builtin_int(EvalContext *ctx, const ArgLocs *exprList,
                      const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);
    const EvalValue &val = args[0];

    if (val.is<int_type>()) {

        return val;

    } else if (val.is<bool>()) {

        return static_cast<int_type>(val.get<bool>() ? 1 : 0);

    } else if (val.is<float_type>()) {

        return static_cast<int_type>(val.get<float_type>());

    } else if (val.is<SharedStr>()) {

        const string &strval = string(val.get<SharedStr>().get_view());

        try {

            if constexpr (sizeof(int_type) == sizeof(int))
               return static_cast<int_type>(stoi(strval));
            else if constexpr(sizeof(int_type) == sizeof(long))
               return static_cast<int_type>(stol(strval));
            else if constexpr(sizeof(int_type) == sizeof(long long))
               return static_cast<int_type>(stoll(strval));
            else
               assert(0);

        } catch (...) {

            throw TypeErrorEx("The string cannot be converted to integer", arg->start, arg->end);
        }

    } else {

        throw TypeErrorEx("Unsupported type for int()", arg->start, arg->end);
    }
}

EvalValue builtin_float(EvalContext *ctx, const ArgLocs *exprList,
                        const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);
    const EvalValue &val = args[0];

    if (val.is<float_type>()) {

        return val;

    } else if (val.is<int_type>()) {

        return static_cast<float_type>(val.get<int_type>());

    } else if (val.is<bool>()) {

        return static_cast<float_type>(val.get<bool>() ? 1 : 0);

    } else if (val.is<SharedStr>()) {

        try {

            return stod(string(val.get<SharedStr>().get_view()));

        } catch (...) {

            throw TypeErrorEx("The string cannot be converted to float", arg->start, arg->end);
        }

    } else {

        throw TypeErrorEx("Unsupported type for float()", arg->start, arg->end);
    }
}

EvalValue builtin_abs(EvalContext *ctx, const ArgLocs *exprList,
                      const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);
    const EvalValue &e = args[0];

    if (e.is<int_type>()) {

        const int_type val = e.get<int_type>();
        return val >= 0 ? val : -val;

    } else if (e.is<float_type>()) {

        return std::fabs(e.get<float_type>());

    } else {

        throw TypeErrorEx("Unsupported type for abs()", arg->start, arg->end);
    }
}

template <bool is_max>
EvalValue b_min_max_arr(const SharedArrayObj &arr)
{
    /* Flat fast path: scan the unboxed int/float vector directly, no promotion
     * and no per-element virtual compare (see plans/typed-arrays.md).
     * strs/structs take the general path below (a LOCAL handle promote -
     * min/max of strings compares lexically as before; the else-chain here
     * would misread their union member). */
    if (arr.skind() != SharedArrayObj::Storage::general
        && arr.skind() != SharedArrayObj::Storage::strs
        && arr.skind() != SharedArrayObj::Storage::structs) {

        const size_type n = arr.size(), off = arr.offset();

        if (n == 0)
            return EvalValue();

        if (arr.skind() == SharedArrayObj::Storage::ints) {
            const auto &iv = arr.flat_ints();
            int_type best = iv[off];
            for (size_type i = 1; i < n; i++) {
                const int_type x = iv[off + i];
                if (is_max ? (x > best) : (x < best))
                    best = x;
            }
            return EvalValue(best);
        }

        if (arr.skind() == SharedArrayObj::Storage::bools) {
            const auto &bv = arr.flat_bools();
            unsigned char best = bv[off];
            for (size_type i = 1; i < n; i++) {
                const unsigned char x = bv[off + i];
                if (is_max ? (x > best) : (x < best))
                    best = x;
            }
            return EvalValue(static_cast<bool>(best));   /* min/max stay bool */
        }

        const auto &fv = arr.flat_floats();
        float_type best = fv[off];
        for (size_type i = 1; i < n; i++) {
            const float_type x = fv[off + i];
            if (is_max ? (x > best) : (x < best))
                best = x;
        }
        return EvalValue(best);
    }

    /* strs/structs: promote a LOCAL handle copy (the caller's array keeps
     * its flat storage) and run the general compare loop. */
    SharedArrayObj marr = arr;
    if (marr.skind() != SharedArrayObj::Storage::general)
        (void)marr.get_vec();          /* promotes strs/structs in place */
    const ArrayConstView &arr_view = marr.get_view();
    EvalValue val;

    if (arr_view.size() > 0) {

        val = arr_view[0].get();

        for (size_type i = 1; i < arr_view.size(); i++) {

            const EvalValue &other = arr_view[i].get();

            if constexpr(is_max) {

                if (other > val)
                    val = other;

            } else {

                if (other < val)
                    val = other;
            }
        }
    }

    return val;
}

template <bool is_max>
EvalValue b_min_max(EvalContext *ctx, const ArgLocs *exprList,
                    const EvalValue *args, size_t n)
{
    if (n == 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *first_arg = exprList->arg(0);
    EvalValue val = args[0];

    if (n == 1) {

        if (!val.is<SharedArrayObj>()) {
            throw TypeErrorEx(
                "When a single argument is provided, it must be an array",
                first_arg->start,
                first_arg->end
            );
        }

        return b_min_max_arr<is_max>(val.get<SharedArrayObj>());
    }

    for (size_type i = 1; i < n; i++) {

        const EvalValue &other = args[i];

        if constexpr(is_max) {

            if (other > val)
                val = other;

        } else {

            if (other < val)
                val = other;
        }
    }

    return val;
}

EvalValue builtin_min(EvalContext *ctx, const ArgLocs *exprList,
                      const EvalValue *args, size_t n)
{
    return b_min_max<false>(ctx, exprList, args, n);
}

EvalValue builtin_max(EvalContext *ctx, const ArgLocs *exprList,
                      const EvalValue *args, size_t n)
{
    return b_min_max<true>(ctx, exprList, args, n);
}

template <size_type N, typename funcT>
static EvalValue
float_func(EvalContext *ctx, const ArgLocs *exprList,
           const EvalValue *args, size_t n, funcT f)
{
    if (n != N)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    float_type x[N];

    for (size_type i = 0; i < N; i++) {

        const ArgLoc *arg = exprList->arg(i);
        const EvalValue &v = args[i];

        if (v.is<float_type>())

            x[i] = v.get<float_type>();

        else if (v.is<int_type>())

            x[i] = static_cast<float_type>(v.get<int_type>());

        else

            throw TypeErrorEx("Expected numeric type", arg->start, arg->end);
    }

    if constexpr(N == 1)
        return f(x[0]);
    else if constexpr(N == 2)
        return f(x[0], x[1]);
    else
        /* We should never get here */
        assert(0);
}

/* The std::<name> overload for float_type (double), selected by an explicit
 * function-pointer cast (the name is overloaded for float/double/long dbl). */
#define INST_FLOAT_BUILTIN_1(name)                                      \
    EvalValue builtin_##name(EvalContext *ctx, const ArgLocs *exprList,      \
                             const EvalValue *args, size_t n) {         \
        return float_func<1>(ctx, exprList, args, n,                   \
            static_cast<float_type (*)(float_type)>(std::name));        \
    }

#define INST_FLOAT_BUILTIN_1_ex(name, funcT, funcName)                  \
    EvalValue builtin_##name(EvalContext *ctx, const ArgLocs *exprList,      \
                             const EvalValue *args, size_t n) {         \
        return float_func<1, funcT>(ctx, exprList, args, n, funcName);  \
    }

#define INST_FLOAT_BUILTIN_2(name)                                      \
    EvalValue builtin_##name(EvalContext *ctx, const ArgLocs *exprList,      \
                             const EvalValue *args, size_t n) {         \
        return float_func<2>(ctx, exprList, args, n,                   \
            static_cast<float_type (*)(float_type, float_type)>(std::name)); \
    }

INST_FLOAT_BUILTIN_1(exp);
INST_FLOAT_BUILTIN_1(exp2);
INST_FLOAT_BUILTIN_1(log);
INST_FLOAT_BUILTIN_1(log2);
INST_FLOAT_BUILTIN_1(log10);
INST_FLOAT_BUILTIN_1(sqrt);
INST_FLOAT_BUILTIN_1(cbrt);
INST_FLOAT_BUILTIN_2(pow);
INST_FLOAT_BUILTIN_1(sin);
INST_FLOAT_BUILTIN_1(cos);
INST_FLOAT_BUILTIN_1(tan);
INST_FLOAT_BUILTIN_1(asin);
INST_FLOAT_BUILTIN_1(acos);
INST_FLOAT_BUILTIN_1(atan);
INST_FLOAT_BUILTIN_1(ceil);
INST_FLOAT_BUILTIN_1(floor);
INST_FLOAT_BUILTIN_1(trunc);
INST_FLOAT_BUILTIN_1_ex(isinf, bool (*)(float_type), std::isinf);
INST_FLOAT_BUILTIN_1_ex(isfinite, bool (*)(float_type), std::isfinite);
INST_FLOAT_BUILTIN_1_ex(isnormal, bool (*)(float_type), std::isnormal);
INST_FLOAT_BUILTIN_1_ex(isnan, bool (*)(float_type), std::isnan);

EvalValue builtin_round(EvalContext *ctx, const ArgLocs *exprList,
                        const EvalValue *args, size_t n)
{
    if (n < 1)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const EvalValue &v0 = args[0];
    float_type x;

    if (v0.is<float_type>())
        x = v0.get<float_type>();
    else if (v0.is<int_type>())
        x = static_cast<float_type>(v0.get<int_type>());
    else
        throw TypeErrorEx("Expected numeric type", arg0->start, arg0->end);

    if (n == 1) {

        return std::round(x);

    } else {

        if (n != 2)
            throw InvalidArgumentEx(exprList->start, exprList->end);

        const ArgLoc *arg1 = exprList->arg(1);
        const EvalValue &v1 = args[1];

        if (!v1.is<int_type>() || v1.get<int_type>() < 0) {
            throw TypeErrorEx(
                "Expected a non-negative integer", arg1->start, arg1->end
            );
        }

        const float_type base10exp = std::pow(
            10.0,
            static_cast<float_type>(v1.get<int_type>())
        );
        return std::round(x * base10exp) / base10exp;
    }
}

EvalValue builtin_rand(EvalContext *ctx, const ArgLocs *exprList,
                       const EvalValue *args, size_t n)
{
    if (n != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const ArgLoc *arg1 = exprList->arg(1);
    const EvalValue &v0 = args[0];
    const EvalValue &v1 = args[1];

    if (!v0.is<int_type>())
        throw TypeErrorEx("Expected integer", arg0->start, arg0->end);

    if (!v1.is<int_type>())
        throw TypeErrorEx("Expected integer", arg1->start, arg1->end);

    if (v1.get<int_type>() < v0.get<int_type>())
        return none;

    if (v0.get<int_type>() == v1.get<int_type>())
        return v0;

    std::uniform_int_distribution<int_type> distrib(
        v0.get<int_type>(), v1.get<int_type>()
    );

    return distrib(mt_engine);
}

EvalValue builtin_randf(EvalContext *ctx, const ArgLocs *exprList,
                        const EvalValue *args, size_t n)
{
    if (n != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const ArgLoc *arg1 = exprList->arg(1);
    const EvalValue &v0 = args[0];
    const EvalValue &v1 = args[1];

    if (!v0.is<float_type>())
        throw TypeErrorEx("Expected float", arg0->start, arg0->end);

    if (!v1.is<float_type>())
        throw TypeErrorEx("Expected float", arg1->start, arg1->end);

    if (v1.get<float_type>() < v0.get<float_type>())
        return none;

    if (v0.get<float_type>() == v1.get<float_type>())
        return v0;

    std::uniform_real_distribution<float_type> distrib(
        v0.get<float_type>(), v1.get<float_type>()
    );

    return distrib(mt_engine);
}
