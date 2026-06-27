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

/*
 * keys()/values(): extract the dict's keys (KEYS=true) or values into a fresh
 * array. When the inferencer typed the result as a flat array<int>/<float>/
 * <bool> (the ArrHint on the call site, from the destination's static type),
 * build that UNBOXED storage directly - no per-element LValue boxing - which is
 * what keeps keys()/values() of a large scalar dict cheap in time and memory
 * (a flat int array is 8 bytes/element vs 48 boxed). Otherwise build a general
 * (boxed) array. A flat hint is set only when the static element type is that
 * scalar, so reading each key/value as int/float/bool is sound.
 */
template <bool KEYS>
static EvalValue
dict_extract(const DictObject::inner_type &data, ArrHint hint)
{
    /* KEYS picks the pair's key, else its value (an LValue -> EvalValue). */
    #define DICT_ELEM(e) (KEYS ? (e).first : (e).second.get())

    if (hint == ArrHint::flat_i) {
        SharedArrayObj::ivec_type v;
        v.reserve(data.size());
        for (auto const &e : data) {
            const EvalValue &kv = DICT_ELEM(e);
            v.push_back(kv.get<int_type>());
        }
        return SharedArrayObj(move(v));
    }
    if (hint == ArrHint::flat_f) {
        SharedArrayObj::fvec_type v;
        v.reserve(data.size());
        for (auto const &e : data) {
            const EvalValue &kv = DICT_ELEM(e);
            v.push_back(kv.get<float_type>());
        }
        return SharedArrayObj(move(v));
    }
    if (hint == ArrHint::flat_b) {
        SharedArrayObj::bvec_type v;
        v.reserve(data.size());
        for (auto const &e : data) {
            const EvalValue &kv = DICT_ELEM(e);
            v.push_back(kv.get<bool>() ? 1 : 0);
        }
        return SharedArrayObj(move(v));
    }

    SharedArrayObj::vec_type result;
    result.reserve(data.size());
    for (auto const &e : data)
        result.emplace_back(DICT_ELEM(e), false);
    return SharedArrayObj(move(result));

    #undef DICT_ELEM
}

static EvalValue
dict_keys(const DictObject::inner_type &data, ArrHint hint)
{
    return dict_extract<true>(data, hint);
}

static EvalValue
dict_values(const DictObject::inner_type &data, ArrHint hint)
{
    return dict_extract<false>(data, hint);
}

static EvalValue
dict_kvpairs(const DictObject::inner_type &data, ArrHint /*hint*/)
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
               EvalValue (*f)(const DictObject::inner_type &, ArrHint))
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<intrusive_ptr<DictObject>>())
        throw TypeErrorEx("Expected dict object", arg0->start, arg0->end);

    const DictObject::inner_type &data
        = val0.get<intrusive_ptr<DictObject>>()->get_ref();

    /* arr_hint is the type-driven flat-storage hint the inferencer stamped on
     * this call when the result's destination is a flat array (keys/values). */
    return f(data, exprList->arr_hint);
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

    /*
     * dict(default_value): a non-array argument is a *default value*, producing
     * an empty "default dict" - reading a missing key returns (and inserts) the
     * default instead of throwing, so `d[k] += 1` works without a none-check.
     * `none` is not a valid default (it would make d[k] yield none yet be typed
     * non-opt). An array argument is the array-of-[k,v]-pairs form below.
     */
    if (!e.is<SharedArrayObj>()) {

        if (e.is<NoneVal>())
            throw InvalidValueEx(
                "dict() default value cannot be none", arg->start, arg->end
            );

        auto obj = make_intrusive<DictObject>();
        obj->set_default(e);
        return intrusive_ptr<DictObject>(obj);
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
         * later [key, value] pair wins, matching Python's dict(). The key is
         * frozen (see TypeDict::subscript) so a mutable container key can't be
         * mutated later and corrupt the dict.
         */
        data.insert_or_assign(
            make_const_clone(arr_elem_at(pair, 0)),
            LValue(arr_elem_at(pair, 1), false)
        );
    }

    return intrusive_ptr<DictObject>(make_intrusive<DictObject>(move(data)));
}
