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
#include <algorithm>
#include <cstring>
#include <vector>

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

#ifdef TESTS
uint32_t g_slice_group_limit = UINT32_MAX;
#endif

/* The flat CLASS storage's counts (sharedarray.h cannot see StructObject) */
void sobj_retain(StructObject *p) noexcept
{
    if (p)
        ++p->intr_refcount;
}

void sobj_release(StructObject *p) noexcept
{
    if (p) {
        ML_CHECK(p->intr_refcount > 0);
        if (--p->intr_refcount == 0)
            destroy_counted(p);         /* bounded: intrusiveptr.h */
    }
}

/*
 * A fresh GENERAL array holding this array's elements (this handle's range),
 * leaving the storage every handle shares untouched - for a READ-only walk
 * that wants the general view of a flat struct/strs array (sum/min/max).
 */
template <class LValueT>
SharedArrayObjTempl<LValueT> SharedArrayObjTempl<LValueT>::general_copy() const
{
    const size_type n = size();
    vec_type nv;
    nv.reserve(n);
    for (size_type i = 0; i < n; i++)
        nv.emplace_back(arr_elem_at(*this, i), false);
    return SharedArrayObjTempl(std::move(nv));
}

/*
 * #53b: the promotion is IN PLACE, on the storage every handle shares.
 *
 * It used to RESEAT this handle onto a fresh general copy - so a cold op
 * (pop/erase/insert/sort/reverse) on a flat struct or strs array through
 * one variable left every OTHER variable holding the old storage, and
 * MyLang's reference semantics ("assignment aliases") silently broke:
 * `var f = e; pop(f)` shrank f and not e, where the same program over an
 * int or a general array shrinks both. Three sites had patched the NAMED
 * variable back through its lvalue; an alias, a parameter or a container
 * element still diverged.
 *
 * A SLICE handle is first materialized - the slice becomes an ordinary
 * array, every handle of it at once (materialize_slice) - and that array
 * is then promoted in place like any other, so every name sharing the
 * slice sees the promotion too.
 */
template <class LValueT>
void SharedArrayObjTempl<LValueT>::promote_structs_to_general()
{
    if (shobj->kind != Storage::structs)
        return;

    if (slice)
        materialize_slice();

    const size_type n = size();         /* a non-slice: the whole storage */
    vec_type nv;
    nv.reserve(n);
    for (size_type i = 0; i < n; i++)
        nv.emplace_back(struct_elem_at(*this, i), false);

    shobj->svec.~svec_type();
    shobj->kind = Storage::general;
    new (&shobj->vec) vec_type(std::move(nv));
    len = n;
}

template <class LValueT>
void SharedArrayObjTempl<LValueT>::promote_strs_to_general()
{
    if (shobj->kind != Storage::strs)
        return;

    if (slice)
        materialize_slice();            /* see promote_structs_to_general */

    const size_type n = size();
    vec_type nv;
    nv.reserve(n);
    for (size_type i = 0; i < n; i++)
        nv.emplace_back(EvalValue(SharedStr(shobj->tvec[i])), false);

    shobj->tvec.~tvec_type();
    shobj->kind = Storage::general;
    new (&shobj->vec) vec_type(std::move(nv));
    len = n;
}

template <class LValueT>
void SharedArrayObjTempl<LValueT>::promote_objs_to_general()
{
    if (shobj->kind != Storage::objs)
        return;

    if (slice)
        materialize_slice();            /* see promote_structs_to_general */

    const size_type n = size();
    vec_type nv;
    nv.reserve(n);
    for (size_type i = 0; i < n; i++)
        nv.emplace_back(obj_elem_value(shobj->ovec[i]), false);

    shobj->ovec.~ovec_type();
    shobj->kind = Storage::general;
    new (&shobj->vec) vec_type(std::move(nv));
    len = n;
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
            *this = SharedArrayObjTempl(std::move(nv));
            return;
        }

        case Storage::floats: {
            fvec_type nv(
                shobj->fvec.cbegin() + offset(),
                shobj->fvec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(std::move(nv));
            return;
        }

        case Storage::bools: {
            bvec_type nv(
                shobj->bvec.cbegin() + offset(),
                shobj->bvec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(std::move(nv));
            return;
        }

        case Storage::structs: {
            const int stride = shobj->svec.stride;
            std::vector<char> nb(
                shobj->svec.buf.cbegin() + offset() * stride,
                shobj->svec.buf.cbegin() + (offset() + size()) * stride
            );
            *this = SharedArrayObjTempl(
                svec_type(std::move(nb), shobj->svec.def, stride));
            return;
        }

        case Storage::strs: {
            tvec_type nv(
                shobj->tvec.cbegin() + offset(),
                shobj->tvec.cbegin() + offset() + size()
            );
            *this = SharedArrayObjTempl(std::move(nv));
            return;
        }

        case Storage::objs:
            *this = SharedArrayObjTempl(
                shobj->ovec.copy_range(offset(), offset() + size()));
            return;

        default:
            break;
    }

    vec_type new_vec(
        shobj->vec.cbegin() + offset(),
        shobj->vec.cbegin() + offset() + size()
    );

    *this = SharedArrayObjTempl(std::move(new_vec));
}

