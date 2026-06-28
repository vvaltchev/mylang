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

    if (e.is<SharedStr>()) {

        return e;

    } else if (e.is<float_type>()) {

        if (exprList->elems.size() > 2)
            throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

        if (exprList->elems.size() == 2) {

            Construct *arg1 = exprList->elems[1].get();
            const EvalValue &p = RValue(arg1->eval(ctx));

            if (!p.is<int_type>() || p.get<int_type>() < 0 || p.get<int_type>() > 64) {

                throw TypeErrorEx(
                    "Expected an integer in the range [0, 64]",
                    arg1->start,
                    arg1->end
                );
            }

            const int precision = static_cast<int>(p.get<int_type>());
            const float_type fval = e.get<float_type>();

            /*
             * Size the buffer to the exact length: a fixed buffer would
             * silently truncate for large-magnitude values at high precision
             * (e.g. str(1e30, 64) needs ~96 chars).
             */
            const int n = snprintf(nullptr, 0, "%.*f", precision, fval);

            if (n < 0)
                throw InternalErrorEx(arg0->start, arg0->end);

            std::vector<char> buf(static_cast<size_t>(n) + 1);
            snprintf(buf.data(), buf.size(), "%.*f", precision, fval);
            return SharedStr(string(buf.data(), static_cast<size_t>(n)));
        }

    } else {

        if (exprList->elems.size() > 1)
            throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    }

    return SharedStr(e.to_string());
}

/*
 * runtime(expr): an optimization barrier. Returns its single argument's value
 * unchanged at run time but - because it is a *non-const* builtin - the call is
 * opaque to const-folding and auto-const: any expression that contains
 * runtime(x) is never folded and is therefore evaluated (and any error it
 * raises thrown) at run time rather than at "compile" time. The ARGUMENT is
 * still folded normally, so runtime(1/0) fails at compile time (the error is
 * inside the expression, before it is "runtime-ized"), while 1/runtime(0)
 * throws at run time. Useful in tests, and to opt an expression out of folding.
 */
EvalValue builtin_runtime(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    return RValue(exprList->elems[0]->eval(ctx));
}

/*
 * isconst(expr) / isconstdecl(expr): compile-time introspection, normally
 * resolved (folded to a literal) by the auto-const pass - see resolver.cpp.
 * isconst() is true when `expr` is *effectively* constant (a literal, an
 * explicit `const`, a const expression, an auto-const var, or a const /
 * auto-const param); isconstdecl() is true only when `expr` is const by *decl*
 * (explicit `const`/const expression), i.e. NOT merely via auto-const. These
 * runtime bodies are conservative fallbacks for the rare case the call survives
 * to runtime (e.g. inside an unresolved function): they report the parse-time
 * const flag of the argument.
 */
EvalValue builtin_isconst(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    return exprList->elems[0]->is_const;
}

EvalValue builtin_isconstdecl(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    return exprList->elems[0]->is_const;
}

/*
 * ispure(func) / ispuredecl(func): true if `func` evaluates to a function
 * object that is effectively pure (explicitly `pure`, OR proven pure by the
 * resolver), resp. *explicitly* declared `pure`. The arg is evaluated.
 */
