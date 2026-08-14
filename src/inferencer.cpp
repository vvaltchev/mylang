/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"
#include "statictype.h"
#include "errors.h"
#include "inferencer.h"
#include "analyzer.h"
#include "evalvalue.h"
#include "eval.h"
#include "trace.h"
#include "resolver.h"     /* for_each_child_slot (fold_show_calls) */
#include "coderender.h"   /* render_func_code / render_construct_code */

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <algorithm>
#include <ostream>
#include <cstdio>

/*
 * Whole-program static type inference + checking. See plans/archived/type-inference.md
 * and plans/archived/type-inference-questions.md for the design and decisions.
 *
 * Pipeline (Inferencer::run):
 *   1. structural  - build scopes; one TypeSym per declaration; resolve every
 *                    Identifier use to its TypeSym; one FuncInfo per function.
 *   2. fixpoint    - Jacobi iteration: each round recomputes every var/param/
 *                    return type into `acc` (join of all contributions) while
 *                    reading the previous round's stable `type`; commit acc->
 *                    type; repeat until stable. Reading stable values keeps the
 *                    round order-independent; kinds only climb the lattice,
 *                    so a join conflict (e.g. int vs str) is a real, stable
 *                    error and is raised immediately.
 *   3. check       - with final types, validate every operation/call/assign/
 *                    return and throw on a violation.
 */

/* intern_msg (stable const char* for a terminal compile error's message) is
 * shared from errors.h - the named-arg desugaring throws the same way. */

namespace {

struct FuncInfo;

struct TypeSym {
    const UniqueId *name = nullptr;
    StaticTypeRef type = nullptr;     /* stable type read by type_of() */
    StaticTypeRef acc = nullptr;      /* accumulator built during a round */
    bool dyn_decl = false;
    bool opt_decl = false;
    /* Explicit type annotation (`int x`, `func f(str s)`, `array a`); `none`
     * for an inferred `var`/`const`/param. A scalar annotation pins the type
     * (with assignability checked); `arr`/`dict` constrain only the kind. */
    DeclType ann = DeclType::none;
    /* When ann == DeclType::strct, the struct type the var is pinned to. */
    const StructTypeDef *ann_struct = nullptr;
    /* For a PARAMETERIZED container annotation (`array<int>`, `dict<str,P>`):
     * the recursive element/key/value type. When set, the symbol is pinned to
     * the full type (kind AND element types), exactly like a scalar pin. */
    std::shared_ptr<TypeAnnot> ann_annot;
    bool is_param = false;
    bool const_decl = false;   /* declared `const` (vs `var`) */
    bool is_loopvar = false;   /* a foreach loop variable (type is derived) */
    /*
     * dyn-into-concrete COERCION. `int OP dyn` is `dyn` (see binop_result), so a
     * `var s = 0; s = s + x` (x dyn) contributes `dyn` to `s`. Rather than widen
     * `s` to dyn (which would demand `var dyn s`), a `dyn` value is treated as
     * ASSIGNABLE to a concrete NUMERIC local - a runtime-checked coercion: `s`
     * keeps the type of its non-dyn contributions and the store coerces the dyn
     * value to it (error if not that type). `round_got_dyn` records a dyn
     * contribution THIS round (order-independent - the numeric-vs-dyn decision is
     * made at commit, not at the contribution); `coerces_dyn` (sticky) is the
     * final "this concrete var receives a dyn -> the runtime must coerce" — the
     * inferencer stamps `decl_id->decl_type` so `coerce_to_decl_type` fires.
     */
    bool round_got_dyn = false;
    bool coerces_dyn = false;
    Identifier *decl_id = nullptr;   /* the decl Identifier (for decl_type stamp) */
    /* Structural-pass bookkeeping for safe var-bound-lambda monomorphization:
     * how many times the name is written (decl + assigns), and whether it is
     * ever referenced in a non-callee (value) position. A write-once,
     * never-value-used var bound to a non-capturing lambda is safe to treat as
     * a template (calls redirect to typed clones; nothing else holds it). */
    int writes = 0;
    bool value_used = false;
    /* A param that received a possibly-none (opt/none) argument at some call
     * site (set in the check pass). With strict_dyn on, a non-opt non-dyn param
     * with this set is a compile error demanding `opt` (enforce_nonnull_params,
     * the param analogue of the mandatory-`dyn` rule). */
    bool received_optish = false;
    /*
     * REPL incremental inference only: a symbol committed by a PRIOR input is
     * "pinned" - its `type` is fixed (a later input may read it or assign to
     * it, but cannot change its type), and the fixpoint must read it without
     * resetting/recomputing it. Always false in the one-shot (whole-program)
     * path, so every "skip pinned" branch is a no-op there.
     */
    bool pinned = false;
    /* Declared inside an un-instantiated TEMPLATE's body (a param or local of a
     * function the fixpoint skips). Its finalized type is a dyn/none fallback,
     * not real inference, so `:trace infer` reports the template as a whole
     * rather than these. Set during the structural pass. */
    bool in_template = false;
    Loc decl_loc;
    FuncInfo *func = nullptr;  /* non-null when this name is a function */
    /* non-null when this name is a struct TYPE (a `struct` decl): the symbol is
     * a type descriptor, callable (construction) + `.CONST`-accessible. */
    const StructTypeDef *struct_type = nullptr;
};

struct FuncInfo {
    FuncDeclStmt *decl = nullptr;
    std::vector<TypeSym *> params;
    StaticTypeRef ret = nullptr;        /* stable return type */
    StaticTypeRef ret_acc = nullptr;    /* return accumulator (current round) */
    bool falls_through = false;
    bool pinned = false;         /* committed by a prior REPL input */
    /*
     * REPL: set when a call INSIDE a function body is redirected to this
     * template/spec instance - i.e. the instance has a live consumer (another
     * function), so it must NOT be garbage-collected when its base function is
     * redefined. An instance reached only from a top-level throwaway
     * expression (`f(1,2)` at the prompt) stays false and is GC'd on redefine.
     */
    bool has_func_consumer = false;
    /*
     * A TEMPLATE: at least one parameter is un-annotated and not explicitly
     * `dyn`, so its type is not fixed by the declaration. Such a function is
     * not type-checked in isolation; it is instantiated per call-site signature
     * as a typed clone (see plans/archived/function-templates.md). A `dyn` param is not
     * a template parameter - it is the explicit "one instantiation, any type".
     */
    bool is_template = false;

    /*
     * Value-template tracking (plans/archived/value-template-instantiation.md): the
     * template's VALUE escaped precise tracking (a join dropped its finfo
     * set, or it was passed as a call/builtin ARGUMENT or captured) - an
     * untracked call site could then reach a typed instance with a
     * mismatched signature, so an escaped template is never
     * value-instantiated (its base keeps running, as before).
     */
    bool value_escaped = false;
};

struct Scope {
    std::unordered_map<const UniqueId *, TypeSym *> syms;
    Scope *parent = nullptr;
};

class Inferencer {

public:

    explicit Inferencer(Construct *root = nullptr) : root(root) { }
    void run();
    void stamp_proven_params();     /* C3: see the definition */
    void setup();                  /* once: bottom + global scope */
    void infer_input(Block *root); /* REPL: one input, commit+pin its globals */
    void undef_global(const UniqueId *name);  /* REPL undef(x) */
    void dump_debug_ti(std::ostream &os);   /* --debug-ti */
    void collect_arrays(AnalysisInfo &out); /* -a: array storage colors */

    /* Complete child-visitor (no `this`, hence static) - also used by the
     * for-range specialization in the static specialize() path. */
    static void for_each_child(Construct *n,
                               const std::function<void(Construct *)> &fn);

    /* REPL :globals/:type - the inferred static type string of a committed
     * global (or "" if it is not a committed inferred symbol). */
    std::string global_type_str(const UniqueId *name);

    /* REPL :show - the inferred type of each of `fn`'s parameters, and `fn`'s
     * inferred return type (for a concrete function / template instance); empty
     * for an un-instantiated template (unbound) or an unknown function. */
    std::vector<std::string> func_param_types(const FuncDeclStmt *fn);
    std::string func_return_type(const FuncDeclStmt *fn);
    /* REPL instance GC: does this template/spec instance have a function
     * consumer (a redirected call inside a function body)? */
    bool instance_has_consumer(const FuncDeclStmt *fn);

    bool strict_dyn = false;   /* enforce the mandatory-`dyn` rule */
    bool strict_deep = false;  /* Phase B: dyn anywhere (incl. array<dyn>) */
    /* When false (the CLI's -nti), run() still does the structural pass and the
     * named-argument lowering (both required for correctness), but skips the
     * fixpoint / checking / strict enforcement. */
    bool checks_enabled = true;

private:

    StaticTypeArena A;
    Construct *root;
    StaticTypeRef bottom = nullptr;   /* shared Unknown identity (join) */

    std::vector<std::unique_ptr<TypeSym>> all_syms;
    std::vector<std::unique_ptr<FuncInfo>> all_funcs;
    std::vector<std::unique_ptr<Scope>> all_scopes;

    std::unordered_map<const Construct *, TypeSym *> id_sym;
    std::unordered_map<const Construct *, FuncInfo *> func_of_decl;
    /* struct types by name (for resolving a struct-typed field/annotation). */
    std::unordered_map<const UniqueId *, const StructTypeDef *> struct_by_name;

    Scope *global = nullptr;
    bool changed = false;
    FuncInfo *cur_func = nullptr;
    /* >0 while the structural pass is inside a template function's body, so
     * new_sym tags that function's params/locals `in_template` (their finalized
     * types are fallbacks - see TypeSym::in_template). */
    int struct_tmpl_depth = 0;
    /* Set while walk_struct'ing a freshly-built instantiation CLONE: its body
     * has the template's (un-annotated) params, so declare_funcdecl marks it a
     * template momentarily (cleared right after) - but the clone's syms ARE the
     * concrete instance, so they must NOT be tagged in_template. */
    bool struct_in_clone = false;

    /* Monomorphization: instantiations cached by (template, signature) so the
     * same call signature shares ONE clone; a monotonic counter names them.
     * The cache is SESSION-persistent (not cleared per REPL input): a signature
     * already instantiated by a prior input reuses that instance instead of
     * building a duplicate. The value is the instance's interned NAME (a stable
     * identity - never a raw node pointer); the instance is reached across
     * inputs by that name in the global scope (its FuncInfo is pinned). The key
     * `template_sig_key` is stable too (the template's arena-stable FuncInfo
     * pointer + the signature's type strings). */
    std::unordered_map<std::string, const UniqueId *> tmpl_cache;
    int tmpl_clone_counter = 0;       /* lambda-template fallback names */
    /* per-base-name monotonic counter for instance names `<base>$<n>` (kept for
     * the whole session so a re-declared template can't reuse a prior name) */
    std::unordered_map<const UniqueId *, int> clone_name_counter;
    /* Per-template instantiation count + a per-template "already warned" guard:
     * past MAX_TMPL_INSTANCES distinct signatures, further calls run
     * dynamically (no clone) - a backstop for runaway polymorphism (D4). */
    std::unordered_map<FuncInfo *, int> tmpl_inst_count;
    /* Value-instantiated clones' uniform signatures - seeded into the
     * clone's params each fixpoint round (no direct call feeds them). */
    std::map<FuncInfo *, std::vector<StaticTypeRef>> value_inst_sigs;
    /* Templates already value-instantiated (their uses redirected). */
    std::set<FuncInfo *> value_inst_done;
    std::unordered_set<FuncInfo *> tmpl_cap_warned;

    /* Flow-sensitive null narrowing (check pass only): inside the proven branch
     * of `if (x != none)` / `if (x)`, x reads as non-opt. */
    std::unordered_map<const TypeSym *, StaticTypeRef> narrowed;
    bool narrowing_on = false;

    void infer_one(Block *root);     /* the per-root passes (shared) */

    /* helpers */
    Scope *new_scope(Scope *parent);
    TypeSym *new_sym(const UniqueId *name, Scope *s, Loc loc);
    static TypeSym *lookup(Scope *s, const UniqueId *name);
    FuncInfo *callee_funcinfo(Construct *e);   /* named func or inline lambda */
    static bool is_builtin(const UniqueId *name);
    static bool always_exits(const Construct *n);

    /* structural pass */
    void hoist_globals(Block *root);
    void walk_struct(Construct *n, Scope *s);
    void declare_funcdecl(FuncDeclStmt *fd, Scope *s);
    void declare_structdecl(StructDeclStmt *sd, Scope *s);
    /* a struct field's static type */
    StaticTypeRef field_static_type(const FieldDef &fd);
    /* a parsed TypeAnnot -> StaticType */
    StaticTypeRef annot_to_static_type(const TypeAnnot *ta);
    void declare_target(Construct *lvalue, Scope *s, bool is_const);
    void enforce_decl_types();       /* explicit-type annotations (array/dict) */
    void enforce_concrete_decls();   /* the mandatory-`dyn` rule */
    void lower_named_args(Construct *n);        /* desugar named-arg calls */
    void lower_call_named_args(CallExpr *call); /* ...for one call */
    void reject_dev_builtins(Construct *n);     /* DEV-only builtins in a script */
    void check_isbound_args(Construct *n);     /* isbound()'s arg SHAPE */
    void enforce_nonnull_params();   /* the mandatory-`opt` rule for params */
    static bool type_has_dyn(StaticTypeRef t, bool deep);
    static bool type_has_bottom_elem(StaticTypeRef t);

    /* type computation (reads stable `type`) */
    StaticTypeRef type_of(const Construct *e);
    StaticTypeRef func_static_type(FuncInfo *fi);
    StaticTypeRef binop_result(Op op, StaticTypeRef a, StaticTypeRef b);
    StaticTypeRef unary_result(Op op, StaticTypeRef a);
    StaticTypeRef builtin_result(const UniqueId *name, ExprList *args);
    StaticTypeRef static_type_from_value(const EvalValue &v);
    static Op compound_binop(Op op);

    /* fixpoint */
    void reset_round();
    void commit_round();
    void accumulate(Construct *n);
    void contribute(TypeSym *s, StaticTypeRef t, Loc loc);
    void contribute_arg(TypeSym *param, StaticTypeRef argT, Loc loc);
    void contribute_ret(StaticTypeRef t);
    void accumulate_assign(Expr14 *e);
    void contribute_to_lvalue(Construct *lv, StaticTypeRef ct, Loc loc);
    void accumulate_call(CallExpr *call);
    void accumulate_foreach(ForeachStmt *fe);
    void spread_idlist(IdList *idl, Construct *rvalue);

    /* monomorphization (templates - see plans/archived/function-templates.md) */
    void mark_lambda_templates();   /* safe var-bound lambdas -> templates */
    void run_fixpoint(Block *root);             /* the Jacobi loop, extracted */
    bool instantiate_round(Block *root);     /* clone + redirect; progress? */
    bool value_instantiate_round(Block *root);  /* value-used templates (see
                                                 * plans/value-template-
                                                 * instantiation.md) */
    void drain_escapes();       /* arena escape ledger -> value_escaped */
    FuncDeclStmt *make_template_clone(FuncInfo *tmpl, const std::string &key,
                                      Block *root);
    std::string template_sig_key(FuncInfo *tmpl,
                                  const std::vector<StaticTypeRef> &sig);
    void collect_calls(Construct *n,
                       std::vector<std::pair<CallExpr *, bool>> &out,
                       bool in_func = false);

    /* check pass */
    void check(Construct *n);
    void check_if(IfStmt *i);
    TypeSym *narrow_target(Construct *cond, bool &in_then);
    void annotate_hints(Construct *n);   /* stamp TypeHints for specializer */
    void set_array_repr_hint(Expr14 *e);    /* type-driven ArrHint on rvalue */
    void check_call(CallExpr *call);
    void check_struct_construction(CallExpr *call, const StructTypeDef *def);
    void check_binops(MultiOpConstruct *mo, bool comparison, bool logical,
                      bool arith);
    void require_nonopt(StaticTypeRef t, Loc s, Loc e, const char *what);
    [[noreturn]] void mismatch(const std::string &m, Loc s, Loc e);
    [[noreturn]] void nullability(const std::string &m, Loc s, Loc e);
    [[noreturn]] void argcount(const std::string &m, Loc s, Loc e);

    /* Compile-time type queries `decltype(var)` / `typestr(expr)` /
     * `kindstr(expr)`: if the call is one, fold its (unevaluated) arg to a
     * LiteralStr of the static type (structural for decltype/typestr, the bare
     * kind for kindstr) and return true; false otherwise. See the def below. */
    bool fold_type_query(CallExpr *call);

    /* The StaticType a declaration's annotation PINS the symbol to (seeded by
     * reset_round, checked by contribute), or nullptr when it does not pin: a
     * scalar (bool/int/float/str), a struct, OR a parameterized container
     * (`array<int>`, `dict<str,P>`). A *generic* `array`/`dict` (no annot)
     * returns nullptr - it constrains only the kind (enforce_decl_types). */
    StaticTypeRef ann_scalar_static_type(const TypeSym *s) {
        const bool o = s->opt_decl;
        if (s->ann_annot) {        /* parameterized array<>/dict<>: full pin */
            StaticTypeRef t = annot_to_static_type(s->ann_annot.get());
            return o ? A.with_opt(t, true) : t;
        }
        switch (s->ann) {
            case DeclType::b: return A.bool_ty(o);
            case DeclType::i: return A.int_ty(o);
            case DeclType::f: return A.float_ty(o);
            case DeclType::s: return A.str_ty(o);
            case DeclType::strct:
                /* a struct annotation pins the exact struct type, just like a
                 * scalar one (reset_round seeds it, contribute checks each
                 * assignment is assignable - so `A x = B(...)` errors). */
                return s->ann_struct
                    ? A.struct_ty(s->ann_struct, s->ann_struct->name, o)
                    : nullptr;
            default:          return nullptr;
        }
    }

    /* small predicates */
    StaticTypeRef strip(StaticTypeRef t) { return A.with_opt(t, false); }
    static bool is_num(StaticTypeRef t) {
        t = static_type_resolve(t);
        return t->kind == StaticTypeKind::Bool || t->kind ==
            StaticTypeKind::Int ||
               t->kind == StaticTypeKind::Float;
    }
    static bool is_dyn(StaticTypeRef t) {
        return static_type_resolve(t)->kind == StaticTypeKind::Dyn;
    }
    static bool is_none(StaticTypeRef t) {
        return static_type_resolve(t)->kind == StaticTypeKind::None;
    }
    static bool is_unknown(StaticTypeRef t) {
        return static_type_resolve(t)->kind == StaticTypeKind::Unknown;
    }
    static bool is_optish(StaticTypeRef t) {
        t = static_type_resolve(t);
        return t->opt || t->kind == StaticTypeKind::None;
    }
    static bool is_func(StaticTypeRef t) {
        return static_type_resolve(t)->kind == StaticTypeKind::Func;
    }
};

/* ------------------------------- helpers --------------------------------- */

Scope *Inferencer::new_scope(Scope *parent)
{
    all_scopes.push_back(std::make_unique<Scope>());
    Scope *s = all_scopes.back().get();
    s->parent = parent;
    return s;
}

TypeSym *Inferencer::new_sym(const UniqueId *name, Scope *s, Loc loc)
{
    /*
     * `_` is RESERVED as the destructuring / foreach placeholder - it may only
     * appear in a binding position that is skipped (an IdList target or a
     * foreach loop var, both of which never reach here). Every readable
     * declaration of a name funnels through new_sym (a single var/const, a
     * param, a catch var, a function), so this one guard reserves `_`
     * everywhere it would otherwise become a real, readable name. (A struct
     * FIELD / dict key `_` is a member, not a scope sym, so it stays allowed.)
     */
    if (name->val == "_")
        throw SyntaxErrorEx(loc,
            "'_' is reserved as the destructuring/foreach placeholder and "
            "cannot be used as a variable, parameter, or function name");

    all_syms.push_back(std::make_unique<TypeSym>());
    TypeSym *sym = all_syms.back().get();
    sym->name = name;
    sym->type = bottom;
    sym->acc = bottom;
    sym->decl_loc = loc;
    sym->in_template = struct_tmpl_depth > 0;
    s->syms[name] = sym;
    return sym;
}

TypeSym *Inferencer::lookup(Scope *s, const UniqueId *name)
{
    for (; s; s = s->parent) {
        auto it = s->syms.find(name);
        if (it != s->syms.end())
            return it->second;
    }
    return nullptr;
}

/* The FuncInfo a callee expression denotes, if statically a specific function:
 * an identifier bound to a function (named func, or a var initialized with a
 * lambda), or an inline lambda. */
FuncInfo *Inferencer::callee_funcinfo(Construct *e)
{
    if (auto *id = dynamic_cast<Identifier *>(e)) {
        auto it = id_sym.find(id);
        if (it != id_sym.end() && it->second && it->second->func)
            return it->second->func;
        return nullptr;
    }
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(e)) {
        auto it = func_of_decl.find(fd);
        return it != func_of_decl.end() ? it->second : nullptr;
    }
    return nullptr;
}

bool Inferencer::is_builtin(const UniqueId *name)
{
    return EvalContext::builtins.find(name) != EvalContext::builtins.end() ||
           EvalContext::const_builtins.find(name) !=
               EvalContext::const_builtins.end();
}

Op Inferencer::compound_binop(Op op)
{
    switch (op) {
        case Op::addeq: return Op::plus;
        case Op::subeq: return Op::minus;
        case Op::muleq: return Op::times;
        case Op::diveq: return Op::div;
        case Op::modeq: return Op::mod;
        default:        return Op::invalid;
    }
}

/* True when control cannot fall off the end of `n` (it always returns/throws).
 * Used to decide if a block-bodied function contributes a `none` return. */
bool Inferencer::always_exits(const Construct *n)
{
    if (!n)
        return false;

    if (dynamic_cast<const ReturnStmt *>(n) ||
        dynamic_cast<const ThrowStmt *>(n) ||
        dynamic_cast<const RethrowStmt *>(n))
        return true;

    if (auto *b = dynamic_cast<const Block *>(n))
        return !b->elems.empty() && always_exits(b->elems.back().get());

    if (auto *i = dynamic_cast<const IfStmt *>(n))
        return i->elseBlock && always_exits(i->thenBlock.get()) &&
               always_exits(i->elseBlock.get());

    return false;
}

void Inferencer::for_each_child(Construct *n,
                                const std::function<void(Construct *)> &fn)
{
    if (!n)
        return;

    if (auto *e = dynamic_cast<Expr14 *>(n)) {
        fn(e->lvalue.get()); fn(e->rvalue.get()); return;
    }
    if (auto *c = dynamic_cast<CallExpr *>(n)) {
        fn(c->what.get()); fn(c->args.get()); return;
    }
    if (auto *m = dynamic_cast<MemberExpr *>(n)) {
        fn(m->what.get()); return;
    }
    if (auto *s = dynamic_cast<Subscript *>(n)) {
        fn(s->what.get()); fn(s->index.get()); return;
    }
    if (auto *s = dynamic_cast<Slice *>(n)) {
        fn(s->what.get()); fn(s->start_idx.get()); fn(s->end_idx.get()); return;
    }
    if (auto *i = dynamic_cast<IfStmt *>(n)) {
        fn(i->condExpr.get()); fn(i->thenBlock.get()); fn(i->elseBlock.get());
        return;
    }
    if (auto *w = dynamic_cast<WhileStmt *>(n)) {
        fn(w->condExpr.get()); fn(w->body.get()); return;
    }
    if (auto *f = dynamic_cast<ForStmt *>(n)) {
        fn(f->init.get()); fn(f->cond.get());
        fn(f->inc.get()); fn(f->body.get());
        return;
    }
    if (auto *fr = dynamic_cast<ForRangeStmt *>(n)) {
        fn(fr->init.get()); fn(fr->bound.get());
        fn(fr->step.get()); fn(fr->body.get());
        return;
    }
    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        fn(fe->container.get()); fn(fe->body.get()); return;
    }
    if (auto *t = dynamic_cast<TryCatchStmt *>(n)) {
        fn(t->tryBody.get());
        for (auto &cs : t->catchStmts)
            fn(cs.second.get());
        fn(t->finallyBody.get());
        return;
    }
    if (auto *r = dynamic_cast<ReturnStmt *>(n)) {
        fn(r->elem.get()); return;
    }
    if (auto *idc = dynamic_cast<IncDecExpr *>(n)) {
        fn(idc->lvalue.get()); return;
    }
    if (auto *te = dynamic_cast<TernaryExpr *>(n)) {
        fn(te->condExpr.get()); fn(te->thenExpr.get());
        fn(te->elseExpr.get()); return;
    }
    if (auto *co = dynamic_cast<CoalesceExpr *>(n)) {
        fn(co->lhs.get()); fn(co->rhs.get()); return;
    }
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        fn(fd->body.get()); return;
    }
    if (auto *ld = dynamic_cast<LiteralDict *>(n)) {
        for (auto &kv : ld->elems) {
            fn(kv->key.get()); fn(kv->value.get());
        }
        return;
    }
    if (auto *sc = dynamic_cast<SingleChildConstruct *>(n)) {
        fn(sc->elem.get()); return;
    }
    if (auto *mo = dynamic_cast<MultiOpConstruct *>(n)) {
        for (auto &pr : mo->elems)
            fn(pr.second.get());
        return;
    }
    /* MultiElemConstruct<>: Block, LiteralArray, ExprList */
    if (auto *me = dynamic_cast<MultiElemConstruct<> *>(n)) {
        for (auto &e : me->elems)
            fn(e.get());
        return;
    }
    /* leaves: literals, Identifier, Break/Continue/Rethrow/Nop */
}

/* ----------------------- monomorphization (templates) -------------------- */

/*
 * Promote a SAFE var-bound lambda to a template after the structural pass (when
 * `writes`/`value_used` are known). Safe = a non-capturing anonymous lambda
 * bound to a write-once var that is only ever CALLED (never passed/stored as a
 * value): then redirecting its calls to typed clones can't be observed through
 * the var, and there is no value use whose type would regress to dyn. This is
 * how `var id = func(x) => x; id(1); id("s")` stops being a join conflict.
 */
void Inferencer::mark_lambda_templates()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        FuncInfo *fi = s->func;
        if (!fi || fi->is_template || fi->pinned)
            continue;
        FuncDeclStmt *fd = fi->decl;
        if (!fd || fd->id)                  /* anonymous lambdas only */
            continue;
        if (s->writes != 1 || s->value_used)    /* write-once + calls-only */
            continue;
        if (fd->captures && !fd->captures->elems.empty())   /* non-capturing */
            continue;
        bool has_tparam = false;
        if (fd->params)
            for (auto &p : fd->params->elems)
                if (!p->opt_mod && !p->dyn_mod &&
                    p->decl_type == DeclType::none) {
                    has_tparam = true;
                    break;
                }
        if (has_tparam)
            fi->is_template = true;
    }
}

/* The Jacobi fixpoint, extracted so the instantiation loop can re-run it. */
void Inferencer::run_fixpoint(Block *rootBlock)
{
    for (int iter = 0; iter < 1000; iter++) {
        reset_round();
        cur_func = nullptr;
        for (auto &e : rootBlock->elems)
            accumulate(e.get());
        /* Value-instantiated clones have NO direct call to feed their
         * params - seed each with its uniform observed signature (a
         * phantom call, once per round; see the plan). */
        for (auto &kv : value_inst_sigs) {
            FuncInfo *cfi = kv.first;
            const std::vector<StaticTypeRef> &sig = kv.second;
            size_t k = 0;
            for (TypeSym *p : cfi->params) {
                if (p->opt_decl || p->dyn_decl || p->ann != DeclType::none)
                    continue;
                if (k < sig.size())
                    contribute(p, sig[k], Loc());
                k++;
            }
        }
        changed = false;
        commit_round();          /* sets `changed` if any type moved */
        if (!changed)
            break;
    }
}

/* Every CallExpr in the subtree (complete traversal). */
void Inferencer::collect_calls(Construct *n,
                               std::vector<std::pair<CallExpr *, bool>> &out,
                               bool in_func)
{
    if (!n)
        return;
    if (auto *c = dynamic_cast<CallExpr *>(n))
        out.push_back({ c, in_func });
    /* a call below a FuncDeclStmt is inside a function body (a live use) */
    const bool inside = in_func || (dynamic_cast<FuncDeclStmt *>(n) != nullptr);
    for_each_child(n, [&](Construct *c) { collect_calls(c, out, inside); });
}

/* Dedup key for an instantiation: the template identity + the signature. */
std::string Inferencer::template_sig_key(FuncInfo *tmpl,
                                         const std::vector<StaticTypeRef> &sig)
{
    std::string k = "T";
    k += std::to_string(reinterpret_cast<uintptr_t>(tmpl));
    for (StaticTypeRef t : sig) {
        k += ";";
        k += static_type_to_string(t);
    }
    return k;
}

/*
 * Build a concrete clone of a template for one signature: a fresh `$tmplN` name
 * (display_name keeps the original for backtraces), structurally walked so it
 * has its own FuncInfo + param/local syms, marked non-template, and inserted at
 * the root block's front. Its params are NOT seeded - the redirected call (the
 * only call site for this signature) feeds them through the concrete path.
 */
FuncDeclStmt *Inferencer::make_template_clone(FuncInfo *tmpl,
                                              const std::string &key,
                                              Block *rootBlock)
{
    unique_ptr<Construct> cl = tmpl->decl->clone();
    auto *fc = static_cast<FuncDeclStmt *>(cl.get());
    fc->desc->display_name = tmpl->decl->id
        ? std::string(tmpl->decl->id->get_str()) : std::string("<lambda>");
    /* Name the instance `<base>$<n>` so it is readable and INSPECTABLE
     * (typestr(f$0), :show f$0). The counter is per base NAME and monotonic for
     * the session (clone_name_counter, never reset), so a re-declared template
     * cannot reuse a prior instance's name. display_name keeps the original
     * for backtraces. A lambda template (no id) uses a `lambda$N` fallback. */
    const std::string base = tmpl->decl->id
        ? std::string(tmpl->decl->id->get_str()) : std::string("lambda");
    const int n = tmpl->decl->id
        ? clone_name_counter[tmpl->decl->id->uid]++
        : tmpl_clone_counter++;
    fc->id = make_unique<Identifier>(base + "$" + std::to_string(n));
    fc->sync_params();   /* re-snapshot desc->name from the synthetic id */

    /* clone() ran every node's ctor, so the clone is a genuinely DISTINCT node
     * from the template (a fresh node_id) - the property the whole monomorph-
     * ization relies on. If these were equal, clone() failed to deep-copy and
     * we would be re-resolving the template in place. (A var-bound lambda
     * template has no decl->id - it is anonymous - so guard that.) */
    ML_CHECK(fc->node_id != tmpl->decl->node_id);
    ML_CHECK(!tmpl->decl->id ||
             fc->id->node_id != tmpl->decl->id->node_id);

    /*
     * id_sym / func_of_decl are keyed by node POINTER and persist for the
     * session; clone() produces fresh nodes whose addresses can collide with
     * stale entries from freed nodes. Clear them on the clone's subtree so
     * walk_struct declares the clone's FuncInfo fresh (declare_funcdecl's
     * own guard would otherwise skip on a stale func_of_decl entry). The
     * Identifier resolution is also re-done unconditionally in walk_struct.
     */
    {
        std::function<void(Construct *)> clr = [&](Construct *n) {
            if (!n) return;
            id_sym.erase(n);
            if (auto *fd2 = dynamic_cast<FuncDeclStmt *>(n)) {
                func_of_decl.erase(fd2);
                clr(fd2->captures.get());
                clr(fd2->params.get());
                clr(fd2->body.get());
                return;
            }
            for_each_child(n, clr);
        };
        clr(fc);
    }

    struct_in_clone = true;             /* the clone's syms are the instance */
    walk_struct(fc, global);            /* declare its name + build its syms */
    struct_in_clone = false;
    FuncInfo *cfi = func_of_decl.count(fc) ? func_of_decl[fc] : nullptr;
    /* walk_struct must have built the clone its OWN FuncInfo (not reused the
     * template's, and not skipped on a stale func_of_decl entry). */
    ML_CHECK(cfi && cfi != tmpl);
    cfi->is_template = false;             /* an instantiation is concrete */

    FuncDeclStmt *raw = fc;
    rootBlock->elems.insert(rootBlock->elems.begin(), std::move(cl));
    tmpl_cache[key] = fc->id->uid;       /* cache the stable NAME, not a node */
    return raw;
}

