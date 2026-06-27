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
        t_bool,
        t_structtype,   /* a struct TYPE descriptor (raw StructTypeDef*) */

        /* Non-trivial types */
        t_str,
        t_func,
        t_arr,
        t_ex,
        t_dict,
        t_struct,       /* a struct INSTANCE (intrusive_ptr<StructObject>) */

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

    virtual void band(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator &");
    }

    virtual void bor(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator |");
    }

    virtual void bxor(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator ^");
    }

    virtual void shl(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator <<");
    }

    virtual void shr(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator >>");
    }

    virtual void ushr(EvalValueT &a, const EvalValueT &b) {
        throw TypeErrorEx("The object does NOT support operator >>>");
    }

    virtual void bnot(EvalValueT &a) {
        throw TypeErrorEx("The object does NOT support unary operator ~");
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

    /*
     * "repr" form: how a value is rendered INSIDE a container (array/dict/
     * struct), and by the REPL `=>` echo. A string is quoted + escaped so
     * `["a", 1]` is unambiguous and re-parseable (matching how print() shows a
     * list in Python, and IRB's quoted echo). Identical to to_string for every
     * type but str - a container's own to_string already repr's its elements.
     */
    virtual std::string to_string_repr(const EvalValueT &a) {
        return to_string(a);
    }

    /*
     * Multi-line "pretty" rendering for the REPL `=>` echo: a container whose
     * single-line repr would exceed `width` (measured from column `indent`) is
     * expanded one element per line, indented, recursively. Anything that fits
     * - and every scalar/string (the default here) - stays on one line as its
     * to_string_repr. Only array/dict/struct override it.
     */
    virtual std::string pretty(const EvalValueT &a, int indent, int width) {
        return to_string_repr(a);
    }

    virtual int_type len(const EvalValueT &a) {
        throw TypeErrorEx("The object does NOT support len()");
    }

    virtual void lnot(EvalValueT &a) {
        a = EvalValueT(!is_true(a));
    }

    /*
     * `for_write` is true only when the subscript is the target of a plain
     * assignment (`a[i] = v`); a dict uses it to auto-vivify a missing key
     * instead of throwing. Other types ignore it.
     */
    virtual EvalValueT subscript(const EvalValueT &what, const EvalValueT &idx,
                                 bool for_write = false)
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