EvalValue builtin_ispure(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &v = RValue(arg->eval(ctx));

    if (!v.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx(arg->start, arg->end);

    return v.get<intrusive_ptr<FuncObject>>()->func->effective_pure;
}

EvalValue builtin_ispuredecl(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &v = RValue(arg->eval(ctx));

    if (!v.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx(arg->start, arg->end);

    return v.get<intrusive_ptr<FuncObject>>()->func->explicit_pure;
}

EvalValue builtin_clone(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (e.is<SharedStr>()) {
        /* Strings are immutable */
        return e;
    }

    return e.get_type()->clone(e);
}

/*
 * deepclone(x): a fully-mutable deep copy - unlike clone(), which copies only
 * the top level and shares (read-only, for a const) the nested objects. Use it
 * to get a mutable version of a const you need to mutate at any depth. It is a
 * runtime (non-const) builtin: it produces a mutable value that must be copied
 * fresh per evaluation anyway, so folding it would only bloat the tree.
 */
EvalValue builtin_deepclone(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    return make_deep_mutable_clone(RValue(arg->eval(ctx)));
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

EvalValue builtin_assert(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.get_type()->is_true(e))
        throw AssertionFailureEx(exprList->start, exprList->end);

    return none;
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

    if (lval->is<intrusive_ptr<DictObject>>()) {

        if (lval->getval<intrusive_ptr<DictObject>>()->is_readonly())
            throw CannotChangeConstEx(arg0->start, arg0->end);

        return builtin_erase_dict(lval, index_val);

    } else if (lval->is<SharedArrayObj>()) {

        if (lval->getval<SharedArrayObj>().is_readonly())
            throw CannotChangeConstEx(arg0->start, arg0->end);

        if (!index_val.is<int_type>())
            throw TypeErrorEx("Expected integer", arg1->start, arg1->end);

        return builtin_erase_arr(lval, index_val.get<int_type>());

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

    if (lval->is<intrusive_ptr<DictObject>>()) {

        if (lval->getval<intrusive_ptr<DictObject>>()->is_readonly())
            throw CannotChangeConstEx(arg0->start, arg0->end);

        return builtin_insert_dict(lval, index_val, val);

    } else if (lval->is<SharedArrayObj>()) {

        if (lval->getval<SharedArrayObj>().is_readonly())
            throw CannotChangeConstEx(arg0->start, arg0->end);

        if (!index_val.is<int_type>())
            throw TypeErrorEx("Expected integer", arg1->start, arg1->end);

        return builtin_insert_arr(lval, index_val.get<int_type>(), val);

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
    if (exprList->elems.size() < 2 || exprList->elems.size() > 3)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &container_val = RValue(arg0->eval(ctx));
    const EvalValue &elem_val = RValue(arg1->eval(ctx));

    if (container_val.is<intrusive_ptr<DictObject>>()) {

        return builtin_find_dict(
            container_val.get<intrusive_ptr<DictObject>>(), elem_val);

    } else if (container_val.is<SharedArrayObj>()) {

        FuncObject *key = nullptr;
        /* Hold the key function's handle for the whole find: an inline lambda
         * has no other owner, so without this its FuncObject would be freed
         * when the temporary `keyval` below goes out of scope, leaving `key`
         * dangling (a use-after-free when find_arr calls it). */
        intrusive_ptr<FuncObject> key_holder;

        if (exprList->elems.size() == 3) {

            Construct *arg2 = exprList->elems[2].get();
            const EvalValue &keyval = RValue(arg2->eval(ctx));

            if (!keyval.is<intrusive_ptr<FuncObject>>())
                throw TypeErrorEx("Expected function object", arg2->start, arg2->end);

            key_holder = keyval.get<intrusive_ptr<FuncObject>>();
            key = key_holder.get();
        }

        return builtin_find_arr(container_val.get<SharedArrayObj>(), elem_val, key, ctx);

    } else if (container_val.is<SharedStr>()) {

        if (!elem_val.is<SharedStr>())
            throw TypeErrorEx("Expected string", arg1->start, arg1->end);

        return builtin_find_str(
            container_val.get<SharedStr>(),
            elem_val.get<SharedStr>()
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
    return static_cast<int_type>(e.hash());
}

EvalValue builtin_map(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx("Expected function", arg0->start, arg0->end);

    const EvalValue &val1 = RValue(arg1->eval(ctx));
    FuncObject &funcObj = *val0.get<intrusive_ptr<FuncObject>>().get();
    SharedArrayObj::vec_type result;

    if (val1.is<SharedArrayObj>()) {

        /* Read the input element-by-element WITHOUT promoting flat storage
         * (arr_elem_at). map() builds a fresh array - not a promotion. */
        const SharedArrayObj &arr = val1.get<SharedArrayObj>();
        const size_type n = arr.size();

        for (size_type i = 0; i < n; i++) {

            result.emplace_back(
                eval_func(ctx, funcObj, arr_elem_at(arr, i)),
                ctx->const_ctx
            );
        }

    } else if (val1.is<intrusive_ptr<DictObject>>()) {

        const DictObject::inner_type &data
            = val1.get<intrusive_ptr<DictObject>>()->get_ref();

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

    return SharedArrayObj(move(result));
}

EvalValue builtin_filter(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx("Expected function", arg0->start, arg0->end);

    const EvalValue &val1 = RValue(arg1->eval(ctx));
    FuncObject &funcObj = *val0.get<intrusive_ptr<FuncObject>>().get();

    if (val1.is<SharedArrayObj>()) {

        /* Read input without promoting flat storage; build a fresh array. */
        const SharedArrayObj &arr = val1.get<SharedArrayObj>();
        const size_type n = arr.size();
        SharedArrayObj::vec_type result;

        for (size_type i = 0; i < n; i++) {

            const EvalValue e = arr_elem_at(arr, i);
            if (eval_func(ctx, funcObj, e).is_true())
                result.emplace_back(e, ctx->const_ctx);
        }

        return SharedArrayObj(move(result));

    } else if (val1.is<intrusive_ptr<DictObject>>()) {

        const DictObject::inner_type &data
            = val1.get<intrusive_ptr<DictObject>>()->get_ref();

        DictObject::inner_type result;

        for (auto const &e : data) {
            if (eval_func(ctx, funcObj, make_pair(e.first, e.second.get())).is_true())
                result.insert(e);
        }

        return make_intrusive<DictObject>(move(result));

    } else {

        throw TypeErrorEx(
            "Unsupported container type for filter()",
            arg1->start,
            arg1->end
        );
    }
}
