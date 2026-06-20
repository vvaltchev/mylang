/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"
#include "stype.h"
#include "errors.h"
#include "inferencer.h"
#include "analyzer.h"
#include "evalvalue.h"
#include "eval.h"

#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <deque>
#include <functional>
#include <algorithm>
#include <ostream>

/*
 * Whole-program static type inference + checking. See plans/type-inference.md
 * and plans/type-inference-questions.md for the design and decisions.
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

/* Compile errors are terminal; interning their messages into a leaked static
 * pool keeps Exception::msg a stable const char*. */
static const char *intern_msg(const std::string &s)
{
    static std::deque<std::string> pool;
    pool.push_back(s);
    return pool.back().c_str();
}

namespace {

struct FuncInfo;

struct TypeSym {
    const UniqueId *name = nullptr;
    STyRef type = nullptr;     /* stable type read by type_of() */
    STyRef acc = nullptr;      /* accumulator built during a round */
    bool dyn_decl = false;
    bool opt_decl = false;
    bool is_param = false;
    bool const_decl = false;   /* declared `const` (vs `var`) */
    bool is_loopvar = false;   /* a foreach loop variable (type is derived) */
    /* A param that received a possibly-none (opt/none) argument at some call
     * site (set in the check pass). With strict_dyn on, a non-opt non-dyn param
     * with this set is a compile error demanding `opt` (enforce_nonnull_params,
     * the param analogue of the mandatory-`dyn` rule). */
    bool received_optish = false;
    Loc decl_loc;
    FuncInfo *func = nullptr;  /* non-null when this name is a function */
};

struct FuncInfo {
    FuncDeclStmt *decl = nullptr;
    std::vector<TypeSym *> params;
    STyRef ret = nullptr;        /* stable return type */
    STyRef ret_acc = nullptr;    /* return accumulator (current round) */
    bool falls_through = false;
};

struct Scope {
    std::unordered_map<const UniqueId *, TypeSym *> syms;
    Scope *parent = nullptr;
};

class Inferencer {

public:

    explicit Inferencer(Construct *root) : root(root) { }
    void run();
    void dump_debug_ti(std::ostream &os);   /* --debug-ti */
    void collect_arrays(AnalysisInfo &out); /* -a: array storage colors */

    bool strict_dyn = false;   /* enforce the mandatory-`dyn` rule */
    bool strict_deep = false;  /* Phase B: dyn anywhere (incl. array<dyn>) */

private:

    STyArena A;
    Construct *root;
    STyRef bottom = nullptr;     /* the shared Unknown identity for join */

    std::vector<std::unique_ptr<TypeSym>> all_syms;
    std::vector<std::unique_ptr<FuncInfo>> all_funcs;
    std::vector<std::unique_ptr<Scope>> all_scopes;

    std::unordered_map<const Construct *, TypeSym *> id_sym;
    std::unordered_map<const Construct *, FuncInfo *> func_of_decl;

    Scope *global = nullptr;
    bool changed = false;
    FuncInfo *cur_func = nullptr;

    /* Flow-sensitive null narrowing (check pass only): inside the proven branch
     * of `if (x != none)` / `if (x)`, x reads as non-opt. */
    std::unordered_map<const TypeSym *, STyRef> narrowed;
    bool narrowing_on = false;

    /* helpers */
    Scope *new_scope(Scope *parent);
    TypeSym *new_sym(const UniqueId *name, Scope *s, Loc loc);
    static TypeSym *lookup(Scope *s, const UniqueId *name);
    FuncInfo *callee_funcinfo(Construct *e);   /* named func or inline lambda */
    static bool is_builtin(const UniqueId *name);
    void for_each_child(Construct *n,
                        const std::function<void(Construct *)> &fn);
    static bool always_exits(const Construct *n);

    /* structural pass */
    void hoist_globals(Block *root);
    void walk_struct(Construct *n, Scope *s);
    void declare_funcdecl(FuncDeclStmt *fd, Scope *s);
    void declare_target(Construct *lvalue, Scope *s, bool is_const);
    void enforce_concrete_decls();   /* the mandatory-`dyn` rule */
    void enforce_nonnull_params();   /* the mandatory-`opt` rule for params */
    static bool type_has_dyn(STyRef t, bool deep);

    /* type computation (reads stable `type`) */
    STyRef type_of(const Construct *e);
    STyRef func_sty(FuncInfo *fi);
    STyRef binop_result(Op op, STyRef a, STyRef b);
    STyRef unary_result(Op op, STyRef a);
    STyRef builtin_result(const UniqueId *name, ExprList *args);
    STyRef sty_from_value(const EvalValue &v);
    static Op compound_binop(Op op);

    /* fixpoint */
    void reset_round();
    void commit_round();
    void accumulate(Construct *n);
    void contribute(TypeSym *s, STyRef t, Loc loc);
    void contribute_arg(TypeSym *param, STyRef argT, Loc loc);
    void contribute_ret(STyRef t);
    void accumulate_assign(Expr14 *e);
    void accumulate_call(CallExpr *call);
    void accumulate_foreach(ForeachStmt *fe);
    void spread_idlist(IdList *idl, Construct *rvalue);

    /* check pass */
    void check(Construct *n);
    void check_if(IfStmt *i);
    TypeSym *narrow_target(Construct *cond, bool &in_then);
    void annotate_hints(Construct *n);   /* stamp TypeHints for specializer */
    void set_array_repr_hint(Expr14 *e);    /* type-driven ArrHint on rvalue */
    void check_call(CallExpr *call);
    void check_binops(MultiOpConstruct *mo, bool comparison, bool logical,
                      bool arith);
    void require_nonopt(STyRef t, Loc s, Loc e, const char *what);
    [[noreturn]] void mismatch(const std::string &m, Loc s, Loc e);
    [[noreturn]] void nullability(const std::string &m, Loc s, Loc e);
    [[noreturn]] void argcount(const std::string &m, Loc s, Loc e);

