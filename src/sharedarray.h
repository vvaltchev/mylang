/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include "intrusiveptr.h"
#include "errors.h"
#include <vector>
#include <unordered_set>
#include <cassert>
#include <new>

struct StructTypeDef;   /* a struct type (structtype.h); only a pointer here */


/*
 * These classes are templates simply because otherwise this header wouldn't be
 * able to compile if included independently (it requires LValue, which requires
 * EvalValue which requires SharedArrayObj). In this case, it wouldn't be a big
 * deal, but in general it's an anti-pattern to have headers requiring a specific
 * include order.
 */

template <class LValueT>
class ArrayConstViewTempl {

    const std::vector<LValueT> &vec;
    const size_type off;
    const size_type len;

public:

    ArrayConstViewTempl(const std::vector<LValueT> &vec, size_type off, size_type len)
        : vec(vec), off(off), len(len)
    { }

    const LValueT &operator[](size_t index) const {
        ML_CHECK(index < len && off + index < vec.size());
        return vec[off + index];
    }

    size_type size() const {
        return len;
    }
};

template <class LValueT>
class SharedArrayObjTempl final {

public:
    typedef std::vector<LValueT>      vec_type;
    typedef std::vector<int_type>     ivec_type;
    typedef std::vector<float_type>   fvec_type;
    typedef std::vector<unsigned char> bvec_type;   /* one byte per bool */

    /*
     * Flat storage for an array of POD structs (plans/structs.md phase 7): the
     * elements laid out contiguously as raw C-struct bytes, `stride`
     * bytes each.
     * `def` (a StructTypeDef*) is the element type - used only by the impl
     * (arr.cpp.h, where it is complete) to materialize/store a StructObject;
     * `stride` is cached here so the inline template never needs StructTypeDef
     * to be a complete type. Cold operations promote this to `general` via
     * get_vec() (see promote_structs_to_general), so only the hot paths
     * (creation / append / subscript read / foreach) touch the bytes directly.
     */
    struct svec_type {
        std::vector<char> buf;
        StructTypeDef *def = nullptr;
        int stride = 0;
        svec_type() = default;
        svec_type(std::vector<char> &&b, StructTypeDef *d, int s)
            : buf(move(b)), def(d), stride(s) { }
    };

    /*
     * Backing-storage kind (see plans/typed-arrays.md). A homogeneous
     * int/float/bool array keeps an *unboxed* vector instead of vector<LValue>
     * (48-byte slots), which makes bulk ops (reverse/sort/sum/foreach) move far
     * less memory: int/float are 8-byte slots, bool is a single byte (48x
     * denser than general, ideal for sieves/bitmaps). mylang never promotes a
     * flat array to general (representation is type-driven, fixed at creation);
     * the hot ops branch on the kind and touch the flat vector directly.
     */
    enum class Storage : unsigned char {
        general, ints, floats, bools, structs
    };

private:
    static constexpr size_type all_slices = static_cast<size_type>(-1);

    struct SharedObject final : RefCounted {

        Storage kind;

        /*
         * Exactly one member is live, per `kind`. It is a union, so its
         * non-trivial vector members get explicit placement-new in the ctors
         * and an explicit destructor call in ~SharedObject.
         */
        union {
            vec_type  vec;     /* kind == general */
            ivec_type ivec;    /* kind == ints */
            fvec_type fvec;    /* kind == floats */
            bvec_type bvec;    /* kind == bools */
            svec_type svec;    /* kind == structs */
        };

        std::unordered_set<SharedArrayObjTempl *> slices;

        /*
         * When set, this array's data is read-only: it backs a `const` value,
         * so element writes, `+=`, append/insert/erase/pop and sort-in-place
         * are rejected. The flag lives on the shared object so it travels with
         * every alias and slice; clone() builds a fresh object and is therefore
         * always mutable. See make_const_clone() in eval.cpp.
         */
        bool readonly = false;

        /*
         * Cached deep hash of this array's elements (see TypeArr::hash). Lazily
         * computed; maintained in O(1) on append; INVALIDATED on every other
         * mutation (pop/insert/erase/sort/reverse/+=/element write). A fresh
         * SharedObject (and every clone, which is constructed - never copied,
         * the copy ctor is deleted) starts invalid, so a COW clone naturally
         * recomputes. A read-only array never mutates, so its cache, once set,
         * stays valid forever (the frozen-dict-key fast path).
         */
        size_t hash_cache = 0;
        bool hash_valid = false;

        SharedObject() : kind(Storage::general) { new (&vec) vec_type(); }
        SharedObject(vec_type &&a) : kind(Storage::general) {
            new (&vec) vec_type(move(a));
        }
        SharedObject(ivec_type &&a) : kind(Storage::ints) {
            new (&ivec) ivec_type(move(a));
        }
        SharedObject(fvec_type &&a) : kind(Storage::floats) {
            new (&fvec) fvec_type(move(a));
        }
        SharedObject(bvec_type &&a) : kind(Storage::bools) {
            new (&bvec) bvec_type(move(a));
        }
        SharedObject(svec_type &&a) : kind(Storage::structs) {
            new (&svec) svec_type(move(a));
        }

