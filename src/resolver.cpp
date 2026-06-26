/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"
#include "resolver.h"
#include "analyzer.h"
#include "errors.h"
#include "eval.h"
#include "trace.h"

#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>

/*
 * Name-resolution pass: resolve a function's PARAMETERS and LOCAL variables
 * (var/const, for-init, foreach variables, catch variables), plus eligible
 * TOP-LEVEL variables, to fixed Frame slots, so references become O(1) slot
 * reads at runtime instead of std::map lookups walking the scope chain.
 *
 * What is and isn't resolved:
 *
 *   - params, var/const, for/foreach/catch variables  -> slotted
 *   - function NAMES (FuncDeclStmt::id)                -> NOT slotted; kept in
 *         the map. They are forward-reference-able (mutual recursion), and a
 *         forward reference necessarily resolves to "unresolved" in this
 *         forward pass; a name in a slot would then not be found.
 *         A function-name declaration still creates a scope entry so it
 *         correctly SHADOWS an outer slotted binding of the same name.
 *   - top-level (module-scope) vars -> slotted in an implicit "main" frame,
 *         EXCEPT any a function reads: functions reach globals through the
 *         scope-chain map walk (not slots), so a function-read global must
 *         stay in the map. See run()'s two passes and `escaped`.
 *   - builtins, captures, anything unresolved         -> map fallback.
 *
 * Resolution is a forward walk with a lexical scope stack: a ref resolves
 * only against declarations seen earlier (no hoisting), which is what makes
 * `var x = x + 1;` in a nested block read the OUTER x for its RHS. Shadowing
 * falls out of searching the scope stack innermost-out. Frame slots persist for
 * the whole call, so Block::do_eval clears a block's slots on entry (see eval.h
 * Frame / eval.cpp Block::do_eval) to restore fresh-per-iteration semantics.
 * The resolver records each block's contiguous slot range for that.
 *
 * The pass is purely an optimization: anything it leaves unresolved still works
 * via the runtime map. The one behavior it MOVES rather than preserves is
 * duplicate-declaration detection: a same-block redeclaration now raises
 * AlreadyDefinedEx here (before execution) instead of when the second
 * declaration would have run. AlreadyDefinedEx is not script-catchable, so this
 * is only an earlier-and-always failure for genuinely duplicate code.
 */

/*
 * Defined (non-static) in parser.cpp: replace `out` with a literal Construct
 * holding the constant value `v`. Reused by the auto-const folder below.
 */
bool MakeConstructFromConstVal(const EvalValue &v, unique_ptr<Construct> &out,
                               bool process_arrays, bool immutable = false);

namespace {

/* Max slots per frame: Frame::live is one 64-bit word, one bit per slot. */
constexpr int MAX_SLOTS = 64;

void for_each_child(Construct *c, const std::function<void(Construct *)> &fn);

/*
 * A lexical scope (a function's param scope, a block, a for/foreach header, or
 * catch clause). `decls` maps each declared name to its slot, or to -1 for a
 * "masking" entry: a name that is in scope but NOT slotted (a function name, or
 * a local that overflowed the slot budget). A masking entry shadows an outer
 * slotted binding while still resolving to a runtime map lookup.
 */
struct Scope {
    /* name -> { slot (or -1 = masked/map), explicit-type annotation }. The
     * annotation is propagated to every use so an assignment can coerce a
     * widening value to the declared type (e.g. `float f; f = 3;` stores 3.0). */
    struct Decl {
        const UniqueId *name;
        int slot;
        DeclType type;
    };
    std::vector<Decl> decls;
};

/*
 * Resolution state for one function (or, eventually, the top level). Only
 * meaningful while `slottable`; otherwise declare/resolve_ref are no-ops and
 * every identifier in the function is left unresolved.
 */
struct FuncState {
    bool slottable = false;
    bool is_main = false;           /* the implicit top-level "main" frame */
    FuncDeclStmt *fd = nullptr;
    int next_slot = 0;
    std::vector<Scope> scopes;
    std::vector<int> writes;        /* per slot -> fd->slot_writes */
    std::unordered_set<const UniqueId *> captures;   /* capture names */
};

/*
 * Auto-const folding pass, run after slot resolution. Within each function (and
 * the top-level "main") it promotes write-once scalar variables - a `var`
 * assigned exactly once, with a constant scalar initializer, never reassigned,
 * captured or undef'd - to compile-time constants, propagates them into
 * expressions, folds the resulting constant arithmetic to literals, drops the
 * now-dead declarations, and removes statically-dead `if`/`while` branches.
 *
 * This is the optimization CPython lacks: a loop-invariant constant built from
 * named `var` constants collapses to one literal here, instead of being
 * recomputed every iteration. It keys off the resolver's per-slot write counts
 * (FuncDeclStmt::slot_writes + the top-level frame's) and slot identity, so a
 * reference's binding is unambiguous - no scope tracking is needed here: every
 * Identifier with the same sym.slot in a function is the same variable.
 */

/*
 * Builtins that take their FIRST argument as an lvalue or identifier (and throw
 * NotLValueEx / require an identifier otherwise). A value substituted/folded
 * there would change behavior, so neither auto-const nor the inliner folds it.
 * Every other call's arguments are safe to fold.
 */
static bool is_lvalue_arg_builtin(std::string_view name)
{
    return name == "append" || name == "push" || name == "pop"
        || name == "insert" || name == "erase" || name == "intptr"
        || name == "undef";
}

/*
 * Coerce a const-folded value to a declared scalar type when inlining a typed
 * var/const (`float f = 3` -> 3.0). Mirrors eval.cpp's coerce_to_decl_type (a
 * separate TU); the inferencer has already validated assignability, so this only
 * applies the numeric widenings (float <- int/bool, int <- bool).
 */
static EvalValue coerce_decl_scalar(const EvalValue &v, DeclType dt)
{
    if (dt == DeclType::f) {
        if (v.is<int_type>())
            return EvalValue(static_cast<float_type>(v.get<int_type>()));
        if (v.is<bool>())
            return EvalValue(static_cast<float_type>(v.get<bool>() ? 1 : 0));
    } else if (dt == DeclType::i) {
        if (v.is<bool>())
            return EvalValue(static_cast<int_type>(v.get<bool>() ? 1 : 0));
    }
    return v;
}

/* True if `v` is a read-only (const-backed) array or dict value. Mirrors the
 * static helper of the same name in parser.cpp; scalars are never read-only. */
static bool is_readonly_value(const EvalValue &v)
{
    if (v.is<SharedArrayObj>())
        return v.get<SharedArrayObj>().is_readonly();
    if (v.is<intrusive_ptr<DictObject>>())
        return v.get<intrusive_ptr<DictObject>>()->is_readonly();
    return false;
}

class AutoConst {

    EvalContext cctx;   /* const context for evaluating folded constants */
    AnalysisInfo *analysis;   /* -a: record auto-const/dead/folded, or null */

public:

    explicit AutoConst(AnalysisInfo *a = nullptr,
                       EvalContext *prior_pure = nullptr)
        : cctx(nullptr, true), analysis(a)
    {
        /* REPL: seed the fold context with the prior inputs' effectively-pure
         * functions (and their template/spec instances) so a call to one folds
         * across inputs - e.g. `func f2() => f(1,2)` where f's instance came
         * from an earlier input. Each FuncObject keeps its own capture_ctx, so
         * its body still resolves its callees against the runtime scope. Only
         * pure FUNCTIONS are seeded (never a runtime var), so folding stays
         * sound; a current-input redefinition redirects to its own (new)
         * instance, so a stale prior instance here is never used. */
        if (prior_pure) {
            std::vector<std::pair<const UniqueId *, const LValue *>> syms;
            prior_pure->collect_symbols(syms);
            for (const auto &kv : syms) {
                const EvalValue &v = kv.second->get();
                if (v.is<shared_ptr<FuncObject>>() &&
                    v.get<shared_ptr<FuncObject>>()->func->effective_pure) {
                    try {
                        cctx.emplace(kv.first->val, EvalValue(v), true);
                    } catch (const Exception &) { /* dup: skip */ }
                }
            }
        }
    }

    void run(Block *root, const std::vector<int> &main_writes)
    {
        register_pure_funcs(root);
        fold_function(root, main_writes, nullptr);
    }

    /*
     * Specialization fold (used by the Inliner): like fold_function, but with
     * parameter slots pre-bound to constant values (`seed`). Reads of those
     * params fold to literals and everything that becomes const folds too, with
     * dead-code elimination. A seeded slot that is "blocked" (used as an
     * lvalue: subscript/member base, lvalue builtin arg, capture, foreach var)
     * is NOT bound, so such a use is never folded unsoundly. Returns false if
     * folding raised a const error (e.g. 6/0 reachable): the caller then keeps
     * the ordinary call, so a runtime error is never turned into a compile one.
     */
    bool fold_specialized(Block *b,
                          const std::vector<int> &writes,
                          const IdList *params,
                          const std::unordered_map<int, EvalValue> &seed)
    {
        FCtx fc{ writes, {}, {}, params };
        prescan_blocked(b, fc.blocked);

        /* A read-only array/dict const may also be substituted as a
         * subscript/member READ base, which the strict set blocks; the relaxed
         * set keeps every other block (capture, lvalue-builtin first arg,
         * callee, foreach var). Scalars stay on the strict set. */
        std::unordered_set<int> arr_blocked;
        prescan_blocked(b, arr_blocked, /*block_subscript_bases=*/false);

        for (const auto &kv : seed) {
            const bool arr = kv.second.is<SharedArrayObj>()
                          || kv.second.is<intrusive_ptr<DictObject>>();
            const std::unordered_set<int> &blk = arr ? arr_blocked : fc.blocked;
            if (!blk.count(kv.first))
                fc.consts.emplace(kv.first, kv.second);
        }

        try {
            fold_block(b, fc);
        } catch (const Exception &) {
            return false;
        }

        return true;
    }

private:

    struct FCtx {
        const std::vector<int> &writes;   /* per-slot write counts */
        std::unordered_map<int, EvalValue> consts;  /* promoted: slot->value */
        std::unordered_set<int> blocked;  /* slots that can't be promoted */
        const IdList *params = nullptr;   /* params (slots 0..n-1) */
    };

    bool promotable(const FCtx &fc, int slot) const
    {
        return slot >= 0 && slot < static_cast<int>(fc.writes.size())
            && fc.writes[slot] == 1 && !fc.blocked.count(slot);
    }

    /* A scalar literal (int/float/str/none); arrays/dicts are not Literals. */
    static bool is_scalar_literal(const Construct *c)
    {
        return dynamic_cast<const Literal *>(c) != nullptr;
    }

    /*
     * Resolve isconst(arg) / isconstdecl(arg) to a literal 0/1. isconstdecl is
     * true only for things constant by DECLARATION (a parse-time const - a
     * literal, explicit `const`, or const expression - or a `const` param);
     * isconst is also true for an auto-const var (foldable here) or auto-const
     * param.
     */
    void fold_isconst(unique_ptr<Construct> &slot, CallExpr *ce, FCtx &fc,
                      bool decl_only)
    {
        unique_ptr<Construct> &arg = ce->args->elems[0];
        bool result;
        auto *id = dynamic_cast<Identifier *>(arg.get());

        if (id && id->sym.kind == SymKind::local && fc.params
                && id->sym.slot >= 0
                && id->sym.slot < static_cast<int>(fc.params->elems.size())) {
            const Identifier *p = fc.params->elems[id->sym.slot].get();
            result = decl_only ? p->const_param
                               : (p->const_param || p->auto_const_param);
        } else if (decl_only) {
            result = arg->is_const;     /* parse-time const, not auto-const */
        } else {
            fold_reads(arg, fc);        /* effective: try to auto-const it */
            result = arg->is_const;
        }

        MakeConstructFromConstVal(EvalValue(result), slot, false);
    }

