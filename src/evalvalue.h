/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "errors.h"
#include "flatval.h"
#include "sharedstr.h"
#include "sharedarray.h"
#include "exceptionobj.h"
#include "shareddict.h"
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
struct StructTypeDef;   /* a struct type descriptor (structtype.h) */
class StructObject;     /* a struct instance (structtype.h) */
enum class ArrHint : unsigned char;   /* defined in syntax.h */

/*
 * The AST-FREE data a value-ABI builtin (func_v) needs besides its (already-
 * evaluated) arg VALUES: the source carets for its error messages and the
 * array-repr hint. It replaces the `ExprList *` a func_v used to take purely for
 * locs/hint - so a builtin reports the SAME carets without any AST pointer. The
 * tree-walker fills it from the real ExprList (per-arg `elems[i]->start/end`);
 * the VM fills it from a serializable Chunk pool. `arg(i)` is the i-th arg's
 * caret; `start`/`end` the whole args-list caret (arity / generic errors).
 */
struct ArgLoc { Loc start, end; };
struct ArgLocs {
    Loc start, end;                     /* the args-list caret */
    const ArgLoc *args = nullptr;       /* per-arg carets, [0, nargs) */
    size_t nargs = 0;                   /* the TOTAL arg count (incl. arg0 for a
                                         * func_lv) - a func_v also gets it as its
                                         * own `n` param, but a SELF-EVAL func_lv
                                         * (pop/intptr) is passed n_rest==0
                                         * regardless of arity, so its arity check
                                         * must read this, not n_rest. */
    ArrHint arr_hint;                   /* the array-repr hint (range/array/…) */
    const ArgLoc *arg(size_t i) const { return &args[i]; }
};
/*
 * A builtin. `func` is the ORIGINAL ABI (self-evaluates the unevaluated arg
 * nodes) - always set; the tree-walker calls it. `func_v` is the VM's
 * VALUE ABI: the args are ALREADY evaluated into `args[0..n)` and passed by
 * value (no node->eval); the AST-FREE `ArgLocs` carries the error carets +
 * repr hint it used to reach through the arg nodes (built by the tree-walker
 * adapter from the ExprList, by the VM from a serializable Chunk pool - so a
 * value-ABI builtin holds NO AST pointer). It is non-null only for a migrated,
 * read-only builtin; the VM's CallBuiltinV uses it, else it falls back to
 * `func` via EvalToSlot. A MUTATING
 * builtin (append/pop/...) uses the `func_lv` form below instead (arg0 is an
 * lvalue); an AST builtin (defined/type) needs the arg node, so it keeps the
 * union null and stays a fallback. For a migrated builtin `func` is a generic
 * adapter (make_const_builtin_v / make_builtin_lv) that prepares the args and
 * calls the native form - so the two engines share one implementation.
 */
struct Builtin {
    EvalValue (*func)(EvalContext *, ExprList *);
    /*
     * The native (VM / tree-walker-adapter) form. ONE of two shapes, mutually
     * exclusive per builtin, so they SHARE storage in a union - keeping Builtin
     * at two pointers (EvalValue stays 32). Which one is live is decided by
     * DirectBuiltinCallExpr::lvalue_arg0 (the VM) / the registered adapter (the
     * tree-walker), never by reading both:
     *   func_v  - a READ-ONLY builtin: args pre-evaluated by VALUE, no lvalue.
     *   func_lv - a MUTATING builtin: arg0 handed over as an LValue* target,
     *             plus the "REST" args - the TAIL ARGS BY VALUE, i.e. args 1..n,
     *             everything after the arg0 lvalue - either pre-evaluated in
     *             `rest`/`n_rest` (a REST-NATIVE builtin: insert/erase, via
     *             make_builtin_lv_v + lvalue_rest_native) or `rest == nullptr`
     *             for a SELF-EVAL builtin (append/push/pop/intptr), which reads
     *             its args off `exprList` - so append keeps construct-in-place,
     *             which needs the arg node.
     * Null (union zero) == not migrated: the VM falls back to func (EvalStmt).
     */
    union {
        EvalValue (*func_v)(EvalContext *, const ArgLocs *, const EvalValue *,
                            size_t);
        /* AST-FREE like func_v: the carets/arity come from `ArgLocs` (not an
         * ExprList). `rest`/`n_rest` = the TAIL ARGS BY VALUE (args 1..n,
         * everything after the arg0 `target` lvalue), pre-evaluated; a func_lv
         * NEVER self-evaluates off a node now (append/push construct-in-place is
         * the tree-walker's append_tw + the VM's EmplaceStruct). (Name from the
         * rest-parameter idiom: target = the head/first arg, rest = the tail.) */
        EvalValue (*func_lv)(EvalContext *, const ArgLocs *, LValue *,
                             const EvalValue *, size_t);
    };