/*
 * One instantiation round: for every call whose callee is a template and whose
 * argument types have settled, get-or-make the clone for that signature and
 * redirect the call to it. Returns whether any call was redirected (so the
 * caller re-runs the fixpoint and looks for newly-exposed template calls).
 */
bool Inferencer::instantiate_round(Block *rootBlock)
{
    std::vector<std::pair<CallExpr *, bool>> calls;
    for (auto &e : rootBlock->elems)
        collect_calls(e.get(), calls);

    bool progress = false;
    for (const auto &call_pair : calls) {
        CallExpr *call = call_pair.first;
        const bool call_in_func = call_pair.second;
        FuncInfo *tmpl = callee_funcinfo(call->what.get());
        if (!tmpl || !tmpl->is_template)
            continue;

        /* (cross-input re-enabled; instrumented to root-cause the MSVC bug) */

        /* Arity must be in the legal range [min, nparams] (a trailing opt param
         * may be omitted); else leave the call for check_call to report. */
        const size_t nparams = tmpl->params.size();
        const size_t nargs = call->args->elems.size();
        size_t min_args = 0;
        for (size_t i = 0; i < nparams; i++)
            if (!tmpl->params[i]->opt_decl)
                min_args = i + 1;
        if (nargs < min_args || nargs > nparams)
            continue;

        /* The signature is keyed by the TEMPLATE params (un-annotated, non-dyn,
         * non-opt) only; opt/typed/dyn params just join within the clone. A
         * template param is non-opt, so min_args > its index => its arg is
         * always present. */
        std::vector<StaticTypeRef> sig;
        bool ready = true;
        for (size_t i = 0; i < nparams; i++) {
            TypeSym *p = tmpl->params[i];
            if (p->opt_decl || p->dyn_decl || p->ann != DeclType::none)
                continue;       /* not a template param */
            StaticTypeRef t =
                static_type_resolve(type_of(call->args->elems[i].get()));
            if (is_unknown(t)) { ready = false; break; }
            sig.push_back(t);
        }
        if (!ready)
            continue;       /* args not settled yet; a later round */

        const std::string key = template_sig_key(tmpl, sig);

        /*
         * Get-or-make the instance for this (template, signature). The cache is
         * session-persistent, so a cached entry may name an instance built by a
         * PRIOR input - reach it by NAME in the global scope (its FuncInfo is
         * pinned). The cached name is dropped (rebuild) only if the instance is
         * gone (undef, or a rejected input rolled it back).
         */
        const UniqueId *clone_name = nullptr;
        TypeSym *clone_sym = nullptr;
        bool reused = false;

        auto it = tmpl_cache.find(key);
        if (it != tmpl_cache.end()) {
            auto gs = global->syms.find(it->second);
            if (gs != global->syms.end() && gs->second && gs->second->func) {
                clone_name = it->second;
                clone_sym = gs->second;
                reused = true;
            }
        }

        if (!clone_sym) {
            /* D4: a new signature past the cap runs dynamically, not as a clone
             * - a backstop for pathological polymorphic recursion. */
            static const int MAX_TMPL_INSTANCES = 64;
            if (tmpl_inst_count[tmpl] >= MAX_TMPL_INSTANCES) {
                if (tmpl_cap_warned.insert(tmpl).second && tmpl->decl->id) {
                    auto nm = tmpl->decl->id->get_str();
                    fprintf(stderr,
                            "warning: template '%.*s' exceeded %d "
                            "instantiations; further calls run dynamically\n",
                            static_cast<int>(nm.size()), nm.data(),
                            MAX_TMPL_INSTANCES);
                }
                continue;
            }
            FuncDeclStmt *clone = make_template_clone(tmpl, key, rootBlock);
            clone_name = clone->id->uid;
            auto cs = id_sym.find(clone->id.get());
            clone_sym = (cs != id_sym.end()) ? cs->second : nullptr;
            if (!clone_sym) {            /* fall back to the by-name binding */
                auto gs = global->syms.find(clone_name);
                clone_sym = (gs != global->syms.end()) ? gs->second : nullptr;
            }
            tmpl_inst_count[tmpl]++;
        }

        if (!clone_sym)
            continue;                   /* couldn't resolve the instance */

        /* mark the instance as having a live consumer when this redirected call
         * is inside a function body (so a redefine of the base won't GC it) */
        if (call_in_func && clone_sym->func)
            clone_sym->func->has_func_consumer = true;

        if (trace_enabled(TraceCat::templ)) {
            std::string ss;
            for (size_t i = 0; i < sig.size(); i++) {
                if (i)
                    ss += ", ";
                ss += static_type_to_string(sig[i]);
            }
            const std::string nm = tmpl->decl->id
                ? std::string(tmpl->decl->id->get_str())
                : std::string("<lambda>");
            TRACE(templ, 0, nm + "(" + ss + ") -> " +
                  std::string(clone_name->val) +
                  (reused ? "  (reused)"
                          : "  (instance " +
                            std::to_string(tmpl_inst_count[tmpl]) + ")"));
        }

        auto what = make_unique<Identifier>(clone_name->val);
        what->start = call->what->start;
        what->end = call->what->end;
        id_sym[what.get()] = clone_sym;
        /* The old callee Identifier is about to be freed: drop its id_sym entry
         * so nothing (e.g. -a's collect_arrays) walks a dangling key. */
        id_sym.erase(call->what.get());
        call->what = std::move(what);
        progress = true;
    }
    return progress;
}

void Inferencer::drain_escapes()
{
    for (void *p : A.escaped_finfos)
        static_cast<FuncInfo *>(p)->value_escaped = true;
    A.escaped_finfos.clear();
}

/*
 * Value-used template instantiation (plans/archived/value-template-instantiation.md).
 * A template whose VALUE flows (through vars/containers - the finfo set on
 * its Func static type rides the ordinary lattice) to indirect call sites
 * that all agree on ONE settled signature is instantiated for that
 * signature, and EVERY value use of its name is redirected (in place) to
 * the instance - so the containers hold the TYPED clone and the indirect
 * calls run its typed body. Escapes (a set-dropping join, an arg-position
 * use, a capture) disable it - the base keeps running, as before.
 */
bool Inferencer::value_instantiate_round(Block *rootBlock)
{
    /* 1) Every VALUE USE of a template identifier: a non-callee Identifier
     * resolving to a template FuncInfo. An ARG-position use (any call's
     * argument - incl. builtins: map/filter/runtime/...) or a CAPTURE-list
     * use ESCAPES the template (an untracked consumer). */
    struct Use { Identifier *id; TypeSym *sym; bool in_func; };
    std::map<FuncInfo *, std::vector<Use>> uses;

    std::function<void(Construct *, bool)> walk =
        [&](Construct *n, bool in_func) {
        if (!n)
            return;
        if (auto *call = dynamic_cast<CallExpr *>(n)) {
            /* the callee position is NOT a value use; ARG uses escape */
            walk(call->what.get(), in_func);   /* nested callee exprs */
            for (auto &a : call->args->elems) {
                if (auto *id = dynamic_cast<Identifier *>(a.get())) {
                    auto it = id_sym.find(id);
                    if (it != id_sym.end() && it->second && it->second->func
                            && it->second->func->is_template) {
                        it->second->func->value_escaped = true;
                        continue;
                    }
                }
                walk(a.get(), in_func);
            }
            return;
        }
        if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
            if (fd->captures)
                for (auto &cap : fd->captures->elems) {
                    if (auto *id = dynamic_cast<Identifier *>(cap.get())) {
                        auto it = id_sym.find(id);
                        if (it != id_sym.end() && it->second
                                && it->second->func
                                && it->second->func->is_template)
                            it->second->func->value_escaped = true;
                    }
                }
            for_each_child(n, [&](Construct *c) { walk(c, true); });
            return;
        }
        if (auto *id = dynamic_cast<Identifier *>(n)) {
            auto it = id_sym.find(id);
            if (it != id_sym.end() && it->second && it->second->func
                    && it->second->func->is_template)
                uses[it->second->func].push_back({ id, it->second, in_func });
            return;
        }
        for_each_child(n, [&](Construct *c) { walk(c, in_func); });
    };
    for (auto &e : rootBlock->elems)
        walk(e.get(), false);

    if (uses.empty())
        return false;

    /* 2) Every INDIRECT call (no direct FuncInfo) whose callee type carries
     * a finfo set: attribute its signature to each member. */
    struct Site { CallExpr *call; bool in_func; };
    std::map<FuncInfo *, std::vector<Site>> sites;
    std::vector<std::pair<CallExpr *, bool>> calls;
    for (auto &e : rootBlock->elems)
        collect_calls(e.get(), calls);
    for (auto &cp : calls) {
        if (callee_funcinfo(cp.first->what.get()))
            continue;                       /* a direct call */
        StaticTypeRef ct =
            static_type_resolve(type_of(cp.first->what.get()));
        if (!ct || ct->kind != StaticTypeKind::Func)
            continue;
        for (void *p : ct->finfos)
            sites[static_cast<FuncInfo *>(p)].push_back(
                { cp.first, cp.second });
    }

    bool progress = false;

    for (auto &kv : uses) {
        FuncInfo *tmpl = kv.first;
        if (!tmpl->is_template || tmpl->value_escaped || !tmpl->decl)
            continue;
        if (value_inst_done.count(tmpl))
            continue;
        auto st = sites.find(tmpl);
        if (st == sites.end() || st->second.empty())
            continue;               /* no attributed call - can't know a sig */

        /* 3) UNIFORMITY: every attributed call must have the same settled,
         * arity-legal signature over the TEMPLATE params. */
        const size_t nparams = tmpl->params.size();
        size_t min_args = 0;
        for (size_t i = 0; i < nparams; i++)
            if (!tmpl->params[i]->opt_decl)
                min_args = i + 1;

        std::vector<StaticTypeRef> sig;
        bool ok = true, first = true;
        for (const Site &site : st->second) {
            const size_t nargs = site.call->args->elems.size();
            if (nargs < min_args || nargs > nparams) { ok = false; break; }
            std::vector<StaticTypeRef> s1;
            for (size_t i = 0; i < nparams; i++) {
                TypeSym *p = tmpl->params[i];
                if (p->opt_decl || p->dyn_decl || p->ann != DeclType::none)
                    continue;              /* not a template param */
                StaticTypeRef t = i < nargs
                    ? static_type_resolve(
                          type_of(site.call->args->elems[i].get()))
                    : A.none_ty();
                if (is_unknown(t)) { ok = false; break; }
                s1.push_back(t);
            }
            if (!ok)
                break;
            if (first) {
                sig = std::move(s1);
                first = false;
            } else if (sig.size() != s1.size()
                       || !std::equal(sig.begin(), sig.end(), s1.begin(),
                                      [](StaticTypeRef a, StaticTypeRef b) {
                                          return static_type_equal(a, b);
                                      })) {
                ok = false;
                break;
            }
        }
        if (!ok || first)
            continue;

        /*
         * A signature that cannot improve on the base - leave the base
         * running (boxed, exactly as before this feature):
         *  - a `dyn` param: the clone would be just as boxed.
         *  - a container carrying the BOTTOM `none` element type (an empty
         *    `[]`/`{}` the caller has no element info for - see
         *    type_has_bottom_elem). This one is not merely useless but
         *    HARMFUL: the clone's params are seeded from the signature, so a
         *    body that only READS the container types every element as `none`,
         *    and an ordinary use of one (`k + ":"`) becomes a spurious
         *    NullabilityEx - even though the container is non-empty at
         *    runtime, having been filled by a SIBLING function through its
         *    reference param (samples/phonebook: `cmd_add` fills `data`,
         *    `cmd_view` iterates it). A writer body repairs the type by
         *    contribution; a reader body cannot, so declining is the only
         *    sound answer. Deferred, not vetoed: a later fixpoint round may
         *    settle the element type, and then the instance is made.
         */
        bool bad_sig = false;
        for (StaticTypeRef t : sig)
            if (type_has_dyn(t, /*deep=*/false) || type_has_bottom_elem(t))
                bad_sig = true;
        if (bad_sig)
            continue;

        /* 4) Get-or-make the instance (the ordinary template machinery). */
        const std::string key = template_sig_key(tmpl, sig);
        const UniqueId *clone_name = nullptr;
        TypeSym *clone_sym = nullptr;
        auto itc = tmpl_cache.find(key);
        if (itc != tmpl_cache.end()) {
            auto gs = global->syms.find(itc->second);
            if (gs != global->syms.end() && gs->second && gs->second->func) {
                clone_name = itc->second;
                clone_sym = gs->second;
            }
        }
        if (!clone_sym) {
            static const int MAX_TMPL_INSTANCES = 64;
            if (tmpl_inst_count[tmpl] >= MAX_TMPL_INSTANCES)
                continue;
            FuncDeclStmt *clone = make_template_clone(tmpl, key, rootBlock);
            clone_name = clone->id->uid;
            auto cs = id_sym.find(clone->id.get());
            clone_sym = (cs != id_sym.end()) ? cs->second : nullptr;
            if (!clone_sym) {
                auto gs = global->syms.find(clone_name);
                clone_sym = (gs != global->syms.end()) ? gs->second : nullptr;
            }
            tmpl_inst_count[tmpl]++;
        }
        if (!clone_sym || !clone_sym->func)
            continue;

        value_inst_sigs[clone_sym->func] = sig;
        value_inst_done.insert(tmpl);

        if (trace_enabled(TraceCat::templ)) {
            std::string ss;
            for (size_t i = 0; i < sig.size(); i++) {
                if (i) ss += ", ";
                ss += static_type_to_string(sig[i]);
            }
            const std::string nm = tmpl->decl->id
                ? std::string(tmpl->decl->id->get_str())
                : std::string("<lambda>");
            TRACE(templ, 0, "value-instantiate " + nm + "(" + ss + ") -> " +
                  std::string(clone_name->val));
        }

        /* 5) Redirect EVERY value use IN PLACE (uid + sym rebind - the
         * Identifier node keeps its loc, so carets are unchanged). Each
         * redirected use IS a value use of the CLONE (that is the
         * feature's name), so mark its sym: the instance's FuncObject
         * is reachable as a value (`ops[0]`), dyn-launderable, and
         * therefore callable with unchecked args - C3's proven-param
         * stamp must never fire on it. */
        for (const Use &u : kv.second) {
            u.id->uid = clone_name;
            id_sym[u.id] = clone_sym;
            clone_sym->value_used = true;
            if (u.in_func)
                clone_sym->func->has_func_consumer = true;
        }
        progress = true;
    }

    return progress;
}

/* ------------------------------ run / passes ----------------------------- */

/*
 * One-time setup shared by the one-shot and the REPL-incremental paths: the
 * `bottom` Unknown identity and the persistent global scope. The arena and side
 * tables persist for the Inferencer's lifetime (one call for a script; a
 * whole session for the REPL).
 */
void Inferencer::setup()
{
    bottom = A.fresh_var();
    global = new_scope(nullptr);

    /* Native composite types (reflection): register so a builtin returning one
     * (e.g. layout() -> StructLayout) and its field accesses type-check. */
    const StructTypeDef *sf = native_struct_field_def();
    const StructTypeDef *sl = native_struct_layout_def();
    const StructTypeDef *ty = native_struct_type_def();
    struct_by_name[sf->name] = sf;
    struct_by_name[sl->name] = sl;
    struct_by_name[ty->name] = ty;
}

/*
 * Infer + check one root Block. In the one-shot path it is the whole program;
 * in the REPL it is one input, with prior globals already PINNED (so the
 * fixpoint reads but never recomputes them). All passes operate on this root's
 * elems only; the persistent global scope / arena carry cross-input state.
 */
void Inferencer::infer_one(Block *rootBlock)
{
    hoist_globals(rootBlock);
    for (auto &e : rootBlock->elems)
        walk_struct(e.get(), global);

    /*
     * Desugar named-argument calls into positional ones BEFORE anything else
     * reads the call shape (the fixpoint, the check pass, and every later pass:
     * resolve_names, the optimizers, eval). This is syntactic lowering, not
     * type checking, so it runs even when checks are disabled (-nti).
     */
    for (auto &e : rootBlock->elems)
        lower_named_args(e.get());

    /* isbound()'s argument SHAPE - syntactic, so ungated (see the fn). */
    for (auto &e : rootBlock->elems)
        check_isbound_args(e.get());

    /* Reserve DEV-only builtins (show()) in a script - a compile-time error,
     * even under -nti (this runs before the checks_enabled early return). A
     * no-op in the REPL / test runner (they set g_dev_builtins_allowed). */
    if (!g_dev_builtins_allowed)
        for (auto &e : rootBlock->elems)
            reject_dev_builtins(e.get());

    if (!checks_enabled)
        return;

    mark_lambda_templates();   /* safe var-bound lambdas become templates */
    run_fixpoint(rootBlock);

    /*
     * Monomorphization. A template (un-annotated params) is skipped by the
     * fixpoint; instead, each of its call-site signatures becomes a typed clone
     * (params accumulate the signature through the concrete path), the call is
     * redirected, and the fixpoint re-runs so the clone's body infers. Iterate
     * until no new signature appears - a clone's body may call other templates
     * (or itself), which surface only once its own params are settled.
     */
    for (int round = 0; round < 64; round++) {
        drain_escapes();
        const bool p1 = instantiate_round(rootBlock);
        const bool p2 = value_instantiate_round(rootBlock);
        if (!p1 && !p2)
            break;
        run_fixpoint(rootBlock);
    }
    drain_escapes();

    /* finalize. An unconstrained value is `none` for a local ("only-none /
     * doesn't matter") but `dyn` for a PARAMETER: a never-(concretely-)called
     * function's parameter could be anything, so its body must still type-check
     * (this covers uncalled funcs, callbacks folded at parse time, and HO uses
     * we don't model precisely). dyn/opt declarations win as written. */
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func || s->pinned)   /* already finalized in its own input */
            continue;
        if (s->dyn_decl) {
            /*
             * Phase B: a dyn symbol's TYPE is dyn, but its nullability is
             * orthogonal and tracked independently. A dyn *param*'s opt is
             * *declared* (`opt dyn`); a dyn *var*'s opt is *inferred* from what
             * flows in (the committed type carries the opt bit, since
             * contribute() now joins dyn vars). A non-opt dyn is proven
             * non-null - usable without a none-check.
             */
            const bool opt = s->opt_decl ||
                             (!s->is_param && is_optish(s->type));
            s->type = A.with_opt(A.dyn_ty(), opt);
        } else {
            if (is_unknown(s->type))
                /* An unconstrained PARAM (never concretely called) or foreach
                 * loop var (container was Unknown/dyn) is `dyn` - it could be
                 * anything. A plain unconstrained local is `none`. */
                s->type = (s->is_param || s->is_loopvar) ? A.dyn_ty()
                                                         : A.none_ty();
            if (s->opt_decl)
                s->type = A.with_opt(s->type, true);
        }
    }
    /*
     * Mark a DEAD base TEMPLATE's decl so the AOT codegen / -vd skip it: it is a
     * monomorphization source with no runtime reachability, so it must not exist
     * as a compiled chunk. "Dead" = a template that is NEVER used as a VALUE
     * (`!value_used`): then every call to it is a DIRECT call, all of which the
     * instantiation pass redirected to a concrete `name$N` instance - so the
     * base never runs. A VALUE-used template (stored in a dict, passed to a
     * builtin, bound to a var) can be dispatched INDIRECTLY - that call is NOT
     * redirected, so it runs the base body - so it is kept (compiled + shown).
     * (Its instances, if any, have is_template=false and are unaffected.) The
     * residual case a `!value_used` template still runs the base - a D4 overflow
     * (>64 instances) or an uninstantiable direct call - just tree-walks instead
     * of VM-running (correct, only slower); the planned first-class dyn instance
     * (foo$dyn) erases even that. Keyed off the name SYM's value_used.
     *
     * #149: reachability is TRANSITIVE, so "kept" is a CLOSURE, not the
     * flag. A value-used base runs its ORIGINAL body, whose calls were
     * never redirected to instances (the check/instantiation passes skip
     * template bodies) - so any template that body names is itself
     * runtime-reachable one hop in, and so on: `var dyn g = aa;` keeps aa
     * (value-used), and aa's base body calls bb, whose base used to be
     * excluded as dead - the indirect g() call then reached a chunk-less
     * bb and, post-teardown, aborted (or under ASSERTS=0 walked a freed
     * AST). Fixpoint over the kept bases' bodies: any IDENTIFIER naming a
     * template (call or value use; nested lambdas descended - the whole
     * subtree runs when the base runs) keeps that template too.
     * Over-keeping (a body-local shadowing a template's name) costs a
     * compiled chunk, never correctness.
     */
    std::map<const FuncInfo *, TypeSym *> tmpls;
    std::map<const UniqueId *, TypeSym *> tmpl_by_uid;
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func && s->func->is_template && s->func->decl) {
            tmpls.emplace(s->func, s);
            tmpl_by_uid.emplace(s->name, s);
        }
    }
    std::set<const FuncInfo *> kept_bases;
    {
        std::vector<TypeSym *> work;
        for (auto &kv : tmpls)
            if (kv.second->value_used && kept_bases.insert(kv.first).second)
                work.push_back(kv.second);
        std::function<void(Construct *)> scan = [&](Construct *n) {
            if (!n)
                return;
            if (auto *id = dynamic_cast<Identifier *>(n)) {
                auto it = tmpl_by_uid.find(id->uid);
                if (it != tmpl_by_uid.end()
                        && kept_bases.insert(it->second->func).second)
                    work.push_back(it->second);
                return;
            }
            if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
                scan(fd->body.get());   /* a nested lambda runs with the
                                         * base - its references count */
                return;
            }
            for_each_child(n, [&](Construct *c) { scan(c); });
        };
        while (!work.empty()) {
            TypeSym *t = work.back();
            work.pop_back();
            scan(t->func->decl->body.get());
        }
    }
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func && s->func->is_template && s->func->decl) {
            /* ANY template base -> skip specialization (a monomorphization
             * shell). A dead one (unreachable per the closure) -> also
             * skip codegen. */
            s->func->decl->is_template = true;
            if (!kept_bases.count(s->func))
                s->func->decl->desc->is_template_base = true;
        }
        /*
         * dyn-into-concrete coercion: a plain `var` that keeps a NUMERIC type
         * while receiving a `dyn` value (coerces_dyn) needs the runtime to
         * coerce that value to its type - exactly what an EXPLICIT `int s` /
         * `float s` does via `coerce_to_decl_type`. Stamp the decl Identifier's
         * `decl_type` (only when NOT already annotated), so resolve_names
         * propagates it to the uses and the store fast paths coerce. A `dyn`
         * value holding the right numeric type stores fine; a mismatched one
         * (e.g. a float into an int var) is the runtime error the maintainer's
         * rule wants. Skip a param (bound, not decl_type-coerced) and a pinned
         * REPL global.
         */
        if (s->coerces_dyn && !s->is_param && !s->pinned && s->decl_id &&
                s->decl_id->decl_type == DeclType::none) {
            const StaticTypeRef ty = static_type_resolve(s->type);
            if (ty->kind == StaticTypeKind::Int)
                s->decl_id->decl_type = DeclType::i;
            else if (ty->kind == StaticTypeKind::Float)
                s->decl_id->decl_type = DeclType::f;
        }
    }
    for (auto &up : all_funcs) {
        /*
         * An Unknown return means the function has return statement(s) whose
         * value never resolved to a concrete type - it depends on unconstrained
         * (effectively dyn) inputs, e.g. a function only ever passed as a value
         * (never directly called, so its params stay Unknown). That is `dyn`,
         * not `none`. A function with no value-returning path contributes
         * `none` to ret_acc, so its return is `none` here, not Unknown - this
         * branch doesn't touch it.
         */
        if (up->pinned)
            continue;
        if (is_unknown(up->ret))
            up->ret = A.dyn_ty();
    }

    /* Trace the finalized types (the conclusion of the fixpoint), so
     * `:trace infer` shows both the per-round climb and the final answer. */
    if (trace_enabled(TraceCat::infer)) {
        /* A template's base params/locals are dyn/none FALLBACKS, not real
         * inference (the fixpoint skips an un-instantiated template - it is
         * checked per call-site clone, not in isolation). Don't report them as
         * types; the "template" line below says so instead. */
        for (auto &up : all_syms) {
            TypeSym *s = up.get();
            if (s->func || s->pinned || !s->name || s->in_template)
                continue;
            const char *kind = s->is_param ? "param"
                             : s->const_decl ? "const" : "var";
            TRACE(infer, 0, std::string(kind) + " " +
                            std::string(s->name->val) + " : " +
                            static_type_to_string(s->type) + "  (final)");
        }
        for (auto &up : all_funcs) {
            if (up->pinned || !up->decl || !up->decl->id)
                continue;
            std::string ps;
            for (size_t i = 0; i < up->params.size(); i++) {
                if (i)
                    ps += ", ";
                ps += up->params[i]->name
                          ? std::string(up->params[i]->name->val)
                          : std::string("_");
                if (!up->is_template)            /* a template's are unbound */
                    ps += ": " + static_type_to_string(up->params[i]->type);
            }
            if (up->is_template)
                TRACE(infer, 0, "func " +
                                std::string(up->decl->id->get_str()) + "(" +
                                ps + ")  template (instantiated per call)");
            else
                TRACE(infer, 0, "func " +
                                std::string(up->decl->id->get_str()) + "(" +
                                ps + ") -> " + static_type_to_string(up->ret) +
                                "  (final)");
        }
    }

    cur_func = nullptr;
    narrowing_on = true;
    for (auto &e : rootBlock->elems)
        check(e.get());
    narrowing_on = false;

    /*
     * Mandatory-`dyn` rule: a plain var/const must have a concrete type. Run it
     * AFTER the check pass so that a var which is dyn *because of* a real type
     * error (e.g. `-none`, subscripting a non-container) surfaces that error
     * first, instead of being masked by DynRequiredEx.
     */
    if (strict_dyn) {
        enforce_decl_types();
        enforce_concrete_decls();
        enforce_nonnull_params();
    }

    /* Stamp TypeHints (int/float) for the M8 specializer + typed conditions. */
    for (auto &e : rootBlock->elems)
        annotate_hints(e.get());
}

void Inferencer::run()
{
    Block *rootBlock = dynamic_cast<Block *>(root);
    if (!rootBlock)
        return;

    setup();
    infer_one(rootBlock);
    stamp_proven_params();
}

/*
 * C3: stamp ParamDesc::proven_type for params that can ONLY ever
 * receive one scalar kind - see the field's comment in funcdesc.h for
 * the full soundness gate. ONE-SHOT only (run(); ReplInfer never calls
 * this): the REPL's open-world redefinition + input-local value rules
 * make the never-value-used proof weaker there, and the win is a
 * script-runtime one. Global-scope functions only (a nested scoped
 * func is rare and conservatively skipped). specializations()/globals()
 * return NAMES, and `$` is not an identifier character, so an instance
 * FuncObject is unreachable as a value except through the value uses
 * this gate excludes.
 */
void Inferencer::stamp_proven_params()
{
    for (const auto &kv : global->syms) {
        const TypeSym *s = kv.second;
        if (!s->func || s->value_used)
            continue;
        FuncInfo *fi = s->func;
        if (fi->is_template || fi->value_escaped || !fi->decl
                || !fi->decl->desc)
            continue;
        FuncDescriptor *d =
            const_cast<FuncDescriptor *>(fi->decl->desc);
        const size_t n = std::min(fi->params.size(), d->params.size());
        for (size_t i = 0; i < n; i++) {
            const TypeSym *p = fi->params[i];
            if (!p || p->dyn_decl || p->opt_decl
                    || p->ann != DeclType::none || !p->type)
                continue;
            const StaticType *t = static_type_resolve(p->type);
            if (!t || t->opt)
                continue;
            if (t->kind == StaticTypeKind::Int)
                d->params[i].proven_type = DeclType::i;
            else if (t->kind == StaticTypeKind::Float)
                d->params[i].proven_type = DeclType::f;
        }
    }
}

/*
 * REPL incremental: type-check one input against the already-committed globals,
 * then COMMIT this input's new globals - mark every symbol/func created during
 * this call `pinned`, so the next input reads their types as fixed (and an
 * assignment that violates one is the cross-input type error). Throws on a type
 * error before pinning, so a rejected input commits nothing.
 */
void Inferencer::infer_input(Block *rootBlock)
{
    if (!rootBlock)
        return;

    /*
     * The template instantiation cache is SESSION-persistent (see its
     * declaration): a signature already instantiated by a prior input reuses
     * that instance instead of building a duplicate (the cross-input link is
     * the instance's name in the global scope, not a node pointer). It is only
     * snapshotted here so a REJECTED input - whose clone bindings are rolled
     * back below - drops its cache entries too (self-heal would otherwise
     * rebuild on the next use; the snapshot makes it exact). The clone-name
     * counter stays monotonic, so names never collide.
     */
    auto tmpl_cache_snapshot = tmpl_cache;

    /*
     * SCOPE THE NODE-KEYED MAPS TO ONE INPUT. id_sym / func_of_decl are keyed
     * by raw Construct* (AST node ADDRESS), which the allocator recycles once a
     * node is freed - so a stale entry from a prior input could be matched by a
     * fresh node reusing that address (the MSVC-only, address-dependent bug we
     * root-caused). walk_struct rebuilds both for THIS input, and no reader
     * looks up a prior input's node (the cross-input link is the persistent,
     * arena-stable TypeSym/FuncInfo, reached via the UniqueId-keyed scope - see
     * callee_funcinfo). So clearing here removes the staleness at the source.
     * Belt-and-suspenders: walk_struct also always re-resolves (never trusts a
     * present entry), and a RECYCLE=1 build forces the hostile reuse under -rt.
     */
    id_sym.clear();
    func_of_decl.clear();

    const size_t sym_base = all_syms.size();
    const size_t func_base = all_funcs.size();
    /* snapshot the global bindings so a rejected input commits nothing */
    auto global_snapshot = global->syms;

    auto pin_new = [&]() {
        for (size_t i = sym_base; i < all_syms.size(); i++) {
            TypeSym *s = all_syms[i].get();
            /*
             * A plain `var a;` (no annotation, unconstrained) finalized to
             * `none` - a useless committed type that would reject EVERY later
             * assignment (`a = 1` -> "'a' has type 'none' but is assigned
             * 'int'"). A SCRIPT infers it from later uses (join: `var a; a = 3`
             * -> `int?`), but the REPL commits each input before seeing the
             * next, so it cannot defer - commit `var a;` as `dyn?` (nullable
             * dynamic) so future inputs may assign any type. An annotated /
             * `dyn` / param sym keeps its finalized type.
             */
            if (!s->func && !s->is_param && !s->dyn_decl &&
                !ann_scalar_static_type(s) &&
                is_none(static_type_resolve(s->type)))
                s->type = A.with_opt(A.dyn_ty(), true);
            s->pinned = true;
        }
        for (size_t i = func_base; i < all_funcs.size(); i++)
            all_funcs[i]->pinned = true;
    };

    try {
        infer_one(rootBlock);
    } catch (...) {
        /* Reject: restore the global scope (drop any decl this input added) and
         * the instantiation cache (drop any clone this input registered), then
         * `pin` (i.e. skip henceforth) the half-built symbols so no later input
         * re-finalizes/re-enforces them. They stay in all_syms but inert. */
        global->syms = std::move(global_snapshot);
        tmpl_cache = std::move(tmpl_cache_snapshot);
        pin_new();
        throw;
    }

    pin_new();     /* success: commit this input's new globals */
}