    /*
     * Register every effectively-pure NAMED function into `cctx`, so that
     * fold_reads can evaluate their constant-argument calls at compile time.
     * Does not descend into function bodies (a pure func contains no funcs, and
     * cctx is flat). Duplicate names across sibling scopes are skipped. This
     * mirrors what the parser already does for explicit `pure` funcs in its own
     * const context; here we rebuild it for the (post-parse) auto-const pass so
     * auto-pure funcs - and pure calls whose args only become const via
     * auto-const - can fold too.
     */
    void register_pure_funcs(Construct *c)
    {
        if (!c)
            return;

        if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
            if (fd->effective_pure && fd->id) {
                try {
                    fd->eval(&cctx);
                } catch (const Exception &) {
                    /* already defined / not registerable: just skip it */
                }
            }
            return;
        }

        auto rec = [&](Construct *ch) { register_pure_funcs(ch); };

        if (auto *b = dynamic_cast<Block *>(c)) {
            for (auto &e : b->elems)
                rec(e.get());
        } else if (auto *f = dynamic_cast<ForStmt *>(c)) {
            rec(f->init.get()); rec(f->cond.get());
            rec(f->inc.get());  rec(f->body.get());
        } else if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
            rec(fe->container.get()); rec(fe->body.get());
        } else if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
            rec(tc->tryBody.get());
            for (auto &p : tc->catchStmts)
                rec(p.second.get());
            rec(tc->finallyBody.get());
        } else if (auto *e14 = dynamic_cast<Expr14 *>(c)) {
            rec(e14->lvalue.get()); rec(e14->rvalue.get());
        } else {
            for_each_child(c, rec);
        }
    }

    /* Fold one function body. Only block bodies are folded; a `=> expr` body is
     * a single expression and is left as-is (minor missed optimization).
     * `params` is the function's parameter list (nullptr for top-level main),
     * used to answer isconst()/isconstdecl() on parameter references. */
    void fold_function(Construct *body, const std::vector<int> &writes,
                       const IdList *params)
    {
        Block *b = dynamic_cast<Block *>(body);

        if (!b)
            return;

        FCtx fc{ writes, {}, {}, params };
        prescan_blocked(b, fc.blocked);
        fold_block(b, fc);
    }

    /* Fold a function's body, handling BOTH a `{ ... }` block and an
     * expression body (`=> expr`). The expression form was previously skipped
     * (fold_function bails on a non-Block), so a pure call in an
     * expression-bodied function never const-folded - e.g.
     * `func g() => f(1,2)` kept the call. */
    void fold_func_body(FuncDeclStmt *fd)
    {
        if (!fd->body)
            return;
        if (dynamic_cast<Block *>(fd->body.get())) {
            fold_function(fd->body.get(), fd->slot_writes, fd->params.get());
        } else {
            FCtx fc{ fd->slot_writes, {}, {}, fd->params.get() };
            prescan_blocked(fd->body.get(), fc.blocked);
            fold_reads(fd->body, fc);
        }
    }

    /*
     * Collect slots that must NOT be promoted, because replacing the variable
     * with its literal value there would change behavior. A slot is blocked if
     * it appears as:
     *   - a nested function's capture (a capture must stay an identifier);
     *   - a DIRECT identifier argument of a call - a builtin may take it as an
     *     lvalue (append/push/sort/.../undef), so a literal there would throw
     *     NotLValueEx instead. (Expression args like `f(a + 0)` are already
     *     non-lvalues, so those fold safely and aren't blocked.)
     *   - a subscript or member base (`a[i]`, `a.k`), which needs an lvalue
     *     when assigned and a container when read;
     *   - a foreach loop variable, which is implicitly reassigned every
     *     iteration (so it is not really write-once despite its write count).
     * Does not descend into nested function bodies (a separate slot frame).
     *
     * `block_subscript_bases` (default true) gates the subscript/member-base
     * rule. Specialization passes false: a read-only array/dict const is sound
     * to substitute as a subscript/member base (the param decl is kept, so an
     * lvalue base doesn't dangle, and fold_reads never rewrites an lvalue
     * position anyway), so only the genuinely-unsafe blocks (capture, lvalue
     * builtin first arg, callee, foreach var) should apply to it.
     */
    void prescan_blocked(Construct *c, std::unordered_set<int> &blocked,
                         bool block_subscript_bases = true)
    {
        if (!c)
            return;

        auto block = [&](Construct *x) {
            if (auto *id = dynamic_cast<Identifier *>(x))
                if (id->sym.kind == SymKind::local)
                    blocked.insert(id->sym.slot);
        };

        if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
            if (fd->captures)
                for (auto &cap : fd->captures->elems)
                    block(cap.get());
            return;                    /* separate slot frame: don't descend */
        }

        if (auto *ce = dynamic_cast<CallExpr *>(c)) {
            /* Block the callee: it isn't folded, so promoting a var used there
             * would drop its decl and leave a dangling reference (a misleading
             * "undefined variable" instead of not-callable). */
            block(ce->what.get());
            /* Block only the first arg of a builtin that takes it as an lvalue
             * or identifier; all other call arguments are safe to fold. */
            auto *callee = dynamic_cast<Identifier *>(ce->what.get());
            if (callee && ce->args && !ce->args->elems.empty()
                    && is_lvalue_arg_builtin(callee->get_str()))
                block(ce->args->elems[0].get());
        } else if (auto *sub = dynamic_cast<Subscript *>(c)) {
            if (block_subscript_bases)
                block(sub->what.get());
        } else if (auto *me = dynamic_cast<MemberExpr *>(c)) {
            if (block_subscript_bases)
                block(me->what.get());
        } else if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
            if (fe->ids)
                for (auto &id : fe->ids->elems)
                    block(id.get());
        }

        /* Complete recursion. for_each_child intentionally skips the nodes the
         * resolver's walk() handles itself (Block/for/foreach/try), so descend
         * into those explicitly here. */
        auto rec = [&](Construct *ch) {
            prescan_blocked(ch, blocked, block_subscript_bases);
        };

        if (auto *b = dynamic_cast<Block *>(c)) {
            for (auto &e : b->elems)
                rec(e.get());
        } else if (auto *f = dynamic_cast<ForStmt *>(c)) {
            rec(f->init.get());
            rec(f->cond.get());
            rec(f->inc.get());
            rec(f->body.get());
        } else if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
            rec(fe->container.get());
            rec(fe->body.get());
        } else if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
            rec(tc->tryBody.get());
            for (auto &p : tc->catchStmts)
                rec(p.second.get());
            rec(tc->finallyBody.get());
        } else if (auto *e14 = dynamic_cast<Expr14 *>(c)) {
            rec(e14->lvalue.get());        /* reaches `a[i]=`/`a.k=` bases */
            rec(e14->rvalue.get());        /* reaches a func-expr's captures */
        } else {
            for_each_child(c, rec);
        }
    }

    /* A block's statements: promote const decls, fold, drop dead code. */
    void fold_block(Block *b, FCtx &fc)
    {
        std::vector<unique_ptr<Construct>> kept;

        for (auto &e : b->elems) {

            if (auto *e14 = dynamic_cast<Expr14 *>(e.get())) {

                if (e14->fl & pFlags::pInDecl) {

                    fold_reads(e14->rvalue, fc);
                    auto *id = dynamic_cast<Identifier *>(e14->lvalue.get());

                    if (id && id->sym.kind == SymKind::local
                           && is_scalar_literal(e14->rvalue.get())
                           && promotable(fc, id->sym.slot)) {
                        /* write-once scalar const: record it and drop the decl;
                         * all uses fold to the literal. */
                        if (analysis)
                            analysis->mark(id->start,
                                static_cast<int>(id->get_str().length()),
                                AnnoKind::auto_const);
                        fc.consts[id->sym.slot] = coerce_decl_scalar(
                            e14->rvalue->eval(&cctx), id->decl_type);
                        TRACE(autoconst, 0, std::string(id->get_str()) +
                              "  write-once scalar -> " +
                              fc.consts[id->sym.slot].to_string() +
                              "  (folded at uses)");
                        continue;
                    }

                    kept.push_back(move(e));
                    continue;
                }

                fold_reads(e14->rvalue, fc);   /* assignment: rhs ... */
                fold_lvalue_reads(e14->lvalue, fc);   /* ... and lvalue reads */
                kept.push_back(move(e));
                continue;
            }

            if (fold_child(e, fc))
                kept.push_back(move(e));
        }

        b->elems = move(kept);
    }

    /*
     * Fold a statement/child in place. Returns false if the enclosing block
     * should drop it (a statically-dead branch).
     */
    bool fold_child(unique_ptr<Construct> &slot, FCtx &fc)
    {
        Construct *c = slot.get();

        if (!c)
            return false;

        if (auto *b = dynamic_cast<Block *>(c)) {
            fold_block(b, fc);
            return true;
        }

        if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
            fold_func_body(fd);            /* block / expr-bodied func */
            return true;
        }

        if (auto *iff = dynamic_cast<IfStmt *>(c)) {

            fold_reads(iff->condExpr, fc);

            if (is_scalar_literal(iff->condExpr.get())) {
                /* Const condition: this is dead-code elimination. We proved
                 * one branch unreachable, so we DROP it without folding it (no
                 * point analyzing code we just proved can't run) and keep+fold
                 * only the live branch. Errors in dead code are not surfaced
                 * here - that's the parser's job for const/literal exprs. */
                const EvalValue v = iff->condExpr->eval(&cctx);
                const bool t = v.get_type()->is_true(v);
                /* -a: the not-taken branch is dead - dim it (capture its span
                 * before it's dropped). */
                if (analysis) {
                    Construct *dead = t ? iff->elseBlock.get()
                                        : iff->thenBlock.get();
                    if (dead)
                        analysis->mark_dead(dead->start, dead->end);
                }
                unique_ptr<Construct> taken =
                    t ? move(iff->thenBlock) : move(iff->elseBlock);
                if (!taken || !fold_child(taken, fc))
                    return false;          /* no live branch (or folds away) */
                slot = move(taken);        /* replace if with its live branch */
                return true;
            }

            /* Non-const condition: both branches may run - fold both. */
            if (iff->thenBlock && !fold_child(iff->thenBlock, fc))
                iff->thenBlock.reset();
            if (iff->elseBlock && !fold_child(iff->elseBlock, fc))
                iff->elseBlock.reset();
            return true;
        }

        if (auto *w = dynamic_cast<WhileStmt *>(c)) {
            fold_reads(w->condExpr, fc);
            if (is_scalar_literal(w->condExpr.get())) {
                const EvalValue v = w->condExpr->eval(&cctx);
                if (!v.get_type()->is_true(v)) {
                    /* while (false): the loop is dead - dim it (the body's end,
                     * not w->end, which points at the next token) and drop it
                     * without folding the body. */
                    if (analysis)
                        analysis->mark_dead(w->start,
                            w->body ? w->body->end : w->end);
                    return false;
                }
            }
            if (w->body && !fold_child(w->body, fc))   /* live / maybe-live */
                w->body.reset();
            return true;
        }

        if (auto *f = dynamic_cast<ForStmt *>(c)) {
            if (f->init && !fold_child(f->init, fc)) f->init.reset();
            fold_reads(f->cond, fc);
            if (f->inc && !fold_child(f->inc, fc)) f->inc.reset();
            if (f->body && !fold_child(f->body, fc)) f->body.reset();
            return true;
        }

        if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
            fold_reads(fe->container, fc);
            if (fe->body && !fold_child(fe->body, fc)) fe->body.reset();
            return true;
        }

        if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
            if (tc->tryBody && !fold_child(tc->tryBody, fc))
                tc->tryBody.reset();
            for (auto &p : tc->catchStmts)
                if (p.second && !fold_child(p.second, fc)) p.second.reset();
            if (tc->finallyBody && !fold_child(tc->finallyBody, fc))
                tc->finallyBody.reset();
            return true;
        }

        if (auto *r = dynamic_cast<ReturnStmt *>(c)) {
            /* fold the returned expression. ReturnStmt is a plain Construct
             * (not a SingleChildConstruct), so fold_reads doesn't recurse into
             * it - without this a promoted const used only in a `return` would
             * have its decl dropped but the use left dangling (undefined var),
             * and a specialized clone couldn't fold its return expression. */
            fold_reads(r->elem, fc);
            return true;
        }

        if (auto *e14 = dynamic_cast<Expr14 *>(c)) {
            fold_reads(e14->rvalue, fc);   /* assignment as a statement: rhs */
            fold_lvalue_reads(e14->lvalue, fc);   /* ... and lvalue reads */
            return true;
        }

        fold_reads(slot, fc);              /* expression statement */
        return true;
    }

    /*
     * Fold the read subexpressions of an assignment lvalue - a subscript index
     * or a subscript/member base - so a promoted const used there folds like
     * any other read (without this, `a[i] = v` keeps `i` after its decl is
     * dropped -> "undefined variable" at runtime). A bare identifier or id-list
     * IS the write target, never a read, so it is skipped (a reassigned target
     * isn't write-once and so is never promoted anyway). A blocked base (every
     * subscript/member base is blocked from promotion) simply doesn't fold.
     */
    void fold_lvalue_reads(unique_ptr<Construct> &lvalue, FCtx &fc)
    {
        if (lvalue && !lvalue->is_id() && !lvalue->is_idlist())
            fold_reads(lvalue, fc);
    }

    /*
     * Fold a read-position expression: propagate promoted consts into it and
     * collapse all-literal arithmetic to a single literal (bottom-up). Never
     * touches write targets (assignment lvalues), capture lists, or callees.
     */
    void fold_reads(unique_ptr<Construct> &slot, FCtx &fc)
    {
        Construct *c = slot.get();

        if (!c)
            return;

        if (auto *id = dynamic_cast<Identifier *>(c)) {
            if (id->sym.kind == SymKind::local) {
                auto it = fc.consts.find(id->sym.slot);
                if (it != fc.consts.end()) {
                    /* A use of an auto-const var folds to its literal here -
                     * color the original identifier yellow before it's gone. */
                    if (analysis)
                        analysis->mark(id->start,
                            static_cast<int>(id->get_str().length()),
                            AnnoKind::auto_const);
                    /* Scalars inline as a literal. A seeded array/dict const
                     * (specialization only) bakes into a read-only LiteralObj
                     * so refold can fold reads of it - process_arrays and
                     * immutable both on. The value is already read-only, so any
                     * mutation of it still throws the same error at runtime. */
                    const bool arr =
                        it->second.is<SharedArrayObj>()
                        || it->second.is<intrusive_ptr<DictObject>>();
                    MakeConstructFromConstVal(it->second, slot, arr, arr);
                }
            }
            return;
        }

        if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
            fold_func_body(fd);        /* block / expr-bodied func */
            return;                    /* leave capture list / name alone */
        }

        if (auto *mo = dynamic_cast<MultiOpConstruct *>(c)) {
            bool all_lit = true;
            for (auto &p : mo->elems) {
                fold_reads(p.second, fc);
                if (!is_scalar_literal(p.second.get()))
                    all_lit = false;
            }
            if (all_lit) {
                /* Fold the constant op to a literal. If evaluating it raises an
                 * exception (x/0, a type mismatch, ...), we DON'T swallow it: a
                 * value we can fully compute at compile time that always fails
                 * is a program that can never run correctly, so the error
                 * propagates out of name resolution and aborts before run -
                 * like the parser's const-folding. try/catch is for *runtime*
                 * exceptions and does not (and should not) catch these; see the
                 * const-eval / auto-const notes in CLAUDE.md and README.md. */
                EvalValue fv = RValue(mo->eval(&cctx));
                TRACE(fold, 0, "const expr -> " + fv.to_string());
                MakeConstructFromConstVal(fv, slot, false);
            }
            return;
        }

        if (auto *e14 = dynamic_cast<Expr14 *>(c)) {
            fold_reads(e14->rvalue, fc);   /* embedded assignment: rhs only */
            return;
        }

        if (auto *sc = dynamic_cast<SingleChildConstruct *>(c)) {
            fold_reads(sc->elem, fc);
            return;
        }
        if (auto *ce = dynamic_cast<CallExpr *>(c)) {
            auto *callee = dynamic_cast<Identifier *>(ce->what.get());
            if (callee && ce->args && ce->args->elems.size() == 1
                    && (callee->get_str() == "isconst"
                        || callee->get_str() == "isconstdecl")) {
                fold_isconst(slot, ce, fc,
                             callee->get_str() == "isconstdecl");
                return;
            }

            bool all_const = ce->args != nullptr;
            if (ce->args)
                for (auto &a : ce->args->elems) {
                    fold_reads(a, fc);     /* not the callee */
                    if (!a->is_const)
                        all_const = false;
                }

            /*
             * A call to an (effectively) pure function or const builtin with
             * all constant arguments folds to its result. We evaluate it vs
             * cctx, which holds the const builtins + the registered pure funcs:
             * if the callee isn't there (a non-pure func, runtime(), print...)
             * the lookup throws UndefinedVariableEx and we leave the call for
             * runtime. Any OTHER exception is a real error in fully-constant
             * code and propagates (a build error), per the auto-const rule.
             */
            if (all_const && callee) {
                /* Capture callee loc + name before the node may be freed. */
                const Loc cloc = callee->start;
                const std::string cname(callee->get_str());
                const int clen = static_cast<int>(cname.length());
                try {
                    EvalValue cv = RValue(ce->eval(&cctx));
                    TRACE(fold, 0, cname + "(...) -> " + cv.to_string() +
                          "  (const-arg call)");
                    MakeConstructFromConstVal(cv, slot, false);
                    /* -a: an auto-pure call folded away - color it magenta. */
                    if (analysis)
                        analysis->mark(cloc, clen, AnnoKind::folded);
                } catch (const UndefinedVariableEx &) {
                    /* not a const-foldable callee: keep the runtime call */
                }
            }
            return;
        }
        if (auto *sub = dynamic_cast<Subscript *>(c)) {
            fold_reads(sub->what, fc);
            fold_reads(sub->index, fc);
            return;
        }
        if (auto *sl = dynamic_cast<Slice *>(c)) {
            fold_reads(sl->what, fc);
            fold_reads(sl->start_idx, fc);
            fold_reads(sl->end_idx, fc);
            return;
        }
        if (auto *me = dynamic_cast<MemberExpr *>(c)) {
            fold_reads(me->what, fc);
            return;
        }
        if (auto *la = dynamic_cast<LiteralArray *>(c)) {
            for (auto &el : la->elems)
                fold_reads(el, fc);
            return;
        }
        if (auto *ld = dynamic_cast<LiteralDict *>(c)) {
            for (auto &kv : ld->elems) {
                fold_reads(kv->key, fc);
                fold_reads(kv->value, fc);
            }
            return;
        }
        /* literals and childless constructs: nothing to fold */
    }
};

