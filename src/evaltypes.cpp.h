/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include <cstring>

using std::string;
using std::string_view;

/*
 * Special base class for any Type class related to non-trivial C++ type.
 * Typically, the object is just a shared_ptr<T> and in that case it's enough
 * to derive the custom Type from NonTrivialType<shared_ptr<T>>. But, in some
 * special cases, like FlatSharedStr, there are additional trivial members related
 * to the non-trivial object that must be copied as well. Therefore NonTrivialType
 * has an additional non-type template param (S) which allows to specify the real
 * size of the container object. NOTE: the non-trivial object MUST BE at offset 0
 * in the wrapper type (e.g. FlatSharedStr).
 */

template <class T, size_t S = sizeof(T)>
class NonTrivialType : public Type {

public:

    NonTrivialType(Type::TypeE e) : Type(e) { }

    void default_ctor(void *obj) override {

        static_assert(S >= sizeof(T));

        if constexpr(S > sizeof(T)) {
            memset(obj, 0, S);
        }

        new (obj) T;
    }

    void dtor(void *obj) override {
        reinterpret_cast<T *>(obj)->~T();
    }

    void copy_ctor(void *obj, const void *other) override {

        if constexpr(S > sizeof(T)) {
            memcpy((char *)obj + sizeof(T), (char *)other + sizeof(T), S - sizeof(T));
        }

        new (obj) T(*reinterpret_cast<const T *>(other));
    }

    void move_ctor(void *obj, void *other) override {

        if constexpr(S > sizeof(T)) {
            memcpy((char *)obj + sizeof(T), (char *)other + sizeof(T), S - sizeof(T));
        }

        new (obj) T(move(*reinterpret_cast<T *>(other)));
    }

    void copy_assign(void *obj, const void *other) override {

        if constexpr(S > sizeof(T)) {
            memcpy((char *)obj + sizeof(T), (char *)other + sizeof(T), S - sizeof(T));
        }

        *reinterpret_cast<T *>(obj) = *reinterpret_cast<const T *>(other);
    }

    void move_assign(void *obj, void *other) override {

        if constexpr(S > sizeof(T)) {
            memcpy((char *)obj + sizeof(T), (char *)other + sizeof(T), S - sizeof(T));
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

    bool is_true(const EvalValue &a) override {
        return false;
    }

    string to_string(const EvalValue &a) override {
        return "<none>";
    }

    void eq(EvalValue &a, const EvalValue &b) override {
        a = b.is<NoneVal>();
    }

    void noteq(EvalValue &a, const EvalValue &b) override {
        a = !b.is<NoneVal>();
    }
};

string_view
find_builtin_name(const Builtin &b)
{
    for (const auto &[k, v]: EvalContext::const_builtins) {
        if (v.is<Builtin>() && v.getval<Builtin>().func == b.func)
            return k->val;
    }

    for (const auto &[k, v]: EvalContext::builtins) {
        if (v.is<Builtin>() && v.getval<Builtin>().func == b.func)
            return k->val;
    }

    throw InternalErrorEx();
}

class TypeBuiltin : public Type {

public:
    TypeBuiltin() : Type(Type::t_builtin) { }
    string to_string(const EvalValue &a) override {
        return "<Builtin(" + string(find_builtin_name(a.get<Builtin>())) + ")>";
    }
};
