/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "evaltypes.cpp.h"
#include "type_int.cpp.h"
#include "type_str.cpp.h"
#include "type_func.cpp.h"
#include "type_arr.cpp.h"

EvalValue builtin_defined(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    return !arg->eval(ctx).is<UndefinedId>();
}

EvalValue builtin_len(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));
    return e.get_type()->len(e);
}

EvalValue builtin_int(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &val = RValue(arg->eval(ctx));

    if (!val.is<SharedStr>())
        throw TypeErrorEx(arg->start, arg->end);

    try {

        return stol(string(val.get<SharedStr>().get_view()));

    } catch (...) {

        throw TypeErrorEx(arg->start, arg->end);
    }
}

EvalValue builtin_str(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    return SharedStr(e.get_type()->to_string(e));
}

EvalValue builtin_clone(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));
    return e.get_type()->clone(e);
}

EvalValue builtin_print(EvalContext *ctx, ExprList *exprList)
{
    for (const auto &e: exprList->elems) {
        cout << RValue(e->eval(ctx)) << " ";
    }

    cout << endl;
    return EvalValue();
}

EvalValue builtin_assert(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.get_type()->is_true(e))
        throw AssertionFailureEx(exprList->start, exprList->end);

    return EvalValue();
}

EvalValue builtin_exit(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<long>())
        throw TypeErrorEx(arg->start, arg->end);

    exit(e.get<long>());
}

EvalValue builtin_intptr(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &lval = arg->eval(ctx);

    if (!lval.is<LValue *>())
        throw NotLValueEx(arg->start, arg->end);

    const EvalValue &e = lval.get<LValue *>()->get();
    return e.get_type()->intptr(e);
}

EvalValue builtin_undef(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    Identifier *id = dynamic_cast<Identifier *>(arg);

    if (!id)
        throw TypeErrorEx(arg->start, arg->end);

    const auto &it = ctx->symbols.find(id->value);

    if (it == ctx->symbols.end())
        return false;

    ctx->symbols.erase(it);
    return true;
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
    new TypeArr(),
};

/*
 * NOTE: these definitions *MUST FOLLOW* the definition of `AllTypes`
 * simply because the creation of LValue's contents does a lookup
 * in AllTypes.
 */

const EvalValue EvalValue::empty_str = SharedStr(string());

const EvalContext::SymbolsType EvalContext::const_builtins =
{
    make_pair("defined", LValue(Builtin{builtin_defined}, true)),
    make_pair("len", LValue(Builtin{builtin_len}, true)),
    make_pair("str", LValue(Builtin{builtin_str}, true)),
    make_pair("int", LValue(Builtin{builtin_int}, true)),
    make_pair("clone", LValue(Builtin{builtin_clone}, true)),
};

const EvalContext::SymbolsType EvalContext::builtins =
{
    make_pair("print", LValue(Builtin{builtin_print}, false)),
    make_pair("assert", LValue(Builtin{builtin_assert}, false)),
    make_pair("exit", LValue(Builtin{builtin_exit}, false)),
    make_pair("intptr", LValue(Builtin{builtin_intptr}, false)),
    make_pair("undef", LValue(Builtin{builtin_undef}, false)),
};
