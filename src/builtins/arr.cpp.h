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

#include <algorithm>

/*
 * A flat (unboxed) array<int>/array<float> holds exactly one scalar kind, fixed
 * at creation from the proven static type. Storing a different type would need
 * an in-place flat->general conversion (promotion), which mylang deliberately
 * does NOT do - that is what type-driven representation buys (no GC-like
 * latency spikes; the representation is decided once, at creation). This is
 * reachable only by laundering a typed array through `dyn` and then mutating it
 * (`var dyn d = int_array; append(d, "x")`): the shared storage stays int-typed
 * even through the dyn alias, and an alias-affecting write can't change it.
 * Declare the array `dyn` from the start for a (general) polymorphic array.
 */
static const char *const flat_array_violation_msg =
    "Cannot store a value of a different type in a flat (typed) array; "
    "declare the array dyn for a polymorphic array";

/*
 * array(N)        -> N elements of `none` (general storage).
 * array(N, value) -> N elements all equal to `value`. The fill value drives the
 *                    storage (value-driven, see plans/typed-arrays.md): an int
 *                    -> flat int, a float -> flat float, else general. For a
 *                    callback-built array use make_array().
 */
EvalValue builtin_array(EvalContext *ctx, ExprList *exprList)
{
    const size_t nargs = exprList->elems.size();

    if (nargs < 1 || nargs > 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<int_type>())
        throw TypeErrorEx("Expected integer", arg->start, arg->end);

    const int_type n = e.get<int_type>();

    if (n < 0)
        throw InvalidValueEx("Expected non-negative integer",
                             arg->start, arg->end);

    const ArrHint hint = exprList->arr_hint;

    if (nargs == 1) {

        /*
         * No fill value. Type-driven: the inferencer stamps flat_i/flat_f when
         * the destination is array<int>/array<float>, so the array is born flat
         * with a 0 / 0.0 fill (never promoted). Otherwise a general array of
         * `none`.
         */
        if (hint == ArrHint::flat_i)
            return SharedArrayObj(SharedArrayObj::ivec_type(n, 0));
        if (hint == ArrHint::flat_f)
            return SharedArrayObj(SharedArrayObj::fvec_type(n, 0.0));
        if (hint == ArrHint::flat_b)
            return SharedArrayObj(SharedArrayObj::bvec_type(n, 0));

        SharedArrayObj::vec_type vec;
        vec.reserve(n);

        for (int_type i = 0; i < n; i++)
            vec.emplace_back(none, ctx->const_ctx);

        return SharedArrayObj(move(vec));
    }

    const EvalValue &v = RValue(exprList->elems[1]->eval(ctx));

    /*
     * Flat storage for a scalar fill value, unless the destination is
     * dynamically typed (hint == general), in which case build general from the
     * start so a later mixed write never has to promote.
     */
    if (hint != ArrHint::general) {

        if (v.is<int_type>())
            return SharedArrayObj(
                SharedArrayObj::ivec_type(n, v.get<int_type>()));

        if (v.is<float_type>())
            return SharedArrayObj(
                SharedArrayObj::fvec_type(n, v.get<float_type>()));

        if (v.is<bool>())
            return SharedArrayObj(
                SharedArrayObj::bvec_type(n, v.get<bool>() ? 1 : 0));
    }

    /* General fill: every element is (a copy of) the value. */
    SharedArrayObj::vec_type vec;
    vec.reserve(n);

    for (int_type i = 0; i < n; i++)
        vec.emplace_back(v, ctx->const_ctx);

    return SharedArrayObj(move(vec));
}

/*
 * make_array(N, gen) -> [gen(0), gen(1), ..., gen(N-1)]. The callback form of
 * array(): each element is produced by calling `gen` with its index. The
 * representation is value-driven (optimistic flat): elements accumulate in an
 * unboxed int/float vector while they stay that one scalar kind, and the array
 * promotes to general the moment a callback returns something else.
 */