/*
 * Question 3 (Python's rule): turn the slice THIS handle belongs to into
 * an ordinary array - one fresh copy of the window, which every handle of
 * the slice's group then holds. The handles are gathered from the
 * storage's registry before any is reseated (reseating unregisters it);
 * `this` is one of them, since a slice handle is always registered (a raw
 * bit-copy of one, #94's borrow, is refused). Reseating drops the views'
 * counts on the old storage, so nothing here touches it after the last
 * reseat. The handles of a group agree on the window: the INT state
 * checker (int_slice_fault) verifies it at every op of every INT run.
 */
template <class LValueT>
void SharedArrayObjTempl<LValueT>::materialize_slice()
{
    if (!slice)
        return;

    SharedArrayObjTempl fresh(*this);   /* a sibling, for a moment */
    fresh.clone_internal_vec();         /* now an ordinary array */
    fresh.shobj->readonly = shobj->readonly; /* a view of a const stays one */

    std::vector<SharedArrayObjTempl *> members;
    auto r = shobj->slices.equal_range(group);
    for (auto it = r.first; it != r.second; ++it)
        members.push_back(it->second);

    for (SharedArrayObjTempl *p : members)
        *p = fresh;                     /* unregisters p; shares `fresh` */
    ML_CHECK(!slice);   /* INT-COV-EXEMPT: `this` was registered */
}

/* The group numbers of this storage's live views, made dense again
 * (1..n) before slice_groups would wrap - a group must never be handed
 * out twice while a view holding it is alive. Cold: once per four
 * billion slices of one storage. */
