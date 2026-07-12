/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "bitops.h"
#include "hashing.h"
#include "env.h"      /* env_get - shared by builtins/io.cpp.h (tmpdir) */

#include <unordered_map>
#include <vector>
#include "evaltypes.cpp.h"
#include "types/int.cpp.h"
#include "types/bool.cpp.h"
#include "types/str.cpp.h"
#include "types/func.cpp.h"
#include "types/arr.cpp.h"
#include "types/exception.cpp.h"
#include "types/float.cpp.h"
#include "types/dict.cpp.h"
#include "types/struct.cpp.h"

/* Build the AST-free ArgLocs a func_v/func_lv takes, from an ExprList. Defined
 * below (after the type tables), forward-declared here so a builtin's custom
 * tree-walker `func` (append_tw / sort_arr / reverse_arr) can build one to hand
 * to the shared, now-ArgLocs core. */
static inline ArgLocs build_arglocs(ExprList *exprList, ArgLoc *locbuf, size_t n);

#include "builtins/str.cpp.h"
#include "builtins/io.cpp.h"
#include "builtins/num.cpp.h"
#include "builtins/arr.cpp.h"
#include "builtins/dict.cpp.h"
#include "builtins/generic.cpp.h"
#include "builtins/reflect.cpp.h"

#include <cmath>
#include <limits>

static const std::array<SharedStr, Type::t_count> TypeNames =
{
    string("none"),
    string(),
    string(),
    string("int"),
    string("builtin"),
    string("float"),
    string("bool"),
    string("struct_type"),
    string("str"),
    string("func"),
    string("array"),
    string("exception"),
    string("dict"),
    string("struct"),
};

EvalValue builtin_exit(EvalContext *ctx, const ArgLocs *exprList,
                       const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const ArgLoc *arg = exprList->arg(0);
    const EvalValue &e = args[0];

    if (!e.is<int_type>())
        throw TypeErrorEx("Expected integer", arg->start, arg->end);

    exit(static_cast<int>(e.get<int_type>()));
}

/*
 * Build a flat (non-recursive) `Type` reflection object from a runtime VALUE -
 * the -nti fallback for type()/decltype() (no inference, so no static type to
 * bake; the normal path folds a recursive Type at compile time instead).
 */
static EvalValue make_runtime_type_value(const EvalValue &v)
{
    StructTypeDef *td = const_cast<StructTypeDef *>(native_struct_type_def());
    auto obj = make_intrusive<StructObject>(td);
    obj->fields.emplace_back(EvalValue(TypeNames[v.get_type()->t]), false);
    obj->fields.emplace_back(EvalValue(SharedStr(reflect_typeof(v))), false);
    obj->fields.emplace_back(EvalValue(false), false);   /* nullable */
    obj->fields.emplace_back(EvalValue(), false);         /* elem (none) */
    obj->fields.emplace_back(EvalValue(), false);         /* key  (none) */
    obj->fields.emplace_back(EvalValue(), false);         /* val  (none) */
    return EvalValue(intrusive_ptr<StructObject>(obj));
}

/*
 * type(x) / decltype(v): a `Type` reflection OBJECT (native composite) of x's
 * runtime / v's static type - `.kind`, `.name`, `.nullable`, and the recursive
 * `.elem` / `.key` / `.val`. The inferencer PRE-GENERATES the object at compile
 * time (a baked const LiteralObj) and these hand it back; the body below runs
 * only under -nti, building a flat Type from the runtime value.
 */
/*
 * NOTE: a FOLDED type query never reaches these `func` bodies - both engines
 * elide it (return the baked args[0]): the VM at codegen, the tree-walker in
 * CallExpr/DirectBuiltinCallExpr::do_eval (keyed on CallExpr::tq_folded). So
 * these run ONLY for a NON-folded query (a `dyn`/Unknown-typed arg, or -nti),
 * where they build the type from the runtime VALUE - identical to the func_v
 * forms. (A node-based `dynamic_cast<Literal...>` "is it folded?" check would be
 * WRONG here: a user's own `typestr("hi")` arg is also a literal, so under -nti
 * it must still report "str", not "hi".)
 */
