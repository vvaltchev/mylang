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

EvalValue builtin_array(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<long>())
        throw TypeErrorEx(arg->start, arg->end);

    const long n = e.get<long>();

    if (n < 0)
        throw TypeErrorEx(arg->start, arg->end);

    FlatSharedArray::vec_type vec;

    for (long i = 0; i < n; i++)
        vec.emplace_back(EvalValue(), ctx->const_ctx);

    return FlatSharedArray(move(vec));
}
