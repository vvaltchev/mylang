/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "errors.h"
#include <string_view>
#include <array>
#include <cassert>

using namespace std;

class Type;
class LValue;
class ExprList;
class EvalValue;
class EvalContext;

struct UndefinedId { string_view id; };
struct Builtin { EvalValue (*func)(EvalContext *, ExprList *); };

class EvalValue {

    union ValueU {

        long ival;
        LValue *lval;
        UndefinedId undef;
        Builtin bfunc;

        ValueU() : ival(0) { }
        ValueU(long val) : ival(val) { }
        ValueU(LValue *val) : lval(val) { }
        ValueU(const UndefinedId &val) : undef(val) { }
        ValueU(const Builtin &val) : bfunc(val) { }
    };


    ValueU val;
    Type *type;

public:

    EvalValue();
    EvalValue(long val);
    EvalValue(LValue *val);
    EvalValue(const UndefinedId &val);
    EvalValue(const Builtin &val);

    Type *get_type() const {
        return type;
    }

    template <class T>
    T &get();

    template <class T>
    T get() const {
        return const_cast<EvalValue *>(this)->get<T>();
    }

    template <class T>
    bool is() const;
};

ostream &operator<<(ostream &s, const EvalValue &c);

class Type {

public:

    enum TypeE : int {

        t_none,
        t_lval,
        t_undefid,
        t_int,
        t_builtin,

        t_count,
    };

    const TypeE t;
    Type(TypeE t) : t(t) { assert(t != t_count); }

    virtual void add(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void sub(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void mul(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void div(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void mod(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void lt(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void gt(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void le(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void ge(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void eq(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void noteq(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void opneg(EvalValue &a) { throw TypeErrorEx(); }
    virtual void lnot(EvalValue &a) { throw TypeErrorEx(); }
    virtual void land(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }
    virtual void lor(EvalValue &a, EvalValue b) { throw TypeErrorEx(); }

    virtual bool is_true(const EvalValue &a) { throw TypeErrorEx(); }
    virtual string to_string(const EvalValue &a) { throw TypeErrorEx(); }
};

extern const array<Type *, Type::t_count> AllTypes;

inline EvalValue::EvalValue()
    : val(), type(AllTypes[Type::t_none]) { }

inline EvalValue::EvalValue(LValue *val)
    : val(val), type(AllTypes[Type::t_lval]) { }

inline EvalValue::EvalValue(const UndefinedId &val)
    : val(val), type(AllTypes[Type::t_undefid]) { }

inline EvalValue::EvalValue(long val)
    : val(val), type(AllTypes[Type::t_int]) { }

inline EvalValue::EvalValue(const Builtin &val)
    : val(val), type(AllTypes[Type::t_builtin]) { }


template <>
inline bool EvalValue::is<nullptr_t>() const { return get_type()->t == Type::t_none; }
template <>
inline bool EvalValue::is<LValue *>() const { return get_type()->t == Type::t_lval; }
template <>
inline bool EvalValue::is<UndefinedId>() const { return get_type()->t == Type::t_undefid; }
template <>
inline bool EvalValue::is<long>() const { return get_type()->t == Type::t_int; }
template <>
inline bool EvalValue::is<Builtin>() const { return get_type()->t == Type::t_builtin; }


template <>
inline LValue *&EvalValue::get<LValue *>() {

    if (is<LValue *>())
        return val.lval;

    throw TypeErrorEx();
}

template <>
inline UndefinedId &EvalValue::get<UndefinedId>() {

    if (is<UndefinedId>())
        return val.undef;

    throw TypeErrorEx();
}

template <>
inline long &EvalValue::get<long>() {

    if (is<long>())
        return val.ival;

    throw TypeErrorEx();
}

template <>
inline Builtin &EvalValue::get<Builtin>() {

    if (is<Builtin>())
        return val.bfunc;

    throw TypeErrorEx();
}

EvalValue RValue(EvalValue v);

inline bool
is_true(EvalValue v)
{
    EvalValue val = RValue(v);
    return val.get_type()->is_true(val);
}