/* REPL `undef(x)`: drop a global from the committed set so a later `var x` of a
 * different type is a fresh declaration, not a type conflict. The old TypeSym
 * stays in all_syms (pinned, inert); only the scope binding is removed. */
void Inferencer::undef_global(const UniqueId *name)
{
    if (global)
        global->syms.erase(name);
}

std::string Inferencer::global_type_str(const UniqueId *name)
{
    if (!global)
        return "";
    auto it = global->syms.find(name);
    if (it == global->syms.end() || !it->second)
        return "";
    TypeSym *s = it->second;
    StaticTypeRef ty = s->func ? func_static_type(s->func) : s->type;
    return static_type_to_string(ty);
}

std::vector<std::string>
Inferencer::func_param_types(const FuncDeclStmt *fn)
{
    std::vector<std::string> out;
    if (!fn)
        return out;
    for (auto &up : all_funcs) {
        if (up->decl != fn)
            continue;
        if (up->is_template)        /* an un-instantiated template: unbound */
            return out;
        for (TypeSym *p : up->params)
            out.push_back(p ? static_type_to_string(p->type) : std::string());
        return out;
    }
    return out;
}

std::string
Inferencer::func_return_type(const FuncDeclStmt *fn)
{
    if (!fn)
        return "";
    for (auto &up : all_funcs) {
        if (up->decl != fn)
            continue;
        if (up->is_template)        /* an un-instantiated template: unbound */
            return "";
        return static_type_to_string(up->ret);
    }
    return "";
}

bool
Inferencer::instance_has_consumer(const FuncDeclStmt *fn)
{
    if (!fn)
        return true;                /* unknown: keep (conservative) */
    for (auto &up : all_funcs)
        if (up->decl == fn)
            return up->has_func_consumer;
    return true;
}

/*
 * True if `t` is dynamic. `deep` recurses into container element/key/value
 * types (so array<dyn>/dict<_,dyn> count) - the Phase-B (strict) sense; with
 * `deep` false only a bare top-level `dyn` counts (Phase A, tolerating
 * dynamic-element arrays). Function types are NOT recursed into: a var holding
 * a `func(dyn)->int` holds a concrete function value.
 */
bool Inferencer::type_has_dyn(StaticTypeRef t, bool deep)
{
    t = static_type_resolve(t);

    if (t->kind == StaticTypeKind::Dyn)
        return true;

    if (!deep)
        return false;

    if (t->kind == StaticTypeKind::Array)
        return type_has_dyn(t->elem, true);

    if (t->kind == StaticTypeKind::Dict)
        return type_has_dyn(t->key, true) || type_has_dyn(t->val, true);

    return false;
}

/*
 * True if `t` carries NO element information: a container whose element (or
 * key/value) type is the BOTTOM `none` - what an empty `[]` / `{}` infers, and
 * what a container symbol KEEPS for as long as nothing the inferencer can see
 * writes into it (a callee filling it through its reference param contributes
 * to the PARAM's symbol, never back to the caller's - so `var data = {}` filled
 * only by a callee stays `dict<none,none>`). Such a type is a placeholder, not
 * a settled signature: monomorphizing on it types every element READ in the
 * clone as `none`. Recurses through containers; a Func carries no elements.
 */
bool Inferencer::type_has_bottom_elem(StaticTypeRef t)
{
    t = static_type_resolve(t);

    if (t->kind == StaticTypeKind::Array)
        return is_none(t->elem) || type_has_bottom_elem(t->elem);

    if (t->kind == StaticTypeKind::Dict)
        return is_none(t->key) || is_none(t->val)
            || type_has_bottom_elem(t->key) || type_has_bottom_elem(t->val);

    return false;
}

/*
 * Enforce the generic `array`/`dict` explicit-type annotations: the symbol must
 * actually be an array / dict (its element/key/value types are inferred, so only
 * the kind is constrained here). The scalar annotations (bool/int/float/str) are
 * enforced inline in contribute() by pinning + assignability; this post-pass
 * covers the container kinds, whose element types only settle after the fixpoint.
 * `none`/`opt` of the right kind is accepted (an `opt array` may be none).
 */
void Inferencer::enforce_decl_types()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();

        if (s->ann != DeclType::arr && s->ann != DeclType::dict)
            continue;
        if (s->func || s->pinned || !s->decl_loc)
            continue;
        /* A PARAMETERIZED container is already pinned to its full type (kind +
         * element) via ann_scalar_static_type in reset_round/contribute, so the
         * kind-only check here is redundant - skip it. */
        if (s->ann_annot)
            continue;

        StaticTypeRef t = static_type_resolve(s->type);
        /* Defer on a not-yet-pinned/none/dyn type (a never-written `array a;` is
         * array<none> which IS an array; an unresolved one is tolerated). */
        if (t->kind == StaticTypeKind::None || t->kind ==
            StaticTypeKind::Unknown ||
            t->kind == StaticTypeKind::Dyn)
            continue;

        const StaticTypeKind want = s->ann == DeclType::arr ?
            StaticTypeKind::Array
                                                     : StaticTypeKind::Dict;
        if (t->kind != want)
            mismatch("'" + std::string(s->name->val) + "' is declared '" +
                         (s->ann == DeclType::arr ? "array" : "dict") +
                         "' but has type '" + static_type_to_string(s->type) +
                             "'",
                     s->decl_loc, s->decl_loc);
    }
}

/*
 * Enforce that every plain `var`/`const` declaration infers to a concrete
 * static type. A genuinely dynamic one must be declared `dyn`. `strict_deep`
 * selects the Phase: false (Phase A) enforces only a bare top-level `dyn`
 * (dynamic-element arrays are tolerated); true (Phase B) recurses into
 * containers.
 */
void Inferencer::enforce_concrete_decls()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();

        /* Skip: explicitly `dyn`, function names, struct type names (a type
         * descriptor, not a value), params (a never-called func's param is
         * legitimately `dyn` and has no `var` to annotate), foreach loop vars
         * (their type is derived from the container, which carries any `dyn`),
         * and builtins (no decl loc). */
        if (s->dyn_decl || s->func || s->struct_type || s->is_param ||
            s->is_loopvar || s->pinned || !s->decl_loc)
            continue;

        if (!type_has_dyn(s->type, strict_deep))
            continue;

        const std::string what = s->const_decl ? "const" : "variable";
        throw DynRequiredEx(
            intern_msg(
                std::string(s->name->val) + ": " + what +
                " has dynamic type '" + static_type_to_string(s->type) +
                "'; declare it 'dyn' (e.g. '" +
                (s->const_decl ? "const dyn " : "var dyn ") +
                std::string(s->name->val) + " = ...')"),
            s->decl_loc, s->decl_loc);
    }
}

/*
 * Mandatory-`opt` rule for parameters (the nullability analogue of
 * enforce_concrete_decls): a parameter that can receive `none` from some call
 * path must be declared `opt` (Phase B: even a `dyn` parameter - nullability is
 * orthogonal, so a nullable dyn must be `opt dyn`). `received_optish` was set
 * in the check pass when a possibly-none argument reached a non-opt param; here
 * we turn that into a compile error at the param's declaration, so nullability
 * is *proven* (a non-opt param is guaranteed never none) rather than only
 * checked per call site. Skips already-`opt` params and ones with no decl loc.
 * Runs after the check pass so a genuine type error surfaces first.
 */
void Inferencer::enforce_nonnull_params()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();

        if (!s->is_param || s->opt_decl || s->pinned || !s->decl_loc)
            continue;
        if (!s->received_optish)
            continue;

        const std::string kw = s->dyn_decl ? "opt dyn" : "opt";
        throw OptRequiredEx(
            intern_msg(
                "parameter '" + std::string(s->name->val) +
                "' may be none; declare it '" + kw + "' (e.g. '" + kw + " " +
                std::string(s->name->val) + "')"),
            s->decl_loc, s->decl_loc);
    }
}

/*
 * --debug-ti: dump every declared identifier's inferred type and use sites in a
 * machine-readable, tab-separated form. Used to audit the corpus for spurious
 * `dyn`s (plans/archived/type-driven-specialization.md). One `ti` record per symbol:
 *   ti<TAB>name<TAB>kind<TAB>line<TAB>col<TAB>const<TAB>type<TAB>uses
 * where kind is var|const|param|func, const is 0|1, type is the rendered static
 * type (int, opt float, array<int>, array<dyn>, func(int)->int, dyn, ...), and
 * uses is a comma-separated list of line:col occurrences (decl + reads/writes).
 */
void Inferencer::dump_debug_ti(std::ostream &os)
{
    std::unordered_map<const TypeSym *, std::vector<Loc>> uses;
    for (const auto &kv : id_sym) {
        if (kv.second && kv.first)
            uses[kv.second].push_back(kv.first->start);
    }

    os << "# ti\tname\tkind\tline\tcol\tconst\ttype\tuses(line:col,...)\n";

    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (!s->name)
            continue;

        const char *kind = s->func    ? "func"
                         : s->is_param ? "param"
                         : s->const_decl ? "const"
                         : "var";

        StaticTypeRef ty = s->func ? func_static_type(s->func) : s->type;

        os << "ti\t" << std::string(s->name->val)
           << "\t" << kind
           << "\t" << s->decl_loc.line
           << "\t" << s->decl_loc.col
           << "\t" << (s->const_decl ? 1 : 0)
           << "\t" << static_type_to_string(ty)
           << "\t";

        auto it = uses.find(s);
        if (it != uses.end()) {
            std::vector<Loc> &v = it->second;
            std::sort(v.begin(), v.end(), [](const Loc &a, const Loc &b) {
                return a.line != b.line ? a.line < b.line : a.col < b.col;
            });
            for (size_t i = 0; i < v.size(); i++) {
                if (i)
                    os << ",";
                os << v[i].line << ":" << v[i].col;
            }
        }
        os << "\n";
    }
}

void Inferencer::annotate_hints(Construct *n)
{
    if (!n)
        return;

    StaticTypeRef t = static_type_resolve(type_of(n));
    if (!t->opt) {
        /* bool is evaluated through the int (eval_int) path: it promotes to
         * 0/1, so a typed scalar over bool operands computes unboxed exactly
         * like int. The boxing in TypedScalarExpr::do_eval / LiteralBool keeps
         * the value bool where it must (a comparison/logical result, a bool
         * literal); arithmetic over bool correctly yields int. */
        if (t->kind == StaticTypeKind::Int || t->kind == StaticTypeKind::Bool)
            n->th = TypeHint::i;
        else if (t->kind == StaticTypeKind::Float)
            n->th = TypeHint::f;
    }

    if (auto *e = dynamic_cast<Expr14 *>(n))
        set_array_repr_hint(e);

    /* Mark `a[i]` whose base is statically an ARRAY, so the VM only compiles a
     * native element load/store for arrays (a dict subscript stays a fallback -
     * see Subscript::base_array). */
    if (auto *sub = dynamic_cast<Subscript *>(n)) {
        StaticTypeRef bt = static_type_resolve(type_of(sub->what.get()));
        sub->base_array = bt->kind == StaticTypeKind::Array;
        sub->base_dict = bt->kind == StaticTypeKind::Dict;
        sub->base_str = bt->kind == StaticTypeKind::Str && !bt->opt;
        if (sub->base_array && bt->elem) {
            StaticTypeRef et = static_type_resolve(bt->elem);
            sub->elem_bool = et->kind == StaticTypeKind::Bool && !et->opt;
        }
    }

    /* Lever 3: a slice over a statically-proven non-opt array/string can
     * only clamp, never throw (given int/absent bounds) - the hoist's
     * zero-iteration safety fact. */
    if (auto *sl = dynamic_cast<Slice *>(n)) {
        StaticTypeRef bt = static_type_resolve(type_of(sl->what.get()));
        sl->base_sliceable = !bt->opt
            && (bt->kind == StaticTypeKind::Array
                || bt->kind == StaticTypeKind::Str);
    }

    /* Mark `d.k` whose base is statically a DICT (vs a struct), so the VM
     * compiles a native typed dict read (P3) rather than a boxed MemberV. */
    if (auto *mem = dynamic_cast<MemberExpr *>(n)) {
        StaticTypeRef bt = static_type_resolve(type_of(mem->what.get()));
        mem->base_dict = bt->kind == StaticTypeKind::Dict;
        /* a non-opt struct instance base -> native field store (StoreMemberV) */
        mem->base_struct = bt->kind == StaticTypeKind::Struct && !bt->opt;
        mem->base_struct_def = mem->base_struct
            ? static_cast<const StructTypeDef *>(bt->struct_def) : nullptr;
        mem->field_slot = mem->base_struct_def && mem->memUid
            ? mem->base_struct_def->slot_of(mem->memUid) : -1;
    }

    /* VM native-call hint: the callee is a user FUNCTION (Func static type -
     * not a struct constructor / builtin). With a global-slot callee this lets
     * the VM emit a CallV instead of node->eval-ing the call. */
    if (auto *call = dynamic_cast<CallExpr *>(n)) {
        StaticTypeRef ct = static_type_resolve(type_of(call->what.get()));
        call->vm_direct_func = ct->kind == StaticTypeKind::Func;

        /*
         * Which arguments could the call INVOKE? Bit i is set when argument
         * i's static type is a `Func` or a `dyn` that might hold one - the
         * higher-order builtins (map/filter/sort/make_array/make_dict/find)
         * are exactly the calls that end up with a bit set. A caller that
         * needs the callback to be harmless (the LICM hoist) then checks
         * purity on just those arguments; everything else - `len(arr)`,
         * `abs(x)`, `str(v)` - reads as a mask of 0. Only the TYPE question
         * is answerable here: `effective_pure` is the resolver's, and the
         * resolver has not run yet.
         */
        uint32_t cmask = 0;
        const size_t na = call->args ? call->args->elems.size() : 0;
        if (na > 32) {
            cmask = ~0u;                /* no room to be precise: assume all */
        } else {
            for (size_t i = 0; i < na; i++) {
                StaticTypeRef at = static_type_resolve(
                    type_of(call->args->elems[i].get()));
                if (at->kind == StaticTypeKind::Func
                    || at->kind == StaticTypeKind::Dyn)
                    cmask |= 1u << i;
            }
        }
        call->callable_arg_mask = cmask;
        /* lever 4b: len(x)'s arg proven a non-opt array/string - the
         * BUILTIN-ness proof is codegen's (DirectBuiltinCallExpr + the
         * `len` uid), so this stamp alone triggers nothing. */
        if (call->args && call->args->elems.size() == 1) {
            StaticTypeRef at = static_type_resolve(
                type_of(call->args->elems[0].get()));
            if (!at->opt) {
                if (at->kind == StaticTypeKind::Array)
                    call->vm_len_kind = 1;
                else if (at->kind == StaticTypeKind::Str)
                    call->vm_len_kind = 2;
            }
        }
        /* A `dyn` callee is resolved at runtime (func / builtin / struct desc /
         * non-callable): the VM dispatches it generically (CallValueGenericV). */
        call->vm_dyn_callee = ct->kind == StaticTypeKind::Dyn;
    }

    /* Native-foreach hint: a single, non-indexed loop var over a flat
     * array<int>/array<float>. Only then may the VM iterate it as a counted
     * loop with a direct element load (a dict / string / general / tuple /
     * indexed foreach stays the tree-walker fallback). The loop var's own `th`
     * is NOT enough - a single var over a dict binds the KEY (also int), so the
     * container kind must be checked here where the static type is known. */
    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        /* A single-var array foreach (ids = [elem]) OR an INDEXED 2-var one
         * (ids = [index, elem]) can go native: elem_th is keyed off the ARRAY
         * ELEMENT type either way (ids[0] vs ids[1] is a codegen detail). */
        const bool single = !fe->indexed && fe->ids
                            && fe->ids->elems.size() == 1;
        const bool indexed2 = fe->indexed && fe->ids
                            && fe->ids->elems.size() == 2;
        if (single || indexed2) {
            StaticTypeRef c = static_type_resolve(type_of(fe->container.get()));
            if (c->kind == StaticTypeKind::Array) {
                StaticTypeRef el = static_type_resolve(c->elem);
                if (!el->opt && el->kind == StaticTypeKind::Int) {
                    fe->elem_th = TypeHint::i;          /* flat int fast path */
                    fe->container_is_array = true;
                } else if (!el->opt && el->kind == StaticTypeKind::Float) {
                    fe->elem_th = TypeHint::f;          /* flat float path */
                    fe->container_is_array = true;
                } else if (!el->opt && el->kind == StaticTypeKind::Bool) {
                    fe->elem_is_bool = true;            /* flat bool -> LoadElemBool */
                    fe->container_is_array = true;
                } else if (el->kind == StaticTypeKind::Str
                        || el->kind == StaticTypeKind::Array
                        || el->kind == StaticTypeKind::Dict
                        || el->kind == StaticTypeKind::Dyn) {
                    /* GENERAL (non-flat) storage: the VM binds each element's
                     * EvalValue into the loop var via LoadElemValue - box-free.
                     * Flat bool arrays are intentionally NOT here: their raw
                     * bool byte needs a scalar read, not LoadElemValue. */
                    fe->container_is_array = true;
                } else if (single && el->kind == StaticTypeKind::Struct
                           && el->struct_def) {
                    /* Flat array<PodStruct>: the loop var's SCALAR fields are
                     * read DIRECTLY from the array bytes (no per-iteration
                     * StructObject), IF the body uses `p` only as scalar-field
                     * reads - the codegen (try_native_struct_foreach) checks
                     * that, else falls back. Not container_is_array (the
                     * loop var is never bound to an element value). */
                    const StructTypeDef *sd =
                        static_cast<const StructTypeDef *>(el->struct_def);
                    if (sd->is_pod())
                        fe->container_struct_def = sd;
                }
            } else if (c->kind == StaticTypeKind::Str) {
                /* `foreach (c in s)` / indexed `foreach (i, c in s)`: the VM
                 * iterates chars with a counted loop (StrLen + LoadStrChar),
                 * each char a fresh 1-char string. */
                fe->container_is_str = true;
            }
        }

        /* A 1- or 2-var (key[+value]) foreach over a proven DICT -> the VM's
         * native live iterator (DictIterInit/DictIterNext). The map value is
         * always a boxed LValue, so both the key and the value bind the general
         * EvalValue box (box-free copy, like LoadElemValue) - no key/value
         * TypeHint is needed. The INDEXED form (`foreach (i, k[, v] in indexed
         * d)`) adds an int index counter, so it carries one extra var (ids.size
         * 2 or 3). A `dyn` container that can't be proven a dict is left to the
         * tree-walker fallback. */
        const int dict_off = fe->indexed ? 1 : 0;
        const int dict_nkv = fe->ids
            ? static_cast<int>(fe->ids->elems.size()) - dict_off : 0;
        const bool dict_form = fe->ids && (dict_nkv == 1 || dict_nkv == 2);
        if (dict_form) {
            StaticTypeRef c = static_type_resolve(type_of(fe->container.get()));
            if (c->kind == StaticTypeKind::Dict)
                fe->container_is_dict = true;
        }

        /* A foreach over a DYN container (can't prove array OR dict): the
         * VM's runtime-dispatching ForeachDyn iterator, GENERAL over the id
         * list - any var count, the `indexed` form, `_` placeholders. It
         * dispatches at runtime and binds exactly as do_iter: an array
         * element (single var) or its STRICT unpack (multi-var); a dict key
         * [, value [, none-padded]]; `indexed` counts in ids[0]. */
        if (fe->ids && !fe->ids->elems.empty()
            && !fe->container_is_array && !fe->container_is_dict) {
            StaticTypeRef c = static_type_resolve(type_of(fe->container.get()));
            if (c->kind == StaticTypeKind::Dyn)
                fe->container_is_dyn = true;
        }

        /* A STRICT-unpack foreach over a proven array<array<...>>: the VM
         * iterates the outer array counted and destructures each sub-array into
         * the consecutive loop-var slots. A flat int/float sub-array reads its
         * scalars BOX-FREE (UnpackElemInt/Float); any other sub-array (general /
         * dyn / str / mixed, incl. shopping's [str, float]) binds each element's
         * value box-free (UnpackElemValue). The `indexed` form counts the index
         * var as the counter, so the unpack width is elems - (indexed?1:0), which
         * must be >= 2. The runtime size check keeps it strict (arrays vary, so
         * the width is not matched statically). A `_`/non-local target bails in
         * codegen; only the outer-is-array-of-arrays proof is here. */
        const int nunpack = fe->ids
            ? static_cast<int>(fe->ids->elems.size()) - (fe->indexed ? 1 : 0)
            : 0;
        if (fe->ids && nunpack >= 2) {
            StaticTypeRef c = static_type_resolve(type_of(fe->container.get()));
            if (c->kind == StaticTypeKind::Array) {
                StaticTypeRef el = static_type_resolve(c->elem);
                if (!el->opt && el->kind == StaticTypeKind::Array) {
                    StaticTypeRef sub = static_type_resolve(el->elem);
                    if (!fe->indexed && !sub->opt
                        && sub->kind == StaticTypeKind::Int)
                        fe->unpack_elem_th = TypeHint::i;
                    else if (!fe->indexed && !sub->opt
                             && sub->kind == StaticTypeKind::Float)
                        fe->unpack_elem_th = TypeHint::f;
                    else
                        fe->unpack_elem_value = true;
                }
            }
        }
    }

    for_each_child(n, [&](Construct *c) { annotate_hints(c); });
}

/*
 * Type-driven array representation. For `a = <array-producing rvalue>` (decl or
 * plain assign) where `a`'s inferred type is an array, stamp the rvalue with
 * the representation its PROVEN element type demands, so the array is built in
 * final representation at creation - never promoted later:
 *   - non-null int element  -> ArrHint::flat_i  (flat unboxed int storage)
 *   - non-null float element -> ArrHint::flat_f (flat unboxed float storage)
 *   - anything else (array<dyn>, opt element, nested containers) -> general.
 * The hint goes on a range()/array()/make_array() call's args ExprList (which
 * the builtin reads) or directly on an array literal / folded LiteralObj. The
 * fixpoint propagates `a`'s type through direct aliases (`var b = a`), so they
 * agree on representation. Runs in annotate_hints (types final), the
 * inferencer's one AST-mutation point. This replaces the old array(N) autofill:
 * a flat_i/flat_f hint makes array(N) born flat (0 / 0.0 fill), no rewrite.
 */
void Inferencer::set_array_repr_hint(Expr14 *e)
{
    if (e->op != Op::assign)
        return;

    auto *id = dynamic_cast<Identifier *>(e->lvalue.get());
    if (!id)
        return;
    auto it = id_sym.find(id);
    if (it == id_sym.end() || !it->second)
        return;
    StaticTypeRef ty = static_type_resolve(it->second->type);

    /*
     * A `dyn`-typed destination (`var dyn d = ...`) means "I want a polymorphic
     * array", so build it general - otherwise `var dyn d = [1,2,3]; d[0]="x"`
     * would wrongly hit the flat-array error even though `d` is already dyn.
     * Anything that is neither an array nor `dyn` has no array repr to pick.
     */
    ArrHint hint;
    if (ty->kind == StaticTypeKind::Array) {
        StaticTypeRef el = static_type_resolve(ty->elem);
        if (!el->opt && el->kind == StaticTypeKind::Int)
            hint = ArrHint::flat_i;
        else if (!el->opt && el->kind == StaticTypeKind::Float)
            hint = ArrHint::flat_f;
        else if (!el->opt && el->kind == StaticTypeKind::Bool)
            hint = ArrHint::flat_b;
        else if (!el->opt && el->kind == StaticTypeKind::Struct &&
                 static_cast<const StructTypeDef *>(el->struct_def) &&
                 static_cast<const StructTypeDef *>(el->struct_def)->is_pod())
            /* array<POD struct>: flat storage (the def lets even an empty
             * `[]` start flat); a boxed-struct array stays general. */
            hint = ArrHint::flat_s;
        else if (!el->opt && el->kind == StaticTypeKind::Str)
            /* array<str> (top-10 #7): NO hint - the value keeps its natural
             * storage (a split()/keys() result stays FLAT strs; a plain
             * general literal stays general). Forcing `general` here used
             * to general-ify a baked flat-strs literal; the strs promote-
             * on-write model makes a later non-string element write safe
             * without it. */
            return;
        else
            hint = ArrHint::general;
    } else if (ty->kind == StaticTypeKind::Dyn) {
        hint = ArrHint::general;
    } else {
        return;
    }

    if (trace_enabled(TraceCat::arrays)) {
        const char *hn = hint == ArrHint::flat_i ? "flat (ints)"
                       : hint == ArrHint::flat_f ? "flat (floats)"
                       : hint == ArrHint::flat_b ? "flat (bools)"
                       : hint == ArrHint::flat_s ? "flat (structs)"
                                                 : "general";
        TRACE(arrays, 0, std::string(id->get_str()) + "  dest " +
              static_type_to_string(ty) + " -> " + hn);
    }

    /* the element struct type, needed by an empty flat_s array literal */
    const StructTypeDef *sdef =
        hint == ArrHint::flat_s
            ? static_cast<const StructTypeDef *>(
                  static_type_resolve(ty->elem)->struct_def)
            : nullptr;

    Construct *rv = e->rvalue.get();

    if (auto *call = dynamic_cast<CallExpr *>(rv)) {

        auto *cid = dynamic_cast<Identifier *>(call->what.get());
        if (!cid || !call->args)
            return;
        /* must be the builtin, not a user function of the same name */
        auto sit = id_sym.find(cid);
        if ((sit != id_sym.end() && sit->second) || !is_builtin(cid->uid))
            return;
        const std::string nm(cid->uid->val);
        /* keys()/values() honor a flat hint too: building keys/values of a
         * scalar dict into a flat array<int>/<float>/<bool> avoids per-element
         * boxing (a big win for keys()/values() of a large dict). */
        if (nm == "range" || nm == "array" || nm == "make_array" ||
            nm == "keys" || nm == "values") {
            call->args->arr_hint = hint;
            call->args->arr_hint_struct = sdef;
        }

    } else {

        /* an array literal or a folded const literal (LiteralObj) */
        rv->arr_hint = hint;
        rv->arr_hint_struct = sdef;
    }
}

void Inferencer::hoist_globals(Block *rootBlock)
{
    for (auto &e : rootBlock->elems) {
        Construct *n = e.get();
        if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
            declare_funcdecl(fd, global);
        } else if (auto *sd = dynamic_cast<StructDeclStmt *>(n)) {
            declare_structdecl(sd, global);
        } else if (auto *e14 = dynamic_cast<Expr14 *>(n)) {
            if (e14->fl & pFlags::pInDecl)
                declare_target(e14->lvalue.get(), global,
                               e14->fl & pFlags::pInConstDecl);
        }
    }
}

/*
 * Declare a struct type name (a type descriptor symbol) and record the def by
 * name so struct-typed fields/annotations resolve. A bare struct-name value is
 * typed `dyn`; its constructor / `.CONST` uses are intercepted before that.
 */
void Inferencer::declare_structdecl(StructDeclStmt *sd, Scope *s)
{
    StructTypeDef *def = sd->def;
    struct_by_name[def->name] = def;

    if (sd->id) {
        const UniqueId *nm = sd->id->uid;
        auto it = s->syms.find(nm);
        TypeSym *sym = (it != s->syms.end()) ? it->second
                                             : new_sym(nm, s, sd->start);
        sym->struct_type = def;
        sym->type = A.dyn_ty();
        id_sym[sd->id.get()] = sym;
    }
}

/* The static type of one struct field (an array/dict field is generic - its
 * element/key/value are `dyn`, since v1 has no inference inside a struct). */
/* Convert a parsed recursive TypeAnnot (array<int>, dict<str, Point>, ...) into
 * an StaticType, applying the `?` nullable flag at each level. */
StaticTypeRef Inferencer::annot_to_static_type(const TypeAnnot *ta)
{
    if (!ta)
        return A.dyn_ty();

    StaticTypeRef base;
    switch (ta->kind) {
        case DeclType::b:   base = A.bool_ty();  break;
        case DeclType::i:   base = A.int_ty();   break;
        case DeclType::f:   base = A.float_ty(); break;
        case DeclType::s:   base = A.str_ty();   break;
        case DeclType::dyn: base = A.dyn_ty();   break;
        case DeclType::strct:
            base = ta->strct ? A.struct_ty(ta->strct, ta->strct->name)
                             : A.dyn_ty();
            break;
        case DeclType::arr:
            base = A.array_of(annot_to_static_type(ta->elem.get()));
            break;
        case DeclType::dict:
            base = A.dict_of(annot_to_static_type(ta->key.get()),
                             annot_to_static_type(ta->val.get()));
            break;
        default: base = A.dyn_ty();
    }
    return A.with_opt(base, ta->opt);
}

StaticTypeRef Inferencer::field_static_type(const FieldDef &fd)
{
    StaticTypeRef base;

    /* A parameterized container field (`array<int> xs`) carries its element
     * type in `annot`; a generic `array`/`dict` field leaves it `dyn`. */
    if (fd.annot &&
        (fd.kind == FieldKind::f_array || fd.kind == FieldKind::f_dict)) {
        base = annot_to_static_type(fd.annot.get());
        return fd.is_opt ? A.with_opt(base, true) : base;
    }

    switch (fd.kind) {
        case FieldKind::f_bool:   base = A.bool_ty();  break;
        case FieldKind::f_int:    base = A.int_ty();   break;
        case FieldKind::f_float:  base = A.float_ty(); break;
        case FieldKind::f_str:    base = A.str_ty();   break;
        case FieldKind::f_array:  base = A.array_of(A.dyn_ty()); break;
        case FieldKind::f_dict:
            base = A.dict_of(A.dyn_ty(), A.dyn_ty()); break;
        case FieldKind::f_dyn:    base = A.dyn_ty();   break;
        case FieldKind::f_struct: {
            auto it = struct_by_name.find(fd.struct_ty);
            base = (it != struct_by_name.end())
                       ? A.struct_ty(it->second, fd.struct_ty)
                       : A.dyn_ty();    /* unknown struct type: defer to dyn */
            break;
        }
        default: base = A.dyn_ty();
    }

    return fd.is_opt ? A.with_opt(base, true) : base;
}

void Inferencer::declare_funcdecl(FuncDeclStmt *fd, Scope *s)
{
    if (func_of_decl.count(fd))
        return;

    all_funcs.push_back(std::make_unique<FuncInfo>());
    FuncInfo *fi = all_funcs.back().get();
    fi->decl = fd;
    fi->ret = bottom;
    fi->ret_acc = bottom;
    fi->falls_through =
        fd->body && fd->body->is_block() && !always_exits(fd->body.get());
    /*
     * A template iff it is a NAMED function with at least one *template
     * parameter*: an un-annotated, non-`dyn`, non-`opt` param. Those are the
     * params the instance signature is keyed by; any `opt`/typed/`dyn` params
     * coexist and just `join` within each clone (handled by the concrete path).
     * So `func f(a, opt b)` is a template over `a` (b joins), while `func
     * f(opt x)` (no template param) keeps the join model. (A lambda has no name
     * to redirect a call to, so it also keeps the join model - v1; see
     * plans/archived/function-templates.md.)
     */
    if (fd->id && fd->params)
        for (auto &p : fd->params->elems)
            if (!p->opt_mod && !p->dyn_mod && p->decl_type == DeclType::none) {
                fi->is_template = true;
                break;
            }
    func_of_decl[fd] = fi;

    if (fd->id) {
        const UniqueId *nm = fd->id->uid;
        TypeSym *sym;
        auto it = s->syms.find(nm);
        sym = (it != s->syms.end()) ? it->second : new_sym(nm, s, fd->start);
        sym->func = fi;
        id_sym[fd->id.get()] = sym;
    }
}

