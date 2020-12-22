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

EvalValue builtin_str(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &e = RValue(arg0->eval(ctx));

    if (e.is<FlatSharedStr>()) {

        return e;

    } else if (e.is<long double>()) {

        if (exprList->elems.size() > 2)
            throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

        if (exprList->elems.size() == 2) {

            Construct *arg1 = exprList->elems[1].get();
            const EvalValue &p = RValue(arg1->eval(ctx));

            if (!p.is<long>() || p.get<long>() < 0 || p.get<long>() > 64) {

                throw TypeErrorEx(
                    "Expected an integer in the range [0, 64]",
                    arg1->start,
                    arg1->end
                );
            }

            char buf[80];
            const int precision = static_cast<int>(p.get<long>());
            snprintf(buf, sizeof(buf), "%.*Lf", precision, e.get<long double>());
            return FlatSharedStr(string(buf));
        }

    } else {

        if (exprList->elems.size() > 1)
            throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    }

    return FlatSharedStr(e.to_string());
}

EvalValue builtin_clone(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (e.is<FlatSharedStr>()) {
        /* Strings are immutable */
        return e;
    }

    return e.get_type()->clone(e);
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
        throw TypeErrorEx("Expected identifier", arg->start, arg->end);

    const auto &it = ctx->symbols.find(id->value);

    if (it == ctx->symbols.end())
        return false;

    ctx->symbols.erase(it);
    return true;
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

EvalValue builtin_erase(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &container_lval = arg0->eval(ctx);
    const EvalValue &index_val = RValue(arg1->eval(ctx));

    if (!container_lval.is<LValue *>())
        throw NotLValueEx(arg0->start, arg0->end);

    LValue *lval = container_lval.get<LValue *>();

    if (lval->is_const_var())
        throw CannotChangeConstEx(arg0->start, arg0->end);

    if (lval->is<FlatSharedDictObj>()) {

        return builtin_erase_dict(lval, index_val);

    } else if (lval->is<FlatSharedArray>()) {

        if (!index_val.is<long>())
            throw TypeErrorEx("Expected integer", arg1->start, arg1->end);

        return builtin_erase_arr(lval, index_val.get<long>());

    } else {

        throw TypeErrorEx(
            "Unsupported container type by erase()",
            arg0->start,
            arg0->end
        );
    }
}

EvalValue builtin_insert(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 3)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    Construct *arg2 = exprList->elems[2].get();
    const EvalValue &container_lval = arg0->eval(ctx);
    const EvalValue &index_val = RValue(arg1->eval(ctx));
    const EvalValue &val = RValue(arg2->eval(ctx));

    if (!container_lval.is<LValue *>())
        throw NotLValueEx(arg0->start, arg0->end);

    LValue *lval = container_lval.get<LValue *>();

    if (lval->is_const_var())
        throw CannotChangeConstEx(arg0->start, arg0->end);

    if (lval->is<FlatSharedDictObj>()) {

        return builtin_insert_dict(lval, index_val, val);

    } else if (lval->is<FlatSharedArray>()) {

        if (!index_val.is<long>())
            throw TypeErrorEx("Expected integer", arg1->start, arg1->end);

        return builtin_insert_arr(lval, index_val.get<long>(), val);

    } else {

        throw TypeErrorEx(
            "Unsupported container type by erase()",
            arg0->start,
            arg0->end
        );
    }
}

EvalValue builtin_find(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &container_val = RValue(arg0->eval(ctx));
    const EvalValue &elem_val = RValue(arg1->eval(ctx));

    if (container_val.is<FlatSharedDictObj>()) {

        return builtin_find_dict(container_val.get<FlatSharedDictObj>(), elem_val);

    } else if (container_val.is<FlatSharedArray>()) {

        return builtin_find_arr(container_val.get<FlatSharedArray>(), elem_val);

    } else if (container_val.is<FlatSharedStr>()) {

        if (!elem_val.is<FlatSharedStr>())
            throw TypeErrorEx("Expected string", arg1->start, arg1->end);

        return builtin_find_str(
            container_val.get<FlatSharedStr>(),
            elem_val.get<FlatSharedStr>()
        );

    } else {

        throw TypeErrorEx("Unsupported container type by find()", arg0->start, arg0->end);
    }
}

EvalValue builtin_hash(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));
    return static_cast<long>(e.hash());
}

EvalValue builtin_map(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));
    const EvalValue &val1 = RValue(arg1->eval(ctx));
    FuncObject &funcObj = val0.get<FlatSharedFuncObj>().get();
    FlatSharedArray::vec_type result;

    if (!val0.is<FlatSharedFuncObj>())
        throw TypeErrorEx("Expected function", arg0->start, arg0->end);

    if (val1.is<FlatSharedArray>()) {

        const ArrayConstView &view = val1.get<FlatSharedArray>().get_view();

        for (unsigned i = 0; i < view.size(); i++) {

            result.emplace_back(
                eval_func(ctx, funcObj, view[i].get()),
                ctx->const_ctx
            );
        }

    } else if (val1.is<FlatSharedDictObj>()) {

        const DictObject::inner_type &data
            = val1.get<FlatSharedDictObj>()->get_ref();

        for (auto const &e : data) {

            result.emplace_back(
                eval_func(ctx, funcObj, make_pair(e.first, e.second.get())),
                ctx->const_ctx
            );
        }

    } else {

        throw TypeErrorEx(
            "Unsupported container type for map()",
            arg1->start,
            arg1->end
        );
    }

    return FlatSharedArray(move(result));
}

EvalValue builtin_filter(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));
    const EvalValue &val1 = RValue(arg1->eval(ctx));
    FuncObject &funcObj = val0.get<FlatSharedFuncObj>().get();

    if (!val0.is<FlatSharedFuncObj>())
        throw TypeErrorEx("Expected function", arg0->start, arg0->end);

    if (val1.is<FlatSharedArray>()) {

        const ArrayConstView &view = val1.get<FlatSharedArray>().get_view();
        FlatSharedArray::vec_type result;

        for (unsigned i = 0; i < view.size(); i++) {

            if (eval_func(ctx, funcObj, view[i].get()).is_true())
                result.emplace_back(view[i].get(), ctx->const_ctx);
        }

        return FlatSharedArray(move(result));

    } else if (val1.is<FlatSharedDictObj>()) {

        const DictObject::inner_type &data
            = val1.get<FlatSharedDictObj>()->get_ref();

        DictObject::inner_type result;

        for (auto const &e : data) {
            if (eval_func(ctx, funcObj, make_pair(e.first, e.second.get())).is_true())
                result.insert(e);
        }

        return FlatSharedDictObj(make_shared<DictObject>(move(result)));

    } else {

        throw TypeErrorEx(
            "Unsupported container type for filter()",
            arg1->start,
            arg1->end
        );
    }
}
