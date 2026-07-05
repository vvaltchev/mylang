/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "bitops.h"
#include "hashing.h"

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

EvalValue builtin_exit(EvalContext *ctx, ExprList *exprList,
                       const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const Construct *arg = exprList->elems[0].get();
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
EvalValue builtin_type(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    if (dynamic_cast<LiteralObj *>(arg))   /* folded Type object */
        return RValue(arg->eval(ctx));
    return make_runtime_type_value(RValue(arg->eval(ctx)));
}

EvalValue builtin_decltype(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    if (dynamic_cast<LiteralObj *>(arg))   /* folded Type object */
        return RValue(arg->eval(ctx));
    return make_runtime_type_value(RValue(arg->eval(ctx)));
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

    Construct *arg = exprList->elems[0].get();
    if (dynamic_cast<LiteralStr *>(arg))   /* folded by the inferencer */
        return RValue(arg->eval(ctx));
    return SharedStr(reflect_typeof(RValue(arg->eval(ctx))));
}

EvalValue builtin_kindstr(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    if (dynamic_cast<LiteralStr *>(arg))   /* folded by the inferencer */
        return RValue(arg->eval(ctx));
    return TypeNames[RValue(arg->eval(ctx)).get_type()->t];
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

inline auto make_const_builtin(const char *name, float_type val)
{
    return make_pair(UniqueId::get(name), LValue(val, true));
}

inline auto make_builtin(const char *name, decltype(Builtin::func) f)
{
    return make_pair(UniqueId::get(name), LValue(Builtin{f}, false));
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
    return fv(ctx, exprList, heapbuf.data(), n);
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
    return FV(ctx, exprList, stackbuf, n);
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
    /* SELF-EVAL form (append/push/pop/intptr): no pre-evaluated rest - the
     * builtin reads args 1..n off exprList itself (append needs the node). */
    return FLV(ctx, exprList, target, nullptr, 0);
}

/* Cold >8-rest path for the REST-NATIVE lvalue adapter (heap arg buffer). */
static ML_NOINLINE EvalValue
builtin_lv_v_adapter_big(decltype(Builtin::func_lv) flv, EvalContext *ctx,
                         ExprList *exprList, LValue *target, size_t n_rest)
{
    std::vector<EvalValue> heapbuf(n_rest);
    for (size_t i = 0; i < n_rest; i++)
        heapbuf[i] = RValue(exprList->elems[i + 1]->eval(ctx));
    return flv(ctx, exprList, target, heapbuf.data(), n_rest);
}

/*
 * REST-NATIVE lvalue adapter (insert/erase): arg0 -> LValue* target as above,
 * PLUS the value args (1..n) pre-evaluated by VALUE into a buffer, so the
 * builtin has ZERO node->eval. Both engines reach it - the tree-walker as the
 * builtin's `func`, the VM as CallBuiltinLV (which forms the same rest run in
 * registers). arg0 is evaluated first (a pure slot reference), then the rest,
 * matching the old self-eval order.
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
    return FLV(ctx, exprList, target, stackbuf, n_rest);
}

template <decltype(Builtin::func_lv) FLV>
inline auto make_builtin_lv(const char *name)
{
    Builtin b{builtin_lv_adapter<FLV>};   /* func set; the union is zeroed */
    b.func_lv = FLV;                      /* the mutating-form pointer */
    return make_pair(UniqueId::get(name), LValue(b, false));
}

/* Rest-native registration (insert/erase): the value args are pre-evaluated.
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
    make_const_builtin("defined", builtin_defined),
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
    make_const_builtin("sort", builtin_sort),
    make_const_builtin("rev_sort", builtin_rev_sort),
    make_const_builtin("reverse", builtin_reverse),
    make_const_builtin_v<builtin_sum>("sum"),
    make_const_builtin("map", builtin_map),
    make_const_builtin("filter", builtin_filter),

    /* Dictionary builtins */
    make_const_builtin_v<builtin_keys>("keys"),
    make_const_builtin_v<builtin_values>("values"),
    make_const_builtin_v<builtin_kvpairs>("kvpairs"),
    make_const_builtin_v<builtin_dict>("dict"),
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
    make_builtin("isconst", builtin_isconst),
    make_builtin("isconstdecl", builtin_isconstdecl),
    make_builtin_v<builtin_ispure>("ispure"),
    make_builtin_v<builtin_ispuredecl>("ispuredecl"),
    make_builtin_lv<builtin_intptr>("intptr"),
    make_builtin_v<builtin_array_storage>("array_storage"),

    /* Compile-time type queries (folded by the inferencer; the runtime body is
     * a -nti fallback). decltype(var) takes a variable; typestr/kindstr take an
     * expression - the full structural string vs the bare kind. */
    make_builtin("type", builtin_type),
    make_builtin("decltype", builtin_decltype),
    make_builtin("typestr", builtin_typestr),
    make_builtin("kindstr", builtin_kindstr),

    /* Runtime reflection (see builtins/reflect.cpp.h) */
    make_builtin_v<builtin_globals>("globals"),
    make_builtin_v<builtin_signature>("signature"),
    make_builtin_v<builtin_layout>("layout"),
    make_builtin_v<builtin_specializations>("specializations"),
    make_builtin("show", builtin_show),

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

    /* Array builtins */
    make_builtin_lv<builtin_append>("append"),
    make_builtin_lv<builtin_append>("push"),   /* push() aliases append() */
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