EvalValue builtin_make_array(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &e = RValue(arg0->eval(ctx));

    if (!e.is<int_type>())
        throw TypeErrorEx("Expected integer", arg0->start, arg0->end);

    const int_type n = e.get<int_type>();

    if (n < 0)
        throw InvalidValueEx("Expected non-negative integer",
                             arg0->start, arg0->end);

    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &fval = RValue(arg1->eval(ctx));

    if (!fval.is<shared_ptr<FuncObject>>())
        throw TypeErrorEx("Expected function", arg1->start, arg1->end);

    FuncObject &funcObj = *fval.get<shared_ptr<FuncObject>>().get();

    /*
     * Optimistic flat: stay in `ivec`/`fvec` while every element matches that
     * scalar kind; on the first mismatch, spill what we have into a general
     * `vec` and continue there. `mode`: 0 = empty/undecided, 1 = ints,
     * 2 = floats, 3 = general.
     */
    SharedArrayObj::ivec_type ivec;
    SharedArrayObj::fvec_type fvec;
    SharedArrayObj::bvec_type bvec;
    SharedArrayObj::vec_type  gvec;
    /* Type-driven: a dynamically-typed destination (hint general) builds
     * general from the start; otherwise optimistic flat. mode: 0 = empty,
     * 1 = ints, 2 = floats, 4 = bools, 3 = general. */
    int mode = exprList->arr_hint == ArrHint::general ? 3 : 0;
    if (mode == 3)
        gvec.reserve(n);

    auto spill_to_general = [&]() {
        gvec.reserve(n);
        if (mode == 1)
            for (int_type x : ivec) gvec.emplace_back(EvalValue(x), false);
        else if (mode == 2)
            for (float_type x : fvec) gvec.emplace_back(EvalValue(x), false);
        else if (mode == 4)
            for (unsigned char x : bvec)
                gvec.emplace_back(EvalValue(static_cast<bool>(x)), false);
        ivec.clear();
        fvec.clear();
        bvec.clear();
        mode = 3;
    };

    for (int_type i = 0; i < n; i++) {

        const EvalValue r = eval_func(ctx, funcObj, EvalValue(i));

        if (mode == 0) {
            if (r.is<int_type>()) {
                mode = 1; ivec.push_back(r.get<int_type>());
            } else if (r.is<float_type>()) {
                mode = 2; fvec.push_back(r.get<float_type>());
            } else if (r.is<bool>()) {
                mode = 4; bvec.push_back(r.get<bool>() ? 1 : 0);
            } else {
                mode = 3; gvec.reserve(n); gvec.emplace_back(r, false);
            }
        } else if (mode == 1 && r.is<int_type>()) {
            ivec.push_back(r.get<int_type>());
        } else if (mode == 2 && r.is<float_type>()) {
            fvec.push_back(r.get<float_type>());
        } else if (mode == 4 && r.is<bool>()) {
            bvec.push_back(r.get<bool>() ? 1 : 0);
        } else {
            if (mode != 3)
                spill_to_general();
            gvec.emplace_back(r, false);
        }
    }

    if (mode == 1) return SharedArrayObj(move(ivec));
    if (mode == 2) return SharedArrayObj(move(fvec));
    if (mode == 4) return SharedArrayObj(move(bvec));
    return SharedArrayObj(move(gvec));
}

/*
 * Introspection: report an array's backing-storage specialization, named by the
 * element TYPE for the flat (unboxed) kinds - "int" / "float" / "bool" /
 * "struct" - or "general" for the boxed vector<LValue>. type() can't express
 * this (flat and general are the same t_arr); this exists mainly so tests can
 * pin the representation and catch regressions. Not const: it reflects a
 * runtime fact and must not fold.
 */
EvalValue builtin_array_storage(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg->start, arg->end);

    switch (e.get<SharedArrayObj>().skind()) {
        case SharedArrayObj::Storage::ints:
            return SharedStr(string("int"));
        case SharedArrayObj::Storage::floats:
            return SharedStr(string("float"));
        case SharedArrayObj::Storage::bools:
            return SharedStr(string("bool"));
        case SharedArrayObj::Storage::structs:
            return SharedStr(string("struct"));
        default:
            return SharedStr(string("general"));
    }
}

/*
 * dynarray(a): return a fresh, mutable GENERAL (polymorphic) copy of array `a`.
 * This is the explicit, manual way to "promote" a flat (statically-typed)
 * array<int>/array<float> into one that can hold any element type - there is no
 * automatic runtime promotion (the representation is fixed at creation from the
 * proven static type). Reading via arr_elem_at, so the source is never promoted
 * in place; an already-general array is just copied. Shallow (top-level):
 * nested arrays keep their own representation. Not const (a fresh mutable value
 * that must be re-copied per eval). Compare `var dyn d = [1,2,3]`, where the
 * dyn declaration already builds general from the start.
 */
