/* SPDX-License-Identifier: BSD-2-Clause */

#include "statictype.h"
#include "uniqueid.h"
#include "defs.h"       /* ML_CHECK */

/*
 * M0 of the type-inference feature: the static-type lattice and its operations.
 * See statictype.h for the data model and plans/type-inference.md sections 2 &
 * 4 for the rules these implement.
 */

/* ---------------- StaticTypeArena: allocation & ground singletons
    --------------- */

StaticTypeArena::StaticTypeArena()
{
    for (int o = 0; o < 2; o++) {
        g_bool[o]  = alloc(StaticTypeKind::Bool);      g_bool[o]->opt  = o;
        g_int[o]   = alloc(StaticTypeKind::Int);       g_int[o]->opt   = o;
        g_float[o] = alloc(StaticTypeKind::Float);     g_float[o]->opt = o;
        g_str[o]   = alloc(StaticTypeKind::Str);       g_str[o]->opt   = o;
        g_exc[o]   = alloc(StaticTypeKind::Exception); g_exc[o]->opt   = o;
    }

    g_none = alloc(StaticTypeKind::None);
    g_dyn[0] = alloc(StaticTypeKind::Dyn);
    g_dyn[1] = alloc(StaticTypeKind::Dyn);  g_dyn[1]->opt = true;
}

StaticTypeRef StaticTypeArena::alloc(StaticTypeKind k)
{
    nodes.push_back(std::make_unique<StaticType>(k));
    return nodes.back().get();
}

StaticTypeRef StaticTypeArena::ground(StaticTypeKind k, bool opt)
{
    const int i = opt ? 1 : 0;

    switch (k) {
        case StaticTypeKind::Bool:      return g_bool[i];
        case StaticTypeKind::Int:       return g_int[i];
        case StaticTypeKind::Float:     return g_float[i];
        case StaticTypeKind::Str:       return g_str[i];
        case StaticTypeKind::Exception: return g_exc[i];
        case StaticTypeKind::Dyn:       return g_dyn[i];   /* dyn / opt dyn */
        default:                 return nullptr;   /* not a cached ground */
    }
}

StaticTypeRef StaticTypeArena::fresh_var()
{
    return alloc(StaticTypeKind::Unknown);
}

StaticTypeRef StaticTypeArena::array_of(StaticTypeRef elem, bool opt)
{
    /* a structural
        type's components are always real StaticTypeRefs (Unknown at worst,
     * never null) - a null would null-deref in resolve/join/equal/to_string */
    ML_CHECK(elem != nullptr);
    StaticTypeRef t = alloc(StaticTypeKind::Array);
    t->elem = elem;
    t->opt = opt;
    return t;
}

StaticTypeRef StaticTypeArena::dict_of(StaticTypeRef key, StaticTypeRef val,
    bool opt)
{
    ML_CHECK(key != nullptr && val != nullptr);
    StaticTypeRef t = alloc(StaticTypeKind::Dict);
    t->key = key;
    t->val = val;
    t->opt = opt;
    return t;
}

StaticTypeRef StaticTypeArena::struct_ty(const void *def, const UniqueId
    *name, bool opt)
{
    StaticTypeRef t = alloc(StaticTypeKind::Struct);
    t->struct_def = def;            /* nominal identity (the StructTypeDef*) */
    t->struct_name = name;
    t->opt = opt;
    return t;
}

StaticTypeRef StaticTypeArena::func_of(std::vector<StaticTypeRef> params,
                         std::vector<bool> param_opt,
                         StaticTypeRef ret,
                         bool opt)
{
    ML_CHECK(ret != nullptr);
    /* one opt flag per param, so a call's nullability check can't index OOB */
    ML_CHECK(param_opt.size() == params.size());
    StaticTypeRef t = alloc(StaticTypeKind::Func);
    t->params = std::move(params);
    t->param_opt = std::move(param_opt);
    t->ret = ret;
    t->opt = opt;
    return t;
}

StaticTypeRef StaticTypeArena::with_opt(StaticTypeRef t, bool optflag)
{
    t = static_type_resolve(t);

    /* none is inherently nullable, and an unbound variable carries its
     * nullability only once it is resolved. dyn, however, CAN carry an opt bit
     * (Phase B: nullability is orthogonal to dyn - `dyn` vs `opt dyn`). */
    if (t->kind == StaticTypeKind::None || t->kind == StaticTypeKind::Unknown)
        return t;

    if (t->opt == optflag)
        return t;

    switch (t->kind) {
        case StaticTypeKind::Bool:
        case StaticTypeKind::Int:
        case StaticTypeKind::Float:
        case StaticTypeKind::Str:
        case StaticTypeKind::Exception:
        case StaticTypeKind::Dyn:
            return ground(t->kind, optflag);
        default:
            break;
    }

    StaticTypeRef c = alloc(t->kind);
    c->opt = optflag;
    c->elem = t->elem;
    c->key = t->key;
    c->val = t->val;
    c->params = t->params;
    c->param_opt = t->param_opt;
    c->ret = t->ret;
    c->struct_def = t->struct_def;
    c->struct_name = t->struct_name;
    return c;
}

