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
        return vec[off + index];
    }

    size_type size() const {
        return len;
    }
};

template <class LValueT>
class SharedArrayObjTempl final {

public:
    typedef std::vector<LValueT>    vec_type;
    typedef std::vector<int_type>   ivec_type;
    typedef std::vector<float_type> fvec_type;

    /*
     * Backing-storage kind (see plans/typed-arrays.md). A homogeneous int/float
     * array keeps an *unboxed* vector<int_type>/<float_type> (8-byte slots)
     * instead of vector<LValue> (48-byte slots), which makes bulk ops
     * (reverse/sort/sum/foreach) move ~6x less memory. get_vec() promotes a
     * flat array to `general` on demand (any code needing LValue access); the
     * hot ops branch on the kind and touch the flat vector directly.
     */
    enum class Storage : unsigned char { general, ints, floats };

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

        ~SharedObject() {
            switch (kind) {
                case Storage::general: vec.~vec_type();   break;
                case Storage::ints:    ivec.~ivec_type(); break;
                case Storage::floats:  fvec.~fvec_type(); break;
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
    ivec_type &flat_ints()   { return shobj->ivec; }   /* skind()==ints */
    fvec_type &flat_floats() { return shobj->fvec; }   /* skind()==floats */
    const ivec_type &flat_ints()   const { return shobj->ivec; }
    const fvec_type &flat_floats() const { return shobj->fvec; }

    int_type use_count() const { return shobj.use_count(); }

    bool is_readonly() const { return shobj && shobj->readonly; }
    void set_readonly() { shobj->readonly = true; }

    bool is_slice() const { return slice; }
    size_type offset() const { return slice ? off : 0; }

    /* Element count without promoting (kind-aware). */
    size_type size() const {
        if (slice)
            return len;
        switch (shobj->kind) {
            case Storage::ints:   return shobj->ivec.size();
            case Storage::floats: return shobj->fvec.size();
            default:              return shobj->vec.size();
        }
    }

    ArrayConstViewTempl<LValueT> get_view() const {
        return ArrayConstViewTempl<LValueT>(get_vec(), offset(), size());
    };
};
