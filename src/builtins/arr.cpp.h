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

#include <algorithm>

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

EvalValue builtin_range(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1 || exprList->elems.size() > 3)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    long end, start = 0, step = 1;
    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<long>())
        throw TypeErrorEx(arg0->start, arg0->end);

    if (exprList->elems.size() >= 2) {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &val1 = RValue(arg1->eval(ctx));

        if (!val1.is<long>())
            throw TypeErrorEx(arg1->start, arg1->end);

        start = val0.get<long>();
        end = val1.get<long>();

        if (exprList->elems.size() == 3) {

            Construct *arg2 = exprList->elems[2].get();
            const EvalValue &val2 = RValue(arg2->eval(ctx));

            if (!val2.is<long>())
                throw TypeErrorEx(arg2->start, arg2->end);

            step = val2.get<long>();

            if (step == 0)
                throw TypeErrorEx(arg2->start, arg2->end);
        }

    } else {

        end = val0.get<long>();
    }

    FlatSharedArray::vec_type vec;

    if (step > 0) {

        for (long i = start; i < end; i += step)
            vec.emplace_back(EvalValue(i), ctx->const_ctx);

    } else {

        for (long i = start; i > end; i += step)
            vec.emplace_back(EvalValue(i), ctx->const_ctx);
    }

    return FlatSharedArray(move(vec));
}

EvalValue
builtin_find_arr(const FlatSharedArray &arr, const EvalValue &v)
{
    const ArrayConstView &view = arr.get_view();

    for (unsigned i = 0; i < view.size(); i++) {
        if (view[i].get() == v)
            return static_cast<long>(i);
    }

    return EvalValue();
}

static EvalValue
sort_arr(EvalContext *ctx, ExprList *exprList, bool reverse)
{
    if (exprList->elems.size() == 0)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0_lval = arg0->eval(ctx);
    EvalValue val0 = RValue(val0_lval);

    if (!val0.is<FlatSharedArray>())
        throw TypeErrorEx(arg0->start, arg0->end);

    FlatSharedArray &arr = val0.get<FlatSharedArray>();

    if (arr.is_slice()) {

        arr.clone_internal_vec();

        if (val0_lval.is<LValue *>())
            val0_lval.get<LValue *>()->put(arr);

    } else {

        arr.clone_all_slices();
    }

    auto &vec = arr.get_ref();

    if (exprList->elems.size() == 1) {

        if (!reverse) {

            sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
                return a.get() < b.get();
            });

        } else {

            sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
                return a.get() > b.get();
            });
        }

    } else {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &val1 = RValue(arg1->eval(ctx));

        if (!val1.is<FlatSharedFuncObj>())
            throw TypeErrorEx(arg1->start, arg1->end);

        FuncObject &funcObj = val1.get<FlatSharedFuncObj>().get();

        if (!reverse) {

            sort(vec.begin(), vec.end(), [&](const auto &a, const auto &b) {
                return eval_func(ctx, funcObj, make_pair(a.get(),b.get())).is_true();
            });

        } else {

            sort(vec.begin(), vec.end(), [&](const auto &a, const auto &b) {
                return !eval_func(ctx, funcObj, make_pair(a.get(),b.get())).is_true();
            });
        }
    }

    return arr;
}

EvalValue builtin_sort(EvalContext *ctx, ExprList *exprList)
{
    return sort_arr(ctx, exprList, false);
}

EvalValue builtin_rev_sort(EvalContext *ctx, ExprList *exprList)
{
    return sort_arr(ctx, exprList, true);
}

EvalValue builtin_reverse(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0_lval = arg0->eval(ctx);
    EvalValue val0 = RValue(val0_lval);

    if (!val0.is<FlatSharedArray>())
        throw TypeErrorEx(arg0->start, arg0->end);

    FlatSharedArray &arr = val0.get<FlatSharedArray>();

    if (arr.is_slice()) {

        arr.clone_internal_vec();

        if (val0_lval.is<LValue *>())
            val0_lval.get<LValue *>()->put(arr);

    } else {

        arr.clone_all_slices();
    }

    auto &vec = arr.get_ref();
    reverse(vec.begin(), vec.end());
    return arr;
}

EvalValue builtin_sum(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1 || exprList->elems.size() > 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<FlatSharedArray>())
        throw TypeErrorEx(arg0->start, arg0->end);

    const FlatSharedArray &arr = val0.get<FlatSharedArray>();
    const ArrayConstView &view = arr.get_view();

    if (exprList->elems.size() == 1) {

        EvalValue val = view[0].get();

        for (unsigned i = 1; i < view.size(); i++) {
            val.get_type()->add(val, view[i].get());
        }

        return val;

    } else {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &val1 = RValue(arg1->eval(ctx));

        if (!val1.is<FlatSharedFuncObj>())
            throw TypeErrorEx(arg1->start, arg1->end);

        FuncObject &funcObj = val1.get<FlatSharedFuncObj>().get();
        EvalValue val = eval_func(ctx, funcObj, view[0].get());

        for (unsigned i = 1; i < view.size(); i++) {
            val.get_type()->add(
                val,
                eval_func(ctx, funcObj, view[i].get())
            );
        }

        return val;
    }
}