EvalValue builtin_dynarray(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg->start, arg->end);

    const SharedArrayObj &arr = e.get<SharedArrayObj>();
    const size_type n = arr.size();
    SharedArrayObj::vec_type gvec;
    gvec.reserve(n);

    for (size_type i = 0; i < n; i++)
        gvec.emplace_back(arr_elem_at(arr, i), false);

    return SharedArrayObj(move(gvec));
}

/*
 * Keep the array's cached hash correct across an append in O(1): a still-valid
 * cache just folds the new element in (an append extends the sequence at the
 * end, which is exactly one more hash_combine step). If the cache is invalid,
 * this is a no-op and the next hash() recomputes. See SharedObject::hash_cache.
 */
static void arr_append_maintain_hash(SharedArrayObj &arr, const EvalValue &elem)
{
    if (arr.hash_is_cached()) {
        size_t h = arr.get_cached_hash();
        hash_combine(h, elem.hash());
        arr.store_cached_hash(h);
    }
}

EvalValue builtin_append(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &arr_lval = arg0->eval(ctx);

    if (!arr_lval.is<LValue *>())
        throw NotLValueEx(arg0->start, arg0->end);

    LValue *lval = arr_lval.get<LValue *>();

    if (!lval->is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg0->start, arg0->end);

    if (lval->is_const_var())
        throw CannotChangeConstEx(arg0->start, arg0->end);

    SharedArrayObj &arr = lval->getval<SharedArrayObj>();

    if (arr.is_readonly())
        throw CannotChangeConstEx(arg0->start, arg0->end);

    if (arr.is_slice())
        arr.clone_internal_vec();

    /* Build-hot fast path: `append(flat_struct_arr, S(...))` constructs the new
     * element straight into the byte buffer (no temporary StructObject). The
     * arg is evaluated INSIDE on success; on a miss it is evaluated normally
     * below, so it is never evaluated twice. */
    if (arr.skind() == SharedArrayObj::Storage::structs &&
        try_construct_into_struct_array(ctx, arr, arg1)) {
        arr.invalidate_hash();   /* built in-place; nothing to fold in */
        return lval->get();
    }

    const EvalValue &elem = RValue(arg1->eval(ctx));

    /*
     * Flat fast path: append a matching scalar straight into the unboxed
     * vector, no promotion. Growing in place is sound for aliases (they share
     * the mutation) and slices (they keep their off/len). A non-fitting element
     * on a flat array is the dyn-laundering case - it errors rather than
     * promoting (see flat_array_violation_msg).
     */
    if (arr.skind() == SharedArrayObj::Storage::ints && elem.is<int_type>()) {
        arr.flat_ints().push_back(elem.get<int_type>());
        arr_append_maintain_hash(arr, elem);
        return lval->get();
    }
    if (arr.skind() == SharedArrayObj::Storage::floats &&
        (elem.is<float_type>() || elem.is<int_type>())) {
        arr.flat_floats().push_back(elem.is<int_type>()
            ? static_cast<float_type>(elem.get<int_type>())
            : elem.get<float_type>());
        arr_append_maintain_hash(arr, elem);
        return lval->get();
    }
    if (arr.skind() == SharedArrayObj::Storage::bools && elem.is<bool>()) {
        arr.flat_bools().push_back(elem.get<bool>() ? 1 : 0);
        arr_append_maintain_hash(arr, elem);
        return lval->get();
    }
    /* flat POD-struct array: append the element's bytes (the hot path that
     * keeps a built-up array<Struct> unboxed). */
    if (arr.skind() == SharedArrayObj::Storage::structs &&
        elem.is<intrusive_ptr<StructObject>>() &&
        elem.get<intrusive_ptr<StructObject>>()->is_pod() &&
        elem.get<intrusive_ptr<StructObject>>()->def ==
            arr.flat_structs().def) {
        auto &sv = arr.flat_structs();
        const StructObject &o = *elem.get<intrusive_ptr<StructObject>>().get();
        const size_t at = sv.buf.size();
        sv.buf.resize(at + sv.stride);
        std::memcpy(sv.buf.data() + at, o.bytes.data(), sv.stride);
        arr_append_maintain_hash(arr, elem);
        return lval->get();
    }

    if (arr.skind() != SharedArrayObj::Storage::general)
        throw TypeErrorEx(flat_array_violation_msg, arg0->start, arg1->end);

    arr.get_vec().emplace_back(elem, ctx->const_ctx);
    arr_append_maintain_hash(arr, elem);
    return lval->get();
}

