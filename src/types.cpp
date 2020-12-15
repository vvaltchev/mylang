/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "evaltypes.cpp.h"
#include "types/int.cpp.h"
#include "types/str.cpp.h"
#include "types/func.cpp.h"
#include "types/arr.cpp.h"
#include "builtins/str.cpp.h"
#include "builtins/generic.cpp.h"
#include "builtins/io.cpp.h"

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
    make_pair("assert", LValue(Builtin{builtin_assert}, false)),
    make_pair("exit", LValue(Builtin{builtin_exit}, false)),
    make_pair("intptr", LValue(Builtin{builtin_intptr}, false)),
    make_pair("undef", LValue(Builtin{builtin_undef}, false)),
    make_pair("print", LValue(Builtin{builtin_print}, false)),
    make_pair("write", LValue(Builtin{builtin_write}, false)),
    make_pair("writeln", LValue(Builtin{builtin_writeln}, false)),
    make_pair("read", LValue(Builtin{builtin_read}, false)),
    make_pair("readln", LValue(Builtin{builtin_readln}, false)),
    make_pair("readlines", LValue(Builtin{builtin_readlines}, false)),
    make_pair("writelines", LValue(Builtin{builtin_writelines}, false)),
};