class Resolver {

public:

    /*
     * Resolve the whole tree. Two passes: first resolve every function body
     * (collecting the globals functions read into `escaped`), then slot the
     * top-level variables that aren't in `escaped`. The root Block records its
     * own slot range (slot_count == top-level frame size), and Block::do_eval
     * builds the "main" Frame from it - so nothing needs to be returned here.
     */
    void run(Construct *root, AnalysisInfo *analysis = nullptr,
             bool repl = false, EvalContext *prior_pure = nullptr)
    {
        repl_mode = repl;

        /* REPL: a function from an earlier input that is effectively pure lets
         * a NEW function calling it is recognized pure too (cross-input
         * auto-pure propagation - see func_body_is_pure / pure_func_names). */
        if (prior_pure) {
            std::vector<std::pair<const UniqueId *, const LValue *>> syms;
            prior_pure->collect_symbols(syms);
            for (const auto &kv : syms) {
                const EvalValue &v = kv.second->get();
                if (v.is<shared_ptr<FuncObject>>() &&
                    v.get<shared_ptr<FuncObject>>()->func->effective_pure)
                    pure_func_names.insert(kv.first);
            }
        }

        top_level_only = false;
        walk(root, nullptr);            /* pass 1: functions; fill `escaped` */

        top_level_only = true;          /* pass 2: top level as "main" */
        FuncState main_st;
        main_st.slottable = true;
        main_st.is_main = true;
        walk(root, &main_st);

        /* Promote write-once scalar vars to constants and fold (uses the write
         * counts just collected; the top-level frame's in main_st.writes).
         * prior_pure seeds the fold context so cross-input pure calls fold. */
        if (auto *rb = dynamic_cast<Block *>(root))
            AutoConst(analysis, prior_pure).run(rb, main_st.writes);
    }

private:

    /* Names a function reads from outer scope: kept in the map, not slotted. */
    std::unordered_set<const UniqueId *> escaped;
    /* Names of functions PROVEN effectively-pure so far (in walk order). A call
     * to one is itself pure - so the auto-pure test recognizes a function that
     * calls an earlier auto-pure helper (e.g. f(x,y)=>add(x,y) is pure once add
     * is), letting its const-arg calls fold. Monotonic; populated as
     * process_function decides each function. */
    std::unordered_set<const UniqueId *> pure_func_names;

    /* Pass 2: function bodies are already resolved, so don't re-enter them. */
    bool top_level_only = false;

    /* REPL: keep ALL top-level decls in the map as persistent globals. */
    bool repl_mode = false;

    void walk(Construct *c, FuncState *cur);
    void process_function(FuncDeclStmt *fd);

    /*
     * Declare `id` in the current innermost scope and slot it. Raises
     * AlreadyDefinedEx on a same-scope redeclaration. If the slot budget is
     * exhausted the name is added as a masking entry (resolves to the map) so
     * shadowing still works. No-op when the function isn't slottable.
     */
    void declare(FuncState *cur, Identifier *id)
    {
        if (!cur || !cur->slottable || !id)
            return;

        check_no_redecl(cur, id);

        /* A top-level variable that some function reads must stay in the map
         * (functions reach globals via the scope-chain map walk, not slots),
         * so add it as a masking entry instead of slotting it. In REPL mode
         * EVERY top-level decl stays in the map - it is a persistent global
         * that survives to the next input (and so is never auto-const-promoted,
         * which would be unsound in an open world). */
        const bool keep_in_map =
            cur->is_main && (escaped.count(id->uid) || repl_mode);

        if (keep_in_map || cur->next_slot >= MAX_SLOTS) {
            cur->scopes.back().decls.push_back({ id->uid, -1, id->decl_type });
            return;
        }

        const int slot = cur->next_slot++;
        cur->scopes.back().decls.push_back({ id->uid, slot, id->decl_type });
        cur->writes.push_back(1);   /* the declaration is write #1 */
        id->sym = ResolvedSym{ SymKind::local, slot };
    }

    /*
     * Declare a name that is in scope but stays in the map (function names).
     * Like declare() it rejects same-scope duplicates and shadows outer slots,
     * but assigns no slot and stamps nothing.
     */
    void declare_masking(FuncState *cur, Identifier *id)
    {
        if (!cur || !cur->slottable || !id)
            return;

        check_no_redecl(cur, id);
        cur->scopes.back().decls.push_back({ id->uid, -1, id->decl_type });
    }

