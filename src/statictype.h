/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstddef>

class UniqueId;

/*
 * Static types for MyLang's type-inference pass (see plans/type-inference.md).
 *
 * An StaticType is a node in the inference type graph. It is distinct from
     the runtime
 * `Type *` (the ops table in type.h): an StaticType describes what the
     inferencer
 * knows at COMPILE time about an expression / variable / parameter / function
 * return -- including type variables (Unknown), nullability (opt), and
 * structural shapes (array element, dict key/value, function signature).
 *
 * Nodes are owned by an StaticTypeArena (which keeps them at stable
     addresses); StaticTypeRef
 * is a non-owning pointer into that arena. The ground types
 * (int/float/str/exception/none/dyn) are cached singletons per opt-flag;
 * everything else is allocated on demand.
 *
 * This file + statictype.cpp are the M0 milestone: the lattice and its
 * operations (resolve / unify / assignable / join / equal / to_string), with no
 * AST wiring yet. The lattice rules (none-nullability, int<=float promotion,
 * mixed containers falling to dyn, type conflicts being errors) are specified
 * in plans/type-inference.md sections 2 and 4.
 */

enum class StaticTypeKind {
    Unknown,      /* a fresh type variable (union-find); no type yet */
    None,         /* the only-none / not-yet-pinned unit type */
    Bool,         /* bool <= int <= float promotion chain */
    Int,
    Float,
    Str,
    Array,        /* uses `elem` */
    Dict,         /* uses `key`, `val` */
    Func,         /* uses `params`, `param_opt`, `ret` */
    Exception,
    Struct,       /* uses `struct_def` (future; see plans/structs.md) */
    Dyn,          /* explicit dynamic top */
};

struct StaticType;
typedef StaticType *StaticTypeRef;

struct StaticType {
    StaticTypeKind kind;
    bool opt = false;          /* nullable: "kind, or none" */

    /* Unknown only: union-find link. Non-null => this var is bound to *link. */
    StaticTypeRef link = nullptr;

    StaticTypeRef elem = nullptr;             /* Array */
    StaticTypeRef key = nullptr;              /* Dict */
    StaticTypeRef val = nullptr;              /* Dict */
    std::vector<StaticTypeRef> params;        /* Func */
    std::vector<bool> param_opt;       /* Func */
    StaticTypeRef ret = nullptr;              /* Func */
    const void *struct_def = nullptr;  /* Struct: the StructTypeDef* identity */
    const UniqueId *struct_name = nullptr;  /* Struct: name (for to_string) */

    explicit StaticType(StaticTypeKind k) : kind(k) { }
};

class StaticTypeArena {

public:

    StaticTypeArena();

    /* Ground singletons (cached per opt-flag). */
    StaticTypeRef bool_ty(bool opt = false)  { return
        ground(StaticTypeKind::Bool, opt); }
    StaticTypeRef int_ty(bool opt = false)   { return
        ground(StaticTypeKind::Int, opt); }
    StaticTypeRef float_ty(bool opt = false) { return
        ground(StaticTypeKind::Float, opt); }
    StaticTypeRef str_ty(bool opt = false)   { return
        ground(StaticTypeKind::Str, opt); }
    StaticTypeRef exc_ty(bool opt = false) { return
        ground(StaticTypeKind::Exception, opt); }
    StaticTypeRef none_ty()                  { return g_none; }
    StaticTypeRef dyn_ty()                   { return g_dyn[0]; }

    /* Constructors for compound / variable types (always freshly allocated). */
    StaticTypeRef fresh_var();
    StaticTypeRef array_of(StaticTypeRef elem, bool opt = false);
    StaticTypeRef dict_of(StaticTypeRef key, StaticTypeRef val, bool opt =
        false);
    StaticTypeRef struct_ty(const void *def, const UniqueId *name, bool opt =
        false);
    StaticTypeRef func_of(std::vector<StaticTypeRef> params,
                   std::vector<bool> param_opt,
                   StaticTypeRef ret,
                   bool opt = false);

    /* `t` with its nullability set to `optflag` (cached for grounds). */
    StaticTypeRef with_opt(StaticTypeRef t, bool optflag);

    /* Least upper bound; nullptr on an irreconcilable type conflict. */
    StaticTypeRef join(StaticTypeRef a, StaticTypeRef b);

private:

    std::vector<std::unique_ptr<StaticType>> nodes;
    StaticTypeRef g_bool[2];
    StaticTypeRef g_int[2];
    StaticTypeRef g_float[2];
    StaticTypeRef g_str[2];
    StaticTypeRef g_exc[2];
    StaticTypeRef g_none;
    StaticTypeRef g_dyn[2];   /* [0] = dyn (non-null), [1] = opt dyn (Phase B)
        */

    StaticTypeRef alloc(StaticTypeKind k);
    StaticTypeRef ground(StaticTypeKind k, bool opt);

    /* join for a *container element* (array elem / dict value): treats None as
     * bottom (absorbed, not nullable) so array(N)-then-fill or a default-none
     * slot does not make the element type `opt`. */
    StaticTypeRef join_elem(StaticTypeRef a, StaticTypeRef b);
};

/* Free functions: pure (no allocation) given the nodes they are handed. */

/* Follow union-find links to the representative (with path compression). */
StaticTypeRef static_type_resolve(StaticTypeRef t);

/* Equality constraint: binds type variables; false on an occurs-check failure
 * or a structural mismatch. */
bool static_type_unify(StaticTypeRef a, StaticTypeRef b);

/* Can a value of static type `src` be stored where `dst` is expected?
 * (the lattice's subtyping: none->opt, T->opt T, int->float, anything->dyn). */
bool static_type_assignable(StaticTypeRef src, StaticTypeRef dst);

/* Deep structural equality (including the opt flag). */
bool static_type_equal(StaticTypeRef a, StaticTypeRef b);

/* Human-readable form for error messages (e.g. "opt array<int>"). */
std::string static_type_to_string(StaticTypeRef t);
