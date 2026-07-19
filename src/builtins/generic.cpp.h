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

EvalValue builtin_len(EvalContext *ctx, const ArgLocs *exprList,
                      const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const EvalValue &e = args[0];
    return e.get_type()->len(e);
}

EvalValue builtin_str(EvalContext *ctx, const ArgLocs *exprList,
                      const EvalValue *args, size_t n)
{
    if (n < 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const EvalValue &e = args[0];

    if (e.is<SharedStr>()) {

        return e;

    } else if (e.is<float_type>()) {

        if (n > 2)
            throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

        if (n == 2) {

            const ArgLoc *arg1 = exprList->arg(1);
            const EvalValue &p = args[1];

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
            const int slen = snprintf(nullptr, 0, "%.*f", precision, fval);

            if (slen < 0)
                throw InternalErrorEx(exprList->arg(0)->start,
                                      exprList->arg(0)->end);

            std::vector<char> buf(static_cast<size_t>(slen) + 1);
            snprintf(buf.data(), buf.size(), "%.*f", precision, fval);
            return SharedStr(string(buf.data(), static_cast<size_t>(slen)));
        }

    } else {

        if (n > 1)
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
EvalValue builtin_runtime(EvalContext *ctx, const ArgLocs *exprList,
                          const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    return args[0];
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
EvalValue builtin_ispure(EvalContext *ctx, const ArgLocs *exprList,
                         const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);
    const EvalValue &v = args[0];

    if (!v.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx(arg->start, arg->end);

    return v.get<intrusive_ptr<FuncObject>>()->func->effective_pure;
}

EvalValue builtin_ispuredecl(EvalContext *ctx, const ArgLocs *exprList,
                             const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);
    const EvalValue &v = args[0];

    if (!v.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx(arg->start, arg->end);

    return v.get<intrusive_ptr<FuncObject>>()->func->explicit_pure;
}

EvalValue builtin_clone(EvalContext *ctx, const ArgLocs *exprList,
                        const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const EvalValue &e = args[0];

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
EvalValue builtin_deepclone(EvalContext *ctx, const ArgLocs *exprList,
                            const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    return make_deep_mutable_clone(args[0]);
}

EvalValue builtin_intptr(EvalContext *ctx, const ArgLocs *exprList,
                         LValue *target, const EvalValue *rest, size_t n_rest)
{
    if (exprList->nargs != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);

    if (!target)
        throw NotLValueEx(arg->start, arg->end);

    const EvalValue &e = target->get();
    return e.get_type()->intptr(e);
}

EvalValue builtin_assert(EvalContext *ctx, const ArgLocs *exprList,
                         const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const EvalValue &e = args[0];

    if (!e.get_type()->is_true(e))
        throw AssertionFailureEx(exprList->start, exprList->end);

    return none;
}

/* func_lv (mutating) ABI: `target` = arg0's lvalue (the container); `rest`/
 * `n_rest` = the TAIL ARGS BY VALUE (args 1..n, everything after the arg0
 * lvalue), pre-evaluated. erase is REST-NATIVE, so it reads its index from
 * `rest[0]` (zero node->eval), unlike self-eval append. */
EvalValue builtin_erase(EvalContext *ctx, const ArgLocs *exprList,
                        LValue *target, const EvalValue *rest, size_t n_rest)
{
    if (exprList->nargs != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const ArgLoc *arg1 = exprList->arg(1);
    const EvalValue &index_val = rest[0];   /* value ABI: pre-evaluated */

    if (!target)
        throw NotLValueEx(arg0->start, arg0->end);

    LValue *lval = target;

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

EvalValue builtin_insert(EvalContext *ctx, const ArgLocs *exprList,
                         LValue *target, const EvalValue *rest, size_t n_rest)
{
    if (exprList->nargs != 3)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const ArgLoc *arg1 = exprList->arg(1);
    const EvalValue &index_val = rest[0];   /* value ABI: pre-evaluated */
    const EvalValue &val = rest[1];

    if (!target)
        throw NotLValueEx(arg0->start, arg0->end);

    LValue *lval = target;

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

EvalValue builtin_find(EvalContext *ctx, const ArgLocs *exprList,
                       const EvalValue *args, size_t n)
{
    if (n < 2 || n > 3)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg0 = exprList->arg(0);
    const ArgLoc *arg1 = exprList->arg(1);
    const EvalValue &container_val = args[0];
    const EvalValue &elem_val = args[1];

    if (container_val.is<intrusive_ptr<DictObject>>()) {

        return builtin_find_dict(
            container_val.get<intrusive_ptr<DictObject>>(), elem_val);

    } else if (container_val.is<SharedArrayObj>()) {

        FuncObject *key = nullptr;
        /* args[2] owns the key function's handle for the whole call, so a raw
         * FuncObject* into it stays valid for the loop; hold an extra ref. */
        intrusive_ptr<FuncObject> key_holder;

        if (n == 3) {

            const ArgLoc *arg2 = exprList->arg(2);
            const EvalValue &keyval = args[2];

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

EvalValue builtin_hash(EvalContext *ctx, const ArgLocs *exprList,
                       const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const EvalValue &e = args[0];
    return static_cast<int_type>(e.hash());
}

/*
 * map/filter are NOT value-ABI-migrated: they validate arg0 (the function) and
 * throw BEFORE evaluating arg1 (the container), which the eager value ABI would
 * break (`map(5, undefined_var)` must be TypeErrorEx on the 5, not
 * UndefinedVariableEx on arg1). Pinned by "map()/filter() validates its
 * function argument first". Stay on the old ABI (self-eval, order-controlled).
 */
/*
 * VM (MapFilterV) + tree-walker (builtin_map/filter) SHARED core: apply
 * `func_val` (a FuncObject, validated by the caller) to each element of
 * `container` - map builds a fresh array from every result; filter keeps the
 * elements whose result is truthy (an array from an array, a dict from a dict).
 * The caller validates arg0 BEFORE evaluating the container (builtin_map/filter
 * in the tree-walker; CheckFuncV in the VM), preserving that order.
 * `cstart`/`cend` = the container arg's caret. Defined here (arr_elem_at's TU),
 * declared in eval.h so the VM's MapFilterV handler can call it.
 */
EvalValue vm_map_filter(EvalContext *ctx, const EvalValue &func_val,
                        const EvalValue &container, bool is_filter,
                        Loc cstart, Loc cend)
{
    FuncObject &funcObj = *func_val.get<intrusive_ptr<FuncObject>>().get();

    /* Prepared per-loop callback invoker (VmInvoker, vm.h): under -vm the
     * callback frame is pushed ONCE and each element just rebinds the param
     * slot(s); tree-walk/const-eval fall back to eval_func per element. */
    VmInvoker inv(ctx, funcObj);

    if (container.is<SharedArrayObj>()) {

        /* Read element-by-element WITHOUT promoting flat storage (arr_elem_at);
         * build a fresh array. */
        const SharedArrayObj &arr = container.get<SharedArrayObj>();
        const size_type n = arr.size();
        SharedArrayObj::vec_type result;

        for (size_type i = 0; i < n; i++) {
            /* e / r non-const so the kept one is MOVED into the result vector
             * (avoiding a per-element retain for a general/str/dyn element),
             * not copied (#60 Tier 1). e is passed to the callback by pointer
             * FIRST, so it is dead by the time filter moves it. */
            EvalValue e = arr_elem_at(arr, i);
            EvalValue r = inv.ready() ? inv.invoke(&e, 1)
                                      : eval_func(ctx, funcObj, e);
            if (!is_filter)
                result.emplace_back(std::move(r), ctx->const_ctx);
            else if (r.is_true())
                result.emplace_back(std::move(e), ctx->const_ctx);
        }

        return SharedArrayObj(std::move(result));

    } else if (container.is<intrusive_ptr<DictObject>>()) {

        const DictObject::inner_type &data =
            container.get<intrusive_ptr<DictObject>>()->get_ref();

        auto call_kv = [&](const EvalValue &k, const EvalValue &v) {
            if (inv.ready()) {
                const EvalValue argv[2] = { k, v };
                return inv.invoke(argv, 2);
            }
            return eval_func(ctx, funcObj, make_pair(k, v));
        };

        if (!is_filter) {

            SharedArrayObj::vec_type result;
            for (auto const &e : data)
                result.emplace_back(call_kv(e.first, e.second.get()),
                                    ctx->const_ctx);
            return SharedArrayObj(std::move(result));
        }

        DictObject::inner_type result;
        for (auto const &e : data)
            if (call_kv(e.first, e.second.get()).is_true())
                result.insert(e);
        return make_intrusive<DictObject>(std::move(result));
    }

    throw TypeErrorEx(is_filter ? "Unsupported container type for filter()"
                                : "Unsupported container type for map()",
                      cstart, cend);
}

/* VM ForeachDynNext element read: box-free (boxing a flat int/float/bool
 * scalar), reaching the static arr_elem_at in this (types.cpp) TU. */
EvalValue vm_arr_elem(const EvalValue &arr_val, size_type i)
{
    return arr_elem_at(arr_val.get_ref<SharedArrayObj>(), i);
}

/* map/filter validate arg0 (the function) BEFORE evaluating arg1 (the
 * container) - a TESTED order (`map(5, undefined_var)` is a type error on the
 * 5, not undefined-var on arg1), so they stay on the `func` ABI (the eager
 * value ABI would evaluate arg1 first). The VM preserves the order with a
 * CheckFuncV between arg0 and arg1; both engines then share vm_map_filter. */
EvalValue builtin_map(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue val0 = RValue(arg0->eval(ctx));

    if (!val0.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx("Expected function", arg0->start, arg0->end);

    const EvalValue val1 = RValue(arg1->eval(ctx));
    return vm_map_filter(ctx, val0, val1, /*is_filter=*/false,
                         arg1->start, arg1->end);
}

EvalValue builtin_filter(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue val0 = RValue(arg0->eval(ctx));

    if (!val0.is<intrusive_ptr<FuncObject>>())
        throw TypeErrorEx("Expected function", arg0->start, arg0->end);

    const EvalValue val1 = RValue(arg1->eval(ctx));
    return vm_map_filter(ctx, val0, val1, /*is_filter=*/true,
                         arg1->start, arg1->end);
}
