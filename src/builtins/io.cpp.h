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
#include <cstdio>
#include <cstdlib>

EvalValue builtin_print(EvalContext *ctx, const ArgLocs *exprList,
                        const EvalValue *args, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        cout << args[i] << " ";
    }

    cout << endl;
    return none;
}

EvalValue builtin_write(EvalContext *ctx, const ArgLocs *exprList,
                        const EvalValue *args, size_t n)
{
    if (n < 1 || n > 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const EvalValue &e = args[0];

    if (!e.is<SharedStr>())
        throw TypeErrorEx("Expected string", arg0->start, arg0->end);

    ostream *s = &cout;
    std::ofstream fs;

    if (n == 2) {

        const ArgLoc *arg1 = exprList->arg(1);
        const EvalValue &fstr = args[1];

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

EvalValue builtin_writeln(EvalContext *ctx, const ArgLocs *exprList,
                          const EvalValue *args, size_t n)
{
    builtin_write(ctx, exprList, args, n);
    cout << endl;
    return none;
}

EvalValue builtin_read(EvalContext *ctx, const ArgLocs *exprList,
                       const EvalValue *args, size_t n)
{
    if (n > 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    std::istream *s = &cin;
    std::ifstream fs;

    if (n == 1) {

        const ArgLoc *arg0 = exprList->arg(0);
        const EvalValue &fstr = args[0];

        if (!fstr.is<SharedStr>())
            throw TypeErrorEx("Expect filename (string)", arg0->start, arg0->end);

        fs.open(string(fstr.get<SharedStr>().get_view()));

        if (!fs)
            throw CannotOpenFileEx(arg0->start, arg0->end);

        s = &fs;
    }

    return SharedStr(string(std::istreambuf_iterator<char>(*s), {}));
}

EvalValue builtin_readln(EvalContext *ctx, const ArgLocs *exprList,
                         const EvalValue *args, size_t n)
{
    if (n != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    string str;
    getline(cin, str);
    return SharedStr(move(str));
}

EvalValue builtin_readlines(EvalContext *ctx, const ArgLocs *exprList,
                            const EvalValue *args, size_t n)
{
    if (n > 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    SharedArrayObj::vec_type vec;
    std::istream *s = &cin;
    std::ifstream fs;
    string tmp;

    if (n == 1) {

        const ArgLoc *arg0 = exprList->arg(0);
        const EvalValue &fstr = args[0];

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

    return SharedArrayObj(move(vec));
}

EvalValue builtin_writelines(EvalContext *ctx, const ArgLocs *exprList,
                             const EvalValue *args, size_t nargs)
{
    if (nargs < 1 || nargs > 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);
    const EvalValue &val = args[0];

    if (!val.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg->start, arg->end);

    ostream *s = &cout;
    std::ofstream fs;

    if (nargs == 2) {

        const ArgLoc *arg1 = exprList->arg(1);
        const EvalValue &fstr = args[1];

        if (!fstr.is<SharedStr>())
            throw TypeErrorEx("Expect filename (string)", arg1->start, arg1->end);

        fs.open(string(fstr.get<SharedStr>().get_view()));

        if (!fs)
            throw CannotOpenFileEx(arg1->start, arg1->end);

        s = &fs;
    }

    /* Read kind-aware (arr_elem_at) so a flat int/float array doesn't promote;
     * each element streams via its own operator<<. */
    const SharedArrayObj &arr = val.get<SharedArrayObj>();
    const size_type n = arr.size();

    for (size_type i = 0; i < n; i++)
        *s << arr_elem_at(arr, i) << endl;

    return none;
}

/*
 * remove(path): delete a file. Returns true (1) if a file was removed, false
 * (0) otherwise (e.g. it did not exist) - so it is safe to call for cleanup
 * without checking first. Throws only on a bad argument.
 */
EvalValue builtin_remove(EvalContext *ctx, const ArgLocs *exprList,
                         const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const EvalValue &fstr = args[0];

    if (!fstr.is<SharedStr>())
        throw TypeErrorEx("Expect filename (string)", arg0->start, arg0->end);

    const string path(fstr.get<SharedStr>().get_view());
    return static_cast<int_type>(std::remove(path.c_str()) == 0 ? 1 : 0);
}

/*
 * tmpdir(): the OS temporary directory, as a string with no trailing separator
 * (so a caller can append "/name"). Portable: honors $TMPDIR / %TEMP% / %TMP%,
 * falling back to "/tmp". Like Python's tempfile.gettempdir().
 */
EvalValue builtin_tmpdir(EvalContext *ctx, const ArgLocs *exprList,
                         const EvalValue *args, size_t n)
{
    if (n != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    string s = "/tmp";
    for (const char *var : { "TMPDIR", "TEMP", "TMP" }) {
        const std::optional<string> e = env_get(var);
        if (e && !e->empty()) {
            s = *e;
            break;
        }
    }

    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();

    return SharedStr(move(s));
}