void Inferencer::declare_target(Construct *lvalue, Scope *s, bool is_const)
{
    auto decl_one = [&](Identifier *id) {
        const UniqueId *nm = id->uid;
        auto it = s->syms.find(nm);
        TypeSym *sym = (it != s->syms.end()) ? it->second
                                             : new_sym(nm, s, id->start);
        sym->opt_decl = sym->opt_decl || id->opt_mod;
        sym->dyn_decl = sym->dyn_decl || id->dyn_mod;
        sym->const_decl = sym->const_decl || is_const;
        if (id->decl_type != DeclType::none) {
            sym->ann = id->decl_type;
            sym->ann_struct = id->decl_struct;   /* set when ann == strct */
            sym->ann_annot = id->decl_annot;     /* set when parameterized */
        }
        sym->decl_id = id;       /* for a post-inference decl_type stamp
                                  * (dyn-into-concrete coercion) */
        id_sym[id] = sym;        /* the decl write is counted in walk_struct
                                  * (declare_target runs twice via hoist) */
    };

    if (auto *id = dynamic_cast<Identifier *>(lvalue))
        decl_one(id);
    else if (auto *idl = dynamic_cast<IdList *>(lvalue))
        for (auto &up : idl->elems)
            if (!up->is_underscore())   /* `_` placeholder: not declared */
                decl_one(up.get());
}

void Inferencer::walk_struct(Construct *n, Scope *s)
{
    if (!n)
        return;

    if (auto *blk = dynamic_cast<Block *>(n)) {
        Scope *inner = new_scope(s);

        /*
         * PRE-DECLARE this block's NAMED func/struct declarations (#134/#136).
         * They bind at SCOPE ENTRY at run time (bind_hoistable_decls /
         * gen_stmts' decls-first emission), and the RESOLVER already hoists
         * them (hoist_scoped_decls) - the inferencer was the one pass that
         * still saw them in statement order, and only INSIDE a block:
         * hoist_globals does exactly this for the root.
         *
         * That asymmetry was the bug. A call above the declaration got a
         * callee whose static type had not settled to `Func`, so
         * annotate_hints left `vm_direct_func` false and the codegen REFUSED
         * to lower the call (NotLoweredEx) while the tree-walker ran it - and
         * a struct used above its `struct` line typed as `none`, so `p.x`
         * became a bogus NullabilityEx. Measurable as `typestr(inner)`:
         * "func inner()" before the decl vs "func()->int" after it, where the
         * top level said "func()->int" in both positions.
         *
         * Both declare_* are idempotent (declare_funcdecl returns early when
         * func_of_decl already has the node; declare_structdecl re-finds the
         * existing sym), so the in-order walk below is a no-op for these.
         */
        for (auto &e : blk->elems) {
            if (auto *fd = dynamic_cast<FuncDeclStmt *>(e.get())) {
                if (fd->id)
                    declare_funcdecl(fd, inner);
            } else if (auto *sd = dynamic_cast<StructDeclStmt *>(e.get())) {
                if (sd->id)
                    declare_structdecl(sd, inner);
            }
        }

        for (auto &e : blk->elems)
            walk_struct(e.get(), inner);
        return;
    }

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        declare_funcdecl(fd, s);
        FuncInfo *fi = func_of_decl[fd];
        Scope *fscope = new_scope(global);   /* funcs see globals only */

        /* tag this function's params/locals if it is a template BASE (see
         * struct_tmpl_depth / TypeSym::in_template); not an instance clone */
        const bool is_tmpl = fi && fi->is_template && !struct_in_clone;
        if (is_tmpl)
            struct_tmpl_depth++;

        if (fd->captures)
            for (auto &cap : fd->captures->elems) {
                TypeSym *outer = lookup(s, cap->uid);
                if (outer)
                    fscope->syms[cap->uid] = outer;
                id_sym[cap.get()] = outer;
            }

        if (fd->params)
            for (auto &p : fd->params->elems) {
                TypeSym *psym = new_sym(p->uid, fscope, p->start);
                psym->is_param = true;
                psym->opt_decl = p->opt_mod;
                psym->dyn_decl = p->dyn_mod;
                psym->ann = p->decl_type;
                psym->ann_struct = p->decl_struct;   /* set when ann == strct */
                psym->ann_annot = p->decl_annot;     /* if parameterized */
                id_sym[p.get()] = psym;
                fi->params.push_back(psym);
            }

        walk_struct(fd->body.get(), fscope);
        if (is_tmpl)
            struct_tmpl_depth--;
        return;
    }

    if (auto *e14 = dynamic_cast<Expr14 *>(n)) {
        walk_struct(e14->rvalue.get(), s);   /* RHS before name exists */
        /* count a write to each target name (decl or assign), once - this walk
         * runs once per node (declare_target runs twice via hoist). */
        auto count_write = [&](Construct *lv) {
            if (auto *lid = dynamic_cast<Identifier *>(lv)) {
                auto it = id_sym.find(lid);
                if (it != id_sym.end() && it->second)
                    it->second->writes++;
            }
        };
        if (e14->fl & pFlags::pInDecl) {
            declare_target(e14->lvalue.get(), s,
                           e14->fl & pFlags::pInConstDecl);
            /* `var f = <lambda>`: bind f to the lambda's FuncInfo so calls to f
             * type the lambda's params and check its arity. */
            auto *id = dynamic_cast<Identifier *>(e14->lvalue.get());
            auto *fd = dynamic_cast<FuncDeclStmt *>(e14->rvalue.get());
            if (id && fd) {
                TypeSym *sym = id_sym[id];
                if (sym && !sym->func)
                    sym->func = func_of_decl[fd];
            }
        } else {
            walk_struct(e14->lvalue.get(), s);
        }
        if (auto *idl = dynamic_cast<IdList *>(e14->lvalue.get()))
            for (auto &up : idl->elems) count_write(up.get());
        else
            count_write(e14->lvalue.get());
        return;
    }

    if (auto *call = dynamic_cast<CallExpr *>(n)) {
        /* The callee is a CALL use (not a value use); a bare-Identifier callee
         * resolves here without being marked value_used, so a var used only to
         * be called stays monomorphizable. Args are value uses. */
        if (auto *cid = dynamic_cast<Identifier *>(call->what.get())) {
            id_sym[cid] = lookup(s, cid->uid);   /* always - see Identifier */
        } else {
            walk_struct(call->what.get(), s);
        }
        walk_struct(call->args.get(), s);
        return;
    }

    if (auto *fs = dynamic_cast<ForStmt *>(n)) {
        Scope *inner = new_scope(s);
        walk_struct(fs->init.get(), inner);
        walk_struct(fs->cond.get(), inner);
        walk_struct(fs->inc.get(), inner);
        walk_struct(fs->body.get(), inner);
        return;
    }

    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        Scope *inner = new_scope(s);
        walk_struct(fe->container.get(), s);
        if (fe->ids)
            for (auto &id : fe->ids->elems) {
                if (id->is_underscore())
                    continue;   /* `_` placeholder: not declared, no shadow */
                /* A loop var written WITHOUT `var` may not shadow a variable
                 * visible outside the loop (a subtle-bug guard) - it is looked
                 * up in the ENCLOSING scope `s` (the fresh loop scope `inner`
                 * is still empty), and if found the loop is rejected; write
                 * `var` to shadow on purpose. */
                if (fe->implicit_var && lookup(s, id->uid))
                    throw ShadowingEx(
                        intern_msg("foreach variable '" +
                            std::string(id->uid->val) + "' shadows a variable "
                            "visible here; write 'var " +
                            std::string(id->uid->val) + "' to shadow it"),
                        id->start, id->end);
                TypeSym *sym = new_sym(id->uid, inner, id->start);
                sym->is_loopvar = true;   /* type derived from container */
                id_sym[id.get()] = sym;
            }
        walk_struct(fe->body.get(), inner);
        return;
    }

    if (auto *tc = dynamic_cast<TryCatchStmt *>(n)) {
        walk_struct(tc->tryBody.get(), s);
        for (auto &cs : tc->catchStmts) {
            Scope *inner = new_scope(s);
            if (cs.first.asId) {
                TypeSym *sym = new_sym(cs.first.asId->uid, inner,
                                       cs.first.asId->start);
                sym->type = A.exc_ty();
                sym->dyn_decl = true;   /* exception payload is dynamic */
                id_sym[cs.first.asId.get()] = sym;
            }
            walk_struct(cs.second.get(), inner);
        }
        walk_struct(tc->finallyBody.get(), s);
        return;
    }

    if (auto *sd = dynamic_cast<StructDeclStmt *>(n)) {
        declare_structdecl(sd, s);
        return;
    }

    if (auto *id = dynamic_cast<Identifier *>(n)) {
        /* ALWAYS (re)resolve - never `if (!id_sym.count(id))`. id_sym is keyed
         * by node pointer and persists for the session; a fresh node can reuse
         * a freed node's address, so a stale entry must be OVERWRITTEN, not
         * kept (keeping it bound a callee to a prior clone -> the bug).
         * walk_struct visits each node once, so this never double-resolves. */
        id_sym[id] = lookup(s, id->uid);
        /* reached for every reference EXCEPT a call callee (handled above): a
         * value (non-callee) use. */
        if (TypeSym *sym = id_sym[id])
            sym->value_used = true;
        return;
    }

    for_each_child(n, [&](Construct *c) { walk_struct(c, s); });
}

/* ----------------------- named-argument desugaring ----------------------- */

/*
 * Lower ONE named-argument call to positional. Runs once, right after the
 * structural pass and before everything else, so resolve_names, the optimizers
 * and eval only ever see positional calls - named args have provably zero
 * effect on them. This is the inferencer's adapter: it resolves the callee
 * (names require a directly-named function - a dyn/func-value/builtin callee is
 * the error here), then hands a normalized ParamSpec view to the shared
 * desugar_named_call (syntax.cpp), which owns the mapping/ordering/none-filling
 * rules and is also used by the parser's const-fold path.
 */
void Inferencer::lower_call_named_args(CallExpr *call)
{
    if (call->args->arg_names.empty())
        return;                               /* all positional */

    /* Struct construction: the "parameters" are the fields, in order. */
    if (auto *cid = dynamic_cast<Identifier *>(call->what.get())) {
        auto it = id_sym.find(cid);
        if (it != id_sym.end() && it->second && it->second->struct_type) {
            const StructTypeDef *def = it->second->struct_type;
            std::vector<ParamSpec> params;
            for (const auto &f : def->fields)
                params.push_back({ f.name, f.is_opt });
            desugar_named_call(call, params);
            return;
        }
    }

    FuncInfo *fi = callee_funcinfo(call->what.get());
    if (!fi)
        mismatch("named arguments require a directly-named function",
                 call->what->start, call->what->end);

    std::vector<ParamSpec> params;
    for (TypeSym *p : fi->params)
        params.push_back({ p->name, p->opt_decl });

    desugar_named_call(call, params);
}

/* Walk the tree and lower every named-argument call (post-order: nested calls
 * in the arguments are lowered before the call that contains them). */
void Inferencer::lower_named_args(Construct *n)
{
    if (!n)
        return;

    for_each_child(n, [this](Construct *c) { lower_named_args(c); });

    if (auto *call = dynamic_cast<CallExpr *>(n))
        lower_call_named_args(call);
}

/*
 * Reject a call to a DEV-ONLY builtin (show()) in a SCRIPT - such builtins need
 * the AST and are reserved to the dev harnesses (REPL / tests), so a script use
 * is a compile-time error. Runs in the structural pass (so it fires even under
 * -nti); the caller gates it on !g_dev_builtins_allowed, so it is a no-op in the
 * REPL / test runner. A user symbol of the same name (a `func show(){}` shadow)
 * has a non-null id_sym entry and is left alone - it is the user's function.
 */
/*
 * `isbound` takes ONLY an identifier (maintainer's call): `none` and `len`
 * are identifiers, `3` and `"blah"` are not. A SYNTACTIC rule, so it runs
 * ungated - not inside reject_dev_builtins (which the test runner and the
 * REPL skip wholesale) and not behind `checks_enabled` (`-nti` must not turn
 * it off), exactly like the named-argument lowering beside it.
 *
 * It has to be a COMPILE error rather than the runtime body's own throw: the
 * tree-walker would answer TypeErrorEx while the no-fail codegen refuses to
 * lower the shape at all (NotLoweredEx) - an engine divergence on top of a
 * worse diagnostic.
 */
void Inferencer::check_isbound_args(Construct *n)
{
    if (!n)
        return;

    if (auto *call = dynamic_cast<CallExpr *>(n)) {
        if (auto *cid = dynamic_cast<Identifier *>(call->what.get())) {
            auto it = id_sym.find(cid);
            const bool user_shadow = (it != id_sym.end() && it->second);
            if (!user_shadow && cid->uid->val == "isbound") {
                const size_t n_args =
                    call->args ? call->args->elems.size() : 0;
                if (n_args != 1)
                    throw WrongArgCountEx(
                        intern_msg("isbound() takes exactly 1 argument"),
                        cid->start, cid->end);
                /* `none` is name-SHAPED but parses to a LiteralNone, not an
                 * Identifier - let it through and let try_fold_isbound
                 * answer `true`. */
                Construct *ba = call->args->elems[0].get();
                if (!dynamic_cast<Identifier *>(ba)
                        && !dynamic_cast<LiteralNone *>(ba))
                    throw TypeMismatchEx(
                        intern_msg("isbound() takes an identifier"),
                        ba->start, ba->end);
            }
        }
    }

    for_each_child(n, [this](Construct *c) { check_isbound_args(c); });
}

void Inferencer::reject_dev_builtins(Construct *n)
{
    if (!n)
        return;

    if (auto *call = dynamic_cast<CallExpr *>(n)) {
        if (auto *cid = dynamic_cast<Identifier *>(call->what.get())) {
            auto it = id_sym.find(cid);
            const bool user_shadow = (it != id_sym.end() && it->second);
            if (!user_shadow && is_dev_builtin(cid->uid))
                throw SyntaxErrorEx(cid->start,
                    intern_msg("'" + std::string(cid->uid->val) +
                        "' is a dev-only builtin (REPL / tests only); it is not "
                        "available in a script"));
            /* The CALLEE position is the one LEGAL use of a LAZY-ARG builtin
             * (defined/isbound/isconst/isconstdecl): walk the args only,
             * skipping the callee id, so the value-use check below doesn't
             * fire on it. */
            reject_dev_builtins(call->args.get());
            return;
        }
    }

    /* A LAZY-ARG builtin used as a VALUE (`var dyn f = defined;`,
     * `[isconst]`, an argument, ...): its semantics need the UNEVALUATED
     * argument node, which an indirect (dyn) call can never supply - a script
     * compile error (fork F1, maintainer-decided). A user symbol shadowing
     * the name is untouched; the REPL (g_dev_builtins_allowed) keeps the
     * indirect form working via its retained AST. */
    if (auto *id = dynamic_cast<Identifier *>(n)) {
        auto it = id_sym.find(id);
        const bool user_shadow = (it != id_sym.end() && it->second);
        if (!user_shadow && is_lazy_builtin(id->uid))
            throw SyntaxErrorEx(id->start,
                intern_msg("'" + std::string(id->uid->val) +
                    "' takes an unevaluated argument; it cannot be used as a "
                    "value (only called directly)"));
    }

    for_each_child(n, [this](Construct *c) { reject_dev_builtins(c); });
}

/* --------------------------- type computation ---------------------------- */

StaticTypeRef Inferencer::func_static_type(FuncInfo *fi)
{
    std::vector<StaticTypeRef> ps;
    std::vector<bool> popt;
    for (TypeSym *p : fi->params) {
        ps.push_back(p->dyn_decl ? A.dyn_ty() : p->type);
        popt.push_back(p->opt_decl);
    }
    StaticTypeRef t = A.func_of(ps, popt, fi->ret);
    if (fi->is_template)
        t->finfos.push_back(fi);   /* value-template tracking (see the plan) */
    return t;
}

/*
 * Derive the exact static type of a baked const value (a folded const array/
 * dict literal), recursing into its elements. A const value is immutable and
 * fully known at compile time, so element types are not discarded: a
 * homogeneous const array is array<T>, a heterogeneous one array<dyn> (its
 * individual elements are still exact via const-folding of any constant-index
 * access at parse time). Same for dict keys/values.
 */
StaticTypeRef Inferencer::static_type_from_value(const EvalValue &v)
{
    Type *t = v.get_type();
    switch (t->t) {

        case Type::t_bool:  return A.bool_ty();
        case Type::t_int:   return A.int_ty();
        case Type::t_float: return A.float_ty();
        case Type::t_str:   return A.str_ty();
        case Type::t_none:  return A.none_ty();

        case Type::t_struct: {
            const StructTypeDef *def =
                v.get<intrusive_ptr<StructObject>>()->def;
            return A.struct_ty(def, def->name);
        }

        case Type::t_arr: {
            const SharedArrayObj &arr = v.get<SharedArrayObj>();

            /*
             * Flat (unboxed) storage already pins the element type, so read it
             * from the kind - crucially WITHOUT get_view(), which would promote
             * the const value to general (see plans/archived/typed-arrays.md).
             */
            if (arr.skind() == SharedArrayObj::Storage::bools)
                return A.array_of(A.bool_ty());
            if (arr.skind() == SharedArrayObj::Storage::ints)
                return A.array_of(A.int_ty());
            if (arr.skind() == SharedArrayObj::Storage::floats)
                return A.array_of(A.float_ty());
            if (arr.skind() == SharedArrayObj::Storage::structs) {
                const StructTypeDef *def = arr.flat_structs().def;
                return A.array_of(A.struct_ty(def, def->name));
            }
            if (arr.skind() == SharedArrayObj::Storage::strs)
                return A.array_of(A.str_ty());

            ArrayConstView view = arr.get_view();
            if (view.size() == 0)
                return A.array_of(A.none_ty());
            StaticTypeRef el = static_type_from_value(view[0].get());
            for (size_type i = 1; i < view.size(); i++) {
                StaticTypeRef j = A.join(el,
                    static_type_from_value(view[i].get()));
                el = j ? j : A.dyn_ty();
            }
            return A.array_of(el);
        }

        case Type::t_dict: {
            const auto &m = v.get<intrusive_ptr<DictObject>>()->get_ref();
            if (m.empty())
                return A.dict_of(A.none_ty(), A.none_ty());
            StaticTypeRef k = nullptr, val = nullptr;
            for (const auto &kv : m) {
                StaticTypeRef kt = static_type_from_value(kv.first);
                StaticTypeRef vt = static_type_from_value(kv.second.get());
                if (!k) {
                    k = kt; val = vt;
                } else {
                    StaticTypeRef jk = A.join(k, kt);
                    StaticTypeRef jv = A.join(val, vt);
                    k = jk ? jk : A.dyn_ty();
                    val = jv ? jv : A.dyn_ty();
                }
            }
            return A.dict_of(k, val);
        }

        default:
            return A.dyn_ty();
    }
}

StaticTypeRef Inferencer::type_of(const Construct *e)
{
    if (!e)
        return A.none_ty();

    if (dynamic_cast<const LiteralBool *>(e))  return A.bool_ty();
    if (dynamic_cast<const LiteralInt *>(e))   return A.int_ty();
    if (dynamic_cast<const LiteralFloat *>(e)) return A.float_ty();
    if (dynamic_cast<const LiteralStr *>(e))   return A.str_ty();
    if (dynamic_cast<const LiteralNone *>(e))  return A.none_ty();

    if (auto *la = dynamic_cast<const LiteralArray *>(e)) {
        if (la->elems.empty())
            return A.array_of(A.none_ty());
        StaticTypeRef el = type_of(la->elems[0].get());
        for (size_t i = 1; i < la->elems.size(); i++) {
            StaticTypeRef j = A.join(el, type_of(la->elems[i].get()));
            el = j ? j : A.dyn_ty();
        }
        return A.array_of(el);
    }

    if (auto *ld = dynamic_cast<const LiteralDict *>(e)) {
        if (ld->elems.empty())
            return A.dict_of(A.none_ty(), A.none_ty());
        StaticTypeRef k = type_of(ld->elems[0]->key.get());
        StaticTypeRef val = type_of(ld->elems[0]->value.get());
        for (size_t i = 1; i < ld->elems.size(); i++) {
            StaticTypeRef jk = A.join(k, type_of(ld->elems[i]->key.get()));
            StaticTypeRef jv = A.join(val, type_of(ld->elems[i]->value.get()));
            k = jk ? jk : A.dyn_ty();
            val = jv ? jv : A.dyn_ty();
        }
        return A.dict_of(k, val);
    }

    if (auto *lo = dynamic_cast<const LiteralObj *>(e))
        return static_type_from_value(lo->literal_value());

    if (auto *id = dynamic_cast<const Identifier *>(e)) {
        auto it = id_sym.find(id);
        if (it != id_sym.end() && it->second) {
            TypeSym *s = it->second;
            if (s->func)
                return func_static_type(s->func);
            /* flow narrowing: in an `if (x != none)` branch, x is non-opt */
            if (narrowing_on) {
                auto nit = narrowed.find(s);
                if (nit != narrowed.end())
                    return nit->second;
            }
            return s->type;
        }
        /* `argv` is a runtime-injected global: script args, array<str>. */
        if (std::string(id->uid->val) == "argv")
            return A.array_of(A.str_ty());
        /*
         * Not a declared symbol: a builtin used as a value is genuinely dynamic
         * (a function-ish handle), but a truly-undefined identifier defers to
         * Unknown - it is an UndefinedVariableEx at runtime, and typing it dyn
         * would make the enclosing `var` spuriously require `dyn` and mask that
         * runtime error.
         */
        if (is_builtin(id->uid))
            return A.dyn_ty();
        return bottom;
    }

    if (auto *e1 = dynamic_cast<const Expr01 *>(e))
        return type_of(e1->elem.get());

    if (auto *e2 = dynamic_cast<const Expr02 *>(e)) {
        const auto &pr = e2->elems[0];
        return unary_result(pr.first, type_of(pr.second.get()));
    }

    if (auto *mo = dynamic_cast<const MultiOpConstruct *>(e)) {
        /* Expr03/04/06/07/11/12 */
        StaticTypeRef t = type_of(mo->elems[0].second.get());
        for (size_t i = 1; i < mo->elems.size(); i++)
            t = binop_result(mo->elems[i].first, t,
                             type_of(mo->elems[i].second.get()));
        return t;
    }

    /*
     * A TypedScalarExpr (M8) - normally inference runs BEFORE specialization, so
     * type_of never meets one; but a REPL RETAINS a prior input's
     * POST-specialization body, and a template instance CLONED from it in a
     * later input re-enters inference (like the inliner's cross-input
     * TypedScalarExpr handling). Its result type is known from cat/kind: a
     * comparison / logical / `!` yields bool, arithmetic / negation the operand
     * `kind` (int or float). Without this, such a node fell through to `dyn`,
     * spuriously forcing an accumulator var to `dyn` -> DynRequiredEx.
     */
    if (auto *ts = dynamic_cast<const TypedScalarExpr *>(e)) {
        if (ts->cat == TypedScalarExpr::Cat::cmp ||
            ts->cat == TypedScalarExpr::Cat::logical ||
            ts->cat == TypedScalarExpr::Cat::lnot)
            return A.bool_ty();
        return ts->kind == TypeHint::f ? A.float_ty() : A.int_ty();
    }

    if (auto *idc = dynamic_cast<const IncDecExpr *>(e)) {
        /* `x++` / `++x` yields `x +/- 1` (int stays int, float stays float);
         * binop_result defers on an Unknown operand. The check pass enforces
         * the int/float-lvalue requirement. */
        return binop_result(idc->is_inc ? Op::plus : Op::minus,
                            type_of(idc->lvalue.get()), A.int_ty());
    }

    if (auto *te = dynamic_cast<const TernaryExpr *>(e)) {
        /* `c ? a : b` is the join of the two branches (the condition does not
         * affect the result type). Defer on an Unknown branch. INCOMPATIBLE
         * arms (`i == 0 ? [1] : 7` - a null join) are a genuine runtime
         * VARIANT: the type is `dyn` (opt if either arm is), the `int OP
         * dyn` philosophy - so a plain var bound to it gets the ordinary
         * DynRequiredEx "declare it dyn" and `var dyn q = ...` works. NOT a
         * premature dyn (the defer invariant): both arms are SETTLED
         * concrete kinds, a conflict cannot converge in a later round. (The
         * null used to escape into contribute/is_dyn - a pre-existing
         * SEGFAULT, found by the lever-1 step-5 sync-call test.) */
        StaticTypeRef tt = static_type_resolve(type_of(te->thenExpr.get()));
        StaticTypeRef el = static_type_resolve(type_of(te->elseExpr.get()));
        if (is_unknown(tt) || is_unknown(el))
            return bottom;
        StaticTypeRef j = A.join(tt, el);
        if (!j)
            j = A.with_opt(A.dyn_ty(), tt->opt || el->opt);
        return j;
    }

    if (auto *co = dynamic_cast<const CoalesceExpr *>(e)) {
        /* `a ?? b`: a's non-none part, or b. */
        StaticTypeRef ta = static_type_resolve(type_of(co->lhs.get()));
        StaticTypeRef tb = static_type_resolve(type_of(co->rhs.get()));
        if (is_unknown(ta) || is_unknown(tb))
            return bottom;
        if (ta->kind == StaticTypeKind::None)
            return tb;                      /* a is always none -> b */
        if (!ta->opt)
            return ta;                      /* a is never none -> a (b dead) */
        StaticTypeRef j = A.join(A.with_opt(ta, false), tb);
        if (!j)                             /* incompatible sides: a runtime
                                             * variant - dyn (see ternary) */
            j = A.with_opt(A.dyn_ty(), tb->opt);
        return j;
    }

    if (auto *e14 = dynamic_cast<const Expr14 *>(e)) {
        if (e14->op == Op::assign)
            return type_of(e14->rvalue.get());
        return binop_result(compound_binop(e14->op),
                            type_of(e14->lvalue.get()),
                            type_of(e14->rvalue.get()));
    }

    if (auto *call = dynamic_cast<const CallExpr *>(e)) {
        Construct *callee = call->what.get();
        if (auto *cid = dynamic_cast<const Identifier *>(callee)) {
            auto it = id_sym.find(cid);
            TypeSym *s = (it != id_sym.end()) ? it->second : nullptr;
            if (s && s->struct_type)   /* construction -> the struct type */
                return A.struct_ty(s->struct_type, s->struct_type->name);
            if (s && s->func) {
                /* A call to a TEMPLATE defers (bottom), NOT the template's
                 * fallback `ret` (which finalizes to `dyn`): instantiation will
                 * redirect this call to a concrete clone, and the clone's `ret`
                 * is the real type. Returning `dyn` here would be STICKY - it
                 * climbs the lattice and never comes back - so the enclosing var
                 * would stay `dyn` even after the redirect. This is what made a
                 * CROSS-INPUT call to a prior (pinned) template reject with
                 * DynRequiredEx and roll the instance back. */
                if (s->func->is_template)
                    return bottom;
                return s->func->ret;
            }
            if (s && is_func(s->type))
                return static_type_resolve(s->type)->ret;
            if (s && is_unknown(s->type))
                return bottom;          /* defer: callee not yet known */
            if (s && is_dyn(s->type))
                return A.dyn_ty();
            if (!s && is_builtin(cid->uid))
                return builtin_result(cid->uid, call->args.get());
            /* unresolved callee: defer (an UndefinedVariableEx at runtime), so
             * the enclosing var isn't forced to `dyn`, masking that error. */
            return bottom;
        }
        StaticTypeRef ct = type_of(callee);
        if (is_unknown(static_type_resolve(ct))) return bottom;   /* defer */
        return is_func(ct) ? static_type_resolve(ct)->ret : A.dyn_ty();
    }

    if (auto *sub = dynamic_cast<const Subscript *>(e)) {
        StaticTypeRef w = static_type_resolve(type_of(sub->what.get()));
        /* Defer on Unknown OR None: a None base is a fixpoint transient (the
         * element of an array(N) before its element type is pinned); a genuine
         * non-subscriptable base is caught in the check pass. */
        if (is_unknown(w) || is_none(w)) return bottom;
        if (w->kind == StaticTypeKind::Array) return w->elem;
        /* A dict read is non-opt: `d[k]` yields the value, the dict's default
         * (a default dict), or throws on a missing key - never none. Use
         * get()/get!() for explicit nullable / fail-fast lookup. */
        if (w->kind == StaticTypeKind::Dict)  return w->val;
        if (w->kind == StaticTypeKind::Str)   return A.str_ty();
        return A.dyn_ty();
    }

    if (auto *sl = dynamic_cast<const Slice *>(e)) {
        StaticTypeRef w = static_type_resolve(type_of(sl->what.get()));
        if (is_unknown(w) || is_none(w)) return bottom;   /* defer */
        if (w->kind == StaticTypeKind::Array || w->kind == StaticTypeKind::Str)
            return w;
        return A.dyn_ty();
    }

    if (auto *mem = dynamic_cast<const MemberExpr *>(e)) {

        /* `Type.CONST` (a struct descriptor base): a const member's type. */
        if (auto *cid = dynamic_cast<const Identifier *>(mem->what.get())) {
            auto it = id_sym.find(cid);
            if (it != id_sym.end() && it->second && it->second->struct_type) {
                const StructTypeDef *def = it->second->struct_type;
                if (const EvalValue *cv = def->const_of(mem->memUid))
                    return static_type_from_value(*cv);
                return A.dyn_ty();
            }
        }

        StaticTypeRef w = static_type_resolve(type_of(mem->what.get()));
        if (is_unknown(w) || is_none(w)) return bottom;   /* defer */

        StaticTypeRef mt;   /* the member's type, before optionality */
        /* A struct instance: `s.field` is the field's type (a dict read is
         * non-opt), `s.CONST` is the const member's type. */
        if (w->kind == StaticTypeKind::Struct) {
            const StructTypeDef *def =
                static_cast<const StructTypeDef *>(w->struct_def);
            if (const FieldDef *f = def->field_of(mem->memUid))
                mt = field_static_type(*f);
            else if (const EvalValue *cv = def->const_of(mem->memUid))
                mt = static_type_from_value(*cv);
            else
                mt = A.dyn_ty();
        } else if (w->kind == StaticTypeKind::Dict) {
            /* `d.key` mirrors `d[key]`: non-opt (value / default / throws). */
            mt = w->val;
        } else {
            mt = A.dyn_ty();
        }

        /* `a?.b` is nullable: none when `a` is none, else the member's type. */
        return mem->optional ? A.with_opt(static_type_resolve(mt), true) : mt;
    }

    if (auto *fd = dynamic_cast<const FuncDeclStmt *>(e)) {
        auto it = func_of_decl.find(fd);
        if (it != func_of_decl.end())
            return func_static_type(it->second);
        return A.dyn_ty();
    }

    return A.dyn_ty();
}

