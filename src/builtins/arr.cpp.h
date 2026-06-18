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

    if (nargs == 1) {

        /* No fill value: a general array of `none`. */
        SharedArrayObj::vec_type vec;
        vec.reserve(n);

        for (int_type i = 0; i < n; i++)
            vec.emplace_back(none, ctx->const_ctx);

        return SharedArrayObj(move(vec));
    }

    const EvalValue &v = RValue(exprList->elems[1]->eval(ctx));

    /* Value-driven flat storage for a scalar fill value. */
    if (v.is<int_type>())
        return SharedArrayObj(
            SharedArrayObj::ivec_type(n, v.get<int_type>()));

    if (v.is<float_type>())
        return SharedArrayObj(
            SharedArrayObj::fvec_type(n, v.get<float_type>()));

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
    SharedArrayObj::vec_type  gvec;
    int mode = 0;

    auto spill_to_general = [&]() {
        gvec.reserve(n);
        if (mode == 1)
            for (int_type x : ivec) gvec.emplace_back(EvalValue(x), false);
        else if (mode == 2)
            for (float_type x : fvec) gvec.emplace_back(EvalValue(x), false);
        ivec.clear();
        fvec.clear();
        mode = 3;
    };

    for (int_type i = 0; i < n; i++) {

        const EvalValue r = eval_func(ctx, funcObj, EvalValue(i));

        if (mode == 0) {
            if (r.is<int_type>()) {
                mode = 1; ivec.push_back(r.get<int_type>());
            } else if (r.is<float_type>()) {
                mode = 2; fvec.push_back(r.get<float_type>());
            } else {
                mode = 3; gvec.reserve(n); gvec.emplace_back(r, false);
            }
        } else if (mode == 1 && r.is<int_type>()) {
            ivec.push_back(r.get<int_type>());
        } else if (mode == 2 && r.is<float_type>()) {
            fvec.push_back(r.get<float_type>());
        } else {
            if (mode != 3)
                spill_to_general();
            gvec.emplace_back(r, false);
        }
    }

    if (mode == 1) return SharedArrayObj(move(ivec));
    if (mode == 2) return SharedArrayObj(move(fvec));
    return SharedArrayObj(move(gvec));
}

/*
 * Introspection: report an array's backing-storage specialization as a string -
 * "ints" / "floats" for flat (unboxed) storage, "general" for vector<LValue>.
 * type() can't express this (flat and general are the same t_arr); this exists
 * mainly so tests can pin the representation and catch regressions. Not const:
 * it reflects a runtime fact and must not fold.
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
            return SharedStr(string("ints"));
        case SharedArrayObj::Storage::floats:
            return SharedStr(string("floats"));
        default:
            return SharedStr(string("general"));
    }
}

EvalValue builtin_append(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg0 = exprList->elems[0].get();
    Construct *arg1 = exprList->elems[1].get();
    const EvalValue &arr_lval = arg0->eval(ctx);
    const EvalValue &elem = RValue(arg1->eval(ctx));

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

    /*
     * Flat fast path: append a matching scalar straight into the unboxed
     * vector, no promotion. A mismatched element type falls through to the
     * general append below (which promotes). Growing in place is sound for
     * aliases (they share the mutation) and slices (they keep their off/len).
     */
    if (arr.skind() == SharedArrayObj::Storage::ints && elem.is<int_type>()) {
        arr.flat_ints().push_back(elem.get<int_type>());
        return lval->get();
    }
    if (arr.skind() == SharedArrayObj::Storage::floats &&
        (elem.is<float_type>() || elem.is<int_type>())) {
        arr.flat_floats().push_back(elem.is<int_type>()
            ? static_cast<float_type>(elem.get<int_type>())
            : elem.get<float_type>());
        return lval->get();
    }

    arr.get_vec().emplace_back(elem, ctx->const_ctx);
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

    /* Read the last element without promoting flat storage. */
    const size_type last_i = arr.offset() + n - 1;
    EvalValue last;
    switch (arr.skind()) {
        case SharedArrayObj::Storage::ints:
            last = EvalValue(arr.flat_ints()[last_i]);   break;
        case SharedArrayObj::Storage::floats:
            last = EvalValue(arr.flat_floats()[last_i]); break;
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

    const ArrayConstView &view = e.get<SharedArrayObj>().get_view();

    if (!view.size())
        throw OutOfBoundsEx(arg->start, arg->end);

    return view[view.size() - 1].get();
}

