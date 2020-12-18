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

EvalValue builtin_append(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &arr_lval = arg0->eval(ctx);
    const EvalValue &elem = RValue(arg1->eval(ctx));

    if (!arr_lval.is<LValue *>())
        throw NotLValueEx(arg0->start, arg0->end);

    LValue *lval = arr_lval.get<LValue *>();

    if (!lval->is<FlatSharedArray>())
        throw TypeErrorEx(arg0->start, arg0->end);

    FlatSharedArray &arr = lval->getval<FlatSharedArray>();

    if (arr.is_slice())
        arr.clone_internal_vec();

    arr.get_ref().emplace_back(elem, ctx->const_ctx);
    return lval->get();
}

EvalValue builtin_pop(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &arr_lval = arg->eval(ctx);

    if (!arr_lval.is<LValue *>())
        throw NotLValueEx(arg->start, arg->end);

    LValue *lval = arr_lval.get<LValue *>();

    if (!lval->is<FlatSharedArray>())
        throw TypeErrorEx(arg->start, arg->end);

    FlatSharedArray &arr = lval->getval<FlatSharedArray>();
    const ArrayConstView &view = arr.get_view();

    if (!view.size())
        throw OutOfBoundsEx(arg->start, arg->end);

    EvalValue last = view[view.size() - 1].get();

    if (arr.is_slice()) {

        lval->put(FlatSharedArray(arr, arr.offset(), arr.size() - 1));

    } else {

        arr.clone_aliased_slices(arr.offset() + arr.size() - 1);
        arr.get_ref().pop_back();
    }

    return last;
}

EvalValue builtin_top(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<FlatSharedArray>())
        throw TypeErrorEx(arg->start, arg->end);

    const ArrayConstView &view = e.get<FlatSharedArray>().get_view();

    if (!view.size())
        throw OutOfBoundsEx(arg->start, arg->end);

    return view[view.size() - 1].get();
}

EvalValue builtin_erase_arr(LValue *lval, long index)
{
    FlatSharedArray &arr = lval->getval<FlatSharedArray>();
    const ArrayConstView &view = arr.get_view();

    if (!view.size())
        throw OutOfBoundsEx();

    if (index < 0)
        throw OutOfBoundsEx();

    if (arr.is_slice()) {

        if (index == 0) {

            lval->put(FlatSharedArray(arr, arr.offset() + 1, arr.size() - 1));

        } else if (index == view.size() - 1) {

            lval->put(FlatSharedArray(arr, arr.offset(), arr.size() - 1));

        } else {

            arr.clone_internal_vec();
            arr.get_ref().erase(arr.get_ref().begin() + arr.offset() + index);
            lval->put(FlatSharedArray(arr));
        }

    } else {

        arr.clone_aliased_slices(arr.offset() + arr.size() - 1);
        arr.get_ref().erase(arr.get_ref().begin() + arr.offset() + index);
    }

    return EvalValue();
}
