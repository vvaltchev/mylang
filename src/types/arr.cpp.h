/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "evaltypes.cpp.h"
#include "structtype.h"
#include <cstring>

/* Materialize the POD-struct element at index `i` (already offset-adjusted)
 * flat struct array as a boxed StructObject EvalValue (a copy of its bytes). */
static EvalValue struct_elem_at(const SharedArrayObj &arr, size_type i)
{
    const SharedArrayObj::svec_type &sv = arr.flat_structs();
    ML_CHECK(sv.stride > 0 && i < arr.size());
    /* the element's bytes [start, start+stride) must lie within buf */
    ML_CHECK((static_cast<size_t>(arr.offset() + i) + 1) *
                 static_cast<size_t>(sv.stride) <= sv.buf.size());
    auto obj = make_intrusive<StructObject>(sv.def);   /* resizes bytes */
    std::memcpy(obj->bytes.data(),
                sv.buf.data() + (arr.offset() + i) * sv.stride, sv.stride);
    return intrusive_ptr<StructObject>(obj);
}

template <class LValueT>
void SharedArrayObjTempl<LValueT>::promote_structs_to_general()
{
    if (shobj->kind != Storage::structs)
        return;

    const size_type n = size();
    vec_type nv;
    nv.reserve(n);
    for (size_type i = 0; i < n; i++)
        nv.emplace_back(struct_elem_at(*this, i), false);

    *this = SharedArrayObjTempl(move(nv));
}