    /* Throw AlreadyDefinedEx if `id` is already declared in this scope. */
    void check_no_redecl(FuncState *cur, Identifier *id) const
    {
        for (const auto &d : cur->scopes.back().decls) {
            if (d.name == id->uid)
                throw AlreadyDefinedEx(id->start, id->end);
        }
    }

    /*
     * Resolve a reference: search scopes innermost-out. A slotted match stamps
     * the identifier; a masking match (-1) leaves it unresolved (map); no match
     * also leaves it unresolved (builtin / capture / global). A non-captured
     * free name in a function is recorded in `escaped` so the top-level pass
     * keeps that global in the map (functions read globals via the map).
     */
    void resolve_ref(FuncState *cur, Identifier *id)
    {
        if (!cur || !id)
            return;

        if (cur->slottable) {
            for (auto s = cur->scopes.rbegin(); s != cur->scopes.rend(); ++s) {
                for (const auto &d : s->decls) {
                    if (d.name == id->uid) {
                        if (d.slot >= 0)
                            id->sym = ResolvedSym{ SymKind::local, d.slot };
                        /* Carry the declared type to the use so an assignment
                         * can coerce a widening value (float f; f = 3). */
                        id->decl_type = d.type;
                        return;     /* found (slotted or masked) */
                    }
                }
            }
        }

        if (!cur->is_main && !cur->captures.count(id->uid))
            escaped.insert(id->uid);
    }

    /* Count an assignment to a resolved-local lvalue as a write of its slot. */
    void count_write(FuncState *cur, Construct *lvalue)
    {
        if (!cur || !cur->slottable)
            return;

        auto bump = [&](Identifier *id) {
            if (id && id->sym.kind == SymKind::local)
                cur->writes[id->sym.slot]++;
        };

        if (auto *id = dynamic_cast<Identifier *>(lvalue)) {
            bump(id);
        } else if (auto *il = dynamic_cast<IdList *>(lvalue)) {
            for (auto &id : il->elems) {
                bump(id.get());
            }
        }
    }

    /* Declare the name(s) a declaration's lvalue introduces (Id/IdList). */
    void declare_lvalue(FuncState *cur, Construct *lvalue)
    {
        if (auto *id = dynamic_cast<Identifier *>(lvalue)) {
            declare(cur, id);
        } else if (auto *il = dynamic_cast<IdList *>(lvalue)) {
            for (auto &id : il->elems) {
                declare(cur, id.get());
            }
        }
    }
};

/*
 * Invoke `fn` on every direct child Construct of `c`. Used for the generic,
 * scope-neutral nodes (if/while/call/subscript/...); the nodes that introduce
 * names or scopes are handled in Resolver::walk and never reach here.
 * Children may be null (e.g. an `if` with no else), so callers tolerate null.
 */
void
for_each_child(Construct *c, const std::function<void(Construct *)> &fn)
{
    if (auto *n = dynamic_cast<SingleChildConstruct *>(c)) {
        fn(n->elem.get());
    } else if (auto *n = dynamic_cast<MultiOpConstruct *>(c)) {
        for (auto &p : n->elems) {
            fn(p.second.get());
        }
    } else if (auto *n = dynamic_cast<CallExpr *>(c)) {
        fn(n->what.get());
        fn(n->args.get());
    } else if (auto *n = dynamic_cast<IfStmt *>(c)) {
        fn(n->condExpr.get());
        fn(n->thenBlock.get());
        fn(n->elseBlock.get());
    } else if (auto *n = dynamic_cast<WhileStmt *>(c)) {
        fn(n->condExpr.get());
        fn(n->body.get());
    } else if (auto *n = dynamic_cast<Subscript *>(c)) {
        fn(n->what.get());
        fn(n->index.get());
    } else if (auto *n = dynamic_cast<Slice *>(c)) {
        fn(n->what.get());
        fn(n->start_idx.get());
        fn(n->end_idx.get());
    } else if (auto *n = dynamic_cast<MemberExpr *>(c)) {
        fn(n->what.get());
    } else if (auto *n = dynamic_cast<ReturnStmt *>(c)) {
        fn(n->elem.get());
    } else if (auto *n = dynamic_cast<LiteralDictKVPair *>(c)) {
        fn(n->key.get());
        fn(n->value.get());
    } else if (auto *n = dynamic_cast<MultiElemConstruct<Construct> *>(c)) {
        /* ExprList, LiteralArray (Block is handled in walk) */
        for (auto &e : n->elems) {
            fn(e.get());
        }
    } else if (auto *n = dynamic_cast<MultiElemConstruct<Identifier> *>(c)) {
        /* IdList */
        for (auto &e : n->elems) {
            fn(e.get());
        }
    } else if (auto *n =
                   dynamic_cast<MultiElemConstruct<LiteralDictKVPair> *>(c)) {
        /* LiteralDict */
        for (auto &e : n->elems) {
            fn(e.get());
        }
    }
    /* literals, Identifier and childless constructs have no children */
}

/*
 * Auto-pure test: true if a function body is "effectively pure" - every free
 * identifier (sym.kind != local, i.e. not a param/local) is a compile-time
 * const (is_const: a const global, const builtin, or explicitly-pure func) OR
 * the name of a function already PROVEN auto-pure (`pure_names` - so calling an
 * earlier auto-pure helper is pure), and the body declares no nested function.
 * Reads the resolver's sym.kind, so it must run AFTER the body is walked.
 * Conservative: self-recursion and calls to a not-yet-decided (e.g.
 * forward-referenced or mutually-recursive) auto-pure func stay impure.
 */
bool
func_body_is_pure(const Construct *c,
                  const std::unordered_set<const UniqueId *> &pure_names)
{
    if (!c)
        return true;

    if (auto *id = dynamic_cast<const Identifier *>(c))
        return id->sym.kind == SymKind::local || id->is_const ||
               pure_names.count(id->uid) != 0;

    if (dynamic_cast<const FuncDeclStmt *>(c))
        return false;   /* a nested function: be conservative */

    if (auto *b = dynamic_cast<const Block *>(c)) {
        for (auto &e : b->elems)
            if (!func_body_is_pure(e.get(), pure_names))
                return false;
        return true;
    }
    if (auto *f = dynamic_cast<const ForStmt *>(c))
        return func_body_is_pure(f->init.get(), pure_names)
            && func_body_is_pure(f->cond.get(), pure_names)
            && func_body_is_pure(f->inc.get(), pure_names)
            && func_body_is_pure(f->body.get(), pure_names);
    if (auto *fe = dynamic_cast<const ForeachStmt *>(c))
        return func_body_is_pure(fe->container.get(), pure_names)
            && func_body_is_pure(fe->body.get(), pure_names);
    if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
        if (!func_body_is_pure(tc->tryBody.get(), pure_names))
            return false;
        for (auto &p : tc->catchStmts)
            if (!func_body_is_pure(p.second.get(), pure_names))
                return false;
        return func_body_is_pure(tc->finallyBody.get(), pure_names);
    }
    if (auto *e14 = dynamic_cast<const Expr14 *>(c))
        return func_body_is_pure(e14->lvalue.get(), pure_names)
            && func_body_is_pure(e14->rvalue.get(), pure_names);

    bool ok = true;
    for_each_child(const_cast<Construct *>(c), [&](Construct *ch) {
        ok = ok && func_body_is_pure(ch, pure_names);
    });
    return ok;
}

/*
 * Set up `fd`'s slotting and resolve its body. Params take slots 0..n-1 in the
 * function's outermost scope; body locals are slotted as the walk encounters
 * their declarations. Afterwards, if anything was slotted, the FuncDeclStmt is
 * marked resolved with its final frame_size and per-slot write counts. A
 * function with no slottable symbols (or a pathological >64 params) is left
 * unresolved and uses the map, but its body is still walked to slot any nested
 * functions.
 */
void
Resolver::process_function(FuncDeclStmt *fd)
{
    FuncState st;
    st.fd = fd;

    /* Capture names are bound via capture_ctx (the map), not slotted; a body
     * reference to one is not a "free global" - exclude them from `escaped`. */
    if (fd->captures) {
        for (auto &cap : fd->captures->elems)
            st.captures.insert(cap->uid);
    }

    const int nparams =
        fd->params ? static_cast<int>(fd->params->elems.size()) : 0;
    st.slottable = nparams <= MAX_SLOTS;

    if (st.slottable) {

        st.scopes.emplace_back();   /* the function's outermost (param) scope */

        for (int i = 0; i < nparams; i++) {
            Identifier *p = fd->params->elems[i].get();
            check_no_redecl(&st, p);   /* duplicate param -> error */
            st.scopes.back().decls.push_back({ p->uid, i, p->decl_type });
            st.writes.push_back(0);   /* binding isn't a body write */
            p->sym = ResolvedSym{ SymKind::local, i };
        }

        st.next_slot = nparams;
    }

    if (fd->body)
        walk(fd->body.get(), &st);

    /*
     * Param const-ness. st.writes[i] for a param counts body reassignments
     * (0 == never reassigned). A `const` param that is reassigned is a
     * compile-time error; a plain param that is never reassigned is effectively
     * const, so mark it auto_const_param (used by isconst()).
     */
    if (st.slottable) {
        for (int i = 0; i < nparams; i++) {
            Identifier *p = fd->params->elems[i].get();
            const bool reassigned =
                i < static_cast<int>(st.writes.size()) && st.writes[i] > 0;

            if (p->const_param) {
                if (reassigned)
                    throw CannotRebindConstEx(p->start, p->end);
            } else if (!reassigned) {
                p->auto_const_param = true;
            }
        }
    }

    /* Auto-pure: a non-pure function with no captures whose body is effectively
     * pure is promoted, so ispure() sees it and its const-arg calls can fold
     * (in the auto-const pass). */
    if (!fd->effective_pure
            && (!fd->captures || fd->captures->elems.empty())
            && fd->body
            && func_body_is_pure(fd->body.get(), pure_func_names)) {
        fd->effective_pure = true;
        if (fd->id)
            TRACE(autopure, 0, std::string(fd->id->get_str()) +
                  "  reads only consts/params -> effective pure");
    }

    /* record any proven-pure function (auto OR explicit) so a LATER function
     * that calls it is recognized pure too (see func_body_is_pure). */
    if (fd->effective_pure && fd->id)
        pure_func_names.insert(fd->id->uid);

    if (st.slottable && st.next_slot > 0) {
        fd->resolved = true;
        fd->frame_size = st.next_slot;
        fd->slot_writes = move(st.writes);
    }
}

/*
 * Recursive driver. `cur` is the function whose scope is being resolved
 * (nullptr at top level, where nothing is slotted but nested functions are
 * still found). Nodes that introduce names or scopes are handled here; the rest
 * are traversed generically.
 */