EvalValue builtin_erase_arr(LValue *lval, int_type index)
{
    SharedArrayObj &arr = lval->getval<SharedArrayObj>();
    const ArrayConstView &view = arr.get_view();

    if (!view.size())
        throw OutOfBoundsEx();

    if (index < 0)
        throw OutOfBoundsEx();

    if (static_cast<size_t>(index) >= view.size())
        throw OutOfBoundsEx();

    if (arr.is_slice()) {

        if (index == 0) {

            lval->put(SharedArrayObj(arr, arr.offset() + 1, arr.size() - 1));

        } else if (index == view.size() - 1) {

            lval->put(SharedArrayObj(arr, arr.offset(), arr.size() - 1));

        } else {

            arr.clone_internal_vec();
            auto &vec = arr.get_vec();
            vec.erase(vec.begin() + arr.offset() + index);
            lval->put(SharedArrayObj(move(vec)));
        }

    } else {

        arr.clone_aliased_slices(arr.offset() + arr.size() - 1);
        arr.get_vec().erase(arr.get_vec().begin() + arr.offset() + index);
    }

    return true;
}

EvalValue builtin_insert_arr(LValue *lval, int_type index, const EvalValue &val)
{
    SharedArrayObj &arr = lval->getval<SharedArrayObj>();
    const ArrayConstView &view = arr.get_view();

    if (index < 0)
        throw OutOfBoundsEx();

    if (static_cast<size_t>(index) > view.size())
        throw OutOfBoundsEx();

    if (arr.is_slice()) {

        arr.clone_internal_vec();
        auto &vec = arr.get_vec();
        vec.insert(vec.begin() + index, LValue(val, false));
        lval->put(SharedArrayObj(move(vec)));

    } else {

        if (index != view.size())
            arr.clone_all_slices();

        arr.get_vec().insert(arr.get_vec().begin() + index, LValue(val, false));
    }

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

    /* range() is all-int by construction, so build flat (unboxed) int storage
     * directly - see plans/typed-arrays.md. */
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
    const ArrayConstView &view = arr.get_view();

    if (key) {

        for (size_type i = 0; i < view.size(); i++) {

            if (eval_func(ctx, *key, view[i].get()) == v)
                return static_cast<int_type>(i);
        }

    } else {

        for (size_type i = 0; i < view.size(); i++) {
            if (view[i].get() == v)
                return static_cast<int_type>(i);
        }
    }

    return none;
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

        auto &vec = arr.get_vec();
        Construct *arg1 = exprList->elems[1].get();
        const EvalValue &val1 = RValue(arg1->eval(ctx));

        if (!val1.is<shared_ptr<FuncObject>>())
            throw TypeErrorEx("Expected function", arg1->start, arg1->end);

        FuncObject &funcObj = *val1.get<shared_ptr<FuncObject>>().get();

        auto cmp = [&](const auto &a, const auto &b) {
            const bool lt =
                eval_func(ctx, funcObj, make_pair(a.get(), b.get())).is_true();
            return reverse ? !lt : lt;
        };

        /*
         * A user comparator is arbitrary script code, so it need NOT be a valid
         * strict weak ordering (e.g. `func(a, b) => a != b`, or one that varies
         * with call order). std::sort's introsort would run its *unguarded*
         * partition/insertion past the ends of the buffer once that assumption
         * breaks - a heap-buffer-overflow reachable straight from a script. We
         * therefore hand-roll a heapsort: `sift_down`'s root index strictly
         * descends, so it terminates for ANY comparator, and it only ever
         * indexes within [0, n), so a bogus comparator yields an
         * unspecified-but-memory-safe order instead of UB. (We do NOT use
         * std::make_heap/std::sort_heap: MSVC's debug STL wraps them in
         * comparator-validity instrumentation that hangs on a non-ordering
         * comparator. Same O(n log n) comparisons; for a valid comparator the
         * result is identical - neither is stable.)
         */
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
