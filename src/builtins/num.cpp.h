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
    EvalValue right;

    if (arr.size() > 0) {

        right = vec[arr.offset()].get();

        for (unsigned i = 1; i < arr.size(); i++) {

            EvalValue left = vec[arr.offset() + i].get();

            if constexpr(is_max)
                left.get_type()->gt(left, right);   /* left = left > right */
            else
                left.get_type()->lt(left, right);   /* left = left < right */

            if (left.get<long>())
                right = vec[arr.offset() + i].get();
        }
    }

    return right;
}

template <bool is_max>
EvalValue b_min_max(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() == 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *first_arg = exprList->elems[0].get();
    EvalValue right = RValue(first_arg->eval(ctx));
    const auto &vec = exprList->elems;

    if (vec.size() == 1) {

        if (!right.is<FlatSharedArray>())
            throw TypeErrorEx(first_arg->start, first_arg->end);

        return b_min_max_arr<is_max>(right.get<FlatSharedArray>());
    }

    for (unsigned i = 1; i < vec.size(); i++) {

        EvalValue left_orig = RValue(vec[i]->eval(ctx));
        EvalValue left = left_orig;

        if constexpr(is_max)
            left.get_type()->gt(left, right);   /* left = left > right */
        else
            left.get_type()->lt(left, right);   /* left = left < right */

        if (left.get<long>())
            right = move(left_orig);
    }

    return right;
}

EvalValue builtin_min(EvalContext *ctx, ExprList *exprList)
{
    return b_min_max<false>(ctx, exprList);
}

EvalValue builtin_max(EvalContext *ctx, ExprList *exprList)
{
    return b_min_max<true>(ctx, exprList);
}