void
Resolver::walk(Construct *c, FuncState *cur)
{
    if (!c)
        return;

    /* --- nested function: own scope; its captures see the enclosing one --- */
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {

        if (fd->captures) {
            for (auto &cap : fd->captures->elems) {
                resolve_ref(cur, cap.get());
            }
        }

        /* The function NAME is a local of the enclosing scope (masking entry:
         * stays in the map so forward references / mutual recursion work). */
        if (fd->id)
            declare_masking(cur, fd->id.get());

        /* In the top-level pass the body was already resolved (pass 1); here we
         * only needed its capture list and name re-resolved at top level. */
        if (!top_level_only)
            process_function(fd);

        return;
    }

    /* --- identifier reference --- */
    if (auto *id = dynamic_cast<Identifier *>(c)) {
        resolve_ref(cur, id);
        return;
    }

    /* --- assignment / declaration --- */
    if (auto *e = dynamic_cast<Expr14 *>(c)) {

        /* The rvalue is evaluated first, so it must see the scope BEFORE this
         * declaration (so `var x = x + 1` reads the outer x). */
        walk(e->rvalue.get(), cur);

        if (e->fl & pFlags::pInDecl) {
            declare_lvalue(cur, e->lvalue.get());
        } else {
            walk(e->lvalue.get(), cur);     /* assignment: resolve the target */
            count_write(cur, e->lvalue.get());
        }

        return;
    }

    /* --- block: own scope; record slot range for live-bit clearing --- */
    if (auto *b = dynamic_cast<Block *>(c)) {

        const bool track = cur && cur->slottable;
        const int start = track ? cur->next_slot : 0;

        if (track)
            cur->scopes.emplace_back();

        for (auto &e : b->elems) {
            walk(e.get(), cur);
        }

        if (track) {
            b->slot_start = start;
            /* contiguous range: includes nested blocks' slots too */
            b->slot_count = cur->next_slot - start;

            /* Scope-free iff every direct decl got a slot (none stayed in the
             * map: no capture, no nested-func name, no slot-budget overflow).
             * Then do_eval needs no EvalContext of its own. */
            bool all_slotted = true;
            for (const auto &d : cur->scopes.back().decls)
                if (d.slot < 0) { all_slotted = false; break; }
            b->scope_free = all_slotted;

            cur->scopes.pop_back();
        }

        return;
    }

    /* --- for: a scope for the loop variable spanning cond/inc/body --- */
    if (auto *f = dynamic_cast<ForStmt *>(c)) {

        const bool track = cur && cur->slottable;

        if (track)
            cur->scopes.emplace_back();

        walk(f->init.get(), cur);   /* may declare the loop variable */
        walk(f->cond.get(), cur);
        walk(f->inc.get(), cur);
        walk(f->body.get(), cur);   /* body is a Block -> own scope */

        if (track)
            cur->scopes.pop_back();

        return;
    }

    /* --- foreach: container evaluated before the loop vars exist --- */
    if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {

        walk(fe->container.get(), cur);

        const bool track = cur && cur->slottable;

        if (track)
            cur->scopes.emplace_back();

        if (fe->ids) {
            for (auto &id : fe->ids->elems) {
                if (fe->idsVarDecl)
                    declare(cur, id.get());  /* `foreach (var a, b in ...)` */
                else
                    resolve_ref(cur, id.get()); /* existing vars */
            }
        }

        walk(fe->body.get(), cur);

        if (track)
            cur->scopes.pop_back();

        return;
    }

    /* --- try/catch/finally: each catch clause scopes its `as` variable --- */
    if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {

        walk(tc->tryBody.get(), cur);

        for (auto &p : tc->catchStmts) {

            const bool track = cur && cur->slottable;

            if (track)
                cur->scopes.emplace_back();

            if (p.first.asId)
                declare(cur, p.first.asId.get());

            walk(p.second.get(), cur);          /* catch body */

            if (track)
                cur->scopes.pop_back();
        }

        walk(tc->finallyBody.get(), cur);
        return;
    }

    /* --- everything else: generic traversal --- */
    for_each_child(c, [&](Construct *ch) { walk(ch, cur); });
}

} // anonymous namespace

/* ----------------------- Function inlining ------------------------ */

/*
 * Size-only inlining of expression-bodied, top-level, non-capturing,
 * non-recursive functions at direct call sites (plans/function-inlining.md).
 * Splices the callee body in place of the call, substituting parameters with
 * the arguments. Spliced nodes carry an InlineCtx so the backtrace is identical
 * with inlining on or off (the Construct::eval / do_func_call flush points
 * rebuild the virtual frame from it).
 *
 * Expression bodies have no locals, so after param substitution the spliced
 * expression contains only the caller's argument subtrees (already resolved)
 * and free identifiers (map-resolved, scope-independent) - no re-resolution is
 * needed. A body that was itself inlined-into keeps correct frames via chain
 * rebasing. Argument substitution is sound: an argument is evaluated exactly as
 * often as the parameter is used, and side-effecting args are never dropped or
 * duplicated.
 */

/*
 * InlineCtx objects outlive the inliner (the AST references them; their strings
 * are copied into a BacktraceFrame only on error). Pooled here and never freed,
 * like UniqueId's interned strings - there are only a few of them. A deque
 * keeps element addresses stable as it grows.
 */
static std::deque<InlineCtx> inline_ctx_pool;

static const InlineCtx *
alloc_inline_ctx(InlineCtx ic)
{
    inline_ctx_pool.push_back(std::move(ic));
    return &inline_ctx_pool.back();
}

/*
 * Enumerate the Construct-typed child *slots* of `c` (the slots an inlined
 * expression may replace, and the ones to recurse through to reach nested
 * calls). Identifier / IdList children (params, captures, foreach ids) cannot
 * hold a call and are skipped. Mirrors for_each_child but yields replaceable
 * unique_ptr<Construct>& slots.
 */
static void
for_each_child_slot(Construct *c,
                    const std::function<void(unique_ptr<Construct> &)> &fn)
{
    if (auto *n = dynamic_cast<SingleChildConstruct *>(c)) {
        fn(n->elem);
    } else if (auto *n = dynamic_cast<MultiOpConstruct *>(c)) {
        for (auto &p : n->elems) fn(p.second);
    } else if (auto *n = dynamic_cast<CallExpr *>(c)) {
        fn(n->what);
        for (auto &e : n->args->elems) fn(e);
    } else if (auto *n = dynamic_cast<IfStmt *>(c)) {
        fn(n->condExpr);
        if (n->thenBlock) fn(n->thenBlock);
        if (n->elseBlock) fn(n->elseBlock);
    } else if (auto *n = dynamic_cast<WhileStmt *>(c)) {
        fn(n->condExpr); fn(n->body);
    } else if (auto *n = dynamic_cast<ForStmt *>(c)) {
        if (n->init) fn(n->init);
        if (n->cond) fn(n->cond);
        if (n->inc) fn(n->inc);
        fn(n->body);
    } else if (auto *n = dynamic_cast<ForeachStmt *>(c)) {
        fn(n->container); fn(n->body);
    } else if (auto *n = dynamic_cast<Subscript *>(c)) {
        fn(n->what); fn(n->index);
    } else if (auto *n = dynamic_cast<Slice *>(c)) {
        fn(n->what);
        if (n->start_idx) fn(n->start_idx);
        if (n->end_idx) fn(n->end_idx);
    } else if (auto *n = dynamic_cast<MemberExpr *>(c)) {
        fn(n->what);
    } else if (auto *n = dynamic_cast<ReturnStmt *>(c)) {
        if (n->elem) fn(n->elem);
    } else if (auto *n = dynamic_cast<Expr14 *>(c)) {
        if (n->lvalue) fn(n->lvalue);
        if (n->rvalue) fn(n->rvalue);
    } else if (auto *n = dynamic_cast<LiteralDictKVPair *>(c)) {
        fn(n->key); fn(n->value);
    } else if (auto *n = dynamic_cast<FuncDeclStmt *>(c)) {
        if (n->body) fn(n->body);
    } else if (auto *n = dynamic_cast<TryCatchStmt *>(c)) {
        fn(n->tryBody);
        for (auto &p : n->catchStmts) fn(p.second);
        if (n->finallyBody) fn(n->finallyBody);
    } else if (auto *n = dynamic_cast<MultiElemConstruct<Construct> *>(c)) {
        for (auto &e : n->elems) fn(e);   /* Block, ExprList, LiteralArray */
    } else if (auto *n =
                   dynamic_cast<MultiElemConstruct<LiteralDictKVPair> *>(c)) {
        for (auto &e : n->elems) { fn(e->key); fn(e->value); }
    }
    /* literals, Identifier, IdList, childless: nothing inlinable */
}

class Inliner {

    /* uid -> the unique top-level expression-bodied func to inline, or nullptr
     * if the name is ambiguous (declared more than once). */
    std::unordered_map<const UniqueId *, FuncDeclStmt *> funcs;

    /* uid -> the unique top-level block-bodied func to specialize (clone for a
     * given const-arg tuple), or nullptr if ambiguous. */
    std::unordered_map<const UniqueId *, FuncDeclStmt *> spec_funcs;

    const int max_nodes;   /* inline only when the body is at most this big */
    EvalContext cctx;      /* const context for re-folding spliced bodies */
    AutoConst ac;          /* used to fold specialized clones */
    AnalysisInfo *analysis;   /* -a: record inlined / specialized; or null */

    /* (func, const-arg tuple) -> the specialized clone, or nullptr if building
     * it was not beneficial (cached so it isn't retried). */
    std::unordered_map<std::string, FuncDeclStmt *> spec_cache;

    /* specialized clones to register (inserted at the root block's front after
     * the walk, so they exist before any call reaches them). */
    std::vector<unique_ptr<Construct>> new_funcs;
    int spec_counter = 0;

    /*
     * Fixpoint bounds. Re-scanning a spliced body can expose more inlining (a
     * forward-declared callee, a call revealed by const-folding), so the
     * inliner iterates by re-walking each splice. MAX_INLINE_DEPTH caps the
     * nesting so mutual recursion (`a()=>b(); b()=>a()`) terminates;
     * `inline_budget` caps the total nodes added so breadth-doubling
     * (`f()=>g()+g()`) can't blow the tree up. Either bound just leaves the
     * remaining calls in place (still correct - they run at runtime).
     */
    static const int MAX_INLINE_DEPTH = 16;
    long inline_budget = 0;

public:

    explicit Inliner(int max_nodes, AnalysisInfo *a = nullptr)
        /* `ac` folds specialization *clones*, not the original source, so it
         * must NOT record analysis (that would color the original body for one
         * specialized call). The Inliner records inline/specialize itself. */
        : max_nodes(max_nodes), cctx(nullptr, true), ac(), analysis(a) { }

    void run(Block *root)
    {
        for (auto &e : root->elems) {
            auto *fd = dynamic_cast<FuncDeclStmt *>(e.get());
            if (!fd || !fd->id)
                continue;
            if (inlinable_decl(fd))
                add_unique(funcs, fd);
            else if (specializable_decl(fd))
                add_unique(spec_funcs, fd);
        }

        seed_const_globals(root);

        /* Budget for the re-scan fixpoint: generous (so a normal program never
         * hits it - it's a runaway guard), but proportional so a pathological
         * expansion is bounded by program size. */
        inline_budget = std::max<long>(4096,
                                       static_cast<long>(count_all_nodes(root))
                                           * 8);

        /* Top-level statements run in "main", whose frame is the root block's
         * slot_count (Block::do_eval builds it); pass that as the frame to grow
         * when a top-level tail call is inlined. */
        for (auto &e : root->elems)
            walk(e, 0, &root->slot_count);

        /* Register clones at the front: ready before any call reaches one. */
        if (!new_funcs.empty()) {
            root->elems.insert(root->elems.begin(),
                std::make_move_iterator(new_funcs.begin()),
                std::make_move_iterator(new_funcs.end()));
            new_funcs.clear();
        }
    }

private:

    /*
     * Register top-level const array/dict globals into cctx so refold can fold
     * reads of them (e.g. `tbl[0]`). Their rvalues are self-contained baked
     * values (LiteralObj), so order doesn't matter and eval needs no other
     * context. Scalar consts are already inlined everywhere (no decl remains);
     * const funcs are skipped (not foldable as a value here).
     */
    void seed_const_globals(Block *root)
    {
        for (auto &e : root->elems) {
            auto *e14 = dynamic_cast<Expr14 *>(e.get());
            if (!e14 || !(e14->fl & pFlags::pInConstDecl))
                continue;
            auto *id = dynamic_cast<Identifier *>(e14->lvalue.get());
            if (!id || !dynamic_cast<LiteralObj *>(e14->rvalue.get()))
                continue;
            try {
                cctx.emplace(id, RValue(e14->rvalue->eval(&cctx)), true);
            } catch (const Exception &) {
                /* unexpected: just don't seed it */
            }
        }
    }

