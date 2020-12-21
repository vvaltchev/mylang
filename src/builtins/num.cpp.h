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

            throw TypeErrorEx(arg->start, arg->end);
        }

    } else {

        throw TypeErrorEx(arg->start, arg->end);
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

            throw TypeErrorEx(arg->start, arg->end);
        }

    } else {

        throw TypeErrorEx(arg->start, arg->end);
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

        throw TypeErrorEx(arg->start, arg->end);
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

        if (!val.is<FlatSharedArray>())
            throw TypeErrorEx(first_arg->start, first_arg->end);

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