EvalValue builtin_pop(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &arr_lval = arg->eval(ctx);

    if (!arr_lval.is<LValue *>())
        throw NotLValueEx(arg->start, arg->end);

    LValue *lval = arr_lval.get<LValue *>();

    if (!lval->is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg->start, arg->end);

    if (lval->is_const_var())
        throw CannotChangeConstEx(arg->start, arg->end);

    SharedArrayObj &arr = lval->getval<SharedArrayObj>();

    if (arr.is_readonly())
        throw CannotChangeConstEx(arg->start, arg->end);

    const size_type n = arr.size();

    if (!n)
        throw OutOfBoundsEx(arg->start, arg->end);

    arr.invalidate_hash();   /* pop removes the last element */

    /* Read the last element without promoting flat storage. */
    const size_type last_i = arr.offset() + n - 1;
    EvalValue last;
    switch (arr.skind()) {
        case SharedArrayObj::Storage::ints:
            last = EvalValue(arr.flat_ints()[last_i]);   break;
        case SharedArrayObj::Storage::floats:
            last = EvalValue(arr.flat_floats()[last_i]); break;
        case SharedArrayObj::Storage::bools:
            last = EvalValue(static_cast<bool>(arr.flat_bools()[last_i]));
            break;
        default:
            last = arr.get_vec()[last_i].get();          break;
    }

    if (arr.is_slice()) {

        /* Shrink the view by one (shares the parent storage, stays flat). */
        lval->put(SharedArrayObj(arr, arr.offset(), arr.size() - 1));

    } else {

        arr.clone_aliased_slices(last_i);
        switch (arr.skind()) {
            case SharedArrayObj::Storage::ints:
                arr.flat_ints().pop_back();   break;
            case SharedArrayObj::Storage::floats:
                arr.flat_floats().pop_back(); break;
            case SharedArrayObj::Storage::bools:
                arr.flat_bools().pop_back();  break;
            default:
                arr.get_vec().pop_back();     break;
        }
    }

    return last;
}

EvalValue builtin_top(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg->start, arg->end);

    const SharedArrayObj &arr = e.get<SharedArrayObj>();
    const size_type n = arr.size();

    if (!n)
        throw OutOfBoundsEx(arg->start, arg->end);

    return arr_elem_at(arr, n - 1);   /* no promotion of flat storage */
}

EvalValue builtin_erase_arr(LValue *lval, int_type index)
{
    SharedArrayObj &arr = lval->getval<SharedArrayObj>();
    const size_type n = arr.size();

    if (!n || index < 0 || static_cast<size_t>(index) >= n)
        throw OutOfBoundsEx();

    arr.invalidate_hash();   /* any erase changes the array's hash */

    if (arr.is_slice()) {

        /* Erasing the first/last element of a slice is just a narrower view. */
        if (index == 0) {
            lval->put(SharedArrayObj(arr, arr.offset() + 1, n - 1));
            return true;
        }
        if (static_cast<size_type>(index) == n - 1) {
            lval->put(SharedArrayObj(arr, arr.offset(), n - 1));
            return true;
        }
        arr.clone_internal_vec();   /* middle: own the data, keep-flat */

    } else if (static_cast<size_type>(index) == n - 1) {

        /*
         * Erasing the LAST element shifts nothing: only a slice that actually
         * reaches the last index becomes out of bounds, so detach just those.
         */
        arr.clone_aliased_slices(arr.offset() + n - 1);

    } else {

        /*
         * A front/middle erase shifts every element after `index` left by one,
         * so ANY live slice overlapping that region would silently see shifted
         * data (slices must act like independent copies). Detach them all, the
         * way insert() does for a middle insert.
         */
        arr.clone_all_slices();
    }

    /* Erase in place, kind-aware (no promotion of flat storage). */
    const size_type at = arr.offset() + index;
    switch (arr.skind()) {
        case SharedArrayObj::Storage::ints: {
            auto &v = arr.flat_ints();   v.erase(v.begin() + at); break;
        }
        case SharedArrayObj::Storage::floats: {
            auto &v = arr.flat_floats(); v.erase(v.begin() + at); break;
        }
        case SharedArrayObj::Storage::bools: {
            auto &v = arr.flat_bools();  v.erase(v.begin() + at); break;
        }
        default: {
            auto &v = arr.get_vec();     v.erase(v.begin() + at); break;
        }
    }
    return true;
}