    static bool inlinable_decl(const FuncDeclStmt *fd)
    {
        return fd->body
            && !fd->body->is_block()                       /* => expr body */
            && (!fd->captures || fd->captures->elems.empty())
            /* No nested function: a closure in the body may capture this
             * function's parameters, which substitution would break. */
            && !contains_func(fd->body.get())
            /* not recursive / does not reference its own name */
            && count_uses(fd->body.get(), fd->id->uid) == 0;
    }

    static bool contains_func(const Construct *c)
    {
        if (!c)
            return false;
        if (dynamic_cast<const FuncDeclStmt *>(c))
            return true;
        bool found = false;
        for_each_child(const_cast<Construct *>(c),
            [&](Construct *ch) { if (contains_func(ch)) found = true; });
        return found;
    }

    /*
     * A block-bodied function we may clone+specialize for a const-arg tuple. No
     * captures or nested function (a closure could capture this function's
     * params). Recursion is allowed: the clone's self-calls go to the original.
     */
    static bool specializable_decl(const FuncDeclStmt *fd)
    {
        return fd->body
            && fd->body->is_block()
            && (!fd->captures || fd->captures->elems.empty())
            && !contains_func(fd->body.get());
    }

    static void add_unique(
        std::unordered_map<const UniqueId *, FuncDeclStmt *> &map,
        FuncDeclStmt *fd)
    {
        auto res = map.emplace(fd->id->uid, fd);
        if (!res.second)
            res.first->second = nullptr;   /* duplicate name: ambiguous */
    }

    /*
     * `fsize` points at the frame size of the function (or the root block's
     * slot_count for "main") whose body we are walking - the frame that block-
     * body tail inlining grows when it remaps a callee's locals into it.
     * nullptr means the enclosing function is unresolved (no frame), so block-
     * body inlining is skipped there.
     */
    void walk(unique_ptr<Construct> &slot, int depth, int *fsize)
    {
        if (!slot)
            return;

        /* A nested function owns a separate frame: walk its body with that
         * function's frame size (or no frame if unresolved). Its name, params
         * and capture list hold no calls. */
        if (auto *fd = dynamic_cast<FuncDeclStmt *>(slot.get())) {
            if (fd->body)
                walk(fd->body, depth,
                     fd->resolved ? &fd->frame_size : nullptr);
            return;
        }

        /* Recurse into children at the same splice depth. */
        for_each_child_slot(slot.get(),
            [&](unique_ptr<Construct> &ch) { walk(ch, depth, fsize); });

        try_inline(slot, depth, fsize);   /* re-scans its splice (depth + 1) */
        try_inline_tail(slot, depth, fsize);   /* tail call to a block func */
        try_specialize(slot);   /* if still a call to a block-bodied func */
    }

    void try_inline(unique_ptr<Construct> &slot, int depth, int *fsize)
    {
        auto *ce = dynamic_cast<CallExpr *>(slot.get());
        if (!ce)
            return;

        /* The callee must be a plain name that is NOT a resolved local (a local
         * could shadow a same-named top-level function). */
        auto *callee = dynamic_cast<Identifier *>(ce->what.get());
        if (!callee || callee->sym.kind == SymKind::local)
            return;

        auto it = funcs.find(callee->uid);
        if (it == funcs.end() || !it->second)
            return;

        FuncDeclStmt *f = it->second;
        const size_t nparams = f->params ? f->params->elems.size() : 0;

        /* Arg count must match, else the runtime arity error must survive. */
        if (ce->args->elems.size() != nparams)
            return;

        const int bsz = node_count(f->body.get());
        if (bsz > max_nodes)
            return;

        for (size_t i = 0; i < nparams; i++) {
            const int uses = count_uses(f->body.get(),
                                        f->params->elems[i]->uid);
            if (!sub_ok(uses, ce->args->elems[i].get()))
                return;
        }

        /* Fixpoint bounds: stop nesting at the depth cap (terminates mutual
         * recursion) and once the growth budget is spent (bounds expansion).
         * Either way the call is left for runtime - still correct. */
        if (depth >= MAX_INLINE_DEPTH || bsz > inline_budget)
            return;
        inline_budget -= bsz;

        /*
         * Eligible: clone the body, substitute params with the args, then tag
         * the whole result as inlined. Substitution happens first and copies
         * each parameter occurrence's source loc onto the arg, so an error in
         * the spliced body points where it would in the un-inlined callee (the
         * operator ladder stamps the operand loc) - keeping the backtrace
         * identical. Tagging last covers body and args alike.
         *
         * The new frame's parent is THIS call's existing inline_ctx, not null:
         * when the call site is itself a node spliced in by an outer inline
         * (the re-scan below), the inlined-at chain must stack (g inside f
         * shows [g, f]). `rebase` in tag_inline then re-roots the body's own
         * chains under it, so arbitrarily deep nesting renders correctly.
         */
        const InlineCtx *ic = alloc_inline_ctx(
            { std::string(f->id->get_str()), param_names(f),
              ce->start, ce->inline_ctx });

        unique_ptr<Construct> body = f->body->clone();

        for (size_t i = 0; i < nparams; i++)
            substitute(body, f->params->elems[i]->uid,
                       ce->args->elems[i].get());

        tag_inline(body.get(), ic);

        if (analysis)
            analysis->mark(callee->start,
                static_cast<int>(callee->get_str().length()),
                AnnoKind::inlined);

        TRACE(inlining, 0, std::string(f->id->get_str()) + "(" +
              std::to_string(nparams) + " arg(s))  body " +
              std::to_string(bsz) + " nodes -> splice");

        /* Replaces (frees) the old CallExpr; its args were already cloned. */
        slot = move(body);

        /*
         * Re-fold: a const argument substituted into the body can make a
         * subexpression all-const that AutoConst (which ran before this pass)
         * never saw - e.g. `f(3)` with `f(x) => x * 10 + g` splices to
         * `3 * 10 + g`, and `3 * 10` folds to `30`. This is the const-
         * propagation half for non-pure functions (a pure one's whole call
         * already folded earlier).
         */
        refold(slot);

        /*
         * The fixpoint: re-scan the spliced result one level deeper. A call
         * inside the body that is now reachable - a forward-declared callee
         * whose own decl wasn't inlined yet, or a call newly exposed by the
         * re-fold above - gets inlined too, so a g-into-f-into-h chain
         * collapses in one pass regardless of decl order. depth+1 bounds it.
         */
        walk(slot, depth + 1, fsize);
    }

    /* Frame::live is a 64-bit word, so a frame holds at most 64 slots. */
    static const int MAX_FRAME_SLOTS = 64;

    /*
     * Conservative "this block always returns": its last statement is an
     * unconditional ReturnStmt, so control can never fall off the end. A
     * block-bodied function with this property is safe to splice in tail
     * position - it never falls through to whatever followed the call.
     */
    static bool block_always_returns(const Block *b)
    {
        return b && !b->elems.empty()
            && dynamic_cast<const ReturnStmt *>(b->elems.back().get());
    }