    /*
     * The ABI KIND, so an INDIRECT (dyn-callee) call can dispatch from the
     * VALUE alone - a direct call knows the kind at compile time
     * (DirectBuiltinCallExpr::lvalue_arg0), an indirect one does not (F1
     * step 2). Fits the EvalValue payload (Builtin grows 16 -> 24 <= the
     * 24-byte union max member; EvalValue stays 32).
     *   value   - func_v live (a migrated read-only builtin).
     *   lvalue  - func_lv live (arg0 handed over as an LValue*).
     *   map / filter - the validate-order pair: a DIRECT call checks arg0 is
     *             a function BEFORE evaluating arg1; an INDIRECT call is
     *             EAGER-ARGS by language rule (dispatch_builtin_values).
     *   lazy    - the arg is a NODE property (defined/isconst/isconstdecl;
     *             dev-only show): value-uses are compile-rejected in scripts,
     *             the REPL dispatches via its retained node.
     *   node    - ExprList-only, none of the above (no live builtin is left
     *             in this kind - a tripwire for an unmigrated straggler).
     */
    enum class Kind : unsigned char { node, value, lvalue, map, filter, lazy };
    Kind kind = Kind::node;

    /*
     * Explicit ctors so no construction leaves the anonymous union member
     * uninitialized - aggregate init `Builtin{f}` / `Builtin{nullptr}` used to,
     * which -Wmissing-field-initializers (rightly) flagged. Both pointers null
     * == "not migrated" (the VM falls back to `func`). A MUTATING builtin is
     * built via the 1-arg ctor (func set, union zeroed) then `b.func_lv = FLV`
     * + `b.kind = Kind::lvalue`. Only ctors are user-declared, so Builtin
     * stays trivially COPYABLE (the union's bit-copy of a t_builtin value is
     * unaffected; a default member initializer doesn't change that).
     */
    Builtin() : func(nullptr), func_v(nullptr) { }
    explicit Builtin(decltype(func) f) : func(f), func_v(nullptr) { }
    Builtin(decltype(func) f, decltype(func_v) fv)
        : func(f), func_v(fv), kind(fv ? Kind::value : Kind::node) { }
};

/* Base typedefs for non-generic template types */
typedef DictObjectTempl<EvalValue, LValue> DictObject;
typedef ExceptionObjectTempl<EvalValue> ExceptionObject;
typedef SharedArrayObjTempl<LValue> SharedArrayObj;

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
template <> struct TypeToEnum<bool> { enum { val = Type::t_bool }; };
template <> struct TypeToEnum<SharedStr> { enum { val = Type::t_str }; };
template <> struct TypeToEnum<intrusive_ptr<FuncObject>> { enum { val = Type::t_func }; };
template <> struct TypeToEnum<SharedArrayObj> { enum { val = Type::t_arr }; };
template <> struct TypeToEnum<intrusive_ptr<ExceptionObject>> { enum { val = Type::t_ex }; };
template <> struct TypeToEnum<intrusive_ptr<DictObject>> { enum { val = Type::t_dict }; };
template <> struct TypeToEnum<StructTypeDef *> { enum { val = Type::t_structtype }; };
template <> struct TypeToEnum<intrusive_ptr<StructObject>> { enum { val = Type::t_struct }; };

