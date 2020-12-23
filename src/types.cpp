/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "evaltypes.cpp.h"
#include "types/int.cpp.h"
#include "types/str.cpp.h"
#include "types/func.cpp.h"
#include "types/arr.cpp.h"
#include "types/exception.cpp.h"
#include "types/float.cpp.h"
#include "types/dict.cpp.h"
#include "builtins/str.cpp.h"
#include "builtins/io.cpp.h"
#include "builtins/num.cpp.h"
#include "builtins/arr.cpp.h"
#include "builtins/dict.cpp.h"
#include "builtins/generic.cpp.h"

#include <cmath>
#include <limits>

static const array<FlatSharedStr, Type::t_count> TypeNames =
{
    string("none"),
    string(),
    string(),
    string("int"),
    string("builtin"),
    string("float"),
    string("str"),
    string("func"),
    string("arr"),
    string("exception"),
    string("dict"),
};

EvalValue builtin_exit(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<long>())
        throw TypeErrorEx("Expected integer", arg->start, arg->end);

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
        throw TypeErrorEx("Expected string", exprList->elems[0]->start, exprList->elems[0]->end);

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
        throw TypeErrorEx("Expected exception object", arg->start, arg->end);

    return e.get<FlatSharedException>().get_ref().get_data();
}

const array<Type *, Type::t_count> AllTypes =
{
    /* Trivial types */
    new TypeNone(),
    new Type(Type::t_lval),       /* internal type: not visible from outside */
    new Type(Type::t_undefid),    /* internal type: not visible from outside */
    new TypeInt(),
    new TypeBuiltin(),
    new TypeFloat(),

    /* Non-trivial types */
    new TypeStr(),
    new TypeFunc(),
    new TypeArr(),
    new TypeException(),
    new TypeDict(),
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

inline auto make_const_builtin(const char *name, long double val)
{
    return make_pair(name, LValue(val, true));
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
    make_const_builtin("range", builtin_range),
    make_const_builtin("find", builtin_find),
    make_const_builtin("sort", builtin_sort),
    make_const_builtin("rev_sort", builtin_rev_sort),
    make_const_builtin("reverse", builtin_reverse),
    make_const_builtin("sum", builtin_sum),
    make_const_builtin("lpad", builtin_lpad),
    make_const_builtin("rpad", builtin_rpad),
    make_const_builtin("float", builtin_float),
    make_const_builtin("map", builtin_map),
    make_const_builtin("filter", builtin_filter),
    make_const_builtin("hash", builtin_hash),
    make_const_builtin("keys", builtin_keys),
    make_const_builtin("values", builtin_values),
    make_const_builtin("kvpairs", builtin_kvpairs),
    make_const_builtin("dict", builtin_dict),

    /* Float math funcs */
    make_const_builtin("exp", builtin_exp),
    make_const_builtin("exp2", builtin_exp2),
    make_const_builtin("log", builtin_log),
    make_const_builtin("log2", builtin_log2),
    make_const_builtin("log10", builtin_log10),
    make_const_builtin("sqrt", builtin_sqrt),
    make_const_builtin("cbrt", builtin_cbrt),
    make_const_builtin("pow", builtin_pow),
    make_const_builtin("sin", builtin_sin),
    make_const_builtin("cos", builtin_cos),
    make_const_builtin("tan", builtin_tan),
    make_const_builtin("asin", builtin_asin),
    make_const_builtin("acos", builtin_acos),
    make_const_builtin("atan", builtin_atan),
    make_const_builtin("ceil", builtin_ceil),
    make_const_builtin("floor", builtin_floor),
    make_const_builtin("trunc", builtin_trunc),
    make_const_builtin("isinf", builtin_isinf),
    make_const_builtin("isfinite", builtin_isfinite),
    make_const_builtin("isnormal", builtin_isnormal),
    make_const_builtin("isnan", builtin_isnan),
    make_const_builtin("round", builtin_round),

    /* Numeric constants */
    make_const_builtin("math_e", M_E), /* e */
    make_const_builtin("math_log2e", M_LOG2E), /* log_2 e */
    make_const_builtin("math_log10e", M_LOG10E), /* log_10 e */
    make_const_builtin("math_ln2", M_LN2), /* log_e 2 */
    make_const_builtin("math_ln10", M_LN10), /* log_e 10 */
    make_const_builtin("math_pi", M_PI), /* pi */
    make_const_builtin("math_pi2", M_PI_2), /* pi/2 */
    make_const_builtin("math_pi4", M_PI_4), /* pi/4 */
    make_const_builtin("math_1_pi", M_1_PI), /* 1/pi */
    make_const_builtin("math_2_pi", M_2_PI), /* 2/pi */
    make_const_builtin("math_2_sqrt_pi", M_2_SQRTPI), /* 2/sqrt(pi) */
    make_const_builtin("math_sqrt2", M_SQRT2), /* sqrt(2) */
    make_const_builtin("math_1_sqrt2", M_SQRT1_2), /* 1/sqrt(2) */
    make_const_builtin("nan", NAN), /* 1/sqrt(2) */
    make_const_builtin("inf", INFINITY), /* 1/sqrt(2) */
    make_const_builtin("eps", numeric_limits<long double>::epsilon()),
};

EvalContext::SymbolsType EvalContext::builtins =
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
    make_builtin("erase", builtin_erase),
    make_builtin("insert", builtin_insert),
    make_builtin("exception", builtin_exception),
    make_builtin("ex", builtin_exception), /* ex() is an alias for exception() */
    make_builtin("exdata", builtin_exdata),
};