EvalValue builtin_type(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    return make_runtime_type_value(RValue(exprList->elems[0]->eval(ctx)));
}

EvalValue builtin_decltype(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    return make_runtime_type_value(RValue(exprList->elems[0]->eval(ctx)));
}

/*
 * typestr(x) / kindstr(x): the static type of x as a string - the full
 * structural form ("array<int>", "Point", "int?") and the bare kind ("array",
 * "int", "struct"). Compile-time type QUERIES with an UNEVALUATED operand: the
 * inferencer folds the call to a string literal of x's static type (the arg is
 * never evaluated). These bodies run only under -nti (no inference), where they
 * fall back to the value's runtime type. (Defined here, after TypeNames, so the
 * kindstr fallback can use it; reflect_typeof comes from reflect.cpp.h above.)
 */
EvalValue builtin_typestr(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    return SharedStr(reflect_typeof(RValue(exprList->elems[0]->eval(ctx))));
}

EvalValue builtin_kindstr(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    return TypeNames[RValue(exprList->elems[0]->eval(ctx)).get_type()->t];
}

/*
 * Value-ABI (func_v) forms of the four type queries, for the VM's CallBuiltinV.
 * The VM only reaches these for a NON-FOLDED query (the FOLDED case is elided at
 * codegen to a plain constant - the baked literal - so it never becomes a call
 * there). So these always build from the runtime VALUE - the exact NON-folded
 * branch of the func bodies above (make_runtime_type_value / reflect_typeof /
 * TypeNames), keeping the two engines byte-identical on the differential. The
 * ExprList is passed for arity/locs only; args[0] is the pre-evaluated value.
 */
EvalValue builtin_type_v(EvalContext *, const ArgLocs *el, const EvalValue *args,
                         size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(el->start, el->end);
    return make_runtime_type_value(args[0]);
}
EvalValue builtin_decltype_v(EvalContext *, const ArgLocs *el, const EvalValue *args,
                             size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(el->start, el->end);
    return make_runtime_type_value(args[0]);
}
EvalValue builtin_typestr_v(EvalContext *, const ArgLocs *el, const EvalValue *args,
                            size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(el->start, el->end);
    return SharedStr(reflect_typeof(args[0]));
}
EvalValue builtin_kindstr_v(EvalContext *, const ArgLocs *el, const EvalValue *args,
                            size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(el->start, el->end);
    return TypeNames[args[0].get_type()->t];
}

const std::array<Type *, Type::t_count> AllTypes =
{
    /* Trivial types */
    new TypeNone(),
    new Type(Type::t_lval),       /* internal type: not visible from outside */
    new Type(Type::t_undefid),    /* internal type: not visible from outside */
    new TypeInt(),
    new TypeBuiltin(),
    new TypeFloat(),
    new TypeBool(),
    new TypeStructType(),

    /* Non-trivial types */
    new TypeStr(),
    new TypeFunc(),
    new TypeArr(),
    new TypeException(),
    new TypeDict(),
    new TypeStruct(),
};


/*
 * NOTE[1]: these definitions *MUST FOLLOW* the definition of `AllTypes`
 * simply because the creation of LValue's contents does a lookup
 * in AllTypes.
 *
 * NOTE[2]: (( ... )) used to initialize empty_*_actual to avoid syntactic
 * ambiguity with function declaration.
 */

static const SharedStr empty_str_actual((string()));
static const SharedArrayObj empty_arr_actual((std::vector<LValue>()));

const EvalValue empty_str(SharedStr(empty_str_actual, 0, 0));
const EvalValue empty_arr(SharedArrayObj(empty_arr_actual, 0, 0));
const EvalValue none;

std::set<UniqueId, UniqueId::Comparator> UniqueId::unique_set;

inline auto make_const_builtin(const char *name, decltype(Builtin::func) f)
{
    return make_pair(UniqueId::get(name), LValue(Builtin{f}, true));
}

