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

EvalValue builtin_print(EvalContext *ctx, ExprList *exprList)
{
    for (const auto &e: exprList->elems) {
        cout << RValue(e->eval(ctx)) << " ";
    }

    cout << endl;
    return EvalValue();
}

EvalValue builtin_write(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<FlatSharedStr>())
        throw TypeErrorEx("Expected string", arg->start, arg->end);

    cout << e;
    cout.flush();
    return EvalValue();
}

EvalValue builtin_writeln(EvalContext *ctx, ExprList *exprList)
{
    builtin_write(ctx, exprList);
    cout << endl;
    return EvalValue();
}

EvalValue builtin_read(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    return FlatSharedStr(string(std::istreambuf_iterator<char>(cin), {}));
}

EvalValue builtin_readln(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    string str;
    getline(cin, str);
    return FlatSharedStr(move(str));
}

EvalValue builtin_readlines(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    FlatSharedArray::vec_type vec;
    string tmp;

    while (getline(cin, tmp)) {
        vec.emplace_back(EvalValue(FlatSharedStr(move(tmp))), false);
        tmp.clear();
    }

    return FlatSharedArray(move(vec));
}

EvalValue builtin_writelines(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (!val.is<FlatSharedArray>())
        throw TypeErrorEx("Expected array", arg->start, arg->end);

    const FlatSharedArray::vec_type &vec = val.get<FlatSharedArray>().get_ref();

    for (const auto &e : vec) {
        cout << e.get() << endl;
    }

    return EvalValue();
}