    /* small predicates */
    STyRef strip(STyRef t) { return A.with_opt(t, false); }
    static bool is_num(STyRef t) {
        t = sty_resolve(t);
        return t->kind == STyKind::Int || t->kind == STyKind::Float;
    }
    static bool is_dyn(STyRef t) {
        return sty_resolve(t)->kind == STyKind::Dyn;
    }
    static bool is_none(STyRef t) {
        return sty_resolve(t)->kind == STyKind::None;
    }
    static bool is_unknown(STyRef t) {
        return sty_resolve(t)->kind == STyKind::Unknown;
    }
    static bool is_optish(STyRef t) {
        t = sty_resolve(t);
        return t->opt || t->kind == STyKind::None;
    }
    static bool is_func(STyRef t) {
        return sty_resolve(t)->kind == STyKind::Func;
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
    all_syms.push_back(std::make_unique<TypeSym>());
    TypeSym *sym = all_syms.back().get();
    sym->name = name;
    sym->type = bottom;
    sym->acc = bottom;
    sym->decl_loc = loc;
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

/* ------------------------------ run / passes ----------------------------- */

void Inferencer::run()
{
    Block *rootBlock = dynamic_cast<Block *>(root);
    if (!rootBlock)
        return;

    bottom = A.fresh_var();
    global = new_scope(nullptr);

    hoist_globals(rootBlock);
    for (auto &e : rootBlock->elems)
        walk_struct(e.get(), global);

    /* fixpoint */
    for (int iter = 0; iter < 1000; iter++) {
        reset_round();
        cur_func = nullptr;
        for (auto &e : rootBlock->elems)
            accumulate(e.get());
        changed = false;
        commit_round();          /* sets `changed` if any type moved */
        if (!changed)
            break;
    }

    /* finalize. An unconstrained value is `none` for a local ("only-none /
     * doesn't matter") but `dyn` for a PARAMETER: a never-(concretely-)called
     * function's parameter could be anything, so its body must still type-check
     * (this covers uncalled funcs, callbacks folded at parse time, and HO uses
     * we don't model precisely). dyn/opt declarations win as written. */
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func)
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
        if (is_unknown(up->ret))
            up->ret = A.dyn_ty();
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
        enforce_concrete_decls();
        enforce_nonnull_params();
    }

    /* Stamp TypeHints (int/float) for the M8 specializer + typed conditions. */
    for (auto &e : rootBlock->elems)
        annotate_hints(e.get());
}

/*
 * True if `t` is dynamic. `deep` recurses into container element/key/value
 * types (so array<dyn>/dict<_,dyn> count) - the Phase-B (strict) sense; with
 * `deep` false only a bare top-level `dyn` counts (Phase A, tolerating
 * dynamic-element arrays). Function types are NOT recursed into: a var holding
 * a `func(dyn)->int` holds a concrete function value.
 */
bool Inferencer::type_has_dyn(STyRef t, bool deep)
{
    t = sty_resolve(t);

    if (t->kind == STyKind::Dyn)
        return true;

    if (!deep)
        return false;

    if (t->kind == STyKind::Array)
        return type_has_dyn(t->elem, true);

    if (t->kind == STyKind::Dict)
        return type_has_dyn(t->key, true) || type_has_dyn(t->val, true);

    return false;
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

        /* Skip: explicitly `dyn`, function names, params (a never-called func's
         * param is legitimately `dyn` and has no `var` to annotate), foreach
         * loop vars (their type is derived from the container, which carries
         * any `dyn`), and builtins (no decl loc). */
        if (s->dyn_decl || s->func || s->is_param || s->is_loopvar ||
            !s->decl_loc)
            continue;

        if (!type_has_dyn(s->type, strict_deep))
            continue;

        const std::string what = s->const_decl ? "const" : "variable";
        throw DynRequiredEx(
            intern_msg(
                std::string(s->name->val) + ": " + what +
                " has dynamic type '" + sty_to_string(s->type) +
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

        if (!s->is_param || s->opt_decl || !s->decl_loc)
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
 * `dyn`s (plans/type-driven-specialization.md). One `ti` record per symbol:
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

        STyRef ty = s->func ? func_sty(s->func) : s->type;

        os << "ti\t" << std::string(s->name->val)
           << "\t" << kind
           << "\t" << s->decl_loc.line
           << "\t" << s->decl_loc.col
           << "\t" << (s->const_decl ? 1 : 0)
           << "\t" << sty_to_string(ty)
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

    STyRef t = sty_resolve(type_of(n));
    if (!t->opt) {
        if (t->kind == STyKind::Int)
            n->th = TypeHint::i;
        else if (t->kind == STyKind::Float)
            n->th = TypeHint::f;
    }

    if (auto *e = dynamic_cast<Expr14 *>(n))
        set_array_repr_hint(e);

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
    STyRef ty = sty_resolve(it->second->type);

    /*
     * A `dyn`-typed destination (`var dyn d = ...`) means "I want a polymorphic
     * array", so build it general - otherwise `var dyn d = [1,2,3]; d[0]="x"`
     * would wrongly hit the flat-array error even though `d` is already dyn.
     * Anything that is neither an array nor `dyn` has no array repr to pick.
     */
    ArrHint hint;
    if (ty->kind == STyKind::Array) {
        STyRef el = sty_resolve(ty->elem);
        if (!el->opt && el->kind == STyKind::Int)
            hint = ArrHint::flat_i;
        else if (!el->opt && el->kind == STyKind::Float)
            hint = ArrHint::flat_f;
        else
            hint = ArrHint::general;
    } else if (ty->kind == STyKind::Dyn) {
        hint = ArrHint::general;
    } else {
        return;
    }

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
        if (nm == "range" || nm == "array" || nm == "make_array")
            call->args->arr_hint = hint;

    } else {

        /* an array literal or a folded const literal (LiteralObj) */
        rv->arr_hint = hint;
    }
}

void Inferencer::hoist_globals(Block *rootBlock)
{
    for (auto &e : rootBlock->elems) {
        Construct *n = e.get();
        if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
            declare_funcdecl(fd, global);
        } else if (auto *e14 = dynamic_cast<Expr14 *>(n)) {
            if (e14->fl & pFlags::pInDecl)
                declare_target(e14->lvalue.get(), global,
                               e14->fl & pFlags::pInConstDecl);
        }
    }
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
        id_sym[id] = sym;
    };

    if (auto *id = dynamic_cast<Identifier *>(lvalue))
        decl_one(id);
    else if (auto *idl = dynamic_cast<IdList *>(lvalue))
        for (auto &up : idl->elems)
            decl_one(up.get());
}

