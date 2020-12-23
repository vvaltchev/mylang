/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "errors.h"
#include <string_view>
#include <cassert>

class LValue;
struct NoneVal { };
struct UndefinedId { std::string_view id; };

/*
 * This class is a template simply because otherwise this header wouldn't be
 * able to compile if included independently (it requires EvalValue which requires
 * Type). In this case, it wouldn't be a big deal, but in general it's an anti-pattern
 * to have headers requiring a specific include order.
 */

class TypeBaseDefs {

public:

    enum TypeE : int {

        /* Trivial types */
        t_none,
        t_lval,
        t_undefid,
        t_int,
        t_builtin,
        t_float,

        /* Non-trivial types */
        t_str,
        t_func,
        t_arr,
        t_ex,
        t_dict,

        /* Number of types */
        t_count,
    };
};

class TypeErasureOps {

public:

    /* Helper functions needed for our custom "variant" */
    virtual void default_ctor(void *obj) { throw InternalErrorEx(); }
    virtual void dtor(void *obj) { throw InternalErrorEx(); }
    virtual void copy_ctor(void *obj, const void *other) { throw InternalErrorEx(); }
    virtual void move_ctor(void *obj, void *other) { throw InternalErrorEx(); }
    virtual void copy_assign(void *obj, const void *other) { throw InternalErrorEx(); }
    virtual void move_assign(void *obj, void *other) { throw InternalErrorEx(); }
};

template <class EvalValueT>
class TypeTemplate : public TypeBaseDefs, public TypeErasureOps {

public:

    const TypeE t;
    TypeTemplate(TypeE t) : t(t) { assert(t != t_count); }

    virtual void add(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator +");
    }

    virtual void sub(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator -");
    }

    virtual void mul(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator *");
    }

    virtual void div(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator /");
    }

    virtual void mod(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator %");
    }

    virtual void lt(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator <");
    }

    virtual void gt(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator >");
    }

    virtual void le(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator <=");
    }

    virtual void ge(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator >=");
    }

    virtual void eq(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator ==");
    }

    virtual void noteq(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator !=");
    }

    virtual void opneg(EvalValueT &a) {
        throw TypeErrorEx("The object does NOT support unary operator - (negation)");
    }

    virtual void land(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator &&");
    }

    virtual void lor(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator ||");
    }

    virtual bool is_true(const EvalValueT &a) {
        throw TypeErrorEx("The object does NOT support conversion to bool");
    }

    virtual std::string to_string(const EvalValueT &a) {
        throw TypeErrorEx("The object does NOT support conversion to string");
    }

    virtual int_type len(const EvalValueT &a) {
        throw TypeErrorEx("The object does NOT support len()");
    }

    virtual void lnot(EvalValueT &a) {
        a = EvalValueT(!is_true(a));
    }

    virtual EvalValueT subscript(const EvalValueT &what, const EvalValueT &idx)
    {
        throw TypeErrorEx("The object does NOT support subscript operator []");
    }

    virtual EvalValueT slice(const EvalValueT &what,
                             const EvalValueT &start,
                             const EvalValueT &end)
    {
        throw TypeErrorEx("The object does NOT support slice operator []");
    }

    virtual int_type use_count(const EvalValueT &a)
    {
        return 1;
    }

    virtual bool is_slice(const EvalValueT &a)
    {
        return false;
    }

    virtual EvalValueT clone(const EvalValueT &a)
    {
        return a;
    }

    virtual EvalValueT intptr(const EvalValueT &a)
    {
        assert(!a.template is<LValue *>() && !a.template is<UndefinedId>());
        return reinterpret_cast<int_type>(&a);
    }

    virtual size_t hash(const EvalValueT &a)
    {
        throw TypeErrorEx("The object does NOT support hash()");
    }
};