/* ----------------------------- resolve ----------------------------------- */

StaticTypeRef static_type_resolve(StaticTypeRef t)
{
    if (!t)
        return t;

    StaticTypeRef r = t;
    while (r->kind == StaticTypeKind::Unknown && r->link)
        r = r->link;

    /* path compression */
    while (t->kind == StaticTypeKind::Unknown && t->link && t->link != r) {
        StaticTypeRef next = t->link;
        t->link = r;
        t = next;
    }

    /* the representative is a proper root: a concrete kind, or an unbound var
     * (no link). If this fails the union-find graph has a cycle. */
    ML_CHECK(r && (r->kind != StaticTypeKind::Unknown || !r->link));
    return r;
}

/* ------------------------------ equality --------------------------------- */

bool static_type_equal(StaticTypeRef a, StaticTypeRef b)
{
    a = static_type_resolve(a);
    b = static_type_resolve(b);

    if (a == b)
        return true;

    if (a->kind != b->kind || a->opt != b->opt)
        return false;

    switch (a->kind) {

        case StaticTypeKind::Unknown:
            return false;                 /* distinct unbound variables */

        case StaticTypeKind::Array:
            return static_type_equal(a->elem, b->elem);

        case StaticTypeKind::Dict:
            return static_type_equal(a->key, b->key) &&
                static_type_equal(a->val, b->val);

        case StaticTypeKind::Func:
            if (a->params.size() != b->params.size())
                return false;
            if (a->param_opt != b->param_opt)
                return false;
            for (size_t i = 0; i < a->params.size(); i++)
                if (!static_type_equal(a->params[i], b->params[i]))
                    return false;
            return static_type_equal(a->ret, b->ret);

        case StaticTypeKind::Struct:
            return a->struct_def == b->struct_def;

        default:
            return true;                  /* grounds with matching kind+opt */
    }
}

/* ------------------------------- unify ----------------------------------- */

static bool static_type_occurs(StaticTypeRef var, StaticTypeRef t)
{
    t = static_type_resolve(t);

    if (t == var)
        return true;

    switch (t->kind) {
        case StaticTypeKind::Array:
            return static_type_occurs(var, t->elem);
        case StaticTypeKind::Dict:
            return static_type_occurs(var, t->key) || static_type_occurs(var,
                t->val);
        case StaticTypeKind::Func:
            for (StaticTypeRef p : t->params)
                if (static_type_occurs(var, p))
                    return true;
            return static_type_occurs(var, t->ret);
        default:
            return false;
    }
}

bool static_type_unify(StaticTypeRef a, StaticTypeRef b)
{
    a = static_type_resolve(a);
    b = static_type_resolve(b);

    if (a == b)
        return true;

    if (a->kind == StaticTypeKind::Unknown) {
        if (static_type_occurs(a, b))
            return false;                 /* infinite type */
        a->link = b;
        return true;
    }

    if (b->kind == StaticTypeKind::Unknown) {
        if (static_type_occurs(b, a))
            return false;
        b->link = a;
        return true;
    }

    /* unify is strict equality: no promotion, opt must match (promotion and
     * nullability widening are the job of assignable()/join(), not unify()). */
    if (a->kind != b->kind || a->opt != b->opt)
        return false;

    switch (a->kind) {

        case StaticTypeKind::Array:
            return static_type_unify(a->elem, b->elem);

        case StaticTypeKind::Dict:
            return static_type_unify(a->key, b->key) &&
                static_type_unify(a->val, b->val);

        case StaticTypeKind::Func:
            if (a->params.size() != b->params.size())
                return false;
            if (a->param_opt != b->param_opt)
                return false;
            for (size_t i = 0; i < a->params.size(); i++)
                if (!static_type_unify(a->params[i], b->params[i]))
                    return false;
            return static_type_unify(a->ret, b->ret);

        case StaticTypeKind::Struct:
            return a->struct_def == b->struct_def;

        default:
            return true;
    }
}

/* ---------------------------- assignable --------------------------------- */