EvalValue builtin_insert_arr(LValue *lval, int_type index, const EvalValue &val)
{
    SharedArrayObj &arr = lval->getval<SharedArrayObj>();
    const size_type n = arr.size();

    if (index < 0 || static_cast<size_t>(index) > n)
        throw OutOfBoundsEx();

    arr.invalidate_hash();   /* any insert changes the array's hash */

    if (arr.is_slice())
        arr.clone_internal_vec();          /* keep-flat; standalone, off=0 */
    else if (static_cast<size_type>(index) != n)
        arr.clone_all_slices();

    const size_type at = arr.offset() + index;

    /* Flat in-place insert when the value matches the kind. A non-fitting value
     * on a flat array is the dyn-laundering case - it errors below rather than
     * promoting (an array<dyn> was built general by type-driven creation). */
    if (arr.skind() == SharedArrayObj::Storage::ints && val.is<int_type>()) {
        auto &v = arr.flat_ints();
        v.insert(v.begin() + at, val.get<int_type>());
        return true;
    }
    if (arr.skind() == SharedArrayObj::Storage::floats &&
        (val.is<float_type>() || val.is<int_type>())) {
        auto &v = arr.flat_floats();
        v.insert(v.begin() + at, val.is<int_type>()
            ? static_cast<float_type>(val.get<int_type>())
            : val.get<float_type>());
        return true;
    }
    if (arr.skind() == SharedArrayObj::Storage::bools && val.is<bool>()) {
        auto &v = arr.flat_bools();
        v.insert(v.begin() + at, val.get<bool>() ? 1 : 0);
        return true;
    }

    /* A struct array has no flat insert fast path: fall through to get_vec(),
     * which promotes it to general first (the cold-path cost). */
    if (arr.skind() != SharedArrayObj::Storage::general &&
        arr.skind() != SharedArrayObj::Storage::structs)
        throw TypeErrorEx(flat_array_violation_msg);

    auto &v = arr.get_vec();
    v.insert(v.begin() + at, LValue(val, false));
    return true;
}

EvalValue builtin_range(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1 || exprList->elems.size() > 3)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    int_type end, start = 0, step = 1;
    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<int_type>())
        throw TypeErrorEx("Expected integer", arg0->start, arg0->end);

    if (exprList->elems.size() >= 2) {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &val1 = RValue(arg1->eval(ctx));

        if (!val1.is<int_type>())
            throw TypeErrorEx("Expected integer", arg1->start, arg1->end);

        start = val0.get<int_type>();
        end = val1.get<int_type>();

        if (exprList->elems.size() == 3) {

            Construct *arg2 = exprList->elems[2].get();
            const EvalValue &val2 = RValue(arg2->eval(ctx));

            if (!val2.is<int_type>())
                throw TypeErrorEx("Expected integer", arg2->start, arg2->end);

            step = val2.get<int_type>();

            if (step == 0)
                throw InvalidValueEx("Expected integer != 0", arg2->start, arg2->end);
        }

    } else {

        end = val0.get<int_type>();
    }

    /*
     * range() is all-int, so flat int by default. If the destination is
     * dynamically typed (arr_hint == general, set by the inferencer), build a
     * general array instead - it is created in its final representation, never
     * promoted later.
     */
    if (exprList->arr_hint == ArrHint::general) {

        SharedArrayObj::vec_type vec;
        if (step > 0)
            for (int_type i = start; i < end; i += step)
                vec.emplace_back(EvalValue(i), false);
        else
            for (int_type i = start; i > end; i += step)
                vec.emplace_back(EvalValue(i), false);
        return SharedArrayObj(move(vec));
    }

    SharedArrayObj::ivec_type ivec;

    if (step > 0) {

        for (int_type i = start; i < end; i += step)
            ivec.push_back(i);

    } else {

        for (int_type i = start; i > end; i += step)
            ivec.push_back(i);
    }

    return SharedArrayObj(move(ivec));
}

