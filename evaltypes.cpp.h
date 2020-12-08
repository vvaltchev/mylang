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
class NonTrivialType : public Type {

public:

    NonTrivialType(Type::TypeE e) : Type(e) { }

    virtual void default_ctor(void *obj) {
        new (obj) T;
    }

    virtual void dtor(void *obj) {
        reinterpret_cast<T *>(obj)->~T();
    }

    virtual void copy_ctor(void *obj, const void *other) {
        new (obj) T(*reinterpret_cast<const T *>(other));
    }

    virtual void move_ctor(void *obj, void *other) {
        new (obj) T(move(*reinterpret_cast<T *>(other)));
    }

    virtual void copy_assign(void *obj, const void *other) {
        *reinterpret_cast<T *>(obj) = *reinterpret_cast<const T *>(other);
    }

    virtual void move_assign(void *obj, void *other) {
        *reinterpret_cast<T *>(obj) = move(*reinterpret_cast<T *>(other));
    }
};

template <class T>
class SharedType :
    public NonTrivialType<
        shared_ptr<typename T::type>
    >
{
    typedef shared_ptr<typename T::type> S;

public:
    SharedType(Type::TypeE e) : NonTrivialType<S>(e) { }
};

class TypeNone : public Type {

public:

    TypeNone() : Type(Type::t_none) { }

    virtual string to_string(const EvalValue &a) {
        return "<none>";
    }

    virtual void eq(EvalValue &a, const EvalValue &b) {
        a = b.is<NoneVal>();
    }

    virtual void noteq(EvalValue &a, const EvalValue &b) {
        a = !b.is<NoneVal>();
    }
};

class TypeBuiltin : public Type {

public:
    TypeBuiltin() : Type(Type::t_builtin) { }
    virtual string to_string(const EvalValue &a) {
        return "<builtin: " + find_builtin_name(a.get<Builtin>()) + ">";
    }
};