/*
 * A const, read-only-foldable builtin whose arg0 is an LVALUE (sort/reverse):
 * `f` is the tree-walker / const-eval form (eval's arg0 itself), `flv` the VM's
 * CallBuiltinLV form (handed arg0's slot LValue*). Both share a core so the two
 * engines behave identically; it stays const so a const-array sort still folds
 * at parse time. Unlike make_builtin_lv, `func` is NOT the generic adapter (it
 * would pass a null target for a non-lvalue arg0, which sort/reverse accept).
 */
inline auto make_const_builtin_lv(const char *name, decltype(Builtin::func) f,
                                  decltype(Builtin::func_lv) flv)
{
    Builtin b{f};        /* the custom func (value-or-lvalue arg0) */
    b.func_lv = flv;     /* the VM's lvalue form */
    return make_pair(UniqueId::get(name), LValue(b, true));
}

inline auto make_const_builtin(const char *name, float_type val)
{
    return make_pair(UniqueId::get(name), LValue(val, true));
}

inline auto make_builtin(const char *name, decltype(Builtin::func) f)
{
    return make_pair(UniqueId::get(name), LValue(Builtin{f}, false));
}

/*
 * The DEV-ONLY builtin category (see eval.h): registers a runtime builtin like
 * make_builtin AND records its interned name in g_dev_builtin_ids - the single
 * source of truth for is_dev_builtin(). Such a builtin inherently needs the AST
 * (e.g. show() decompiles it), so it is reserved to the dev harnesses (REPL /
 * tests); a script call to it is a compile-time error (inferencer). Defined
 * BEFORE the `builtins` map so g_dev_builtin_ids is initialized first (its
 * static-init runs top-down in this TU) when make_dev_builtin populates it.
 */
static std::set<const UniqueId *> g_dev_builtin_ids;
inline auto make_dev_builtin(const char *name, decltype(Builtin::func) f)
{
    const UniqueId *uid = UniqueId::get(name);
    g_dev_builtin_ids.insert(uid);
    return make_pair(uid, LValue(Builtin{f}, false));
}
bool is_dev_builtin(const UniqueId *uid)
{
    return g_dev_builtin_ids.count(uid) != 0;
}
bool g_dev_builtins_allowed = false;

/*
 * The LAZY-ARG builtin category (see eval.h): a builtin whose argument is
 * NOT evaluated - a NODE property (`defined`'s UndefinedId probe,
 * `isconst`/`isconstdecl`'s `is_const` flag) that a runtime VALUE can never
 * reproduce, so an INDIRECT call through a dyn value cannot be honored
 * AST-free. The inferencer (reject_dev_builtins) makes a script VALUE-use of
 * such a name a compile error - the callee position stays the one legal use.
 * (The REPL keeps the AST, so it allows the indirect form - the `show`
 * precedent; gated by the same g_dev_builtins_allowed.) NOTE the type
 * queries (type/decltype/typestr/kindstr) are NOT lazy - they are dual-ABI
 * (func_v builds from the runtime value), so they stay usable as values.
 */
static std::set<const UniqueId *> g_lazy_builtin_ids;
template <typename P>
static inline P mark_lazy_builtin(P p)
{
    g_lazy_builtin_ids.insert(p.first);
    return p;
}
bool is_lazy_builtin(const UniqueId *uid)
{
    return g_lazy_builtin_ids.count(uid) != 0;
}

/*
 * A builtin with a CUSTOM tree-walker `func` AND a value-ABI `func_v` (for the
 * VM's CallBuiltinV) - the two are DIFFERENT implementations, unlike
 * make_builtin_v (where `func` is a generic adapter over `func_v`). Used by the
 * type queries: the `func` distinguishes a folded arg (return it) from a runtime
 * one (build from it) via the NODE, while `func_v` always builds from the value
 * (the VM only reaches it non-folded - the folded case is elided at codegen).
 */
inline auto make_builtin_customv(const char *name, decltype(Builtin::func) f,
                                 decltype(Builtin::func_v) fv)
{
    Builtin b{f};
    b.func_v = fv;
    return make_pair(UniqueId::get(name), LValue(b, false));
}

