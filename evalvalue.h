/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "errors.h"
#include "sharedval.h"
#include "sharedstr.h"

#include <string_view>
#include <vector>
#include <string>
#include <memory>
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

class FuncObject;
class SharedArray;
struct NoneVal { };
struct UndefinedId { string_view id; };
struct Builtin { EvalValue (*func)(EvalContext *, ExprList *); };

typedef SharedVal<FuncObject> SharedFuncObjWrapper;

class SharedArray {

    friend class TypeArr;

public:

    typedef vector<LValue> inner_type;

private:

    /* See SharedStr */
    inner_type &get_ref() const {
        return const_cast<SharedArray *>(this)->vec.get();
    }

    /* See SharedStr */
    long use_count() const {
        return const_cast<SharedArray *>(this)->vec.use_count();
    }

public:

    SharedVal<inner_type> vec;
    unsigned off;   /* NOTE: cannot be const because we're using this in a union */
    unsigned len;   /* NOTE: cannot be const because we're using this in a union */

    SharedArray() = default;
    SharedArray(const inner_type &arr) = delete;
    SharedArray(inner_type &&arr);

    unsigned size() const {
        return const_cast<SharedArray *>(this)->len;
    }
};

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
        t_arr,

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
    virtual EvalValue subscript(const EvalValue &what, const EvalValue &idx);
    virtual EvalValue slice(const EvalValue &what,
                            const EvalValue &start,
                            const EvalValue &end);

    virtual long use_count(const EvalValue &a);
    virtual EvalValue clone(const EvalValue &a);

    /* Helper functions for our custom variant */

    virtual void default_ctor(void *obj) { throw InternalErrorEx(); }
    virtual void dtor(void *obj) { throw InternalErrorEx(); }
    virtual void copy_ctor(void *obj, const void *other) { throw InternalErrorEx(); }
    virtual void move_ctor(void *obj, void *other) { throw InternalErrorEx(); }
    virtual void copy_assign(void *obj, const void *other) { throw InternalErrorEx(); }
    virtual void move_assign(void *obj, void *other) { throw InternalErrorEx(); }
};

extern const array<Type *, Type::t_count> AllTypes;

class EvalValue {

    union ValueU {

        /* trivial types */
        NoneVal none;
        LValue *lval;
        UndefinedId undef;
        long ival;
        Builtin bfunc;

        /* non-trivial types */
        SharedStr str;
        SharedFuncObjWrapper func;
        SharedArray arr;

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
    EvalValue(const SharedStr &val);
    EvalValue(SharedStr &&val);
    EvalValue(const SharedFuncObjWrapper &val);
    EvalValue(SharedFuncObjWrapper &&val);
    EvalValue(const SharedArray &val);
    EvalValue(SharedArray &&val);

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
        static_assert(offsetof(ValueU, arr) == 0);

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

    static const EvalValue empty_str;
};

ostream &operator<<(ostream &s, const EvalValue &c);

inline EvalValue Type::subscript(const EvalValue &what, const EvalValue &idx)
{
    throw TypeErrorEx();
}

inline EvalValue Type::slice(const EvalValue &what,
                             const EvalValue &start,
                             const EvalValue &end)
{
    throw TypeErrorEx();
}

inline long Type::use_count(const EvalValue &a)
{
    return 1;
}

inline EvalValue Type::clone(const EvalValue &a)
{
    return a;
}

#define DEFINE_EVALVALUE_IS(type_class, type_enum)    \
    template <>                                       \
    inline bool EvalValue::is<type_class>() const {   \
        return type->t == Type::type_enum;            \
    }

DEFINE_EVALVALUE_IS(NoneVal, t_none)
DEFINE_EVALVALUE_IS(LValue *, t_lval)
DEFINE_EVALVALUE_IS(UndefinedId, t_undefid)
DEFINE_EVALVALUE_IS(long, t_int)
DEFINE_EVALVALUE_IS(Builtin, t_builtin)
DEFINE_EVALVALUE_IS(SharedStr, t_str)
DEFINE_EVALVALUE_IS(SharedFuncObjWrapper, t_func)
DEFINE_EVALVALUE_IS(SharedArray, t_arr)

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

inline EvalValue::EvalValue(const SharedStr &v)
    : type(AllTypes[Type::t_str])
{
    type->copy_ctor(
        reinterpret_cast<void *>( &val ),
        reinterpret_cast<const void *>( &v )
    );
}

inline EvalValue::EvalValue(SharedStr &&v)
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

inline EvalValue::EvalValue(const SharedArray &v)
    : type(AllTypes[Type::t_arr])
{
    type->copy_ctor(
        reinterpret_cast<void *>( &val ),
        reinterpret_cast<const void *>( &v )
    );
}

inline EvalValue::EvalValue(SharedArray &&v)
    : type(AllTypes[Type::t_arr])
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

class LValue {

    EvalValue val;
    bool is_const;

    LValue clone();
    EvalValue &get_value_for_put();

    void type_checks() const {
        assert(val.get_type()->t != Type::t_lval);
        assert(val.get_type()->t != Type::t_undefid);
    }

public:

    /* Used only by TypeArr::subscript() */
    LValue *container;
    unsigned container_idx;

    LValue(const EvalValue &val, bool is_const)
        : val(val)
        , is_const(is_const)
        , container(nullptr)
    {
        type_checks();
    }

    LValue(EvalValue &&val, bool is_const)
        : val(move(val))
        , is_const(is_const)
        , container(nullptr)
    {
        type_checks();
    }

    void put(const EvalValue &v);
    void put(EvalValue &&v);

    bool is_const_var() const { return is_const; }
    const EvalValue &get() const { return val; }
    EvalValue get_rval() const { return val; }

    template <class T>
    T &getval() { return val.get<T>(); }

    template <class T>
    T getval() const { return val.get<T>(); }

    template <class T>
    bool is() const { return val.is<T>(); }
};

inline EvalValue
RValue(const EvalValue &v)
{
    if (v.is<LValue *>())
        return v.get<LValue *>()->get();

    if (v.is<UndefinedId>())
        throw UndefinedVariableEx(v.get<UndefinedId>().id);

    return v;
}

inline bool
is_true(const EvalValue &v)
{
    const EvalValue &val = RValue(v);
    return val.get_type()->is_true(val);
}
