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
 * An STy is a node in the inference type graph. It is distinct from the runtime
 * `Type *` (the ops table in type.h): an STy describes what the inferencer
 * knows at COMPILE time about an expression / variable / parameter / function
 * return -- including type variables (Unknown), nullability (opt), and
 * structural shapes (array element, dict key/value, function signature).
 *
 * Nodes are owned by an STyArena (which keeps them at stable addresses); STyRef
 * is a non-owning pointer into that arena. The ground types
 * (int/float/str/exception/none/dyn) are cached singletons per opt-flag;
 * everything else is allocated on demand.
 *
 * This file + stype.cpp are the M0 milestone: the lattice and its operations
 * (resolve / unify / assignable / join / equal / to_string), with no AST wiring
 * yet. The lattice rules (none-nullability, int<=float promotion, mixed
 * containers falling to dyn, type conflicts being errors) are specified in
 * plans/type-inference.md sections 2 and 4.
 */

enum class STyKind {
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

struct STy;
typedef STy *STyRef;

struct STy {
    STyKind kind;
    bool opt = false;          /* nullable: "kind, or none" */

    /* Unknown only: union-find link. Non-null => this var is bound to *link. */
    STyRef link = nullptr;

    STyRef elem = nullptr;             /* Array */
    STyRef key = nullptr;              /* Dict */
    STyRef val = nullptr;              /* Dict */
    std::vector<STyRef> params;        /* Func */
    std::vector<bool> param_opt;       /* Func */
    STyRef ret = nullptr;              /* Func */
    const void *struct_def = nullptr;  /* Struct: the StructTypeDef* identity */
    const UniqueId *struct_name = nullptr;  /* Struct: name (for to_string) */

    explicit STy(STyKind k) : kind(k) { }
};

class STyArena {

public:

    STyArena();

    /* Ground singletons (cached per opt-flag). */
    STyRef bool_ty(bool opt = false)  { return ground(STyKind::Bool, opt); }
    STyRef int_ty(bool opt = false)   { return ground(STyKind::Int, opt); }
    STyRef float_ty(bool opt = false) { return ground(STyKind::Float, opt); }
    STyRef str_ty(bool opt = false)   { return ground(STyKind::Str, opt); }
    STyRef exc_ty(bool opt = false) { return ground(STyKind::Exception, opt); }
    STyRef none_ty()                  { return g_none; }
    STyRef dyn_ty()                   { return g_dyn[0]; }

    /* Constructors for compound / variable types (always freshly allocated). */
    STyRef fresh_var();
    STyRef array_of(STyRef elem, bool opt = false);
    STyRef dict_of(STyRef key, STyRef val, bool opt = false);
    STyRef struct_ty(const void *def, const UniqueId *name, bool opt = false);
    STyRef func_of(std::vector<STyRef> params,
                   std::vector<bool> param_opt,
                   STyRef ret,
                   bool opt = false);

    /* `t` with its nullability set to `optflag` (cached for grounds). */
    STyRef with_opt(STyRef t, bool optflag);

    /* Least upper bound; nullptr on an irreconcilable type conflict. */
    STyRef join(STyRef a, STyRef b);

private:

    std::vector<std::unique_ptr<STy>> nodes;
    STyRef g_bool[2];
    STyRef g_int[2];
    STyRef g_float[2];
    STyRef g_str[2];
    STyRef g_exc[2];
    STyRef g_none;
    STyRef g_dyn[2];   /* [0] = dyn (non-null), [1] = opt dyn (Phase B) */

    STyRef alloc(STyKind k);
    STyRef ground(STyKind k, bool opt);

    /* join for a *container element* (array elem / dict value): treats None as
     * bottom (absorbed, not nullable) so array(N)-then-fill or a default-none
     * slot does not make the element type `opt`. */
    STyRef join_elem(STyRef a, STyRef b);
};

/* Free functions: pure (no allocation) given the nodes they are handed. */

/* Follow union-find links to the representative (with path compression). */
STyRef sty_resolve(STyRef t);

/* Equality constraint: binds type variables; false on an occurs-check failure
 * or a structural mismatch. */
bool sty_unify(STyRef a, STyRef b);

/* Can a value of static type `src` be stored where `dst` is expected?
 * (the lattice's subtyping: none->opt, T->opt T, int->float, anything->dyn). */
bool sty_assignable(STyRef src, STyRef dst);

/* Deep structural equality (including the opt flag). */
bool sty_equal(STyRef a, STyRef b);

/* Human-readable form for error messages (e.g. "opt array<int>"). */
std::string sty_to_string(STyRef t);
