/* SPDX-License-Identifier: BSD-2-Clause */

#include "stype.h"
#include "uniqueid.h"

/*
 * M0 of the type-inference feature: the static-type lattice and its operations.
 * See stype.h for the data model and plans/type-inference.md sections 2 & 4 for
 * the rules these implement.
 */

/* ---------------- STyArena: allocation & ground singletons --------------- */

STyArena::STyArena()
{
    for (int o = 0; o < 2; o++) {
        g_bool[o]  = alloc(STyKind::Bool);      g_bool[o]->opt  = o;
        g_int[o]   = alloc(STyKind::Int);       g_int[o]->opt   = o;
        g_float[o] = alloc(STyKind::Float);     g_float[o]->opt = o;
        g_str[o]   = alloc(STyKind::Str);       g_str[o]->opt   = o;
        g_exc[o]   = alloc(STyKind::Exception); g_exc[o]->opt   = o;
    }

    g_none = alloc(STyKind::None);
    g_dyn[0] = alloc(STyKind::Dyn);
    g_dyn[1] = alloc(STyKind::Dyn);  g_dyn[1]->opt = true;
}

STyRef STyArena::alloc(STyKind k)
{
    nodes.push_back(std::make_unique<STy>(k));
    return nodes.back().get();
}

STyRef STyArena::ground(STyKind k, bool opt)
{
    const int i = opt ? 1 : 0;

    switch (k) {
        case STyKind::Bool:      return g_bool[i];
        case STyKind::Int:       return g_int[i];
        case STyKind::Float:     return g_float[i];
        case STyKind::Str:       return g_str[i];
        case STyKind::Exception: return g_exc[i];
        case STyKind::Dyn:       return g_dyn[i];   /* dyn / opt dyn */
        default:                 return nullptr;   /* not a cached ground */
    }
}

STyRef STyArena::fresh_var()
{
    return alloc(STyKind::Unknown);
}

STyRef STyArena::array_of(STyRef elem, bool opt)
{
    STyRef t = alloc(STyKind::Array);
    t->elem = elem;
    t->opt = opt;
    return t;
}

STyRef STyArena::dict_of(STyRef key, STyRef val, bool opt)
{
    STyRef t = alloc(STyKind::Dict);
    t->key = key;
    t->val = val;
    t->opt = opt;
    return t;
}

STyRef STyArena::struct_ty(const void *def, const UniqueId *name, bool opt)
{
    STyRef t = alloc(STyKind::Struct);
    t->struct_def = def;            /* nominal identity (the StructTypeDef*) */
    t->struct_name = name;
    t->opt = opt;
    return t;
}

STyRef STyArena::func_of(std::vector<STyRef> params,
                         std::vector<bool> param_opt,
                         STyRef ret,
                         bool opt)
{
    STyRef t = alloc(STyKind::Func);
    t->params = std::move(params);
    t->param_opt = std::move(param_opt);
    t->ret = ret;
    t->opt = opt;
    return t;
}

