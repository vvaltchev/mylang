/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "evaltypes.cpp.h"
#include "type_int.cpp.h"
#include "type_str.cpp.h"
#include "type_func.cpp.h"

EvalValue builtin_print(EvalContext *ctx, ExprList *exprList)
{
    for (const auto &e: exprList->elems) {
        cout << RValue(e->eval(ctx)) << " ";
    }

    cout << endl;
    return EvalValue();
}

EvalValue builtin_len(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() == 0)
        throw TooFewArgsEx(exprList->start, exprList->end);

    if (exprList->elems.size() > 1)
        throw TooManyArgsEx(exprList->start, exprList->end);

    const EvalValue &e = RValue(exprList->elems[0]->eval(ctx));
    return e.get_type()->len(e);
}

EvalValue builtin_defined(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() == 0)
        throw TooFewArgsEx(exprList->start, exprList->end);

    if (exprList->elems.size() > 1)
        throw TooManyArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    return !arg->eval(ctx).is<UndefinedId>();
}

EvalValue builtin_str(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw TooManyArgsEx(exprList->start, exprList->end);

    const EvalValue &e = RValue(exprList->elems[0]->eval(ctx));
    string &&s = e.get_type()->to_string(e);
    return SharedStrWrapper(make_shared<string>(s));
}

EvalValue builtin_assert(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw TooManyArgsEx(exprList->start, exprList->end);

    const EvalValue &e = RValue(exprList->elems[0]->eval(ctx));

    if (!e.get_type()->is_true(e))
        throw AssertionFailureEx(exprList->start, exprList->end);

    return EvalValue();
}

const string &
find_builtin_name(const Builtin &b)
{
    for (const auto &[k, v]: EvalContext::const_builtins) {
        if (v.getval<Builtin>().func == b.func)
            return k;
    }

    for (const auto &[k, v]: EvalContext::builtins) {
        if (v.getval<Builtin>().func == b.func)
            return k;
    }

    throw InternalErrorEx();
}

const array<Type *, Type::t_count> AllTypes = {
    new TypeNone(),
    new Type(Type::t_lval),       /* internal type: not visible from outside */
    new Type(Type::t_undefid),    /* internal type: not visible from outside */
    new TypeInt(),
    new TypeBuiltin(),
    new TypeStr(),
    new TypeFunc(),
};

/*
 * NOTE: these definitions *MUST FOLLOW* the definition of `AllTypes`
 * simply because the creation of LValue's contents does a lookup
 * in AllTypes.
 */

const EvalContext::SymbolsType EvalContext::const_builtins =
{
    make_pair("len", LValue(Builtin{builtin_len})),
    make_pair("str", LValue(Builtin{builtin_str})),
    make_pair("defined", LValue(Builtin{builtin_defined})),
};

const EvalContext::SymbolsType EvalContext::builtins =
{
    make_pair("print", LValue(Builtin{builtin_print})),
    make_pair("assert", LValue(Builtin{builtin_assert})),
};