/*
 * Binary-operation dispatch with int -> float promotion.
 *
 * Binary ops dispatch on the LEFT operand's type, and TypeFloat already
 * accepts an int right operand, so the only combination the type classes do
 * not handle themselves is an int left operand with a float right operand:
 * promote the left to float so the call lands in TypeFloat and int-OP-float
 * behaves exactly like float-OP-int. This is the single place mixed int/float
 * promotion happens; it is used for arithmetic and for comparison (including
 * == / !=). It is a no-op for any non-(int,float) operand pair, so string,
 * array, dict, etc. comparisons are dispatched unchanged.
 *
 * Defined out-of-line below, once EvalValue is a complete type.
 */
using NumBinOp = void (Type::*)(EvalValue &, const EvalValue &);
void num_bin_op(EvalValue &a, const EvalValue &b, NumBinOp op);

class EvalValue final {

    /*
     * Why not just using std::variant? Because it's much slower.
     * See the comments in flatval.h.
     */
    union ValueU {

        /* trivial types */
        NoneVal none;
        LValue *lval;
        UndefinedId undef;
        int_type ival;
        Builtin bfunc;
        float_type ldval;
        bool bval;          /* t_bool; aliases ival's low byte */
        StructTypeDef *structtype;   /* t_structtype (raw, AST-owned) */

        /* non-trivial types */
        FlatVal<SharedStr> str;
        FlatVal<intrusive_ptr<FuncObject>> func;
        FlatVal<SharedArrayObj> arr;
        FlatVal<intrusive_ptr<ExceptionObject>> ex;
        FlatVal<intrusive_ptr<DictObject>> dict;
        FlatVal<intrusive_ptr<StructObject>> struct_;   /* t_struct */

        ValueU() : ival(0) { }
        ValueU(LValue *val) : lval(val) { }
        ValueU(const UndefinedId &val) : undef(val) { }
        ValueU(int_type val) : ival(val) { }
        ValueU(const Builtin &val) : bfunc(val) { }
        ValueU(float_type val) : ldval(val) { }
        ValueU(StructTypeDef *val) : structtype(val) { }
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
            std::is_same_v<T, int_type>
        >
    >
    EvalValue(T v)
        : type(AllTypes[std::is_same_v<T, bool> ? Type::t_bool : Type::t_int])
    {
        /* `true`/`false` are the bool type; an int_type is `int`. The bool is
         * stored in `bval` (low byte), the rest of `ival` is zeroed by the
         * union's default ctor, so reading it as bool or as the int 0/1 both
         * work (a bool promotes to int 0/1 via num_bin_op). */
        if constexpr (std::is_same_v<T, bool>)
            val.bval = v;
        else
            val.ival = v;
    }

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
            !std::is_same_v<U, int_type> &&            /* disallow int_type */
            !std::is_same_v<U, bool> &&                /* disallow bool (above) */
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
        static_assert(offsetof(ValueU, bval) == 0);
        static_assert(offsetof(ValueU, structtype) == 0);
        static_assert(offsetof(ValueU, str) == 0);
        static_assert(offsetof(ValueU, func) == 0);
        static_assert(offsetof(ValueU, arr) == 0);
        static_assert(offsetof(ValueU, ex) == 0);
        static_assert(offsetof(ValueU, dict) == 0);
        static_assert(offsetof(ValueU, struct_) == 0);

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

    /* Like get<T>() const, but returns a const reference (no copy / no refcount
     * bump for handle types). Valid as long as this value is. */
    template <class T>
    const T &get_ref() const {

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
        num_bin_op(tmp, rhs, &Type::eq);
        return tmp.is_true();
    }

    bool operator!=(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        num_bin_op(tmp, rhs, &Type::noteq);
        return tmp.is_true();
    }

    bool operator<(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        num_bin_op(tmp, rhs, &Type::lt);
        return tmp.is_true();
    }