/*
 * Build the AST-FREE ArgLocs a func_v now takes, from the tree-walker's real
 * ExprList: the whole-args caret + the array-repr hint, and each arg's caret
 * into `locbuf` (caller-provided, [0, n)). The VM builds the same struct from a
 * serializable Chunk pool - so a builtin's carets are identical in both engines
 * with no AST pointer in the value-ABI signature.
 */
static inline ArgLocs
build_arglocs(ExprList *exprList, ArgLoc *locbuf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        locbuf[i].start = exprList->elems[i]->start;
        locbuf[i].end = exprList->elems[i]->end;
    }
    ArgLocs al;
    al.start = exprList->start;
    al.end = exprList->end;
    al.args = locbuf;
    al.nargs = n;
    al.arr_hint = exprList->arr_hint;
    return al;
}

/*
 * Cold n>8 path for the adapter below: heap-allocate the arg buffer.
 * ML_NOINLINE and NON-template (fv is a runtime arg) so the hot adapter body
 * carries no std::vector ctor/dtor - that overhead, paid on every
 * migrated-builtin call, measurably taxes the tree-walker.
 */
static ML_NOINLINE EvalValue
builtin_v_adapter_big(decltype(Builtin::func_v) fv, EvalContext *ctx,
                      ExprList *exprList)
{
    const size_t n = exprList->elems.size();
    std::vector<EvalValue> heapbuf(n);
    for (size_t i = 0; i < n; i++)
        heapbuf[i] = RValue(exprList->elems[i]->eval(ctx));
    std::vector<ArgLoc> locbuf(n);
    ArgLocs al = build_arglocs(exprList, locbuf.data(), n);
    return fv(ctx, &al, heapbuf.data(), n);
}

/*
 * The generic tree-walker adapter for a VALUE-ABI builtin (Builtin::func_v):
 * evaluate the unevaluated args into a buffer, then call the value form. So a
 * migrated builtin has ONE implementation, reached by the tree-walker through
 * this adapter and by the VM (CallBuiltinV) directly with pre-evaluated args.
 * A stack buffer covers the common small-arity call (NO vector on the path);
 * only a big (>8-arg) variadic call heap-allocates, via the cold helper above.
 */
template <decltype(Builtin::func_v) FV>
EvalValue builtin_v_adapter(EvalContext *ctx, ExprList *exprList)
{
    const size_t n = exprList->elems.size();
    if (n > 8)
        return builtin_v_adapter_big(FV, ctx, exprList);
    EvalValue stackbuf[8];
    for (size_t i = 0; i < n; i++)
        stackbuf[i] = RValue(exprList->elems[i]->eval(ctx));
    ArgLoc locbuf[8];
    ArgLocs al = build_arglocs(exprList, locbuf, n);
    return FV(ctx, &al, stackbuf, n);
}

template <decltype(Builtin::func_v) FV>
inline auto make_const_builtin_v(const char *name)
{
    return make_pair(UniqueId::get(name),
                     LValue(Builtin{builtin_v_adapter<FV>, FV}, true));
}

template <decltype(Builtin::func_v) FV>
inline auto make_builtin_v(const char *name)
{
    return make_pair(UniqueId::get(name),
                     LValue(Builtin{builtin_v_adapter<FV>, FV}, false));
}

/*
 * The tree-walker adapter for a MUTATING (lvalue-arg0) builtin: evaluate arg0
 * to an lvalue (NOT RValue - keep the LValue* the builtin needs to mutate the
 * caller's storage) and hand it over; the builtin self-evaluates its remaining
 * args. Registered as `func`, so the tree-walker + const-eval reach the same
 * impl as the VM's CallBuiltinLV. A non-lvalue arg0 (or none) passes a null
 * target, and the builtin throws NotLValueEx / the arity error, matching the
 * pre-migration behavior byte-for-byte.
 */
