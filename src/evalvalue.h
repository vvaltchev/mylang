/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "errors.h"
#include "flat/flatval.h"
#include "flat/sharedstr.h"
#include "flat/sharedarray.h"
#include "flat/sharedexception.h"
#include "flat/shareddict.h"
#include "type.h"

#include <string_view>
#include <vector>
#include <string>
#include <memory>
#include <array>
#include <type_traits>
#include <cassert>
#include <cstddef>

class LValue;
class ExprList;
class FuncDeclStmt;
class EvalValue;
class EvalContext;

class FuncObject;
struct Builtin { EvalValue (*func)(EvalContext *, ExprList *); };

/* Base typedefs */
typedef DictObjectTempl<EvalValue, LValue> DictObject;
typedef ExceptionObjectTempl<EvalValue> ExceptionObject;

/* Flat type wrapper */
typedef FlatSharedVal<FuncObject> FlatSharedFuncObj;
typedef FlatSharedArrayTempl<LValue> FlatSharedArray;
typedef FlatSharedExceptionTempl<EvalValue> FlatSharedException;
typedef FlatSharedVal<DictObject> FlatSharedDictObj;

/* Other types */
typedef TypeTemplate<EvalValue> Type;
typedef ArrayConstViewTempl<LValue> ArrayConstView;

extern const std::array<Type *, Type::t_count> AllTypes;

template <class T>
struct TypeToEnum;

template <> struct TypeToEnum<NoneVal> { enum { val = Type::t_none }; };
template <> struct TypeToEnum<LValue *> { enum { val = Type::t_lval }; };
template <> struct TypeToEnum<UndefinedId> { enum { val = Type::t_undefid }; };
template <> struct TypeToEnum<int_type> { enum { val = Type::t_int }; };
template <> struct TypeToEnum<Builtin> { enum { val = Type::t_builtin }; };
template <> struct TypeToEnum<float_type> { enum { val = Type::t_float }; };
template <> struct TypeToEnum<FlatSharedStr> { enum { val = Type::t_str }; };
template <> struct TypeToEnum<FlatSharedFuncObj> { enum { val = Type::t_func }; };
template <> struct TypeToEnum<FlatSharedArray> { enum { val = Type::t_arr }; };
template <> struct TypeToEnum<FlatSharedException> { enum { val = Type::t_ex }; };
template <> struct TypeToEnum<FlatSharedDictObj> { enum { val = Type::t_dict }; };

class EvalValue final {

    /*
     * Why not just using std::variant? Because it's much slower.
     * See flatval.h.
     */
    union ValueU {

        /* trivial types */
        NoneVal none;
        LValue *lval;
        UndefinedId undef;
        int_type ival;
        Builtin bfunc;
        float_type ldval;

        /* non-trivial types */
        FlatSharedStr str;
        FlatSharedFuncObj func;
        FlatSharedArray arr;
        FlatSharedException ex;
        FlatSharedDictObj dict;

        ValueU() : ival(0) { }
        ValueU(LValue *val) : lval(val) { }
        ValueU(const UndefinedId &val) : undef(val) { }
        ValueU(int_type val) : ival(val) { }
        ValueU(const Builtin &val) : bfunc(val) { }
        ValueU(float_type val) : ldval(val) { }
    };


    ValueU val;
    Type *type;

    void create_val();
    void destroy_val();

public:

    EvalValue()
        : val(), type(AllTypes[Type::t_none]) { }

    /*
     * Constructor accepting bool. SFINAE is used to prevent implicit
     * conversions from pointer types.
     */
    template <
        class T,
        class = std::enable_if_t<                      /* SFINAE template param */
            std::is_same_v<T, bool> ||                 /* disallow for T != bool, int */
            std::is_same_v<T, int>
        >
    >
    EvalValue(T val)
        : val(static_cast<int_type>(val)), type(AllTypes[Type::t_int]) { }

    /*
     * Constructor accepting ONLY known types defined in enum TypeE.
     */
    template <
        class T,                                       /* actual template param */
        class U = std::remove_const_t<                 /* helper template param */
            std::remove_reference_t<T>
        >,
        class = std::enable_if_t<                      /* SFINAE template param */
            !std::is_same_v<U, EvalValue> &&           /* disallow EvalValue */
            TypeToEnum<U>::val != Type::t_count        /* disallow types not in TypeToEnum */
        >
    >
    EvalValue(T &&val);


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
        static_assert(offsetof(ValueU, ldval) == 0);
        static_assert(offsetof(ValueU, str) == 0);
        static_assert(offsetof(ValueU, func) == 0);
        static_assert(offsetof(ValueU, arr) == 0);
        static_assert(offsetof(ValueU, ex) == 0);
        static_assert(offsetof(ValueU, dict) == 0);

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
    bool is() const {
        return type->t == static_cast<Type::TypeE>(TypeToEnum<T>::val);
    }

    EvalValue clone() const {

        if (type->t < Type::t_str)
            return *this;

        return type->clone(*this);
    }

    bool is_true() const {
        return type->is_true(*this);
    }

    bool operator!() const {
        return !is_true();
    }

    bool operator==(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        tmp.type->eq(tmp, rhs);
        return tmp.get<int_type>() != 0;
    }

    bool operator!=(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        tmp.type->noteq(tmp, rhs);
        return tmp.get<int_type>() != 0;
    }

    bool operator<(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        tmp.type->lt(tmp, rhs);
        return tmp.get<int_type>() != 0;
    }

    bool operator<=(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        tmp.type->le(tmp, rhs);
        return tmp.get<int_type>() != 0;
    }

    bool operator>(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        tmp.type->gt(tmp, rhs);
        return tmp.get<int_type>() != 0;
    }

    bool operator>=(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        tmp.type->ge(tmp, rhs);
        return tmp.get<int_type>() != 0;
    }

    std::string to_string() const {
        return type->to_string(*this);
    };

    size_t hash() const {
        return type->hash(*this);
    };
};

namespace std {
    template<> struct hash<EvalValue>
    {
        size_t operator()(EvalValue const& e) const
        {
            return e.hash();
        }
    };
}

extern const EvalValue empty_str;
extern const EvalValue empty_arr;
extern const EvalValue none;

ostream &operator<<(ostream &s, const EvalValue &c);

template <class T, class U, class S>
inline EvalValue::EvalValue(T &&new_val)
    : type(AllTypes[TypeToEnum<U>::val])
{
    if constexpr(static_cast<Type::TypeE>(TypeToEnum<U>::val) >= Type::t_str) {

        if constexpr(std::is_lvalue_reference_v<T>) {

            type->copy_ctor(
                reinterpret_cast<void *>( &val ),
                reinterpret_cast<const void *>( &new_val )
            );

        } else {

            type->move_ctor(
                reinterpret_cast<void *>( &val ),
                reinterpret_cast<void *>( &new_val )
            );
        }

    } else {

        val = ValueU(new_val);
    }
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

class LValue final {

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
    size_type container_idx;

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
    Type *valtype() const { return val.get_type(); }

    template <class T>
    T &getval() { return val.get<T>(); }

    template <class T>
    T getval() const { return val.get<T>(); }

    template <class T>
    bool is() const { return val.is<T>(); }

    bool operator==(const LValue &rhs) const {
        return val == rhs.val;
    }

    bool operator!=(const LValue &rhs) const {
        return val != rhs.val;
    }
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

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const std::vector<EvalValue> &args);

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const EvalValue &arg);

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const std::pair<EvalValue, EvalValue> &args);
