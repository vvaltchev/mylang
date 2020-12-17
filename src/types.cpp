/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "evaltypes.cpp.h"
#include "types/int.cpp.h"
#include "types/str.cpp.h"
#include "types/func.cpp.h"
#include "types/arr.cpp.h"
#include "types/exception.cpp.h"
#include "builtins/str.cpp.h"
#include "builtins/generic.cpp.h"
#include "builtins/io.cpp.h"
#include "builtins/num.cpp.h"
#include "builtins/arr.cpp.h"

static const array<FlatSharedStr, Type::t_count> TypeNames =
{
    string("none"),
    string(),
    string(),
    string("int"),
    string("builtin"),
    string("str"),
    string("func"),
    string("arr"),
    string("exception"),
};

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

EvalValue builtin_type(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    return TypeNames[e.get_type()->t];
}

EvalValue builtin_exception(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1 || exprList->elems.size() > 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const EvalValue &name_val = RValue(exprList->elems[0]->eval(ctx));

    if (!name_val.is<FlatSharedStr>())
        throw TypeErrorEx(exprList->elems[0]->start, exprList->elems[0]->end);

    return FlatSharedException(
        ExceptionObject(
            name_val.get<FlatSharedStr>().get_ref(),
            exprList->elems.size() == 2
                ? RValue(exprList->elems[1]->eval(ctx))
                : EvalValue()
        )
    );
}

EvalValue builtin_exdata(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<FlatSharedException>())
        throw TypeErrorEx(arg->start, arg->end);

    return e.get<FlatSharedException>().get_ref().get_data();
}

const array<Type *, Type::t_count> AllTypes =
{
    new TypeNone(),
    new Type(Type::t_lval),       /* internal type: not visible from outside */
    new Type(Type::t_undefid),    /* internal type: not visible from outside */
    new TypeInt(),
    new TypeBuiltin(),
    new TypeStr(),
    new TypeFunc(),
    new TypeArr(),
    new TypeException(),
};


/*
 * NOTE: these definitions *MUST FOLLOW* the definition of `AllTypes`
 * simply because the creation of LValue's contents does a lookup
 * in AllTypes.
 */

const EvalValue empty_str = FlatSharedStr(string());
const EvalValue empty_arr = FlatSharedArray(vector<LValue>());

inline auto make_const_builtin(const char *name, decltype(Builtin::func) f)
{
    return make_pair(name, LValue(Builtin{f}, true));
}

inline auto make_builtin(const char *name, decltype(Builtin::func) f)
{
    return make_pair(name, LValue(Builtin{f}, false));
}

const EvalContext::SymbolsType EvalContext::const_builtins =
{
    make_const_builtin("defined", builtin_defined),
    make_const_builtin("len", builtin_len),
    make_const_builtin("str", builtin_str),
    make_const_builtin("int", builtin_int),
    make_const_builtin("clone", builtin_clone),
    make_const_builtin("split", builtin_split),
    make_const_builtin("join", builtin_join),
    make_const_builtin("splitlines", builtin_splitlines),
    make_const_builtin("abs", builtin_abs),
    make_const_builtin("ord", builtin_ord),
    make_const_builtin("chr", builtin_chr),
    make_const_builtin("min", builtin_min),
    make_const_builtin("max", builtin_max),
    make_const_builtin("array", builtin_array),
    make_const_builtin("top", builtin_top),
    make_const_builtin("type", builtin_type),
};

const EvalContext::SymbolsType EvalContext::builtins =
{
    make_builtin("assert", builtin_assert),
    make_builtin("exit", builtin_exit),
    make_builtin("intptr", builtin_intptr),
    make_builtin("undef", builtin_undef),
    make_builtin("print", builtin_print),
    make_builtin("write", builtin_write),
    make_builtin("writeln", builtin_writeln),
    make_builtin("read", builtin_read),
    make_builtin("readln", builtin_readln),
    make_builtin("readlines", builtin_readlines),
    make_builtin("writelines", builtin_writelines),
    make_builtin("append", builtin_append),
    make_builtin("push", builtin_append), /* push() is an alias for append() */
    make_builtin("pop", builtin_pop),
    make_builtin("exception", builtin_exception),
    make_builtin("ex", builtin_exception), /* ex() is an alias for exception() */
    make_builtin("exdata", builtin_exdata),
};
