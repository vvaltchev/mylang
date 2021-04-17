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

#include <fstream>

EvalValue builtin_print(EvalContext *ctx, ExprList *exprList)
{
    for (const auto &e: exprList->elems) {
        cout << RValue(e->eval(ctx)) << " ";
    }

    cout << endl;
    return none;
}

EvalValue builtin_write(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1 || exprList->elems.size() > 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &e = RValue(arg0->eval(ctx));

    if (!e.is<SharedStr>())
        throw TypeErrorEx("Expected string", arg0->start, arg0->end);

    ostream *s = &cout;
    std::ofstream fs;

    if (exprList->elems.size() == 2) {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &fstr = RValue(arg1->eval(ctx));

        if (!fstr.is<SharedStr>())
            throw TypeErrorEx("Expect filename (string)", arg1->start, arg1->end);

        fs.open(string(fstr.get<SharedStr>().get_view()));

        if (!fs)
            throw CannotOpenFileEx(arg1->start, arg1->end);

        s = &fs;
    }

    *s << e;
    s->flush();
    return none;
}

EvalValue builtin_writeln(EvalContext *ctx, ExprList *exprList)
{
    builtin_write(ctx, exprList);
    cout << endl;
    return none;
}

EvalValue builtin_read(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() > 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    std::istream *s = &cin;
    std::ifstream fs;

    if (exprList->elems.size() == 1) {

        Construct *arg0 = exprList->elems[0].get();
        const EvalValue &fstr = RValue(arg0->eval(ctx));

        if (!fstr.is<SharedStr>())
            throw TypeErrorEx("Expect filename (string)", arg0->start, arg0->end);

        fs.open(string(fstr.get<SharedStr>().get_view()));

        if (!fs)
            throw CannotOpenFileEx(arg0->start, arg0->end);

        s = &fs;
    }

    return SharedStr(string(std::istreambuf_iterator<char>(*s), {}));
}

EvalValue builtin_readln(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    string str;
    getline(cin, str);
    return SharedStr(move(str));
}

EvalValue builtin_readlines(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() > 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    SharedArrayObj::vec_type vec;
    std::istream *s = &cin;
    std::ifstream fs;
    string tmp;

    if (exprList->elems.size() == 1) {

        Construct *arg0 = exprList->elems[0].get();
        const EvalValue &fstr = RValue(arg0->eval(ctx));

        if (!fstr.is<SharedStr>())
            throw TypeErrorEx("Expect filename (string)", arg0->start, arg0->end);

        fs.open(string(fstr.get<SharedStr>().get_view()));

        if (!fs)
            throw CannotOpenFileEx(arg0->start, arg0->end);

        s = &fs;
    }

    while (getline(*s, tmp)) {
        vec.emplace_back(EvalValue(SharedStr(move(tmp))), false);
        tmp.clear();
    }

    return FlatSharedArray(move(vec));
}

EvalValue builtin_writelines(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1 || exprList->elems.size() > 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (!val.is<FlatSharedArray>())
        throw TypeErrorEx("Expected array", arg->start, arg->end);

    ostream *s = &cout;
    std::ofstream fs;

    if (exprList->elems.size() == 2) {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &fstr = RValue(arg1->eval(ctx));

        if (!fstr.is<SharedStr>())
            throw TypeErrorEx("Expect filename (string)", arg1->start, arg1->end);

        fs.open(string(fstr.get<SharedStr>().get_view()));

        if (!fs)
            throw CannotOpenFileEx(arg1->start, arg1->end);

        s = &fs;
    }

    const SharedArrayObj::vec_type &vec = val.get<FlatSharedArray>()->get_vec();

    for (const auto &e : vec) {
        *s << e.get() << endl;
    }

    return none;
}
