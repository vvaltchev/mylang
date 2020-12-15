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

    if (!val.is<FlatSharedStr>())
        throw TypeErrorEx(arg->start, arg->end);

    try {

        return stol(string(val.get<FlatSharedStr>().get_view()));

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

    return FlatSharedStr(e.get_type()->to_string(e));
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

    const string_view &str = val_str.get<FlatSharedStr>().get_view();
    const string_view &delim = val_delim.get<FlatSharedStr>().get_view();

    FlatSharedArray::inner_type vec;
    size_t last = 0, next = 0;

    while ((next = str.find(delim, last)) != string::npos) {

        vec.emplace_back(
            EvalValue(FlatSharedStr(string(str.substr(last, next-last)))),
            ctx->const_ctx
        );

        last = next + delim.size();
    }

    vec.emplace_back(
        EvalValue(FlatSharedStr(string(str.substr(last)))),
        ctx->const_ctx
    );

    return EvalValue(FlatSharedArray(move(vec)));
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
    const FlatSharedArray &arr = val_arr.get<FlatSharedArray>();
    const FlatSharedArray::inner_type &vec = arr.get_shval().get();
    string result;

    for (size_t i = 0; i < arr.size(); i++) {

        const EvalValue &val = vec[arr.offset() + i].get();

        if (!val.is<FlatSharedStr>())
            throw TypeErrorEx(arg_arr->start, arg_arr->end);

        result += val.get<FlatSharedStr>().get_view();

        if (i != arr.size() - 1)
            result += delim;
    }

    return EvalValue(FlatSharedStr(move(result)));
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

const EvalValue EvalValue::empty_str = FlatSharedStr(string());

const EvalContext::SymbolsType EvalContext::const_builtins =
{
    make_pair("defined", LValue(Builtin{builtin_defined}, true)),
    make_pair("len", LValue(Builtin{builtin_len}, true)),
    make_pair("str", LValue(Builtin{builtin_str}, true)),
    make_pair("int", LValue(Builtin{builtin_int}, true)),
    make_pair("clone", LValue(Builtin{builtin_clone}, true)),
    make_pair("split", LValue(Builtin{builtin_split}, true)),
    make_pair("join", LValue(Builtin{builtin_join}, true)),
};

const EvalContext::SymbolsType EvalContext::builtins =
{
    make_pair("print", LValue(Builtin{builtin_print}, false)),
    make_pair("assert", LValue(Builtin{builtin_assert}, false)),
    make_pair("exit", LValue(Builtin{builtin_exit}, false)),
    make_pair("intptr", LValue(Builtin{builtin_intptr}, false)),
    make_pair("undef", LValue(Builtin{builtin_undef}, false)),
};
