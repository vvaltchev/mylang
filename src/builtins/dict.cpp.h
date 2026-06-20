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

EvalValue
builtin_find_dict(const intrusive_ptr<DictObject> &obj, const EvalValue &key)
{
    const DictObject &dictObj = *obj.get();
    const DictObject::inner_type &data = dictObj.get_ref();

    const auto &it = data.find(key);

    if (it == data.end())
        return none;

    return it->second.get();
}

/*
 * get(dict, key)  -> the value, or `none` if the key is absent (opt V).
 * get!(dict, key) -> the value, or throws KeyNotFoundEx if absent (non-opt V).
 * Neither inserts. These are the explicit dict accessors: `get` for nullable
 * lookup, `get!` for fail-fast (so the result is usable without a none-check).
 * The `d.key` / `d[k]` sugar behaves like get! (throws on a missing key) unless
 * the dict has a default value (see dict(default_value)).
 */
static EvalValue
dict_get_impl(EvalContext *ctx, ExprList *exprList, bool throw_if_absent)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &d = RValue(arg0->eval(ctx));
    const EvalValue &key = RValue(arg1->eval(ctx));

    if (!d.is<intrusive_ptr<DictObject>>())
        throw TypeErrorEx("Expected dict object", arg0->start, arg0->end);

    const DictObject::inner_type &data =
        d.get<intrusive_ptr<DictObject>>()->get_ref();
    const auto &it = data.find(key);

    if (it != data.end())
        return it->second.get();

    if (throw_if_absent)
        throw KeyNotFoundEx(arg1->start, arg1->end);

    return none;
}

EvalValue builtin_get(EvalContext *ctx, ExprList *exprList)
{
    return dict_get_impl(ctx, exprList, false);
}

EvalValue builtin_get_throw(EvalContext *ctx, ExprList *exprList)
{
    return dict_get_impl(ctx, exprList, true);
}

EvalValue
builtin_erase_dict(LValue *lval, const EvalValue &key)
{
    DictObject &dictObj = *lval->getval<intrusive_ptr<DictObject>>().get();
    DictObject::inner_type &data = dictObj.get_ref();
    return data.erase(key) > 0;
}

EvalValue
builtin_insert_dict(LValue *lval, const EvalValue &key, const EvalValue &val)
{
    DictObject &dictObj = *lval->getval<intrusive_ptr<DictObject>>().get();
    DictObject::inner_type &data = dictObj.get_ref();
    const auto &it = data.insert(make_pair(key, LValue(val, false)));
    return it.second;
}

static EvalValue
dict_keys(const DictObject::inner_type &data)
{
    SharedArrayObj::vec_type result;

    for (auto const &e : data) {
        result.emplace_back(e.first, false);
    }

    return SharedArrayObj(move(result));
}

static EvalValue
dict_values(const DictObject::inner_type &data)
{
    SharedArrayObj::vec_type result;

    for (auto const &e : data) {
        result.emplace_back(e.second.get(), false);
    }

    return SharedArrayObj(move(result));
}

static EvalValue
dict_kvpairs(const DictObject::inner_type &data)
{
    SharedArrayObj::vec_type result;

    for (auto const &e : data) {

        SharedArrayObj::vec_type pair_arr;
        pair_arr.emplace_back(e.first, false);
        pair_arr.emplace_back(e.second.get(), false);
        result.emplace_back(SharedArrayObj(move(pair_arr)), false);
    }

    return SharedArrayObj(move(result));
}

static EvalValue
dict_1arg_func(EvalContext *ctx,
               ExprList *exprList,
               EvalValue (*f)(const DictObject::inner_type &))
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<intrusive_ptr<DictObject>>())
        throw TypeErrorEx("Expected dict object", arg0->start, arg0->end);

    const DictObject::inner_type &data
        = val0.get<intrusive_ptr<DictObject>>()->get_ref();

    return f(data);
}

EvalValue
builtin_keys(EvalContext *ctx, ExprList *exprList)
{
    return dict_1arg_func(ctx, exprList, &dict_keys);
}

EvalValue
builtin_values(EvalContext *ctx, ExprList *exprList)
{
    return dict_1arg_func(ctx, exprList, &dict_values);
}

EvalValue
builtin_kvpairs(EvalContext *ctx, ExprList *exprList)
{
    return dict_1arg_func(ctx, exprList, &dict_kvpairs);
}

EvalValue
builtin_dict(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));
    DictObject::inner_type data;

    if (!e.is<SharedArrayObj>()) {
        throw TypeErrorEx(
            "Expected array of [key, value] pairs", arg->start, arg->end
        );
    }

    /* Read both the outer array and each [k, v] pair via arr_elem_at, so a flat
     * (unboxed int/float) pair like [1, 2] is read directly - no promotion. */
    const SharedArrayObj &outer = e.get<SharedArrayObj>();
    const size_type on = outer.size();

    for (size_type i = 0; i < on; i++) {

        const EvalValue pe = arr_elem_at(outer, i);

        if (!pe.is<SharedArrayObj>()) {
            throw TypeErrorEx(
                "Expected array of [key, value] pairs", arg->start, arg->end
            );
        }

        const SharedArrayObj &pair = pe.get<SharedArrayObj>();

        if (pair.size() != 2) {
            throw TypeErrorEx(
                "Expected array of [key, value] pairs", arg->start, arg->end
            );
        }

        /*
         * insert_or_assign (not emplace) so that on a duplicate key the
         * later [key, value] pair wins, matching Python's dict().
         */
        data.insert_or_assign(
            arr_elem_at(pair, 0),
            LValue(arr_elem_at(pair, 1), false)
        );
    }

    return intrusive_ptr<DictObject>(make_intrusive<DictObject>(move(data)));
}
