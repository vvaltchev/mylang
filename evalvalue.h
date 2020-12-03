/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "errors.h"
#include <string_view>

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
        t_none     = 0,
        t_lval     = 1,
        t_undefid  = 2,
        t_int      = 3,
    };

    const TypeE t;
    Type(TypeE t) : t(t) { }

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

class TypeInt : public Type {

public:

    TypeInt() : Type(Type::t_int) { }

    virtual void add(EvalValue &a, EvalValue b);
    virtual void sub(EvalValue &a, EvalValue b);
    virtual void mul(EvalValue &a, EvalValue b);
    virtual void div(EvalValue &a, EvalValue b);
    virtual void mod(EvalValue &a, EvalValue b);
    virtual void lt(EvalValue &a, EvalValue b);
    virtual void gt(EvalValue &a, EvalValue b);
    virtual void le(EvalValue &a, EvalValue b);
    virtual void ge(EvalValue &a, EvalValue b);
    virtual void eq(EvalValue &a, EvalValue b);
    virtual void noteq(EvalValue &a, EvalValue b);
    virtual void opneg(EvalValue &a);
    virtual void lnot(EvalValue &a);
    virtual void land(EvalValue &a, EvalValue b);
    virtual void lor(EvalValue &a, EvalValue b);

    virtual bool is_true(EvalValue &a);
};


static Type *AllTypes[] = {

    new Type(Type::t_none),
    new Type(Type::t_lval),
    new Type(Type::t_undefid),
    new TypeInt(),
};

inline EvalValue::EvalValue()
    : val(), type(AllTypes[(int)Type::t_none]) { }

inline EvalValue::EvalValue(long val)
    : val(val), type(AllTypes[(int)Type::t_int]) { }

inline EvalValue::EvalValue(LValue *val)
    : val(val), type(AllTypes[(int)Type::t_lval]) { }

inline EvalValue::EvalValue(const UndefinedId &val)
    : val(val), type(AllTypes[(int)Type::t_undefid]) { }


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