template <class LValueT>
void SharedArrayObjTempl<LValueT>::renumber_slice_groups()
{
    std::vector<std::pair<uint32_t, SharedArrayObjTempl *>> all(
        shobj->slices.begin(), shobj->slices.end());
    std::sort(all.begin(), all.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    shobj->slices.clear();
    uint32_t n = 0, prev = 0;
    for (size_t i = 0; i < all.size(); i++) {
        if (i == 0 || all[i].first != prev)
            n++;
        prev = all[i].first;
        all[i].second->group = n;
        shobj->slices.emplace(n, all[i].second);
    }
    shobj->slice_groups = n;
}

/*
 * A write to this storage at `index` (all_slices: anywhere): every slice
 * whose window holds that index would show the write, so each is
 * materialized first - ONE copy per slice, shared by all its handles.
 * The overlapping groups are collected before any is materialized
 * (materializing unregisters their handles), and `this`, a non-slice
 * handle, keeps the storage alive throughout.
 */
template <class LValueT>
void SharedArrayObjTempl<LValueT>::clone_aliased_slices(size_type index)
{
    std::vector<std::pair<uint32_t, SharedArrayObjTempl *>> hit;
    for (const auto &e : shobj->slices) {
        const SharedArrayObjTempl *p = e.second;
        if (index == all_slices ||
            (p->off <= index && index < p->off + p->len))
            hit.push_back(e);
    }
    std::sort(hit.begin(), hit.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    /* materialize_slice unregisters through the handles' assignment,
     * which keeps the has_slices MIRROR true to the registry */
    for (size_t i = 0; i < hit.size(); i++)
        if (i == 0 || hit[i].first != hit[i - 1].first)
            hit[i].second->materialize_slice();
}

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

/* builtins/arr.cpp.h: the append core TypeArr::add shares */
static bool arr_elem_fits(const SharedArrayObj &arr, const EvalValue &elem);
static bool arr_push_value(SharedArrayObj &arr, const EvalValue &elem,
                           bool is_const);

/* A NEW empty slice: its own group over the shared empty storage (see
 * TypeArr::slice). */
static EvalValue arr_empty_slice()
{
    return SharedArrayObj(empty_arr.get_ref<SharedArrayObj>(), 0, 0);
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
        case SharedArrayObj::Storage::structs:
            return reinterpret_cast<int_type>(&arr.flat_structs());
        case SharedArrayObj::Storage::strs:
            return reinterpret_cast<int_type>(&arr.flat_strs());
        case SharedArrayObj::Storage::objs:
            return reinterpret_cast<int_type>(&arr.flat_objs());
        default:
            return reinterpret_cast<int_type>(&arr.get_vec());
    }
}

/* `lv += rv[roff, roff + rn)` for one flat kind, in place. The range
 * may lie in `lv` itself (`a += a`, `a += a[0:2]`): it is copied out
 * first, since inserting a vector's own range into it is undefined. */
template <class V>
static void arr_concat_flat(V &lv, const V &rv, size_type roff, size_type rn)
{
    if (&lv == &rv) {
        const V tmp(rv.cbegin() + roff, rv.cbegin() + roff + rn);
        lv.insert(lv.end(), tmp.cbegin(), tmp.cend());
        return;
    }
    lv.reserve(lv.size() + rn);
    lv.insert(lv.end(), rv.cbegin() + roff, rv.cbegin() + roff + rn);
}

void TypeArr::add(EvalValue &a, const EvalValue &b)
{
    SharedArrayObj &lval = a.get<SharedArrayObj>();

    if (lval.is_readonly())
        throw CannotChangeConstEx();

    if (!b.is<SharedArrayObj>())
        throw TypeErrorEx("Expected array on the right side of +");

    /* `s += x` on a slice grows the slice every name holding it shares
     * (question 3): it becomes an ordinary array first, then grows in
     * place like one. The rhs may be a handle of that same slice
     * (`s += s`); it is materialized with it, and the in-place paths
     * below read a copy of the elements before growing. */
    lval.materialize_slice();

    lval.invalidate_hash();   /* += appends elements -> the hash changes */

    const SharedArrayObj &rhs = b.get<SharedArrayObj>();

    /*
     * Flat homogeneous fast path: int+int / float+float / bool+bool
     * concatenate in unboxed storage, IN PLACE (every alias shares the
     * grown array). A mismatched or general operand falls through to the
     * element-by-element path below.
     */
    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::ints) {
        arr_concat_flat(lval.flat_ints(), rhs.flat_ints(), rhs.offset(),
                        rhs.size());
        return;
    }
    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::floats) {
        arr_concat_flat(lval.flat_floats(), rhs.flat_floats(), rhs.offset(),
                        rhs.size());
        return;
    }
    if (lval.skind() == rhs.skind() &&
        lval.skind() == SharedArrayObj::Storage::bools) {
        arr_concat_flat(lval.flat_bools(), rhs.flat_bools(), rhs.offset(),
                        rhs.size());
        return;
    }

    /*
     * Every other pairing appends ELEMENT BY ELEMENT, IN PLACE, through
     * the storage every alias shares - `var t = s; t += x` grows `s` too,
     * whatever the storage kind (it used to build a fresh array whenever
     * the lhs was not general, so a flat string or struct array lost its
     * aliases). The elements are read first (`a += a` reads the array it
     * grows), and each must fit the lhs storage before any is appended: a
     * flat array through a `dyn` alias refuses a value of another type, as
     * append does, and the array is left unchanged.
     */
    const size_type rn = rhs.size();
    std::vector<EvalValue> items;
    items.reserve(rn);
    for (size_type i = 0; i < rn; i++)
        items.push_back(arr_elem_at(rhs, i));

    for (const EvalValue &e : items)
        if (!arr_elem_fits(lval, e))
            throw TypeErrorEx(flat_array_violation_msg);

    for (const EvalValue &e : items) {
        const bool ok = arr_push_value(lval, e, false);
        ML_CHECK(ok);
        (void)ok;
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
        case SharedArrayObj::Storage::strs:
            return EvalValue(SharedStr(arr.flat_strs()[at]));
        case SharedArrayObj::Storage::objs:
            return obj_elem_value(arr.flat_objs()[at]);
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
    const SharedArrayObj &rhs = b.get_ref<SharedArrayObj>();
    const size_type n = lhs.size();

    if (n != rhs.size()) {
        a = false;
        return;
    }

    /*
     * Two GENERAL arrays may hold references, so the pair may be on a cycle:
     * the comparison runs on the cycle guard's pair stack (cyclewalk.h). A
     * flat side holds only scalars, so it is never on a stack and a cyclic
     * general side differs from it at some element anyway - the plain walk
     * answers that case, as it always did.
     */
    const bool general = lhs.skind() == SharedArrayObj::Storage::general &&
                         rhs.skind() == SharedArrayObj::Storage::general;
    CycPairStack &st = cyc_eq();
    CycKey kl, kr;

    if (general) {
        cyc_key(a, kl);
        cyc_key(b, kr);

        /*
         * Identity shortcut: the same storage window on both sides covers the
         * exact same elements, hence equal - when the guard says skipping the
         * walk cannot change the answer (always at the top level; inside a
         * comparison of two different containers, unless this one is on a
         * cycle - see CycPairStack::identity_ok).
         */
        if (kl == kr && st.identity_ok(a, kl)) {
            a = true;
            return;
        }

        const int be = st.back_edge(kl, kr);
        if (be >= 0) {
            a = be == 1;
            return;
        }
    }

    /* Element-wise compare, reading each side without promoting (so two flat
     * arrays - or a flat and a general one - compare equal element by element,
     * with the usual 1 == 1.0 numeric equality via EvalValue::operator!=).
     * ONE loop for both cases: a second copy of it cost the inlining of the
     * element compare (+33% Ir on a walk over nested arrays). */
    bool equal = true;
    {
        CycPairGuard g(st, kl, kr, general);
        for (size_type i = 0; i < n; i++) {
            if (arr_elem_at(lhs, i) != arr_elem_at(rhs, i)) {
                equal = false;
                break;
            }
        }
    }
    a = equal;
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
    const SharedArrayObj &arr = a.get_ref<SharedArrayObj>();

    if (arr.hash_is_cached())          /* a non-slice with a valid cache */
        return arr.get_cached_hash();

    const size_type n = arr.size();
    size_t seed = hash_salt_array;

    /* A general array may be on a cycle: met again inside its own hash, it
     * hashes as a back edge at its depth (cyclewalk.h). Never cached - only
     * a flat array caches, and its hash does not depend on a walk. */
    CycKey k;
    const bool keyed = cyc_key(a, k);
    if (keyed) {
        const int d = cyc_hash().depth(k);
        if (d >= 0)
            return cyc_backedge_hash(d);
    }
    CycGuard g(cyc_hash(), k, keyed);

    for (size_type i = 0; i < n; i++)
        hash_combine(seed, arr_elem_at(arr, i).hash());

    arr.store_cached_hash(seed);       /* no-op for a slice */
    return seed;
}

