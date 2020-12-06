/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"

const string &
find_builtin_name(const Builtin &b);

template <class T>
class SharedType : public Type {

    /*
     * T is our POD wrapper for shared_ptr<U>.
     * U is the real data type wrapped by shared_ptr.
     */
    typedef typename T::type U;
    typedef shared_ptr<U> S;

public:

    SharedType(Type::TypeE e) : Type(e) { }

    virtual void default_ctor(void *obj) {
        new (obj) S;
    }

    virtual void dtor(void *obj) {
        reinterpret_cast<S *>(obj)->~S();
    }

    virtual void copy_ctor(void *obj, const void *other) {
        new (obj) S(*reinterpret_cast<const S *>(other));
    }

    virtual void move_ctor(void *obj, void *other) {
        new (obj) S(move(*reinterpret_cast<S *>(other)));
    }

    virtual void copy_assign(void *obj, const void *other) {
        *reinterpret_cast<S *>(obj) = *reinterpret_cast<const S *>(other);
    }

    virtual void move_assign(void *obj, void *other) {
        *reinterpret_cast<S *>(obj) = move(*reinterpret_cast<S *>(other));
    }
};

class TypeNone : public Type {

public:
    TypeNone() : Type(Type::t_none) { }
    virtual string to_string(const EvalValue &a) {
        return "<none>";
    }
};

class TypeBuiltin : public Type {

public:
    TypeBuiltin() : Type(Type::t_builtin) { }
    virtual string to_string(const EvalValue &a) {
        return "<builtin: " + find_builtin_name(a.get<Builtin>()) + ">";
    }
};