StaticTypeRef Inferencer::unary_result(Op op, StaticTypeRef a)
{
    if (op == Op::invalid)
        return a;
    if (op == Op::lnot)
        return A.bool_ty();
    if (op == Op::plus || op == Op::minus) {
        StaticTypeRef u = strip(static_type_resolve(a));
        if (is_unknown(u)) return bottom;   /* defer: operand not yet known */
        /* unary +/- promotes bool to int (`-true` is int -1). */
        if (u->kind == StaticTypeKind::Bool || u->kind == StaticTypeKind::Int)
            return A.int_ty();
        if (u->kind == StaticTypeKind::Float)
            return A.float_ty();
        return A.dyn_ty();
    }
    if (op == Op::bnot) {
        /* unary ~ : int only (bool promotes to int); float/other is an error */
        StaticTypeRef u = strip(static_type_resolve(a));
        if (is_unknown(u)) return bottom;
        if (u->kind == StaticTypeKind::Bool || u->kind == StaticTypeKind::Int)
            return A.int_ty();
        return A.dyn_ty();
    }
    return A.dyn_ty();
}

StaticTypeRef Inferencer::binop_result(Op op, StaticTypeRef a, StaticTypeRef b)
{
    a = static_type_resolve(a);
    b = static_type_resolve(b);

    /* comparisons / equality / logical always yield bool */
    if (op == Op::eq || op == Op::noteq || op == Op::lt || op == Op::gt ||
        op == Op::le || op == Op::ge || op == Op::land || op == Op::lor)
        return A.bool_ty();

    /* `str + anything` stringifies the RHS and yields str - even a dyn/none RHS
     * (so a `s = s + e` accumulator over a dyn element stays str). Handle it
     * before the dyn short-circuit below. */
    if (op == Op::plus && strip(a)->kind == StaticTypeKind::Str)
        return A.str_ty();

    /*
     * A `dyn` operand makes the arithmetic result `dyn` - `int + dyn` is the
     * NATURAL result of mixing a concrete with a variant. It does NOT get forced
     * to the concrete type: a fresh `var r = 3 + d` (d dyn) then correctly
     * infers `dyn` (must be declared `var dyn r`). The accumulator case
     * `var s = 0; s = s + x` keeps `s` int NOT by typing `s + x` int, but by the
     * ASSIGNMENT: a `dyn` value is *assignable* to a concrete-typed local (a
     * runtime-checked COERCION - `contribute`/`static_type_assignable` +
     * `coerce_to_decl_type`), so `s` (int, from `0`) coerces the `dyn` rhs and
     * stays int, erroring at runtime only if the value isn't int. See the
     * *dyn-into-concrete coercion* handling in `contribute` / the check pass.
     */
    if (is_dyn(a) || is_dyn(b))
        return A.dyn_ty();

    /*
     * An operand the fixpoint hasn't refined yet (Unknown / bottom) must NOT
     * collapse the result to `dyn` - that `dyn` would be sticky and poison a
     * self-referential accumulator (`acc = (acc + i) * 3` reads `acc` as Unknown
     * in round 0). Defer instead: return Unknown so a later round refines it
     * once the operand's real type is known. (Comparisons above already yield
     * int regardless.)
     */
    if (is_unknown(a) || is_unknown(b))
        return bottom;

    /*
     * A bare `None` operand also defers here. None arises transiently in the
     * fixpoint as the element of an array(N) before a later write pins the
     * element type (`a[i]` reads None until then); collapsing `None + x` to dyn
     * would poison a downstream accumulator the same way Unknown did. A
     * genuinely-none arithmetic operand is still rejected by require_nonopt in
     * the check pass (which runs before binop_result there), so deferring here
     * only affects the accumulate phase.
     */
    if (is_none(a) || is_none(b))
        return bottom;

    StaticTypeRef au = strip(a), bu = strip(b);
    const bool an = is_num(au), bn = is_num(bu);

    /* Arithmetic over numeric operands promotes bool to int first (so
     * `true + true` is int 2, never bool), then joins along bool < int <
     * float. Comparisons/logical are handled above and stay bool. */
    auto arith_join = [&](StaticTypeRef x, StaticTypeRef y) -> StaticTypeRef {
        StaticTypeRef j = A.join(x, y);
        if (j && static_type_resolve(j)->kind == StaticTypeKind::Bool)
            j = A.int_ty(j->opt);
        return j;
    };

    switch (op) {
        case Op::plus:
            /* str + anything -> str (the RHS is stringified); but int/float +
             * str is an error (handled by falling through to dyn). */
            if (au->kind == StaticTypeKind::Str)
                return A.str_ty();
            if (an && bn) {
                StaticTypeRef j = arith_join(au, bu);
                return j ? j : A.dyn_ty();
            }
            if (au->kind == StaticTypeKind::Array && bu->kind ==
                StaticTypeKind::Array) {
                StaticTypeRef j = A.join(au, bu);
                return j ? j : A.array_of(A.dyn_ty());
            }
            return A.dyn_ty();

        case Op::times:
            if (au->kind == StaticTypeKind::Str && bu->kind ==
                StaticTypeKind::Int)
                return A.str_ty();
            if (an && bn) {
                StaticTypeRef j = arith_join(au, bu);
                return j ? j : A.dyn_ty();
            }
            return A.dyn_ty();

        case Op::minus:
        case Op::div:
        case Op::mod:
            if (an && bn) {
                StaticTypeRef j = arith_join(au, bu);
                return j ? j : A.dyn_ty();
            }
            return A.dyn_ty();

        case Op::band:
        case Op::bor:
        case Op::bxor:
        case Op::shl:
        case Op::shr:
        case Op::ushr:
            /* bitwise/shift: int (or bool, which promotes) operands ONLY, ->
             * int. A float operand is an error (-> dyn here; the check pass
             * reports "operator does not apply"). */
            if (an && bn && au->kind != StaticTypeKind::Float
                    && bu->kind != StaticTypeKind::Float)
                return A.int_ty();
            return A.dyn_ty();

        default:
            return A.dyn_ty();
    }
}

/*
 * Result type of a builtin call. Precise where it matters downstream;
 * `dyn` otherwise (sound - the runtime still type-checks the call). See
 * plans/archived/type-inference-questions.md Q8.
 */
StaticTypeRef Inferencer::builtin_result(const UniqueId *name, ExprList *args)
{
    const std::string n(name->val);
    auto arg = [&](size_t i) -> StaticTypeRef {
        return i < args->elems.size() ? type_of(args->elems[i].get())
                                      : A.dyn_ty();
    };
    auto elem_of = [&](StaticTypeRef c) -> StaticTypeRef {
        c = static_type_resolve(c);
        if (is_unknown(c) || is_none(c)) return bottom;   /* defer */
        if (c->kind == StaticTypeKind::Array) return c->elem;
        if (c->kind == StaticTypeKind::Str)   return A.str_ty();
        if (c->kind == StaticTypeKind::Dict)  return c->key;
        return A.dyn_ty();
    };

    /* int-returning */
    if (n == "len" || n == "hash" || n == "ord" || n == "rand" ||
        n == "intptr" || n == "remove")
        return A.int_ty();

    /* bool-returning predicates */
    if (n == "defined" || n == "isbound" || n == "isconst" ||
        n == "isconstdecl" ||
        n == "ispure" || n == "ispuredecl" || n == "startswith" ||
        n == "endswith" || n == "isinf" || n == "isfinite" ||
        n == "isnormal" || n == "isnan")
        return A.bool_ty();

    /* float-returning */
    if (n == "float" || n == "exp" || n == "log" || n == "sqrt" ||
        n == "cbrt" || n == "pow" || n == "sin" || n == "cos" || n == "tan" ||
        n == "asin" || n == "acos" || n == "atan" || n == "ceil" ||
        n == "floor" || n == "trunc" || n == "round" || n == "randf" ||
        n == "math_e" || n == "math_pi" || n == "nan" || n == "inf" ||
        n == "eps")
        return A.float_ty();

    if (n == "int")    return A.int_ty();
    if (n == "str" || n == "typestr" || n == "kindstr" || n == "chr" ||
        n == "join" || n == "lpad" || n == "rpad" || n == "lstrip" ||
        n == "rstrip" || n == "strip" || n == "readln" || n == "read" ||
        n == "tmpdir")
        return A.str_ty();

    /* type(x) / decltype(v) -> a Type reflection object (native composite) */
    if (n == "type" || n == "decltype") {
        const StructTypeDef *ty = native_struct_type_def();
        return A.struct_ty(ty, ty->name);
    }

    if (n == "split" || n == "splitlines" || n == "readlines")
        return A.array_of(A.str_ty());

    /* layout(S) -> a StructLayout reflection object (native composite type) */
    if (n == "layout") {
        const StructTypeDef *sl = native_struct_layout_def();
        return A.struct_ty(sl, sl->name);
    }

    if (n == "range")  return A.array_of(A.int_ty());
    if (n == "array") {
        /*
         * array(N, value) -> array<typeof value>. array(N) -> array<none>: an
         * uninitialized array whose element type is pinned by later writes
         * (join_elem absorbs None, so array(N)-then-fill with ints is
         * array<int>); if never written, it stays array<none>. NOT array<dyn> -
         * that would defeat fill-based typing (see plans/type-driven-*.md).
         */
        if (args->elems.size() >= 2)
            return A.array_of(arg(1));
        return A.array_of(A.none_ty());
    }
    if (n == "make_array") {
        /* make_array(N, gen) -> array of the callback's return type. */
        StaticTypeRef f = static_type_resolve(arg(1));
        if (is_unknown(f)) return bottom;   /* defer: callback not yet known */
        return A.array_of(is_func(f) ? f->ret : A.dyn_ty());
    }
    if (n == "make_dict") {
        /* make_dict(keys, gen) -> dict<K, V>: K = the keys array's element
         * type, V = the callback's return type. The dict analogue of
         * make_array (typed the same way). */
        StaticTypeRef ks = static_type_resolve(arg(0));
        StaticTypeRef f = static_type_resolve(arg(1));
        if (is_unknown(ks) || is_unknown(f))
            return bottom;                  /* defer: args not yet known */
        StaticTypeRef k =
            ks->kind == StaticTypeKind::Array ? ks->elem : A.dyn_ty();
        return A.dict_of(k, is_func(f) ? f->ret : A.dyn_ty());
    }
    if (n == "dict") {
        /* dict(default_value) -> dict<dyn, typeof default> (a default dict, so
         * d[k] is non-opt). dict(pairs) -> dict<dyn,dyn>. A non-array arg is
         * the default value, which becomes the dict's value type. */
        if (args && args->elems.size() == 1) {
            StaticTypeRef a0 = static_type_resolve(arg(0));
            if (is_unknown(a0)) return bottom;
            if (a0->kind != StaticTypeKind::Array && a0->kind !=
                StaticTypeKind::None)
                return A.dict_of(A.dyn_ty(), a0);
        }
        return A.dict_of(A.dyn_ty(), A.dyn_ty());
    }

    /* get(d,key) -> opt V (nullable lookup); get!(d,key) -> V (or throws, so
     * the result is non-opt). */
    if (n == "get" || n == "get!") {
        StaticTypeRef d = static_type_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        StaticTypeRef v = d->kind == StaticTypeKind::Dict ? d->val : A.dyn_ty();
        return n == "get!" ? v : A.with_opt(v, true);
    }

    if (n == "abs" || n == "clone" || n == "deepclone")
        return arg(0);
    /* dynarray(a) -> array<dyn>: a polymorphic (general) copy. Typed array<dyn>
     * (not bare dyn) so plain `var d = dynarray(a)` is accepted under the
     * tolerant-array rule and d is built/typed general. */
    if (n == "dynarray")
        return A.array_of(A.dyn_ty());
    if (n == "runtime")
        return A.dyn_ty();   /* the documented opt-out: defer to runtime (Q) */
    if (n == "min" || n == "max") {
        /* min(array) -> the element type; min(a, b, ...) -> the join of args. */
        if (args->elems.size() == 1)
            return elem_of(arg(0));
        StaticTypeRef t = arg(0);
        for (size_t i = 1; i < args->elems.size(); i++) {
            if (is_unknown(static_type_resolve(t)) ||
                is_unknown(static_type_resolve(arg(i))))
                return bottom;          /* defer */
            StaticTypeRef j = A.join(t, arg(i));
            t = j ? j : A.dyn_ty();
        }
        return t;
    }

    if (n == "top" || n == "pop")
        return elem_of(arg(0));

    if (n == "sum") {
        /* sum returns the element type, except a bool array sums to an int
         * (it counts the `true`s; bool promotes to int in arithmetic). */
        StaticTypeRef el = elem_of(arg(0));
        if (is_unknown(el))
            return bottom;
        return static_type_resolve(el)->kind == StaticTypeKind::Bool ?
            A.int_ty() : el;
    }

    if (n == "map") {
        /* map(func, container) -> array of the callback's return type */
        StaticTypeRef f = static_type_resolve(arg(0));
        if (is_unknown(f)) return bottom;   /* defer: callback not yet known */
        return A.array_of(is_func(f) ? f->ret : A.dyn_ty());
    }
    if (n == "filter")              /* filter(func, container) -> container */
        return arg(1);
    if (n == "sort" || n == "rev_sort" || n == "reverse")
        return arg(0);             /* sort(array, [func]) -> array */

    if (n == "keys") {
        StaticTypeRef d = static_type_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        return A.array_of(d->kind == StaticTypeKind::Dict ? d->key :
            A.dyn_ty());
    }
    if (n == "values") {
        StaticTypeRef d = static_type_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        return A.array_of(d->kind == StaticTypeKind::Dict ? d->val :
            A.dyn_ty());
    }
    if (n == "kvpairs") {
        /* dict<k,v> -> array of [k, v] pairs, i.e. array<array<join(k,v)>>. */
        StaticTypeRef d = static_type_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        if (d->kind != StaticTypeKind::Dict) return A.array_of(A.dyn_ty());
        StaticTypeRef pair = A.join(d->key, d->val);
        return A.array_of(A.array_of(pair ? pair : A.dyn_ty()));
    }
    if (n == "find") {
        /*
         * find(dict, key) -> opt val; find(array, x) / find(str, sub) -> the
         * opt int index of the first match (or none). Only a fully-unknown,
         * non-container base stays opt dyn.
         */
        StaticTypeRef d = static_type_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        if (d->kind == StaticTypeKind::Dict) return A.with_opt(d->val, true);
        if (d->kind == StaticTypeKind::Array || d->kind == StaticTypeKind::Str)
            return A.with_opt(A.int_ty(), true);
        return A.with_opt(A.dyn_ty(), true);
    }

    if (n == "exception" || n == "ex")
        return A.exc_ty();

    if (n == "print" || n == "writeln" || n == "assert" || n == "append" ||
        n == "push" || n == "insert" || n == "exit" || n == "write" ||
        n == "writelines" || n == "erase")
        return A.none_ty();

    return A.dyn_ty();
}

/* ------------------------------- fixpoint -------------------------------- */

void Inferencer::reset_round()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func || s->pinned)   /* pinned: a prior input's fixed type */
            continue;
        s->round_got_dyn = false;   /* dyn-into-concrete coercion tracking */
        /* A scalar annotation pins the type: seed the accumulator with the
         * declared type so it stays fixed (contribute() keeps it and checks
         * assignability). dyn next, else bottom. */
        if (StaticTypeRef d = ann_scalar_static_type(s))
            s->acc = d;
        else
            s->acc = s->dyn_decl ? A.dyn_ty() : bottom;
    }
    for (auto &up : all_funcs)
        if (!up->pinned)
            up->ret_acc = bottom;
}

void Inferencer::commit_round()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func || s->pinned)
            continue;
        /*
         * dyn-into-concrete coercion decision (see contribute): a dyn value was
         * assigned to this plain `var`. If the NON-dyn contributions gave a
         * NUMERIC type (int/float), the var keeps it and coerces the dyn at
         * runtime (coerces_dyn, sticky). Otherwise (a non-numeric concrete, or
         * no concrete contribution at all) the dyn folds back in -> the var is
         * `dyn` (-> DynRequiredEx unless declared `dyn`).
         */
        if (s->round_got_dyn && !s->dyn_decl) {
            const StaticTypeRef ac = static_type_resolve(s->acc);
            if (ac->kind == StaticTypeKind::Int ||
                ac->kind == StaticTypeKind::Float)
                s->coerces_dyn = true;   /* keep numeric acc; runtime coerces */
            else
                s->acc = A.dyn_ty();     /* non-numeric / dyn-only -> dyn */
        }
        if (!static_type_equal(s->acc, s->type)) {
            changed = true;
            if (s->name)
                TRACE(infer, 1, std::string(s->name->val) + "  " +
                                static_type_to_string(s->type) + " -> " +
                                static_type_to_string(s->acc));
        }
        s->type = s->acc;
    }
    for (auto &up : all_funcs) {
        if (up->pinned)
            continue;
        if (!static_type_equal(up->ret_acc, up->ret)) {
            changed = true;
            if (up->decl && up->decl->id)
                TRACE(infer, 1, "func " +
                                std::string(up->decl->id->get_str()) +
                                " returns " + static_type_to_string(up->ret) +
                                    " -> " +
                                static_type_to_string(up->ret_acc));
        }
        up->ret = up->ret_acc;
    }
}

void Inferencer::contribute(TypeSym *s, StaticTypeRef t, Loc loc)
{
    /*
     * func-name syms derive their type from func_static_type(); never
         accumulated.
     * A dyn *var* DOES accumulate (Phase B): join keeps the type dyn but tracks
     * the opt bit, so `var dyn d = 5; d = none;` infers `opt dyn`. (A dyn param
     * is skipped in contribute_arg - its opt is declared, not inferred.)
     */
    if (!s || s->func)
        return;

    /*
     * REPL incremental: a symbol committed by a prior input has a FIXED type.
     * A new input may read it or assign to it, but an assignment must be
     * assignable to the committed type (the cross-input type-commitment) - the
     * same rule as a scalar annotation, but for any type. The message says
     * "is declared" when the commit came from an explicit annotation, else
     * "has type" (an inferred commit). Pinned syms are skipped by reset/commit,
     * so the type never moves.
     */
    if (s->pinned) {
        if (strict_dyn && !s->is_param) {
            StaticTypeRef rt = static_type_resolve(t);
            StaticTypeRef pt = static_type_resolve(s->type);
            if (!is_unknown(rt) && !is_none(rt) && !static_type_assignable(rt,
                pt))
                mismatch("'" + std::string(s->name->val) + "' " +
                             (s->ann != DeclType::none ? "is declared '"
                                                       : "has type '") +
                             static_type_to_string(pt) + "' but is assigned '" +
                             static_type_to_string(t) + "'",
                         loc, Loc());
        }
        return;
    }

    /*
     * A scalar-annotated symbol is PINNED to its declared type (`int x`): the
     * accumulator stays the declared type (seeded in reset_round), and each
     * contribution is instead CHECKED to be assignable to it. So `int x = 3.5`,
     * `int x = 5; x = 2.5`, and a float arg to an `int` parameter are errors,
     * while `float f = 3` (int widens) is fine. A param's *call-site* args get
     * the precise per-argument error from check_call; here we only check
     * assignments to a declared var. Unknown/none defer (Unknown is bottom; a
     * `none` into a non-opt declared type is reported by require_nonopt/the
     * mandatory-opt rule).
     */
    if (StaticTypeRef d = ann_scalar_static_type(s)) {
        if (strict_dyn && !s->is_param) {
            StaticTypeRef rt = static_type_resolve(t);
            if (!is_unknown(rt) && !is_none(rt) && !static_type_assignable(rt,
                d))
                mismatch("'" + std::string(s->name->val) +
                             "' is declared '" + static_type_to_string(d) +
                             "' but is assigned '" + static_type_to_string(t)
                                 + "'",
                         loc, Loc());
        }
        s->acc = d;                  /* pinned: never widens */
        return;
    }

    /*
     * dyn-into-concrete COERCION: a `dyn` value assigned to a plain `var` does
     * NOT widen it to dyn. Record the dyn contribution (round_got_dyn) but do
     * NOT join it - the accumulator collects only the NON-dyn contributions. At
     * commit, a NUMERIC accumulator + a dyn contribution means the var keeps its
     * numeric type and coerces the dyn value at runtime (coerces_dyn); a var
     * with ONLY dyn contributions (or a non-numeric one) folds the dyn back in
     * and becomes `dyn` -> DynRequiredEx. A `dyn`-declared var is unaffected
     * (its acc is seeded dyn and it accumulates normally). Order-independent:
     * the numeric-vs-dyn decision is made at commit from the collected concrete
     * type, not from the acc's transient state when the dyn arrived.
     */
    if (!s->dyn_decl && is_dyn(t)) {
        s->round_got_dyn = true;
        return;
    }

    StaticTypeRef nw = A.join(s->acc, t);
    if (!nw)
        mismatch("'" + std::string(s->name->val) + "' has type '" +
                     static_type_to_string(s->acc) + "' but is assigned '" +
                     static_type_to_string(t) + "'",
                 loc, Loc());
    s->acc = nw;
}

void Inferencer::contribute_arg(TypeSym *param, StaticTypeRef argT, Loc loc)
{
    if (!param || param->dyn_decl)
        return;
    argT = static_type_resolve(argT);
    if (!param->opt_decl) {
        if (argT->kind == StaticTypeKind::None)
            return;                  /* none->non-opt: flagged in check */
        argT = strip(argT);
    }
    contribute(param, argT, loc);
}

void Inferencer::contribute_ret(StaticTypeRef t)
{
    if (!cur_func)
        return;
    StaticTypeRef nw = A.join(cur_func->ret_acc, t);
    cur_func->ret_acc = nw ? nw : A.dyn_ty();   /* return conflict -> dyn */
}

void Inferencer::accumulate(Construct *n)
{
    if (!n)
        return;

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        FuncInfo *fi = func_of_decl[fd];
        if (fi && fi->is_template)
            return;   /* a template: inferred per instantiation, not here */
        FuncInfo *prev = cur_func;
        cur_func = fi;
        if (fd->body && !fd->body->is_block()) {
            accumulate(fd->body.get());
            contribute_ret(type_of(fd->body.get()));
        } else {
            accumulate(fd->body.get());
            if (cur_func->falls_through)
                contribute_ret(A.none_ty());
        }
        cur_func = prev;
        return;
    }

    if (auto *e14 = dynamic_cast<Expr14 *>(n)) {
        accumulate_assign(e14);
        return;
    }

    if (auto *idc = dynamic_cast<IncDecExpr *>(n)) {
        accumulate(idc->lvalue.get());
        /* Only a CONTAINER element accrues a type from `++` - so `d[k]++` pins
         * a `dict(0)`'s value type to int exactly as `d[k] += 1` does. A scalar
         * identifier does NOT: its type is fixed by its declaration, and
         * widening it here (`b = b + 1` -> int for a bool) would mask the
         * "++ requires int/float" check, which must still reject `b++`. */
        Construct *lv = idc->lvalue.get();
        if (dynamic_cast<Subscript *>(lv) || dynamic_cast<MemberExpr *>(lv))
            contribute_to_lvalue(lv, type_of(idc), idc->start);
        return;
    }

    if (auto *call = dynamic_cast<CallExpr *>(n)) {
        accumulate_call(call);
        return;
    }

    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        accumulate_foreach(fe);
        return;
    }

    if (auto *r = dynamic_cast<ReturnStmt *>(n)) {
        accumulate(r->elem.get());
        contribute_ret(r->elem ? type_of(r->elem.get()) : A.none_ty());
        return;
    }

    for_each_child(n, [&](Construct *c) { accumulate(c); });
}

void Inferencer::accumulate_call(CallExpr *call)
{
    accumulate(call->what.get());
    for (auto &a : call->args->elems)
        accumulate(a.get());

    ExprList *args = call->args.get();

    /* direct call to a known function (named, func-var, or inline lambda): feed
     * the argument types into its parameters */
    if (FuncInfo *fi = callee_funcinfo(call->what.get())) {
        if (fi->is_template)
            return;   /* a template call: handled by instantiation (redirect) */
        size_t n = std::min(fi->params.size(), args->elems.size());
        for (size_t i = 0; i < n; i++)
            contribute_arg(fi->params[i], type_of(args->elems[i].get()),
                           args->elems[i]->start);
        return;
    }

    auto *cid = dynamic_cast<Identifier *>(call->what.get());
    if (!cid)
        return;

    auto it = id_sym.find(cid);
    TypeSym *s = (it != id_sym.end()) ? it->second : nullptr;
    if (s)
        return;   /* a non-function symbol; not a builtin */

    /* element-contributing builtins: append/push(arr, x), insert(arr|dict, k,
     * x). Contribute only when the base kind is already known, and match it:
     * insert on a dict feeds dict<key,val>, on an array feeds array<val>. */
    const std::string nm(cid->uid->val);
    auto contribute_container = [&](size_t base_i, int key_i, size_t val_i) {
        if (base_i >= args->elems.size() || val_i >= args->elems.size())
            return;
        auto *bid = dynamic_cast<Identifier *>(args->elems[base_i].get());
        if (!bid)
            return;
        auto bit = id_sym.find(bid);
        TypeSym *bs = (bit != id_sym.end()) ? bit->second : nullptr;
        if (!bs)
            return;
        StaticTypeRef vt = type_of(args->elems[val_i].get());
        StaticTypeRef bt = static_type_resolve(bs->type);
        /* Defer if the element type isn't settled yet (the defer-on-Unknown
         * invariant): contributing `array<?>` to a PINNED global array would
         * trip its assignability check immediately - `array<?>` is not
         * top-level Unknown, so contribute()'s own guard wouldn't catch it -
         * and throw before a template-instance arg settles to its real type. A
         * later fixpoint round contributes the real `array<int>`. */
        if (is_unknown(static_type_resolve(vt)))
            return;
        if (bt->kind == StaticTypeKind::Dict) {
            StaticTypeRef kt = (key_i >= 0 && (size_t)key_i <
                args->elems.size())
                            ? type_of(args->elems[key_i].get()) : A.dyn_ty();
            if (is_unknown(static_type_resolve(kt)))
                return;
            contribute(bs, A.dict_of(kt, vt), bid->start);
        } else if (bt->kind == StaticTypeKind::Array) {
            contribute(bs, A.array_of(vt), bid->start);
        }
        /* unknown/other base kind: skip (don't guess) */
    };
    if (nm == "append" || nm == "push")
        contribute_container(0, -1, 1);
    else if (nm == "insert")
        contribute_container(0, 1, 2);
    else {
        /* Higher-order builtins: feed the container's element type to the
         * callback's parameter(s). map/filter are (func, container); sort and
         * rev_sort are (array, [comparator]). */
        size_t func_i, cont_i;
        bool comparator = false;
        if (nm == "map" || nm == "filter") {
            func_i = 0; cont_i = 1;
        } else if (nm == "sort" || nm == "rev_sort") {
            func_i = 1; cont_i = 0; comparator = true;
        } else if (nm == "make_array") {
            /* make_array(N, gen): the callback's param is the int index, not a
             * container element - type it int directly and return. */
            if (args->elems.size() >= 2) {
                FuncInfo *cb = callee_funcinfo(args->elems[1].get());
                if (cb && !cb->params.empty())
                    contribute_arg(cb->params[0], A.int_ty(),
                                   args->elems[1]->start);
            }
            return;
        } else if (nm == "make_dict") {
            /* make_dict(keys, gen): the callback's param is a KEY, so feed it
             * the keys array's element type via the general path below. */
            func_i = 1; cont_i = 0;
        } else {
            return;
        }
        if (func_i >= args->elems.size() || cont_i >= args->elems.size())
            return;
        FuncInfo *cb = callee_funcinfo(args->elems[func_i].get());
        if (!cb)
            return;
        StaticTypeRef ct =
            static_type_resolve(type_of(args->elems[cont_i].get()));
        /* DEFER while the container type is still Unknown - feeding the
         * callback param `dyn` here would be sticky (it climbs the lattice and
         * never comes back), poisoning the callback's ret and any accumulator
         * of the higher-order result (e.g. `total += make_dict(ks, f)[k]` or
         * `map`/`filter` in a loop). The param settles once ct does; if ct
         * finalizes Unknown the param legitimately finalizes dyn. Invariant. */
        if (is_unknown(ct))
            return;
        StaticTypeRef el = ct->kind == StaticTypeKind::Array ? ct->elem
                  : ct->kind == StaticTypeKind::Dict  ? ct->key : A.dyn_ty();
        Loc fl = args->elems[func_i]->start;
        auto &ps = cb->params;
        if (!ps.empty())
            contribute_arg(ps[0], el, fl);
        if (comparator && ps.size() >= 2)
            contribute_arg(ps[1], el, fl);
    }
}

void Inferencer::spread_idlist(IdList *idl, Construct *rvalue)
{
    StaticTypeRef rt = static_type_resolve(type_of(rvalue));

    if (auto *la = dynamic_cast<LiteralArray *>(rvalue)) {
        if (la->elems.size() == idl->elems.size()) {
            for (size_t i = 0; i < idl->elems.size(); i++)
                contribute(id_sym[idl->elems[i].get()],
                           type_of(la->elems[i].get()), idl->elems[i]->start);
            return;
        }
    }

    StaticTypeRef each = rt->kind == StaticTypeKind::Array ? rt->elem : rt;
    for (auto &id : idl->elems)
        contribute(id_sym[id.get()], each, id->start);
}

void Inferencer::accumulate_assign(Expr14 *e)
{
    accumulate(e->rvalue.get());
    if (!(e->fl & pFlags::pInDecl))
        accumulate(e->lvalue.get());

    StaticTypeRef ct;
    if (e->op == Op::assign) {
        ct = type_of(e->rvalue.get());
    } else {
        StaticTypeRef l = type_of(e->lvalue.get());
        StaticTypeRef r = type_of(e->rvalue.get());
        ct = binop_result(compound_binop(e->op), l, r);
        /* An invalid compound op (e.g. str -= int) yields dyn; don't let that
         * widen the target to dyn and hide the error - keep its type so the
         * check pass reports the mismatch. */
        if (is_dyn(ct) && !is_dyn(strip(l)) && !is_dyn(strip(r)))
            ct = l;
    }

    Construct *lv = e->lvalue.get();

    if (auto *idl = dynamic_cast<IdList *>(lv)) {
        spread_idlist(idl, e->rvalue.get());
        return;
    }

    contribute_to_lvalue(lv, ct, e->start);
}

/*
 * Contribute the post-write type `ct` back to an lvalue's symbol or container.
 * Shared by `x OP= v` (accumulate_assign) and `x++`/`--x` (accumulate): a bare
 * identifier accrues `ct`; an array/dict element accrues `array<ct>` /
 * `dict<K,ct>` to its base (only once the base kind is known, so an as-yet
 * Unknown base isn't wrongly guessed); a dict member accrues `dict<str,ct>`
 * (a struct field leaves the struct's fixed type alone). An IdList spread is
 * caller-specific (only assignment has one).
 */
void Inferencer::contribute_to_lvalue(Construct *lv, StaticTypeRef ct, Loc loc)
{
    if (auto *id = dynamic_cast<Identifier *>(lv)) {
        auto it = id_sym.find(id);
        if (it != id_sym.end())
            contribute(it->second, ct, loc);
        return;
    }

    if (auto *sub = dynamic_cast<Subscript *>(lv)) {
        if (auto *bid = dynamic_cast<Identifier *>(sub->what.get())) {
            auto it = id_sym.find(bid);
            if (it != id_sym.end() && it->second) {
                StaticTypeRef bt = static_type_resolve(it->second->type);
                if (bt->kind == StaticTypeKind::Dict)
                    contribute(it->second,
                               A.dict_of(type_of(sub->index.get()), ct),
                               bid->start);
                else if (bt->kind == StaticTypeKind::Array)
                    contribute(it->second, A.array_of(ct), bid->start);
            }
        }
        return;
    }

    if (auto *mem = dynamic_cast<MemberExpr *>(lv)) {
        if (auto *bid = dynamic_cast<Identifier *>(mem->what.get())) {
            auto it = id_sym.find(bid);
            if (it != id_sym.end() && it->second &&
                static_type_resolve(it->second->type)->kind ==
                    StaticTypeKind::Dict)
                contribute(it->second, A.dict_of(A.str_ty(), ct), bid->start);
        }
        return;
    }
}