void Inferencer::walk_struct(Construct *n, Scope *s)
{
    if (!n)
        return;

    if (auto *blk = dynamic_cast<Block *>(n)) {
        Scope *inner = new_scope(s);
        for (auto &e : blk->elems)
            walk_struct(e.get(), inner);
        return;
    }

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        declare_funcdecl(fd, s);
        FuncInfo *fi = func_of_decl[fd];
        Scope *fscope = new_scope(global);   /* funcs see globals only */

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
                id_sym[p.get()] = psym;
                fi->params.push_back(psym);
            }

        walk_struct(fd->body.get(), fscope);
        return;
    }

    if (auto *e14 = dynamic_cast<Expr14 *>(n)) {
        walk_struct(e14->rvalue.get(), s);   /* RHS before name exists */
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
                if (fe->idsVarDecl) {
                    TypeSym *sym = new_sym(id->uid, inner, id->start);
                    sym->is_loopvar = true;   /* type derived from container */
                    id_sym[id.get()] = sym;
                } else {
                    id_sym[id.get()] = lookup(s, id->uid);
                }
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

    if (auto *id = dynamic_cast<Identifier *>(n)) {
        if (!id_sym.count(id))
            id_sym[id] = lookup(s, id->uid);
        return;
    }

    for_each_child(n, [&](Construct *c) { walk_struct(c, s); });
}

/* --------------------------- type computation ---------------------------- */

STyRef Inferencer::func_sty(FuncInfo *fi)
{
    std::vector<STyRef> ps;
    std::vector<bool> popt;
    for (TypeSym *p : fi->params) {
        ps.push_back(p->dyn_decl ? A.dyn_ty() : p->type);
        popt.push_back(p->opt_decl);
    }
    return A.func_of(ps, popt, fi->ret);
}

/*
 * Derive the exact static type of a baked const value (a folded const array/
 * dict literal), recursing into its elements. A const value is immutable and
 * fully known at compile time, so element types are not discarded: a
 * homogeneous const array is array<T>, a heterogeneous one array<dyn> (its
 * individual elements are still exact via const-folding of any constant-index
 * access at parse time). Same for dict keys/values.
 */
