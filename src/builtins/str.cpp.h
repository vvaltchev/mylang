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

EvalValue builtin_split(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg_str = exprList->elems[0].get();
    Construct *arg_delim = exprList->elems[1].get();

    const EvalValue &val_str = RValue(arg_str->eval(ctx));
    const EvalValue &val_delim = RValue(arg_delim->eval(ctx));

    if (!val_str.is<FlatSharedStr>())
        throw TypeErrorEx(arg_str->start, arg_str->end);

    if (!val_delim.is<FlatSharedStr>())
        throw TypeErrorEx(arg_delim->start, arg_delim->end);

    const FlatSharedStr &flat_str = val_str.get<FlatSharedStr>();
    const string_view &str = flat_str.get_view();
    const string_view &delim = val_delim.get<FlatSharedStr>().get_view();

    FlatSharedArray::vec_type vec;

    if (delim.size()) {

        size_t last = 0, next = 0;

        while ((next = str.find(delim, last)) != string::npos) {

            vec.emplace_back(
                FlatSharedStr(flat_str, last, next-last),
                ctx->const_ctx
            );

            last = next + delim.size();
        }

        vec.emplace_back(
            FlatSharedStr(flat_str, last, str.size() - last),
            ctx->const_ctx
        );

    } else {

        for (size_t i = 0; i < str.size(); i++) {
            vec.emplace_back(
                FlatSharedStr(flat_str, i, 1),
                ctx->const_ctx
            );
        }
    }

    return FlatSharedArray(move(vec));
}

EvalValue builtin_join(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg_arr = exprList->elems[0].get();
    Construct *arg_delim = exprList->elems[1].get();

    const EvalValue &val_arr = RValue(arg_arr->eval(ctx));
    const EvalValue &val_delim = RValue(arg_delim->eval(ctx));

    if (!val_arr.is<FlatSharedArray>())
        throw TypeErrorEx(arg_arr->start, arg_arr->end);

    if (!val_delim.is<FlatSharedStr>())
        throw TypeErrorEx(arg_delim->start, arg_delim->end);

    const string_view &delim = val_delim.get<FlatSharedStr>().get_view();
    const ArrayConstView &arr_view = val_arr.get<FlatSharedArray>().get_view();
    string result;

    for (size_t i = 0; i < arr_view.size(); i++) {

        const EvalValue &val = arr_view[i].get();

        if (!val.is<FlatSharedStr>())
            throw TypeErrorEx(arg_arr->start, arg_arr->end);

        result += val.get<FlatSharedStr>().get_view();

        if (i != arr_view.size() - 1)
            result += delim;
    }

    return FlatSharedStr(move(result));
}

EvalValue builtin_splitlines(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (!val.is<FlatSharedStr>())
        throw TypeErrorEx(arg->start, arg->end);

    const FlatSharedStr &flat_str = val.get<FlatSharedStr>();
    const string_view &str = flat_str.get_view();
    FlatSharedArray::vec_type vec;
    unsigned i, start = 0;

    for (i = 0; i < str.size(); i++) {

        if (str[i] == '\r') {

            vec.emplace_back(
                FlatSharedStr(flat_str, start, i - start),
                ctx->const_ctx
            );

            if (i + 1 < str.size() && str[i + 1] == '\n')
                i++;

            start = i + 1;

        } else if (str[i] == '\n') {

            vec.emplace_back(
                FlatSharedStr(flat_str, start, i - start),
                ctx->const_ctx
            );

            start = i + 1;
        }
    }

    if (!str.empty()) {
        vec.emplace_back(
            FlatSharedStr(flat_str, start, i - start),
            ctx->const_ctx
        );
    }

    return FlatSharedArray(move(vec));
}

EvalValue builtin_ord(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (!val.is<FlatSharedStr>())
        throw TypeErrorEx(arg->start, arg->end);

    const FlatSharedStr &flat_str = val.get<FlatSharedStr>();
    const string_view &str = flat_str.get_view();

    if (str.size() != 1)
         throw TypeErrorEx(arg->start, arg->end);

    return static_cast<long>(static_cast<unsigned char>(str[0]));
}

EvalValue builtin_chr(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (!val.is<long>())
        throw TypeErrorEx(arg->start, arg->end);

    char c = static_cast<char>(val.get<long>());
    return FlatSharedStr(string(&c, 1));
}

EvalValue
builtin_find_str(const FlatSharedStr &str, const FlatSharedStr &substr)
{
    const size_t pos = str.get_view().find(substr.get_view());

    if (pos == string::npos)
        return EvalValue();

    return static_cast<long>(pos);
}

template <bool leftpad>
static EvalValue
generic_pad(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 2 || exprList->elems.size() > 3)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &strval = RValue(arg0->eval(ctx));
    const EvalValue &nval = RValue(arg1->eval(ctx));
    char pad_char = ' ';

    if (!strval.is<FlatSharedStr>())
        throw TypeErrorEx(arg0->start, arg0->end);

    if (!nval.is<long>())
        throw TypeErrorEx(arg1->start, arg1->end);

    if (exprList->elems.size() == 3) {

        Construct *arg2 = exprList->elems[2].get();
        const EvalValue &padc = RValue(arg2->eval(ctx));

        if (!padc.is<FlatSharedStr>())
            throw TypeErrorEx(arg2->start, arg2->end);

        const string_view &padstr = padc.get<FlatSharedStr>().get_view();

        if (padstr.size() > 1)
            throw TypeErrorEx(arg2->start, arg2->end);

        pad_char = padstr[0];
    }

    const string_view &str = strval.get<FlatSharedStr>().get_view();
    const long n_orig = nval.get<long>();

    if (n_orig < 0)
        throw TypeErrorEx(arg1->start, arg1->end);

    const size_t n = static_cast<size_t>(n_orig);

    if constexpr(leftpad) {

        if (str.size() < n)
            return FlatSharedStr(string(n - str.size(), pad_char) + string(str));

    } else {

        if (str.size() < n)
            return FlatSharedStr(string(str) + string(n - str.size(), pad_char));
    }

    return strval;
}

EvalValue builtin_lpad(EvalContext *ctx, ExprList *exprList)
{
    return generic_pad<true>(ctx, exprList);
}

EvalValue builtin_rpad(EvalContext *ctx, ExprList *exprList)
{
    return generic_pad<false>(ctx, exprList);
}
