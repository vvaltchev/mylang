/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "errors.h"
#include <string_view>
#include <cassert>

class LValue;
struct NoneVal { };
struct UndefinedId { string_view id; };

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

        /* Non-trivial types */
        t_str,
        t_func,
        t_arr,

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

    virtual void add(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void sub(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void mul(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void div(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void mod(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void lt(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void gt(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void le(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void ge(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void eq(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void noteq(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void opneg(EvalValueT &a) { throw TypeErrorEx(); }
    virtual void lnot(EvalValueT &a) { throw TypeErrorEx(); }
    virtual void land(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }
    virtual void lor(EvalValueT &a, const EvalValueT &b) { throw TypeErrorEx(); }

    virtual bool is_true(const EvalValueT &a) { throw TypeErrorEx(); }
    virtual string to_string(const EvalValueT &a) { throw TypeErrorEx(); }
    virtual long len(const EvalValueT &a) { throw TypeErrorEx(); }

    virtual EvalValueT subscript(const EvalValueT &what, const EvalValueT &idx)
    {
        throw TypeErrorEx();
    }

    virtual EvalValueT slice(const EvalValueT &what,
                             const EvalValueT &start,
                             const EvalValueT &end)
    {
        throw TypeErrorEx();
    }

    virtual long use_count(const EvalValueT &a)
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
        return reinterpret_cast<long>(&a);
    }
};


