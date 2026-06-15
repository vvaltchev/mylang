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

EvalValue builtin_array(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<int_type>())
        throw TypeErrorEx("Expected integer", arg->start, arg->end);

    const int_type n = e.get<int_type>();

    if (n < 0)
        throw InvalidValueEx("Expected non-negative integer", arg->start, arg->end);

    SharedArrayObj::vec_type vec;

    for (int_type i = 0; i < n; i++)
        vec.emplace_back(none, ctx->const_ctx);

    return SharedArrayObj(move(vec));
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

    const ArrayConstView &view = arr.get_view();

    if (!view.size())
        throw OutOfBoundsEx(arg->start, arg->end);

    EvalValue last = view[view.size() - 1].get();

    if (arr.is_slice()) {

        lval->put(SharedArrayObj(arr, arr.offset(), arr.size() - 1));

    } else {

        arr.clone_aliased_slices(arr.offset() + arr.size() - 1);
        arr.get_vec().pop_back();
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

    SharedArrayObj::vec_type vec;

    if (step > 0) {

        for (int_type i = start; i < end; i += step)
            vec.emplace_back(EvalValue(i), ctx->const_ctx);

    } else {

        for (int_type i = start; i > end; i += step)
            vec.emplace_back(EvalValue(i), ctx->const_ctx);
    }

    return SharedArrayObj(move(vec));
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

    auto &vec = arr.get_vec();

    if (exprList->elems.size() == 1) {

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

    auto &vec = arr.get_vec();
    reverse(vec.begin(), vec.end());
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
