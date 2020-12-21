/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "eval.h"
#include "evaltypes.cpp.h"
#include "syntax.h"

#include <cmath>

EvalValue builtin_int(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (val.is<long>()) {

        return val;

    } else if (val.is<long double>()) {

        return static_cast<long>(val.get<long double>());

    } else if (val.is<FlatSharedStr>()) {

        try {

            return stol(string(val.get<FlatSharedStr>().get_view()));

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

    if (val.is<long double>()) {

        return val;

    } else if (val.is<long>()) {

        return static_cast<long double>(val.get<long>());

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

    if (e.is<long>()) {

        const long val = e.get<long>();
        return val >= 0 ? val : -val;

    } else if (e.is<long double>()) {

        return fabsl(e.get<long double>());

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

        for (unsigned i = 1; i < arr_view.size(); i++) {

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

    for (unsigned i = 1; i < vec.size(); i++) {

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

template <unsigned N, typename funcT>
static EvalValue
float_func(EvalContext *ctx, ExprList *exprList, funcT f)
{
    if (exprList->elems.size() != N)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    long double x[N];

    for (unsigned i = 0; i < N; i++) {

        Construct *arg = exprList->elems[i].get();
        const EvalValue &v = RValue(arg->eval(ctx));

        if (v.is<long double>())

            x[i] = v.get<long double>();

        else if (v.is<long>())

            x[i] = static_cast<long double>(v.get<long>());

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
INST_FLOAT_BUILTIN_1_ex(isinf, int (*)(long double), isinfl);
INST_FLOAT_BUILTIN_1_ex(isfinite, bool (*)(long double), isfinite);
INST_FLOAT_BUILTIN_1_ex(isnormal, bool (*)(long double), isnormal);

EvalValue builtin_isnan(EvalContext *ctx, ExprList *exprList)
{
    /*
     * isnan() returns a non-zero value in case of NaN instead just returning
     * a boolean [0, 1]. Therefore, we need to manually convert it to a bool.
     */
    return !!float_func<1>(ctx, exprList, &isnanl).get<long>();
}

EvalValue builtin_round(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &v0 = RValue(arg0->eval(ctx));

    if (exprList->elems.size() == 1) {

        return roundl(v0.get<long double>());

    } else {

        if (exprList->elems.size() != 2)
            throw InvalidArgumentEx(exprList->start, exprList->end);

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &v1 = RValue(arg1->eval(ctx));

        if (!v1.is<long>() || v1.get<long>() < 0) {
            throw TypeErrorEx(
                "Expected a non-negative integer", arg1->start, arg1->end
            );
        }

        const long double base10exp = powl(10.0, v1.get<long>());
        return roundl(v0.get<long double>() * base10exp) / base10exp;
    }
}
