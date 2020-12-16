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

EvalValue builtin_abs(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<long>())
        throw TypeErrorEx(arg->start, arg->end);

    const long val = e.get<long>();
    return val >= 0 ? val : -val;
}

template <bool is_max>
EvalValue b_min_max_arr(const FlatSharedArray &arr)
{
    const FlatSharedArray::vec_type &vec = arr.get_ref();
    EvalValue val;

    if (arr.size() > 0) {

        val = vec[arr.offset()].get();

        for (unsigned i = 1; i < arr.size(); i++) {

            const EvalValue &other = vec[arr.offset() + i].get();

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