EvalValue
builtin_find_arr(const SharedArrayObj &arr,
                 const EvalValue &v,
                 FuncObject *key,
                 EvalContext *ctx)
{
    /* Read elements without promoting flat storage (arr_elem_at). */
    const size_type n = arr.size();

    if (key) {

        for (size_type i = 0; i < n; i++) {

            if (eval_func(ctx, *key, arr_elem_at(arr, i)) == v)
                return static_cast<int_type>(i);
        }

        return none;
    }

    /*
     * Flat-scalar fast path: a SAME-typed scalar target scans the unboxed
     * vector with a raw C comparison - no per-element EvalValue boxing and no
     * num_bin_op-dispatched ==, the cost the general loop below pays. A
     * different-typed target (e.g. find(int_array, 2.0)) falls through to the
     * general path, which keeps the cross-type numeric == semantics. This is
     * what makes a flat-array find() as fast as Python's list.index.
     */
    const size_type off = arr.offset();
    if (arr.skind() == SharedArrayObj::Storage::ints && v.is<int_type>()) {
        const int_type t = v.get<int_type>();
        const SharedArrayObj::ivec_type &iv = arr.flat_ints();
        for (size_type i = 0; i < n; i++)
            if (iv[off + i] == t)
                return static_cast<int_type>(i);
        return none;
    }
    if (arr.skind() == SharedArrayObj::Storage::floats && v.is<float_type>()) {
        const float_type t = v.get<float_type>();
        const SharedArrayObj::fvec_type &fv = arr.flat_floats();
        for (size_type i = 0; i < n; i++)
            if (fv[off + i] == t)
                return static_cast<int_type>(i);
        return none;
    }
    if (arr.skind() == SharedArrayObj::Storage::bools && v.is<bool>()) {
        const unsigned char t = v.get<bool>() ? 1 : 0;
        const SharedArrayObj::bvec_type &bv = arr.flat_bools();
        for (size_type i = 0; i < n; i++)
            if (bv[off + i] == t)
                return static_cast<int_type>(i);
        return none;
    }

    for (size_type i = 0; i < n; i++) {
        if (arr_elem_at(arr, i) == v)
            return static_cast<int_type>(i);
    }

    return none;
}

/*
 * Memory-safe heapsort with an arbitrary (possibly non-ordering) user
 * comparator - see the note in sort_arr's comparator branch. Templated on the
 * element vector so it runs over flat int/float storage too (no promotion).
 * cmp(elem, elem) returns true when the first element should sort *after* the
 * second (max-heap order).
 */
template <class Vec, class Cmp>
static void comparator_heapsort(Vec &vec, Cmp cmp)
{
    const size_t n = vec.size();

    auto sift_down = [&](size_t root, size_t end) {
        for (;;) {
            size_t child = 2 * root + 1;
            if (child >= end)
                break;
            if (child + 1 < end && cmp(vec[child], vec[child + 1]))
                child++;                 /* the larger of the children */
            if (!cmp(vec[root], vec[child]))
                break;                   /* root already >= its children */
            std::swap(vec[root], vec[child]);
            root = child;                /* strictly increases -> bounded */
        }
    };

    for (size_t i = n / 2; i > 0; ) {    /* build a max-heap */
        --i;
        sift_down(i, n);
    }
    for (size_t end = n; end > 1; ) {    /* pop the max, n-1 times */
        --end;
        std::swap(vec[0], vec[end]);
        sift_down(0, end);
    }
}