    /*
     * A COMPLETE count of Identifier nodes matching `pred` (unlike count_uses,
     * whose for_each_child stops at Block/Expr14/for/foreach/try - fine for the
     * expression bodies it serves, but a block body hides uses inside those).
     * Used to (1) reject a self-recursive callee and (2) count a param's uses
     * by SLOT, so a local that shadows a param's name isn't miscounted.
     */
    static int count_matching(
        const Construct *c,
        const std::function<bool(const Identifier *)> &pred)
    {
        if (!c)
            return 0;
        if (auto *id = dynamic_cast<const Identifier *>(c))
            return pred(id) ? 1 : 0;

        int n = 0;
        if (auto *b = dynamic_cast<const Block *>(c)) {
            for (auto &e : b->elems)
                n += count_matching(e.get(), pred);
        } else if (auto *e14 = dynamic_cast<const Expr14 *>(c)) {
            n += count_matching(e14->lvalue.get(), pred);
            n += count_matching(e14->rvalue.get(), pred);
        } else if (auto *fs = dynamic_cast<const ForStmt *>(c)) {
            n += count_matching(fs->init.get(), pred);
            n += count_matching(fs->cond.get(), pred);
            n += count_matching(fs->inc.get(), pred);
            n += count_matching(fs->body.get(), pred);
        } else if (auto *fe = dynamic_cast<const ForeachStmt *>(c)) {
            if (fe->ids)
                for (auto &id : fe->ids->elems)
                    n += count_matching(id.get(), pred);
            n += count_matching(fe->container.get(), pred);
            n += count_matching(fe->body.get(), pred);
        } else if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
            n += count_matching(tc->tryBody.get(), pred);
            for (auto &p : tc->catchStmts) {
                if (p.first.asId)
                    n += count_matching(p.first.asId.get(), pred);
                n += count_matching(p.second.get(), pred);
            }
            n += count_matching(tc->finallyBody.get(), pred);
        } else {
            for_each_child(const_cast<Construct *>(c),
                [&](Construct *ch) { n += count_matching(ch, pred); });
        }
        return n;
    }

    /*
     * Splice a (cloned) block body for tail inlining, in ONE pass: substitute
     * each param use (a local Identifier whose ORIGINAL slot < nparams) with
     * the matching arg, and remap every body LOCAL (slot >= nparams) and Block
     * slot range by `off` so they land in the caller's frame at
     * [caller_fsize, ...). Deciding by the original slot in one pass avoids any
     * transient collision between f's locals and the caller's slots. The
     * substituted arg's own identifiers (caller slots) are left untouched.
     * Loop-var / catch-var / multi-assign-target identifiers are declarations
     * (always locals, never params), so they are remapped, never substituted.
     */
    static void splice_tail(unique_ptr<Construct> &ref,
                            const std::vector<Construct *> &args,
                            int nparams, int off)
    {
        Construct *c = ref.get();
        if (!c)
            return;

        if (auto *id = dynamic_cast<Identifier *>(c)) {
            if (id->sym.kind == SymKind::local) {
                if (id->sym.slot < nparams) {
                    unique_ptr<Construct> a = args[id->sym.slot]->clone();
                    a->start = id->start;     /* keep the param's source loc */
                    a->end = id->end;
                    ref = move(a);
                } else {
                    id->sym.slot += off;
                }
            }
            return;
        }

        auto remap_id = [&](Identifier *id) {
            if (id && id->sym.kind == SymKind::local && id->sym.slot >= nparams)
                id->sym.slot += off;
        };

        if (auto *il = dynamic_cast<IdList *>(c)) {
            for (auto &id : il->elems)
                remap_id(id.get());
            return;
        }
        if (auto *b = dynamic_cast<Block *>(c)) {
            if (b->slot_count > 0)
                b->slot_start += off;
            for (auto &e : b->elems)
                splice_tail(e, args, nparams, off);
            return;
        }
        if (auto *e14 = dynamic_cast<Expr14 *>(c)) {
            splice_tail(e14->lvalue, args, nparams, off);
            splice_tail(e14->rvalue, args, nparams, off);
            return;
        }
        if (auto *fs = dynamic_cast<ForStmt *>(c)) {
            splice_tail(fs->init, args, nparams, off);
            splice_tail(fs->cond, args, nparams, off);
            splice_tail(fs->inc, args, nparams, off);
            splice_tail(fs->body, args, nparams, off);
            return;
        }
        if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
            if (fe->ids)
                for (auto &id : fe->ids->elems)
                    remap_id(id.get());
            splice_tail(fe->container, args, nparams, off);
            splice_tail(fe->body, args, nparams, off);
            return;
        }
        if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
            splice_tail(tc->tryBody, args, nparams, off);
            for (auto &p : tc->catchStmts) {
                remap_id(p.first.asId.get());
                splice_tail(p.second, args, nparams, off);
            }
            splice_tail(tc->finallyBody, args, nparams, off);
            return;
        }

        for_each_child_slot(c,
            [&](unique_ptr<Construct> &ch) {
                splice_tail(ch, args, nparams, off);
            });
    }

    /*
     * Inline a TAIL call to a block-bodied function: `return f(args);` where
     * f's body always returns. Splicing f's body in place of the return is
     * sound because f's own `return`s become the caller's returns (it was a
     * tail call) and f never falls through. f's params are substituted (so they
     * must be non-reassigned and substitutable), and f's locals are RE-RESOLVED
     * - remapped to a fresh range at the top of the caller's frame (which grows
     * by f's local count, capped at 64 slots). This is the block-body analogue
     * of expression-body inlining; specialization (a shared clone) stays the
     * fallback for non-tail or non-substitutable calls.
     */
    void try_inline_tail(unique_ptr<Construct> &slot, int depth, int *fsize)
    {
        if (!fsize)
            return;       /* the enclosing function has no frame to grow */

        auto *ret = dynamic_cast<ReturnStmt *>(slot.get());
        if (!ret)
            return;
        auto *ce = dynamic_cast<CallExpr *>(ret->elem.get());
        if (!ce)
            return;

        auto *callee = dynamic_cast<Identifier *>(ce->what.get());
        if (!callee || callee->sym.kind == SymKind::local)
            return;

        auto it = spec_funcs.find(callee->uid);   /* block-bodied, no capture/
                                                   * nested func */
        if (it == spec_funcs.end() || !it->second)
            return;
        FuncDeclStmt *f = it->second;

        if (!f->resolved)
            return;       /* its locals aren't slotted: nothing to remap into */

        auto *body = dynamic_cast<const Block *>(f->body.get());
        if (!body || !block_always_returns(body))
            return;

        const int nparams = f->params
            ? static_cast<int>(f->params->elems.size()) : 0;
        if (static_cast<int>(ce->args->elems.size()) != nparams)
            return;

        const int bsz = node_count(f->body.get());
        if (bsz > max_nodes)
            return;

        /* Exclude a self-recursive callee (else it re-expands to the cap). */
        const UniqueId *fid = f->id->uid;
        if (count_matching(f->body.get(),
                [&](const Identifier *id) { return id->uid == fid; }) != 0)
            return;

        /*
         * Every param must be substitutable (see tail_arg_ok): never reassigned
         * in the body, and an arg whose value the body reads identically no
         * matter when or how often. Use count is by SLOT so a same-named
         * shadowing local doesn't mislead.
         */
        for (int i = 0; i < nparams; i++) {
            if (i < static_cast<int>(f->slot_writes.size())
                    && f->slot_writes[i] != 0)
                return;
            const int uses = count_matching(f->body.get(),
                [&](const Identifier *id) {
                    return id->sym.kind == SymKind::local
                        && id->sym.slot == i;
                });
            if (!tail_arg_ok(uses, ce->args->elems[i].get()))
                return;
        }

        const int nlocals = f->frame_size - nparams;
        if (*fsize + nlocals > MAX_FRAME_SLOTS)
            return;       /* would overflow the 64-slot frame */
        if (depth >= MAX_INLINE_DEPTH || bsz > inline_budget)
            return;
        inline_budget -= bsz;

        /* Splice: substitute params, remap locals into [caller_fsize, ...). */
        const int off = *fsize - nparams;

        std::vector<Construct *> args;
        for (auto &a : ce->args->elems)
            args.push_back(a.get());

        unique_ptr<Construct> spliced = f->body->clone();
        splice_tail(spliced, args, nparams, off);

        *fsize += nlocals;   /* the caller's frame absorbed f's locals */

        const InlineCtx *ic = alloc_inline_ctx(
            { std::string(f->id->get_str()), param_names(f),
              ce->start, ce->inline_ctx });
        tag_inline(spliced.get(), ic);

        if (analysis)
            analysis->mark(callee->start,
                static_cast<int>(callee->get_str().length()),
                AnnoKind::inlined);

        TRACE(inlining, 0, std::string(f->id->get_str()) +
              "  tail call -> splice (+" + std::to_string(nlocals) +
              " local slot(s))");

        slot = move(spliced);   /* the ReturnStmt becomes f's (spliced) body */
        refold(slot);
        walk(slot, depth + 1, fsize);   /* re-scan: nested tail/expr calls */
    }

    /* Sound iff the argument is evaluated as often as the param is used and a
     * side-effecting arg is neither dropped nor duplicated. An identifier or a
     * self-contained constant (scalar, or array/dict literal of constants) is
     * side-effect-free, so it can be duplicated; a constant can also be dropped
     * (an identifier cannot - that would skip an undefined-variable error). */
    static bool sub_ok(int uses, const Construct *arg)
    {
        if (uses == 1)
            return true;       /* single evaluation: any argument is fine */

        if (uses >= 2)
            return dynamic_cast<const Identifier *>(arg)
                || is_const_literal(arg);

        return is_const_literal(arg);     /* uses == 0: drop a const only */
    }

    /*
     * Substitutability of a tail-inline arg, stricter than sub_ok because a
     * block body may read the param after statements that change shared state.
     * The arg must be VALUE-STABLE - read identically whenever the body reads
     * it: a caller LOCAL (the spliced body can't reach the caller's frame, so
     * it can't reassign it) or a const literal. A global is excluded (the body
     * might reassign it before the use); so is any side-effecting expression.
     * For 0 or >=2 uses (drop / duplicate) it must also be identical and
     * immutable per copy: a scalar literal always, or - for >=2 - an identifier
     * (reads the one slot each time). A mutable array/dict literal would split
     * into independent objects, so it is only allowed for a single use.
     */
    static bool tail_arg_ok(int uses, const Construct *arg)
    {
        auto *id = dynamic_cast<const Identifier *>(arg);
        const bool local = id && id->sym.kind == SymKind::local;

        if (!local && !is_const_literal(arg))
            return false;     /* global / side-effecting: not value-stable */

        if (uses == 1)
            return true;      /* one copy: any value-stable arg is fine */

        return dynamic_cast<const Literal *>(arg)   /* scalar: drop or dup ok */
            || (uses >= 2 && local);                /* identifier: same slot */
    }

    static std::vector<std::string> param_names(const FuncDeclStmt *f)
    {
        std::vector<std::string> out;
        if (f->params)
            for (auto &p : f->params->elems)
                out.push_back(std::string(p->get_str()));
        return out;
    }

    static int node_count(const Construct *c)
    {
        if (!c)
            return 0;
        int n = 1;
        for_each_child(const_cast<Construct *>(c),
            [&](Construct *ch) { n += node_count(ch); });
        return n;
    }

    /*
     * A COMPLETE node count: unlike node_count (which uses for_each_child, and
     * so stops at Block/Expr14/for/foreach/try the way the resolver's walk()
     * handles them itself), this descends into those too. Needed by the
     * specialization shrink check, whose win is often deep inside a kept
     * statement (a folded `var t = a[0]+a[1]` rvalue) node_count can't see.
     */
    static int count_all_nodes(const Construct *c)
    {
        if (!c)
            return 0;
        int n = 1;

        if (auto *b = dynamic_cast<const Block *>(c)) {
            for (auto &e : b->elems)
                n += count_all_nodes(e.get());
        } else if (auto *e14 = dynamic_cast<const Expr14 *>(c)) {
            n += count_all_nodes(e14->lvalue.get());
            n += count_all_nodes(e14->rvalue.get());
        } else if (auto *f = dynamic_cast<const ForStmt *>(c)) {
            n += count_all_nodes(f->init.get());
            n += count_all_nodes(f->cond.get());
            n += count_all_nodes(f->inc.get());
            n += count_all_nodes(f->body.get());
        } else if (auto *fe = dynamic_cast<const ForeachStmt *>(c)) {
            n += count_all_nodes(fe->container.get());
            n += count_all_nodes(fe->body.get());
        } else if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
            n += count_all_nodes(tc->tryBody.get());
            for (auto &p : tc->catchStmts)
                n += count_all_nodes(p.second.get());
            n += count_all_nodes(tc->finallyBody.get());
        } else {
            for_each_child(const_cast<Construct *>(c),
                [&](Construct *ch) { n += count_all_nodes(ch); });
        }
        return n;
    }

    static int count_uses(const Construct *c, const UniqueId *uid)
    {
        if (!c)
            return 0;
        int n = 0;
        if (auto *id = dynamic_cast<const Identifier *>(c))
            if (id->uid == uid)
                n = 1;
        for_each_child(const_cast<Construct *>(c),
            [&](Construct *ch) { n += count_uses(ch, uid); });
        return n;
    }

    /*
     * A self-contained constant value node: a scalar literal, a baked const
     * value (LiteralObj), or an array/dict literal whose elements are all
     * themselves such values. These can be evaluated with no scope context, so
     * folding an op over them never needs a frame or const-global lookup (which
     * keeps it safe on a specialized clone, whose body still has slotted locals
     * that have no frame here).
     */
    static bool is_const_literal(const Construct *c)
    {
        if (!c)
            return false;
        if (dynamic_cast<const Literal *>(c)
                || dynamic_cast<const LiteralObj *>(c))
            return true;
        if (auto *la = dynamic_cast<const LiteralArray *>(c)) {
            for (auto &e : la->elems)
                if (!is_const_literal(e.get()))
                    return false;
            return true;
        }
        if (auto *ld = dynamic_cast<const LiteralDict *>(c)) {
            for (auto &kv : ld->elems)
                if (!is_const_literal(kv->key.get())
                        || !is_const_literal(kv->value.get()))
                    return false;
            return true;
        }
        return false;
    }

    /* A read-expression node we may try to evaluate-and-fold. */
    static bool is_foldable_expr(const Construct *c)
    {
        return dynamic_cast<const MultiOpConstruct *>(c)
            || dynamic_cast<const Subscript *>(c)
            || dynamic_cast<const Slice *>(c)
            || dynamic_cast<const MemberExpr *>(c)
            || dynamic_cast<const CallExpr *>(c);
    }

    /*
     * Does the subtree reference a slotted local (a runtime param/local)? That
     * read would deref the frame, which `cctx` doesn't have, so we must not try
     * to evaluate a node that contains one. (A const global / builtin is a map
     * lookup, sym.kind != local, and lives in cctx - safe.)
     */
    static bool has_slotted_local(const Construct *c)
    {
        if (!c)
            return false;
        if (auto *id = dynamic_cast<const Identifier *>(c))
            return id->sym.kind == SymKind::local;
        bool found = false;
        for_each_child(const_cast<Construct *>(c),
            [&](Construct *ch) { if (has_slotted_local(ch)) found = true; });
        return found;
    }

    /*
     * Bottom-up constant folding of a spliced/specialized body. Folds an
     * operator, subscript, slice, member access, or const-builtin call whose
     * operands are compile-time constants - scalar/array/dict literals AND
     * const globals (seeded into `cctx` from the top-level const decls) - to a
     * literal. The folded value's read-only-ness is preserved (a slice of a
     * const stays read-only; a fresh result stays mutable). A node that throws
     * (6/0, an out-of-bounds index, a type mismatch) or references a runtime
     * value (a slotted local, or a global not in cctx) is left for runtime,
     * matching the un-inlined call. Lvalue positions are NOT folded (an
     * assignment target or an lvalue builtin's first arg), so folding never
     * turns a write target into a value (changing a const-mutation error type).
     */
    void refold(unique_ptr<Construct> &slot)
    {
        if (!slot)
            return;

        /* Recurse, skipping lvalue positions. */
        Construct *cc = slot.get();
        if (auto *e14 = dynamic_cast<Expr14 *>(cc)) {
            refold(e14->rvalue);                 /* skip assignment target */
        } else if (auto *ce = dynamic_cast<CallExpr *>(cc)) {
            refold(ce->what);
            auto *callee = dynamic_cast<Identifier *>(ce->what.get());
            const bool lval0 = callee && ce->args && !ce->args->elems.empty()
                            && is_lvalue_arg_builtin(callee->get_str());
            for (size_t i = 0; i < ce->args->elems.size(); i++)
                if (!(lval0 && i == 0))          /* skip the lvalue first arg */
                    refold(ce->args->elems[i]);
        } else {
            for_each_child_slot(cc,
                [&](unique_ptr<Construct> &ch) { refold(ch); });
        }

        Construct *c = slot.get();
        if (is_const_literal(c) || !is_foldable_expr(c) || has_slotted_local(c))
            return;

        try {
            unique_ptr<Construct> lit;
            if (MakeConstructFromConstVal(
                    RValue(c->eval(&cctx)), lit, true, false)) {
                lit->start = c->start;
                lit->end = c->end;
                lit->inline_ctx = c->inline_ctx;
                slot = move(lit);
            }
        } catch (const Exception &) {
            /* not const-foldable / would throw: leave it for runtime */
        }
    }

    /*
     * Try to redirect a call to a block-bodied function to a specialized clone
     * (built once per (function, const-arg tuple) and shared). The clone keeps
     * the same signature/frame - the const args are still passed but ignored,
     * so no re-resolution is needed - and its body is folded with those params
     * bound to their constants. Only built when folding actually shrinks it.
     */
    void try_specialize(unique_ptr<Construct> &slot)
    {
        auto *ce = dynamic_cast<CallExpr *>(slot.get());
        if (!ce)
            return;

        auto *callee = dynamic_cast<Identifier *>(ce->what.get());
        if (!callee || callee->sym.kind == SymKind::local)
            return;

        auto it = spec_funcs.find(callee->uid);
        if (it == spec_funcs.end() || !it->second)
            return;

        FuncDeclStmt *f = it->second;
        if (!f->resolved)
            return;

        const size_t nparams = f->params ? f->params->elems.size() : 0;
        if (ce->args->elems.size() != nparams)
            return;

        /* Const args on never-reassigned params -> the binding seed. Scalars
         * bind directly; a const (deep read-only) array/dict binds too, so its
         * reads fold in the clone. Binding a read-only value is sound: it is
         * only ever substituted in read positions, and any mutation throws the
         * same error at runtime as the un-specialized call would. */
        std::unordered_map<int, EvalValue> seed;
        for (size_t i = 0; i < nparams; i++) {
            if (i >= f->slot_writes.size() || f->slot_writes[i] != 0)
                continue;          /* unknown or reassigned: don't bind it */

            Construct *arg = ce->args->elems[i].get();

            if (dynamic_cast<Literal *>(arg)) {
                seed[static_cast<int>(i)] = arg->eval(&cctx);
                continue;
            }

            /* A baked const array/dict literal, or an identifier bound to a
             * const global; bind only when its value is deep read-only. */
            if (dynamic_cast<LiteralObj *>(arg)
                    || dynamic_cast<Identifier *>(arg)) {
                try {
                    EvalValue v = RValue(arg->eval(&cctx));
                    if (is_readonly_value(v))
                        seed[static_cast<int>(i)] = move(v);
                } catch (const Exception &) {
                    /* not const-evaluable (a runtime local/global): skip */
                }
            }
        }

        if (seed.empty())
            return;

        const std::string key = spec_key(f, seed);
        FuncDeclStmt *clone;
        auto cit = spec_cache.find(key);

        if (cit != spec_cache.end()) {
            clone = cit->second;               /* may be nullptr: not built */
        } else {
            clone = build_specialization(f, seed);
            spec_cache.emplace(key, clone);
        }

        if (!clone)
            return;

        if (analysis)
            analysis->mark(callee->start,
                static_cast<int>(callee->get_str().length()),
                AnnoKind::specialized);

        TRACE(specialize, 0, std::string(callee->get_str()) +
              "  const arg(s) folded -> " +
              std::string(clone->id->get_str()));

        /* Redirect to the clone (same args; the const ones are now ignored). */
        auto what = make_unique<Identifier>(clone->id->get_str());
        what->start = ce->what->start;
        what->end = ce->what->end;
        ce->what = move(what);
    }

    FuncDeclStmt *build_specialization(
        FuncDeclStmt *f, const std::unordered_map<int, EvalValue> &seed)
    {
        const int before = count_all_nodes(f->body.get());

        unique_ptr<Construct> clone = f->clone();
        auto *fc = static_cast<FuncDeclStmt *>(clone.get());

        /* Synthetic name `<base>$s<N>` so the redirected call resolves to the
         * clone and it is readable/inspectable (the `$s` distinguishes a
         * specialization from a `<base>$N` template instance); display_name
         * keeps the original for backtraces. `base` is f's own original name
         * (f may itself be a template-instance clone, whose display_name is the
         * user's name). The counter is monotonic, so names never collide. */
        const std::string base = !f->display_name.empty()
            ? f->display_name : std::string(f->id->get_str());
        fc->display_name = base;
        fc->id = make_unique<Identifier>(
            base + "$s" + std::to_string(spec_counter++));

        auto *body = dynamic_cast<Block *>(fc->body.get());
        if (!body)
            return nullptr;

        if (!ac.fold_specialized(body, fc->slot_writes, fc->params.get(), seed))
            return nullptr;        /* const error: keep the ordinary call */

        /* AutoConst folds operators + scalar const-builtin calls; this also
         * folds self-contained subscript/slice/member/array-call results. */
        refold(fc->body);

        if (count_all_nodes(fc->body.get()) >= before)
            return nullptr;        /* folding didn't shrink it: not worth it */

        FuncDeclStmt *raw = fc;
        new_funcs.push_back(move(clone));
        return raw;
    }

    std::string spec_key(const FuncDeclStmt *f,
                         const std::unordered_map<int, EvalValue> &seed)
    {
        std::string k = "F";
        k += std::to_string(reinterpret_cast<uintptr_t>(f->id->uid));

        std::vector<int> slots;
        for (const auto &kv : seed)
            slots.push_back(kv.first);
        std::sort(slots.begin(), slots.end());

        for (int s : slots) {
            k += ";";
            k += std::to_string(s);
            k += "=";
            k += value_repr(seed.at(s));
        }
        return k;
    }

    static std::string value_repr(const EvalValue &v)
    {
        if (v.is<int_type>())
            return "i" + std::to_string(v.get<int_type>());
        if (v.is<bool>())
            return v.get<bool>() ? "bT" : "bF";
        if (v.is<float_type>())
            return "f" + std::to_string(
                static_cast<double>(v.get<float_type>()));
        if (v.is<NoneVal>())
            return "z";
        if (v.is<SharedStr>()) {
            const std::string_view sv = v.get<SharedStr>().get_view();
            return "s" + std::to_string(sv.size()) + ":" + std::string(sv);
        }
        /* An array/dict const: key by the shared object's identity (intptr).
         * The same const object (e.g. a const global passed at several sites)
         * keys once and shares one clone; two structurally-equal but distinct
         * literals key apart (a missed de-dup, never a wrong reuse). */
        if (v.is<SharedArrayObj>() || v.is<intrusive_ptr<DictObject>>())
            return "p" + std::to_string(v.get_type()->intptr(v)
                                            .get<int_type>());
        return "?";
    }

    void substitute(unique_ptr<Construct> &slot, const UniqueId *uid,
                    const Construct *arg)
    {
        if (!slot)
            return;

        if (auto *id = dynamic_cast<Identifier *>(slot.get())) {
            if (id->uid == uid) {
                unique_ptr<Construct> a = arg->clone();  /* fresh per use */
                a->start = slot->start;   /* keep the param's source position */
                a->end = slot->end;       /* so errors point as in the callee */
                slot = move(a);
                return;
            }
        }

        for_each_child_slot(slot.get(),
            [&](unique_ptr<Construct> &ch) { substitute(ch, uid, arg); });
    }

    /* Copy an existing inlined-at chain, rooting it at `root` (used when the
     * cloned body was itself inlined-into). Fresh copies leave the original
     * (shared by the un-inlined source) untouched. */
    static const InlineCtx *rebase(const InlineCtx *chain,
                                   const InlineCtx *root)
    {
        if (!chain)
            return root;
        return alloc_inline_ctx({ chain->callee_name, chain->params,
                                  chain->call_site,
                                  rebase(chain->parent, root) });
    }

    static void tag_inline(Construct *c, const InlineCtx *ic)
    {
        if (!c)
            return;
        c->inline_ctx = rebase(c->inline_ctx, ic);
        for_each_child(c, [&](Construct *ch) { tag_inline(ch, ic); });
    }
};