string TypeArr::to_string(const EvalValue &a)
{
    const SharedArrayObj &arr = a.get_ref<SharedArrayObj>();
    const size_type n = arr.size();
    string res;

    /* Flat fast path: stringify the unboxed vector directly, no promotion. */
    if (arr.skind() != SharedArrayObj::Storage::general) {

        res.reserve(n * 32);
        res += "[";

        for (size_type i = 0; i < n; i++) {
            res += arr_elem_at(arr, i).to_string_repr();
            if (i != n - 1)
                res += ", ";
        }

        res += "]";
        return res;
    }

    /* A general array already being printed further up this walk is shown
     * as `[...]` (cyclewalk.h, README *Values that contain themselves*). */
    CycKey k;
    cyc_key(a, k);
    if (cyc_render().contains(k))
        return "[...]";
    CycGuard g(cyc_render(), k);

    res.reserve(n * 32);
    res += "[";

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
    /* already being printed further up: `[...]`, like to_string */
    CycKey k;
    const bool keyed = cyc_key(a, k);
    if (keyed && cyc_render().contains(k))
        return "[...]";

    const string flat = to_string_repr(a);
    const SharedArrayObj &arr = a.get_ref<SharedArrayObj>();
    const size_type n = arr.size();

    /* fits on one line (or empty) -> single line */
    if (n == 0 || indent + static_cast<int>(flat.size()) <= width)
        return flat;

    /* otherwise expand one element per line, indented - on the render
     * stack, so an element that leads back here prints as `[...]` */
    CycGuard g(cyc_render(), k, keyed);
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
    /*
     * BORROW the array handle (get_ref, NOT get). `what` is const, so a
     * get<SharedArrayObj>() would call the by-VALUE const overload and COPY the
     * handle - an intrusive_ptr retain + later release on EVERY general/flat
     * subscript READ (measured ~2.8% of a dict-word-count loop: the base array
     * is re-read per iteration). `arr` is only READ here; the one mutable path
     * (the LValue-array element back-pointer below) re-derives the array
     * through the base LValue*, which is a ref, not a copy.
     */
    const SharedArrayObj &arr = what.get_ref<SharedArrayObj>();
    int_type idx = idx_val.get<int_type>();

    ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
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
    /* A plain-assignment WRITE target (for_write) on a flat STRING array
     * PROMOTES (the value-driven strs model) - through the ORIGINAL LValue,
     * never the RValue() temp above (promoting the temp freed the fresh
     * general vector with it - an ASan-caught UAF). Reads stay flat. */
    if (arr.skind() == SharedArrayObj::Storage::strs && for_write
            && what_lval.is<LValue *>() && !arr.is_readonly()) {
        LValue *base = what_lval.get<LValue *>();
        base->getval<SharedArrayObj>().promote_strs_to_general();
        /* re-enter on the (now general) slot value */
        return subscript(what_lval, idx_val, for_write);
    }

    if (arr.skind() != SharedArrayObj::Storage::general)
        return arr_elem_at(arr, idx);

    if (!what_lval.is<LValue *>() || arr.is_readonly()) {
        /*
         * Return a simple RValue when the input array was not an LValue, or
         * when it is read-only (a `const` value): a read still works, but an
         * assignment target is an rvalue, so `a[i] = x` fails with NotLValueEx.
         * const get_vec() - no handle copy (arr is borrowed).
         */
        return arr.get_vec()[arr.offset() + idx].get();
    }

    /* We dereferenced an LValue array, so return the element's LValue with its
     * container back-pointer. Get the MUTABLE vec through the base LValue* (a
     * ref, not a handle copy - the array is already general, so no promote). */
    LValue *base = what_lval.get<LValue *>();
    SharedArrayObj::vec_type &vec = base->getval<SharedArrayObj>().get_vec();
    LValue *ret = &vec[arr.offset() + idx];
    ret->container = base;
    ret->container_idx = arr.offset() + idx;
    return ret;
}