    bool operator<=(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        num_bin_op(tmp, rhs, &Type::le);
        return tmp.is_true();
    }

    bool operator>(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        num_bin_op(tmp, rhs, &Type::gt);
        return tmp.is_true();
    }

    bool operator>=(const EvalValue &rhs) const {

        EvalValue tmp = *this;
        num_bin_op(tmp, rhs, &Type::ge);
        return tmp.is_true();
    }

    std::string to_string() const {
        return type->to_string(*this);
    };

    std::string to_string_repr() const {
        return type->to_string_repr(*this);
    };

    std::string pretty(int indent, int width) const {
        return type->pretty(*this, indent, width);
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

inline void
num_bin_op(EvalValue &a, const EvalValue &b, NumBinOp op)
{
    /*
     * bool promotes to int (the bottom of the numeric chain bool <= int <=
     * float), so `true + 1 == 2`, `true < 3`, `true == 1` all behave like the
     * int 0/1. The left operand is promoted in place; for a bool right operand
     * we recurse once with an int copy (b is const). After this, neither
     * operand is bool, so the existing int->float promotion runs.
     */
    if (a.is<bool>())
        a = static_cast<int_type>(a.get<bool>() ? 1 : 0);

    if (b.is<bool>()) {
        const EvalValue bi = static_cast<int_type>(b.get<bool>() ? 1 : 0);
        num_bin_op(a, bi, op);
        return;
    }

    if (a.is<int_type>() && b.is<float_type>())
        a = static_cast<float_type>(a.get<int_type>());

    (a.get_type()->*op)(a, b);
}


// ---------------------------------------------------------------

class LValue final {

    EvalValue val;

public:

    /* Used only by TypeArr::subscript() */
    LValue *container;
    size_type container_idx;

private:
    /*
     * Declared AFTER container_idx so this bool packs into the value's tail
     * padding, instead of sitting between `val` and `container` and forcing the
     * pointer to a fresh 8-byte boundary (7 bytes wasted). Read via
     * is_const_var().
     */
    bool is_const;

    LValue clone();
    EvalValue &get_value_for_put();

    void type_checks() const {
        assert(val.get_type()->t != Type::t_lval);
        assert(val.get_type()->t != Type::t_undefid);
    }

public:

    /*
     * Default: an unbound `none` slot. Needed so Frame (eval.h) can hold an
     * inline array of slots; the value is always `none` here, so type_checks()
     * would trivially pass and is skipped to keep this cheap (runs per slot).
     */
    LValue() : val(), container(nullptr), is_const(false) { }

    LValue(const EvalValue &val, bool is_const)
        : val(val)
        , container(nullptr)
        , is_const(is_const)
    {
        type_checks();
    }

    LValue(EvalValue &&val, bool is_const)
        : val(move(val))
        , container(nullptr)
        , is_const(is_const)
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

/* The VM's native-call entry (CallV): the args are already evaluated into a
 * contiguous run of the CALLER's frame slots `argslots[0..n)`, so bind them
 * directly (no per-call vector allocation - the hot recursion path);
 * `call_site`
 * is the CallExpr's loc, for the backtrace. */
/* The call-site loc for a native VM call's backtrace is AST-free: instead of a
 * Loc, the VM passes its chunk + pc, and do_func_call resolves the caret from
 * the loc side table ON THE ERROR PATH only (no per-call lookup). */
struct Chunk;
EvalValue vm_call_func(EvalContext *ctx,
                       FuncObject &obj,
                       const LValue *argslots,
                       size_t n,
                       const Chunk *ck, size_t pc);

/* The VM's CachedCallV: like vm_call_func but with the per-frame pure-call
 * cache
 * (a CachedCallExpr - the recursion-unroll dedup, e.g. fib). */
EvalValue vm_cached_call(EvalContext *ctx,
                         FuncObject &obj,
                         const LValue *argslots,
                         size_t n,
                         const Chunk *ck, size_t pc);

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const EvalValue &arg);

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const std::pair<EvalValue, EvalValue> &args);
