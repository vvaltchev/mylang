/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include <cstring>

const string &
find_builtin_name(const Builtin &b);

/*
 * Special base class for any Type class related to non-trivial C++ type.
 * Typically, the object is just a shared_ptr<T> and in that case it's enough
 * to derive the custom Type from NonTrivialType<shared_ptr<T>>. But, in some
 * special cases, like SharedStr, there are additional trivial members related
 * to the non-trivial object that must be copied as well. Therefore NonTrivialType
 * has an additional non-type template param (S) which allows to specify the real
 * size of the container object. NOTE: the non-trivial object MUST BE at offset 0
 * in the wrapper type (e.g. SharedStr).
 */

template <class T, size_t S = sizeof(T)>
class NonTrivialType : public Type {

public:

    NonTrivialType(Type::TypeE e) : Type(e) { }

    virtual void default_ctor(void *obj) {

        static_assert(S >= sizeof(T));

        if (S > sizeof(T)) {
            memset(obj, 0, S);
        }

        new (obj) T;
    }

    virtual void dtor(void *obj) {
        reinterpret_cast<T *>(obj)->~T();
    }

    virtual void copy_ctor(void *obj, const void *other) {

        if (S > sizeof(T)) {
            memcpy(obj, other, S);
        }

        new (obj) T(*reinterpret_cast<const T *>(other));
    }

    virtual void move_ctor(void *obj, void *other) {

        if (S > sizeof(T)) {
            memcpy(obj, other, S);
        }

        new (obj) T(move(*reinterpret_cast<T *>(other)));
    }

    virtual void copy_assign(void *obj, const void *other) {

        if (S > sizeof(T)) {
            memcpy(obj, other, S);
        }

        *reinterpret_cast<T *>(obj) = *reinterpret_cast<const T *>(other);
    }

    virtual void move_assign(void *obj, void *other) {

        if (S > sizeof(T)) {
            memcpy(obj, other, S);
        }

        *reinterpret_cast<T *>(obj) = move(*reinterpret_cast<T *>(other));
    }
};

template <class T>
class SharedType :
    public NonTrivialType<
        shared_ptr<typename T::inner_type>, sizeof(T)
    >
{
    typedef shared_ptr<typename T::inner_type> S;

public:
    SharedType(Type::TypeE e) : NonTrivialType<S, sizeof(T)>(e) { }
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
