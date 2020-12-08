/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "errors.h"
#include <string_view>
#include <array>
#include <cassert>
#include <cstddef>

using namespace std;

class Type;
class LValue;
class ExprList;
class FuncDeclStmt;
class EvalValue;
class EvalContext;

struct NoneVal { };
struct UndefinedId { string_view id; };
struct Builtin { EvalValue (*func)(EvalContext *, ExprList *); };
class FuncObject;

template <class T>
struct SharedVal {

    typedef T type;

    char data[sizeof(shared_ptr<T>)] alignas(shared_ptr<T>);

    SharedVal() = default;

    SharedVal(const shared_ptr<T> &s) {
        new ((void *)data) shared_ptr<T>(s);
    }

    SharedVal(shared_ptr<T> &&s) {
        new ((void *)data) shared_ptr<T>(move(s));
    }

    shared_ptr<T> &to_shared_ptr() {
        return *reinterpret_cast<shared_ptr<T> *>(data);
    }

    T &get() {
        return *to_shared_ptr().get();
    }

    T get() const {
        /* Unfortunately, this const_cast is unavoidable */
        return *const_cast<SharedVal<T> *>(this).get();
    }

    long use_count() {
        return to_shared_ptr().use_count();
    }
};

typedef SharedVal<string> SharedStrWrapper;
typedef SharedVal<FuncObject> SharedFuncObjWrapper;

class EvalValue {

    union ValueU {

        NoneVal none;
        LValue *lval;
        UndefinedId undef;
        long ival;
        Builtin bfunc;
        SharedStrWrapper str;
        SharedFuncObjWrapper func;

        ValueU() : ival(0) { }
        ValueU(LValue *val) : lval(val) { }
        ValueU(const UndefinedId &val) : undef(val) { }
        ValueU(long val) : ival(val) { }
        ValueU(const Builtin &val) : bfunc(val) { }
    };


    ValueU val;
    Type *type;

    void create_val();
    void destroy_val();

public:

    EvalValue();
    EvalValue(long val);
    EvalValue(LValue *val);
    EvalValue(const UndefinedId &val);
    EvalValue(const Builtin &val);
    EvalValue(const SharedStrWrapper &val);
    EvalValue(SharedStrWrapper &&val);
    EvalValue(const SharedFuncObjWrapper &val);
    EvalValue(SharedFuncObjWrapper &&val);

    EvalValue(const EvalValue &other);
    EvalValue(EvalValue &&other);
    EvalValue &operator=(const EvalValue &other);
    EvalValue &operator=(EvalValue &&other);

    ~EvalValue();

    Type *get_type() const {
        return type;
    }

    template <class T>
    T &get() {

        static_assert(offsetof(ValueU, lval) == 0);
        static_assert(offsetof(ValueU, undef) == 0);
        static_assert(offsetof(ValueU, ival) == 0);
        static_assert(offsetof(ValueU, bfunc) == 0);
        static_assert(offsetof(ValueU, str) == 0);
        static_assert(offsetof(ValueU, func) == 0);

        if (is<T>())
            return *reinterpret_cast<T *>(&val);

        throw TypeErrorEx();
    }

    template <class T>
    T get() const {

        if (is<T>())
            return *reinterpret_cast<const T *>(&val);

        throw TypeErrorEx();
    }

    template <class T>
    bool is() const;
};

ostream &operator<<(ostream &s, const EvalValue &c);

class Type {

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