/* Two container component types are compatible for assignment when they are
 * equal, or when either is None (an empty container - the bottom element type)
 * or Dyn (a heterogeneous container). Keeps arrays/dicts practically usable
 * despite invariance: an empty [] fits any array<T>, a dyn array fits any. */
static bool static_type_elem_compat(StaticTypeRef a, StaticTypeRef b)
{
    a = static_type_resolve(a);
    b = static_type_resolve(b);
    if (a->kind == StaticTypeKind::None || b->kind == StaticTypeKind::None ||
        a->kind == StaticTypeKind::Dyn  || b->kind == StaticTypeKind::Dyn)
        return true;
    return static_type_equal(a, b);
}

static bool static_type_same_underlying(StaticTypeRef a, StaticTypeRef b)
{
    if (a->kind != b->kind)
        return false;

    switch (a->kind) {

        case StaticTypeKind::Array:
            return static_type_elem_compat(a->elem, b->elem);

        case StaticTypeKind::Dict:
            return static_type_elem_compat(a->key, b->key) &&
                   static_type_elem_compat(a->val, b->val);

        case StaticTypeKind::Func:
            /* Assignability between function types checks ARITY only. Deep
             * function subtyping over *inferred* signatures is fragile (a
             * callback's own param/return types are themselves inferred and may
             * finalize to dyn after the type that captured them was frozen) and
             * yields false positives on ordinary higher-order code (e.g.
             * apply(sq, i)). Calls through the function are still checked at
             * their own site, and the body type-checks independently. Precise
             * function subtyping is deferred (plans/type-inference.md). */
            return a->params.size() == b->params.size();

        case StaticTypeKind::Struct:
            return a->struct_def == b->struct_def;

        default:
            return true;                  /* Int/Float/Str/Exception */
    }
}

bool static_type_assignable(StaticTypeRef src, StaticTypeRef dst)
{
    src = static_type_resolve(src);
    dst = static_type_resolve(dst);

    if (src == dst)
        return true;

    if (dst->kind == StaticTypeKind::Dyn)
        return true;                      /* anything fits dyn */

    if (src->kind == StaticTypeKind::Dyn)
        return false;                     /* dyn does not implicitly narrow */

    /* An unconstrained variable on either side: defer to constraint solving. */
    if (src->kind == StaticTypeKind::Unknown || dst->kind ==
        StaticTypeKind::Unknown)
        return true;

    if (src->kind == StaticTypeKind::None)
        return dst->opt || dst->kind == StaticTypeKind::None;

    if (src->opt && !dst->opt)
        return false;                     /* maybe-none into a non-null slot */

    if (static_type_same_underlying(src, dst))
        return true;

    /* The numeric promotion chain: bool <= int <= float. */
    if (src->kind == StaticTypeKind::Int && dst->kind == StaticTypeKind::Float)
        return true;
    if (src->kind == StaticTypeKind::Bool &&
        (dst->kind == StaticTypeKind::Int || dst->kind ==
            StaticTypeKind::Float))
        return true;

    return false;
}

/* -------------------------------- join ----------------------------------- */