template <decltype(Builtin::func_lv) FLV>
EvalValue builtin_lv_adapter(EvalContext *ctx, ExprList *exprList)
{
    LValue *target = nullptr;
    if (!exprList->elems.empty()) {
        EvalValue a0 = exprList->elems[0]->eval(ctx);
        if (a0.is<LValue *>())
            target = a0.get<LValue *>();
    }
    /* NO-VALUE-ARG form (pop/intptr - both 1-arg): no rest; the builtin uses
     * arg0 (target) + the ArgLocs carets/arity only (no self-eval of a node). */
    const size_t n = exprList->elems.size();
    ArgLoc locbuf[4];
    ArgLocs al = build_arglocs(exprList, locbuf, n < 4 ? n : 4);
    return FLV(ctx, &al, target, nullptr, 0);
}

/* Cold >8-rest path for the REST-NATIVE lvalue adapter (heap arg buffer). */
static ML_NOINLINE EvalValue
builtin_lv_v_adapter_big(decltype(Builtin::func_lv) flv, EvalContext *ctx,
                         ExprList *exprList, LValue *target, size_t n_rest)
{
    std::vector<EvalValue> heapbuf(n_rest);
    for (size_t i = 0; i < n_rest; i++)
        heapbuf[i] = RValue(exprList->elems[i + 1]->eval(ctx));
    std::vector<ArgLoc> locbuf(exprList->elems.size());
    ArgLocs al = build_arglocs(exprList, locbuf.data(), locbuf.size());
    return flv(ctx, &al, target, heapbuf.data(), n_rest);
}

/*
 * REST-NATIVE lvalue adapter (insert/erase): arg0 -> LValue* target as above,
 * PLUS the `rest` args - the TAIL ARGS BY VALUE (args 1..n, everything after
 * the arg0 lvalue) - pre-evaluated by VALUE into a buffer, so the builtin has
 * ZERO node->eval. Both engines reach it - the tree-walker as the builtin's
 * `func`, the VM as CallBuiltinLV (which forms the same rest run in registers).
 * arg0 is evaluated first (a pure slot reference), then the rest, matching the
 * old self-eval order.
 */
template <decltype(Builtin::func_lv) FLV>
EvalValue builtin_lv_v_adapter(EvalContext *ctx, ExprList *exprList)
{
    LValue *target = nullptr;
    if (!exprList->elems.empty()) {
        EvalValue a0 = exprList->elems[0]->eval(ctx);
        if (a0.is<LValue *>())
            target = a0.get<LValue *>();
    }
    const size_t total = exprList->elems.size();
    const size_t n_rest = total ? total - 1 : 0;
    if (n_rest > 8)
        return builtin_lv_v_adapter_big(FLV, ctx, exprList, target, n_rest);
    EvalValue stackbuf[8];
    for (size_t i = 0; i < n_rest; i++)
        stackbuf[i] = RValue(exprList->elems[i + 1]->eval(ctx));
    ArgLoc locbuf[8];
    ArgLocs al = build_arglocs(exprList, locbuf, total < 8 ? total : 8);
    return FLV(ctx, &al, target, stackbuf, n_rest);
}

template <decltype(Builtin::func_lv) FLV>
inline auto make_builtin_lv(const char *name)
{
    Builtin b{builtin_lv_adapter<FLV>};   /* func set; the union is zeroed */
    b.func_lv = FLV;                      /* the mutating-form pointer */
    return make_pair(UniqueId::get(name), LValue(b, false));
}

/* Custom-func mutating registration (append/push): NON-const analogue of
 * make_const_builtin_lv. `f` is a CUSTOM tree-walker `func` (not the generic
 * adapter) - append/push need it so construct-in-place (which needs the ctor
 * NODE) lives there, letting `flv` be REST-NATIVE (value from `rest`, no node).
 * Both engines share the append logic: the tree-walker via `f`, the VM via the
 * rest-native `flv` (CallBuiltinLV). */
inline auto make_builtin_lv_custom(const char *name, decltype(Builtin::func) f,
                                   decltype(Builtin::func_lv) flv)
{
    Builtin b{f};
    b.func_lv = flv;
    return make_pair(UniqueId::get(name), LValue(b, false));
}