        ~SharedObject() {
            switch (kind) {
                case Storage::general: vec.~vec_type();   break;
                case Storage::ints:    ivec.~ivec_type(); break;
                case Storage::floats:  fvec.~fvec_type(); break;
                case Storage::bools:   bvec.~bvec_type(); break;
                case Storage::structs: svec.~svec_type(); break;
            }
        }

        SharedObject(const SharedObject &) = delete;
        SharedObject &operator=(const SharedObject &) = delete;
    };

    intrusive_ptr<SharedObject> shobj;

    /* Convert flat int/float storage to general vector<LValue> in place.
     * Defined out-of-line (needs EvalValue/LValue) in types/arr.cpp.h. No-op if
     * already general. */

public:
    size_type off;
    size_type len;
    bool slice;

    /* Special constructors */

    SharedArrayObjTempl(const vec_type &arr) = delete;

    SharedArrayObjTempl(vec_type &&arr)
        : shobj(make_intrusive<SharedObject>(move(arr)))
        , off(0)
        , len(shobj->vec.size())
        , slice(false)
    { }

    /* Flat (unboxed) int/float storage - see plans/typed-arrays.md. */
    SharedArrayObjTempl(ivec_type &&arr)
        : shobj(make_intrusive<SharedObject>(move(arr)))
        , off(0)
        , len(shobj->ivec.size())
        , slice(false)
    { }

    SharedArrayObjTempl(fvec_type &&arr)
        : shobj(make_intrusive<SharedObject>(move(arr)))
        , off(0)
        , len(shobj->fvec.size())
        , slice(false)
    { }

    SharedArrayObjTempl(bvec_type &&arr)
        : shobj(make_intrusive<SharedObject>(move(arr)))
        , off(0)
        , len(shobj->bvec.size())
        , slice(false)
    { }

    /* Flat POD-struct storage (plans/structs.md phase 7). `len` is the element
     * count (buf bytes / stride). */
    SharedArrayObjTempl(svec_type &&arr)
        : shobj(make_intrusive<SharedObject>(move(arr)))
        , off(0)
        , len(shobj->svec.stride
                  ? static_cast<size_type>(shobj->svec.buf.size() /
                                           shobj->svec.stride)
                  : 0)
        , slice(false)
    { }

    SharedArrayObjTempl(const SharedArrayObjTempl &obj, size_type off, size_type len)
        : shobj(obj.shobj)
        , off(off)
        , len(len)
        , slice(true)
    {
        shobj->slices.insert(this);
    }

    /* Regular constructors */

    SharedArrayObjTempl() : off(0), len(0), slice(false) { }

    SharedArrayObjTempl(const SharedArrayObjTempl &obj)
        : shobj(obj.shobj)
        , off(obj.off)
        , len(obj.len)
        , slice(obj.slice)
    {
        if (slice)
            shobj->slices.insert(this);
    }

    SharedArrayObjTempl(SharedArrayObjTempl &&obj)
        : shobj(move(obj.shobj))
        , off(obj.off)
        , len(obj.len)
        , slice(obj.slice)
    {
        if (slice) {
            shobj->slices.erase(&obj);
            shobj->slices.insert(this);
            obj.slice = false;
        }
    }

    SharedArrayObjTempl &operator=(const SharedArrayObjTempl &obj)
    {
        shobj = obj.shobj;
        off = obj.off;
        len = obj.len;
        slice = obj.slice;

        if (slice)
            shobj->slices.insert(this);

        return *this;
    }

    SharedArrayObjTempl &operator=(SharedArrayObjTempl &&obj)
    {
        shobj = move(obj.shobj);
        off = obj.off;
        len = obj.len;
        slice = obj.slice;

        if (slice) {
            shobj->slices.erase(&obj);
            shobj->slices.insert(this);
            obj.slice = false;
        }

        return *this;
    }

    ~SharedArrayObjTempl()
    {
        if (slice)
            shobj->slices.erase(this);
    }

    void clone_internal_vec();
    void clone_aliased_slices(size_type index);
    void clone_all_slices() { clone_aliased_slices(all_slices); }

    /*
     * Materialize a flat POD-struct array into a general vector<LValue> (one
     * boxed StructObject per element), in place. Defined out-of-line in
     * types/arr.cpp.h (needs StructObject). No-op if not `structs`. The bounded
     * cost of a struct-array operation that has no flat fast path: it promotes
     * here (via get_vec) rather than touching the bytes, so EVERY general array
     * op works on a struct array without a dedicated case.
     */
    void promote_structs_to_general();