void Inferencer::accumulate_foreach(ForeachStmt *fe)
{
    accumulate(fe->container.get());
    accumulate(fe->body.get());

    if (!fe->ids || fe->ids->elems.empty())
        return;

    StaticTypeRef c = static_type_resolve(type_of(fe->container.get()));

    /*
     * Defer if the container type isn't known yet: contributing `dyn` to the
     * loop var(s) now would be a transient that a downstream accumulator
     * (`s += e`) latches onto permanently. A later round, once the container is
     * a known kind, contributes the real element type.
     */
    if (is_unknown(c))
        return;

    auto &ids = fe->ids->elems;

    auto sym_of = [&](size_t i) { return id_sym[ids[i].get()]; };

    if (fe->indexed) {
        /* enumerate-style: first id is the int index, the rest the element -
         * OR, when there is more than one non-index var, the element is
         * DESTRUCTURED into them (an array element unpacked into its scalars),
         * matching do_iter (id_start=1 then the same tuple-unpack the
         * non-indexed 2+-var path does). Types the vars as the sub-array's
         * ELEMENT type, not the sub-array itself. */
        contribute(sym_of(0), A.int_ty(), ids[0]->start);
        if (c->kind == StaticTypeKind::Dict) {
            /* A dict's "element" is the (key, value) PAIR - do_iter binds
             * ids[1]=key, ids[2]=value (count==2), NOT a destructured single
             * element. So the two targets are key + value respectively, never
             * both the key. (Fixing a latent front-end mis-typing the
             * tree-walker hid because it binds dynamically at runtime.) */
            if (ids.size() >= 2)
                contribute(sym_of(1), c->key, ids[1]->start);
            if (ids.size() >= 3)
                contribute(sym_of(2), c->val, ids[2]->start);
            return;
        }
        StaticTypeRef el = c->kind == StaticTypeKind::Array ? c->elem
                    : c->kind == StaticTypeKind::Str ? A.str_ty()
                    : A.dyn_ty();
        StaticTypeRef bind = el;
        if (ids.size() > 2) {   /* index + 2+ targets -> unpack the element */
            StaticTypeRef re = static_type_resolve(el);
            bind = re->kind == StaticTypeKind::Array ? re->elem : el;
        }
        for (size_t i = 1; i < ids.size(); i++)
            contribute(sym_of(i), bind, ids[i]->start);
        return;
    }

    if (c->kind == StaticTypeKind::Dict && ids.size() >= 2) {
        contribute(sym_of(0), c->key, ids[0]->start);
        contribute(sym_of(1), c->val, ids[1]->start);
        return;
    }

    StaticTypeRef el = c->kind == StaticTypeKind::Array ? c->elem
                : c->kind == StaticTypeKind::Str ? A.str_ty()
                : c->kind == StaticTypeKind::Dict ? c->key : A.dyn_ty();

    if (ids.size() == 1) {
        contribute(sym_of(0), el, ids[0]->start);
    } else {
        /* tuple-unpack each element (an array) into the ids */
        StaticTypeRef inner = static_type_resolve(el)->kind ==
            StaticTypeKind::Array
                           ? static_type_resolve(el)->elem : el;
        for (size_t i = 0; i < ids.size(); i++)
            contribute(sym_of(i), inner, ids[i]->start);
    }
}

/* -------------------------------- check ---------------------------------- */

void Inferencer::mismatch(const std::string &m, Loc s, Loc e)
{
    throw TypeMismatchEx(intern_msg(m), s, e ? e : s);
}
void Inferencer::nullability(const std::string &m, Loc s, Loc e)
{
    throw NullabilityEx(intern_msg(m), s, e ? e : s);
}
void Inferencer::argcount(const std::string &m, Loc s, Loc e)
{
    throw WrongArgCountEx(intern_msg(m), s, e ? e : s);
}

void Inferencer::require_nonopt(StaticTypeRef t, Loc s, Loc e, const char *what)
{
    /*
     * Phase B: nullability is checked even for dyn. A plain `dyn` is non-opt
     * (is_optish false) and passes - its *type* operations are runtime-checked,
     * but it is proven non-none. An `opt dyn` is nullable and must be narrowed,
     * exactly like `opt int`.
     */
    if (is_optish(t))
        nullability(std::string("possibly-none value used ") + what +
                        " (type '" + static_type_to_string(t) + "')",
                    s, e);
}

void Inferencer::check_binops(MultiOpConstruct *mo, bool comparison,
                              bool logical, bool arith)
{
    for (auto &pr : mo->elems)
        check(pr.second.get());

    StaticTypeRef left = type_of(mo->elems[0].second.get());

    for (size_t i = 1; i < mo->elems.size(); i++) {
        Op op = mo->elems[i].first;
        Construct *rnode = mo->elems[i].second.get();
        StaticTypeRef right = type_of(rnode);

        if (logical) {
            /* && / || require int operands (verified runtime behaviour) */
            require_nonopt(left, mo->start, mo->end, "with operator &&/||");
            require_nonopt(right, rnode->start, rnode->end,
                           "with operator &&/||");
        } else if (comparison) {
            Op o = op;
            if (o != Op::eq && o != Op::noteq) {
                /* ordering: numeric or string only, non-opt */
                require_nonopt(left, mo->start, mo->end, "in a comparison");
                require_nonopt(right, rnode->start, rnode->end,
                               "in a comparison");
                StaticTypeRef l = strip(static_type_resolve(left));
                StaticTypeRef r = strip(static_type_resolve(right));
                if (!is_dyn(l) && !is_dyn(r)) {
                    bool ok = (is_num(l) && is_num(r)) ||
                              (l->kind == StaticTypeKind::Str &&
                               r->kind == StaticTypeKind::Str);
                    if (!ok)
                        mismatch("cannot compare '" +
                            static_type_to_string(left) +
                                     "' with '" + static_type_to_string(right)
                                         + "'",
                                 mo->start, rnode->end);
                }
            }
        } else if (arith) {
            require_nonopt(left, mo->start, mo->end,
                           "in an arithmetic operation");
            require_nonopt(right, rnode->start, rnode->end,
                           "in an arithmetic operation");
            StaticTypeRef res = binop_result(op, left, right);
            StaticTypeRef l = strip(static_type_resolve(left)), r =
                strip(static_type_resolve(right));
            if (!is_dyn(l) && !is_dyn(r) && is_dyn(res)) {
                /* binop_result returns dyn for an invalid combination */
                mismatch("operator does not apply to '" +
                    static_type_to_string(left) +
                             "' and '" + static_type_to_string(right) + "'",
                         mo->start, rnode->end);
            }
        }
        left = binop_result(op, left, right);
    }
}

/*
 * If `cond` proves a single variable non-none in one branch, return its
 * TypeSym and set in_then. Recognizes `x != none` / `none != x` (then),
 * `x == none` / `none == x` (else), and a bare `x` (then). Only narrows a
 * symbol whose type is actually nullable.
 */
TypeSym *Inferencer::narrow_target(Construct *cond, bool &in_then)
{
    auto sym_of = [&](Construct *c) -> TypeSym * {
        auto *id = dynamic_cast<Identifier *>(c);
        if (!id)
            return nullptr;
        auto it = id_sym.find(id);
        TypeSym *s = (it != id_sym.end()) ? it->second : nullptr;
        /* Phase B: narrow `opt dyn` too (-> `dyn` in the proven branch). */
        if (s && !s->func && is_optish(s->type))
            return s;
        return nullptr;
    };

    /* bare `if (x)` */
    if (TypeSym *s = sym_of(cond)) {
        in_then = true;
        return s;
    }

    /* `x != none` / `x == none` (Expr07, two operands) */
    auto *mo = dynamic_cast<MultiOpConstruct *>(cond);
    if (mo && (dynamic_cast<Expr07 *>(cond)) && mo->elems.size() == 2) {
        const Op op = mo->elems[1].first;
        if (op != Op::eq && op != Op::noteq)
            return nullptr;
        Construct *a = mo->elems[0].second.get();
        Construct *b = mo->elems[1].second.get();
        TypeSym *s = nullptr;
        if (dynamic_cast<LiteralNone *>(b))
            s = sym_of(a);
        else if (dynamic_cast<LiteralNone *>(a))
            s = sym_of(b);
        if (!s)
            return nullptr;
        in_then = (op == Op::noteq);    /* != none -> non-none in then */
        return s;
    }

    return nullptr;
}

void Inferencer::check_if(IfStmt *i)
{
    check(i->condExpr.get());

    bool in_then = false;
    TypeSym *nsym = narrow_target(i->condExpr.get(), in_then);

    auto with_narrow = [&](Construct *branch, bool active) {
        if (!branch)
            return;
        if (nsym && active) {
            const bool had = narrowed.count(nsym) != 0;
            StaticTypeRef old = had ? narrowed[nsym] : nullptr;
            narrowed[nsym] = strip(nsym->type);
            check(branch);
            if (had) narrowed[nsym] = old; else narrowed.erase(nsym);
        } else {
            check(branch);
        }
    };

    with_narrow(i->thenBlock.get(), in_then);
    with_narrow(i->elseBlock.get(), !in_then);
}

/* The bare kind of an StaticType as a string (kindstr):
    "int"/"array"/"struct"/... -
 * matching the runtime TypeNames so the fold and -nti fallback agree. */
static const char *static_type_kind_string(StaticTypeRef t)
{
    switch (static_type_resolve(t)->kind) {
        case StaticTypeKind::None:      return "none";
        case StaticTypeKind::Bool:      return "bool";
        case StaticTypeKind::Int:       return "int";
        case StaticTypeKind::Float:     return "float";
        case StaticTypeKind::Str:       return "str";
        case StaticTypeKind::Array:     return "array";
        case StaticTypeKind::Dict:      return "dict";
        case StaticTypeKind::Func:      return "func";
        case StaticTypeKind::Exception: return "exception";
        case StaticTypeKind::Struct:    return "struct";
        default:                 return "dyn";   /* Dyn / Unknown */
    }
}

/*
 * Build the native `Type` reflection object for a static type, recursively
 * (elem/key/val), as a boxed StructObject. PRE-GENERATED at compile time and
 * baked as a const LiteralObj by fold_type_query - never created at runtime.
 */
static EvalValue build_type_value(StaticTypeRef t)
{
    t = static_type_resolve(t);
    StructTypeDef *td = const_cast<StructTypeDef *>(native_struct_type_def());
    auto obj = make_intrusive<StructObject>(td);

    obj->fields.emplace_back(
        EvalValue(SharedStr(std::string(static_type_kind_string(t)))), false);
    obj->fields.emplace_back(EvalValue(SharedStr(static_type_to_string(t))),
        false);
    obj->fields.emplace_back(
        EvalValue(t->opt && t->kind != StaticTypeKind::None), false);

    EvalValue elem, key, val;   /* default-constructed = none */
    if (t->kind == StaticTypeKind::Array && t->elem) {
        elem = build_type_value(t->elem);
    } else if (t->kind == StaticTypeKind::Dict) {
        if (t->key) key = build_type_value(t->key);
        if (t->val) val = build_type_value(t->val);
    }
    obj->fields.emplace_back(elem, false);
    obj->fields.emplace_back(key, false);
    obj->fields.emplace_back(val, false);

    return EvalValue(intrusive_ptr<StructObject>(obj));
}

/*
 * Compile-time TYPE QUERIES with an UNEVALUATED operand (like C++ decltype/
 * sizeof): `type(expr)`/`decltype(var)` fold to a baked `Type` OBJECT
 * (LiteralObj) of the argument's static type, `typestr(expr)`/`kindstr(expr)`
 * to a string literal (the full structural string / the bare kind). `decltype`
 * takes a variable; the others take any expression. Returns false when the call
 * is none of these. The folded literal replaces args[0]; the runtime builtin
 * returns it (or, under -nti, reads the value's runtime type).
 */
bool Inferencer::fold_type_query(CallExpr *call)
{
    auto *cid = dynamic_cast<Identifier *>(call->what.get());
    if (!cid)
        return false;
    const std::string_view fn = cid->uid->val;
    const bool is_type     = fn == "type";
    const bool is_decltype = fn == "decltype";
    const bool is_typestr  = fn == "typestr";
    const bool is_kindstr  = fn == "kindstr";
    if (!is_type && !is_decltype && !is_typestr && !is_kindstr)
        return false;

    /* a real user symbol of that name shadows the builtin - not our call.
     * (A builtin callee has a null id_sym entry, so test the value.) */
    auto cit = id_sym.find(cid);
    if (cit != id_sym.end() && cit->second)
        return false;

    ExprList *args = call->args.get();
    if (args->elems.size() != 1)
        argcount(std::string(fn) + " expects exactly one argument",
                 call->start, call->end);

    StaticTypeRef t;
    if (is_decltype) {
        auto *argid = dynamic_cast<Identifier *>(args->elems[0].get());
        if (!argid)
            mismatch("decltype expects a variable (an identifier)",
                     args->elems[0]->start, args->elems[0]->end);
        auto it = id_sym.find(argid);
        if (it == id_sym.end() || !it->second)
            mismatch("decltype: '" + std::string(argid->uid->val) +
                         "' is not a variable in scope",
                     argid->start, argid->end);
        t = it->second->type;
    } else {
        t = type_of(args->elems[0].get());
        if (is_unknown(static_type_resolve(t)))
            return false;   /* type not determined yet: leave for runtime */
    }

    if (is_type || is_decltype)              /* -> a baked Type object */
        args->elems[0] = make_unique<LiteralObj>(build_type_value(t), true);
    else                                     /* typestr / kindstr -> string */
        args->elems[0] = make_unique<LiteralStr>(std::string_view(
            is_kindstr ? std::string(static_type_kind_string(t)) :
                static_type_to_string(t)));
    /* The call now just returns args[0] (the baked literal), so the VM codegen
     * can ELIDE it to a plain constant load - mark it. */
    call->tq_folded = true;
    return true;
}

void Inferencer::check(Construct *n)
{
    if (!n)
        return;

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        auto it = func_of_decl.find(fd);
        FuncInfo *fi = (it != func_of_decl.end()) ? it->second : nullptr;
        if (fi && fi->is_template)
            return;   /* a template: checked per instantiation (its clones) */
        FuncInfo *prev = cur_func;
        cur_func = fi;
        check(fd->body.get());
        cur_func = prev;
        return;
    }

    if (auto *i = dynamic_cast<IfStmt *>(n)) {
        check_if(i);
        return;
    }

    if (auto *b = dynamic_cast<Block *>(n)) {
        /* guard-clause narrowing: after `if (x == none) <return/throw>`, x is
         * non-none for the rest of this block. */
        std::vector<const TypeSym *> added;
        for (auto &st : b->elems) {
            check(st.get());
            auto *gi = dynamic_cast<IfStmt *>(st.get());
            if (!gi || gi->elseBlock || !always_exits(gi->thenBlock.get()))
                continue;
            bool in_then = false;
            TypeSym *s = narrow_target(gi->condExpr.get(), in_then);
            if (s && !in_then && !narrowed.count(s)) {
                narrowed[s] = strip(s->type);
                added.push_back(s);
            }
        }
        for (const TypeSym *s : added)
            narrowed.erase(s);
        return;
    }

    if (auto *call = dynamic_cast<CallExpr *>(n)) {
        if (fold_type_query(call))
            return;
        check_call(call);
        return;
    }

    if (dynamic_cast<Expr03 *>(n) || dynamic_cast<Expr04 *>(n) ||
        dynamic_cast<Expr05 *>(n) || dynamic_cast<Expr08 *>(n) ||
        dynamic_cast<Expr09 *>(n) || dynamic_cast<Expr10 *>(n)) {
        /* arith path: require non-opt operands and flag an invalid combination
         * (binop_result returns dyn for e.g. a float bitwise operand). */
        check_binops(static_cast<MultiOpConstruct *>(n), false, false, true);
        return;
    }
    if (dynamic_cast<Expr06 *>(n) || dynamic_cast<Expr07 *>(n)) {
        check_binops(static_cast<MultiOpConstruct *>(n), true, false, false);
        return;
    }
    if (dynamic_cast<Expr11 *>(n) || dynamic_cast<Expr12 *>(n)) {
        check_binops(static_cast<MultiOpConstruct *>(n), false, true, false);
        return;
    }

    if (auto *e2 = dynamic_cast<Expr02 *>(n)) {
        check(e2->elems[0].second.get());
        Op op = e2->elems[0].first;
        if (op == Op::plus || op == Op::minus) {
            StaticTypeRef t = type_of(e2->elems[0].second.get());
            require_nonopt(t, n->start, n->end, "with a unary +/-");
            StaticTypeRef u = strip(static_type_resolve(t));
            if (!is_dyn(u) && !is_unknown(u) && !is_num(u))
                mismatch("unary +/- needs a number, got '" +
                    static_type_to_string(t) +
                             "'", n->start, n->end);
        } else if (op == Op::bnot) {
            StaticTypeRef t = type_of(e2->elems[0].second.get());
            require_nonopt(t, n->start, n->end, "with a unary ~");
            StaticTypeRef u = strip(static_type_resolve(t));
            /* ~ is int-only (bool promotes); float/str/... is an error */
            if (!is_dyn(u) && !is_unknown(u) &&
                u->kind != StaticTypeKind::Int && u->kind !=
                    StaticTypeKind::Bool)
                mismatch("unary ~ needs an int, got '" +
                    static_type_to_string(t) +
                             "'", n->start, n->end);
        }
        return;
    }

    if (auto *sub = dynamic_cast<Subscript *>(n)) {
        check(sub->what.get());
        check(sub->index.get());
        StaticTypeRef w = type_of(sub->what.get());
        require_nonopt(w, sub->what->start, sub->what->end,
                       "as a subscript base");
        StaticTypeRef wr = strip(static_type_resolve(w));
        if (!is_dyn(wr) && !is_unknown(wr) && wr->kind !=
            StaticTypeKind::Array &&
            wr->kind != StaticTypeKind::Dict && wr->kind != StaticTypeKind::Str)
            mismatch("type '" + static_type_to_string(w) +
                "' is not subscriptable",
                     sub->what->start, sub->what->end);
        return;
    }

    if (auto *sl = dynamic_cast<Slice *>(n)) {
        check(sl->what.get());
        check(sl->start_idx.get());
        check(sl->end_idx.get());
        StaticTypeRef w = type_of(sl->what.get());
        require_nonopt(w, sl->what->start, sl->what->end, "as a slice base");
        StaticTypeRef wr = strip(static_type_resolve(w));
        if (!is_dyn(wr) && !is_unknown(wr) && wr->kind !=
            StaticTypeKind::Array &&
            wr->kind != StaticTypeKind::Str)
            mismatch("type '" + static_type_to_string(w) + "' is not sliceable",
                     sl->what->start, sl->what->end);
        return;
    }

    if (auto *mem = dynamic_cast<MemberExpr *>(n)) {
        check(mem->what.get());

        /* struct descriptor base: only a const member (`Type.CONST`). */
        if (auto *cid = dynamic_cast<Identifier *>(mem->what.get())) {
            auto it = id_sym.find(cid);
            if (it != id_sym.end() && it->second && it->second->struct_type) {
                const StructTypeDef *def = it->second->struct_type;
                if (!def->const_of(mem->memUid)) {
                    if (def->field_of(mem->memUid))
                        mismatch("field '" + std::string(mem->memUid->val) +
                                     "' needs an instance of '" +
                                     std::string(def->name->val) + "'",
                                 mem->start, mem->end);
                    mismatch("struct '" + std::string(def->name->val) +
                                 "' has no member '" +
                                 std::string(mem->memUid->val) + "'",
                             mem->start, mem->end);
                }
                return;
            }
        }

        StaticTypeRef w = type_of(mem->what.get());
        /* `a?.b` deliberately accepts a nullable base (none short-circuits);
         * a plain `a.b` requires a non-opt base. */
        if (!mem->optional)
            require_nonopt(w, mem->what->start, mem->what->end,
                           "as a member-access base");
        StaticTypeRef wr = strip(static_type_resolve(w));

        /* struct instance base: validate the field/const exists. */
        if (wr->kind == StaticTypeKind::Struct) {
            const StructTypeDef *def =
                static_cast<const StructTypeDef *>(wr->struct_def);
            if (!def->field_of(mem->memUid) && !def->const_of(mem->memUid))
                mismatch("struct '" + std::string(def->name->val) +
                             "' has no member '" +
                             std::string(mem->memUid->val) + "'",
                         mem->start, mem->end);
            return;
        }

        if (!is_dyn(wr) && !is_unknown(wr) && wr->kind != StaticTypeKind::Dict)
            mismatch("type '" + static_type_to_string(w) +
                         "' has no members (only a struct or dict does)",
                     mem->what->start, mem->what->end);
        return;
    }

    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        check(fe->container.get());
        check(fe->body.get());
        StaticTypeRef c = type_of(fe->container.get());
        require_nonopt(c, fe->container->start, fe->container->end,
                       "as a foreach container");
        return;
    }

    if (auto *idc = dynamic_cast<IncDecExpr *>(n)) {
        check(idc->lvalue.get());
        Construct *opnd = idc->lvalue.get();

        /* the operand must be an lvalue: a variable, an array element, or a
         * struct field (not a literal, an expression, or a call result). */
        auto *id = dynamic_cast<Identifier *>(opnd);
        if (!id && !dynamic_cast<Subscript *>(opnd)
                && !dynamic_cast<MemberExpr *>(opnd)) {
            mismatch("'++'/'--' needs a variable, array element, or field",
                     idc->start, idc->end);
            return;
        }

        /* not a const target (when the const survived as a symbol). */
        if (id) {
            auto it = id_sym.find(id);
            if (it != id_sym.end() && it->second && it->second->const_decl)
                mismatch("cannot '++'/'--' a const", idc->start, idc->end);
        }

        StaticTypeRef t = type_of(opnd);
        require_nonopt(t, opnd->start, opnd->end, "with '++'/'--'");

        /* int or float ONLY (bool / str / array / ... are rejected); a `dyn`
         * operand defers the check to runtime. */
        StaticTypeRef ts = strip(static_type_resolve(t));
        if (!is_dyn(ts) && !is_unknown(ts)
                && ts->kind != StaticTypeKind::Int && ts->kind !=
                    StaticTypeKind::Float)
            mismatch("'++'/'--' requires an int or float, got '" +
                         static_type_to_string(t) + "'",
                     opnd->start, opnd->end);
        return;
    }

    if (auto *e14 = dynamic_cast<Expr14 *>(n)) {
        check(e14->rvalue.get());
        if (!(e14->fl & pFlags::pInDecl))
            check(e14->lvalue.get());

        /* An explicitly-typed NON-opt variable must never become none: reject
         * `int a = none`, a later `a = none`, or assigning any nullable value
         * to it. A plain `var x` (no annotation) is implicitly nullable and so
         * exempt; an `int? a` / `opt int a` (opt annotation) accepts none. The
         * check runs here, in the check pass, with FINAL types - so a transient
         * none during the fixpoint (e.g. an array(N) element before a write
         * pins it) is not misflagged. */
        if (strict_dyn && e14->op == Op::assign) {
            if (auto *lid = dynamic_cast<Identifier *>(e14->lvalue.get())) {
                auto it = id_sym.find(lid);
                if (it != id_sym.end() && it->second) {
                    StaticTypeRef d = ann_scalar_static_type(it->second);
                    if (d && !d->opt &&
                        is_optish(type_of(e14->rvalue.get())))
                        nullability("'" + std::string(lid->uid->val) +
                                        "' is declared '" +
                                            static_type_to_string(d) +
                                        "' and cannot be none",
                                    e14->start, e14->end);
                }
            }
        }

        if (e14->op != Op::assign) {
            /* compound assign: validate the implied binary op */
            StaticTypeRef l = type_of(e14->lvalue.get());
            StaticTypeRef r = type_of(e14->rvalue.get());
            require_nonopt(l, e14->lvalue->start, e14->lvalue->end,
                           "in a compound assignment");
            require_nonopt(r, e14->rvalue->start, e14->rvalue->end,
                           "in a compound assignment");
            StaticTypeRef res = binop_result(compound_binop(e14->op), l, r);
            StaticTypeRef ls = strip(static_type_resolve(l)), rs =
                strip(static_type_resolve(r));
            if (!is_dyn(ls) && !is_dyn(rs) && is_dyn(res))
                mismatch("operator does not apply to '" +
                    static_type_to_string(l) +
                             "' and '" + static_type_to_string(r) + "'",
                         e14->start, e14->end);
        }
        return;
    }

    if (auto *th = dynamic_cast<ThrowStmt *>(n)) {
        check(th->elem.get());
        StaticTypeRef t = strip(static_type_resolve(type_of(th->elem.get())));
        /* A struct instance is the custom-exception value; a caught built-in
         * exception (Exception) can be re-thrown; dyn/Unknown/None defer. */
        if (!is_dyn(t) && t->kind != StaticTypeKind::Struct &&
            t->kind != StaticTypeKind::Exception &&
            t->kind != StaticTypeKind::Unknown && t->kind !=
                StaticTypeKind::None)
            mismatch("can only throw a struct instance, got '" +
                         static_type_to_string(type_of(th->elem.get())) + "'",
                     th->elem->start, th->elem->end);
        return;
    }

    for_each_child(n, [&](Construct *c) { check(c); });
}

/* Validate a struct construction `Type(args)` against the field list. By now
 * the args are positional (named ones were desugared in lower_named_args). The
 * field list plays the role of the parameter list: arity range [min, nfields]
 * (min = up to the last non-opt field), per-field assignability, and the
 * non-opt-field-can't-receive-none rule. */
void Inferencer::check_struct_construction(CallExpr *call,
                                           const StructTypeDef *def)
{
    ExprList *args = call->args.get();
    const size_t nfields = def->fields.size();
    const size_t nargs = args->elems.size();

    /* VM emplace hint (Phase 2b): a POD struct construction can be fused into
     * `append(struct_arr, Ctor(...))` -> EmplaceStruct (coerce the arg values
     * straight into the flat array's bytes). Only POD (a flat array<Struct> is
     * POD); a boxed struct stays the normal build+append path. */
    if (def->is_pod())
        call->vm_struct_ctor_def = def;
    else
        call->vm_struct_boxed_def = def;   /* VM: StructCtorBoxedV */

    size_t min_args = 0;
    for (size_t i = 0; i < nfields; i++)
        if (!def->fields[i].is_opt)
            min_args = i + 1;

    if (nargs < min_args || nargs > nfields) {
        const std::string want = (min_args == nfields)
            ? std::to_string(nfields)
            : std::to_string(min_args) + " to " + std::to_string(nfields);
        argcount("struct '" + std::string(def->name->val) + "' expects " +
                     want + " field value(s), got " + std::to_string(nargs),
                 call->start, call->end);
    }

    for (size_t i = 0; i < nargs && i < nfields; i++) {
        const FieldDef &fd = def->fields[i];
        StaticTypeRef at = type_of(args->elems[i].get());

        if (!fd.is_opt && is_optish(at))
            nullability("field '" + std::string(fd.name->val) +
                            "' is not 'opt' but the value may be none",
                        args->elems[i]->start, args->elems[i]->end);

        if (fd.kind == FieldKind::f_dyn || is_dyn(at))
            continue;

        StaticTypeRef ft = field_static_type(fd);
        StaticTypeRef src = fd.is_opt ? at : strip(at);
        if (!static_type_assignable(src, ft))
            mismatch("field '" + std::string(fd.name->val) + "' expects '" +
                         static_type_to_string(ft) + "' but got '" +
                         static_type_to_string(at) + "'",
                     args->elems[i]->start, args->elems[i]->end);
    }
}

void Inferencer::check_call(CallExpr *call)
{
    check(call->what.get());
    for (auto &a : call->args->elems)
        check(a.get());

    ExprList *args = call->args.get();

    /* Struct construction: validate the args against the field types. */
    if (auto *cid = dynamic_cast<Identifier *>(call->what.get())) {
        auto it = id_sym.find(cid);
        if (it != id_sym.end() && it->second && it->second->struct_type) {
            check_struct_construction(call, it->second->struct_type);
            return;
        }
    }

    /* resolve the callee to a parameter list (FuncInfo or a Func StaticType) */
    std::vector<TypeSym *> *fparams = nullptr;
    const StaticType *fst = nullptr;
    bool callable_known = false;
    /* An un-instantiated template call: ARITY is still checked here (vs the
     * declared params), but per-argument type/nullability is validated per
     * instantiation (in each clone), so the per-arg loop below is skipped. */
    bool template_call = false;

    if (FuncInfo *fi = callee_funcinfo(call->what.get())) {
        fparams = &fi->params;
        callable_known = true;
        template_call = fi->is_template;
    } else if (auto *cid = dynamic_cast<Identifier *>(call->what.get())) {
        auto it = id_sym.find(cid);
        TypeSym *s = (it != id_sym.end()) ? it->second : nullptr;
        if (s && s->func) {
            fparams = &s->func->params;
            callable_known = true;
        } else if (s && is_func(s->type)) {
            fst = static_type_resolve(s->type);
            callable_known = true;
        } else if (s && is_dyn(s->type)) {
            return;                       /* dyn callee: no checks */
        } else if (!s && is_builtin(cid->uid)) {
            return;                       /* builtin arity checked at runtime */
        } else if (s) {
            mismatch("'" + std::string(cid->uid->val) +
                         "' is not callable (type '" +
                             static_type_to_string(s->type) +
                         "')", call->what->start, call->what->end);
        } else {
            return;                       /* unresolved -> dyn (Q4) */
        }
    } else {
        StaticTypeRef ct = type_of(call->what.get());
        if (is_dyn(ct))
            return;
        if (is_func(ct)) {
            fst = static_type_resolve(ct);
            callable_known = true;
        } else {
            mismatch("expression of type '" + static_type_to_string(ct) +
                         "' is not callable", call->what->start,
                     call->what->end);
        }
    }

    if (!callable_known)
        return;

    size_t nparams = fparams ? fparams->size() : fst->params.size();
    size_t nargs = args->elems.size();

    /*
     * Trailing `opt` parameters may be omitted by the caller (they default to
     * `none`), so the legal arg count is a range [min_args, nparams], where
     * min_args is 1 + the index of the last non-opt param. An omitted opt param
     * is already typed `opt T` at finalization, so the body still null-checks
     * it; no per-call contribution is needed here.
     */
    auto param_is_opt = [&](size_t i) -> bool {
        return fparams ? (*fparams)[i]->opt_decl
                       : (i < fst->param_opt.size() && fst->param_opt[i]);
    };
    size_t min_args = 0;
    for (size_t i = 0; i < nparams; i++)
        if (!param_is_opt(i))
            min_args = i + 1;

    if (nargs < min_args || nargs > nparams) {
        const std::string want = min_args == nparams
            ? std::to_string(nparams)
            : std::to_string(min_args) + " to " + std::to_string(nparams);
        argcount("function expects " + want + " argument(s), got " +
                     std::to_string(nargs),
                 call->start, call->end);
    }

    if (template_call)
        return;   /* per-argument checking happens in each instantiation */

    for (size_t i = 0; i < nargs && i < nparams; i++) {
        Construct *anode = args->elems[i].get();
        StaticTypeRef at = type_of(anode);

        bool p_dyn, p_opt;
        StaticTypeRef ptype;
        if (fparams) {
            p_dyn = (*fparams)[i]->dyn_decl;
            p_opt = (*fparams)[i]->opt_decl;
            ptype = (*fparams)[i]->type;
        } else {
            p_dyn = static_type_resolve(fst->params[i])->kind ==
                StaticTypeKind::Dyn;
            p_opt = i < fst->param_opt.size() && fst->param_opt[i];
            ptype = fst->params[i];
        }

        /*
         * Nullability check FIRST, and it applies even to dyn (Phase B): a
         * possibly-none argument reaching a non-opt parameter (concrete OR dyn)
         * forces `opt`/`opt dyn`. For a named function we record it and report
         * at the param's declaration (enforce_nonnull_params); for a function
         * *value* (no decl) we flag the call. A plain `dyn` arg is non-opt, so
         * it passes.
         */
        if (!p_opt && is_optish(at)) {
            if (fparams)
                (*fparams)[i]->received_optish = true;
            else
                nullability("argument " + std::to_string(i + 1) +
                                " may be none but the parameter is not 'opt'",
                            anode->start, anode->end);
        }

        /* TYPE check: a dyn parameter or a dyn argument accepts any type (the
         * variant use-case), so skip assignability for those. */
        if (p_dyn || is_dyn(at))
            continue;

        StaticTypeRef src = p_opt ? at : strip(at);
        if (!static_type_assignable(src, ptype))
            mismatch("argument " + std::to_string(i + 1) + " has type '" +
                         static_type_to_string(at) +
                             "' but the parameter is '" +
                         static_type_to_string(ptype) + "'",
                     anode->start, anode->end);
    }
}

