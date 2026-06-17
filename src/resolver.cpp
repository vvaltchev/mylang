/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"
#include "resolver.h"
#include "errors.h"
#include "eval.h"

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
    std::vector<std::pair<const UniqueId *, int>> decls;
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

/* True if `v` is a read-only (const-backed) array or dict value. Mirrors the
 * static helper of the same name in parser.cpp; scalars are never read-only. */
static bool is_readonly_value(const EvalValue &v)
{
    if (v.is<SharedArrayObj>())
        return v.get<SharedArrayObj>().is_readonly();
    if (v.is<shared_ptr<DictObject>>())
        return v.get<shared_ptr<DictObject>>()->is_readonly();
    return false;
}

class AutoConst {

    EvalContext cctx;   /* const context for evaluating folded constants */

public:

    AutoConst() : cctx(nullptr, true) { }

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
                          || kv.second.is<shared_ptr<DictObject>>();
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

        MakeConstructFromConstVal(
            EvalValue(static_cast<int_type>(result)), slot, false);
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
                        fc.consts[id->sym.slot] = e14->rvalue->eval(&cctx);
                        continue;
                    }

                    kept.push_back(move(e));
                    continue;
                }

                fold_reads(e14->rvalue, fc);   /* assignment: fold rhs only */
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
            fold_function(fd->body.get(), fd->slot_writes,     /* nested func */
                          fd->params.get());
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
                unique_ptr<Construct> taken =
                    v.get_type()->is_true(v) ? move(iff->thenBlock)
                                             : move(iff->elseBlock);
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
                if (!v.get_type()->is_true(v))
                    return false;          /* while (false): dead, drop it
                                            * without folding the body */
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
            fold_reads(e14->rvalue, fc);   /* assignment as a statement */
            return true;
        }

        fold_reads(slot, fc);              /* expression statement */
        return true;
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
                    /* Scalars inline as a literal. A seeded array/dict const
                     * (specialization only) bakes into a read-only LiteralObj
                     * so refold can fold reads of it - process_arrays and
                     * immutable both on. The value is already read-only, so any
                     * mutation of it still throws the same error at runtime. */
                    const bool arr =
                        it->second.is<SharedArrayObj>()
                        || it->second.is<shared_ptr<DictObject>>();
                    MakeConstructFromConstVal(it->second, slot, arr, arr);
                }
            }
            return;
        }

        if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
            fold_function(fd->body.get(), fd->slot_writes,   /* func expr */
                          fd->params.get());
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
                MakeConstructFromConstVal(RValue(mo->eval(&cctx)), slot, false);
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
                try {
                    MakeConstructFromConstVal(RValue(ce->eval(&cctx)),
                                              slot, false);
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
    void run(Construct *root)
    {
        top_level_only = false;
        walk(root, nullptr);            /* pass 1: functions; fill `escaped` */

        top_level_only = true;          /* pass 2: top level as "main" */
        FuncState main_st;
        main_st.slottable = true;
        main_st.is_main = true;
        walk(root, &main_st);

        /* Promote write-once scalar vars to constants and fold (uses the write
         * counts just collected; the top-level frame's in main_st.writes). */
        if (auto *rb = dynamic_cast<Block *>(root))
            AutoConst().run(rb, main_st.writes);
    }

private:

    /* Names a function reads from outer scope: kept in the map, not slotted. */
    std::unordered_set<const UniqueId *> escaped;

    /* Pass 2: function bodies are already resolved, so don't re-enter them. */
    bool top_level_only = false;

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
         * so add it as a masking entry instead of slotting it. */
        const bool keep_in_map = cur->is_main && escaped.count(id->uid);

        if (keep_in_map || cur->next_slot >= MAX_SLOTS) {
            cur->scopes.back().decls.push_back({ id->uid, -1 });
            return;
        }

        const int slot = cur->next_slot++;
        cur->scopes.back().decls.push_back({ id->uid, slot });
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
        cur->scopes.back().decls.push_back({ id->uid, -1 });
    }

    /* Throw AlreadyDefinedEx if `id` is already declared in this scope. */
    void check_no_redecl(FuncState *cur, Identifier *id) const
    {
        for (const auto &d : cur->scopes.back().decls) {
            if (d.first == id->uid)
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
                    if (d.first == id->uid) {
                        if (d.second >= 0)
                            id->sym = ResolvedSym{ SymKind::local, d.second };
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
 * const (is_const: a const global, const builtin, or explicitly-pure func), and
 * the body declares no nested function. Reads the resolver's sym.kind, so it
 * must run AFTER the body is walked. Conservative: self-recursion (the func's
 * own name is a free, non-const reference) and calls to other auto-pure (not
 * explicitly-pure) functions are not recognized, so such functions stay impure.
 */
bool
func_body_is_pure(const Construct *c)
{
    if (!c)
        return true;

    if (auto *id = dynamic_cast<const Identifier *>(c))
        return id->sym.kind == SymKind::local || id->is_const;

    if (dynamic_cast<const FuncDeclStmt *>(c))
        return false;   /* a nested function: be conservative */

    if (auto *b = dynamic_cast<const Block *>(c)) {
        for (auto &e : b->elems)
            if (!func_body_is_pure(e.get()))
                return false;
        return true;
    }
    if (auto *f = dynamic_cast<const ForStmt *>(c))
        return func_body_is_pure(f->init.get())
            && func_body_is_pure(f->cond.get())
            && func_body_is_pure(f->inc.get())
            && func_body_is_pure(f->body.get());
    if (auto *fe = dynamic_cast<const ForeachStmt *>(c))
        return func_body_is_pure(fe->container.get())
            && func_body_is_pure(fe->body.get());
    if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
        if (!func_body_is_pure(tc->tryBody.get()))
            return false;
        for (auto &p : tc->catchStmts)
            if (!func_body_is_pure(p.second.get()))
                return false;
        return func_body_is_pure(tc->finallyBody.get());
    }
    if (auto *e14 = dynamic_cast<const Expr14 *>(c))
        return func_body_is_pure(e14->lvalue.get())
            && func_body_is_pure(e14->rvalue.get());

    bool ok = true;
    for_each_child(const_cast<Construct *>(c),
                   [&](Construct *ch) { ok = ok && func_body_is_pure(ch); });
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
            st.scopes.back().decls.push_back({ p->uid, i });
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
            && func_body_is_pure(fd->body.get())) {
        fd->effective_pure = true;
    }

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

    /* (func, const-arg tuple) -> the specialized clone, or nullptr if building
     * it was not beneficial (cached so it isn't retried). */
    std::unordered_map<std::string, FuncDeclStmt *> spec_cache;

    /* specialized clones to register (inserted at the root block's front after
     * the walk, so they exist before any call reaches them). */
    std::vector<unique_ptr<Construct>> new_funcs;
    int spec_counter = 0;

public:

    explicit Inliner(int max_nodes)
        : max_nodes(max_nodes), cctx(nullptr, true) { }

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

        for (auto &e : root->elems)
            walk(e);

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

    void walk(unique_ptr<Construct> &slot)
    {
        if (!slot)
            return;

        /* Recurse first; do NOT re-scan a spliced result (single level). */
        for_each_child_slot(slot.get(),
            [&](unique_ptr<Construct> &ch) { walk(ch); });

        try_inline(slot);
        try_specialize(slot);   /* if still a call to a block-bodied func */
    }

    void try_inline(unique_ptr<Construct> &slot)
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

        if (node_count(f->body.get()) > max_nodes)
            return;

        for (size_t i = 0; i < nparams; i++) {
            const int uses = count_uses(f->body.get(),
                                        f->params->elems[i]->uid);
            if (!sub_ok(uses, ce->args->elems[i].get()))
                return;
        }

        /*
         * Eligible: clone the body, substitute params with the args, then tag
         * the whole result as inlined. Substitution happens first and copies
         * each parameter occurrence's source loc onto the arg, so an error in
         * the spliced body points where it would in the un-inlined callee (the
         * operator ladder stamps the operand loc) - keeping the backtrace
         * identical. Tagging last covers body and args alike.
         */
        const InlineCtx *ic = alloc_inline_ctx(
            { std::string(f->id->get_str()), param_names(f),
              ce->start, nullptr });

        unique_ptr<Construct> body = f->body->clone();

        for (size_t i = 0; i < nparams; i++)
            substitute(body, f->params->elems[i]->uid,
                       ce->args->elems[i].get());

        tag_inline(body.get(), ic);

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

        /* Synthetic name so the redirected call resolves to the clone, but keep
         * the original name for backtraces. */
        fc->display_name = std::string(f->id->get_str());
        fc->id = make_unique<Identifier>(
            "$spec" + std::to_string(spec_counter++));

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
        if (v.is<SharedArrayObj>() || v.is<shared_ptr<DictObject>>())
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
resolve_names(Construct *root, bool enable_inline, int inline_threshold)
{
    Resolver().run(root);

    if (enable_inline)
        if (auto *rb = dynamic_cast<Block *>(root))
            Inliner(inline_threshold).run(rb);
}