STyRef Inferencer::sty_from_value(const EvalValue &v)
{
    Type *t = v.get_type();
    switch (t->t) {

        case Type::t_int:   return A.int_ty();
        case Type::t_float: return A.float_ty();
        case Type::t_str:   return A.str_ty();
        case Type::t_none:  return A.none_ty();

        case Type::t_arr: {
            const SharedArrayObj &arr = v.get<SharedArrayObj>();

            /*
             * Flat (unboxed) storage already pins the element type, so read it
             * from the kind - crucially WITHOUT get_view(), which would promote
             * the const value to general (see plans/typed-arrays.md).
             */
            if (arr.skind() == SharedArrayObj::Storage::ints)
                return A.array_of(A.int_ty());
            if (arr.skind() == SharedArrayObj::Storage::floats)
                return A.array_of(A.float_ty());

            ArrayConstView view = arr.get_view();
            if (view.size() == 0)
                return A.array_of(A.none_ty());
            STyRef el = sty_from_value(view[0].get());
            for (size_type i = 1; i < view.size(); i++) {
                STyRef j = A.join(el, sty_from_value(view[i].get()));
                el = j ? j : A.dyn_ty();
            }
            return A.array_of(el);
        }

        case Type::t_dict: {
            const auto &m = v.get<intrusive_ptr<DictObject>>()->get_ref();
            if (m.empty())
                return A.dict_of(A.none_ty(), A.none_ty());
            STyRef k = nullptr, val = nullptr;
            for (const auto &kv : m) {
                STyRef kt = sty_from_value(kv.first);
                STyRef vt = sty_from_value(kv.second.get());
                if (!k) {
                    k = kt; val = vt;
                } else {
                    STyRef jk = A.join(k, kt);
                    STyRef jv = A.join(val, vt);
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

STyRef Inferencer::type_of(const Construct *e)
{
    if (!e)
        return A.none_ty();

    if (dynamic_cast<const LiteralInt *>(e))   return A.int_ty();
    if (dynamic_cast<const LiteralFloat *>(e)) return A.float_ty();
    if (dynamic_cast<const LiteralStr *>(e))   return A.str_ty();
    if (dynamic_cast<const LiteralNone *>(e))  return A.none_ty();

    if (auto *la = dynamic_cast<const LiteralArray *>(e)) {
        if (la->elems.empty())
            return A.array_of(A.none_ty());
        STyRef el = type_of(la->elems[0].get());
        for (size_t i = 1; i < la->elems.size(); i++) {
            STyRef j = A.join(el, type_of(la->elems[i].get()));
            el = j ? j : A.dyn_ty();
        }
        return A.array_of(el);
    }

    if (auto *ld = dynamic_cast<const LiteralDict *>(e)) {
        if (ld->elems.empty())
            return A.dict_of(A.none_ty(), A.none_ty());
        STyRef k = type_of(ld->elems[0]->key.get());
        STyRef val = type_of(ld->elems[0]->value.get());
        for (size_t i = 1; i < ld->elems.size(); i++) {
            STyRef jk = A.join(k, type_of(ld->elems[i]->key.get()));
            STyRef jv = A.join(val, type_of(ld->elems[i]->value.get()));
            k = jk ? jk : A.dyn_ty();
            val = jv ? jv : A.dyn_ty();
        }
        return A.dict_of(k, val);
    }

    if (auto *lo = dynamic_cast<const LiteralObj *>(e))
        return sty_from_value(lo->literal_value());

    if (auto *id = dynamic_cast<const Identifier *>(e)) {
        auto it = id_sym.find(id);
        if (it != id_sym.end() && it->second) {
            TypeSym *s = it->second;
            if (s->func)
                return func_sty(s->func);
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
        STyRef t = type_of(mo->elems[0].second.get());
        for (size_t i = 1; i < mo->elems.size(); i++)
            t = binop_result(mo->elems[i].first, t,
                             type_of(mo->elems[i].second.get()));
        return t;
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
            if (s && s->func)
                return s->func->ret;
            if (s && is_func(s->type))
                return sty_resolve(s->type)->ret;
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
        STyRef ct = type_of(callee);
        if (is_unknown(sty_resolve(ct))) return bottom;   /* defer */
        return is_func(ct) ? sty_resolve(ct)->ret : A.dyn_ty();
    }

    if (auto *sub = dynamic_cast<const Subscript *>(e)) {
        STyRef w = sty_resolve(type_of(sub->what.get()));
        /* Defer on Unknown OR None: a None base is a fixpoint transient (the
         * element of an array(N) before its element type is pinned); a genuine
         * non-subscriptable base is caught in the check pass. */
        if (is_unknown(w) || is_none(w)) return bottom;
        if (w->kind == STyKind::Array) return w->elem;
        /* A dict read is non-opt: `d[k]` yields the value, the dict's default
         * (a default dict), or throws on a missing key - never none. Use
         * get()/get!() for explicit nullable / fail-fast lookup. */
        if (w->kind == STyKind::Dict)  return w->val;
        if (w->kind == STyKind::Str)   return A.str_ty();
        return A.dyn_ty();
    }

    if (auto *sl = dynamic_cast<const Slice *>(e)) {
        STyRef w = sty_resolve(type_of(sl->what.get()));
        if (is_unknown(w) || is_none(w)) return bottom;   /* defer */
        if (w->kind == STyKind::Array || w->kind == STyKind::Str)
            return w;
        return A.dyn_ty();
    }

    if (auto *mem = dynamic_cast<const MemberExpr *>(e)) {
        STyRef w = sty_resolve(type_of(mem->what.get()));
        if (is_unknown(w) || is_none(w)) return bottom;   /* defer */
        /* `d.key` mirrors `d[key]`: non-opt (value / default / throws). */
        if (w->kind == STyKind::Dict)
            return w->val;
        return A.dyn_ty();
    }

    if (auto *fd = dynamic_cast<const FuncDeclStmt *>(e)) {
        auto it = func_of_decl.find(fd);
        if (it != func_of_decl.end())
            return func_sty(it->second);
        return A.dyn_ty();
    }

    return A.dyn_ty();
}

STyRef Inferencer::unary_result(Op op, STyRef a)
{
    if (op == Op::invalid)
        return a;
    if (op == Op::lnot)
        return A.int_ty();
    if (op == Op::plus || op == Op::minus) {
        STyRef u = strip(sty_resolve(a));
        if (is_unknown(u)) return bottom;   /* defer: operand not yet known */
        if (u->kind == STyKind::Int || u->kind == STyKind::Float)
            return u;
        return A.dyn_ty();
    }
    return A.dyn_ty();
}

STyRef Inferencer::binop_result(Op op, STyRef a, STyRef b)
{
    a = sty_resolve(a);
    b = sty_resolve(b);

    /* comparisons / equality / logical always yield int */
    if (op == Op::eq || op == Op::noteq || op == Op::lt || op == Op::gt ||
        op == Op::le || op == Op::ge || op == Op::land || op == Op::lor)
        return A.int_ty();

    /* `str + anything` stringifies the RHS and yields str - even a dyn/none RHS
     * (so a `s = s + e` accumulator over a dyn element stays str). Handle it
     * before the dyn short-circuit below. */
    if (op == Op::plus && strip(a)->kind == STyKind::Str)
        return A.str_ty();

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

    STyRef au = strip(a), bu = strip(b);
    const bool an = is_num(au), bn = is_num(bu);

    switch (op) {
        case Op::plus:
            /* str + anything -> str (the RHS is stringified); but int/float +
             * str is an error (handled by falling through to dyn). */
            if (au->kind == STyKind::Str)
                return A.str_ty();
            if (an && bn) {
                STyRef j = A.join(au, bu);
                return j ? j : A.dyn_ty();
            }
            if (au->kind == STyKind::Array && bu->kind == STyKind::Array) {
                STyRef j = A.join(au, bu);
                return j ? j : A.array_of(A.dyn_ty());
            }
            return A.dyn_ty();

        case Op::times:
            if (au->kind == STyKind::Str && bu->kind == STyKind::Int)
                return A.str_ty();
            if (an && bn) {
                STyRef j = A.join(au, bu);
                return j ? j : A.dyn_ty();
            }
            return A.dyn_ty();

        case Op::minus:
        case Op::div:
        case Op::mod:
            if (an && bn) {
                STyRef j = A.join(au, bu);
                return j ? j : A.dyn_ty();
            }
            return A.dyn_ty();

        default:
            return A.dyn_ty();
    }
}

/*
 * Result type of a builtin call. Precise where it matters downstream;
 * `dyn` otherwise (sound - the runtime still type-checks the call). See
 * plans/type-inference-questions.md Q8.
 */
STyRef Inferencer::builtin_result(const UniqueId *name, ExprList *args)
{
    const std::string n(name->val);
    auto arg = [&](size_t i) -> STyRef {
        return i < args->elems.size() ? type_of(args->elems[i].get())
                                      : A.dyn_ty();
    };
    auto elem_of = [&](STyRef c) -> STyRef {
        c = sty_resolve(c);
        if (is_unknown(c) || is_none(c)) return bottom;   /* defer */
        if (c->kind == STyKind::Array) return c->elem;
        if (c->kind == STyKind::Str)   return A.str_ty();
        if (c->kind == STyKind::Dict)  return c->key;
        return A.dyn_ty();
    };

    /* int-returning */
    if (n == "len" || n == "hash" || n == "ord" || n == "rand" ||
        n == "intptr" || n == "defined" || n == "isconst" ||
        n == "isconstdecl" || n == "ispure" || n == "ispuredecl" ||
        n == "startswith" || n == "endswith" || n == "isinf" ||
        n == "isfinite" || n == "isnormal" || n == "isnan" ||
        n == "remove")
        return A.int_ty();

    /* float-returning */
    if (n == "float" || n == "exp" || n == "log" || n == "sqrt" ||
        n == "cbrt" || n == "pow" || n == "sin" || n == "cos" || n == "tan" ||
        n == "asin" || n == "acos" || n == "atan" || n == "ceil" ||
        n == "floor" || n == "trunc" || n == "round" || n == "randf" ||
        n == "math_e" || n == "math_pi" || n == "nan" || n == "inf" ||
        n == "eps")
        return A.float_ty();

    if (n == "int")    return A.int_ty();
    if (n == "str" || n == "type" || n == "chr" || n == "join" ||
        n == "lpad" || n == "rpad" || n == "lstrip" || n == "rstrip" ||
        n == "strip" || n == "readln" || n == "read" || n == "tmpdir")
        return A.str_ty();

    if (n == "split" || n == "splitlines" || n == "readlines")
        return A.array_of(A.str_ty());

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
        STyRef f = sty_resolve(arg(1));
        if (is_unknown(f)) return bottom;   /* defer: callback not yet known */
        return A.array_of(is_func(f) ? f->ret : A.dyn_ty());
    }
    if (n == "dict") {
        /* dict(default_value) -> dict<dyn, typeof default> (a default dict, so
         * d[k] is non-opt). dict(pairs) -> dict<dyn,dyn>. A non-array arg is
         * the default value, which becomes the dict's value type. */
        if (args && args->elems.size() == 1) {
            STyRef a0 = sty_resolve(arg(0));
            if (is_unknown(a0)) return bottom;
            if (a0->kind != STyKind::Array && a0->kind != STyKind::None)
                return A.dict_of(A.dyn_ty(), a0);
        }
        return A.dict_of(A.dyn_ty(), A.dyn_ty());
    }

    /* get(d,key) -> opt V (nullable lookup); get!(d,key) -> V (or throws, so
     * the result is non-opt). */
    if (n == "get" || n == "get!") {
        STyRef d = sty_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        STyRef v = d->kind == STyKind::Dict ? d->val : A.dyn_ty();
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
        STyRef t = arg(0);
        for (size_t i = 1; i < args->elems.size(); i++) {
            if (is_unknown(sty_resolve(t)) ||
                is_unknown(sty_resolve(arg(i))))
                return bottom;          /* defer */
            STyRef j = A.join(t, arg(i));
            t = j ? j : A.dyn_ty();
        }
        return t;
    }

    if (n == "sum" || n == "top" || n == "pop")
        return elem_of(arg(0));

    if (n == "map") {
        /* map(func, container) -> array of the callback's return type */
        STyRef f = sty_resolve(arg(0));
        if (is_unknown(f)) return bottom;   /* defer: callback not yet known */
        return A.array_of(is_func(f) ? f->ret : A.dyn_ty());
    }
    if (n == "filter")              /* filter(func, container) -> container */
        return arg(1);
    if (n == "sort" || n == "rev_sort" || n == "reverse")
        return arg(0);             /* sort(array, [func]) -> array */

    if (n == "keys") {
        STyRef d = sty_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        return A.array_of(d->kind == STyKind::Dict ? d->key : A.dyn_ty());
    }
    if (n == "values") {
        STyRef d = sty_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        return A.array_of(d->kind == STyKind::Dict ? d->val : A.dyn_ty());
    }
    if (n == "kvpairs") {
        /* dict<k,v> -> array of [k, v] pairs, i.e. array<array<join(k,v)>>. */
        STyRef d = sty_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        if (d->kind != STyKind::Dict) return A.array_of(A.dyn_ty());
        STyRef pair = A.join(d->key, d->val);
        return A.array_of(A.array_of(pair ? pair : A.dyn_ty()));
    }
    if (n == "find") {
        /*
         * find(dict, key) -> opt val; find(array, x) / find(str, sub) -> the
         * opt int index of the first match (or none). Only a fully-unknown,
         * non-container base stays opt dyn.
         */
        STyRef d = sty_resolve(arg(0));
        if (is_unknown(d)) return bottom;   /* defer */
        if (d->kind == STyKind::Dict) return A.with_opt(d->val, true);
        if (d->kind == STyKind::Array || d->kind == STyKind::Str)
            return A.with_opt(A.int_ty(), true);
        return A.with_opt(A.dyn_ty(), true);
    }

    if (n == "exception" || n == "ex")
        return A.exc_ty();

    if (n == "print" || n == "writeln" || n == "assert" || n == "append" ||
        n == "push" || n == "insert" || n == "exit" || n == "write" ||
        n == "writelines" || n == "undef" || n == "erase")
        return A.none_ty();

    return A.dyn_ty();
}

/* ------------------------------- fixpoint -------------------------------- */

void Inferencer::reset_round()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func)
            continue;
        s->acc = s->dyn_decl ? A.dyn_ty() : bottom;
    }
    for (auto &up : all_funcs)
        up->ret_acc = bottom;
}

void Inferencer::commit_round()
{
    for (auto &up : all_syms) {
        TypeSym *s = up.get();
        if (s->func)
            continue;
        if (!sty_equal(s->acc, s->type))
            changed = true;
        s->type = s->acc;
    }
    for (auto &up : all_funcs) {
        if (!sty_equal(up->ret_acc, up->ret))
            changed = true;
        up->ret = up->ret_acc;
    }
}

void Inferencer::contribute(TypeSym *s, STyRef t, Loc loc)
{
    /*
     * func-name syms derive their type from func_sty(); never accumulated.
     * A dyn *var* DOES accumulate (Phase B): join keeps the type dyn but tracks
     * the opt bit, so `var dyn d = 5; d = none;` infers `opt dyn`. (A dyn param
     * is skipped in contribute_arg - its opt is declared, not inferred.)
     */
    if (!s || s->func)
        return;
    STyRef nw = A.join(s->acc, t);
    if (!nw)
        mismatch("'" + std::string(s->name->val) + "' has type '" +
                     sty_to_string(s->acc) + "' but is assigned '" +
                     sty_to_string(t) + "'",
                 loc, Loc());
    s->acc = nw;
}

void Inferencer::contribute_arg(TypeSym *param, STyRef argT, Loc loc)
{
    if (!param || param->dyn_decl)
        return;
    argT = sty_resolve(argT);
    if (!param->opt_decl) {
        if (argT->kind == STyKind::None)
            return;                  /* none->non-opt: flagged in check */
        argT = strip(argT);
    }
    contribute(param, argT, loc);
}

void Inferencer::contribute_ret(STyRef t)
{
    if (!cur_func)
        return;
    STyRef nw = A.join(cur_func->ret_acc, t);
    cur_func->ret_acc = nw ? nw : A.dyn_ty();   /* return conflict -> dyn */
}

void Inferencer::accumulate(Construct *n)
{
    if (!n)
        return;

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        FuncInfo *prev = cur_func;
        cur_func = func_of_decl[fd];
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
        STyRef vt = type_of(args->elems[val_i].get());
        STyRef bt = sty_resolve(bs->type);
        if (bt->kind == STyKind::Dict) {
            STyRef kt = (key_i >= 0 && (size_t)key_i < args->elems.size())
                            ? type_of(args->elems[key_i].get()) : A.dyn_ty();
            contribute(bs, A.dict_of(kt, vt), bid->start);
        } else if (bt->kind == STyKind::Array) {
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
        } else {
            return;
        }
        if (func_i >= args->elems.size() || cont_i >= args->elems.size())
            return;
        FuncInfo *cb = callee_funcinfo(args->elems[func_i].get());
        if (!cb)
            return;
        STyRef ct = sty_resolve(type_of(args->elems[cont_i].get()));
        STyRef el = ct->kind == STyKind::Array ? ct->elem
                  : ct->kind == STyKind::Dict  ? ct->key : A.dyn_ty();
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
    STyRef rt = sty_resolve(type_of(rvalue));

    if (auto *la = dynamic_cast<LiteralArray *>(rvalue)) {
        if (la->elems.size() == idl->elems.size()) {
            for (size_t i = 0; i < idl->elems.size(); i++)
                contribute(id_sym[idl->elems[i].get()],
                           type_of(la->elems[i].get()), idl->elems[i]->start);
            return;
        }
    }

    STyRef each = rt->kind == STyKind::Array ? rt->elem : rt;
    for (auto &id : idl->elems)
        contribute(id_sym[id.get()], each, id->start);
}

void Inferencer::accumulate_assign(Expr14 *e)
{
    accumulate(e->rvalue.get());
    if (!(e->fl & pFlags::pInDecl))
        accumulate(e->lvalue.get());

    STyRef ct;
    if (e->op == Op::assign) {
        ct = type_of(e->rvalue.get());
    } else {
        STyRef l = type_of(e->lvalue.get());
        STyRef r = type_of(e->rvalue.get());
        ct = binop_result(compound_binop(e->op), l, r);
        /* An invalid compound op (e.g. str -= int) yields dyn; don't let that
         * widen the target to dyn and hide the error - keep its type so the
         * check pass reports the mismatch. */
        if (is_dyn(ct) && !is_dyn(strip(l)) && !is_dyn(strip(r)))
            ct = l;
    }

    Construct *lv = e->lvalue.get();

    if (auto *id = dynamic_cast<Identifier *>(lv)) {
        auto it = id_sym.find(id);
        if (it != id_sym.end())
            contribute(it->second, ct, e->start);
        return;
    }

    if (auto *idl = dynamic_cast<IdList *>(lv)) {
        spread_idlist(idl, e->rvalue.get());
        return;
    }

    if (auto *sub = dynamic_cast<Subscript *>(lv)) {
        if (auto *bid = dynamic_cast<Identifier *>(sub->what.get())) {
            auto it = id_sym.find(bid);
            if (it != id_sym.end() && it->second) {
                /* Only contribute once the base kind is known (from a prior
                 * literal/assignment); guessing array for an as-yet-Unknown
                 * base would spuriously conflict with a dict. */
                STyRef bt = sty_resolve(it->second->type);
                if (bt->kind == STyKind::Dict)
                    contribute(it->second,
                               A.dict_of(type_of(sub->index.get()), ct),
                               bid->start);
                else if (bt->kind == STyKind::Array)
                    contribute(it->second, A.array_of(ct), bid->start);
            }
        }
        return;
    }

    if (auto *mem = dynamic_cast<MemberExpr *>(lv)) {
        if (auto *bid = dynamic_cast<Identifier *>(mem->what.get())) {
            auto it = id_sym.find(bid);
            if (it != id_sym.end())
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

    STyRef c = sty_resolve(type_of(fe->container.get()));

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
        /* enumerate-style: first id is the int index, the rest the element */
        contribute(sym_of(0), A.int_ty(), ids[0]->start);
        STyRef el = c->kind == STyKind::Array ? c->elem
                    : c->kind == STyKind::Str ? A.str_ty()
                    : c->kind == STyKind::Dict ? c->key : A.dyn_ty();
        for (size_t i = 1; i < ids.size(); i++)
            contribute(sym_of(i), el, ids[i]->start);
        return;
    }

    if (c->kind == STyKind::Dict && ids.size() >= 2) {
        contribute(sym_of(0), c->key, ids[0]->start);
        contribute(sym_of(1), c->val, ids[1]->start);
        return;
    }

    STyRef el = c->kind == STyKind::Array ? c->elem
                : c->kind == STyKind::Str ? A.str_ty()
                : c->kind == STyKind::Dict ? c->key : A.dyn_ty();

    if (ids.size() == 1) {
        contribute(sym_of(0), el, ids[0]->start);
    } else {
        /* tuple-unpack each element (an array) into the ids */
        STyRef inner = sty_resolve(el)->kind == STyKind::Array
                           ? sty_resolve(el)->elem : el;
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

void Inferencer::require_nonopt(STyRef t, Loc s, Loc e, const char *what)
{
    /*
     * Phase B: nullability is checked even for dyn. A plain `dyn` is non-opt
     * (is_optish false) and passes - its *type* operations are runtime-checked,
     * but it is proven non-none. An `opt dyn` is nullable and must be narrowed,
     * exactly like `opt int`.
     */
    if (is_optish(t))
        nullability(std::string("possibly-none value used ") + what +
                        " (type '" + sty_to_string(t) + "')",
                    s, e);
}

void Inferencer::check_binops(MultiOpConstruct *mo, bool comparison,
                              bool logical, bool arith)
{
    for (auto &pr : mo->elems)
        check(pr.second.get());

    STyRef left = type_of(mo->elems[0].second.get());

    for (size_t i = 1; i < mo->elems.size(); i++) {
        Op op = mo->elems[i].first;
        Construct *rnode = mo->elems[i].second.get();
        STyRef right = type_of(rnode);

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
                STyRef l = strip(sty_resolve(left));
                STyRef r = strip(sty_resolve(right));
                if (!is_dyn(l) && !is_dyn(r)) {
                    bool ok = (is_num(l) && is_num(r)) ||
                              (l->kind == STyKind::Str &&
                               r->kind == STyKind::Str);
                    if (!ok)
                        mismatch("cannot compare '" + sty_to_string(left) +
                                     "' with '" + sty_to_string(right) + "'",
                                 mo->start, rnode->end);
                }
            }
        } else if (arith) {
            require_nonopt(left, mo->start, mo->end,
                           "in an arithmetic operation");
            require_nonopt(right, rnode->start, rnode->end,
                           "in an arithmetic operation");
            STyRef res = binop_result(op, left, right);
            STyRef l = strip(sty_resolve(left)), r = strip(sty_resolve(right));
            if (!is_dyn(l) && !is_dyn(r) && is_dyn(res)) {
                /* binop_result returns dyn for an invalid combination */
                mismatch("operator does not apply to '" + sty_to_string(left) +
                             "' and '" + sty_to_string(right) + "'",
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
            STyRef old = had ? narrowed[nsym] : nullptr;
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

void Inferencer::check(Construct *n)
{
    if (!n)
        return;

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        FuncInfo *prev = cur_func;
        auto it = func_of_decl.find(fd);
        cur_func = (it != func_of_decl.end()) ? it->second : nullptr;
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
        check_call(call);
        return;
    }

    if (dynamic_cast<Expr03 *>(n) || dynamic_cast<Expr04 *>(n)) {
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
            STyRef t = type_of(e2->elems[0].second.get());
            require_nonopt(t, n->start, n->end, "with a unary +/-");
            STyRef u = strip(sty_resolve(t));
            if (!is_dyn(u) && !is_unknown(u) && !is_num(u))
                mismatch("unary +/- needs a number, got '" + sty_to_string(t) +
                             "'", n->start, n->end);
        }
        return;
    }

    if (auto *sub = dynamic_cast<Subscript *>(n)) {
        check(sub->what.get());
        check(sub->index.get());
        STyRef w = type_of(sub->what.get());
        require_nonopt(w, sub->what->start, sub->what->end,
                       "as a subscript base");
        STyRef wr = strip(sty_resolve(w));
        if (!is_dyn(wr) && !is_unknown(wr) && wr->kind != STyKind::Array &&
            wr->kind != STyKind::Dict && wr->kind != STyKind::Str)
            mismatch("type '" + sty_to_string(w) + "' is not subscriptable",
                     sub->what->start, sub->what->end);
        return;
    }

    if (auto *sl = dynamic_cast<Slice *>(n)) {
        check(sl->what.get());
        check(sl->start_idx.get());
        check(sl->end_idx.get());
        STyRef w = type_of(sl->what.get());
        require_nonopt(w, sl->what->start, sl->what->end, "as a slice base");
        STyRef wr = strip(sty_resolve(w));
        if (!is_dyn(wr) && !is_unknown(wr) && wr->kind != STyKind::Array &&
            wr->kind != STyKind::Str)
            mismatch("type '" + sty_to_string(w) + "' is not sliceable",
                     sl->what->start, sl->what->end);
        return;
    }

    if (auto *mem = dynamic_cast<MemberExpr *>(n)) {
        check(mem->what.get());
        STyRef w = type_of(mem->what.get());
        require_nonopt(w, mem->what->start, mem->what->end,
                       "as a member-access base");
        STyRef wr = strip(sty_resolve(w));
        if (!is_dyn(wr) && !is_unknown(wr) && wr->kind != STyKind::Dict)
            mismatch("type '" + sty_to_string(w) +
                         "' has no members (only a dict does)",
                     mem->what->start, mem->what->end);
        return;
    }

    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        check(fe->container.get());
        check(fe->body.get());
        STyRef c = type_of(fe->container.get());
        require_nonopt(c, fe->container->start, fe->container->end,
                       "as a foreach container");
        return;
    }

    if (auto *e14 = dynamic_cast<Expr14 *>(n)) {
        check(e14->rvalue.get());
        if (!(e14->fl & pFlags::pInDecl))
            check(e14->lvalue.get());
        if (e14->op != Op::assign) {
            /* compound assign: validate the implied binary op */
            STyRef l = type_of(e14->lvalue.get());
            STyRef r = type_of(e14->rvalue.get());
            require_nonopt(l, e14->lvalue->start, e14->lvalue->end,
                           "in a compound assignment");
            require_nonopt(r, e14->rvalue->start, e14->rvalue->end,
                           "in a compound assignment");
            STyRef res = binop_result(compound_binop(e14->op), l, r);
            STyRef ls = strip(sty_resolve(l)), rs = strip(sty_resolve(r));
            if (!is_dyn(ls) && !is_dyn(rs) && is_dyn(res))
                mismatch("operator does not apply to '" + sty_to_string(l) +
                             "' and '" + sty_to_string(r) + "'",
                         e14->start, e14->end);
        }
        return;
    }

    if (auto *th = dynamic_cast<ThrowStmt *>(n)) {
        check(th->elem.get());
        STyRef t = strip(sty_resolve(type_of(th->elem.get())));
        if (!is_dyn(t) && t->kind != STyKind::Exception &&
            t->kind != STyKind::Unknown && t->kind != STyKind::None)
            mismatch("can only throw an exception, got '" +
                         sty_to_string(type_of(th->elem.get())) + "'",
                     th->elem->start, th->elem->end);
        return;
    }

    for_each_child(n, [&](Construct *c) { check(c); });
}

void Inferencer::check_call(CallExpr *call)
{
    check(call->what.get());
    for (auto &a : call->args->elems)
        check(a.get());

    ExprList *args = call->args.get();

    /* resolve the callee to a parameter list (FuncInfo or a Func STy) */
    std::vector<TypeSym *> *fparams = nullptr;
    const STy *fsty = nullptr;
    bool callable_known = false;

    if (FuncInfo *fi = callee_funcinfo(call->what.get())) {
        fparams = &fi->params;
        callable_known = true;
    } else if (auto *cid = dynamic_cast<Identifier *>(call->what.get())) {
        auto it = id_sym.find(cid);
        TypeSym *s = (it != id_sym.end()) ? it->second : nullptr;
        if (s && s->func) {
            fparams = &s->func->params;
            callable_known = true;
        } else if (s && is_func(s->type)) {
            fsty = sty_resolve(s->type);
            callable_known = true;
        } else if (s && is_dyn(s->type)) {
            return;                       /* dyn callee: no checks */
        } else if (!s && is_builtin(cid->uid)) {
            return;                       /* builtin arity checked at runtime */
        } else if (s) {
            mismatch("'" + std::string(cid->uid->val) +
                         "' is not callable (type '" + sty_to_string(s->type) +
                         "')", call->what->start, call->what->end);
        } else {
            return;                       /* unresolved -> dyn (Q4) */
        }
    } else {
        STyRef ct = type_of(call->what.get());
        if (is_dyn(ct))
            return;
        if (is_func(ct)) {
            fsty = sty_resolve(ct);
            callable_known = true;
        } else {
            mismatch("expression of type '" + sty_to_string(ct) +
                         "' is not callable", call->what->start,
                     call->what->end);
        }
    }

    if (!callable_known)
        return;

    size_t nparams = fparams ? fparams->size() : fsty->params.size();
    size_t nargs = args->elems.size();

    if (nargs != nparams)
        argcount("function expects " + std::to_string(nparams) +
                     " argument(s), got " + std::to_string(nargs),
                 call->start, call->end);

    for (size_t i = 0; i < nargs && i < nparams; i++) {
        Construct *anode = args->elems[i].get();
        STyRef at = type_of(anode);

        bool p_dyn, p_opt;
        STyRef ptype;
        if (fparams) {
            p_dyn = (*fparams)[i]->dyn_decl;
            p_opt = (*fparams)[i]->opt_decl;
            ptype = (*fparams)[i]->type;
        } else {
            p_dyn = sty_resolve(fsty->params[i])->kind == STyKind::Dyn;
            p_opt = i < fsty->param_opt.size() && fsty->param_opt[i];
            ptype = fsty->params[i];
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

        STyRef src = p_opt ? at : strip(at);
        if (!sty_assignable(src, ptype))
            mismatch("argument " + std::to_string(i + 1) + " has type '" +
                         sty_to_string(at) + "' but the parameter is '" +
                         sty_to_string(ptype) + "'",
                     anode->start, anode->end);
    }
}

/* ============================ M8 specializer ============================= */

/* All operands of a MultiOpConstruct are statically scalar? `allow_float` also
 * accepts float operands (for a float-result op / a float comparison). */
static bool ops_scalar(const MultiOpConstruct *mo, bool allow_float)
{
    for (const auto &pr : mo->elems) {
        const TypeHint t = pr.second->th;
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
    t->start = mo->start;
    t->end = mo->end;
    t->is_const = mo->is_const;
    t->th = result_th;
    t->elems = std::move(mo->elems);
    return t;
}

/* Rewrite a single node into a TypedScalarExpr when its operands are statically
 * int/float; else return it unchanged. Children are already specialized. */
static unique_ptr<Construct> try_specialize(unique_ptr<Construct> n)
{
    /* arithmetic: Expr03 (* / %), Expr04 (+ -) */
    if (dynamic_cast<Expr03 *>(n.get()) || dynamic_cast<Expr04 *>(n.get())) {
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
            const bool anyf = mo->elems[0].second->th == TypeHint::f ||
                              mo->elems[1].second->th == TypeHint::f;
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

static unique_ptr<Construct> specialize(unique_ptr<Construct> n);

/* Recurse into every unique_ptr<Construct> child, specializing it in place. */
static void specialize_children(Construct *n)
{
    if (auto *e = dynamic_cast<Expr14 *>(n)) {
        e->lvalue = specialize(std::move(e->lvalue));
        e->rvalue = specialize(std::move(e->rvalue));
        return;
    }
    if (auto *c = dynamic_cast<CallExpr *>(n)) {
        c->what = specialize(std::move(c->what));
        for (auto &a : c->args->elems)
            a = specialize(std::move(a));
        return;
    }
    if (auto *m = dynamic_cast<MemberExpr *>(n)) {
        m->what = specialize(std::move(m->what));
        return;
    }
    if (auto *s = dynamic_cast<Subscript *>(n)) {
        s->what = specialize(std::move(s->what));
        s->index = specialize(std::move(s->index));
        return;
    }
    if (auto *s = dynamic_cast<Slice *>(n)) {
        s->what = specialize(std::move(s->what));
        if (s->start_idx) s->start_idx = specialize(std::move(s->start_idx));
        if (s->end_idx)   s->end_idx = specialize(std::move(s->end_idx));
        return;
    }
    if (auto *i = dynamic_cast<IfStmt *>(n)) {
        i->condExpr = specialize(std::move(i->condExpr));
        i->thenBlock = specialize(std::move(i->thenBlock));
        if (i->elseBlock) i->elseBlock = specialize(std::move(i->elseBlock));
        return;
    }
    if (auto *w = dynamic_cast<WhileStmt *>(n)) {
        w->condExpr = specialize(std::move(w->condExpr));
        if (w->body) w->body = specialize(std::move(w->body));
        return;
    }
    if (auto *f = dynamic_cast<ForStmt *>(n)) {
        if (f->init) f->init = specialize(std::move(f->init));
        if (f->cond) f->cond = specialize(std::move(f->cond));
        if (f->inc)  f->inc = specialize(std::move(f->inc));
        if (f->body) f->body = specialize(std::move(f->body));
        return;
    }
    if (auto *fe = dynamic_cast<ForeachStmt *>(n)) {
        fe->container = specialize(std::move(fe->container));
        if (fe->body) fe->body = specialize(std::move(fe->body));
        return;
    }
    if (auto *t = dynamic_cast<TryCatchStmt *>(n)) {
        if (t->tryBody) t->tryBody = specialize(std::move(t->tryBody));
        for (auto &cs : t->catchStmts)
            if (cs.second) cs.second = specialize(std::move(cs.second));
        if (t->finallyBody)
            t->finallyBody = specialize(std::move(t->finallyBody));
        return;
    }
    if (auto *r = dynamic_cast<ReturnStmt *>(n)) {
        if (r->elem) r->elem = specialize(std::move(r->elem));
        return;
    }
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(n)) {
        if (fd->body) fd->body = specialize(std::move(fd->body));
        return;
    }
    if (auto *ld = dynamic_cast<LiteralDict *>(n)) {
        for (auto &kv : ld->elems) {
            kv->key = specialize(std::move(kv->key));
            kv->value = specialize(std::move(kv->value));
        }
        return;
    }
    if (auto *sc = dynamic_cast<SingleChildConstruct *>(n)) {
        if (sc->elem) sc->elem = specialize(std::move(sc->elem));
        return;
    }
    if (auto *mo = dynamic_cast<MultiOpConstruct *>(n)) {
        for (auto &pr : mo->elems)
            pr.second = specialize(std::move(pr.second));
        return;
    }
    if (auto *me = dynamic_cast<MultiElemConstruct<> *>(n)) {
        /* Block, LiteralArray, ExprList */
        for (auto &e : me->elems)
            e = specialize(std::move(e));
        return;
    }
}

static unique_ptr<Construct> specialize(unique_ptr<Construct> n)
{
    if (!n)
        return n;
    specialize_children(n.get());     /* bottom-up: children first */
    return try_specialize(std::move(n));
}

}  /* anonymous namespace */

void infer_types(Construct *root, bool enable, bool strict)
{
    if (!enable || !root)
        return;

    Inferencer inf(root);
    inf.strict_dyn = strict;
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

        STyRef ty = sty_resolve(s->type);
        if (ty->kind != STyKind::Array)
            continue;

        STyRef el = sty_resolve(ty->elem);
        AnnoKind k;
        if (!el->opt && (el->kind == STyKind::Int ||
                         el->kind == STyKind::Float))
            k = AnnoKind::flat_array;
        else if (el->kind == STyKind::Dyn)
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

void specialize_types(Construct *root, bool enable)
{
    if (!enable || !root)
        return;

    auto *blk = dynamic_cast<Block *>(root);
    if (!blk)
        return;

    for (auto &e : blk->elems)
        e = specialize(std::move(e));
}