/* ============================ M8 specializer ============================= */

/* All operands of a MultiOpConstruct are statically scalar? `allow_float` also
 * accepts float operands (for a float-result op / a float comparison). */
/*
 * The scalar TypeHint of an operand for the specialization decision. Normally
 * its stamped `th`, but a scalar LITERAL created AFTER infer_types (AutoConst
 * folding a const `var` into a use, or the inliner's refold) carries no th -
 * yet its type is intrinsic. Derive it (int/bool -> i, float -> f) so a
 * comparison / arithmetic / bitwise op over a folded const still specializes.
 * Without this, `var N = 8; while (i < N)` (N auto-const-folded to 8) left
 * `i < 8` a boxed Expr06 - slow in BOTH the tree-walker and unlowerable by the
 * VM. Downstream is unaffected: a literal's eval_int/eval_float and the VM's
 * as_int_operand read it by kind, not th.
 */
static TypeHint operand_th(const Construct *n)
{
    if (n->th == TypeHint::i || n->th == TypeHint::f)
        return n->th;
    if (dynamic_cast<const LiteralInt *>(n)
        || dynamic_cast<const LiteralBool *>(n))
        return TypeHint::i;
    if (dynamic_cast<const LiteralFloat *>(n))
        return TypeHint::f;
    return n->th;
}

static bool ops_scalar(const MultiOpConstruct *mo, bool allow_float)
{
    for (const auto &pr : mo->elems) {
        const TypeHint t = operand_th(pr.second.get());
        if (t == TypeHint::i)
            continue;
        if (t == TypeHint::f && allow_float)
            continue;
        return false;
    }
    return true;
}

/* Build a TypedScalarExpr, moving `mo`'s operands in, copying loc/th. */
static unique_ptr<Construct>
make_typed(TypedScalarExpr::Cat cat, TypeHint kind, TypeHint result_th,
           MultiOpConstruct *mo)
{
    auto t = std::make_unique<TypedScalarExpr>(cat, kind);
    /* copy_base_fields carries start/end/is_const AND inline_ctx (+arr_hint) -
     * the latter matters for a specialized node INSIDE an inlined body: without
     * it the op loses its inlined-at chain, so a VM backtrace crossing it (which
     * relies on the op's node, not a Block wrapper) drops the virtual frame. */
    mo->copy_base_fields(*t);
    t->th = result_th;               /* override: the RESULT type hint */
    t->elems = std::move(mo->elems);
    return t;
}

/* Rewrite a single node into a TypedScalarExpr when its operands are statically
 * int/float; else return it unchanged. Children are already specialized. */
static unique_ptr<Construct> try_specialize(unique_ptr<Construct> n)
{
    /* arithmetic: Expr03 (* / %), Expr04 (+ -); bitwise/shift: Expr05
     * (<< >> >>>), Expr08 (&), Expr09 (^), Expr10 (|). The bitwise ones are
     * int-only (always th==i), and share the unboxed int arith loop in
     * TypedScalarExpr::eval_int. */
    if (dynamic_cast<Expr03 *>(n.get()) || dynamic_cast<Expr04 *>(n.get()) ||
        dynamic_cast<Expr05 *>(n.get()) || dynamic_cast<Expr08 *>(n.get()) ||
        dynamic_cast<Expr09 *>(n.get()) || dynamic_cast<Expr10 *>(n.get())) {
        auto *mo = static_cast<MultiOpConstruct *>(n.get());
        if (n->th == TypeHint::i && ops_scalar(mo, false))
            return make_typed(TypedScalarExpr::Cat::arith, TypeHint::i,
                              TypeHint::i, mo);
        if (n->th == TypeHint::f && ops_scalar(mo, true))
            return make_typed(TypedScalarExpr::Cat::arith, TypeHint::f,
                              TypeHint::f, mo);
        return n;
    }

    /* comparison: Expr06 (< > <= >=), Expr07 (== !=). Only the simple
     * two-operand form (the overwhelmingly common case). Result is int. */
    if (dynamic_cast<Expr06 *>(n.get()) || dynamic_cast<Expr07 *>(n.get())) {
        auto *mo = static_cast<MultiOpConstruct *>(n.get());
        if (mo->elems.size() == 2 && ops_scalar(mo, true)) {
            const bool anyf =
                operand_th(mo->elems[0].second.get()) == TypeHint::f ||
                operand_th(mo->elems[1].second.get()) == TypeHint::f;
            return make_typed(TypedScalarExpr::Cat::cmp,
                              anyf ? TypeHint::f : TypeHint::i,
                              TypeHint::i, mo);
        }
        return n;
    }

    /* logical: Expr11 (&&), Expr12 (||). Int operands, int result. */
    if (dynamic_cast<Expr11 *>(n.get()) || dynamic_cast<Expr12 *>(n.get())) {
        auto *mo = static_cast<MultiOpConstruct *>(n.get());
        if (ops_scalar(mo, false))
            return make_typed(TypedScalarExpr::Cat::logical, TypeHint::i,
                              TypeHint::i, mo);
        return n;
    }

    /* unary: Expr02 (+ - !), exactly one operand (elems[0]). */
    if (dynamic_cast<Expr02 *>(n.get())) {
        auto *mo = static_cast<MultiOpConstruct *>(n.get());
        if (mo->elems.size() != 1)
            return n;
        const Op op = mo->elems[0].first;
        const TypeHint ot = mo->elems[0].second->th;

        if (op == Op::invalid || op == Op::plus) {
            /* pass-through / unary + : unwrap to the operand */
            if (ot == TypeHint::i || ot == TypeHint::f)
                return std::move(mo->elems[0].second);
            return n;
        }
        if (op == Op::minus && (ot == TypeHint::i || ot == TypeHint::f))
            return make_typed(TypedScalarExpr::Cat::neg, ot, ot, mo);
        if (op == Op::lnot && ot == TypeHint::i)
            return make_typed(TypedScalarExpr::Cat::lnot, TypeHint::i,
                              TypeHint::i, mo);
        return n;
    }

    return n;
}

static unique_ptr<Construct> specialize(unique_ptr<Construct> n,
                                        int *fsize);

/* Recurse into every unique_ptr<Construct> child, specializing it in place. */
static void specialize_children(Construct *n, int *fsize)
{
    if (auto *e = dynamic_cast<Expr14 *>(n)) {
        e->lvalue = specialize(std::move(e->lvalue), fsize);
        e->rvalue = specialize(std::move(e->rvalue), fsize);
        return;
    }
    if (auto *c = dynamic_cast<CallExpr *>(n)) {
        c->what = specialize(std::move(c->what), fsize);
        for (auto &a : c->args->elems)
            a = specialize(std::move(a), fsize);
        return;
    }
    if (auto *m = dynamic_cast<MemberExpr *>(n)) {
        m->what = specialize(std::move(m->what), fsize);
        return;
    }
    if (auto *s = dynamic_cast<Subscript *>(n)) {
        s->what = specialize(std::move(s->what), fsize);
        s->index = specialize(std::move(s->index), fsize);
        return;
    }
    if (auto *s = dynamic_cast<Slice *>(n)) {
        s->what = specialize(std::move(s->what), fsize);
        if (s->start_idx)
            s->start_idx = specialize(std::move(s->start_idx), fsize);
        if (s->end_idx)
            s->end_idx = specialize(std::move(s->end_idx), fsize);
        return;
    }
    if (auto *i = dynamic_cast<IfStmt *>(n)) {
        i->condExpr = specialize(std::move(i->condExpr), fsize);
        i->thenBlock = specialize(std::move(i->thenBlock), fsize);
        if (i->elseBlock)
            i->elseBlock = specialize(std::move(i->elseBlock), fsize);
        return;
    }
    if (auto *w = dynamic_cast<WhileStmt *>(n)) {
        w->condExpr = specialize(std::move(w->condExpr), fsize);
        if (w->body) w->body = specialize(std::move(w->body), fsize);
        return;
    }
    if (auto *f = dynamic_cast<ForStmt *>(n)) {
        if (f->init) f->init = specialize(std::move(f->init), fsize);
        if (f->cond) f->cond = specialize(std::move(f->cond), fsize);
        if (f->inc)  f->inc = specialize(std::move(f->inc), fsize);
        if (f->body) f->body = specialize(std::move(f->body), fsize);
        return;
    }
    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        fe->container = specialize(std::move(fe->container), fsize);
        if (fe->body) fe->body = specialize(std::move(fe->body), fsize);
        return;
    }
    if (auto *t = dynamic_cast<TryCatchStmt *>(n)) {
        if (t->tryBody) t->tryBody = specialize(std::move(t->tryBody), fsize);
        for (auto &cs : t->catchStmts)
            if (cs.second) cs.second = specialize(std::move(cs.second), fsize);
        if (t->finallyBody)
            t->finallyBody = specialize(std::move(t->finallyBody), fsize);
        return;
    }
    if (auto *r = dynamic_cast<ReturnStmt *>(n)) {
        if (r->elem) r->elem = specialize(std::move(r->elem), fsize);
        return;
    }
    if (auto *te = dynamic_cast<TernaryExpr *>(n)) {
        /* Previously ABSENT - a ternary's cond/arms were never M8-specialized
         * (both engines ran them boxed; the recursion-unroll's guard ternaries
         * were the visible cost). Same for `??` below. */
        te->condExpr = specialize(std::move(te->condExpr), fsize);
        te->thenExpr = specialize(std::move(te->thenExpr), fsize);
        te->elseExpr = specialize(std::move(te->elseExpr), fsize);
        return;
    }
    if (auto *co = dynamic_cast<CoalesceExpr *>(n)) {
        co->lhs = specialize(std::move(co->lhs), fsize);
        co->rhs = specialize(std::move(co->rhs), fsize);
        return;
    }
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        /* A base template's body is a monomorphization shell - never
         * specialized (see FuncDeclStmt::is_template): it is cloned per concrete
         * signature (each clone specializes separately) and, when value-used,
         * run boxed for indirect dispatch. Specializing it would corrupt those. */
        /* A nested function owns a SEPARATE frame: walk its body with that
         * function's frame size, so a hoisted LICM temp lands in the right
         * frame (nullptr when unresolved - no frame to grow, no hoisting). */
        if (fd->body && !fd->is_template)
            fd->body = specialize(std::move(fd->body),
                                  fd->desc->resolved ? &fd->desc->frame_size
                                                     : nullptr);
        return;
    }
    if (auto *ld = dynamic_cast<LiteralDict *>(n)) {
        for (auto &kv : ld->elems) {
            kv->key = specialize(std::move(kv->key), fsize);
            kv->value = specialize(std::move(kv->value), fsize);
        }
        return;
    }
    if (auto *sc = dynamic_cast<SingleChildConstruct *>(n)) {
        if (sc->elem) sc->elem = specialize(std::move(sc->elem), fsize);
        return;
    }
    if (auto *mo = dynamic_cast<MultiOpConstruct *>(n)) {
        for (auto &pr : mo->elems)
            pr.second = specialize(std::move(pr.second), fsize);
        return;
    }
    if (auto *me = dynamic_cast<MultiElemConstruct<> *>(n)) {
        /* Block, LiteralArray, ExprList */
        for (auto &e : me->elems)
            e = specialize(std::move(e), fsize);
        return;
    }
}

/* ---------------- for-range loop specialization (ForRangeStmt) ----------- */

/* The innermost identifier of an lvalue chain (`x`, `arr[i]`, `obj.f.g` -> the
 * root var), or null if it is not identifier-rooted. */
static const UniqueId *fr_base_id(const Construct *lv)
{
    while (lv) {
        if (auto *id = dynamic_cast<const Identifier *>(lv))
            return id->uid;
        if (auto *s = dynamic_cast<const Subscript *>(lv)) {
            lv = s->what.get(); continue;
        }
        if (auto *m = dynamic_cast<const MemberExpr *>(lv)) {
            lv = m->what.get(); continue;
        }
        return nullptr;
    }
    return nullptr;
}

/* True if `callee` names a CONST (pure, side-effect-free, READ-ONLY,
 * deterministic) builtin - it never mutates an argument, so even a container
 * arg (e.g. `len(arr)`) is safe to evaluate once for the loop. */
static bool fr_is_const_builtin(const Construct *callee)
{
    auto *id = dynamic_cast<const Identifier *>(callee);
    return id && EvalContext::const_builtins.count(id->uid) != 0;
}

/* The set of effectively-pure USER functions in the program being specialized
 * (rebuilt per specialize_types run; see fr_collect_pure_funcs). NOTE mylang's
 * `pure` does NOT forbid mutating a param's ELEMENTS (`a[i] = v` keeps a func
 * pure - only length/global side effects make it impure), so a pure call's
 * result is constant ONLY when its args cannot be mutated by it - i.e. all
 * args are scalars (passed by value). That is enforced at the bound (below). */
static std::unordered_set<const UniqueId *> g_fr_pure;

/* -a/--analyze only (null in a normal run): try_for_range records a counted_for
 * annotation here for each `for` it specializes, so the view greens it. */
static AnalysisInfo *g_specialize_analyze = nullptr;

static void fr_collect_pure_funcs(Construct *c)
{
    if (!c)
        return;
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(c))
        if (fd->id && fd->desc->effective_pure)
            g_fr_pure.insert(fd->id->uid);
    Inferencer::for_each_child(c, fr_collect_pure_funcs);
}

static bool fr_is_pure_func(const Construct *callee)
{
    auto *id = dynamic_cast<const Identifier *>(callee);
    return id && g_fr_pure.count(id->uid) != 0;
}

/* A scalar (int/float) operand - proven by inference, passed by value, so a
 * callee cannot mutate it. */
static bool fr_is_scalar(const Construct *e)
{
    return e && (e->th == TypeHint::i || e->th == TypeHint::f);
}

/*
 * The mutations a loop body performs, split so `len(arr)` (depends only on the
 * LENGTH/identity of arr) can stay constant through an element write that does
 * NOT change the length (the common fill pattern `arr[i] = f(i)`):
 *   - `mut_len`     : identifiers whose value or length/identity may change -
 *                     a direct reassign (`x =`, `x++`), or a NON-const-builtin
 *                     call passed the container (append/pop/insert/erase/... or
 *                     any user func, which by reference can do anything to it);
 *   - `mut_content` : identifiers whose ELEMENT/FIELD may change - an
 *                     `arr[i] = v` / `obj.f = v` / `arr[i]++`, PLUS everything
 *                     in mut_len (reassign/impure call changes content too).
 * NOT descended into nested functions. Reuses the complete for_each_child so no
 * mutating node is ever missed - the immutability proof depends on this.
 */
static void fr_collect_mutated(
    Construct *c,
    std::unordered_set<const UniqueId *> &mut_len,
    std::unordered_set<const UniqueId *> &mut_content)
{
    if (!c || dynamic_cast<FuncDeclStmt *>(c))
        return;
    if (auto *e = dynamic_cast<Expr14 *>(c)) {
        if (!(e->fl & pFlags::pInDecl)) {
            if (auto *il = dynamic_cast<IdList *>(e->lvalue.get())) {
                for (auto &p : il->elems) {
                    mut_len.insert(p->uid);
                    mut_content.insert(p->uid);
                }
            } else if (dynamic_cast<Identifier *>(e->lvalue.get())) {
                const UniqueId *b = fr_base_id(e->lvalue.get());
                mut_len.insert(b);          /* `x = ...` : reassign */
                mut_content.insert(b);
            } else if (const UniqueId *b = fr_base_id(e->lvalue.get())) {
                mut_content.insert(b);      /* `arr[i] =`/`obj.f =` : content */
            }
        }
    } else if (auto *idc = dynamic_cast<IncDecExpr *>(c)) {
        if (dynamic_cast<Identifier *>(idc->lvalue.get())) {
            const UniqueId *b = fr_base_id(idc->lvalue.get());
            mut_len.insert(b);
            mut_content.insert(b);
        } else if (const UniqueId *b = fr_base_id(idc->lvalue.get())) {
            mut_content.insert(b);          /* `arr[i]++` : content */
        }
    } else if (auto *ce = dynamic_cast<CallExpr *>(c)) {
        /* A PURE call (const builtin or pure user func) cannot mutate an
         * argument - `pure` now forbids writing a reference parameter. Only an
         * IMPURE call may change a container passed to it (append/pop/`a[i]=v`/
         * a deeper call), so it taints both the length and the content of every
         * non-scalar argument (a scalar is by value, never mutated). */
        if (!fr_is_const_builtin(ce->what.get()) &&
            !fr_is_pure_func(ce->what.get()) && ce->args) {
            for (auto &a : ce->args->elems) {
                if (fr_is_scalar(a.get()))
                    continue;
                if (const UniqueId *b = fr_base_id(a.get())) {
                    mut_len.insert(b);
                    mut_content.insert(b);
                }
            }
        }
    }
    Inferencer::for_each_child(c, [&](Construct *ch) {
        fr_collect_mutated(ch, mut_len, mut_content);
    });
}

/*
 * True if `e` is a side-effect-free INT expression whose value cannot change
 * across the loop:
 *   - a literal int, or a SLOTTED-LOCAL identifier (not the loop var, whose
 *     length/identity is stable - not in `mut_len`);
 *   - an arith/bitwise/unary chain of immutable operands;
 *   - a subscript / member READ whose base + index are immutable AND whose base
 *     has no element/field write (`mut_content`) - the element is then stable;
 *   - a call to a CONST (pure) builtin with all-immutable arguments - e.g.
 *     `len(arr)` when arr's length is stable (an `arr[i] = v` is fine: it does
 *     not change the length).
 */
static bool fr_immutable(
    const Construct *e,
    const std::unordered_set<const UniqueId *> &mut_len,
    const std::unordered_set<const UniqueId *> &mut_content,
    const UniqueId *i_uid)
{
    if (!e)
        return false;
    if (dynamic_cast<const LiteralInt *>(e))
        return true;
    if (auto *id = dynamic_cast<const Identifier *>(e))
        return id->sym.kind == SymKind::local && id->uid != i_uid &&
               mut_len.find(id->uid) == mut_len.end();
    if (auto *sc = dynamic_cast<const SingleChildConstruct *>(e))  /* Expr01 */
        return fr_immutable(sc->elem.get(), mut_len, mut_content, i_uid);
    if (auto *sub = dynamic_cast<const Subscript *>(e)) {
        const UniqueId *b = fr_base_id(sub->what.get());
        return b && mut_content.find(b) == mut_content.end() &&
               fr_immutable(sub->what.get(), mut_len, mut_content, i_uid) &&
               fr_immutable(sub->index.get(), mut_len, mut_content, i_uid);
    }
    if (auto *m = dynamic_cast<const MemberExpr *>(e)) {
        const UniqueId *b = fr_base_id(m->what.get());
        return b && mut_content.find(b) == mut_content.end() &&
               fr_immutable(m->what.get(), mut_len, mut_content, i_uid);
    }
    if (auto *ce = dynamic_cast<const CallExpr *>(e)) {
        /* A call's result is constant across the loop iff the callee is pure
         * (a const builtin, or an effectively-pure user function) and every
         * argument is immutable. `pure` now forbids mutating a reference
         * parameter (see func_mutates_input in the resolver), so a pure call
         * neither has side effects nor changes its own result between
         * iterations - so even a container arg (`len(arr)`, `compute(arr)`) is
         * safe to evaluate once. */
        if (!fr_is_const_builtin(ce->what.get()) &&
            !fr_is_pure_func(ce->what.get()))
            return false;
        if (ce->args)
            for (auto &a : ce->args->elems)
                if (!fr_immutable(a.get(), mut_len, mut_content, i_uid))
                    return false;
        return true;
    }
    if (auto *mo = dynamic_cast<const MultiOpConstruct *>(e)) {
        /* arith (Expr02/03/04) + bitwise (Expr05/08/09/10) only - not a
         * comparison/logical (bool result, not an int bound). */
        if (dynamic_cast<const Expr06 *>(e) ||
            dynamic_cast<const Expr07 *>(e) ||
            dynamic_cast<const Expr11 *>(e) ||
            dynamic_cast<const Expr12 *>(e))
            return false;
        for (auto &p : mo->elems)
            if (!fr_immutable(p.second.get(), mut_len, mut_content, i_uid))
                return false;
        return true;
    }
    return false;
}

/*
 * If `n` is one of the two specializable counted-`for` forms, return an
 * equivalent ForRangeStmt (its kept sub-trees specialized); else return `n`
 * unchanged. Matched on the RAW for (before its cond/inc are specialized), so
 * the pattern is a plain Expr06 / Expr14 / IncDecExpr.
 */
static unique_ptr<Construct> try_for_range(unique_ptr<Construct> n,
                                          int *fsize)
{
    auto *f = dynamic_cast<ForStmt *>(n.get());
    if (!f || !f->init || !f->cond || !f->inc || !f->body)
        return n;

    /* init: `var i = start`, with i a slotted-local int */
    auto *init = dynamic_cast<Expr14 *>(f->init.get());
    if (!init || !(init->fl & pFlags::pInDecl) || init->op != Op::assign)
        return n;
    auto *ivar = dynamic_cast<Identifier *>(init->lvalue.get());
    if (!ivar || ivar->sym.kind != SymKind::local || ivar->th != TypeHint::i)
        return n;
    const UniqueId *i_uid = ivar->uid;
    const int i_slot = ivar->sym.slot;

    /* cond: `i </<= bound` (asc) or `i >=/> bound` (desc), bound int */
    auto *cond = dynamic_cast<Expr06 *>(f->cond.get());
    if (!cond || cond->elems.size() != 2)
        return n;
    auto *cleft = dynamic_cast<Identifier *>(cond->elems[0].second.get());
    if (!cleft || cleft->uid != i_uid)
        return n;
    const Op cmp_op = cond->elems[1].first;
    bool cmp_asc;
    if (cmp_op == Op::lt || cmp_op == Op::le)      cmp_asc = true;
    else if (cmp_op == Op::ge || cmp_op == Op::gt) cmp_asc = false;
    else return n;
    Construct *bound = cond->elems[1].second.get();
    if (bound->th != TypeHint::i)
        return n;

    /* inc: `i += step` / `i++` (asc) or `i -= step` / `i--` (desc) */
    bool inc_asc;
    Expr14 *inc14 = dynamic_cast<Expr14 *>(f->inc.get());
    Construct *step = nullptr;
    if (auto *idc = dynamic_cast<IncDecExpr *>(f->inc.get())) {
        auto *iid = dynamic_cast<Identifier *>(idc->lvalue.get());
        if (!iid || iid->uid != i_uid)
            return n;
        inc_asc = idc->is_inc;
    } else if (inc14) {
        if (inc14->fl & pFlags::pInDecl)
            return n;
        auto *iid = dynamic_cast<Identifier *>(inc14->lvalue.get());
        if (!iid || iid->uid != i_uid)
            return n;
        if (inc14->op == Op::addeq)       inc_asc = true;
        else if (inc14->op == Op::subeq)  inc_asc = false;
        else return n;
        step = inc14->rvalue.get();
        if (step->th != TypeHint::i)
            return n;
    } else {
        return n;
    }

    /* the comparison direction must match the step direction (`<`/`<=` with
     * `+`, `>=`/`>` with `-`). */
    if (cmp_asc != inc_asc)
        return n;

    /* bound and step must be loop-immutable */
    std::unordered_set<const UniqueId *> mut_len, mut_content;
    fr_collect_mutated(f->body.get(), mut_len, mut_content);
    if (!fr_immutable(bound, mut_len, mut_content, i_uid))
        return n;
    if (step && !fr_immutable(step, mut_len, mut_content, i_uid))
        return n;

    /* Build the ForRangeStmt; specialize the kept sub-trees (the body is the
     * hot part - M8 still applies inside it). */
    auto fr = make_unique<ForRangeStmt>();
    fr->start = f->start;
    fr->end = f->end;
    fr->i_slot = i_slot;
    fr->cmp_op = cmp_op;
    fr->bound = specialize(std::move(cond->elems[1].second), fsize);
    if (step)
        fr->step = specialize(std::move(inc14->rvalue), fsize);
    fr->init = specialize(std::move(f->init), fsize);
    fr->body = specialize(std::move(f->body), fsize);

    /* -a/--analyze: green the `for` keyword of a specialized counted loop. */
    if (g_specialize_analyze)
        g_specialize_analyze->mark(fr->start, 3, AnnoKind::counted_for);

    return fr;
}

/* Does `c` reference identifier `uid` anywhere? (The hoist's cond/inc
 * guard - a use of the slice var BEFORE its decl must keep the original
 * undefined-read semantics.) Complete via for_each_child. */
static bool hoist_refs_uid(Construct *c, const UniqueId *uid)
{
    if (!c)
        return false;
    if (auto *id = dynamic_cast<Identifier *>(c))
        if (id->uid == uid)
            return true;
    bool found = false;
    Inferencer::for_each_child(c, [&](Construct *ch) {
        if (!found && hoist_refs_uid(ch, uid))
            found = true;
    });
    return found;
}

/*
 * LEVER 3 increment 2 (plans/archived/native-gap-roadmap.md) - LOOP-INVARIANT
 * SLICE HOISTING. `for (...) { var sl = base[a:b]; ...reads... }`
 * evaluates an identical slice VIEW every iteration - the registration/
 * refcount churn is the loop's cost (the pooled set halved it; this
 * removes it). The decl statement hoists ABOVE the loop when every part
 * is provably iteration-independent AND the transform is unobservable:
 *
 *  - base: a slotted LOCAL, not in mut_len (reassign/impure-call: the
 *    view would rebind per iteration) AND NOT IN mut_content - the
 *    subtle one: an element write to a base with a LIVE slice COWs the
 *    BASE AWAY (clone_aliased_slices), so a hoisted view would keep
 *    reading the detached OLD storage while per-iteration fresh views
 *    see the new one;
 *  - bounds: absent, or fr_immutable (side-effect-free, loop-stable)
 *    AND int-proven (th == i / an int literal) - with base_sliceable
 *    (a statically non-opt array/str) the slice then CANNOT throw
 *    (pure clamping), so hoisting is safe even for a ZERO-iteration
 *    loop (the throw would otherwise move from "never" to "before");
 *  - sl: written exactly by its decl (not in mut_len/mut_content - no
 *    reassign, no write-through, no mutating builtin), and not
 *    referenced by the loop's cond/inc (a pre-decl read must keep its
 *    original undefined-read semantics). Escaping READS are fine: a
 *    slice view's identity is unobservable (intptr shows the SHARED
 *    storage) and copies register independently.
 *
 * The transform: Block{decl..., loop} with scope_free (the decl's slot
 * is function-frame-wide; resolution already happened). Both engines
 * benefit (the VM compiles the block as decl-then-loop). Only DIRECT
 * body statements are considered (no try-crossing, no nested blocks).
 */
static unique_ptr<Construct> try_hoist_loop_slices(unique_ptr<Construct> n)
{
    Construct *body_c = nullptr;
    Construct *cond1 = nullptr, *cond2 = nullptr;
    if (auto *f = dynamic_cast<ForStmt *>(n.get())) {
        body_c = f->body.get();
        cond1 = f->cond.get();
        cond2 = f->inc.get();
    } else if (auto *w = dynamic_cast<WhileStmt *>(n.get())) {
        body_c = w->body.get();
        cond1 = w->condExpr.get();
    } else {
        return n;
    }
    Block *body = dynamic_cast<Block *>(body_c);
    if (!body)
        return n;

    std::unordered_set<const UniqueId *> mut_len, mut_content;
    fr_collect_mutated(n.get(), mut_len, mut_content);

    std::vector<unique_ptr<Construct>> hoisted;
    for (size_t i = 0; i < body->elems.size(); ) {
        auto *e14 = dynamic_cast<Expr14 *>(body->elems[i].get());
        const Identifier *lhs;
        const Slice *sl;
        if (e14 && e14->op == Op::assign && (e14->fl & pFlags::pInDecl)
            && !(e14->fl & pFlags::pInConstDecl)
            && (lhs = dynamic_cast<Identifier *>(e14->lvalue.get()))
            && lhs->sym.kind == SymKind::local
            && lhs->decl_type != DeclType::i && lhs->decl_type != DeclType::f
            && !lhs->is_underscore()
            && (sl = dynamic_cast<Slice *>(e14->rvalue.get()))
            && sl->base_sliceable
            /* base: an inlined scalar LITERAL (an auto-const string -
             * bench 29's shape) is trivially invariant; else the
             * identifier must be length- AND content-stable (content:
             * the COW-detach hazard - see the header comment) */
            && (dynamic_cast<LiteralStr *>(sl->what.get()) != nullptr
                || (fr_base_id(sl->what.get())
                    && mut_len.find(fr_base_id(sl->what.get()))
                           == mut_len.end()
                    && mut_content.find(fr_base_id(sl->what.get()))
                           == mut_content.end()
                    && fr_immutable(sl->what.get(), mut_len, mut_content,
                                    nullptr)))
            && mut_len.find(lhs->uid) == mut_len.end()
            && mut_content.find(lhs->uid) == mut_content.end()
            && !hoist_refs_uid(cond1, lhs->uid)
            && !hoist_refs_uid(cond2, lhs->uid)) {
            const auto bound_ok = [&](const Construct *b) {
                if (!b)
                    return true;              /* absent -> clamp */
                if (!fr_immutable(b, mut_len, mut_content, nullptr))
                    return false;
                return b->th == TypeHint::i
                    || dynamic_cast<const LiteralInt *>(b) != nullptr;
            };
            if (bound_ok(sl->start_idx.get()) && bound_ok(sl->end_idx.get())) {
                hoisted.push_back(std::move(body->elems[i]));
                body->elems.erase(body->elems.begin()
                                  + static_cast<long>(i));
                continue;
            }
        }
        i++;
    }
    if (hoisted.empty())
        return n;

    auto blk = make_unique<Block>();
    n->copy_base_fields(*blk);            /* the loop's loc for carets */
    blk->scope_free = true;
    for (auto &h : hoisted)
        blk->elems.push_back(std::move(h));
    blk->elems.push_back(std::move(n));
    return blk;
}

/* ------------- loop-invariant CONTAINER-SUBSCRIPT hoisting (LICM) -------- */

/*
 * The synthetic temps are named `$licm<N>` (the `f$0` / `name$sN` convention -
 * `$` cannot occur in a source identifier, so a collision is impossible).
 * Monotonic per specialize_types run; only ever read back through the resolved
 * slot, never by name.
 */