STyRef STyArena::with_opt(STyRef t, bool optflag)
{
    t = sty_resolve(t);

    /* none is inherently nullable, and an unbound variable carries its
     * nullability only once it is resolved. dyn, however, CAN carry an opt bit
     * (Phase B: nullability is orthogonal to dyn - `dyn` vs `opt dyn`). */
    if (t->kind == STyKind::None || t->kind == STyKind::Unknown)
        return t;

    if (t->opt == optflag)
        return t;

    switch (t->kind) {
        case STyKind::Bool:
        case STyKind::Int:
        case STyKind::Float:
        case STyKind::Str:
        case STyKind::Exception:
        case STyKind::Dyn:
            return ground(t->kind, optflag);
        default:
            break;
    }

    STyRef c = alloc(t->kind);
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

STyRef sty_resolve(STyRef t)
{
    if (!t)
        return t;

    STyRef r = t;
    while (r->kind == STyKind::Unknown && r->link)
        r = r->link;

    /* path compression */
    while (t->kind == STyKind::Unknown && t->link && t->link != r) {
        STyRef next = t->link;
        t->link = r;
        t = next;
    }

    return r;
}

/* ------------------------------ equality --------------------------------- */

bool sty_equal(STyRef a, STyRef b)
{
    a = sty_resolve(a);
    b = sty_resolve(b);

    if (a == b)
        return true;

    if (a->kind != b->kind || a->opt != b->opt)
        return false;

    switch (a->kind) {

        case STyKind::Unknown:
            return false;                 /* distinct unbound variables */

        case STyKind::Array:
            return sty_equal(a->elem, b->elem);

        case STyKind::Dict:
            return sty_equal(a->key, b->key) && sty_equal(a->val, b->val);

        case STyKind::Func:
            if (a->params.size() != b->params.size())
                return false;
            if (a->param_opt != b->param_opt)
                return false;
            for (size_t i = 0; i < a->params.size(); i++)
                if (!sty_equal(a->params[i], b->params[i]))
                    return false;
            return sty_equal(a->ret, b->ret);

        case STyKind::Struct:
            return a->struct_def == b->struct_def;

        default:
            return true;                  /* grounds with matching kind+opt */
    }
}

/* ------------------------------- unify ----------------------------------- */

static bool sty_occurs(STyRef var, STyRef t)
{
    t = sty_resolve(t);

    if (t == var)
        return true;

    switch (t->kind) {
        case STyKind::Array:
            return sty_occurs(var, t->elem);
        case STyKind::Dict:
            return sty_occurs(var, t->key) || sty_occurs(var, t->val);
        case STyKind::Func:
            for (STyRef p : t->params)
                if (sty_occurs(var, p))
                    return true;
            return sty_occurs(var, t->ret);
        default:
            return false;
    }
}

bool sty_unify(STyRef a, STyRef b)
{
    a = sty_resolve(a);
    b = sty_resolve(b);

    if (a == b)
        return true;

    if (a->kind == STyKind::Unknown) {
        if (sty_occurs(a, b))
            return false;                 /* infinite type */
        a->link = b;
        return true;
    }

    if (b->kind == STyKind::Unknown) {
        if (sty_occurs(b, a))
            return false;
        b->link = a;
        return true;
    }

    /* unify is strict equality: no promotion, opt must match (promotion and
     * nullability widening are the job of assignable()/join(), not unify()). */
    if (a->kind != b->kind || a->opt != b->opt)
        return false;

    switch (a->kind) {

        case STyKind::Array:
            return sty_unify(a->elem, b->elem);

        case STyKind::Dict:
            return sty_unify(a->key, b->key) && sty_unify(a->val, b->val);

        case STyKind::Func:
            if (a->params.size() != b->params.size())
                return false;
            if (a->param_opt != b->param_opt)
                return false;
            for (size_t i = 0; i < a->params.size(); i++)
                if (!sty_unify(a->params[i], b->params[i]))
                    return false;
            return sty_unify(a->ret, b->ret);

        case STyKind::Struct:
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
static bool sty_elem_compat(STyRef a, STyRef b)
{
    a = sty_resolve(a);
    b = sty_resolve(b);
    if (a->kind == STyKind::None || b->kind == STyKind::None ||
        a->kind == STyKind::Dyn  || b->kind == STyKind::Dyn)
        return true;
    return sty_equal(a, b);
}

static bool sty_same_underlying(STyRef a, STyRef b)
{
    if (a->kind != b->kind)
        return false;

    switch (a->kind) {

        case STyKind::Array:
            return sty_elem_compat(a->elem, b->elem);

        case STyKind::Dict:
            return sty_elem_compat(a->key, b->key) &&
                   sty_elem_compat(a->val, b->val);

        case STyKind::Func:
            /* Assignability between function types checks ARITY only. Deep
             * function subtyping over *inferred* signatures is fragile (a
             * callback's own param/return types are themselves inferred and may
             * finalize to dyn after the type that captured them was frozen) and
             * yields false positives on ordinary higher-order code (e.g.
             * apply(sq, i)). Calls through the function are still checked at
             * their own site, and the body type-checks independently. Precise
             * function subtyping is deferred (plans/type-inference.md). */
            return a->params.size() == b->params.size();

        case STyKind::Struct:
            return a->struct_def == b->struct_def;

        default:
            return true;                  /* Int/Float/Str/Exception */
    }
}

bool sty_assignable(STyRef src, STyRef dst)
{
    src = sty_resolve(src);
    dst = sty_resolve(dst);

    if (src == dst)
        return true;

    if (dst->kind == STyKind::Dyn)
        return true;                      /* anything fits dyn */

    if (src->kind == STyKind::Dyn)
        return false;                     /* dyn does not implicitly narrow */

    /* An unconstrained variable on either side: defer to constraint solving. */
    if (src->kind == STyKind::Unknown || dst->kind == STyKind::Unknown)
        return true;

    if (src->kind == STyKind::None)
        return dst->opt || dst->kind == STyKind::None;

    if (src->opt && !dst->opt)
        return false;                     /* maybe-none into a non-null slot */

    if (sty_same_underlying(src, dst))
        return true;

    /* The numeric promotion chain: bool <= int <= float. */
    if (src->kind == STyKind::Int && dst->kind == STyKind::Float)
        return true;
    if (src->kind == STyKind::Bool &&
        (dst->kind == STyKind::Int || dst->kind == STyKind::Float))
        return true;

    return false;
}

/* -------------------------------- join ----------------------------------- */

STyRef STyArena::join(STyRef a, STyRef b)
{
    a = sty_resolve(a);
    b = sty_resolve(b);

    if (sty_equal(a, b))
        return a;

    if (a->kind == STyKind::Dyn || b->kind == STyKind::Dyn)
        /* Nullability is orthogonal to dyn (Phase B): collapsing a mix to dyn
         * keeps the opt bit, so `dyn | none` / `dyn | opt T` is `opt dyn`. */
        return with_opt(g_dyn[0],
                        a->opt || b->opt ||
                        a->kind == STyKind::None ||
                        b->kind == STyKind::None);

    if (a->kind == STyKind::Unknown)
        return b;
    if (b->kind == STyKind::Unknown)
        return a;

    if (a->kind == STyKind::None)
        return with_opt(b, true);
    if (b->kind == STyKind::None)
        return with_opt(a, true);

    const bool anyopt = a->opt || b->opt;

    /* Numeric promotion chain bool < int < float: the join climbs to the
     * higher-ranked numeric kind. join(bool,int)=int, join(int,float)=float,
     * join(bool,bool)=bool (distinct opt reaches here). */
    auto num_rank = [](STyKind k) -> int {
        switch (k) {
            case STyKind::Bool:  return 0;
            case STyKind::Int:   return 1;
            case STyKind::Float: return 2;
            default:             return -1;
        }
    };

    const int ar = num_rank(a->kind);
    const int br = num_rank(b->kind);

    if (ar >= 0 && br >= 0) {
        const int r = ar > br ? ar : br;
        const STyKind k = r == 2 ? STyKind::Float
                        : r == 1 ? STyKind::Int
                                 : STyKind::Bool;
        return ground(k, anyopt);
    }

    if (a->kind != b->kind)
        return nullptr;                   /* irreconcilable conflict */

    switch (a->kind) {

        case STyKind::Str:
        case STyKind::Exception:
            return ground(a->kind, anyopt);

        case STyKind::Array: {
            STyRef ej = join_elem(a->elem, b->elem);
            if (!ej)
                ej = g_dyn[0];               /* D1: mixed elements -> dyn */
            return array_of(ej, anyopt);
        }

        case STyKind::Dict: {
            STyRef kj = join_elem(a->key, b->key);
            if (!kj)
                kj = g_dyn[0];
            STyRef vj = join_elem(a->val, b->val);
            if (!vj)
                vj = g_dyn[0];
            return dict_of(kj, vj, anyopt);
        }

        case STyKind::Func: {
            /* Same arity: join componentwise (Unknown children fill in, mixed
             * concretes fall to dyn) so a func-valued var assigned the "same"
             * function across fixpoint rounds does not spuriously conflict.
             * Different arity: a real conflict. */
            if (a->params.size() != b->params.size())
                return nullptr;
            std::vector<STyRef> ps;
            std::vector<bool> popt;
            for (size_t i = 0; i < a->params.size(); i++) {
                STyRef pj = join(a->params[i], b->params[i]);
                ps.push_back(pj ? pj : g_dyn[0]);
                popt.push_back((i < a->param_opt.size() && a->param_opt[i]) ||
                               (i < b->param_opt.size() && b->param_opt[i]));
            }
            STyRef rj = join(a->ret, b->ret);
            return func_of(ps, popt, rj ? rj : g_dyn[0], anyopt);
        }

        default:
            return nullptr;
    }
}

STyRef STyArena::join_elem(STyRef a, STyRef b)
{
    a = sty_resolve(a);
    b = sty_resolve(b);
    if (a->kind == STyKind::None)
        return b;                 /* absorbed: an uninitialized/none slot */
    if (b->kind == STyKind::None)
        return a;
    return join(a, b);
}

/* ----------------------------- to_string --------------------------------- */

std::string sty_to_string(STyRef t)
{
    t = sty_resolve(t);

    std::string s;
    if (t->opt && t->kind != STyKind::None)
        s = "opt ";

    switch (t->kind) {

        case STyKind::Unknown:   return s + "?";
        case STyKind::None:      return "none";
        case STyKind::Bool:      return s + "bool";
        case STyKind::Int:       return s + "int";
        case STyKind::Float:     return s + "float";
        case STyKind::Str:       return s + "str";
        case STyKind::Exception: return s + "exception";
        case STyKind::Dyn:       return s + "dyn";   /* `dyn` / `opt dyn` */

        case STyKind::Array:
            return s + "array<" + sty_to_string(t->elem) + ">";

        case STyKind::Dict:
            return s + "dict<" + sty_to_string(t->key) + "," +
                   sty_to_string(t->val) + ">";

        case STyKind::Func: {
            std::string r = s + "func(";
            for (size_t i = 0; i < t->params.size(); i++) {
                if (i)
                    r += ",";
                if (i < t->param_opt.size() && t->param_opt[i])
                    r += "opt ";
                r += sty_to_string(t->params[i]);
            }
            r += ")->" + sty_to_string(t->ret);
            return r;
        }

        case STyKind::Struct:
            return s + (t->struct_name ? std::string(t->struct_name->val)
                                       : "struct");
    }

    return s;
}