/* Rest-native registration (insert/erase): the `rest` args - the TAIL ARGS BY
 * VALUE (args 1..n, everything after the arg0 lvalue) - are pre-evaluated.
 * DirectBuiltinCallExpr::lvalue_rest_native must be set for these so the VM's
 * CallBuiltinLV forms the rest run too (see resolver.cpp). */
template <decltype(Builtin::func_lv) FLV>
inline auto make_builtin_lv_v(const char *name)
{
    Builtin b{builtin_lv_v_adapter<FLV>};
    b.func_lv = FLV;
    return make_pair(UniqueId::get(name), LValue(b, false));
}

const EvalContext::SymbolsType EvalContext::const_builtins =
{
    /* Generic builtins */
    mark_lazy_builtin(make_const_builtin("defined", builtin_defined)),
    make_const_builtin_v<builtin_len>("len"),
    make_const_builtin_v<builtin_str>("str"),
    make_const_builtin_v<builtin_int>("int"),
    make_const_builtin_v<builtin_float>("float"),
    make_const_builtin_v<builtin_clone>("clone"),
    make_const_builtin_v<builtin_hash>("hash"),

    /* Array or container builtins */
    make_const_builtin_v<builtin_make_array>("make_array"),
    make_const_builtin_v<builtin_top>("top"),
    make_const_builtin_v<builtin_range>("range"),
    make_const_builtin_v<builtin_find>("find"),
    make_const_builtin_lv("sort", builtin_sort, builtin_sort_lv),
    make_const_builtin_lv("rev_sort", builtin_rev_sort, builtin_rev_sort_lv),
    make_const_builtin_lv("reverse", builtin_reverse, builtin_reverse_lv),
    make_const_builtin_v<builtin_sum>("sum"),
    make_const_builtin("map", builtin_map),
    make_const_builtin("filter", builtin_filter),

    /* Dictionary builtins */
    make_const_builtin_v<builtin_keys>("keys"),
    make_const_builtin_v<builtin_values>("values"),
    make_const_builtin_v<builtin_kvpairs>("kvpairs"),
    make_const_builtin_v<builtin_dict>("dict"),
    make_const_builtin_v<builtin_make_dict>("make_dict"),
    make_const_builtin_v<builtin_get>("get"),         /* dict lookup -> opt V */
    make_const_builtin_v<builtin_get_throw>("get!"),  /* lookup or throw -> V */

    /* String builtins */
    make_const_builtin_v<builtin_split>("split"),
    make_const_builtin_v<builtin_join>("join"),
    make_const_builtin_v<builtin_ord>("ord"),
    make_const_builtin_v<builtin_chr>("chr"),
    make_const_builtin_v<builtin_splitlines>("splitlines"),
    make_const_builtin_v<builtin_lpad>("lpad"),
    make_const_builtin_v<builtin_rpad>("rpad"),
    make_const_builtin_v<builtin_lstrip>("lstrip"),
    make_const_builtin_v<builtin_rstrip>("rstrip"),
    make_const_builtin_v<builtin_strip>("strip"),
    make_const_builtin_v<builtin_startswith>("startswith"),
    make_const_builtin_v<builtin_endswith>("endswith"),

    /* Numeric builtins */
    make_const_builtin_v<builtin_abs>("abs"),
    make_const_builtin_v<builtin_min>("min"),
    make_const_builtin_v<builtin_max>("max"),
    make_const_builtin_v<builtin_exp>("exp"),
    make_const_builtin_v<builtin_exp2>("exp2"),
    make_const_builtin_v<builtin_log>("log"),
    make_const_builtin_v<builtin_log2>("log2"),
    make_const_builtin_v<builtin_log10>("log10"),
    make_const_builtin_v<builtin_sqrt>("sqrt"),
    make_const_builtin_v<builtin_cbrt>("cbrt"),
    make_const_builtin_v<builtin_pow>("pow"),
    make_const_builtin_v<builtin_sin>("sin"),
    make_const_builtin_v<builtin_cos>("cos"),
    make_const_builtin_v<builtin_tan>("tan"),
    make_const_builtin_v<builtin_asin>("asin"),
    make_const_builtin_v<builtin_acos>("acos"),
    make_const_builtin_v<builtin_atan>("atan"),
    make_const_builtin_v<builtin_ceil>("ceil"),
    make_const_builtin_v<builtin_floor>("floor"),
    make_const_builtin_v<builtin_trunc>("trunc"),
    make_const_builtin_v<builtin_isinf>("isinf"),
    make_const_builtin_v<builtin_isfinite>("isfinite"),
    make_const_builtin_v<builtin_isnormal>("isnormal"),
    make_const_builtin_v<builtin_isnan>("isnan"),
    make_const_builtin_v<builtin_round>("round"),

    /* Numeric constants */
    make_const_builtin("math_e", M_E), /* e */
    make_const_builtin("math_log2e", M_LOG2E), /* log_2 e */
    make_const_builtin("math_log10e", M_LOG10E), /* log_10 e */
    make_const_builtin("math_ln2", M_LN2), /* log_e 2 */
    make_const_builtin("math_ln10", M_LN10), /* log_e 10 */
    make_const_builtin("math_pi", M_PI), /* pi */
    make_const_builtin("math_pi2", M_PI_2), /* pi/2 */
    make_const_builtin("math_pi4", M_PI_4), /* pi/4 */
    make_const_builtin("math_1_pi", M_1_PI), /* 1/pi */
    make_const_builtin("math_2_pi", M_2_PI), /* 2/pi */
    make_const_builtin("math_2_sqrt_pi", M_2_SQRTPI), /* 2/sqrt(pi) */
    make_const_builtin("math_sqrt2", M_SQRT2), /* sqrt(2) */
    make_const_builtin("math_1_sqrt2", M_SQRT1_2), /* 1/sqrt(2) */
    make_const_builtin("nan", NAN), /* Not a Number */
    make_const_builtin("inf", INFINITY), /* Infinity */
    make_const_builtin("eps", std::numeric_limits<float_type>::epsilon()),
};