        /* Number of types */
        t_count,
    };

    const TypeE t;
    Type(TypeE t) : t(t) { assert(t != t_count); }

    virtual void add(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void sub(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void mul(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void div(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void mod(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void lt(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void gt(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void le(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void ge(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void eq(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void noteq(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void opneg(EvalValue &a) { throw TypeErrorEx(); }
    virtual void lnot(EvalValue &a) { throw TypeErrorEx(); }
    virtual void land(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }
    virtual void lor(EvalValue &a, const EvalValue &b) { throw TypeErrorEx(); }

    virtual bool is_true(const EvalValue &a) { throw TypeErrorEx(); }
    virtual string to_string(const EvalValue &a) { throw TypeErrorEx(); }
    virtual long len(const EvalValue &a) { throw TypeErrorEx(); }

    /* Helper functions for our custom variant */

    virtual void default_ctor(void *obj) { throw InternalErrorEx(); }
    virtual void dtor(void *obj) { throw InternalErrorEx(); }
    virtual void copy_ctor(void *obj, const void *other) { throw InternalErrorEx(); }
    virtual void move_ctor(void *obj, void *other) { throw InternalErrorEx(); }
    virtual void copy_assign(void *obj, const void *other) { throw InternalErrorEx(); }
    virtual void move_assign(void *obj, void *other) { throw InternalErrorEx(); }
};

extern const array<Type *, Type::t_count> AllTypes;

template <>
inline bool EvalValue::is<NoneVal>() const { return type->t == Type::t_none; }
template <>
inline bool EvalValue::is<LValue *>() const { return type->t == Type::t_lval; }
template <>
inline bool EvalValue::is<UndefinedId>() const { return type->t == Type::t_undefid; }
template <>
inline bool EvalValue::is<long>() const { return type->t == Type::t_int; }
template <>
inline bool EvalValue::is<Builtin>() const { return type->t == Type::Type::t_builtin; }
template <>
inline bool EvalValue::is<SharedStrWrapper>() const { return type->t == Type::t_str; }
template <>
inline bool EvalValue::is<SharedFuncObjWrapper>() const { return type->t == Type::t_func; }


inline EvalValue::EvalValue()
    : val(), type(AllTypes[Type::t_none]) { }

inline EvalValue::EvalValue(LValue *val)
    : val(val), type(AllTypes[Type::t_lval]) { }

inline EvalValue::EvalValue(const UndefinedId &val)
    : val(val), type(AllTypes[Type::t_undefid]) { }

inline EvalValue::EvalValue(long val)
    : val(val), type(AllTypes[Type::t_int]) { }

inline EvalValue::EvalValue(const Builtin &val)
    : val(val), type(AllTypes[Type::Type::t_builtin]) { }

inline EvalValue::EvalValue(const SharedStrWrapper &v)
    : type(AllTypes[Type::t_str])
{
    type->copy_ctor(
        reinterpret_cast<void *>( &val ),
        reinterpret_cast<const void *>( &v )
    );
}

inline EvalValue::EvalValue(SharedStrWrapper &&v)
    : type(AllTypes[Type::t_str])
{
    type->move_ctor(
        reinterpret_cast<void *>( &val ),
        reinterpret_cast<void *>( &v )
    );
}

inline EvalValue::EvalValue(const SharedFuncObjWrapper &v)
    : type(AllTypes[Type::t_func])
{
    type->copy_ctor(
        reinterpret_cast<void *>( &val ),
        reinterpret_cast<const void *>( &v )
    );
}

inline EvalValue::EvalValue(SharedFuncObjWrapper &&v)
    : type(AllTypes[Type::t_func])
{
    type->move_ctor(
        reinterpret_cast<void *>( &val ),
        reinterpret_cast<void *>( &v )
    );
}


inline EvalValue::EvalValue(const EvalValue &other)
    : type(other.type)
{
    if (type->t >= Type::t_str) {

        type->copy_ctor(
            reinterpret_cast<void *>( &val ),
            reinterpret_cast<const void *>( &other.val )
        );

    } else {

        val = other.val;
    }
}

inline EvalValue::EvalValue(EvalValue &&other)
    : type(other.type)
{
    if (type->t >= Type::t_str) {

        type->move_ctor(
            reinterpret_cast<void *>( &val ),
            reinterpret_cast<void *>( &other.val )
        );

        other.type = AllTypes[Type::t_none];

    } else {

        val = other.val;
    }
}

inline void EvalValue::create_val()
{
    if (type->t >= Type::t_str) {
        type->default_ctor(&val);
    }
}

inline void EvalValue::destroy_val()
{
    if (type->t >= Type::t_str) {
        type->dtor(&val);
    }

    type = AllTypes[Type::t_none];
}

inline EvalValue &EvalValue::operator=(const EvalValue &other)
{
    if (type != other.type) {
        destroy_val();
        type = other.type;
        create_val();
    }

    if (type->t >= Type::t_str) {

        type->copy_assign(
            reinterpret_cast<void *>( &val ),
            reinterpret_cast<const void *>( &other.val )
        );

    } else {

        val = other.val;
    }

    return *this;
}

inline EvalValue &EvalValue::operator=(EvalValue &&other)
{
    if (type != other.type) {
        destroy_val();
        type = other.type;
        create_val();
    }

    if (type->t >= Type::t_str) {

        type->move_assign(
            reinterpret_cast<void *>( &val ),
            reinterpret_cast<void *>( &other.val )
        );

        other.type = AllTypes[Type::t_none];

    } else {

        val = other.val;
    }

    return *this;
}

inline EvalValue::~EvalValue()
{
    destroy_val();
}


// ---------------------------------------------------------------

EvalValue RValue(const EvalValue &v);

inline bool
is_true(const EvalValue &v)
{
    const EvalValue &val = RValue(v);
    return val.get_type()->is_true(val);
}