StaticTypeRef StaticTypeArena::join(StaticTypeRef a, StaticTypeRef b)
{
    a = static_type_resolve(a);
    b = static_type_resolve(b);

    if (static_type_equal(a, b))
        return a;

    if (a->kind == StaticTypeKind::Dyn || b->kind == StaticTypeKind::Dyn)
        /* Nullability is orthogonal to dyn (Phase B): collapsing a mix to dyn
         * keeps the opt bit, so `dyn | none` / `dyn | opt T` is `opt dyn`. */
        return with_opt(g_dyn[0],
                        a->opt || b->opt ||
                        a->kind == StaticTypeKind::None ||
                        b->kind == StaticTypeKind::None);

    if (a->kind == StaticTypeKind::Unknown)
        return b;
    if (b->kind == StaticTypeKind::Unknown)
        return a;

    if (a->kind == StaticTypeKind::None)
        return with_opt(b, true);
    if (b->kind == StaticTypeKind::None)
        return with_opt(a, true);

    const bool anyopt = a->opt || b->opt;

    /* Numeric promotion chain bool < int < float: the join climbs to the
     * higher-ranked numeric kind. join(bool,int)=int, join(int,float)=float,
     * join(bool,bool)=bool (distinct opt reaches here). */
    auto num_rank = [](StaticTypeKind k) -> int {
        switch (k) {
            case StaticTypeKind::Bool:  return 0;
            case StaticTypeKind::Int:   return 1;
            case StaticTypeKind::Float: return 2;
            default:             return -1;
        }
    };

    const int ar = num_rank(a->kind);
    const int br = num_rank(b->kind);

    if (ar >= 0 && br >= 0) {
        const int r = ar > br ? ar : br;
        const StaticTypeKind k = r == 2 ? StaticTypeKind::Float
                        : r == 1 ? StaticTypeKind::Int
                                 : StaticTypeKind::Bool;
        return ground(k, anyopt);
    }

    if (a->kind != b->kind)
        return nullptr;                   /* irreconcilable conflict */

    switch (a->kind) {

        case StaticTypeKind::Str:
        case StaticTypeKind::Exception:
            return ground(a->kind, anyopt);

        case StaticTypeKind::Array: {
            StaticTypeRef ej = join_elem(a->elem, b->elem);
            if (!ej)
                ej = g_dyn[0];               /* D1: mixed elements -> dyn */
            return array_of(ej, anyopt);
        }

        case StaticTypeKind::Dict: {
            StaticTypeRef kj = join_elem(a->key, b->key);
            if (!kj)
                kj = g_dyn[0];
            StaticTypeRef vj = join_elem(a->val, b->val);
            if (!vj)
                vj = g_dyn[0];
            return dict_of(kj, vj, anyopt);
        }

        case StaticTypeKind::Func: {
            /* Same arity: join componentwise (Unknown children fill in, mixed
             * concretes fall to dyn) so a func-valued var assigned the "same"
             * function across fixpoint rounds does not spuriously conflict.
             * Different arity: a real conflict. */
            if (a->params.size() != b->params.size())
                return nullptr;
            std::vector<StaticTypeRef> ps;
            std::vector<bool> popt;
            for (size_t i = 0; i < a->params.size(); i++) {
                StaticTypeRef pj = join(a->params[i], b->params[i]);
                ps.push_back(pj ? pj : g_dyn[0]);
                popt.push_back((i < a->param_opt.size() && a->param_opt[i]) ||
                               (i < b->param_opt.size() && b->param_opt[i]));
            }
            StaticTypeRef rj = join(a->ret, b->ret);
            return func_of(ps, popt, rj ? rj : g_dyn[0], anyopt);
        }

        default:
            return nullptr;
    }
}

StaticTypeRef StaticTypeArena::join_elem(StaticTypeRef a, StaticTypeRef b)
{
    a = static_type_resolve(a);
    b = static_type_resolve(b);
    if (a->kind == StaticTypeKind::None)
        return b;                 /* absorbed: an uninitialized/none slot */
    if (b->kind == StaticTypeKind::None)
        return a;
    return join(a, b);
}

/* ----------------------------- to_string --------------------------------- */

std::string static_type_to_string(StaticTypeRef t)
{
    t = static_type_resolve(t);

    /* Nullability renders as a `?` SUFFIX (Kotlin/Swift style): `int?`, `dyn?`,
     * `array<int>?`, `array<int?>` - the `?` binds to the type it follows, so
     * it composes at every level. */
    const std::string q = (t->opt && t->kind != StaticTypeKind::None) ? "?" :
        "";

    switch (t->kind) {

        case StaticTypeKind::Unknown:   return "?";
        case StaticTypeKind::None:      return "none";
        case StaticTypeKind::Bool:      return "bool" + q;
        case StaticTypeKind::Int:       return "int" + q;
        case StaticTypeKind::Float:     return "float" + q;
        case StaticTypeKind::Str:       return "str" + q;
        case StaticTypeKind::Exception: return "exception" + q;
        case StaticTypeKind::Dyn:       return "dyn" + q;   /* `dyn` / `dyn?` */

        case StaticTypeKind::Array:
            return "array<" + static_type_to_string(t->elem) + ">" + q;

        case StaticTypeKind::Dict:
            return "dict<" + static_type_to_string(t->key) + "," +
                   static_type_to_string(t->val) + ">" + q;

        case StaticTypeKind::Func: {
            std::string r = "func(";
            for (size_t i = 0; i < t->params.size(); i++) {
                if (i)
                    r += ",";
                std::string p = static_type_to_string(t->params[i]);
                /* mark an opt param with `?` (unless the param type already
                 * carries it, to avoid `str??`) */
                if (i < t->param_opt.size() && t->param_opt[i] &&
                    (p.empty() || p.back() != '?'))
                    p += "?";
                r += p;
            }
            r += ")->" + static_type_to_string(t->ret);
            return r + q;
        }

        case StaticTypeKind::Struct:
            return (t->struct_name ? std::string(t->struct_name->val)
                                   : std::string("struct")) + q;
    }

    return q;
}