EvalValue TypeArr::slice(const EvalValue &what_lval,
                         const EvalValue &start_val,
                         const EvalValue &end_val)
{
    const EvalValue &what = RValue(what_lval);
    /* Borrow (get_ref), not copy: `arr` is only read (size/offset) and the
     * slice-view ctor below does its OWN shobj retain, so the extra handle
     * copy a by-value get<> makes is pure churn. */
    const SharedArrayObj &arr = what.get_ref<SharedArrayObj>();
    ML_INT_ONLY(int_vc_slice_vals(false, start_val, end_val, arr.size());)
    int_type start = 0, end = arr.size();

    if (start_val.is<int_type>()) {

        start = start_val.get<int_type>();

        if (start < 0) {

            start += arr.size();

            if (start < 0)
                start = 0;
        }

        /* an EMPTY slice is still a new array of its own (question 3:
         * a slice is separate the moment it is taken) - a fresh group
         * over the shared empty storage, never empty_arr itself, whose
         * every copy would be ONE array: appending to one empty slice
         * would show in all of them */
        if (static_cast<size_t>(start) >= arr.size())
            return arr_empty_slice();

    } else if (!start_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range start");
    }

    if (end_val.is<int_type>()) {

        end = end_val.get<int_type>();

        if (end < 0)
            end += arr.size();

        if (end <= start)
            return arr_empty_slice();

        if (static_cast<size_t>(end) > arr.size())
            end = arr.size();

    } else if (!end_val.is<NoneVal>()) {

        throw TypeErrorEx("Expected integer as range end");
    }

    return SharedArrayObj(arr, arr.offset() + start, end - start);
}
