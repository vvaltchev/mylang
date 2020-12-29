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


EvalValue builtin_int(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (val.is<int_type>()) {

        return val;

    } else if (val.is<float_type>()) {

        return static_cast<int_type>(val.get<float_type>());

    } else if (val.is<FlatSharedStr>()) {

        try {

#ifndef _MSC_VER
            return stol(string(val.get<FlatSharedStr>().get_view()));
#else
            return stoll(string(val.get<FlatSharedStr>().get_view()));
#endif

        } catch (...) {

            throw TypeErrorEx("The string cannot be converted to integer", arg->start, arg->end);
        }

    } else {

        throw TypeErrorEx("Unsupported type for int()", arg->start, arg->end);
    }
}

EvalValue builtin_float(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (val.is<float_type>()) {

        return val;

    } else if (val.is<int_type>()) {

        return static_cast<float_type>(val.get<int_type>());

    } else if (val.is<FlatSharedStr>()) {

        try {

            return stold(string(val.get<FlatSharedStr>().get_view()));

        } catch (...) {

            throw TypeErrorEx("The string cannot be converted to float", arg->start, arg->end);
        }

    } else {

        throw TypeErrorEx("Unsupported type for float()", arg->start, arg->end);
    }
}

EvalValue builtin_abs(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (e.is<int_type>()) {

        const int_type val = e.get<int_type>();
        return val >= 0 ? val : -val;

    } else if (e.is<float_type>()) {

        return fabsl(e.get<float_type>());

    } else {

        throw TypeErrorEx("Unsupported type for abs()", arg->start, arg->end);
    }
}

template <bool is_max>
EvalValue b_min_max_arr(const FlatSharedArray &arr)
{
    const ArrayConstView &arr_view = arr.get_view();
    EvalValue val;

    if (arr.size() > 0) {

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
EvalValue b_min_max(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() == 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *first_arg = exprList->elems[0].get();
    EvalValue val = RValue(first_arg->eval(ctx));
    const auto &vec = exprList->elems;

    if (vec.size() == 1) {

        if (!val.is<FlatSharedArray>()) {
            throw TypeErrorEx(
                "When a single argument is provided, it must be an array",
                first_arg->start,
                first_arg->end
            );
        }

        return b_min_max_arr<is_max>(val.get<FlatSharedArray>());
    }

    for (size_type i = 1; i < vec.size(); i++) {

        const EvalValue &other = RValue(vec[i]->eval(ctx));

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

EvalValue builtin_min(EvalContext *ctx, ExprList *exprList)
{
    return b_min_max<false>(ctx, exprList);
}

EvalValue builtin_max(EvalContext *ctx, ExprList *exprList)
{
    return b_min_max<true>(ctx, exprList);
}

template <size_type N, typename funcT>
static EvalValue
float_func(EvalContext *ctx, ExprList *exprList, funcT f)
{
    if (exprList->elems.size() != N)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    float_type x[N];

    for (size_type i = 0; i < N; i++) {

        Construct *arg = exprList->elems[i].get();
        const EvalValue &v = RValue(arg->eval(ctx));

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

#define INST_FLOAT_BUILTIN_1(name)                                      \
    EvalValue builtin_##name(EvalContext *ctx, ExprList *exprList) {    \
        return float_func<1>(ctx, exprList, &name##l);                  \
    }

#define INST_FLOAT_BUILTIN_1_ex(name, funcT, funcName)                  \
    EvalValue builtin_##name(EvalContext *ctx, ExprList *exprList) {    \
        return float_func<1, funcT>(ctx, exprList, funcName);           \
    }

#define INST_FLOAT_BUILTIN_2(name)                                      \
    EvalValue builtin_##name(EvalContext *ctx, ExprList *exprList) {    \
        return float_func<2>(ctx, exprList, &name##l);                  \
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

EvalValue builtin_round(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &v0 = RValue(arg0->eval(ctx));

    if (exprList->elems.size() == 1) {

        return roundl(v0.get<float_type>());

    } else {

        if (exprList->elems.size() != 2)
            throw InvalidArgumentEx(exprList->start, exprList->end);

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &v1 = RValue(arg1->eval(ctx));

        if (!v1.is<int_type>() || v1.get<int_type>() < 0) {
            throw TypeErrorEx(
                "Expected a non-negative integer", arg1->start, arg1->end
            );
        }

        const float_type base10exp = powl(
            10.0,
            static_cast<float_type>(v1.get<int_type>())
        );
        return roundl(v0.get<float_type>() * base10exp) / base10exp;
    }
}

EvalValue builtin_rand(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &v0 = RValue(arg0->eval(ctx));
    const EvalValue &v1 = RValue(arg1->eval(ctx));

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

EvalValue builtin_randf(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &v0 = RValue(arg0->eval(ctx));
    const EvalValue &v1 = RValue(arg1->eval(ctx));

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