EvalContext::SymbolsType EvalContext::builtins =
{
    /* Misc builtins */
    make_builtin_v<builtin_assert>("assert"),
    make_builtin_v<builtin_exit>("exit"),
    make_builtin_v<builtin_runtime>("runtime"), /* optimization barrier */
    mark_lazy_builtin(make_builtin("isconst", builtin_isconst)),
    mark_lazy_builtin(make_builtin("isconstdecl", builtin_isconstdecl)),
    make_builtin_v<builtin_ispure>("ispure"),
    make_builtin_v<builtin_ispuredecl>("ispuredecl"),
    make_builtin_lv<builtin_intptr>("intptr"),
    make_builtin_v<builtin_array_storage>("array_storage"),

    /* Compile-time type queries (folded by the inferencer; the runtime body is
     * a -nti fallback). decltype(var) takes a variable; typestr/kindstr take an
     * expression - the full structural string vs the bare kind. */
    make_builtin_customv("type", builtin_type, builtin_type_v),
    make_builtin_customv("decltype", builtin_decltype, builtin_decltype_v),
    make_builtin_customv("typestr", builtin_typestr, builtin_typestr_v),
    make_builtin_customv("kindstr", builtin_kindstr, builtin_kindstr_v),

    /* Runtime reflection (see builtins/reflect.cpp.h) */
    make_builtin_v<builtin_globals>("globals"),
    make_builtin_v<builtin_signature>("signature"),
    make_builtin_v<builtin_layout>("layout"),
    make_builtin_v<builtin_specializations>("specializations"),
    /* show() is a DEV-ONLY builtin (it decompiles the AST): available in the
     * REPL / tests, a compile-time error in a script (use :show at the REPL). */
    make_dev_builtin("show", builtin_show),

    /* Diagnostic tracing (see trace.h) */
    make_builtin_v<builtin_trace>("trace"),
    make_builtin_v<builtin_traceoff>("traceoff"),
    make_builtin_v<builtin_tracing>("tracing"),
    /* dynarray(a): manual promotion - a fresh general (polymorphic) copy of an
     * array. Non-const (fresh mutable value). See builtin_dynarray. */
    make_builtin_v<builtin_dynarray>("dynarray"),
    /* array() is non-const: it never folds to a baked literal, so array(N) is
     * always a runtime call (uniform const/runtime type-driven fill) and a huge
     * array(1000000) isn't baked into the AST. */
    make_builtin_v<builtin_array>("array"),

    /* Array builtins. append/push: a CUSTOM tree-walker func (append_tw, for
     * construct-in-place) + the rest-native func_lv core (builtin_append). */
    make_builtin_lv_custom("append", append_tw, builtin_append),
    make_builtin_lv_custom("push", append_tw, builtin_append),
    make_builtin_lv<builtin_pop>("pop"),

    /* Generic-container builtins */
    make_builtin_lv_v<builtin_erase>("erase"),
    make_builtin_lv_v<builtin_insert>("insert"),
    make_builtin_v<builtin_deepclone>("deepclone"), /* deep mutable copy */

    /* Numeric builtins */
    make_builtin_v<builtin_rand>("rand"),
    make_builtin_v<builtin_randf>("randf"),

    /* I/O builtins */
    make_builtin_v<builtin_print>("print"),
    make_builtin_v<builtin_readln>("readln"),
    make_builtin_v<builtin_writeln>("writeln"),
    make_builtin_v<builtin_read>("read"),
    make_builtin_v<builtin_write>("write"),
    make_builtin_v<builtin_readlines>("readlines"),
    make_builtin_v<builtin_writelines>("writelines"),
    make_builtin_v<builtin_remove>("remove"),
    make_builtin_v<builtin_tmpdir>("tmpdir"),
};

