/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "errors.h"
#include <string_view>
#include <array>
#include <cassert>

using namespace std;

class Type;
class LValue;
struct UndefinedId { string_view id; };

union ValueU {

    long ival;
    LValue *lval;
    UndefinedId undef;

    ValueU() : ival(0) { }
    ValueU(long val) : ival(val) { }
    ValueU(LValue *val) : lval(val) { }
    ValueU(const UndefinedId &val) : undef(val) { }
};

class EvalValue {

public:

    ValueU val;
    Type *type;

    EvalValue(const EvalValue &rhs) = default;
    EvalValue(EvalValue &&rhs) = default;
    EvalValue &operator=(const EvalValue &rhs) = default;
    EvalValue &operator=(EvalValue &&rhs) = default;
    virtual ~EvalValue() = default;

    EvalValue();
    EvalValue(long val);
    EvalValue(LValue *val);
    EvalValue(const UndefinedId &val);

    template <class T>
    T get() const;

    template <class T>
    bool is() const;
};

class Type {

public:

    enum TypeE : int {

        t_none,
        t_lval,
        t_undefid,
        t_int,

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

    virtual bool is_true(EvalValue &a) { throw TypeErrorEx(); }
};

extern const array<Type *, Type::t_count> AllTypes;

inline EvalValue::EvalValue()
    : val(), type(AllTypes[Type::t_none]) { }

inline EvalValue::EvalValue(long val)
    : val(val), type(AllTypes[Type::t_int]) { }

inline EvalValue::EvalValue(LValue *val)
    : val(val), type(AllTypes[Type::t_lval]) { }

inline EvalValue::EvalValue(const UndefinedId &val)
    : val(val), type(AllTypes[Type::t_undefid]) { }


template <>
inline bool EvalValue::is<nullptr_t>() const { return type->t == Type::t_none; }
template <>
inline bool EvalValue::is<LValue *>() const { return type->t == Type::t_lval; }
template <>
inline bool EvalValue::is<UndefinedId>() const { return type->t == Type::t_undefid; }
template <>
inline bool EvalValue::is<long>() const { return type->t == Type::t_int; }

template <>
inline LValue *EvalValue::get<LValue *>() const {

    if (is<LValue *>())
        return val.lval;

    throw TypeErrorEx();
}

template <>
inline UndefinedId EvalValue::get<UndefinedId>() const {

    if (is<UndefinedId>())
        return val.undef;

    throw TypeErrorEx();
}

template <>
inline long EvalValue::get<long>() const {

    if (is<long>())
        return val.ival;

    throw TypeErrorEx();
}

EvalValue RValue(EvalValue v);

inline bool
is_true(EvalValue v)
{
    EvalValue val = RValue(v);
    return val.type->is_true(val);
}