    /*
     * General (vector<LValue>) access. mylang does NOT promote flat int/float
     * storage to general on demand: an array's representation is fixed at
     * creation from its proven static type (type-driven), so the only valid
     * caller of get_vec() is one operating on an already-general array. Code
     * that may face a flat array must branch on skind() and read flat_ints()/
     * flat_floats() (or arr_elem_at) directly. Reaching here on a flat array is
     * an internal invariant violation - throw rather than read the inactive
     * union member. A non-fitting *mutation* of a flat array (the dyn-launder
     * case) is caught earlier with a user-facing TypeErrorEx.
     */
    vec_type &get_vec() {
        /* A struct array promotes to general here (the cold-path fallback); a
         * flat scalar array reaching here is an invariant violation. */
        if (shobj->kind == Storage::structs)
            promote_structs_to_general();
        if (shobj->kind != Storage::general)
            throw InternalErrorEx();
        return shobj->vec;
    }
    const vec_type &get_vec() const {
        if (shobj->kind != Storage::general)
            throw InternalErrorEx();
        return shobj->vec;
    }

    Storage skind() const { return shobj->kind; }

    /*
     * Flat (unboxed) accessors. Each returns the union member for ONE kind;
     * reading it for any other kind is undefined (an inactive union member).
     * The ML_CHECK makes that mistake fire immediately in a debug/sanitized
     * build instead of silently corrupting - the caller MUST have branched on
     * skind() first. (Compiled out of a plain release.)
     */
    ivec_type &flat_ints() {
        ML_CHECK(shobj && shobj->kind == Storage::ints);
        return shobj->ivec;
    }
    fvec_type &flat_floats() {
        ML_CHECK(shobj && shobj->kind == Storage::floats);
        return shobj->fvec;
    }
    bvec_type &flat_bools() {
        ML_CHECK(shobj && shobj->kind == Storage::bools);
        return shobj->bvec;
    }
    const ivec_type &flat_ints() const {
        ML_CHECK(shobj && shobj->kind == Storage::ints);
        return shobj->ivec;
    }
    const fvec_type &flat_floats() const {
        ML_CHECK(shobj && shobj->kind == Storage::floats);
        return shobj->fvec;
    }
    const bvec_type &flat_bools() const {
        ML_CHECK(shobj && shobj->kind == Storage::bools);
        return shobj->bvec;
    }
    svec_type &flat_structs() {
        ML_CHECK(shobj && shobj->kind == Storage::structs);
        return shobj->svec;
    }
    const svec_type &flat_structs() const {
        ML_CHECK(shobj && shobj->kind == Storage::structs);
        return shobj->svec;
    }

    int_type use_count() const { return shobj.use_count(); }

    bool is_readonly() const { return shobj && shobj->readonly; }
    void set_readonly() { shobj->readonly = true; }

    /*
     * Hash cache (see TypeArr::hash and SharedObject::hash_cache). Caching is
     * restricted to a NON-slice array of FLAT SCALARS (int/float/bool): its
     * elements are scalars, so the only way to change its hash is a mutation OF
     * THIS array (element write / append / pop / insert / erase / sort /
     * reverse / +=), each of which calls invalidate_hash / the append-maintain.
     * A GENERAL or struct array is NOT cached: a nested mutation (`a[i][j]=v`,
     * `a[i].f=v`, replacing a struct element) changes this array's hash without
     * touching this object - and there is no back-pointer to invalidate it - so
     * it is always recomputed on demand. A slice hashes a sub-range, never the
     * whole shared vector, so it is not cached either. A COW clone is a fresh
     * object (the copy ctor is deleted) and starts invalid, so it recomputes.
     */
    bool hash_cacheable() const {
        return !slice && shobj &&
               (shobj->kind == Storage::ints ||
                shobj->kind == Storage::floats ||
                shobj->kind == Storage::bools);
    }
    bool hash_is_cached() const {
        return hash_cacheable() && shobj->hash_valid;
    }
    size_t get_cached_hash() const { return shobj->hash_cache; }
    void store_cached_hash(size_t h) const {
        if (hash_cacheable()) {
            shobj->hash_cache = h;
            shobj->hash_valid = true;
        }
    }
    void invalidate_hash() {
        if (shobj)
            shobj->hash_valid = false;
    }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }

    /* Element count without promoting (kind-aware). */
    size_type size() const {
        if (slice)
            return len;
        switch (shobj->kind) {
            case Storage::ints:   return shobj->ivec.size();
            case Storage::floats: return shobj->fvec.size();
            case Storage::bools:  return shobj->bvec.size();
            case Storage::structs:
                return shobj->svec.stride
                    ? static_cast<size_type>(shobj->svec.buf.size() /
                                             shobj->svec.stride)
                    : 0;
            default:              return shobj->vec.size();
        }
    }

    ArrayConstViewTempl<LValueT> get_view() const {
        return ArrayConstViewTempl<LValueT>(get_vec(), offset(), size());
    };
};