static int g_licm_counter = 0;

/* The frame-slot ceiling for a hoist, matching the tail inliner's cap. */
static const int LICM_MAX_FRAME_SLOTS = 64;

/*
 * Structural equality over the side-effect-free expression grammar
 * `fr_immutable` admits - enough to recognize two occurrences of the SAME
 * invariant subscript (`a[i]` written twice in one body) so both share one
 * hoisted temp. Deliberately conservative: an unrecognized node kind compares
 * UNEQUAL, which costs a duplicate temp, never a wrong substitution.
 */
static bool licm_expr_equal(const Construct *x, const Construct *y)
{
    if (x == y)
        return true;
    if (!x || !y)
        return false;
    if (auto *ix = dynamic_cast<const Identifier *>(x)) {
        auto *iy = dynamic_cast<const Identifier *>(y);
        return iy && ix->uid == iy->uid && ix->sym.kind == iy->sym.kind
               && ix->sym.slot == iy->sym.slot;
    }
    if (auto *lx = dynamic_cast<const LiteralInt *>(x)) {
        auto *ly = dynamic_cast<const LiteralInt *>(y);
        return ly && lx->ival() == ly->ival();
    }
    if (auto *sx = dynamic_cast<const Subscript *>(x)) {
        auto *sy = dynamic_cast<const Subscript *>(y);
        return sy && licm_expr_equal(sx->what.get(), sy->what.get())
               && licm_expr_equal(sx->index.get(), sy->index.get());
    }
    if (auto *mx = dynamic_cast<const MemberExpr *>(x)) {
        auto *my = dynamic_cast<const MemberExpr *>(y);
        return my && mx->memUid == my->memUid
               && licm_expr_equal(mx->what.get(), my->what.get());
    }
    if (auto *cx = dynamic_cast<const SingleChildConstruct *>(x)) {
        auto *cy = dynamic_cast<const SingleChildConstruct *>(y);
        return cy && licm_expr_equal(cx->elem.get(), cy->elem.get());
    }
    if (auto *ox = dynamic_cast<const MultiOpConstruct *>(x)) {
        auto *oy = dynamic_cast<const MultiOpConstruct *>(y);
        if (!oy || typeid(*x) != typeid(*y) || ox->elems.size()
                                                   != oy->elems.size())
            return false;
        for (size_t i = 0; i < ox->elems.size(); i++)
            if (ox->elems[i].first != oy->elems[i].first
                || !licm_expr_equal(ox->elems[i].second.get(),
                                    oy->elems[i].second.get()))
                return false;
        return true;
    }
    return false;
}

/*
 * Is `f` a function VALUE this pass can prove harmless - one that cannot reach
 * the enclosing function's locals at all? `effective_pure` (the resolver's
 * `func_body_is_pure`) is exactly that proof: a pure function has NO capture
 * list, reads only consts + its own params + other pure functions, and does
 * not mutate a reference parameter. So it cannot see, let alone rebind, the
 * array a hoisted row was read from.
 *
 * Two spellings reach here - an INLINE lambda (`map(func(x) => x*2, a)`, a
 * FuncDeclStmt sitting in the argument list) and a NAME (`sort(a, cmp)`).
 */
static bool licm_pure_callback(const Construct *arg)
{
    if (auto *fd = dynamic_cast<const FuncDeclStmt *>(arg))
        return fd->desc && fd->desc->effective_pure;
    return fr_is_pure_func(arg);
}

/*
 * `fr_is_const_builtin` matches the NAME against const_builtins without
 * checking that the builtin is unshadowed - a `func len(x)` of one's own still
 * answers true there (a pre-existing looseness in try_for_range's bound
 * analysis, left alone: tightening it would also change the REPL, where
 * builtins are map-resident and carry no `builtin` SymKind). This pass wants
 * the real proof, so it additionally requires the resolver's verdict.
 */
static bool licm_is_const_builtin(const Construct *callee)
{
    auto *id = dynamic_cast<const Identifier *>(callee);
    return id && id->sym.kind == SymKind::builtin
           && fr_is_const_builtin(callee);
}

/*
 * Does `c` contain a call this pass cannot vouch for?
 *
 * Allowed: a call to an effectively-pure USER function, and a call to a CONST
 * BUILTIN whose callable arguments are all provably pure. The second clause is
 * why `len(arr)` / `abs(x)` / `str(v)` in a loop body cost nothing - their
 * `callable_arg_mask` is 0, so there is nothing to prove.
 *
 * The mask is what makes this safe rather than optimistic. A HIGHER-ORDER
 * builtin (map/filter/sort/make_array/make_dict/find) runs a callback that
 * `fr_collect_mutated` cannot see - it stops at a FuncDeclStmt - so a lambda
 * that appends to the base array would taint nothing and the base would look
 * invariant. The inferencer stamps which arguments could BE that callback
 * (a `Func` or a `dyn` static type); each one must then be provably pure.
 * A callback that is neither an inline lambda nor a named pure function - a
 * local variable holding one, say - is unprovable here, and refused.
 *
 * try_for_range has the same blind spot and lives with it because it hoists an
 * INT. This pass keeps a live REFERENCE across every iteration, so it checks.
 */
static bool licm_has_opaque_call(Construct *c)
{
    if (!c || dynamic_cast<FuncDeclStmt *>(c))
        return false;
    if (auto *ce = dynamic_cast<CallExpr *>(c)) {
        if (!fr_is_pure_func(ce->what.get())) {
            if (!licm_is_const_builtin(ce->what.get()))
                return true;
            const uint32_t m = ce->callable_arg_mask;
            const size_t na = ce->args ? ce->args->elems.size() : 0;
            if (na > 31)
                return true;
            /* A bit OUTSIDE the argument range means the mask was never
             * stamped (it defaults to ~0u) - treat the call as opaque. */
            const uint32_t valid = na ? ((1u << na) - 1u) : 0u;
            if (m & ~valid)
                return true;
            for (size_t i = 0; i < na; i++)
                if ((m & (1u << i))
                    && !licm_pure_callback(ce->args->elems[i].get()))
                    return true;
        }
        /* fall through: the ARGUMENTS still get walked below - an impure call
         * nested inside a pure call's argument list must still disqualify. */
    }
    bool found = false;
    Inferencer::for_each_child(c, [&](Construct *ch) {
        if (!found && licm_has_opaque_call(ch))
            found = true;
    });
    return found;
}

/*
 * Every name DECLARED inside the loop body - a `var`/`const` decl, a foreach
 * loop var, a catch variable, a nested func/struct name. A hoisted expression
 * may not reference one: the hoist runs BEFORE the loop, where such a slot
 * still holds its previous (or default `none`) value.
 *
 * This is NOT covered by fr_collect_mutated, which deliberately SKIPS a
 * pInDecl assignment - so a body-local `var m = ...` taints nothing and `m[1]`
 * looked perfectly invariant. That is exactly what the -rt suite caught.
 */
static void licm_collect_decls(Construct *c,
                               std::unordered_set<const UniqueId *> &out)
{
    if (!c)
        return;
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
        if (fd->id)
            out.insert(fd->id->uid);
        return;                       /* its params/locals are not visible */
    }
    if (auto *sd = dynamic_cast<StructDeclStmt *>(c)) {
        if (sd->id)
            out.insert(sd->id->uid);
        return;
    }
    if (auto *e = dynamic_cast<Expr14 *>(c)) {
        if (e->fl & pFlags::pInDecl) {
            if (auto *il = dynamic_cast<IdList *>(e->lvalue.get())) {
                for (auto &p : il->elems)
                    out.insert(p->uid);
            } else if (auto *id = dynamic_cast<Identifier *>(e->lvalue.get())) {
                out.insert(id->uid);
            }
        }
    } else if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
        if (fe->ids)
            for (auto &p : fe->ids->elems)
                out.insert(p->uid);
    } else if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
        for (auto &cs : tc->catchStmts)
            if (cs.first.asId)
                out.insert(cs.first.asId->uid);
    }
    Inferencer::for_each_child(c, [&](Construct *ch) {
        licm_collect_decls(ch, out);
    });
}

/* Does `e` reference any of `names`? */
static bool licm_refs_any(const Construct *e,
                          const std::unordered_set<const UniqueId *> &names)
{
    if (!e)
        return false;
    if (auto *id = dynamic_cast<const Identifier *>(e))
        return names.find(id->uid) != names.end();
    bool found = false;
    Inferencer::for_each_child(const_cast<Construct *>(e), [&](Construct *ch) {
        if (!found && licm_refs_any(ch, names))
            found = true;
    });
    return found;
}

/* Is `sub` worth hoisting, and provably the same value every iteration? */
static bool licm_candidate(
    const Subscript *sub,
    const std::unordered_set<const UniqueId *> &mut_len,
    const std::unordered_set<const UniqueId *> &mut_content,
    const std::unordered_set<const UniqueId *> &body_decls,
    const UniqueId *i_uid)
{
    /* Only an ARRAY read: a dict read has vivify/`for_write` subtleties and a
     * string subscript yields a fresh 1-char string (nothing to share). */
    if (!sub->base_array)
        return false;
    /* Only a CONTAINER result - the win is the intrusive_ptr copy + the boxed
     * element materialization. A proven scalar element is already a raw slot
     * read; hoisting one would buy a load and cost a slot. */
    if (sub->th == TypeHint::i || sub->th == TypeHint::f)
        return false;
    /* Nothing declared INSIDE the body: above the loop its slot is stale. */
    if (licm_refs_any(sub, body_decls))
        return false;
    /* Loop-invariant: base and index unchanged, and NO element/field write to
     * the base anywhere in the loop (mut_content, checked inside fr_immutable)
     * - that is also what rules out the COW-detach hazard the slice hoister
     * documents, since any write reachable to the base taints it. Passing
     * i_uid rejects any use of the loop variable, at any depth. */
    return fr_immutable(sub, mut_len, mut_content, i_uid);
}

/*
 * Collect the hoistable subscript SLOTS of one statement. Descends only through
 * positions that are evaluated UNCONDITIONALLY whenever the statement runs: a
 * ternary arm, a `??` rhs and a `&&`/`||` tail are short-circuited, so hoisting
 * out of one could evaluate - and throw on - something the loop never would.
 * Stops at a chosen subscript (does not descend into it), so the collected
 * slots are pairwise disjoint and substituting one cannot dangle another.
 */
static void licm_collect(
    unique_ptr<Construct> &slot,
    const std::unordered_set<const UniqueId *> &mut_len,
    const std::unordered_set<const UniqueId *> &mut_content,
    const std::unordered_set<const UniqueId *> &body_decls,
    const UniqueId *i_uid,
    std::vector<unique_ptr<Construct> *> &out)
{
    Construct *c = slot.get();
    if (!c)
        return;
    if (dynamic_cast<TernaryExpr *>(c) || dynamic_cast<CoalesceExpr *>(c)
        || dynamic_cast<Expr11 *>(c) || dynamic_cast<Expr12 *>(c)
        || dynamic_cast<FuncDeclStmt *>(c) || dynamic_cast<Block *>(c))
        return;
    if (auto *sub = dynamic_cast<Subscript *>(c))
        if (licm_candidate(sub, mut_len, mut_content, body_decls, i_uid)) {
            out.push_back(&slot);
            return;
        }
    for_each_child_slot(c, [&](unique_ptr<Construct> &ch) {
        licm_collect(ch, mut_len, mut_content, body_decls, i_uid, out);
    });
}

/* Build a use of the hoisted temp: a resolved-local Identifier at the
 * subscript's own source location (so a caret inside the loop is unmoved). */
static unique_ptr<Construct>
licm_make_use(const UniqueId *name, int slot, const Construct *at)
{
    auto id = make_unique<Identifier>(name->val);
    id->sym.kind = SymKind::local;
    id->sym.slot = slot;
    id->start = at->start;
    id->end = at->end;
    return id;
}

/*
 * LOOP-INVARIANT CONTAINER-SUBSCRIPT HOISTING. A nested-container read whose
 * base and index do not change across the loop - `a[i]` in
 * `for (var k = 0; k < n; k++) s += a[i][k] * b[k][j];` - re-materializes a
 * boxed row EvalValue (an intrusive_ptr retain/release plus the element read)
 * on every iteration. Roughly half of 46_matrix_mult's inner loop was that.
 * The read is hoisted into a synthetic temp above the loop:
 *
 *     Block(scope_free) {
 *         if (0 < n) { var $licm0 = a[i]; }          // the GUARD
 *         for (var k = 0; k < n; k++) s += $licm0[k] * b[k][j];
 *     }
 *
 * THE GUARD is what makes this sound. `a[i]` CAN throw (OOB), unlike the slice
 * hoister's pure-clamping slice, so an unguarded hoist would turn "never
 * evaluated" into "throws before the loop" for a ZERO-iteration loop. The guard
 * is the loop's own entry test with the loop variable replaced by the init
 * value - `COND[k := INIT]` - so:
 *   - the loop runs >= 1 time -> the guard holds and `a[i]` is evaluated once,
 *     exactly as it was on iteration 1;
 *   - the loop runs 0 times   -> the guard fails, `a[i]` is not evaluated, and
 *     an OOB index still throws never.
 * The temp is READ only inside the body, which runs only when the guard held.
 *
 * Restricted to the counted shape `for (var k = INIT; k CMP BOUND; ...)` with
 * INIT and BOUND both `fr_immutable` (hence side-effect-free), which makes the
 * guard exactly `INIT CMP BOUND` - no general substitution, and no risk of
 * duplicating a side effect into it. A `while` loop is not handled (its
 * condition would have to be proven side-effect-free separately).
 *
 * The ForStmt itself is left UNTOUCHED and merely wrapped: try_for_range runs
 * next and must still recognize the counted shape, and losing ForRangeStmt
 * would cost more than the hoist gains.
 */
static unique_ptr<Construct>
try_hoist_loop_subscripts(unique_ptr<Construct> n, int *fsize)
{
    if (!fsize || *fsize >= LICM_MAX_FRAME_SLOTS)
        return n;                     /* no frame to grow, or no room in it */
    auto *f = dynamic_cast<ForStmt *>(n.get());
    if (!f || !f->init || !f->cond || !f->inc || !f->body)
        return n;

    /* init: `var k = INIT`, k a slotted-local int */
    auto *init = dynamic_cast<Expr14 *>(f->init.get());
    if (!init || !(init->fl & pFlags::pInDecl) || init->op != Op::assign)
        return n;
    auto *ivar = dynamic_cast<Identifier *>(init->lvalue.get());
    if (!ivar || ivar->sym.kind != SymKind::local || ivar->th != TypeHint::i)
        return n;
    const UniqueId *i_uid = ivar->uid;

    /* cond: `k CMP BOUND` (the raw pre-specialization form, as try_for_range
     * matches it) */
    auto *cond = dynamic_cast<Expr06 *>(f->cond.get());
    if (!cond || cond->elems.size() != 2)
        return n;
    auto *cleft = dynamic_cast<Identifier *>(cond->elems[0].second.get());
    if (!cleft || cleft->uid != i_uid)
        return n;
    const Op cmp = cond->elems[1].first;
    if (cmp != Op::lt && cmp != Op::le && cmp != Op::gt && cmp != Op::ge)
        return n;

    /* A call this pass cannot vouch for can invalidate the hoisted reference
     * through a route the mutation sets do not model. */
    if (licm_has_opaque_call(f->body.get()))
        return n;

    std::unordered_set<const UniqueId *> mut_len, mut_content;
    fr_collect_mutated(n.get(), mut_len, mut_content);
    std::unordered_set<const UniqueId *> body_decls;
    licm_collect_decls(f->body.get(), body_decls);

    /* Both halves of the guard must be side-effect-free (it is evaluated once
     * BEFORE the loop's own init) and free of the loop variable. */
    Construct *bound = cond->elems[1].second.get();
    Construct *init_rv = init->rvalue.get();
    if (!fr_immutable(bound, mut_len, mut_content, i_uid)
        || !fr_immutable(init_rv, mut_len, mut_content, i_uid))
        return n;

    /*
     * A guard over two int LITERALS is decidable here (the bounds are const but
     * AutoConst has already run, so nothing else would fold it): false means
     * the loop body never runs, so there is nothing to hoist; true means the
     * guard is unconditional and can be dropped entirely.
     */
    bool guard_needed = true;
    if (auto *li = dynamic_cast<LiteralInt *>(init_rv))
        if (auto *lb = dynamic_cast<LiteralInt *>(bound)) {
            const int_type x = li->ival(), y = lb->ival();
            const bool enters = cmp == Op::lt ? x <  y
                              : cmp == Op::le ? x <= y
                              : cmp == Op::gt ? x >  y
                                              : x >= y;
            if (!enters)
                return n;
            guard_needed = false;
        }

    /*
     * Scan the body's DIRECT statements in order, stopping at the first one
     * that is not a plain expression statement: anything else (an `if`, a
     * nested loop, a `return`/`break`, a `try`) either evaluates its parts
     * conditionally or can skip the statements after it, so nothing beyond it
     * is guaranteed to run on every iteration.
     */
    std::vector<unique_ptr<Construct> *> sites;
    std::vector<unique_ptr<Construct> *> stmts;
    if (auto *b = dynamic_cast<Block *>(f->body.get())) {
        for (auto &e : b->elems)
            stmts.push_back(&e);
    } else {
        stmts.push_back(&f->body);
    }
    for (unique_ptr<Construct> *sp : stmts) {
        Construct *s = sp->get();
        if (!dynamic_cast<Expr14 *>(s) && !dynamic_cast<IncDecExpr *>(s))
            break;
        licm_collect(*sp, mut_len, mut_content, body_decls, i_uid, sites);
    }
    if (sites.empty())
        return n;

    /* One temp per DISTINCT expression; every occurrence reads that temp. */
    struct Group {
        const Construct *rep;      /* owned by the decl below - stable */
        const UniqueId *name;
        int slot;
    };
    std::vector<Group> groups;
    std::vector<unique_ptr<Construct>> decls;

    for (unique_ptr<Construct> *sp : sites) {
        int gi = -1;
        for (size_t g = 0; g < groups.size(); g++)
            if (licm_expr_equal(groups[g].rep, sp->get())) {
                gi = static_cast<int>(g);
                break;
            }
        if (gi >= 0) {
            *sp = licm_make_use(groups[gi].name, groups[gi].slot, sp->get());
            continue;
        }
        if (*fsize >= LICM_MAX_FRAME_SLOTS)
            break;                              /* frame budget exhausted */

        const int slot = (*fsize)++;
        const std::string nm = "$licm" + std::to_string(g_licm_counter++);
        auto decl_id = licm_make_use(UniqueId::get(nm), slot, sp->get());
        const UniqueId *name =
            static_cast<Identifier *>(decl_id.get())->uid;

        auto d = make_unique<Expr14>();
        d->op = Op::assign;
        d->fl = pFlags::pInDecl;
        d->start = (*sp)->start;
        d->end = (*sp)->end;
        d->lvalue = std::move(decl_id);
        auto use = licm_make_use(name, slot, sp->get());
        d->rvalue = std::move(*sp);            /* the subscript itself */
        groups.push_back({d->rvalue.get(), name, slot});
        decls.push_back(std::move(d));
        *sp = std::move(use);
    }
    if (decls.empty())
        return n;

    auto blk = make_unique<Block>();
    n->copy_base_fields(*blk);            /* the loop's loc for carets */
    blk->scope_free = true;

    if (guard_needed) {
        /* The guard: the loop's condition with the loop variable replaced by
         * the init value. Cloned, so the loop's own cond is untouched. */
        auto guard = f->cond->clone();
        auto *gm = static_cast<MultiOpConstruct *>(guard.get());
        gm->elems[0].second = init->rvalue->clone();

        auto then_blk = make_unique<Block>();
        then_blk->scope_free = true;
        then_blk->start = f->start;
        then_blk->end = f->end;
        for (auto &d : decls)
            then_blk->elems.push_back(std::move(d));

        auto iff = make_unique<IfStmt>();
        iff->start = f->start;
        iff->end = f->end;
        iff->condExpr = std::move(guard);
        iff->thenBlock = std::move(then_blk);
        blk->elems.push_back(std::move(iff));
    } else {
        for (auto &d : decls)
            blk->elems.push_back(std::move(d));
    }

    blk->elems.push_back(std::move(n));
    return blk;
}

static unique_ptr<Construct> specialize(unique_ptr<Construct> n, int *fsize)
{
    if (!n)
        return n;
    /* lever 3: hoist loop-invariant slice decls FIRST (the loop keeps its
     * raw form for try_for_range below; the wrapper block's children then
     * specialize individually). */
    if (dynamic_cast<ForStmt *>(n.get())
            || dynamic_cast<WhileStmt *>(n.get())) {
        if (!(g_opt_disabled & opt_slice_hoist))
            n = try_hoist_loop_slices(std::move(n));
        /* then the invariant container reads INSIDE the loop (a `for` only).
         * If the slice hoist already produced a wrapper Block, the recursion
         * below re-enters specialize() on the loop and it gets its turn. */
        if (!(g_opt_disabled & opt_licm)
            && dynamic_cast<ForStmt *>(n.get()))
            n = try_hoist_loop_subscripts(std::move(n), fsize);
        if (auto *blk = dynamic_cast<Block *>(n.get())) {
            for (auto &e : blk->elems)
                e = specialize(std::move(e), fsize);
            return n;
        }
    }
    /* a counted for-loop -> ForRangeStmt (matched on the raw form, BEFORE its
     * cond/inc are specialized away). */
    if (!(g_opt_disabled & opt_for_range)
        && dynamic_cast<ForStmt *>(n.get())) {
        n = try_for_range(std::move(n), fsize);
        if (dynamic_cast<ForRangeStmt *>(n.get()))
            return n;     /* matched: sub-trees already specialized */
    }
    specialize_children(n.get(), fsize);     /* bottom-up: children first */
    if (g_opt_disabled & opt_typed)
        return n;
    return try_specialize(std::move(n));
}

}  /* anonymous namespace */

void infer_types(Construct *root, bool enable, bool strict)
{
    if (!root)
        return;

    Inferencer inf(root);
    inf.strict_dyn = strict;
    /* -nti still lowers named-arg calls (syntactic, required for correctness),
     * but skips the fixpoint / type checking / strict enforcement. */
    inf.checks_enabled = enable;
    inf.run();
}

/*
 * -a/--analyze: record an array-storage color for every resolved identifier
 * whose static type is an array - green for a flat (unboxed) array<int>/
 * array<float>, red for array<dyn> (dynamic). Other general arrays (array<str>,
 * nested, opt-element) are left default: they genuinely cannot be flat, so
 * there is no "missed optimization" to flag. Every occurrence is colored
 * (id_sym maps each Identifier, decl and use, to its TypeSym).
 */
void Inferencer::collect_arrays(AnalysisInfo &out)
{
    for (const auto &kv : id_sym) {

        const Identifier *id = dynamic_cast<const Identifier *>(kv.first);
        TypeSym *s = kv.second;
        if (!id || !s)
            continue;

        StaticTypeRef ty = static_type_resolve(s->type);
        if (ty->kind != StaticTypeKind::Array)
            continue;

        StaticTypeRef el = static_type_resolve(ty->elem);
        AnnoKind k;
        if (!el->opt && (el->kind == StaticTypeKind::Int ||
                         el->kind == StaticTypeKind::Float ||
                         el->kind == StaticTypeKind::Bool))
            k = AnnoKind::flat_array;
        else if (el->kind == StaticTypeKind::Dyn)
            k = AnnoKind::dyn_array;
        else
            continue;   /* a general but non-dynamic array: leave default */

        out.mark(id->start, static_cast<int>(id->get_str().length()), k);
    }
}

void dump_type_info(Construct *root, std::ostream &os)
{
    if (!root)
        return;

    /* Non-strict: we want to SEE the dyn identifiers, not fail on them. */
    Inferencer inf(root);
    inf.strict_dyn = false;
    inf.run();
    inf.dump_debug_ti(os);
}

void collect_array_analysis(Construct *root, AnalysisInfo &out)
{
    if (!root)
        return;

    /* Non-strict: analyze whatever the code is, don't fail on a `dyn`. */
    Inferencer inf(root);
    inf.strict_dyn = false;
    inf.run();
    inf.collect_arrays(out);
}

/* ---- show() compile-time fold (the no-fail-codegen prerequisite) --------
 *
 * `show(x)` is a NODE-property builtin (it decompiles the optimized AST) and
 * the LAST node-ABI builtin - the one construct the codegen cannot lower.
 * Fold it here, on the FINAL tree (end of specialize_types - what the runtime
 * builtin would render), so it NEVER reaches codegen:
 *   - `show(<named top-level function>)`  -> render_func_code(decl)
 *   - `show(<non-identifier expression>)` -> render_construct_code(arg)
 * both replace the call with a LiteralStr - byte-identical to the runtime
 * builtin's answer (which renders the same final tree; the arg is never
 * evaluated, so folding moves no side effect). SELF-GATING: the callee must
 * be a resolved SymKind::builtin identifier - true only in SCRIPT mode (the
 * harness included; a user `func show` shadow is SymKind::global), never in
 * the REPL (map-resident builtins), which keeps its runtime builtin. The
 * residue - an identifier that is NOT a named top-level function (a var
 * holding a func; REPL-style dynamism) - stays a runtime call: fine under
 * the tree-walker, a LOUD compile abort under -vm codegen (show is
 * compile-rejected in real scripts; only the -rt harness can reach this).
 */
static const FuncDeclStmt *
show_named_func(const Block *root, const UniqueId *uid)
{
    for (const auto &e : root->elems)
        if (auto *fd = dynamic_cast<const FuncDeclStmt *>(e.get()))
            if (fd->id && fd->id->uid == uid)
                return fd;
    return nullptr;
}

static void
fold_show_slot(unique_ptr<Construct> &slot, const Block *root)
{
    if (!slot)
        return;

    /* bottom-up: an arg's own show() folds first */
    for_each_child_slot(slot.get(),
        [&](unique_ptr<Construct> &ch) { fold_show_slot(ch, root); });

    auto *ce = dynamic_cast<CallExpr *>(slot.get());
    if (!ce || !ce->args || ce->args->elems.size() != 1)
        return;
    auto *callee = dynamic_cast<Identifier *>(ce->what.get());
    if (!callee || callee->sym.kind != SymKind::builtin
            || callee->get_str() != "show")
        return;

    Construct *arg = ce->args->elems[0].get();
    std::string code;
    if (auto *id = dynamic_cast<Identifier *>(arg)) {
        const FuncDeclStmt *fd = show_named_func(root, id->uid);
        if (!fd)
            return;              /* not a named func: leave for the runtime */
        code = render_func_code(fd);
    } else {
        code = render_construct_code(arg);
    }

    auto lit = make_unique<LiteralStr>(std::string_view(code));
    lit->start = ce->start;
    lit->end = ce->end;
    lit->inline_ctx = ce->inline_ctx;
    slot = std::move(lit);
}

static void fold_show_calls(Block *root)
{
    for (auto &e : root->elems)
        fold_show_slot(e, root);
}

unsigned g_opt_disabled = 0;
/* `--strict` (step 7): every non-local must be DECLARED ABOVE its first use.
 * Off by default - it refuses programs that are correct today, so it is the
 * place for rules too aggressive to impose on everyone. Read by the resolver
 * (strict_forward_globals); a global for the same reason g_opt_disabled is -
 * threading a flag through run_optimizers' callers buys nothing. */
bool g_strict_mode = false;

std::vector<CompileWarning> g_warnings;

void compile_warn(const char *msg, Loc start, Loc end)
{
    g_warnings.push_back(CompileWarning{ msg, start, end });
}

bool opt_pass_bit(const std::string &name, unsigned &bit)
{
    static const struct { const char *n; unsigned b; } tbl[] = {
        { "licm",        opt_licm        },
        { "slice-hoist", opt_slice_hoist },
        { "for-range",   opt_for_range   },
        { "typed",       opt_typed       },
        { "all",         opt_all_passes  },
    };
    for (const auto &e : tbl)
        if (name == e.n) { bit = e.b; return true; }
    return false;
}

const char *opt_pass_names()
{
    return "licm, slice-hoist, for-range, typed, all";
}

void specialize_types(Construct *root, bool enable, EvalContext *prior_scope,
                      AnalysisInfo *analyze)
{
    if (!enable || !root)
        return;

    auto *blk = dynamic_cast<Block *>(root);
    if (!blk)
        return;

    g_specialize_analyze = analyze;   /* null in a normal run (no recording) */
    /* Reset per run, NOT monotonic across the process: two compiles of the
     * same program must produce byte-identical trees (the .myv determinism
     * property, and cross_compile_specialize_stable). */
    g_licm_counter = 0;

    /* a for-range bound may call a pure user function (with scalar args) -
     * collect this program's effectively-pure functions first, plus (in the
     * REPL) those from earlier inputs so a cross-input bound specializes. */
    g_fr_pure.clear();
    fr_collect_pure_funcs(root);
    if (prior_scope) {
        std::vector<std::pair<const UniqueId *, const LValue *>> syms;
        prior_scope->collect_symbols(syms);
        for (const auto &kv : syms) {
            const EvalValue &v = kv.second->get();
            if (!v.is<intrusive_ptr<FuncObject>>())
                continue;
            const FuncDescriptor *fd =
                v.get<intrusive_ptr<FuncObject>>()->func;
            if (fd && fd->name && fd->effective_pure)
                g_fr_pure.insert(fd->name);
        }
    }

    /* Top-level statements run in "main", whose frame is the root block's
     * slot_count - the frame a LICM hoist there grows (Block::do_eval and the
     * codegen both size main's frame from it). */
    for (auto &e : blk->elems)
        e = specialize(std::move(e), &blk->slot_count);

    /* The tree is FINAL now - fold every foldable show() (see above), so the
     * codegen never sees the one remaining node-ABI builtin. */
    fold_show_calls(blk);

    g_fr_pure.clear();
    g_specialize_analyze = nullptr;
}

/* ---- REPL incremental inference (wraps a persistent Inferencer) -------- */

struct ReplInfer::Impl {
    Inferencer inf;            /* root == nullptr; fed one input at a time */
    bool did_setup = false;

    Impl() {
        inf.strict_dyn = true;     /* faithful: the real strict checks */
        inf.checks_enabled = true;
    }
};

ReplInfer::ReplInfer() : impl(new Impl) { }
ReplInfer::~ReplInfer() = default;

void ReplInfer::check_input(Construct *input)
{
    auto *blk = dynamic_cast<Block *>(input);
    if (!blk)
        return;

    if (!impl->did_setup) {
        impl->inf.setup();
        impl->did_setup = true;
    }
    impl->inf.infer_input(blk);
}

void ReplInfer::undef_global(const UniqueId *name)
{
    impl->inf.undef_global(name);
}

std::string ReplInfer::global_type(const UniqueId *name)
{
    return impl->inf.global_type_str(name);
}

std::vector<std::string> ReplInfer::func_param_types(const FuncDeclStmt *fn)
{
    return impl->inf.func_param_types(fn);
}

std::string ReplInfer::func_return_type(const FuncDeclStmt *fn)
{
    return impl->inf.func_return_type(fn);
}

bool ReplInfer::instance_has_consumer(const FuncDeclStmt *fn)
{
    return impl->inf.instance_has_consumer(fn);
}

/* The exported COMPLETE child visitor (inferencer.h) - a thin wrapper over
 * Inferencer::for_each_child so external walks (the VM's AOT precompile /
 * ownership transfer) cannot miss a node. */
void for_each_child_of(Construct *c,
                       const std::function<void (Construct *)> &f)
{
    Inferencer::for_each_child(c, f);
}