static EvalValue
sort_arr(EvalContext *ctx, ExprList *exprList, bool reverse)
{
    if (exprList->elems.size() == 0)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0_lval = arg0->eval(ctx);
    EvalValue val0 = RValue(val0_lval);

    if (!val0.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg0->start, arg0->end);

    /*
     * Sorting a `const` (a read-only value, or a const-declared variable)
     * sorts a fresh copy and returns it, leaving the original untouched -
     * rather than mutating it in place. clone() yields a mutable copy.
     */
    if (val0.get<SharedArrayObj>().is_readonly()) {
        val0 = val0.clone();
    } else if (val0_lval.is<LValue *>()) {
        if (val0_lval.get<LValue *>()->is_const_var())
            val0 = val0.clone();
    }

    SharedArrayObj &arr = val0.get<SharedArrayObj>();

    if (arr.is_slice()) {

        arr.clone_internal_vec();

        if (val0_lval.is<LValue *>())
            val0_lval.get<LValue *>()->put(arr);

    } else {

        arr.clone_all_slices();
    }

    arr.invalidate_hash();   /* sort reorders -> order-dependent hash changes */

    if (exprList->elems.size() == 1) {

        /*
         * Flat fast path: sort the unboxed int/float vector directly with the
         * native <. std::sort is safe here - the default ordering on a
         * homogeneous scalar type is a valid strict weak ordering (the
         * unguarded-partition hazard only applies to the user-comparator path
         * below).
         */
        switch (arr.skind()) {
            case SharedArrayObj::Storage::ints: {
                auto &v = arr.flat_ints();
                if (!reverse)
                    sort(v.begin(), v.end());
                else
                    sort(v.begin(), v.end(), std::greater<int_type>());
                return arr;
            }
            case SharedArrayObj::Storage::floats: {
                auto &v = arr.flat_floats();
                if (!reverse)
                    sort(v.begin(), v.end());
                else
                    sort(v.begin(), v.end(), std::greater<float_type>());
                return arr;
            }
            case SharedArrayObj::Storage::bools: {
                auto &v = arr.flat_bools();   /* 0/1 sorts as false<true */
                if (!reverse)
                    sort(v.begin(), v.end());
                else
                    sort(v.begin(), v.end(), std::greater<unsigned char>());
                return arr;
            }
            default:
                break;
        }

        auto &vec = arr.get_vec();

        if (!reverse) {

            sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
                return a.get() < b.get();
            });

        } else {

            sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
                return a.get() > b.get();
            });
        }

    } else {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &val1 = RValue(arg1->eval(ctx));

        if (!val1.is<shared_ptr<FuncObject>>())
            throw TypeErrorEx("Expected function", arg1->start, arg1->end);

        FuncObject &funcObj = *val1.get<shared_ptr<FuncObject>>().get();

        /*
         * A user comparator is arbitrary script code, so it need NOT be a valid
         * strict weak ordering; comparator_heapsort stays in-bounds and
         * terminates for ANY comparator (see its note). Run it directly over
         * the unboxed int/float vector when flat - no promotion.
         */
        switch (arr.skind()) {
            case SharedArrayObj::Storage::ints: {
                auto &v = arr.flat_ints();
                comparator_heapsort(v, [&](int_type a, int_type b) {
                    const bool lt = eval_func(ctx, funcObj,
                        make_pair(EvalValue(a), EvalValue(b))).is_true();
                    return reverse ? !lt : lt;
                });
                break;
            }
            case SharedArrayObj::Storage::floats: {
                auto &v = arr.flat_floats();
                comparator_heapsort(v, [&](float_type a, float_type b) {
                    const bool lt = eval_func(ctx, funcObj,
                        make_pair(EvalValue(a), EvalValue(b))).is_true();
                    return reverse ? !lt : lt;
                });
                break;
            }
            case SharedArrayObj::Storage::bools: {
                auto &v = arr.flat_bools();
                comparator_heapsort(v, [&](unsigned char a, unsigned char b) {
                    const bool lt = eval_func(ctx, funcObj,
                        make_pair(EvalValue(static_cast<bool>(a)),
                                  EvalValue(static_cast<bool>(b)))).is_true();
                    return reverse ? !lt : lt;
                });
                break;
            }
            default: {
                auto &vec = arr.get_vec();
                comparator_heapsort(vec, [&](const LValue &a, const LValue &b) {
                    const bool lt = eval_func(ctx, funcObj,
                        make_pair(a.get(), b.get())).is_true();
                    return reverse ? !lt : lt;
                });
                break;
            }
        }
    }

    return arr;
}

EvalValue builtin_sort(EvalContext *ctx, ExprList *exprList)
{
    return sort_arr(ctx, exprList, false);
}

EvalValue builtin_rev_sort(EvalContext *ctx, ExprList *exprList)
{
    return sort_arr(ctx, exprList, true);
}

EvalValue builtin_reverse(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0_lval = arg0->eval(ctx);
    EvalValue val0 = RValue(val0_lval);

    if (!val0.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg0->start, arg0->end);

    SharedArrayObj &arr = val0.get<SharedArrayObj>();

    if (arr.is_slice()) {

        arr.clone_internal_vec();

        if (val0_lval.is<LValue *>())
            val0_lval.get<LValue *>()->put(arr);

    } else {

        arr.clone_all_slices();
    }

    arr.invalidate_hash();   /* reverse changes the order-dependent hash */

    /* Flat fast path: reverse the unboxed vector in place (8-byte swaps). */
    switch (arr.skind()) {
        case SharedArrayObj::Storage::ints: {
            auto &v = arr.flat_ints();
            reverse(v.begin(), v.end());
            break;
        }
        case SharedArrayObj::Storage::floats: {
            auto &v = arr.flat_floats();
            reverse(v.begin(), v.end());
            break;
        }
        case SharedArrayObj::Storage::bools: {
            auto &v = arr.flat_bools();
            reverse(v.begin(), v.end());
            break;
        }
        default: {
            auto &vec = arr.get_vec();
            reverse(vec.begin(), vec.end());
            break;
        }
    }

    return arr;
}