/*
 * The program-wide builtin table (see eval.h): a flat vector of every builtin's
 * value for O(1) SymKind::builtin access, plus a name->index map for the
 * resolver. Built once, lazily, on first lookup - by which time both builtin
 * maps are fully static-initialized. const_builtins are added first so that on
 * the (non-existent today) chance a name is in both, the const one wins -
 * matching the runtime root map, whose std::map::insert keeps the const entry.
 * Every entry is forced is_const, so an `aBuiltin = x` assignment to an
 * unshadowed builtin raises CannotRebindBuiltinEx and the shared table can't be
 * mutated (it outlives any one program, e.g. across -rt tests).
 */
static std::vector<LValue> g_builtin_slots;
/* Per slot: did this builtin come from const_builtins? A const-eval context
 * (AutoConst / the inliner's refold) must see ONLY const builtins - mirroring
 * the const EvalContext, which loads only const_builtins - so a runtime-builtin
 * call there stays unfoldable (those passes rely on the resulting "undefined"
 * to keep it a runtime call). See Identifier::do_eval. */
static std::vector<char> g_builtin_is_const;
static std::unordered_map<const UniqueId *, int> g_builtin_index;

static void
build_builtin_table_once()
{
    if (!g_builtin_index.empty())
        return;

    const auto add = [](const UniqueId *uid, const LValue &lv, bool is_const) {
        if (g_builtin_index.count(uid))
            return;
        g_builtin_index[uid] = static_cast<int>(g_builtin_slots.size());
        g_builtin_slots.emplace_back(lv.get(), /*is_const=*/true);
        g_builtin_is_const.push_back(is_const ? 1 : 0);
    };

    for (const auto &kv : EvalContext::const_builtins)
        add(kv.first, kv.second, /*is_const=*/true);

    for (const auto &kv : EvalContext::builtins)
        add(kv.first, kv.second, /*is_const=*/false);
}

int
builtin_slot_index(const UniqueId *uid)
{
    build_builtin_table_once();
    const auto it = g_builtin_index.find(uid);
    return it != g_builtin_index.end() ? it->second : -1;
}

LValue &
builtin_slot(int index)
{
    return g_builtin_slots[index];
}

bool
builtin_is_const(int index)
{
    return g_builtin_is_const[index] != 0;
}