/*
 * Public entry point: run the name-resolution pass over the parsed tree (the
 * top-level frame size is recorded on the root Block's slot_count; the runtime
 * builds the "main" Frame from it in Block::do_eval), then - unless disabled -
 * the inlining pass.
 */
void
resolve_names(Construct *root, bool enable_inline, int inline_threshold,
              AnalysisInfo *analysis, bool repl_mode, EvalContext *prior_pure)
{
    Resolver().run(root, analysis, repl_mode, prior_pure);

    if (enable_inline)
        if (auto *rb = dynamic_cast<Block *>(root))
            Inliner(inline_threshold, analysis).run(rb);
}

/*
 * Post-resolve walk that records the resolver-decided optimizations still
 * readable on the tree: an auto-pure function (effective_pure but not written
 * `pure`) and an auto-const parameter (never reassigned, not `const`) - both
 * yellow. A complete traversal: for_each_child skips Block/for/foreach/try/
 * Expr14/FuncDeclStmt (the resolver's own walk handles those), so descend into
 * them here, including nested function bodies.
 */
void collect_resolver_analysis(Construct *root, AnalysisInfo &out)
{
    if (!root)
        return;

    std::function<void(Construct *)> walk = [&](Construct *c) {

        if (!c)
            return;

        if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {

            if (fd->id && fd->effective_pure && !fd->explicit_pure)
                out.mark(fd->id->start,
                         static_cast<int>(fd->id->get_str().length()),
                         AnnoKind::auto_const);

            if (fd->params)
                for (auto &p : fd->params->elems)
                    if (p->auto_const_param && !p->const_param)
                        out.mark(p->start,
                                 static_cast<int>(p->get_str().length()),
                                 AnnoKind::auto_const);

            walk(fd->body.get());
            return;
        }

        if (auto *b = dynamic_cast<Block *>(c)) {
            for (auto &e : b->elems)
                walk(e.get());
        } else if (auto *f = dynamic_cast<ForStmt *>(c)) {
            walk(f->init.get()); walk(f->cond.get());
            walk(f->inc.get());  walk(f->body.get());
        } else if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
            walk(fe->container.get()); walk(fe->body.get());
        } else if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
            walk(tc->tryBody.get());
            for (auto &p : tc->catchStmts)
                walk(p.second.get());
            walk(tc->finallyBody.get());
        } else if (auto *e14 = dynamic_cast<Expr14 *>(c)) {
            walk(e14->lvalue.get()); walk(e14->rvalue.get());
        } else {
            for_each_child(c, walk);
        }
    };

    walk(root);
}