EvalValue builtin_sum(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() < 1 || exprList->elems.size() > 2)
        throw InvalidArgumentEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    const EvalValue &val0 = RValue(arg0->eval(ctx));

    if (!val0.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", arg0->start, arg0->end);

    const SharedArrayObj &arr = val0.get<SharedArrayObj>();

    /*
     * Flat (unboxed) fast path: sum the int/float vector directly, with no
     * promotion to vector<LValue> and no per-element virtual dispatch. Only the
     * 1-arg (no callback) form - a user reducer needs boxed EvalValues.
     */
    if (exprList->elems.size() == 1 &&
        arr.skind() != SharedArrayObj::Storage::general)
    {
        const size_type off = arr.offset(), n = arr.size();

        if (n == 0)
            return none;

        if (arr.skind() == SharedArrayObj::Storage::ints) {
            const auto &iv = arr.flat_ints();
            int_type acc = 0;
            for (size_type i = 0; i < n; i++)
                acc += iv[off + i];           /* wraps (-fwrapv), like += */
            return EvalValue(acc);
        }

        if (arr.skind() == SharedArrayObj::Storage::bools) {
            /* bool promotes to int: sum counts the `true`s, as an int. */
            const auto &bv = arr.flat_bools();
            int_type acc = 0;
            for (size_type i = 0; i < n; i++)
                acc += bv[off + i] ? 1 : 0;
            return EvalValue(acc);
        }

        const auto &fv = arr.flat_floats();
        float_type acc = 0;
        for (size_type i = 0; i < n; i++)
            acc += fv[off + i];
        return EvalValue(acc);
    }

    const ArrayConstView &view = arr.get_view();

    if (view.size() == 0)
        return none; /* sum of an empty array is none, like min()/max() */

    if (exprList->elems.size() == 1) {

        const EvalValue &first = view[0].get();

        /*
         * Fast path for an all-int array: accumulate raw int_type in a tight
         * loop, skipping num_bin_op's promotion check and the per-element
         * virtual TypeInt::add dispatch. Overflow wraps (-fwrapv), exactly as
         * TypeInt::add does. The first non-int (a float, say) breaks out and
         * the general loop below resumes from there, so a mixed array still
         * promotes correctly (int accumulator -> float via num_bin_op).
         */
        if (first.is<int_type>()) {

            int_type acc = first.get<int_type>();
            size_type i = 1;

            for (; i < view.size(); i++) {
                const EvalValue &e = view[i].get();
                if (!e.is<int_type>())
                    break;
                acc += e.get<int_type>();
            }

            if (i == view.size())
                return EvalValue(acc);

            EvalValue val(acc);
            for (; i < view.size(); i++)
                num_bin_op(val, view[i].get(), &Type::add);
            return val;
        }

        /*
         * General path. Seed the accumulator with a *copy* of the first elem:
         * num_bin_op with Type::add mutates the accumulator in place (array
         * `+=` appends to it), so aliasing view[0] would mutate the input array
         * - and would be rejected outright when the input is a read-only const.
         */
        EvalValue val = first.clone();

        for (size_type i = 1; i < view.size(); i++) {
            num_bin_op(val, view[i].get(), &Type::add);
        }

        return val;

    } else {

        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &val1 = RValue(arg1->eval(ctx));

        if (!val1.is<shared_ptr<FuncObject>>())
            throw TypeErrorEx("Expected function", arg1->start, arg1->end);

        FuncObject &funcObj = *val1.get<shared_ptr<FuncObject>>().get();
        EvalValue val = eval_func(ctx, funcObj, view[0].get());

        for (size_type i = 1; i < view.size(); i++) {
            num_bin_op(
                val,
                eval_func(ctx, funcObj, view[i].get()),
                &Type::add
            );
        }

        return val;
    }
}