template <class LValueT>
void SharedArrayObjTempl<LValueT>::clone_internal_vec()
{
    /*
     * Use offset()/size(), not the raw off/len fields: for a non-slice, `len`
     * is only the size at construction and goes stale when `+=` appends to the
     * vector in place (a non-slice tracks its length via vec.size(), see
     * size()). offset()/size() are correct for both slices and non-slices.
     *
     * Kind-aware: a flat int/float array clones into a fresh *flat* array, so
     * the cheap representation survives clone() and copy-on-write (a const flat
     * array stays flat, `a.clone()` keeps it flat). Only a general array clones
     * into vector<LValue>.
     */
    switch (shobj->kind) {

        case Storage::ints: {
            ivec_type nv(
                shobj->ivec.cbegin() + offset(),
                shobj->ivec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(move(nv));
            return;
        }

        case Storage::floats: {
            fvec_type nv(
                shobj->fvec.cbegin() + offset(),
                shobj->fvec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(move(nv));
            return;
        }

        case Storage::bools: {
            bvec_type nv(
                shobj->bvec.cbegin() + offset(),
                shobj->bvec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(move(nv));
            return;
        }

        case Storage::structs: {
            const int stride = shobj->svec.stride;
            std::vector<char> nb(
                shobj->svec.buf.cbegin() + offset() * stride,
                shobj->svec.buf.cbegin() + (offset() + size()) * stride
            );
            *this = SharedArrayObjTempl(
                svec_type(move(nb), shobj->svec.def, stride));
            return;
        }

        default:
            break;
    }

    vec_type new_vec(
        shobj->vec.cbegin() + offset(),
        shobj->vec.cbegin() + offset() + size()
    );

    *this = SharedArrayObjTempl(move(new_vec));
}

template <class LValueT>
void SharedArrayObjTempl<LValueT>::clone_aliased_slices(size_type index)
{
    auto &slices = shobj->slices;

    for (auto it = slices.begin(); it != slices.end(); /* no inc */) {

        auto obj = *it;

        if (index == all_slices ||
            (obj->off <= index && index < obj->off + obj->len))
        {
            /*
             * Erase the object from here and get an iterator for the next
             * object in the container.
             */
            it = shobj->slices.erase(it);
            assert(obj->slice);

            /*
             * Clone the slice's range into its own vector. We leave obj->slice
             * set so clone_internal_vec's offset()/size() report the slice
             * range; the move-assign it does turns obj into a standalone
             * non-slice, and obj was already removed from the slices set above,
             * so nothing tries to erase it again.
             */
            obj->clone_internal_vec();

        } else {

            /* Just move to the next slice */
            ++it;
        }
    }
}

/* Read element `i` (slice-relative) as a boxed EvalValue WITHOUT promoting flat
 * storage (defined below). */
static EvalValue arr_elem_at(const SharedArrayObj &arr, size_type i);


class TypeArr : public TypeImpl<SharedArrayObj> {

public:

    TypeArr()
        : TypeImpl<SharedArrayObj>(Type::t_arr)
    { }

    void add(EvalValue &a, const EvalValue &b) override;
    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    size_t hash(const EvalValue &a) override;
    EvalValue subscript(const EvalValue &what, const EvalValue &idx,
                        bool for_write = false) override;
    EvalValue slice(const EvalValue &what,
                    const EvalValue &start,
                    const EvalValue &end) override;

    int_type use_count(const EvalValue &a) override;
    bool is_slice(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;

    int_type len(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    string pretty(const EvalValue &a, int indent, int width) override;
    bool is_true(const EvalValue &a) override;
};

int_type TypeArr::len(const EvalValue &a)
{
    return a.get<SharedArrayObj>().size();
}

int_type TypeArr::use_count(const EvalValue &a)
{
    return a.get<SharedArrayObj>().use_count();
}

bool TypeArr::is_slice(const EvalValue &a)
{
    return a.get<SharedArrayObj>().is_slice();
}

EvalValue TypeArr::clone(const EvalValue &a)
{
    /* The ONLY way to get a copy of the internal SharedArrayObj */
    EvalValue new_val = a;
    new_val.get<SharedArrayObj>().clone_internal_vec();
    return new_val;
}

EvalValue TypeArr::intptr(const EvalValue &a)
{
    /* The address of the live backing vector (kind-aware, no promotion): two
     * slices of the same array share one backing object, so they report the
     * same intptr - which is what the COW tests check. */
    const SharedArrayObj &arr = a.get<SharedArrayObj>();
    switch (arr.skind()) {
        case SharedArrayObj::Storage::ints:
            return reinterpret_cast<int_type>(&arr.flat_ints());
        case SharedArrayObj::Storage::floats:
            return reinterpret_cast<int_type>(&arr.flat_floats());
        case SharedArrayObj::Storage::bools:
            return reinterpret_cast<int_type>(&arr.flat_bools());
        default:
            return reinterpret_cast<int_type>(&arr.get_vec());
    }
}

void TypeArr::add(EvalValue &a, const EvalValue &b)
{
    SharedArrayObj &lval = a.get<SharedArrayObj>();

    if (lval.is_readonly())
        throw CannotChangeConstEx();

    if (!b.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array on the right side of +");

    lval.invalidate_hash();   /* += appends elements -> the hash changes */

    const SharedArrayObj &rhs = b.get<SharedArrayObj>();

    /*
     * Flat homogeneous fast path: int+int / float+float concatenate in unboxed
     * storage, with no promotion (8-byte appends). A mismatched or general
     * operand falls through to the boxed path below, which reads both operands
     * directly (arr_elem_at) and builds the result general - it never promotes
     * an operand in place. A non-slice lhs appends in place (same alias
     * semantics as before); a slice lhs builds a fresh flat array.
     */
    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::ints)
    {
        const auto &rv = rhs.flat_ints();

        if (!lval.is_slice()) {

            auto &lv = lval.flat_ints();
            lv.reserve(lval.size() + rhs.size());
            lv.insert(lv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());

        } else {

            const auto &lv = lval.flat_ints();
            SharedArrayObj::ivec_type nv;
            nv.reserve(lval.size() + rhs.size());
            nv.insert(nv.end(),
                      lv.cbegin() + lval.offset(),
                      lv.cbegin() + lval.offset() + lval.size());
            nv.insert(nv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());
            lval = SharedArrayObj(move(nv));
        }

        return;
    }

    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::floats)
    {
        const auto &rv = rhs.flat_floats();

        if (!lval.is_slice()) {

            auto &lv = lval.flat_floats();
            lv.reserve(lval.size() + rhs.size());
            lv.insert(lv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());

        } else {

            const auto &lv = lval.flat_floats();
            SharedArrayObj::fvec_type nv;
            nv.reserve(lval.size() + rhs.size());
            nv.insert(nv.end(),
                      lv.cbegin() + lval.offset(),
                      lv.cbegin() + lval.offset() + lval.size());
            nv.insert(nv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());
            lval = SharedArrayObj(move(nv));
        }

        return;
    }

    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::bools)
    {
        const auto &rv = rhs.flat_bools();

        if (!lval.is_slice()) {

            auto &lv = lval.flat_bools();
            lv.reserve(lval.size() + rhs.size());
            lv.insert(lv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());

        } else {

            const auto &lv = lval.flat_bools();
            SharedArrayObj::bvec_type nv;
            nv.reserve(lval.size() + rhs.size());
            nv.insert(nv.end(),
                      lv.cbegin() + lval.offset(),
                      lv.cbegin() + lval.offset() + lval.size());
            nv.insert(nv.end(),
                      rv.cbegin() + rhs.offset(),
                      rv.cbegin() + rhs.offset() + rhs.size());
            lval = SharedArrayObj(move(nv));
        }

        return;
    }

    /*
     * Mixed / general concat. The result is a general array. If lhs is already
     * a general non-slice, append rhs's elements in place; otherwise build a
     * fresh general array. Either way both operands are read via arr_elem_at,
     * so a flat operand is read directly (its representation is never promoted).
     */
    const size_type rn = rhs.size();

    if (lval.skind() == SharedArrayObj::Storage::general && !lval.is_slice()) {

        auto &v = lval.get_vec();   /* general: get_vec() doesn't promote */
        v.reserve(lval.size() + rn);
        for (size_type i = 0; i < rn; i++)
            v.emplace_back(arr_elem_at(rhs, i), false);

    } else {

        const size_type ln = lval.size();
        SharedArrayObj::vec_type new_arr;
        new_arr.reserve(ln + rn);
        for (size_type i = 0; i < ln; i++)
            new_arr.emplace_back(arr_elem_at(lval, i), false);
        for (size_type i = 0; i < rn; i++)
            new_arr.emplace_back(arr_elem_at(rhs, i), false);
        lval = SharedArrayObj(move(new_arr));
    }
}

/*
 * Read element `i` (slice-relative) of an array as a boxed EvalValue WITHOUT
 * promoting flat storage. For general storage this is the existing element read
 * (get_vec() doesn't promote a general array).
 */
static EvalValue arr_elem_at(const SharedArrayObj &arr, size_type i)
{
    ML_CHECK(i < arr.size());
    const size_type at = arr.offset() + i;
    switch (arr.skind()) {
        case SharedArrayObj::Storage::ints:
            return EvalValue(arr.flat_ints()[at]);
        case SharedArrayObj::Storage::floats:
            return EvalValue(arr.flat_floats()[at]);
        case SharedArrayObj::Storage::bools:
            return EvalValue(static_cast<bool>(arr.flat_bools()[at]));
        case SharedArrayObj::Storage::structs:
            return struct_elem_at(arr, i);   /* materialize a StructObject */
        default:
            return arr.get_vec()[at].get();
    }
}

void TypeArr::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<SharedArrayObj>()) {
        a = false;
        return;
    }

    const SharedArrayObj &lhs = a.get<SharedArrayObj>();
    const SharedArrayObj &rhs = b.get<SharedArrayObj>();
    const size_type n = lhs.size();

    if (n != rhs.size()) {
        a = false;
        return;
    }

    /*
     * Identity shortcut: only safe (and only worth it) when both are general -
     * the same vector AND same offset means the two views cover the exact same
     * region, hence equal. Reading get_vec() here doesn't promote a general
     * array. Flat arrays skip this and use the element loop below.
     */
    if (lhs.skind() == SharedArrayObj::Storage::general &&
        rhs.skind() == SharedArrayObj::Storage::general &&
        &lhs.get_vec() == &rhs.get_vec() && lhs.offset() == rhs.offset())
    {
        a = true;
        return;
    }

    /* Element-wise compare, reading each side without promoting (so two flat
     * arrays - or a flat and a general one - compare equal element by element,
     * with the usual 1 == 1.0 numeric equality via EvalValue::operator!=). */
    for (size_type i = 0; i < n; i++) {

        if (arr_elem_at(lhs, i) != arr_elem_at(rhs, i)) {
            a = false;
            return;
        }
    }

    a = true;
}

void TypeArr::noteq(EvalValue &a, const EvalValue &b)
{
    eq(a, b);
    a = !a.is_true();   /* eq() yields a bool; negate it (stays bool) */
}

/*
 * Deep, ORDER-dependent hash of an array: combine the element hashes in order
 * (an array is a sequence, so `[1,2]` and `[2,1]` differ). `arr_elem_at` boxes
 * a flat int/float/bool/struct element, so this works for every storage kind
 * and reuses the per-element hashes (`1 == 1.0` share a hash, as in eq).
 */
size_t TypeArr::hash(const EvalValue &a)
{
    const SharedArrayObj &arr = a.get<SharedArrayObj>();

    if (arr.hash_is_cached())          /* a non-slice with a valid cache */
        return arr.get_cached_hash();

    const size_type n = arr.size();
    size_t seed = hash_salt_array;

    for (size_type i = 0; i < n; i++)
        hash_combine(seed, arr_elem_at(arr, i).hash());

    arr.store_cached_hash(seed);       /* no-op for a slice */
    return seed;
}

string TypeArr::to_string(const EvalValue &a)
{
    const SharedArrayObj &arr = a.get<SharedArrayObj>();
    const size_type n = arr.size();
    string res;

    res.reserve(n * 32);
    res += "[";

    /* Flat fast path: stringify the unboxed vector directly, no promotion. */
    if (arr.skind() != SharedArrayObj::Storage::general) {

        for (size_type i = 0; i < n; i++) {
            res += arr_elem_at(arr, i).to_string_repr();
            if (i != n - 1)
                res += ", ";
        }

        res += "]";
        return res;
    }

    const ArrayConstView &arr_view = arr.get_view();

    for (size_type i = 0; i < arr_view.size(); i++) {

        const EvalValue &val = arr_view[i].get();
        res += val.to_string_repr();

        if (i != arr_view.size() - 1)
            res += ", ";
    }

    res += "]";
    return res;
}

string TypeArr::pretty(const EvalValue &a, int indent, int width)
{
    const string flat = to_string_repr(a);
    const SharedArrayObj &arr = a.get<SharedArrayObj>();
    const size_type n = arr.size();

    /* fits on one line (or empty) -> single line */
    if (n == 0 || indent + static_cast<int>(flat.size()) <= width)
        return flat;

    /* otherwise expand one element per line, indented */
    string res = "[\n";
    const string pad(indent + 2, ' ');
    for (size_type i = 0; i < n; i++) {
        res += pad;
        res += arr_elem_at(arr, i).pretty(indent + 2, width);
        if (i != n - 1)
            res += ",";
        res += "\n";
    }
    res += string(indent, ' ');
    res += "]";
    return res;
}

bool TypeArr::is_true(const EvalValue &a)
{
    return a.get<SharedArrayObj>().size() > 0;
}

EvalValue TypeArr::subscript(const EvalValue &what_lval,
                             const EvalValue &idx_val, bool for_write)
{
    if (!idx_val.is<int_type>())
        throw TypeErrorEx("Expected integer as subscript");

    const EvalValue &what = RValue(what_lval);
    SharedArrayObj &&arr = what.get<SharedArrayObj>();
    int_type idx = idx_val.get<int_type>();

    if (idx < 0)
        idx += arr.size();

    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx();

    /*
     * Flat (unboxed) storage: return the scalar as an rvalue, without promoting
     * to vector<LValue>. A flat element has no LValue to point at, but it never
     * needs one: `a[i] = v` on a flat array is intercepted upstream by
     * try_flat_subscript_store (eval.cpp), and a flat element is a scalar, so
     * it can't be a mutate-in-place container target either. So every read that
     * reaches here (print(a[i]), a dyn context, a builtin arg, ...) stays flat.
     */
    if (arr.skind() != SharedArrayObj::Storage::general)
        return arr_elem_at(arr, idx);

    SharedArrayObj::vec_type &vec = arr.get_vec();
    LValue *ret = &vec[arr.offset() + idx];

    if (!what_lval.is<LValue *>() || arr.is_readonly()) {
        /*
         * Return a simple RValue when the input array was not an LValue, or
         * when it is read-only (a `const` value): a read still works, but an
         * assignment target is an rvalue, so `a[i] = x` fails with NotLValueEx.
         */
        return ret->get();
    }

    /* We deferenced a LValue array, so return element's LValue */
    ret->container = what_lval.get<LValue *>();
    ret->container_idx = arr.offset() + idx;
    return ret;
}

EvalValue TypeArr::slice(const EvalValue &what_lval,
                         const EvalValue &start_val,
                         const EvalValue &end_val)
{
    const EvalValue &what = RValue(what_lval);
    const SharedArrayObj &arr = what.get<SharedArrayObj>();
    int_type start = 0, end = arr.size();

    if (start_val.is<int_type>()) {

        start = start_val.get<int_type>();

        if (start < 0) {

            start += arr.size();

            if (start < 0)
                start = 0;
        }

        if (static_cast<size_t>(start) >= arr.size())
            return empty_arr;

    } else if (!start_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range start");
    }

    if (end_val.is<int_type>()) {

        end = end_val.get<int_type>();

        if (end < 0)
            end += arr.size();

        if (end <= start)
            return empty_arr;

        if (static_cast<size_t>(end) > arr.size())
            end = arr.size();

    } else if (!end_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range end");
    }

    return SharedArrayObj(arr, arr.offset() + start, end - start);
}
