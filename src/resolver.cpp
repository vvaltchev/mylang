/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"
#include "resolver.h"
#include "inferencer.h"   /* specialize_types (run_optimizers) */
#include "analyzer.h"
#include "errors.h"
#include "eval.h"
#include "trace.h"

#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>

/*
 * Name-resolution pass: resolve a function's PARAMETERS and LOCAL variables
 * (var/const, for-init, foreach variables, catch variables), plus TOP-LEVEL
 * variables and functions, to fixed slots, so references become O(1) slot
 * reads at runtime instead of std::map lookups walking the scope chain.
 *
 * Two slot spaces: a per-call FRAME (a function's params/locals, and a main
 * var no function reads) and the program-wide GLOBAL table (every top-level
 * function, and every top-level var a function reads). A reference resolves to
 * SymKind::local (frame) or SymKind::global (table).
 *
 * What is and isn't resolved:
 *
 *   - params, var/const, for/foreach/catch variables  -> SymKind::local (frame)
 *   - top-level functions (FuncDeclStmt::id)          -> SymKind::global. They
 *         are HOISTED to the table up front (before bodies), so a forward /
 *         mutually-recursive reference resolves; the slot is bound when the
 *         decl executes. A function-name decl still creates a scope entry so it
 *         correctly SHADOWS an outer binding of the same name.
 *   - top-level vars -> SymKind::local in the implicit "main" frame, EXCEPT any
 *         a function reads ("escaped"): those join the GLOBAL table
 *         (SymKind::global), so a function reaches them as an O(1) slot too -
 *         no map walk for any user global. See run()'s two passes, `escaped`,
 *         and `escaped_refs`.
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

#ifdef TESTS
/* #93 reach diagnostics - defined further down, beside the other counters;
 * declared here because the analysis lives in the anonymous namespace below
 * and an `extern` there would get internal linkage instead. */
extern unsigned long g_esc_p_callee, g_esc_p_gwrite,
                     g_esc_p_hobuiltin, g_esc_p_shape;
#endif

namespace {

/* Max slots per frame. The Frame no longer has a fixed-width liveness word
 * (slots are just default-constructed storage), so this is only a generous
 * sanity backstop - a function with more than this many locals falls back to
 * the map (effectively never hit). */
constexpr int MAX_SLOTS = 1 << 20;

void for_each_child(Construct *c, const std::function<void(Construct *)> &fn);

/*
 * A lexical scope (a function's param scope, a block, a for/foreach header, or
 * catch clause). `decls` maps each declared name to its slot, or to -1 for a
 * "masking" entry: a name that is in scope but NOT slotted (a function name, or
 * a local that overflowed the slot budget). A masking entry shadows an outer
 * slotted binding while still resolving to a runtime map lookup.
 */
struct Scope {
    /* name -> { slot (or -1 = masked/map), kind, explicit-type annotation }.
     * `kind` is SymKind::local (a frame slot) or SymKind::global (a top-level
     * var/function in the global table) - so a reference stamps the right
     * storage. The annotation is propagated to every use so an assignment can
     * coerce a widening value to the declared type (e.g. `float f; f = 3;`). */
    struct Decl {
        const UniqueId *name;
        int slot;
        SymKind kind;
        DeclType type;
    };
    std::vector<Decl> decls;

    /*
     * Every name this scope's block DECLARES, including declarations
     * lexically BELOW the current walk position. Filled by
     * collect_scope_names BEFORE the block's statements are walked.
     *
     * Used ONLY by the FIX-1 check ("is this name declared anywhere in an
     * enclosing scope?") - never by resolution, which still walks forward
     * and so still reads an OUTER binding for a name shadowed later. That
     * separation is deliberate: FIX-1 must distinguish "declared nowhere"
     * (a typo - a compile error) from "declared below" (legal today, and
     * what step 3's TDZ will turn into UseBeforeBindingEx), and
     * `unresolved AT THIS POINT` cannot tell them apart.
     */
    std::vector<const UniqueId *> all_names;

    /*
     * The subset of `all_names` that has a TEMPORAL DEAD ZONE: `var`/`const`
     * declarations only. A func or struct declaration HOISTS WITH ITS
     * BINDING (it is a compile-time entity with no runtime initialisation
     * order), which is what keeps mutual recursion and the "helpers below
     * main" layout legal - so it is never "declared but unbound" and must
     * NOT raise UseBeforeBindingEx. This is JavaScript's `function`-vs-`let`
     * split, which MyLang already had for funcs; TDZ adds the `let` half.
     */
    std::vector<const UniqueId *> var_names;
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
        || name == "sort" || name == "rev_sort" || name == "reverse";
}

/*
 * A REST-NATIVE mutating builtin: arg0 is an lvalue, and its value args (1..n)
 * are pre-evaluated (make_builtin_lv_v), so the VM compiles a register run for
 * them. The self-eval ones (append/push - construct-in-place needs the node;
 * pop/intptr - no value args) are NOT in this set.
 */
static bool is_lvalue_rest_native_builtin(std::string_view name)
{
    return name == "insert" || name == "erase";
}

/*
 * A REST-NATIVE-CAPABLE mutating builtin: its value arg(s) CAN be pre-evaluated,
 * but the codegen decides PER-OP whether to (see lvalue_rest_capable). Two here:
 *   - append/push: the single value, PLAIN case only (the ctor case is
 *     EmplaceStruct, the subscript-target case is CallBuiltinLVElem).
 *   - sort/rev_sort: the OPTIONAL cmp arg - pre-evaluated into rest[0] (an empty
 *     run for the no-cmp `sort(a)`); sort_core uses rest instead of node->eval.
 * NOT here: insert/erase (rest-native ALWAYS), pop/intptr/reverse (no value
 * args - they hold the node only for carets, freed later by func_lv->ArgLocs).
 */
static bool is_lvalue_rest_capable_builtin(std::string_view name)
{
    return name == "append" || name == "push"
        || name == "sort" || name == "rev_sort";
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

/* True if `c` STRUCTURALLY yields a bool: a comparison, a logical op, a unary
 * `!`, a bool literal (or their M8 typed forms). Used to decide when
 * `false || x` / `true && x` may collapse to `x` - sound only when `x` is
 * already bool, since mylang's &&/|| yield bool (so bool(x) == x there). */
static bool produces_bool(const Construct *c)
{
    if (dynamic_cast<const LiteralBool *>(c)
            || dynamic_cast<const Expr06 *>(c)    /* < > <= >= */
            || dynamic_cast<const Expr07 *>(c)    /* == != */
            || dynamic_cast<const Expr11 *>(c)    /* && */
            || dynamic_cast<const Expr12 *>(c))   /* || */
        return true;
    if (auto *u = dynamic_cast<const Expr02 *>(c))  /* unary `!` */
        return u->elems.size() == 1 && u->elems[0].first == Op::lnot;
    if (auto *ts = dynamic_cast<const TypedScalarExpr *>(c))
        return ts->cat == TypedScalarExpr::Cat::cmp
            || ts->cat == TypedScalarExpr::Cat::logical
            || ts->cat == TypedScalarExpr::Cat::lnot;
    return false;
}

/* True if `uid` is referenced anywhere in the subtree. A COMPLETE traversal
 * (handles the containers for_each_child stops at), so a self-call buried in a
 * block body is found. Used to detect a (self-)recursive function. */
static bool refs_uid(const Construct *c, const UniqueId *uid)
{
    if (!c)
        return false;
    if (auto *id = dynamic_cast<const Identifier *>(c))
        return id->uid == uid;
    if (auto *b = dynamic_cast<const Block *>(c)) {
        for (auto &e : b->elems)
            if (refs_uid(e.get(), uid)) return true;
        return false;
    }
    if (auto *f = dynamic_cast<const ForStmt *>(c))
        return refs_uid(f->init.get(), uid) || refs_uid(f->cond.get(), uid)
            || refs_uid(f->inc.get(), uid) || refs_uid(f->body.get(), uid);
    if (auto *fe = dynamic_cast<const ForeachStmt *>(c))
        return refs_uid(fe->container.get(), uid) || refs_uid(fe->body.get(), uid);
    if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
        if (refs_uid(tc->tryBody.get(), uid)) return true;
        for (auto &p : tc->catchStmts)
            if (refs_uid(p.second.get(), uid)) return true;
        return refs_uid(tc->finallyBody.get(), uid);
    }
    if (auto *e = dynamic_cast<const Expr14 *>(c))
        return refs_uid(e->lvalue.get(), uid) || refs_uid(e->rvalue.get(), uid);
    bool found = false;
    for_each_child(const_cast<Construct *>(c),
        [&](Construct *ch) { if (refs_uid(ch, uid)) found = true; });
    return found;
}

/* A (self-)recursive function: its body references its own name. Such a func
 * may be auto-pure (a recursive call to a pure function is pure), but it must
 * NOT be eagerly const-folded - evaluating `fib(40)` at compile time would hang.
 * A const-arg recursion instead folds only through the depth/budget-bounded
 * inliner unroll. Mutual recursion is not detected here (stays conservative). */
static bool func_is_self_recursive(const FuncDeclStmt *fd)
{
    return fd->id && fd->body && refs_uid(fd->body.get(), fd->id->uid);
}

/* Count references to `uid` in the subtree (a COMPLETE traversal, like
 * refs_uid). Used to find a TREE-recursive function (>=2 self-calls), where
 * unrolling produces duplicate self-calls that the per-frame cache dedups. */
static int count_uid(const Construct *c, const UniqueId *uid)
{
    if (!c)
        return 0;
    if (auto *id = dynamic_cast<const Identifier *>(c))
        return id->uid == uid ? 1 : 0;
    int n = 0;
    if (auto *b = dynamic_cast<const Block *>(c)) {
        for (auto &e : b->elems) n += count_uid(e.get(), uid);
        return n;
    }
    if (auto *f = dynamic_cast<const ForStmt *>(c))
        return count_uid(f->init.get(), uid) + count_uid(f->cond.get(), uid)
             + count_uid(f->inc.get(), uid) + count_uid(f->body.get(), uid);
    if (auto *fe = dynamic_cast<const ForeachStmt *>(c))
        return count_uid(fe->container.get(), uid)
             + count_uid(fe->body.get(), uid);
    if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
        n += count_uid(tc->tryBody.get(), uid);
        for (auto &p : tc->catchStmts) n += count_uid(p.second.get(), uid);
        return n + count_uid(tc->finallyBody.get(), uid);
    }
    if (auto *e = dynamic_cast<const Expr14 *>(c))
        return count_uid(e->lvalue.get(), uid) + count_uid(e->rvalue.get(), uid);
    for_each_child(const_cast<Construct *>(c),
        [&](Construct *ch) { n += count_uid(ch, uid); });
    return n;
}

/* A pure, TREE-recursive (>=2 self-calls) function. Unrolling it brings its
 * duplicate self-calls into one frame, where the per-frame pure-call cache
 * dedups them (the v3 fib win). A LINEAR recursion (1 self-call) has no dups,
 * so it is NOT included - unrolling it would only grow the body. */
static bool func_is_cacheable_recursive(const FuncDeclStmt *fd)
{
    return fd->desc->effective_pure && fd->id && fd->body
        && count_uid(fd->body.get(), fd->id->uid) >= 2;
}

class AutoConst {

    EvalContext cctx;   /* const context for evaluating folded constants */
    AnalysisInfo *analysis;   /* -a: record auto-const/dead/folded, or null */
    bool repl_mode;     /* REPL: an unresolved name may be a map global */

public:

    explicit AutoConst(AnalysisInfo *a = nullptr,
                       EvalContext *prior_pure = nullptr, bool repl = false)
        : cctx(nullptr, true), analysis(a), repl_mode(repl)
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
                if (v.is<intrusive_ptr<FuncObject>>() &&
                    v.get<intrusive_ptr<FuncObject>>()->func->effective_pure &&
                    /* a recursive pure func stays unfolded - don't run it at
                     * compile time (it could be fib(40)); see func_is_self_
                     * recursive. Its purity is still used for inlining/CSE. */
                    !func_is_self_recursive(
                        v.get<intrusive_ptr<FuncObject>>()->func->decl)) {
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
     * Fold `defined(name)` -> true when `name` resolves to a symbol that is
     * ALWAYS bound: a LOCAL (a param bound at every call - an omitted trailing
     * opt still binds `none`; a var/for/foreach/catch local, which a resolved
     * reference can only reach AFTER its decl ran - forward resolution - and
     * whose slot has no per-slot liveness, so it holds a defined value, never
     * UndefinedId), a CAPTURE (snapshot at closure creation), or an unshadowed
     * BUILTIN (`SymKind::builtin` only exists in a fully-slotted SCRIPT). All
     * three are sound in both script and REPL mode with no gating. A GLOBAL is
     * NOT foldable (its slot's defined-flag is set only when its decl executes -
     * `defined(g)` is false before that runs, true after - a genuine runtime
     * property, left for DefinedGlobalV). An UNRESOLVED name folds to `false` in
     * a SCRIPT (the runtime symbols map is empty + asserted, so it can never be
     * defined at runtime; byte-identical to `arg->eval` yielding UndefinedId) -
     * but NOT in the REPL, where it may be a live open-world map global. A
     * non-identifier arg (`defined(a[0])`, a rare misuse) is still left as a
     * runtime `defined()` call. Returns true iff it folded. Closes the
     * AST-builtin fold gap that made `s + defined(p)` emit a VM node
     * fallback (see plans/archived/vm-fallback-elimination.md).
     */
    bool try_fold_defined(unique_ptr<Construct> &slot, CallExpr *ce)
    {
        auto *id = dynamic_cast<Identifier *>(ce->args->elems[0].get());
        if (!id)
            return false;
        const SymKind k = id->sym.kind;
        /* An ALWAYS-BOUND name (a param/local/capture, or a builtin) is defined
         * -> fold to `true`. A GLOBAL is a genuine runtime property (its decl
         * may not have run) -> left for DefinedGlobalV. */
        if (k == SymKind::local || k == SymKind::capture
                || k == SymKind::builtin || k == SymKind::global
                || id->in_tdz) {
            /* TDZ (#131): `in_tdz` means the name is DECLARED in this scope
             * but its declaration has not been walked yet - `defined()` asks
             * whether the name EXISTS, so the answer is true. Whether its
             * value is bound yet is `isbound()`'s question (step 6), which
             * takes over the runtime DefinedGlobalV check. */
            MakeConstructFromConstVal(EvalValue(true), slot, false);
            return true;
        }
        /* An UNRESOLVED name in a SCRIPT is never defined at runtime (the
         * runtime symbols map is empty and asserted so - see eval.h), so
         * `defined(x)` is a constant `false` - eliminating the AST-builtin call.
         * NOT in the REPL, where an unresolved name may be an open-world map
         * global. `arg->eval` would return the UndefinedId sentinel, which is
         * exactly what builtin_defined tests, so this is byte-identical. */
        if (k == SymKind::unresolved && !repl_mode) {
            MakeConstructFromConstVal(EvalValue(false), slot, false);
            return true;
        }
        return false;
    }

    /*
     * `isbound(name)` - has the DECLARATION run yet? The TDZ twin of
     * try_fold_defined, and the answers are deliberately different:
     *
     *   func f() { var b = isbound(a); var a = 5; return b; }   -> FALSE
     *   func f() { var b = defined(a); var a = 5; return b; }   -> true
     *
     * For a LEXICAL kind the answer is a compile-time constant: a param /
     * capture / builtin is always bound, and a local is bound exactly after
     * its declaration - `in_tdz` is the resolver's own record of "declared in
     * this scope, declaration not yet walked", so the fold is exact. Each loop
     * iteration re-runs the declaration, so "before the decl" is false on
     * every iteration too and no runtime bound-flag is needed.
     *
     * A GLOBAL read from a function body is the one genuine RUNTIME query (the
     * decl may not have executed): left un-folded for DefinedGlobalV in the VM
     * and builtin_isbound in the tree-walker.
     */
    bool try_fold_isbound(unique_ptr<Construct> &slot, CallExpr *ce)
    {
        Construct *arg = ce->args->elems[0].get();
        /* `none` is always available - the inferencer lets it through the
         * identifier check for that reason (see reject_dev_builtins). */
        if (dynamic_cast<LiteralNone *>(arg)) {
            MakeConstructFromConstVal(EvalValue(true), slot, false);
            return true;
        }
        auto *id = dynamic_cast<Identifier *>(arg);
        if (!id)
            return false;                  /* the arg-shape error, not ours */
        if (id->in_tdz) {
            MakeConstructFromConstVal(EvalValue(false), slot, false);
            return true;
        }
        const SymKind k = id->sym.kind;
        if (k == SymKind::local || k == SymKind::capture
                || k == SymKind::builtin) {
            MakeConstructFromConstVal(EvalValue(true), slot, false);
            return true;
        }
        /*
         * A name that exists NOWHERE is not bound -> `false`, the same shape
         * try_fold_defined uses. This is an OPTIMIZATION, not a correctness
         * fix, and the distinction is worth stating because the obvious
         * assumption is the other way: the runtime path answers correctly
         * without it (builtin_isbound's unresolved arm evaluates the
         * Identifier, which yields the UndefinedId sentinel), so removing the
         * fold leaves the whole suite green in all five modes - measured.
         * What it buys is eliminating the call. NOT in the REPL, where an
         * unresolved name may be an open-world map global.
         */
        if (k == SymKind::unresolved && !repl_mode) {
            MakeConstructFromConstVal(EvalValue(false), slot, false);
            return true;
        }
        return false;   /* global -> runtime; unresolved -> the REPL's map */
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
            /* Register a pure func into the fold context so its const-arg calls
             * fold - EXCEPT a recursive one: evaluating it at compile time
             * (fib(40)) could hang. A recursive pure func keeps its purity flag
             * (for inlining/CSE) but its const-arg recursion folds only via the
             * depth/budget-bounded unroll. */
            if (fd->desc->effective_pure && fd->id
                    && !func_is_self_recursive(fd)) {
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

                    kept.push_back(std::move(e));
                    continue;
                }

                fold_reads(e14->rvalue, fc);   /* assignment: rhs ... */
                fold_lvalue_reads(e14->lvalue, fc);   /* ... and lvalue reads */
                kept.push_back(std::move(e));
                continue;
            }

            if (fold_child(e, fc))
                kept.push_back(std::move(e));
        }

        b->elems = std::move(kept);
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
                    t ? std::move(iff->thenBlock) : std::move(iff->elseBlock);
                if (!taken || !fold_child(taken, fc))
                    return false;          /* no live branch (or folds away) */
                slot = std::move(taken);        /* replace if with its live branch */
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
        if (!lvalue || lvalue->is_id() || lvalue->is_idlist())
            return;
        /*
         * Fold the INTERIOR reads of a write chain - the indexes - but
         * never the chain's BASE SPINE (`arr` in `arr[j] = v`, `a` in
         * `a[i][j] = v`, `s` in `s.f = v`).
         *
         * Before specialization relaxed the base block this distinction was
         * moot: a subscript/member base was blocked from promotion
         * entirely, so an id on the spine never had a const to fold and
         * folding "the whole lvalue" only ever touched the indexes. The
         * relaxed seed set (block_subscript_bases=false) lets a seeded
         * const ARRAY fold into subscript bases - sound for READS, but
         * folding it into a WRITE target's base turns `arr[j] = n` into
         * `Obj([...])[j] = n`. The tree-walker happened to survive that
         * (evaluating the read-only literal's subscript for write throws
         * the same NotLValueEx the un-specialized call throws), but the
         * NO-FAIL CODEGEN cannot lower a store whose base is a literal -
         * so a legal program (pass a const array to a function that writes
         * its param; must THROW at runtime) was refused at COMPILE time
         * with NotLoweredEx.
         */
        Construct *c = lvalue.get();
        while (c) {
            if (auto *sub = dynamic_cast<Subscript *>(c)) {
                fold_reads(sub->index, fc);      /* the index is a READ */
                c = sub->what.get();             /* descend the spine */
            } else if (auto *mem = dynamic_cast<MemberExpr *>(c)) {
                c = mem->what.get();             /* nothing to fold here */
            } else {
                /* the spine's root: an Identifier stays (the whole point),
                 * and any other rvalue root (`f()[0] = v`) is left alone
                 * too, exactly as refold leaves assignment targets */
                break;
            }
        }
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
            /* Simplify a logical op with const LEADING operands. mylang's
             * &&/|| yield a BOOL (not the operand, unlike Python: `false || 5`
             * is `true`, not `5`), so the rules are:
             *   - a const that DETERMINES the result (`false && rest` -> false,
             *     `true || rest` -> true) folds the whole thing to that bool -
             *     sound regardless of `rest`, which is short-circuited (never
             *     evaluated, even side effects). This eliminates a
             *     `const FLAG=false; if (FLAG && ...)` branch (the if-DCE then
             *     drops the `if (false)`);
             *   - a NON-determining leading const (`true && rest`, `false ||
             *     rest`) contributes nothing and is dropped. If >=2 operands
             *     remain it stays a logical op (still -> bool, sound for any
             *     operand type); if exactly ONE remains the result is bool(op),
             *     so we drop the const only when that operand is ALREADY a bool
             *     (a comparison / logical / `!` - else bool(x) != x). */
            const bool is_and = dynamic_cast<Expr11 *>(mo) != nullptr;
            const bool is_or  = dynamic_cast<Expr12 *>(mo) != nullptr;
            if (!all_lit && (is_and || is_or) && mo->elems.size() >= 2
                    && is_scalar_literal(mo->elems[0].second.get())) {
                size_t drop = 0;
                bool determined = false, det_val = false;
                for (; drop < mo->elems.size(); drop++) {
                    Construct *op = mo->elems[drop].second.get();
                    if (!is_scalar_literal(op))
                        break;
                    EvalValue v = RValue(op->eval(&cctx));
                    const bool t = v.get_type()->is_true(v);
                    if ((is_and && !t) || (is_or && t)) {
                        determined = true; det_val = t; break;
                    }
                }
                if (determined) {
                    EvalValue r{det_val};
                    TRACE(fold, 0, std::string("short-circuit ")
                          + (is_and ? "&&" : "||") + " -> " + r.to_string());
                    MakeConstructFromConstVal(r, slot, false);
                    return;
                }
                if (drop > 0) {              /* dropped leading no-op consts */
                    const size_t remaining = mo->elems.size() - drop;
                    if (remaining >= 2) {
                        mo->elems.erase(mo->elems.begin(),
                                        mo->elems.begin() + drop);
                        mo->elems[0].first = Op::invalid;
                        TRACE(fold, 0, "drop non-contributing const operand");
                        return;
                    }
                    if (remaining == 1
                            && produces_bool(mo->elems[drop].second.get())) {
                        slot = std::move(mo->elems[drop].second);
                        TRACE(fold, 0, "drop non-contributing const operand");
                        return;
                    }
                    /* one non-bool operand left: bool(x) != x, leave as-is. */
                }
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

        if (auto *inc = dynamic_cast<IncDecExpr *>(c)) {
            /* Fold READS in the inc-dec lvalue (a subscript INDEX, or a
             * subscript/member BASE) but never the mutated target itself -
             * exactly like an assignment lvalue. Without this, `a[i]++` keeps
             * `i` after auto-const drops its write-once decl -> a dangling read
             * ("Expected integer as subscript" from a stale slot). Same bug
             * class as the ReturnStmt / Expr14-lvalue folds. A bare `x++` target
             * is a WRITE, so x is never write-once/promoted - fold_lvalue_reads
             * correctly skips a bare-id lvalue. */
            fold_lvalue_reads(inc->lvalue, fc);
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
            if (callee && ce->args && ce->args->elems.size() == 1
                    && callee->get_str() == "defined"
                    && try_fold_defined(slot, ce)) {
                return;   /* folded; else fall through to normal arg handling */
            }
            if (callee && ce->args && ce->args->elems.size() == 1
                    && callee->get_str() == "isbound"
                    && try_fold_isbound(slot, ce)) {
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
        if (auto *te = dynamic_cast<TernaryExpr *>(c)) {
            fold_reads(te->condExpr, fc);
            fold_reads(te->thenExpr, fc);
            fold_reads(te->elseExpr, fc);
            /* const condition -> the taken branch (the AutoConst analogue of
             * the parser's const-`if` / const-ternary fold: an auto-const flag
             * guard `FLAG ? a : b` collapses, the other branch dropped). */
            if (is_scalar_literal(te->condExpr.get())) {
                const EvalValue v = te->condExpr->eval(&cctx);
                slot = v.get_type()->is_true(v) ? std::move(te->thenExpr)
                                                : std::move(te->elseExpr);
            }
            return;
        }
        if (auto *co = dynamic_cast<CoalesceExpr *>(c)) {
            fold_reads(co->lhs, fc);
            fold_reads(co->rhs, fc);
            /* a const lhs collapses: none -> rhs, otherwise -> lhs */
            if (is_scalar_literal(co->lhs.get())) {
                const EvalValue v = co->lhs->eval(&cctx);
                slot = v.is<NoneVal>() ? std::move(co->rhs)
                                       : std::move(co->lhs);
            }
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
     * Resolve the whole tree. Top-level functions are hoisted to the global
     * table first. Then two passes: pass 1 resolves every function body
     * (collecting the names functions read into `escaped`/`escaped_refs`); pass
     * 2 slots the top-level vars - an escaped one joins the global table, the
     * rest become "main" frame slots. Finally each escaped use site recorded in
     * pass 1 is stamped SymKind::global, and the table's slot->name list is
     * published to the root Block (which sizes the runtime GlobalFuncTable).
     * The root Block also records its frame slot range (slot_count == top-level
     * frame size) for Block::do_eval to build the "main" Frame.
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
                if (v.is<intrusive_ptr<FuncObject>>() &&
                    v.get<intrusive_ptr<FuncObject>>()->func->effective_pure)
                    pure_func_names.insert(kv.first);
            }
        }

        /* Hoist top-level functions to global-table slots (before pass 1, so a
         * forward / mutual ref in a body resolves to its slot). The global
         * function table is separate from the per-call frame, so this has no
         * 64-slot limit. Not in the REPL, where top-level names stay
         * redefinable in the map. The slot->name list is stored on the root
         * block (it sizes the table and lets globals()/reflection list it). */
        if (auto *rb = dynamic_cast<Block *>(root)) {
            if (!repl)
                hoist_global_funcs(rb);
        }

        top_level_only = false;
        walk(root, nullptr);            /* pass 1: functions; fill `escaped` */

        top_level_only = true;          /* pass 2: top level as "main" */
        FuncState main_st;
        main_st.slottable = true;
        main_st.is_main = true;
        walk(root, &main_st);

        /* Now that the top-level pass has given each escaped top-level var its
         * global slot, stamp the function-side use sites recorded in pass 1: a
         * user global wins, else a builtin gets its table slot, else the name
         * stays unresolved (the runtime map -> UndefinedVariableEx). */
        for (const EscapedRef &er : escaped_refs) {
            Identifier *id = er.id;
            auto it = global_func_slots.find(id->uid);
            if (it != global_func_slots.end()) {
                id->sym = ResolvedSym{ SymKind::global, it->second };
                continue;
            }
            stamp_builtin(id);

            /* FIX-1: not a global, not a builtin, and neither this function
             * nor the top level declares the name anywhere - it can never
             * bind. `root_decl_names` is the top-level scope's own pre-scan,
             * kept because that scope is popped by the time we get here. */
            if (!repl_mode && !er.lazy_arg && !id->is_underscore()
                    && !er.guarded                      /* #135 */
                    && id->sym.kind == SymKind::unresolved
                    && !er.declared_later
                    && !root_decl_names.count(id->uid))
                fix1_undefined(id);
        }

        /* Now that escaped globals have their slots, RE-SNAPSHOT every
         * capture list: the in-walk snapshot above ran before this stamp. */
        for (FuncDeclStmt *fd : capture_funcs)
            sync_capture_desc(fd);

        /* Publish the global table's slot->name list to the root block, which
         * sizes the runtime GlobalFuncTable and lets globals() enumerate it;
         * plus the write-once map (reassigned slots) for the native-call gate
         * (#55): 1 == slot reassigned (NOT write-once). */
        if (auto *rb = dynamic_cast<Block *>(root)) {
            rb->global_func_names = global_names;
            rb->global_slot_reassigned.assign(global_names.size(), 0);
            for (int s : reassigned_globals)
                if (s >= 0 && s < static_cast<int>(global_names.size()))
                    rb->global_slot_reassigned[static_cast<size_t>(s)] = 1;
        }

        /*
         * Step 7 tier 2: refuse a call that PROVABLY cannot work. Runs on the
         * CLEAN tree, BEFORE AutoConst/the inliner, on purpose - see
         * prove_unbound_calls for why that is what makes it optimization-
         * invariant (RULE 2).
         */
        if (!repl_mode)
            if (auto *rb = dynamic_cast<Block *>(root)) {
                if (g_strict_mode)      /* --strict: FIX-2's original shape */
                    strict_forward_globals(rb);
                prove_unbound_calls(rb, main_st.writes);
                /* tier 3: the residue it declined */
                warn_unbound_calls(rb, main_st.writes);
            }

        /* Promote write-once scalar vars to constants and fold (uses the write
         * counts just collected; the top-level frame's in main_st.writes).
         * prior_pure seeds the fold context so cross-input pure calls fold. */
        if (auto *rb = dynamic_cast<Block *>(root))
            AutoConst(analysis, prior_pure, repl_mode).run(rb, main_st.writes);
    }

private:

    /*
     * #135: `defined()`-GUARDED NARROWING. A name that exists NOWHERE is a
     * compile error (FIX-1, #130), which leaves no way to feature-test one.
     * Dart's promotion-after-a-test is the model: the SPECIFIC name that was
     * CHECKED is tolerated, and nothing else.
     *
     *     if (defined(x)) { print(x); }           # x tolerated
     *     if (defined(x)) { print(x, y); }        # y is STILL an error
     *     if (defined(x) && defined(y)) { ... }   # both tolerated
     *
     * This is deliberately NOT "code the DCE will delete may say anything" -
     * that would lose FIX-1's typo protection wholesale (maintainer, #135).
     * The code still VANISHES: when `x` exists nowhere, `defined(x)` folds to
     * false, the branch is dead, and the existing DCE drops it. Narrowing's
     * only job is to let the guarded code COMPILE.
     *
     * POLARITY, and it is the whole correctness question: the guard holds for
     * the THEN branch and for the REST of its own `&&` chain (so
     * `if (defined(x) && isbound(x))` works - `isbound` is deliberately not
     * FIX-1-exempt). It does NOT hold for the ELSE branch, and `!defined(x)`
     * establishes nothing.
     */
    void collect_defined_guards(const Construct *cond,
                                std::vector<const UniqueId *> &out)
    {
        if (!cond)
            return;

        /* an `&&` chain: every conjunct contributes. `||` does NOT - the
         * branch can be taken with the other side true, so nothing is proven.
         * A TypedScalarExpr is the M8 form of the same chain. */
        if (auto *e11 = dynamic_cast<const Expr11 *>(cond)) {
            for (const auto &p : e11->elems)
                collect_defined_guards(p.second.get(), out);
            return;
        }
        if (auto *ts = dynamic_cast<const TypedScalarExpr *>(cond)) {
            if (ts->cat != TypedScalarExpr::Cat::logical)
                return;
            for (const auto &p : ts->elems)
                if (p.first == Op::invalid || p.first == Op::land)
                    collect_defined_guards(p.second.get(), out);
            return;
        }

        const auto *call = dynamic_cast<const CallExpr *>(cond);
        if (!call || !call->args || call->args->elems.size() != 1)
            return;
        /* Only `defined`. Broadening this to any 1-arg call is in fact
         * unobservable - every other call EVALUATES its argument, so an
         * undefined name there is refused by FIX-1 before any guard could
         * help - which is why no test can catch that mistake. Named rather
         * than left to chance. */
        const auto *cid = dynamic_cast<const Identifier *>(call->what.get());
        if (!cid)
            return;
        /* `isbound` guards too: since it answers `false` for a name that
         * exists nowhere, `if (isbound(x)) { print(x); }` is THE short
         * feature test - and without narrowing here the `print(x)` inside it
         * would still be refused, so the short form would not work at all. */
        const std::string_view gname = cid->get_str();
        if (gname != "defined" && gname != "isbound")
            return;
        if (const auto *arg =
                dynamic_cast<const Identifier *>(call->args->elems[0].get()))
            out.push_back(arg->uid);
    }

    bool is_guarded(const UniqueId *uid) const
    {
        if (guarded.empty())
            return false;
        for (const UniqueId *g : guarded)
            if (g == uid)
                return true;
        return false;
    }

    /*
     * `--strict` (step 7): EVERY NON-LOCAL MUST BE DECLARED ABOVE ITS FIRST
     * USE. This is FIX-2 in its original shape, kept behind a flag because it
     * is too aggressive to impose on everyone - it refuses programs that are
     * CORRECT today, not merely risky ones:
     *
     *     func total(a) { return a + base; }
     *     func run() { return total(1); }
     *     var base = 100;
     *     print(run());                    # 101 by default, REFUSED here
     *
     * That is the ordinary "helpers at the top, configuration at the bottom"
     * layout, so the cost is real and recurring - which is exactly why it is
     * opt-in. What it buys is that `UnboundSymbolEx` becomes UNREACHABLE: with
     * every global declared before any function that mentions it, no call can
     * find one unbound.
     *
     * FUNCTION and STRUCT names are NOT subject to it. They bind at SCOPE
     * ENTRY (#134), so a forward call is not a forward reference at all -
     * requiring definition order there would forbid mutual recursion and buy
     * nothing.
     *
     * It reuses `escaped_refs`, which already holds every function-body
     * reference to an outer name, so this is a comparison of two source
     * positions and not a second analysis.
     */
    void strict_forward_globals(Block *rb)
    {
        std::unordered_map<const UniqueId *, Loc> decl_at;

        for (auto &e : rb->elems) {
            auto *ex = dynamic_cast<Expr14 *>(e.get());
            if (!ex || !(ex->fl & pFlags::pInDecl))
                continue;
            auto *lv = dynamic_cast<Identifier *>(ex->lvalue.get());
            if (lv && lv->sym.kind == SymKind::global)
                decl_at.emplace(lv->uid, lv->start);
        }

        if (decl_at.empty())
            return;

        for (const EscapedRef &er : escaped_refs) {

            Identifier *id = er.id;

            /* a lazy builtin's argument is a question ABOUT the name, not a
             * use of its value - `isbound(g)` is how a program copes with a
             * name it cannot order above itself */
            if (id->sym.kind != SymKind::global || er.lazy_arg)
                continue;

            auto it = decl_at.find(id->uid);
            if (it == decl_at.end())
                continue;               /* a func/struct name: exempt */

            const Loc &d = it->second;
            if (d.line > id->start.line
                    || (d.line == id->start.line && d.col > id->start.col))
                throw UseBeforeBindingEx(
                    intern_msg("--strict: '" + std::string(id->get_str())
                               + "' is used before its declaration"),
                    id->start, id->end);
        }
    }

    /* ------------------------------------------------------------------
     * STEP 7 tier 2 - THE PROVER: a call that is GUARANTEED to raise
     * UnboundSymbolEx is a COMPILE error, not a runtime one.
     *
     *     func fetch() { return g; }
     *     var dyn t = fetch();          <- refused: `g` cannot be bound yet
     *     var g = 5;
     *
     * and, deliberately, EVEN INSIDE A TRY (maintainer, 2026-08-08) - the two
     * exception kinds exist so this is expressible, `UseBeforeBindingEx` being
     * the uncatchable compile one:
     *
     *     try { var dyn t = fetch(); } catch (UnboundSymbolEx) { ... }
     *     var g = 5;                    <- still refused
     *
     * WHAT MAKES IT SOUND, and it is one idea: only UNCONDITIONALLY evaluated
     * code is considered, on BOTH sides. If the call might not run, or the
     * callee might not reach the read, the failure is not guaranteed and the
     * prover must stay silent - a false "provable" refuses a program that
     * would have worked, which is the one outcome worse than a late error.
     *
     * WHY IT RUNS ON THE CLEAN TREE (before AutoConst / the inliner). Whether
     * a program COMPILES must not depend on which optimizations ran (RULE 2),
     * and both directions of that trap are real here: run it after DCE and
     * `if (false) { fetch(); }` compiles while `-nc` refuses it; run it after
     * the inliner and the callee body may have been spliced into main, so the
     * shape being analysed differs between `-ni` and the default. Restricting
     * to unconditional code removes the question entirely - a const-false
     * branch is CONDITIONAL, so it is skipped either way - and running before
     * every transform means the tree is the one the user wrote.
     *
     * WHAT IT SEES. Transitivity through the call graph
     * (build_reachable_reads), and a callee named indirectly through a
     * WRITE-ONCE binding - a named function, a LAMBDA literal, or a CHAIN
     * of either (index_func_aliases). A callee it cannot name at all -
     * `ops[0]()`, a parameter, an alias declared inside a body - is NOT
     * proven here; it is reported by the weak arm of the warning tier
     * instead, which is allowed to be wrong and says "might".
     * ------------------------------------------------------------------ */

    /*
     * Visit the sub-constructs of `c` that are UNCONDITIONALLY evaluated when
     * `c` is. The exclusions ARE the soundness argument, so they are listed
     * rather than left to a default:
     *   - if/while/for/foreach bodies (and everything under them): may not run
     *   - a catch or finally body: only on the exceptional path
     *   - a ternary arm, a `??` right side, an `&&`/`||` tail: short-circuited
     *   - a function BODY: runs when CALLED, not where it is declared
     * A try BODY is included: it always runs.
     */
    template <typename F>
    static void for_each_unconditional(Construct *c, const F &f)
    {
        if (!c)
            return;

        /* Block and Expr14 are NOT in for_each_child - walk() handles them
         * itself, so a generic descent silently stops at `var t = f();` and
         * at every braced block. Both are unconditional; take them here. */
        if (auto *bl = dynamic_cast<Block *>(c)) {
            for (auto &e : bl->elems)
                f(e.get());
            return;
        }
        if (auto *ex = dynamic_cast<Expr14 *>(c)) {
            f(ex->lvalue.get());
            f(ex->rvalue.get());
            return;
        }
        if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
            f(tc->tryBody.get());
            return;
        }
        /* A LAZY builtin does not EVALUATE its argument - `isbound(g)` and
         * `defined(g)` ask a question ABOUT the name and are exactly what a
         * careful program uses to avoid the error being proven here. Visiting
         * the argument would make every such guard "a read" and refuse the
         * program that got it right. (Caught by the isbound/defined tests.) */
        if (auto *call = dynamic_cast<CallExpr *>(c)) {
            auto *cid = dynamic_cast<Identifier *>(call->what.get());
            if (cid && is_lazy_builtin(cid->uid)) {
                f(call->what.get());
                return;
            }
        }
        if (dynamic_cast<IfStmt *>(c) || dynamic_cast<WhileStmt *>(c)
                || dynamic_cast<ForStmt *>(c) || dynamic_cast<ForeachStmt *>(c)
                || dynamic_cast<FuncDeclStmt *>(c)
                || dynamic_cast<TernaryExpr *>(c)
                || dynamic_cast<CoalesceExpr *>(c)
                || dynamic_cast<Expr11 *>(c) || dynamic_cast<Expr12 *>(c))
            return;

        for_each_child(c, [&](Construct *ch) { f(ch); });
    }

    /* Walk `c` unconditionally, calling `f` on every node reached. */
    template <typename F>
    static void walk_unconditional(Construct *c, const F &f)
    {
        if (!c)
            return;
        f(c);
        for_each_unconditional(c, [&](Construct *ch) {
            walk_unconditional(ch, f);
        });
    }

    /* The GLOBAL VARIABLES `fd` reads on every path through its body. Only
     * names in `global_decl_stmt` count - a func/struct name binds at scope
     * entry (#134), so it is never unbound. */
    void unconditional_global_reads(
        FuncDeclStmt *fd,
        const std::unordered_map<const UniqueId *, size_t> &global_decl_stmt,
        std::vector<Identifier *> &out)
    {
        walk_unconditional(fd->body.get(), [&](Construct *n) {
            auto *id = dynamic_cast<Identifier *>(n);
            if (id && id->sym.kind == SymKind::global
                    && global_decl_stmt.count(id->uid))
                out.push_back(id);
        });
    }

    /*
     * STEP 7 tier 3 - THE WARNING. Where the prover above found the same
     * situation but could NOT prove the failure, say so instead of staying
     * silent. GCC's spirit ("this variable might be uninitialized"): the
     * program is not refused, but the reader is told.
     *
     *     func fetch() { return g; }
     *     if (ready()) { var dyn t = fetch(); }   <- warned: may not be bound
     *     var g = 5;
     *
     * The complement of the error tier by construction: prove_unbound_calls
     * THROWS on everything it can prove, so anything reaching here is exactly
     * the residue - a conditional call, a conditional read, or a call site
     * below the top level. That is why this walks EVERYTHING (plain
     * for_each_child plus the containers it skips) while the prover walks
     * only unconditional code, and why the two cannot double-report.
     *
     * It is TRANSITIVE like the error tier (the same build_reachable_reads,
     * asked for every path rather than the guaranteed ones) and it resolves
     * the same write-once aliases, so the two tiers see one call graph and
     * differ only in which paths count.
     *
     * IT HAS TWO STRENGTHS, and the wording is the contract:
     *
     *   "this call MAY fail: g is not bound until later"
     *        the callee is KNOWN and provably reaches g on some path; only
     *        WHETHER that path runs is open
     *
     *   "this call MIGHT fail: the callee is not known here, and g is not
     *    bound until later"
     *        the callee is NOT known - a container element, a parameter, a
     *        reassigned name. This one is ALLOWED to be a false positive
     *        (maintainer, 2026-08-09), which is what "might" announces.
     *
     * The weak arm is filtered so it is not merely noise: it fires only for
     * a global that SOME function actually reads, since an opaque callee
     * cannot fail on a global nobody touches. Measured over samples/ +
     * bench/ + tests/functional/: ZERO warnings. The filter is what earns
     * that - 76_funcval_dispatch has exactly this call shape and is quiet
     * because its globals are declared above it, and moving one below makes
     * it warn.
     */
    template <typename F>
    static void walk_every(Construct *c, const F &f)
    {
        if (!c)
            return;
        f(c);
        if (auto *bl = dynamic_cast<Block *>(c)) {
            for (auto &e : bl->elems)
                walk_every(e.get(), f);
            return;
        }
        if (auto *ex = dynamic_cast<Expr14 *>(c)) {
            walk_every(ex->lvalue.get(), f);
            walk_every(ex->rvalue.get(), f);
            return;
        }
        if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {
            walk_every(tc->tryBody.get(), f);
            for (auto &p : tc->catchStmts)
                walk_every(p.second.get(), f);
            walk_every(tc->finallyBody.get(), f);
            return;
        }
        if (auto *fo = dynamic_cast<ForStmt *>(c)) {
            walk_every(fo->init.get(), f);
            walk_every(fo->cond.get(), f);
            walk_every(fo->inc.get(), f);
            walk_every(fo->body.get(), f);
            return;
        }
        if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
            walk_every(fe->container.get(), f);
            walk_every(fe->body.get(), f);
            return;
        }
        for_each_child(c, [&](Construct *ch) { walk_every(ch, f); });
    }

    void warn_unbound_calls(Block *rb, const std::vector<int> &main_writes)
    {
        DeclIndex global_decl_stmt;
        FuncIndex top_funcs;
        NameSet structs;
        index_top_level(rb, global_decl_stmt, top_funcs, structs);

        if (global_decl_stmt.empty())
            return;

        AliasIndex aliases;
        index_func_aliases(rb, top_funcs, main_writes, aliases);

        /* a LAMBDA bound to a name is a function this program can call even
         * with no NAMED function anywhere, so the cheap bail must ask about
         * both indexes - asking only about top_funcs skipped s1 entirely */
        if (top_funcs.empty() && aliases.empty())
            return;

        /* every global a call may reach, on ANY path and through ANY number
         * of hops - the conditional read and the deeper one are what this
         * tier exists to catch - plus which functions reach a callee this
         * pass cannot name, for the weaker "might" arm below */
        ReadSets reads;
        FuncSet opaque;
        build_reachable_reads(top_funcs, global_decl_stmt, aliases, structs,
                              /*unconditional_only=*/false, reads, &opaque);

        /*
         * THE "MIGHT" ARM's candidate set. An un-nameable callee could run
         * anything, so the honest statement is about any global that SOME
         * function reads - if no function reads `g` at all, no call can fail
         * on it, however opaque the callee. Ordered by declaration so the
         * message names the FIRST offender and is not at the mercy of an
         * unordered_map's iteration order.
         */
        std::vector<std::pair<size_t, const UniqueId *>> read_anywhere;
        for (const auto &rp : reads)
            for (const UniqueId *g : rp.second) {
                auto dit = global_decl_stmt.find(g);
                bool seen = false;
                for (const auto &e : read_anywhere)
                    if (e.second == g) { seen = true; break; }
                if (!seen && dit != global_decl_stmt.end())
                    read_anywhere.emplace_back(dit->second, g);
            }
        std::sort(read_anywhere.begin(), read_anywhere.end());

        const auto warn_might = [&](CallExpr *call, size_t i) {
            for (const auto &e : read_anywhere)
                if (e.first > i) {
                    compile_warn(
                        intern_msg("this call might fail: the callee is not "
                                   "known here, and " + e.second->val
                                   + " is not bound until later"),
                        call->start, call->end);
                    return;
                }
        };

        for (size_t i = 0; i < rb->elems.size(); i++) {

            if (dynamic_cast<FuncDeclStmt *>(rb->elems[i].get()))
                continue;

            walk_every(rb->elems[i].get(), [&](Construct *n) {
                auto *call = dynamic_cast<CallExpr *>(n);
                if (!call)
                    return;
                FuncDeclStmt *fd = callee_of(call, top_funcs, aliases,
                                             static_cast<ptrdiff_t>(i));
                if (!fd) {
                    /* `ops[0]()`, a parameter, a body-local alias: we cannot
                     * name what runs, so this is the weaker statement */
                    if (call_is_opaque(call, top_funcs, aliases, structs))
                        warn_might(call, i);
                    return;
                }

                for (const UniqueId *g : reads[fd]) {
                    auto dit = global_decl_stmt.find(g);
                    if (dit != global_decl_stmt.end() && dit->second > i) {
                        compile_warn(
                            intern_msg("this call may fail: " + g->val
                                       + " is not bound until later"),
                            call->start, call->end);
                        return;     /* one per call site, not per read */
                    }
                }
                /* the callee IS known, but something it reaches is not - so
                 * the definite statement above could not be made */
                if (opaque.count(fd))
                    warn_might(call, i);
            });
        }
    }

    typedef std::unordered_map<const UniqueId *, size_t> DeclIndex;
    typedef std::unordered_map<const UniqueId *, FuncDeclStmt *> FuncIndex;
    typedef std::unordered_map<FuncDeclStmt *,
                               std::vector<const UniqueId *>> ReadSets;
    /* name -> (the function it always holds, the stmt that bound it) */
    struct Alias { FuncDeclStmt *fd; size_t decl_stmt; };
    typedef std::unordered_map<const UniqueId *, Alias> AliasIndex;

    typedef std::unordered_set<const UniqueId *> NameSet;

    /* The top-level statement index of each global VAR/CONST decl, plus every
     * top-level named function - the two things both tiers index by - plus
     * the STRUCT names, which the "might" warning needs only to stay quiet:
     * `P(1)` is a call whose callee is not a function, and calling it
     * "a callee we cannot analyse" would warn about every construction. */
    static void index_top_level(Block *rb, DeclIndex &decls, FuncIndex &funcs,
                                NameSet &structs)
    {
        for (size_t i = 0; i < rb->elems.size(); i++) {
            Construct *e = rb->elems[i].get();
            if (auto *fd = dynamic_cast<FuncDeclStmt *>(e)) {
                if (fd->id)
                    funcs[fd->id->uid] = fd;
                continue;
            }
            if (auto *sd = dynamic_cast<StructDeclStmt *>(e)) {
                if (sd->id)
                    structs.insert(sd->id->uid);
                continue;
            }
            auto *ex = dynamic_cast<Expr14 *>(e);
            if (!ex || !(ex->fl & pFlags::pInDecl))
                continue;
            auto *lv = dynamic_cast<Identifier *>(ex->lvalue.get());
            if (lv && lv->sym.kind == SymKind::global)
                decls.emplace(lv->uid, i);
        }
    }

    /*
     * WRITE-ONCE NAMES THAT ALWAYS HOLD ONE NAMED FUNCTION (#140).
     *
     *     func fetch() { return g; }
     *     var f = fetch;               <- f can only ever be `fetch`
     *     var t = f();                 <- so this call always fails
     *     var g = 5;
     *
     * Without this the callee is not an Identifier the FuncIndex knows, both
     * tiers give up, and the program dies at RUN time on line 1 - even though
     * the failure is exactly as certain as the direct `fetch()` spelling,
     * which is refused at compile time.
     *
     * WHAT MAKES IT SOUND is write-once, the same argument AutoConst's
     * promotion rests on: the declaration is the ONLY write, so at every
     * point where the name is bound at all it holds that one function. A
     * reassignment anywhere - in any scope, at any depth - disqualifies it,
     * and the two write counters the resolver has ALREADY collected by now
     * are what proves that: `writes[slot] == 1` for a main-frame local (the
     * decl itself is write #1) and absence from `reassigned_globals` for a
     * global (a decl never reaches count_write, so any entry is a reassign).
     *
     * A capture cannot defeat it: captures are BY VALUE, so a closure that
     * reassigns its copy leaves this binding alone - and that write is a
     * capture-slot write, counted against neither table.
     *
     * NOT chased through a container (`ops[0]()`) or a parameter: those
     * need a real callee-SET analysis, and what the resolver can see here
     * is one value. The limit costs a diagnostic, never an answer - and
     * those shapes reach the warning tier's weak arm anyway.
     */
    void index_func_aliases(Block *rb, const FuncIndex &funcs,
                            const std::vector<int> &main_writes,
                            AliasIndex &out)
    {
        for (size_t i = 0; i < rb->elems.size(); i++) {

            auto *ex = dynamic_cast<Expr14 *>(rb->elems[i].get());
            if (!ex || !(ex->fl & pFlags::pInDecl) || ex->op != Op::assign)
                continue;

            /* `var a, b = ...` binds a list: no single function value */
            auto *lv = dynamic_cast<Identifier *>(ex->lvalue.get());
            if (!lv)
                continue;

            /*
             * THREE rvalue shapes name one function. A LAMBDA LITERAL is the
             * function - there is no name to look up, and no way for the
             * binding to hold anything else. A named top-level function is
             * the base case. An earlier ALIAS chains: because this loop runs
             * in statement ORDER, `var f2 = f;` finds f's entry already
             * built, and finds it only when f was bound ABOVE - which is
             * exactly the condition for f2 to hold f's function at all.
             */
            FuncDeclStmt *target = nullptr;
            if (auto *lam = dynamic_cast<FuncDeclStmt *>(ex->rvalue.get())) {
                target = lam;
            } else if (auto *rv =
                           dynamic_cast<Identifier *>(ex->rvalue.get())) {
                if (rv->sym.kind == SymKind::global) {
                    auto ft = funcs.find(rv->uid);
                    if (ft != funcs.end())
                        target = ft->second;
                }
                if (!target) {
                    auto at = out.find(rv->uid);
                    if (at != out.end())
                        target = at->second.fd;
                }
            }
            if (!target)
                continue;

            if (lv->sym.kind == SymKind::local) {
                const int s = lv->sym.slot;
                if (s < 0 || s >= static_cast<int>(main_writes.size())
                        || main_writes[static_cast<size_t>(s)] != 1)
                    continue;           /* reassigned somewhere */
            } else if (lv->sym.kind == SymKind::global) {
                if (reassigned_globals.count(lv->sym.slot))
                    continue;
            } else {
                continue;               /* not a plain top-level binding */
            }

            out.emplace(lv->uid, Alias{ target, i });
        }
    }

    /*
     * The function a call site invokes, or null when that is not decidable.
     * `stmt` is the top-level statement index the call sits in, or -1 when it
     * sits inside a function BODY (which may run at any time).
     */
    static FuncDeclStmt *callee_of(CallExpr *call, const FuncIndex &funcs,
                                   const AliasIndex &aliases,
                                   ptrdiff_t stmt)
    {
        auto *cid = dynamic_cast<Identifier *>(call->what.get());
        if (!cid)
            return nullptr;             /* `ops[0]()`, a call result, ... */

        auto ft = funcs.find(cid->uid);
        if (ft != funcs.end())
            return ft->second;

        auto at = aliases.find(cid->uid);
        if (at == aliases.end())
            return nullptr;
        /*
         * A top-level call ABOVE the binding reaches an unbound name, so the
         * failure is that name, not anything the function it will later hold
         * reads - staying silent leaves the (correct) runtime error rather
         * than reporting the wrong cause. From inside a BODY there is nothing
         * to compare against, and nothing to check either: a body can only
         * read a GLOBAL, which is in `global_decl_stmt`, so an alias that is
         * not bound yet is already caught as an ordinary unbound read.
         */
        if (stmt >= 0 && at->second.decl_stmt > static_cast<size_t>(stmt))
            return nullptr;
        return at->second.fd;
    }

    /*
     * OPAQUE: this call reaches MyLang code we cannot name. Not the same as
     * "callee_of said no" - most of those are perfectly analysable things
     * that simply are not user functions, and warning about them would bury
     * the real diagnostic:
     *
     *     print(x);        a BUILTIN - C++, reads no MyLang global
     *     P(1);            a STRUCT construction - binds fields, calls nothing
     *     ops[0]();        opaque: an element, and we do not track the set
     *     fn();            opaque: a parameter, bound by whoever called us
     *
     * A block-scoped nested function counts as opaque too: it has a global
     * SLOT but no entry in `global_func_slots`, so this cannot name its body.
     * That is a false positive in the honest sense - the callee really is
     * one this pass cannot analyse - and it is why the diagnostic it feeds
     * says "might".
     */
    static bool call_is_opaque(CallExpr *call, const FuncIndex &funcs,
                               const AliasIndex &aliases,
                               const NameSet &structs)
    {
        auto *cid = dynamic_cast<Identifier *>(call->what.get());
        if (!cid)
            return true;                /* a subscript / member / call result */
        if (cid->sym.kind == SymKind::builtin)
            return false;
        if (structs.count(cid->uid))
            return false;
        return !funcs.count(cid->uid) && !aliases.count(cid->uid);
    }

    static void add_uniq(std::vector<const UniqueId *> &v, const UniqueId *u)
    {
        for (const UniqueId *x : v)
            if (x == u)
                return;
        v.push_back(u);
    }

    /*
     * WHICH GLOBALS A CALL CAN REACH - a fixpoint over the CALL GRAPH, not a
     * one-level look at the callee's own body.
     *
     *     func fetch() { return g; }
     *     func outer() { var r = fetch(); return r; }
     *     var dyn t = outer();          <- `outer` does not read `g`...
     *     var g = 5;                    <- ...but what it CALLS does
     *
     * Without this the two-hop program compiled and died at run time while
     * the one-hop one was refused, though the failure is equally certain.
     * `reads(F) = own(F) union reads(h) for every h that F calls`, iterated
     * to a fixpoint - a fixpoint and not a walk because MUTUAL RECURSION
     * makes the graph cyclic, and a cycle has no traversal order.
     *
     * `unconditional_only` picks which walker sees the body, and that ONE
     * switch keeps the error tier sound: with it, both the reads and the
     * CALLS are the unconditional ones, so a global enters F's set only if
     * every step from F to the read is guaranteed. `outer` conditionally
     * calling `fetch` puts `fetch` in no set of outer's; `fetch` reading `g`
     * conditionally puts `g` in no set of fetch's. The warning tier passes
     * false and takes every path.
     */
    typedef std::unordered_set<FuncDeclStmt *> FuncSet;

    void build_reachable_reads(const FuncIndex &funcs, const DeclIndex &decls,
                               const AliasIndex &aliases,
                               const NameSet &structs,
                               bool unconditional_only, ReadSets &out,
                               FuncSet *opaque = nullptr)
    {
        std::unordered_map<FuncDeclStmt *, std::vector<FuncDeclStmt *>> callees;

        /* every body a call can land in: the named functions, plus the
         * LAMBDA an alias binds - which has no name, so the FuncIndex does
         * not know it and iterating that map alone would leave its reads
         * empty (and the alias pointing at an empty set, silently) */
        std::vector<FuncDeclStmt *> bodies;
        for (const auto &fp : funcs)
            bodies.push_back(fp.second);
        for (const auto &ap : aliases) {
            bool seen = false;
            for (FuncDeclStmt *b : bodies)
                if (b == ap.second.fd) { seen = true; break; }
            if (!seen)
                bodies.push_back(ap.second.fd);
        }

        for (FuncDeclStmt *fd : bodies) {

            std::vector<const UniqueId *> reads;
            std::vector<FuncDeclStmt *> calls;

            const auto visit = [&](Construct *n) {
                if (auto *id = dynamic_cast<Identifier *>(n))
                    if (id->sym.kind == SymKind::global && decls.count(id->uid))
                        add_uniq(reads, id->uid);
                auto *call = dynamic_cast<CallExpr *>(n);
                if (!call)
                    return;
                FuncDeclStmt *h = callee_of(call, funcs, aliases, -1);
                if (!h) {
                    if (opaque && call_is_opaque(call, funcs, aliases,
                                                 structs))
                        opaque->insert(fd);
                    return;
                }
                if (h != fd) {
                    bool seen = false;
                    for (FuncDeclStmt *k : calls)
                        if (k == h) { seen = true; break; }
                    if (!seen)
                        calls.push_back(h);
                }
            };

            if (unconditional_only)
                walk_unconditional(fd->body.get(), visit);
            else
                walk_every(fd->body.get(), visit);

            out[fd] = std::move(reads);
            callees[fd] = std::move(calls);
        }

        /* propagate along the call edges until nothing new arrives - the
         * reads, and (for the warning tier) the OPAQUE bit: a function that
         * calls one which reaches an un-nameable callee reaches it too */
        for (bool changed = true; changed; ) {
            changed = false;
            for (const auto &cp : callees)
                for (FuncDeclStmt *h : cp.second) {
                    for (const UniqueId *u : out[h]) {
                        const size_t before = out[cp.first].size();
                        add_uniq(out[cp.first], u);
                        if (out[cp.first].size() != before)
                            changed = true;
                    }
                    if (opaque && opaque->count(h)
                            && !opaque->count(cp.first)) {
                        opaque->insert(cp.first);
                        changed = true;
                    }
                }
        }
    }

    void prove_unbound_calls(Block *rb, const std::vector<int> &main_writes)
    {
        DeclIndex global_decl_stmt;
        FuncIndex top_funcs;
        NameSet structs;
        index_top_level(rb, global_decl_stmt, top_funcs, structs);

        if (global_decl_stmt.empty())
            return;

        AliasIndex aliases;
        index_func_aliases(rb, top_funcs, main_writes, aliases);

        /* a LAMBDA bound to a name is a function this program can call even
         * with no NAMED function anywhere, so the cheap bail must ask about
         * both indexes - asking only about top_funcs skipped s1 entirely */
        if (top_funcs.empty() && aliases.empty())
            return;

        /* what a call can REACH, not just what its callee's own body reads */
        ReadSets reads;
        build_reachable_reads(top_funcs, global_decl_stmt, aliases, structs,
                              /*unconditional_only=*/true, reads);

        for (size_t i = 0; i < rb->elems.size(); i++) {

            if (dynamic_cast<FuncDeclStmt *>(rb->elems[i].get()))
                continue;               /* a declaration calls nothing */

            walk_unconditional(rb->elems[i].get(), [&](Construct *n) {
                auto *call = dynamic_cast<CallExpr *>(n);
                if (!call)
                    return;
                FuncDeclStmt *fd = callee_of(call, top_funcs, aliases,
                                             static_cast<ptrdiff_t>(i));
                if (!fd)
                    return;

                for (const UniqueId *g : reads[fd]) {
                    auto dit = global_decl_stmt.find(g);
                    if (dit != global_decl_stmt.end() && dit->second > i)
                        /* the caret marks the CALL: that is the statement the
                         * user must move, and the read - which may be several
                         * hops down - is correct code for every other
                         * caller */
                        throw UseBeforeBindingEx(
                            intern_msg(
                                "this call always fails: " + g->val
                                + " is not bound until later"),
                            call->start, call->end);
                }
            });
        }
    }

    /* Names a function reads from outer scope (so they are GLOBALs, not the
     * function's locals): a top-level VARIABLE among them gets a global-table
     * slot (so the function reads it as an O(1) slot, not a map walk). */
    std::unordered_set<const UniqueId *> escaped;
    /* GLOBAL-table slots ever REASSIGNED (`f = g` after the decl) - i.e. NOT
     * write-once. Filled by count_write; published to the root block's
     * global_slot_reassigned for the native-call write-once gate (#55). */
    std::set<int> reassigned_globals;
    /* The function-side USE sites of those escaped names (recorded while a
     * function body is resolved in pass 1, before the top-level pass has
     * assigned global slots). After pass 2, each is stamped SymKind::global if
     * its name turned out to be a top-level var (else left for the map: a
     * builtin / genuinely-undefined name). */
    /* FIX-1: every name the OUTERMOST top-level scope declares (its
     * collect_scope_names pre-scan). Kept as a member because that scope is
     * popped by the time the escaped_refs post-pass runs, and a function
     * body's free name may legitimately refer to a global declared BELOW it. */
    std::unordered_set<const UniqueId *> root_decl_names;

    /* Every capturing FuncDeclStmt, so its descriptor's capture snapshot can
     * be re-taken after pass 2 has stamped the escaped globals. */
    std::vector<FuncDeclStmt *> capture_funcs;

    /* Copy the RESOLVED capture sources into the descriptor (kind/slot), so
     * the FuncObject ctor never reads a capture Identifier. */
    static void sync_capture_desc(FuncDeclStmt *fd)
    {
        fd->desc->captures.clear();
        fd->desc->captures.reserve(fd->captures->elems.size());
        for (auto &cap : fd->captures->elems)
            fd->desc->captures.push_back(
                { cap->uid, cap->sym.kind, cap->sym.slot,
                  cap->start, cap->end });
    }

    struct EscapedRef {
        Identifier *id;
        bool declared_later;    /* the function itself declares this name
                                 * below the use - a forward LOCAL ref, so
                                 * FIX-1 must not call it undefined */
        bool lazy_arg;          /* the use is a LAZY builtin's argument
                                 * (defined/isconst/isconstdecl): a question
                                 * about the name, never a read */
        bool guarded;           /* #135: a `defined()` guard vouched for the
                                 * name at this use - recorded HERE because
                                 * the FIX-1 check for a function-body
                                 * reference runs after the walk, when the
                                 * guard stack is long gone */
    };

    /* Set while walking a lazy builtin's arguments - see the CallExpr walk. */
    bool no_undef_check = false;
    /*
     * Step 6: the TDZ suppression is a SEPARATE question from the FIX-1 one.
     * BOTH lazy builtins must be allowed to ask about a name inside its TDZ
     * (`isbound(x)` before `var x` is the whole point, and must answer
     * false) - but only `defined` may ask about a name declared NOWHERE.
     * `isbound(zz)` on such a name is a FIX-1 compile error like any other
     * use, because there is nothing for it to be bound to, ever.
     */
    bool no_tdz_check = false;
    /* #135: names a `defined()` guard has vouched for at this point - see
     * collect_defined_guards. A scoped stack, like `shadowed` in the parser;
     * empty in every program that does not feature-test, so the lookup is one
     * compare. */
    std::vector<const UniqueId *> guarded;
    std::vector<EscapedRef> escaped_refs;
    /*
     * The GLOBAL table: every top-level function (hoisted up front so a forward
     * / mutually-recursive reference resolves) AND every top-level variable a
     * function reads (added as its decl is walked). `global_func_slots` is
     * uid->slot; `global_names` is slot->uid (sizes the runtime table + lets
     * reflection enumerate it). A reference resolves to SymKind::global - an
     * O(1) table read, not a map walk. Empty in REPL mode (top-level names
     * stay redefinable in the map). */
    std::unordered_map<const UniqueId *, int> global_func_slots;
    std::vector<const UniqueId *> global_names;

    /* Append a name to the global table, returning its slot. */
    int add_global_slot(const UniqueId *uid)
    {
        const int slot = static_cast<int>(global_names.size());
        global_func_slots[uid] = slot;
        global_names.push_back(uid);
        return slot;
    }

    /*
     * Append a SCOPED global slot: a non-capturing nested/conditional function
     * or struct gets a real global-table slot (so its decl binds a slot, not the
     * map, and a reference is an O(1) read) but is NOT entered into
     * global_func_slots - so it is reachable ONLY through the lexical scope it is
     * registered in (block-scoped, C-style). Two same-named nested decls in
     * different scopes therefore get distinct slots and never collide.
     */
    int add_anon_global_slot(const UniqueId *uid)
    {
        const int slot = static_cast<int>(global_names.size());
        global_names.push_back(uid);
        return slot;
    }
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
     * Assign a STATIC GLOBAL-table slot to each DIRECT top-level function
     * declaration, so a reference to it resolves to SymKind::global - an O(1)
     * table read, not a map walk. Slots are 0..n-1, recorded by name on the
     * root block (`global_func_names`, which sizes the runtime table and lets
     * reflection enumerate it). The table is a plain vector with NO 64-slot
     * limit (it is not a per-call Frame), so every top-level function gets a
     * slot however many there are. A duplicate top-level function name is a
     * compile error. (Only DIRECT top-level decls: a function nested in an
     * `if`/block at top level is conditionally defined and stays map-based.)
     */
    void hoist_global_funcs(Block *rb)
    {
        for (auto &e : rb->elems) {
            /* A top-level function OR struct: both are non-capturing named
             * declarations visible from any function body, so both get a global
             * table slot (a struct's name binds its type descriptor, like a
             * func name binds its FuncObject). */
            Identifier *id = nullptr;
            if (auto *fd = dynamic_cast<FuncDeclStmt *>(e.get()))
                id = fd->id.get();
            else if (auto *sd = dynamic_cast<StructDeclStmt *>(e.get()))
                id = sd->id.get();
            if (!id)
                continue;
            if (global_func_slots.count(id->uid))
                throw AlreadyDefinedEx(id->start, id->end);
            id->sym = ResolvedSym{ SymKind::global, add_global_slot(id->uid) };
        }
    }

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

        /* An OUTERMOST top-level var clashing with a global-table name (a
         * hoisted function or an earlier escaped var, neither in main's scopes
         * for check_no_redecl to see) is a duplicate declaration. Only at the
         * outermost scope: a nested-block var legitimately shadows a global. */
        if (cur->is_main && cur->scopes.size() == 1 &&
            global_func_slots.count(id->uid))
            throw AlreadyDefinedEx(id->start, id->end);

        /*
         * A top-level variable that some function reads (escaped) goes in the
         * GLOBAL table (SymKind::global), alongside functions, so the function
         * reaches it as an O(1) slot - not a map walk. Its decl binds the table
         * slot at runtime (handle_single_expr14). main's own reads find it here
         * in the root scope (kind=global). NOT auto-const-promoted (it is not a
         * main-frame slot, so the folder never sees it - same as the old map
         * binding). In REPL mode every top-level decl stays in the map (a
         * persistent, redefinable global).
         *
         * Only the OUTERMOST main scope (scopes.size()==1) qualifies: a func is
         * parented to the root context, so it can read only an outermost top-
         * level var, never a main nested-block local that happens to share the
         * name. A shadowing nested decl falls through to a normal local slot.
         */
        if (cur->is_main && !repl_mode && cur->scopes.size() == 1 &&
            escaped.count(id->uid))
        {
            const int gslot = add_global_slot(id->uid);
            cur->scopes.back().decls.push_back(
                { id->uid, gslot, SymKind::global, id->decl_type });
            id->sym = ResolvedSym{ SymKind::global, gslot };
            return;
        }

        /* REPL: keep top-level decls in the map (redefinable across inputs). A
         * slot-budget overflow also falls back to a masking map entry. */
        if ((cur->is_main && repl_mode) || cur->next_slot >= MAX_SLOTS) {
            cur->scopes.back().decls.push_back(
                { id->uid, -1, SymKind::unresolved, id->decl_type });
            return;
        }

        const int slot = cur->next_slot++;
        cur->scopes.back().decls.push_back(
            { id->uid, slot, SymKind::local, id->decl_type });
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
        cur->scopes.back().decls.push_back(
            { id->uid, -1, SymKind::unresolved, id->decl_type });
    }

    /*
     * Hoist the non-capturing named FUNCTION and STRUCT declarations directly in
     * block `b` into the CURRENT lexical scope as scoped global slots, BEFORE the
     * block's statements are walked - so a forward / mutually-recursive reference
     * within the block resolves to the slot. They are block-scoped: registered in
     * this scope only (popped with the block), so a nested func/struct is not
     * visible after its block, and same-named decls in sibling scopes get
     * distinct slots. A CAPTURING named function is skipped (it closes over
     * locals, so it is not a shared global - it stays the enclosing scope's local
     * via declare_masking, as before). Top-level decls (already SymKind::global
     * from hoist_global_funcs) and REPL mode (open-world map) are skipped.
     */
    void hoist_scoped_decls(FuncState *cur, Block *b)
    {
        if (!cur || !cur->slottable || repl_mode)
            return;

        for (auto &e : b->elems) {
            Identifier *id = nullptr;

            if (auto *fd = dynamic_cast<FuncDeclStmt *>(e.get())) {
                /* Non-capturing only; a captured-list func is not a shared
                 * global. Skip a name already hoisted (top-level global). */
                if (fd->id && fd->id->sym.kind != SymKind::global &&
                    (!fd->captures || fd->captures->elems.empty()))
                    id = fd->id.get();
            } else if (auto *sd = dynamic_cast<StructDeclStmt *>(e.get())) {
                if (sd->id && sd->id->sym.kind != SymKind::global)
                    id = sd->id.get();
            }

            if (!id)
                continue;

            check_no_redecl(cur, id);   /* a same-block duplicate is an error */
            const int slot = add_anon_global_slot(id->uid);
            cur->scopes.back().decls.push_back(
                { id->uid, slot, SymKind::global, id->decl_type });
            id->sym = ResolvedSym{ SymKind::global, slot };
        }
    }

    /* Record every name an lvalue declares into `s` (see collect_scope_names).
     * `_` IS recorded: it is never bound, but a reference to it must report
     * "undefined", not "declared nowhere", and the two messages differ. */
    static void collect_lvalue_names(Construct *lvalue, Scope &s)
    {
        /* `_` is recorded in all_names (so a reference to it reports
         * "undefined", not "declared nowhere") but NEVER in var_names: it is
         * never bound, so it has no temporal dead zone - reading it must stay
         * UndefinedVariableEx, not UseBeforeBindingEx. */
        if (auto *id = dynamic_cast<Identifier *>(lvalue)) {
            s.all_names.push_back(id->uid);
            if (!id->is_underscore())
                s.var_names.push_back(id->uid);
        } else if (auto *il = dynamic_cast<IdList *>(lvalue)) {
            for (auto &id : il->elems) {
                s.all_names.push_back(id->uid);
                if (!id->is_underscore())
                    s.var_names.push_back(id->uid);
            }
        }
    }

    /*
     * Pre-scan a block's DIRECT statements for the names they declare, into
     * the freshly-pushed scope. Mirrors the parser's pStmtDeclaresName: only
     * a func decl, a struct decl and a var/const decl (an `Expr14` carrying
     * pInDecl) can introduce a name into the scope that contains them - a
     * nested if/loop/try declares only inside its OWN body, which gets its
     * own scope and its own pre-scan.
     *
     * ADD TO THIS if a new declaring statement form appears: a missing kind
     * makes FIX-1 report "declared nowhere" for a name that IS declared,
     * i.e. it refuses a legal program.
     */
    static void collect_scope_names(Block *b, Scope &s)
    {
        for (auto &e : b->elems) {
            Construct *c = e.get();

            if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
                if (fd->id)
                    s.all_names.push_back(fd->id->uid);
            } else if (auto *sd = dynamic_cast<StructDeclStmt *>(c)) {
                if (sd->id)
                    s.all_names.push_back(sd->id->uid);
            } else if (auto *ex = dynamic_cast<Expr14 *>(c)) {
                if (ex->fl & pFlags::pInDecl)
                    collect_lvalue_names(ex->lvalue.get(), s);
            }
        }
    }

    /* Is `uid` declared ANYWHERE in a live scope of `cur` - including below
     * the current walk position? The FIX-1 "declared nowhere" test. */
    /* Is `uid` a VAR/CONST declared below the walk position in a live scope
     * - i.e. currently in its temporal dead zone? (A func/struct is never:
     * see Scope::var_names.) */
    static bool in_tdz_of_live_scope(const FuncState *cur,
                                     const UniqueId *uid)
    {
        if (!cur)
            return false;

        for (const auto &s : cur->scopes)
            for (const UniqueId *n : s.var_names)
                if (n == uid)
                    return true;

        return false;
    }

    static bool declared_in_live_scopes(const FuncState *cur,
                                        const UniqueId *uid)
    {
        if (!cur)
            return false;

        for (const auto &s : cur->scopes) {
            for (const UniqueId *n : s.all_names)
                if (n == uid)
                    return true;
            /* A name already DECLARED (walked past) is in decls, not
             * all_names, when the scope was pushed without a block
             * pre-scan (a for/foreach/catch scope). */
            for (const auto &d : s.decls)
                if (d.name == uid)
                    return true;
        }

        return false;
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
     * also leaves it unresolved (builtin / capture). A non-captured free name
     * in a function is recorded in `escaped` (so the top-level pass gives that
     * var a global-table slot) and in `escaped_refs` (so this use site is
     * stamped SymKind::global once that slot exists). A name that never gets a
     * slot (a builtin) stays unresolved -> map.
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
                            id->sym = ResolvedSym{ d.kind, d.slot };
                        /* Carry the declared type to the use so an assignment
                         * can coerce a widening value (float f; f = 3). */
                        id->decl_type = d.type;
                        return;     /* found (slotted or masked) */
                    }
                }
            }
        }

        /*
         * Not a local/param/capture of this function (captures resolve in the
         * scope loop above, so they already took precedence over a same-named
         * global). A top-level function (hoisted before pass 1) resolves to its
         * GLOBAL slot (reached via the global table from any call depth), not a
         * map walk. Escaped vars aren't in the table yet during pass 1 - they
         * are stamped post-pass-2 via escaped_refs.
         */
        /*
         * TDZ (#131): the scope loop above searches DECLARED-SO-FAR names;
         * `all_names` additionally holds the ones declared BELOW. A hit here
         * means the name IS in scope but its declaration has not run - and
         * because the reader is inside the scope itself, that is decidable,
         * so it is a COMPILE error. The lazy builtins are the exception:
         * they ask ABOUT the name rather than reading it, and `defined()`
         * must answer TRUE (the name is declared - it is merely unbound).
         */
        if (cur->slottable && in_tdz_of_live_scope(cur, id->uid)
                && global_func_slots.find(id->uid) == global_func_slots.end()) {
            if (!no_tdz_check) {
                if (!repl_mode)
                    throw UseBeforeBindingEx(
                        intern_msg("'" + std::string(id->get_str())
                                   + "' is used before its declaration"),
                        id->start, id->end);
            } else {
                id->in_tdz = true;      /* declared, not yet bound */
            }
            return;
        }

        auto git = global_func_slots.find(id->uid);
        if (git != global_func_slots.end()) {
            id->sym = ResolvedSym{ SymKind::global, git->second };
            return;
        }

        if (!cur->is_main) {
            /*
             * A function-body free name. It is EITHER an escaped top-level var
             * (gets a global slot in pass 2) OR a builtin - but we can't tell
             * yet (vars aren't hoisted), and a user var must win over a
             * same-named builtin. So defer BOTH: record the use site and stamp
             * it (global, else builtin, else leave for the map) after pass 2.
             */
            escaped.insert(id->uid);
            escaped_refs.push_back(
                EscapedRef{ id, declared_in_live_scopes(cur, id->uid),
                            no_undef_check, is_guarded(id->uid) });
            return;
        }

        /*
         * A top-level (main) free name. A user var/func of this name would have
         * been found in the scope loop / global table above, so reaching here
         * means it is a builtin (resolve to its table slot) or genuinely
         * undefined (left unresolved -> the runtime map -> an error).
         * Not in the REPL, where builtins stay map-resident (redefinable).
         */
        stamp_builtin(id);

        /* FIX-1: still unresolved, and no enclosing scope declares this name
         * ANYWHERE (below included) - so it can never bind. Refuse now
         * instead of failing at run time. */
        if (!repl_mode && !no_undef_check && !id->is_underscore()
                && !is_guarded(id->uid)          /* #135 */
                && id->sym.kind == SymKind::unresolved
                && !declared_in_live_scopes(cur, id->uid))
            fix1_undefined(id);
    }

    /*
     * FIX-1 (#130): a name declared in NO scope is a COMPILE error.
     *
     * Free, because a SCRIPT's runtime symbols map is empty and asserted
     * (eval.h), so such a name is GUARANTEED to fail at run time - this only
     * moves a certain runtime failure to compile time. Never in the REPL,
     * which is open-world: a later input may declare it.
     */
    [[noreturn]] static void fix1_undefined(const Identifier *id)
    {
        throw UndefinedVariableEx(id->uid->val, id->start, id->end);
    }

    /* Stamp `id` SymKind::builtin if its name is a builtin (and not in REPL
     * mode, where builtins stay in the map); else leave it unresolved (map). */
    void stamp_builtin(Identifier *id)
    {
        if (repl_mode)
            return;

        const int bi = builtin_slot_index(id->uid);
        if (bi >= 0)
            id->sym = ResolvedSym{ SymKind::builtin, bi };
    }

    /* Count an assignment to a resolved-local lvalue as a write of its slot. */
    void count_write(FuncState *cur, Construct *lvalue)
    {
        /* A write to a GLOBAL-table slot (a function/global REASSIGNED - the
         * only writes to a global slot besides its decl, which never reaches
         * here) makes it NOT write-once: recorded UNCONDITIONALLY (not gated
         * on `cur->slottable`, so a reassign in any scope counts) for the
         * native-call write-once gate (#55). A LOCAL write is counted into the
         * frame's per-slot table (auto-const), which needs a slottable frame. */
        auto note = [&](Identifier *id) {
            if (!id)
                return;
            if (id->sym.kind == SymKind::global)
                reassigned_globals.insert(id->sym.slot);
            else if (id->sym.kind == SymKind::local
                     && cur && cur->slottable)
                cur->writes[id->sym.slot]++;
        };

        if (auto *id = dynamic_cast<Identifier *>(lvalue)) {
            note(id);
        } else if (auto *il = dynamic_cast<IdList *>(lvalue)) {
            for (auto &id : il->elems) {
                note(id.get());
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
                if (id->is_underscore())
                    continue;   /* `_` placeholder: not declared */
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
    /* #102: a TAG SWITCH (see Inferencer::for_each_child's note). NO
     * default: -Werror=switch keeps it exhaustive. The SCOPE nodes -
     * Block, for/foreach/for-range, try, Expr14, FuncDecl - are
     * DELIBERATELY childless here exactly as in the old chain:
     * Resolver::walk handles them and they never reach this walker
     * (the documented for_each_child contract). */
    switch (c->ct) {
    case ConstructType::expr01:
    case ConstructType::inlined_call:
    case ConstructType::throw_stmt: {
        fn(static_cast<SingleChildConstruct *>(c)->elem.get());
        break;
    }
    case ConstructType::expr02:
    case ConstructType::expr03:
    case ConstructType::expr04:
    case ConstructType::expr05:
    case ConstructType::expr06:
    case ConstructType::expr07:
    case ConstructType::expr08:
    case ConstructType::expr09:
    case ConstructType::expr10:
    case ConstructType::expr11:
    case ConstructType::expr12: {
        for (auto &p : static_cast<MultiOpConstruct *>(c)->elems)
            fn(p.second.get());
        break;
    }
    case ConstructType::typed_scalar: {
        /* an M8 specialized node: same elems shape as
         * MultiOpConstruct. The inliner normally runs before
         * specialize_types and never sees one, but a CROSS-INPUT
         * inlined prior body is already specialized. */
        for (auto &p : static_cast<TypedScalarExpr *>(c)->elems)
            fn(p.second.get());
        break;
    }
    case ConstructType::incdec: {
        fn(static_cast<IncDecExpr *>(c)->lvalue.get());
        break;
    }
    case ConstructType::ternary: {
        auto *n = static_cast<TernaryExpr *>(c);
        fn(n->condExpr.get());
        fn(n->thenExpr.get());
        fn(n->elseExpr.get());
        break;
    }
    case ConstructType::coalesce: {
        auto *n = static_cast<CoalesceExpr *>(c);
        fn(n->lhs.get());
        fn(n->rhs.get());
        break;
    }
    case ConstructType::call: {
        auto *n = static_cast<CallExpr *>(c);
        fn(n->what.get());
        fn(n->args.get());
        break;
    }
    case ConstructType::if_stmt: {
        auto *n = static_cast<IfStmt *>(c);
        fn(n->condExpr.get());
        fn(n->thenBlock.get());
        fn(n->elseBlock.get());
        break;
    }
    case ConstructType::while_stmt: {
        auto *n = static_cast<WhileStmt *>(c);
        fn(n->condExpr.get());
        fn(n->body.get());
        break;
    }
    case ConstructType::subscript: {
        auto *n = static_cast<Subscript *>(c);
        fn(n->what.get());
        fn(n->index.get());
        break;
    }
    case ConstructType::slice: {
        auto *n = static_cast<Slice *>(c);
        fn(n->what.get());
        fn(n->start_idx.get());
        fn(n->end_idx.get());
        break;
    }
    case ConstructType::member: {
        fn(static_cast<MemberExpr *>(c)->what.get());
        break;
    }
    case ConstructType::ret: {
        fn(static_cast<ReturnStmt *>(c)->elem.get());
        break;
    }
    case ConstructType::lit_dict_kv: {
        auto *n = static_cast<LiteralDictKVPair *>(c);
        fn(n->key.get());
        fn(n->value.get());
        break;
    }
    case ConstructType::lit_arr:
    case ConstructType::expr_list: {
        /* ExprList, LiteralArray (Block is handled in walk) */
        for (auto &e : static_cast<MultiElemConstruct<Construct> *>(c)
                           ->elems)
            fn(e.get());
        break;
    }
    case ConstructType::idlist: {
        for (auto &e : static_cast<MultiElemConstruct<Identifier> *>(c)
                           ->elems)
            fn(e.get());
        break;
    }
    case ConstructType::lit_dict: {
        auto *n = static_cast<MultiElemConstruct<LiteralDictKVPair> *>(c);
        for (auto &e : n->elems)
            fn(e.get());
        break;
    }
    /* literals, Identifier and childless constructs have no
     * children; the scope nodes (block/for/for-range/foreach/try/
     * expr14/func_decl/struct_decl) are walk()'s, never walked
     * here */
    case ConstructType::other:
    case ConstructType::nop:
    case ConstructType::brk:
    case ConstructType::cont:
    case ConstructType::rethrow:
    case ConstructType::lit_int:
    case ConstructType::lit_bool:
    case ConstructType::lit_float:
    case ConstructType::lit_none:
    case ConstructType::lit_str:
    case ConstructType::lit_obj:
    case ConstructType::id:
    case ConstructType::block:
    case ConstructType::expr14:
    case ConstructType::for_stmt:
    case ConstructType::for_range:
    case ConstructType::foreach_stmt:
    case ConstructType::try_catch:
    case ConstructType::func_decl:
    case ConstructType::struct_decl:
        break;
    }
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
/* ---- pure: detecting mutation of a REFERENCE-typed input parameter ---- */
/*
 * Mutating an array/dict/struct passed in IS an observable side effect (mylang
 * passes them by reference), so a function that does it is NOT pure - even
 * though its body looks "local". A SCALAR param (passed by copy) and a FRESH
 * local container are fine. We approximate this with a small taint analysis:
 * a reference-typed param is tainted; anything that may alias one becomes
 * tainted; an element/field WRITE through a tainted base makes the function
 * impure. Conservative (a clone()/slice of a param taints the result, costing a
 * pure-classification but never soundness).
 */

/* The root identifier of an lvalue chain (`x`, `a[i]`, `o.f.g`), else null. */
static const UniqueId *fmi_base_id(const Construct *lv)
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

/* Complete child visitor (the resolver's for_each_child omits the nodes its
 * walk handles - Block/for/foreach/try/Expr14 - so add them here). */
static void fmi_children(Construct *c,
                         const std::function<void(Construct *)> &fn)
{
    if (!c)
        return;
    /* #102: tag dispatch for the five added kinds; everything else
     * falls to the (already tag-switched) for_each_child */
    switch (c->ct) {
    case ConstructType::block: {
        for (auto &e : static_cast<Block *>(c)->elems)
            fn(e.get());
        return;
    }
    case ConstructType::for_stmt: {
        auto *f = static_cast<ForStmt *>(c);
        fn(f->init.get()); fn(f->cond.get());
        fn(f->inc.get()); fn(f->body.get());
        return;
    }
    case ConstructType::foreach_stmt: {
        auto *fe = static_cast<ForeachStmt *>(c);
        fn(fe->container.get()); fn(fe->body.get());
        return;
    }
    case ConstructType::try_catch: {
        auto *tc = static_cast<TryCatchStmt *>(c);
        fn(tc->tryBody.get());
        for (auto &p : tc->catchStmts) fn(p.second.get());
        fn(tc->finallyBody.get());
        return;
    }
    case ConstructType::expr14: {
        auto *e = static_cast<Expr14 *>(c);
        fn(e->lvalue.get()); fn(e->rvalue.get());
        return;
    }
    default:
        for_each_child(c, fn);
    }
}

/* `e`'s subtree reads some tainted id (over-approximates "may alias one"). */
static bool fmi_mentions(const Construct *e,
                         const std::unordered_set<const UniqueId *> &t)
{
    if (!e || e->ct == ConstructType::func_decl)
        return false;
    if (e->ct == ConstructType::id)
        return t.count(static_cast<const Identifier *>(e)->uid) != 0;
    bool found = false;
    fmi_children(const_cast<Construct *>(e),
                 [&](Construct *ch) { if (fmi_mentions(ch, t)) found = true; });
    return found;
}

/* One taint-propagation step over `c`: `var b = E`/`b = E` and a `foreach`
 * over a tainted container spread taint from a tainted rhs/container to the lhs
 * identifier / loop var. Sets `changed` when the set grows. */
static void fmi_propagate(Construct *c,
                          std::unordered_set<const UniqueId *> &t,
                          bool &changed)
{
    if (!c || dynamic_cast<FuncDeclStmt *>(c))
        return;
    if (auto *e = dynamic_cast<Expr14 *>(c)) {
        /* Only an IDENTIFIER-lvalue assignment makes the lhs alias the rhs
         * (`var b = a` / `b = a`); an element store `r[i] = a` writes the
         * (possibly fresh) container `r`, it does not make `r` alias `a`. So an
         * `r[i] = <tainted>` does NOT taint r - which is exactly why a
         * fresh-local builder `var r = [..]; r[i] = param` stays pure. (A
         * tainted value placed in a literal initializer, `var r = [a]`, DOES
         * taint r, since that is an identifier-lvalue assignment.) */
        if (fmi_mentions(e->rvalue.get(), t)) {
            if (auto *il = dynamic_cast<IdList *>(e->lvalue.get())) {
                for (auto &p : il->elems)
                    if (t.insert(p->uid).second) changed = true;
            } else if (dynamic_cast<Identifier *>(e->lvalue.get())) {
                if (const UniqueId *b = fmi_base_id(e->lvalue.get()))
                    if (t.insert(b).second) changed = true;
            }
        }
    } else if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {
        if (fe->ids && fmi_mentions(fe->container.get(), t))
            for (auto &p : fe->ids->elems)
                if (t.insert(p->uid).second) changed = true;
    }
    fmi_children(c, [&](Construct *ch) { fmi_propagate(ch, t, changed); });
}

/* True if `c` writes an element/field (`x[i]=`, `x.f=`, `x[i]++`) through a
 * tainted base - the mutation of an input. */
static bool fmi_has_tainted_write(
    const Construct *c, const std::unordered_set<const UniqueId *> &t)
{
    if (!c || dynamic_cast<const FuncDeclStmt *>(c))
        return false;
    const Construct *lv = nullptr;
    if (auto *e = dynamic_cast<const Expr14 *>(c))
        lv = e->lvalue.get();
    else if (auto *idc = dynamic_cast<const IncDecExpr *>(c))
        lv = idc->lvalue.get();
    if (lv && (dynamic_cast<const Subscript *>(lv) ||
               dynamic_cast<const MemberExpr *>(lv))) {
        const UniqueId *b = fmi_base_id(lv);
        if (b && t.count(b))
            return true;
    }
    bool found = false;
    fmi_children(const_cast<Construct *>(c), [&](Construct *ch) {
        if (fmi_has_tainted_write(ch, t)) found = true;
    });
    return found;
}

/*
 * THE PARAMETER ESCAPE ANALYSIS (#93, 2026-08-14).
 *
 * Answers, per parameter: can the REFERENCE this parameter is bound to
 * still be reachable after the call returns? If it cannot, the callee's
 * slot does not need its own count on it - the CALLER's slot holds one for
 * the whole of a synchronous call - and the retain/release pair a
 * reference argument pays today (measured 44 Ir to bind + its share of 30
 * to release, ~12% of 76_funcval_dispatch) is removable.
 *
 * ⛔ THIS PASS ONLY ANSWERS THE QUESTION. Wiring the answer to an actual
 * borrow needs ONE more thing, which is NOT here: an element write through
 * a SLICE detaches it in place (`try_flat_subscript_store`'s
 * `arr.clone_internal_vec()`, `get_value_for_put`'s
 * `*container = container->clone()`), and that RELEASES the old
 * SharedObject - a reference a borrowed slot never took, so a refcount
 * underflow. Sliceness is a RUNTIME property, so the bind has to decide
 * per call and record it: a borrow mask on the window record, which
 * `pop_window` then consults, with a slice argument falling back to a real
 * retain.
 *
 * A SECOND hazard was recorded here and is WRONG, corrected 2026-08-15
 * rather than deleted, because the reasoning is the kind that looks right:
 * the three element-store paths gate `clone_aliased_slices` on
 * `use_count() > 1`, and a borrow does not bump the count - so it seemed a
 * live slice of the argument could observe a write it must not. It cannot.
 * `use_count()` is the count on the SharedObject, and a slice HOLDS ITS
 * OWN intrusive_ptr to that same object; so a live slice already makes the
 * count >= 2 by itself. The borrow can only take the count 2 -> 1 in the
 * case where the callee's handle was the second one, i.e. where there are
 * no slices at all and `clone_aliased_slices` would be a no-op.
 * So this lands on its own, with its own tests, and the consumer follows.
 *
 * THE RULES, deliberately conservative - a false "does not escape" is a
 * use-after-free, a false "escapes" only costs the optimization. A param
 * does not escape iff ALL of:
 *   - it is never REASSIGNED (an assignment to the param would release
 *     whatever the slot holds, which a borrowed slot must never do);
 *   - EVERY occurrence of it is the base of a subscript or member access
 *     (`p[i]`, `p.f`, and those as assignment targets). Anything else -
 *     a bare read, an argument, a return operand, a capture, a throw -
 *     is treated as an escape, because each of those can outlive the call;
 *   - the body contains NO call of any kind. This is what makes the pass
 *     non-transitive and therefore cheap AND sound: a callee could pass
 *     the param on, or reassign the global the argument came from, and
 *     either would need a whole-program fixpoint to bound. The transitive
 *     version is the natural increment 2;
 *   - the body writes NO global, for the same reason: the argument
 *     expression at the call site may BE that global, and dropping its
 *     last reference mid-call would leave the borrow dangling;
 *   - the body declares no nested function (a capture list could take it);
 *   - every node in the body is a shape the walker can ENUMERATE
 *     (esc_known_shape) - an unknown kind hides occurrences, so it declines
 *     rather than guessing that it has none.
 *
 * Each rule is pinned by a row of the `param_escape_analysis` test that
 * FAILS when the rule is deleted, except the reassignment one, which is
 * proven redundant by an ML_CHECK instead - see the note at the check.
 */
static bool esc_is_base_position(const Construct *c, const Construct *child)
{
    if (auto *s = dynamic_cast<const Subscript *>(c))
        return s->what.get() == child;
    if (auto *m = dynamic_cast<const MemberExpr *>(c))
        return m->what.get() == child;
    return false;
}

/*
 * ⛔ FAIL CLOSED ON A NODE KIND THE WALKER DOES NOT KNOW.
 *
 * `fmi_children` delegates to `for_each_child`, a dynamic_cast chain whose
 * fallthrough means "this node has no children". For its original caller
 * (func_mutates_input) an unknown kind costs a pure classification. HERE it
 * would hide an occurrence of the parameter, mark it non-escaping, and the
 * consumer would drop a reference that is still reachable - a use-after-free
 * from a node kind nobody remembered to add.
 *
 * So the escape analysis does not trust the fallthrough: it names the kinds
 * whose children it knows are visited, plus the kinds that genuinely have
 * none, and treats ANYTHING ELSE as opaque. A node kind added later, or one
 * that exists at a stage this pass does not run at today, then costs the
 * optimization instead of soundness. `ForRangeStmt` is the live example -
 * absent from `for_each_child`, and only out of reach because this pass runs
 * before `specialize_types` builds one. Moving the pass later (which the
 * consumer may want) must not silently turn that into a dangling borrow.
 */
static bool esc_known_shape(const Construct *c)
{
    /* enumerable: fmi_children's own cases ... */
    if (dynamic_cast<const Block *>(c) ||
        dynamic_cast<const ForStmt *>(c) ||
        dynamic_cast<const ForeachStmt *>(c) ||
        dynamic_cast<const TryCatchStmt *>(c) ||
        dynamic_cast<const Expr14 *>(c))
        return true;
    /* ... and for_each_child's, in its own order */
    if (dynamic_cast<const SingleChildConstruct *>(c) ||
        dynamic_cast<const MultiOpConstruct *>(c) ||
        dynamic_cast<const TypedScalarExpr *>(c) ||
        dynamic_cast<const IncDecExpr *>(c) ||
        dynamic_cast<const TernaryExpr *>(c) ||
        dynamic_cast<const CoalesceExpr *>(c) ||
        dynamic_cast<const CallExpr *>(c) ||
        dynamic_cast<const IfStmt *>(c) ||
        dynamic_cast<const WhileStmt *>(c) ||
        dynamic_cast<const Subscript *>(c) ||
        dynamic_cast<const Slice *>(c) ||
        dynamic_cast<const MemberExpr *>(c) ||
        dynamic_cast<const ReturnStmt *>(c) ||
        dynamic_cast<const LiteralDictKVPair *>(c) ||
        dynamic_cast<const MultiElemConstruct<Construct> *>(c) ||
        dynamic_cast<const MultiElemConstruct<Identifier> *>(c) ||
        dynamic_cast<const MultiElemConstruct<LiteralDictKVPair> *>(c))
        return true;
    /* childless: a leaf cannot hide an occurrence */
    return dynamic_cast<const Literal *>(c) ||
           dynamic_cast<const LiteralObj *>(c) ||
           dynamic_cast<const Identifier *>(c) ||
           dynamic_cast<const ChildlessConstruct *>(c) ||
           dynamic_cast<const NopConstruct *>(c) ||
           dynamic_cast<const StructDeclStmt *>(c) ||
           dynamic_cast<const FuncDeclStmt *>(c);
}

/* Does this statement write a GLOBAL (`g = ..`, `g[i] = ..`, `g.f++`)? The
 * argument at some call site may BE that global, so dropping its last
 * reference mid-call would leave a borrow dangling. */
static bool esc_writes_global(const Construct *c)
{
    const Construct *lv = nullptr;
    if (auto *e = dynamic_cast<const Expr14 *>(c))
        lv = e->lvalue.get();
    else if (auto *idc = dynamic_cast<const IncDecExpr *>(c))
        lv = idc->lvalue.get();
    while (lv) {
        if (auto *id = dynamic_cast<const Identifier *>(lv))
            return id->sym.kind == SymKind::global;
        if (auto *s = dynamic_cast<const Subscript *>(lv)) {
            lv = s->what.get(); continue;
        }
        if (auto *m = dynamic_cast<const MemberExpr *>(lv)) {
            lv = m->what.get(); continue;
        }
        break;
    }
    return false;
}

/*
 * THE BUILTIN ARGUMENT-CAPTURE TABLE.
 *
 * A builtin is C++, so nothing about it can be derived from the AST: the
 * question "can a reference I hand this builtin still be reachable after it
 * returns?" is answered by READING ITS IMPLEMENTATION, once, here.
 *
 * A listed builtin is TRANSPARENT, which is three claims at once:
 *   1. it stores no reference to an argument anywhere that outlives the
 *      call;
 *   2. it does not return an argument ITSELF. Returning something reached
 *      THROUGH one is fine - `min(a)` hands back an ELEMENT of `a`, which
 *      carries its own count and stays alive on its own; the borrow is on
 *      the container;
 *   3. it invokes no MyLang CALLBACK. This is why a listed builtin skips
 *      the `callable_arg_mask` test at the call site: `len` cannot run user
 *      code whatever its argument's static type is, and refusing it for a
 *      `dyn` argument would cost the single most common shape in the
 *      language for no reason.
 *
 * ⛔ IT IS AN ALLOWLIST, SO IT FAILS CLOSED. An unlisted builtin - and any
 * builtin added later - captures every argument and is assumed to invoke,
 * costing the optimization and never soundness. That is what CLAUDE.md's
 * audit-table trap asks for: this table is read at a stage where a missing
 * entry cannot be noticed, so forgetting one has to be harmless.
 *
 * ⛔ AND CLAIM 3 IS WHY THE LIST IS AUDITED BY GREP, NOT BY EYE. `sum` was
 * on it in a first version, on the obvious reasoning that a sum is a number
 * - but `sum(arr, func)` is a REDUCE, and its second form calls
 * `eval_func` per element. The mechanical check is an awk over
 * src/builtins/ that prints the enclosing `EvalValue builtin_...` header of
 * every line mentioning `VmInvoker` or `eval_func(` - i.e. every builtin
 * that can run user code: make_array, make_dict, map/filter, find, sort,
 * and sum. RE-RUN IT when adding an entry.
 *
 * The instructive EXCLUSIONS, since "why is X not here" is the question a
 * future reader will have:
 *   runtime(a)          returns the argument ITSELF - the identity function
 *                       is the exact shape claim 2 exists for, and it is a
 *                       builtin real programs use
 *   append/push/insert  store the argument INTO a container that outlives
 *   dict(pairs)         builds a dict holding them
 *   sum/sort/map/       may run a callback (claim 3)
 *   filter/find/
 *   make_array/make_dict
 *   reverse/pop/erase/  mutate or hand back pieces in ways worth reading
 *   top/get             one at a time before trusting; not needed yet
 */
static bool esc_builtin_transparent(const UniqueId *name)
{
    static const char *const transparent[] = {
        "abs", "array_storage", "chr", "endswith", "float", "hash",
        "int", "intptr", "join", "kindstr", "len", "max", "min", "ord",
        "print", "split", "splitlines", "startswith", "str", "typestr",
    };
    for (const char *n : transparent)
        if (name->val == n)
            return true;
    return false;
}

/*
 * THE CALL-GRAPH ESCAPE FIXPOINT.
 *
 * One `EscFn` per analysable function body. `mask` starts OPTIMISTIC (every
 * candidate param assumed non-escaping) and only ever loses bits, so the
 * iteration is a greatest fixpoint - which is what makes RECURSION come out
 * right rather than needing a special case: `func f(a) { f(a); }` passes `a`
 * to a position that does not escape, so nothing clears it, and the answer
 * "the reference never leaves" is correct. Mutual recursion makes the graph
 * cyclic, so there is no traversal order and it has to be a fixpoint at all
 * - the same argument `build_reachable_reads` makes for the unbound-call
 * prover, which this deliberately mirrors.
 */
struct EscEdge {
    int callee;                 /* index into the EscFn vector */
    int argpos;                 /* my param sits at this argument position */
    int mine;                   /* ... and is my parameter number `mine` */
};

struct EscFn {
    FuncDeclStmt *fd = nullptr;
    uint64_t mask = 0;          /* candidates; shrinks to the fixpoint */
    uint64_t cands = 0;         /* the initial mask, kept for the reach stat */
    uint64_t written = 0;       /* params the body reassigns */
    bool unsafe = false;        /* writes a global, or calls something we
                                 * cannot name (which might write one) */
    std::vector<EscEdge> edges;
    std::vector<int> calls;     /* known callees, for the `unsafe` fixpoint */
};

#ifdef TESTS
/* WHY a function was poisoned - the reach diagnostic. Counted once per
 * function, at the FIRST rule that fires, so the columns sum to the number
 * of locally-poisoned functions and the rest were poisoned transitively. */
#define ESC_POISON(f, ctr) do { if (!(f)->unsafe) (ctr)++; \
                                (f)->unsafe = true; } while (0)
#else
#define ESC_POISON(f, ctr) ((f)->unsafe = true)
#endif

struct EscWorld {
    std::unordered_map<int, int> slot2fn;    /* global slot -> EscFn index */
    std::unordered_set<int> struct_slots;    /* global slot -> a struct name */
    std::unordered_set<int> written_slots;   /* reassigned global slots */
    /* INLINE LAMBDA -> EscFn index. A lambda has no name and no global
     * slot, so `slot2fn` cannot reach it; this is how a callback written
     * in the argument list is recognised as an ordinary callee. Keyed by
     * the node, which is safe here because the map is built and consumed
     * inside ONE run of the pass - the long-lived-node-pointer hazard
     * needs the map to outlive the tree. */
    std::unordered_map<const FuncDeclStmt *, int> fd2fn;
};

struct EscCtx {
    std::unordered_map<const UniqueId *, int> pidx;   /* param name -> index */
    const EscWorld *w;
    EscFn *self;
};

static void esc_scan(const Construct *c, EscCtx &ctx);

/*
 * The function a possibly-callable ARGUMENT names, or -1 if this pass cannot
 * say. Two shapes it can name, and they are the two that occur: an INLINE
 * LAMBDA, which is a FuncDeclStmt sitting in the argument list, and a NAMED
 * function in a global slot nothing reassigns - the same slot question an
 * ordinary direct call already asks. A parameter, a container element
 * (`ops[i]`) or a reassigned name is -1.
 */
static int esc_callback_fn(const Construct *arg, const EscCtx &ctx)
{
    if (auto *fd = dynamic_cast<const FuncDeclStmt *>(arg)) {
        auto it = ctx.w->fd2fn.find(fd);
        return it != ctx.w->fd2fn.end() ? it->second : -1;
    }
    auto *id = dynamic_cast<const Identifier *>(arg);
    if (!id || id->sym.kind != SymKind::global
            || ctx.w->written_slots.count(id->sym.slot))
        return -1;
    auto it = ctx.w->slot2fn.find(id->sym.slot);
    return it != ctx.w->slot2fn.end() ? it->second : -1;
}

/*
 * A HIGHER-ORDER BUILTIN runs a MyLang callback, and the pass cannot see
 * through the C++ that calls it - `map`/`filter`/`sort`/`find`/`make_array`/
 * `make_dict` all do. The first version therefore POISONED the whole
 * enclosing function, which is sound but very blunt: it was 9 of the 12
 * poisoned functions across samples/ + bench/my.
 *
 * It cannot see THROUGH the builtin, but it can nearly always see the
 * CALLBACK ITSELF - and a callback is just a callee. What the poison was
 * actually guarding against is a callback that writes a global (dropping a
 * reference some caller passed us) or reaches a function we cannot analyse;
 * both of those are exactly the `unsafe` flag the fixpoint already computes
 * per function and already propagates along `calls`. So NAME the callback
 * and add the edge. What the callback is HANDED is the container's elements,
 * never our parameters, so it owes no argument edges - only the unsafe one.
 *
 * ⛔ IT STILL FAILS CLOSED, in three places. An argument count past the mask's
 * width, a mask that was never stamped (`callable_arg_mask` defaults to ~0u,
 * so a bit outside the argument range is the tell - the same test LICM makes
 * for the same reason), and any callable argument this pass cannot NAME all
 * poison exactly as before. A callback reached through a parameter or a
 * container element is unanalysable, and saying so is the whole point.
 */
/*
 * Builtins that run NO MyLang code. A second allowlist, and a strictly
 * weaker claim than `esc_builtin_transparent`'s three - these MAY store an
 * argument or return one, they simply never call back into the language - so
 * it is a superset and is consulted only by the callback rule below.
 *
 * ⛔ IT IS AN ALLOWLIST OF NON-INVOKERS, NOT A LIST OF INVOKERS, AND THAT IS
 * DELIBERATE. The obvious alternative - list the higher-order builtins and
 * poison only for those - reads as more precise and is not auditable: the
 * mechanical grep for `VmInvoker`/`eval_func(` finds `make_array`,
 * `make_dict`, `sum`, `find_arr` and `hash`, and MISSES `map`, `filter` and
 * `sort`, which reach their callback through a shared helper whose enclosing
 * function is not named `builtin_*`. A list built from that grep would have
 * declared `sort` safe. Inverted, the same imprecision is harmless: an
 * invoker left off this list is simply not skipped.
 *
 * Each entry is a one-line builtin whose body can be read whole. The ones
 * that must NEVER appear here are the six the grep does find plus the three
 * it misses: map, filter, sort, find, sum, make_array, make_dict, range,
 * hash.
 */
static bool esc_builtin_no_invoke(const UniqueId *name)
{
    static const char *const quiet[] = {
        "append", "array", "clone", "deepclone", "dict", "dynarray",
        "erase", "exit", "insert", "keys", "pop", "push", "reverse",
        "runtime", "top", "values",
    };
    if (esc_builtin_transparent(name))
        return true;                     /* the stronger claim implies this */
    for (const char *q : quiet)
        if (name->val == q)
            return true;
    return false;
}

static void esc_scan_ho_builtin(const CallExpr *call, EscCtx &ctx)
{
    const size_t na = call->args->elems.size();
    if (na >= 32) {
        ESC_POISON(ctx.self, g_esc_p_hobuiltin);
        return;
    }
    const uint32_t valid = na ? ((1u << na) - 1u) : 0u;
    const uint32_t m = call->callable_arg_mask;
    if (m & ~valid) {                    /* never stamped - see above */
        ESC_POISON(ctx.self, g_esc_p_hobuiltin);
        return;
    }
    for (size_t j = 0; j < na; j++) {
        if (!(m >> j & 1))
            continue;
        const int cb = esc_callback_fn(call->args->elems[j].get(), ctx);
        if (cb < 0) {
            ESC_POISON(ctx.self, g_esc_p_hobuiltin);
            return;
        }
        ctx.self->calls.push_back(cb);
    }
}

/* Classify one call: which bits it clears, which edges it owes, and whether
 * it can reach code that writes a global. */
static void esc_scan_call(const CallExpr *call, EscCtx &ctx)
{
    auto *cid = dynamic_cast<const Identifier *>(call->what.get());
    const bool is_builtin = cid && cid->sym.kind == SymKind::builtin;
    /*
     * A STRUCT CONSTRUCTION is a CallExpr whose callee names a struct. The
     * inferencer's `vm_struct_ctor_def`/`vm_struct_boxed_def` stamps say so
     * for the calls IT typed, but a TEMPLATE body is skipped by the check
     * pass and carries neither - so the slot the resolver hoisted the
     * struct name into is the reliable question. Without that, `B(a)` in a
     * template read as a callee we cannot name and poisoned the function
     * (watched: the struct-ctor row answered 0x0 instead of 0x2).
     */
    const bool is_ctor = call->vm_struct_ctor_def || call->vm_struct_boxed_def
        || (cid && call->direct_func_slot >= 0
            && ctx.w->struct_slots.count(call->direct_func_slot));

    int callee = -1;
    if (!is_builtin && !is_ctor && cid && call->direct_func_slot >= 0
        && !ctx.w->written_slots.count(call->direct_func_slot)) {
        auto it = ctx.w->slot2fn.find(call->direct_func_slot);
        if (it != ctx.w->slot2fn.end())
            callee = it->second;
    }

    /*
     * A callee we cannot NAME could do anything, including reassigning the
     * global some caller passed us - so it poisons this function the same
     * way a direct global write does. A STRUCT CONSTRUCTION is named and
     * harmless in that respect (it binds fields, calls nothing), it merely
     * captures whatever it is given. And a HIGHER-ORDER builtin runs a
     * MyLang callback that this pass cannot see: `callable_arg_mask` is the
     * inferencer's existing answer to "which arguments might be callable"
     * (LICM asks it for the same reason), and it defaults to ~0u, so an
     * unstamped call declines.
     */
    const bool transparent = is_builtin && esc_builtin_transparent(cid->uid);
    if (!is_builtin && !is_ctor && callee < 0)
        ESC_POISON(ctx.self, g_esc_p_callee);
    if (is_builtin && !transparent && call->args
            && !esc_builtin_no_invoke(cid->uid))
        esc_scan_ho_builtin(call, ctx);
    if (callee >= 0)
        ctx.self->calls.push_back(callee);

    /* the callee EXPRESSION: `p()` is a bare read of p, `p[0]()` is not */
    if (cid) {
        auto it = ctx.pidx.find(cid->uid);
        if (it != ctx.pidx.end())
            ctx.self->mask &= ~(uint64_t(1) << it->second);
    } else {
        esc_scan(call->what.get(), ctx);
    }

    if (!call->args)
        return;
    for (size_t j = 0; j < call->args->elems.size(); j++) {
        const Construct *arg = call->args->elems[j].get();
        auto *aid = dynamic_cast<const Identifier *>(arg);
        auto it = aid ? ctx.pidx.find(aid->uid) : ctx.pidx.end();
        if (it == ctx.pidx.end()) {
            esc_scan(arg, ctx);
            continue;
        }
        if (transparent)
            continue;                       /* audited: stores nothing */
        if (callee >= 0 && j < 64) {
            ctx.self->edges.push_back(
                EscEdge{ callee, static_cast<int>(j), it->second });
            continue;                       /* resolved by the fixpoint */
        }
        ctx.self->mask &= ~(uint64_t(1) << it->second);
    }
}

/* Clear a param's bit the moment it is used anywhere but a base position (or
 * an argument position the fixpoint can discharge). */
static void esc_scan(const Construct *c, EscCtx &ctx)
{
    if (!c)
        return;
    if (!esc_known_shape(c)) {
        ESC_POISON(ctx.self, g_esc_p_shape); /* see esc_known_shape */
        return;
    }
    /*
     * A NESTED FUNCTION READS NOTHING BUT ITS CAPTURE LIST, and in MyLang
     * that list is EXPLICIT - a lambda is `func [x, y] (...)` and a NAMED
     * nested function may not have one at all (the grammar rejects it). Its
     * body is parented to `capture_root`, the program root, so it cannot
     * reach this frame by any other route. So the list IS the answer: clear
     * the bits of the parameters it names and do not descend.
     *
     * A first version poisoned the whole function for any nested decl, and
     * the reach measurement is what argued it down: 8 of the 15
     * candidate-bearing functions in samples/ + bench/my lost everything
     * that way. Clearing the captured bits is still conservative - a
     * capture SNAPSHOTS the value, taking a reference of its own, so it may
     * well be borrowable - but conservative here costs only the
     * optimization.
     */
    if (auto *nested = dynamic_cast<const FuncDeclStmt *>(c)) {
        if (nested->captures) {
            for (auto &cap : nested->captures->elems) {
                auto it = ctx.pidx.find(cap->uid);
                if (it != ctx.pidx.end())
                    ctx.self->mask &= ~(uint64_t(1) << it->second);
            }
        }
        return;
    }
    if (auto *call = dynamic_cast<const CallExpr *>(c)) {
        esc_scan_call(call, ctx);
        return;
    }
    if (esc_writes_global(c))
        ESC_POISON(ctx.self, g_esc_p_gwrite);
    fmi_children(const_cast<Construct *>(c), [&](Construct *ch) {
        if (!ch)
            return;
        if (auto *id = dynamic_cast<const Identifier *>(ch)) {
            auto it = ctx.pidx.find(id->uid);
            if (it != ctx.pidx.end() && !esc_is_base_position(c, ch))
                ctx.self->mask &= ~(uint64_t(1) << it->second);
            return;                        /* an Identifier has no children */
        }
        esc_scan(ch, ctx);
    });
}

/*
 * The LOCAL half: this body's own contribution, before the fixpoint. Bit i
 * set == param i does not escape as far as this body alone can tell.
 *
 * ⛔ THE BODY IS SCANNED EVEN WHEN THERE IS NOTHING TO CLAIM. `unsafe` is
 * not a fact about this function's own parameters, it is a fact its CALLERS
 * need - "code reachable from here may reassign a global" - so a body with
 * no candidate params (all scalar, or none at all) must still be walked for
 * its writes and its call edges. Returning early there is the bug that made
 * the global-write row pass its poison nowhere: `func h(k) { g = [k]; }`
 * has one int param, so it had no candidates, so it was never scanned, so
 * `f` calling it stayed clean and marked its own parameter safe.
 */
static void esc_scan_body(EscFn &f, const EscWorld &w)
{
    FuncDeclStmt *fd = f.fd;
    if (!fd->body)
        return;
    const size_t n = fd->params ? fd->params->elems.size() : 0;

    EscCtx ctx;
    ctx.w = &w;
    ctx.self = &f;
    uint64_t mask = 0, written = 0;
    for (size_t i = 0; i < n && i < 64; i++) {
        Identifier *p = dynamic_cast<Identifier *>(fd->params->elems[i].get());
        if (!p)
            continue;
        /* A SCALAR param is a by-value copy - there is no reference to
         * borrow, so it is neither interesting nor claimed here. Asked of
         * the DESCRIPTOR (ParamDesc::binds_scalar), which is the same
         * predicate codegen's ref_slots join uses to decide the slot can
         * never hold a reference; the param DECL's own `th` is not stamped
         * by annotate_hints, so testing that answered "not scalar" for
         * every param and the skip never fired. */
        if (fd->desc && i < fd->desc->params.size()
            && fd->desc->params[i].binds_scalar())
            continue;
        if (i < fd->slot_writes.size() && fd->slot_writes[i])
            written |= uint64_t(1) << i;
        ctx.pidx.emplace(p->uid, static_cast<int>(i));
        mask |= uint64_t(1) << i;
    }
    f.mask = mask;
    f.cands = mask;
    f.written = written;
    /* past 64 params nothing is claimable, but the body still has to be
     * walked for `unsafe` and for the call edges (see the note above) */
    if (n > 64)
        f.mask = 0;
    esc_scan(fd->body.get(), ctx);
}

/*
 * REASSIGNMENT: an assignment to the parameter itself would release whatever
 * the slot holds, which a borrowed slot must never do.
 *
 * ⛔ THE GUARD IS PROVEN REDUNDANT RATHER THAN TESTED, on purpose - and the
 * proof is the CHECK, not a comment. No program can reach it: every spelling
 * of a write to a parameter - `a = e`, `a += e`, `a++`, `a, b = pair` - puts
 * the parameter's Identifier somewhere that is not a subscript/member BASE,
 * so the scan has already cleared the bit. A test row for it is therefore
 * impossible to write, and one that merely LOOKS like a test of it (an
 * ordinary `a = [9]` row) passes with the guard deleted - which is what
 * "defensive, not proven" meant, and the sabotage run confirms: deleting the
 * guard leaves the whole suite green.
 *
 * The check earns its place by firing on the change that would make the
 * guard load-bearing again. WATCHED: make `esc_is_base_position` answer true
 * for everything and this assert - not any test row - is what stops the
 * build, because a reassigned param then survives the scan. Keep both; the
 * failure direction here is a use-after-free.
 */
static uint64_t esc_final_mask(const EscFn &f)
{
    if (f.unsafe)
        return 0;
    ML_CHECK((f.mask & f.written) == 0);
    return f.mask & ~f.written;
}

/* True if `fd` may mutate a reference-typed parameter (so it is not pure). */
static bool func_mutates_input(FuncDeclStmt *fd)
{
    if (!fd->body || !fd->params)
        return false;
    std::unordered_set<const UniqueId *> tainted;
    for (auto &p : fd->params->elems)
        if (p->th != TypeHint::i && p->th != TypeHint::f)
            tainted.insert(p->uid);   /* a non-scalar (ref/str/dyn) param */
    if (tainted.empty())
        return false;
    bool changed = true;
    while (changed) {
        changed = false;
        fmi_propagate(fd->body.get(), tainted, changed);
    }
    return fmi_has_tainted_write(fd->body.get(), tainted);
}

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

    const int nparams =
        fd->params ? static_cast<int>(fd->params->elems.size()) : 0;
    st.slottable = nparams <= MAX_SLOTS;

    if (st.slottable) {

        /*
         * Capture scope (outermost, so a param shadows a same-named capture). A
         * captured name resolves to SymKind::capture + its index into the
         * closure's per-instance capture_slots vector - an O(1) slot, not a map
         * walk. Indices match declaration order, which the FuncObject ctor
         * fills in the same order. Captures have their OWN index space
         * (0..C-1), independent of the frame's next_slot.
         */
        if (fd->captures) {
            st.scopes.emplace_back();
            int ci = 0;
            for (auto &cap : fd->captures->elems)
                st.scopes.back().decls.push_back(
                    { cap->uid, ci++, SymKind::capture, DeclType::none });
        }

        st.scopes.emplace_back();   /* the function's outermost (param) scope */

        for (int i = 0; i < nparams; i++) {
            Identifier *p = fd->params->elems[i].get();
            check_no_redecl(&st, p);   /* duplicate param -> error */
            st.scopes.back().decls.push_back(
                { p->uid, i, SymKind::local, p->decl_type });
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

    /*
     * A function that MUTATES a reference-typed parameter is not pure (mylang
     * passes arrays/dicts/structs by reference - mutating one is observable).
     * This demotes `effective_pure` for BOTH an auto-pure candidate and an
     * explicit `pure func` that does it (explicit_pure - the user's declaration
     * - is left intact, so ispuredecl() still reports it while ispure() does
     * not). Scalar-param and fresh-local mutation stay pure (see
     * func_mutates_input).
     */
    if (func_mutates_input(fd)) {
        if (fd->desc->effective_pure && fd->id)
            TRACE(autopure, 0, std::string(fd->id->get_str()) +
                  "  mutates a reference parameter -> NOT pure");
        fd->desc->effective_pure = false;
    }
    /* Auto-pure: a non-pure function with no captures whose body is effectively
     * pure (and mutates no input) is promoted, so ispure() sees it and its
     * const-arg calls can fold (in the auto-const pass). */
    else if (!fd->desc->effective_pure
            && (!fd->captures || fd->captures->elems.empty())
            && fd->body) {
        /*
         * Optimistic self-recursion: assume `fd` is pure while checking its own
         * body, so a recursive self-call counts as a call to a pure function.
         * Sound by induction - a recursive call to a pure function is pure, so
         * if the body is otherwise pure under that assumption, fd really is pure
         * (the only new free name it then has is itself). Mutual recursion is
         * not handled (g isn't in pure_func_names when f is decided). Undo the
         * optimistic add if the body turns out impure. (A recursive pure func is
         * NOT eagerly const-folded - see register_pure_funcs / the ctor guard.)
         */
        bool added_self = false;
        if (fd->id && !pure_func_names.count(fd->id->uid)) {
            pure_func_names.insert(fd->id->uid);
            added_self = true;
        }
        if (func_body_is_pure(fd->body.get(), pure_func_names)) {
            fd->desc->effective_pure = true;
            if (fd->id)
                TRACE(autopure, 0, std::string(fd->id->get_str()) +
                      "  reads only consts/params -> effective pure");
        } else if (added_self) {
            pure_func_names.erase(fd->id->uid);
        }
    }

    /* record any proven-pure function (auto OR explicit) so a LATER function
     * that calls it is recognized pure too (see func_body_is_pure). */
    if (fd->desc->effective_pure && fd->id)
        pure_func_names.insert(fd->id->uid);

    if (st.slottable && st.next_slot > 0) {
        fd->desc->resolved = true;
        fd->desc->frame_size = st.next_slot;
        fd->slot_writes = std::move(st.writes);
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

            /* Snapshot the RESOLVED capture sources into the descriptor: the
             * FuncObject ctor reads kind/slot (read_sym), never the capture
             * Identifiers, so closure creation is AST-free. Unresolved (REPL
             * top-level) captures keep the by-name map fallback via `name`. */
            sync_capture_desc(fd);
            /* ...but an ESCAPED GLOBAL capture is stamped only AFTER pass 2
             * (escaped_refs), so this snapshot would record `unresolved` and
             * silently fall back to the by-name map walk - which in a SCRIPT
             * is the asserted-empty map, so the read reported "undefined"
             * instead of UnboundSymbolEx (#131). Re-sync at the end. */
            capture_funcs.push_back(fd);
        }

        /* A hoisted top-level function already has a GLOBAL slot (resolved via
         * global_func_slots); don't also add a masking entry (which would
         * shadow it to the map). Any other function name is a local of the
         * enclosing scope (masking entry: stays in the map so forward
         * references / mutual recursion work). */
        if (fd->id && fd->id->sym.kind != SymKind::global)
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

    /* --- ++ / -- : resolve the operand AND count it as a write (so the var is
     * not treated as write-once / auto-const-promotable). --- */
    if (auto *idc = dynamic_cast<IncDecExpr *>(c)) {
        walk(idc->lvalue.get(), cur);
        count_write(cur, idc->lvalue.get());
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

        if (track) {
            cur->scopes.emplace_back();
            /* FIX-1: record what this block declares, INCLUDING below the
             * walk position, before any statement resolves against it. */
            collect_scope_names(b, cur->scopes.back());
            if (cur->is_main && cur->scopes.size() == 1)
                for (const UniqueId *n : cur->scopes.back().all_names)
                    root_decl_names.insert(n);
        }

        /* Hoist non-capturing nested funcs / structs into THIS scope first, so a
         * forward / mutual reference within the block resolves to its (block-
         * scoped) global slot. */
        if (track)
            hoist_scoped_decls(cur, b);

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
                if (id->is_underscore())
                    continue;   /* `_` placeholder: not declared/resolved */
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

    /* --- if: a `defined()` guard vouches for its name (#135) --- */
    if (auto *iff = dynamic_cast<IfStmt *>(c)) {

        std::vector<const UniqueId *> names;
        collect_defined_guards(iff->condExpr.get(), names);

        const size_t mark = guarded.size();
        for (const UniqueId *u : names)
            guarded.push_back(u);

        /* the CONDITION is walked with them active, so a later conjunct may
         * use the name (`if (defined(x) && isbound(x))`) */
        walk(iff->condExpr.get(), cur);
        walk(iff->thenBlock.get(), cur);

        guarded.resize(mark);       /* the ELSE arm proves nothing */
        walk(iff->elseBlock.get(), cur);
        return;
    }

    /* --- call: resolve callee + args, then devirtualize a global callee --- */
    if (auto *call = dynamic_cast<CallExpr *>(c)) {
        walk(call->what.get(), cur);

        /* A LAZY builtin (`defined`/`isbound`/`isconst`/`isconstdecl`) never
         * EVALUATES its argument - it asks a question ABOUT the name - so
         * asking about a name inside its TDZ is legal for all of them and
         * must answer rather than throw (that is what lets try_fold_defined /
         * try_fold_isbound fold it).
         *
         * `isbound` is exempt TOO (maintainer, 2026-08-09 - it was not, until
         * the short form proved to be what programs want): asking whether a
         * name that exists nowhere is bound has the obvious answer `false`,
         * and refusing it forced every feature test to spell out
         * `defined(x) && isbound(x)`. The typo hazard that buys is one
         * `defined()` already accepts - `defined(confg)` folds to false in
         * silence - so this makes the two lazy queries consistent rather than
         * adding a new class of risk. */
        const Identifier *cid = dynamic_cast<Identifier *>(call->what.get());
        const bool lazy = cid && is_lazy_builtin(cid->uid);
        const bool saved_nc = no_undef_check;
        const bool saved_tz = no_tdz_check;

        if (lazy) {
            no_tdz_check = true;
            no_undef_check = true;
        }

        if (call->args)
            for (auto &a : call->args->elems)
                walk(a.get(), cur);

        no_undef_check = saved_nc;
        no_tdz_check = saved_tz;

        /* If the callee resolved to a global-table slot (a top-level / scoped
         * function, or a struct descriptor), record it so do_eval can read the
         * callable straight from the slot. A struct construction or a slot that
         * later holds a non-function falls through at runtime (is<FuncObject>),
         * so recording any global slot is sound. */
        if (auto *id = dynamic_cast<Identifier *>(call->what.get()))
            if (id->sym.kind == SymKind::global)
                call->direct_func_slot = id->sym.slot;

        return;
    }

    /* --- everything else: generic traversal --- */
    for_each_child(c, [&](Construct *ch) { walk(ch, cur); });
}

} // anonymous namespace

/* ----------------------- Function inlining ------------------------ */

/*
 * Size-only inlining of expression-bodied, top-level, non-capturing,
 * non-recursive functions at direct call sites (plans/archived/function-inlining.md).
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
void
for_each_child_slot(Construct *c,
                    const std::function<void(unique_ptr<Construct> &)> &fn)
{
    /* #102: a TAG SWITCH (see Inferencer::for_each_child's note) -
     * the dynamic_cast chain here was the resolver's share of the
     * compile RTTI bill. NO default: -Werror=switch keeps it
     * exhaustive. Child sets and null guards are EXACTLY the old
     * chain's. */
    switch (c->ct) {
    case ConstructType::expr01:
    case ConstructType::inlined_call:
    case ConstructType::throw_stmt: {
        fn(static_cast<SingleChildConstruct *>(c)->elem);
        break;
    }
    case ConstructType::expr02:
    case ConstructType::expr03:
    case ConstructType::expr04:
    case ConstructType::expr05:
    case ConstructType::expr06:
    case ConstructType::expr07:
    case ConstructType::expr08:
    case ConstructType::expr09:
    case ConstructType::expr10:
    case ConstructType::expr11:
    case ConstructType::expr12: {
        for (auto &p : static_cast<MultiOpConstruct *>(c)->elems)
            fn(p.second);
        break;
    }
    case ConstructType::typed_scalar: {
        /* M8 specialized node (same elems shape); a cross-input
         * inlined prior body is already specialized, so substitution
         * must descend here. */
        for (auto &p : static_cast<TypedScalarExpr *>(c)->elems)
            fn(p.second);
        break;
    }
    case ConstructType::incdec: {
        fn(static_cast<IncDecExpr *>(c)->lvalue);
        break;
    }
    case ConstructType::call: {
        auto *n = static_cast<CallExpr *>(c);
        fn(n->what);
        for (auto &e : n->args->elems) fn(e);
        break;
    }
    case ConstructType::if_stmt: {
        auto *n = static_cast<IfStmt *>(c);
        fn(n->condExpr);
        if (n->thenBlock) fn(n->thenBlock);
        if (n->elseBlock) fn(n->elseBlock);
        break;
    }
    case ConstructType::while_stmt: {
        auto *n = static_cast<WhileStmt *>(c);
        fn(n->condExpr); fn(n->body);
        break;
    }
    case ConstructType::for_stmt: {
        auto *n = static_cast<ForStmt *>(c);
        if (n->init) fn(n->init);
        if (n->cond) fn(n->cond);
        if (n->inc) fn(n->inc);
        fn(n->body);
        break;
    }
    case ConstructType::foreach_stmt: {
        auto *n = static_cast<ForeachStmt *>(c);
        fn(n->container); fn(n->body);
        break;
    }
    case ConstructType::for_range: {
        /* post-specialize node (fold_show_calls walks the FINAL
         * tree) */
        auto *n = static_cast<ForRangeStmt *>(c);
        if (n->init) fn(n->init);
        fn(n->bound);
        if (n->step) fn(n->step);
        fn(n->body);
        break;
    }
    case ConstructType::subscript: {
        auto *n = static_cast<Subscript *>(c);
        fn(n->what); fn(n->index);
        break;
    }
    case ConstructType::slice: {
        auto *n = static_cast<Slice *>(c);
        fn(n->what);
        if (n->start_idx) fn(n->start_idx);
        if (n->end_idx) fn(n->end_idx);
        break;
    }
    case ConstructType::member: {
        fn(static_cast<MemberExpr *>(c)->what);
        break;
    }
    case ConstructType::ternary: {
        auto *n = static_cast<TernaryExpr *>(c);
        fn(n->condExpr); fn(n->thenExpr); fn(n->elseExpr);
        break;
    }
    case ConstructType::coalesce: {
        auto *n = static_cast<CoalesceExpr *>(c);
        fn(n->lhs); fn(n->rhs);
        break;
    }
    case ConstructType::ret: {
        auto *n = static_cast<ReturnStmt *>(c);
        if (n->elem) fn(n->elem);
        break;
    }
    case ConstructType::expr14: {
        auto *n = static_cast<Expr14 *>(c);
        if (n->lvalue) fn(n->lvalue);
        if (n->rvalue) fn(n->rvalue);
        break;
    }
    case ConstructType::lit_dict_kv: {
        auto *n = static_cast<LiteralDictKVPair *>(c);
        fn(n->key); fn(n->value);
        break;
    }
    case ConstructType::func_decl: {
        auto *n = static_cast<FuncDeclStmt *>(c);
        if (n->body) fn(n->body);
        break;
    }
    case ConstructType::try_catch: {
        auto *n = static_cast<TryCatchStmt *>(c);
        fn(n->tryBody);
        for (auto &p : n->catchStmts) fn(p.second);
        if (n->finallyBody) fn(n->finallyBody);
        break;
    }
    case ConstructType::block:
    case ConstructType::lit_arr:
    case ConstructType::expr_list: {
        /* Block, ExprList, LiteralArray */
        for (auto &e : static_cast<MultiElemConstruct<Construct> *>(c)
                           ->elems)
            fn(e);
        break;
    }
    case ConstructType::lit_dict: {
        auto *n = static_cast<MultiElemConstruct<LiteralDictKVPair> *>(c);
        for (auto &e : n->elems) { fn(e->key); fn(e->value); }
        break;
    }
    /* literals, Identifier, IdList, childless: nothing inlinable */
    case ConstructType::other:
    case ConstructType::nop:
    case ConstructType::brk:
    case ConstructType::cont:
    case ConstructType::rethrow:
    case ConstructType::lit_int:
    case ConstructType::lit_bool:
    case ConstructType::lit_float:
    case ConstructType::lit_none:
    case ConstructType::lit_str:
    case ConstructType::lit_obj:
    case ConstructType::id:
    case ConstructType::idlist:
    case ConstructType::struct_decl:
        break;
    }
}

/* True if `c` holds a ForRangeStmt anywhere (walking statement containers - a
 * ForRangeStmt only ever appears as a statement). Used to keep cross-input
 * inlining off a post-specialization body that contains one (see Inliner). */
static bool has_for_range(const Construct *c)
{
    if (!c)
        return false;
    if (dynamic_cast<const ForRangeStmt *>(c))
        return true;
    if (auto *b = dynamic_cast<const Block *>(c)) {
        for (auto &e : b->elems)
            if (has_for_range(e.get()))
                return true;
        return false;
    }
    if (auto *i = dynamic_cast<const IfStmt *>(c))
        return has_for_range(i->thenBlock.get()) ||
               has_for_range(i->elseBlock.get());
    if (auto *w = dynamic_cast<const WhileStmt *>(c))
        return has_for_range(w->body.get());
    if (auto *f = dynamic_cast<const ForStmt *>(c))
        return has_for_range(f->body.get());
    if (auto *fe = dynamic_cast<const ForeachStmt *>(c))
        return has_for_range(fe->body.get());
    if (auto *t = dynamic_cast<const TryCatchStmt *>(c)) {
        if (has_for_range(t->tryBody.get()))
            return true;
        for (auto &p : t->catchStmts)
            if (has_for_range(p.second.get()))
                return true;
        return has_for_range(t->finallyBody.get());
    }
    return false;
}

class Inliner {

    /* uid -> the unique top-level expression-bodied func to inline, or nullptr
     * if the name is ambiguous (declared more than once). */
    std::unordered_map<const UniqueId *, FuncDeclStmt *> funcs;

    /* uid -> the unique top-level block-bodied func to specialize (clone for a
     * given const-arg tuple), or nullptr if ambiguous. */
    std::unordered_map<const UniqueId *, FuncDeclStmt *> spec_funcs;

    /* uid -> the unique top-level block-bodied func to INLINE in place via an
     * InlinedCallExpr (v1: no-locals, non-recursive, weight below a call), or
     * nullptr if ambiguous. Disjoint from `funcs` (those are expr-bodied); a
     * func here may also be in spec_funcs (tail-inline / specialize get first
     * crack in walk, block-inline catches the other call positions). */
    std::unordered_map<const UniqueId *, FuncDeclStmt *> block_funcs;

    const int max_nodes;   /* inline only when the body is at most this big */
    EvalContext cctx;      /* const context for re-folding spliced bodies */
    AutoConst ac;          /* used to fold specialized clones */
    AnalysisInfo *analysis;   /* -a: record inlined / specialized; or null */
    EvalContext *prior_scope; /* REPL: earlier inputs' globals (or null) */
    Block *root_block = nullptr;   /* set in run(); for slotting spec clones */
    bool repl_mode = false;        /* REPL: clones stay map-resident */

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
    /* A self-recursive function's body is unrolled in place until it reaches
     * this many nodes (count_all_nodes), then the remaining self-calls are the
     * frontier (deduped at runtime by the per-frame cache). Bounds the unrolled
     * decl size; tune vs bench/. */
    /* How deep a self-recursive func is unrolled. The DEPTH is chosen per-func
     * from the cost model so a level always fits a weight BUDGET (and a level is
     * all-or-nothing - every self-call at a level is expanded, so the unroll is
     * BALANCED). Each level roughly multiplies the body weight by the branching
     * factor (#self-calls), so a small body (fib) gets 2 levels while a big one
     * gets fewer or none - 2 levels of a huge body would be a waste.
     * `rec_unroll_depth()` computes it; `REC_UNROLL_BUDGET` is the projected
     * post-unroll weight ceiling, `REC_UNROLL_MAX` a hard level cap. */
    /* Tuned (via the `--trace inline` rec_unroll_depth line) so fib (w0=71,
     * branching 2) unrolls 2 levels and tribonacci (w0=97, branching 3) 1; a
     * body weight >~150 unrolls 0 levels (2 levels of a big body is a waste). */
    static const long REC_UNROLL_BUDGET = 300;
    static const int REC_UNROLL_MAX = 3;
    /* A high node backstop for a pathological body (many self-calls x depth). */
    static const int REC_NODE_CAP = 600;
    /* Max weight of an arg that is DIRECT-substituted (re-evaluated at each param
     * use) instead of bound to a temp. Only for a PURE callee (its args are
     * side-effect-free, so re-evaluation is sound); the cap keeps re-eval cheap
     * and excludes a nested call (weight ~21). A temp-free body is what lets a
     * guard body convert to an EXPRESSIBLE ternary (guard_to_ternary). */
    static const long ARG_SUBST_MAX = 8;
    int rec_depth = 0;   /* current self-inline depth along the walk path */
    long inline_budget = 0;

    /* A recursive function's ORIGINAL (pre-unroll) body, saved on first sight so
     * each self-call splices the original, not the in-place-growing body (which
     * would compound). Keyed by the FuncDeclStmt (stable for the run). */
    std::unordered_map<FuncDeclStmt *, unique_ptr<Construct>> rec_orig;
    /* Per-func unroll depth, computed once from the ORIGINAL body (the first
     * rec_unroll_depth() call, before any unroll grows the body). */
    std::unordered_map<const FuncDeclStmt *, int> rec_levels;

    /* The unroll depth for a self-recursive func: keep adding levels while the
     * projected body weight (w * branching each level) fits REC_UNROLL_BUDGET,
     * capped at REC_UNROLL_MAX. Cached, because the body grows during unroll but
     * the depth must be computed from the ORIGINAL (the first call sees it). */
    int rec_unroll_depth(const FuncDeclStmt *f)
    {
        auto it = rec_levels.find(f);
        if (it != rec_levels.end())
            return it->second;
        const long branching = count_uid(f->body.get(), f->id->uid);
        const long w0 = body_weight(f->body.get());
        long w = w0;
        int k = 0;
        while (k < REC_UNROLL_MAX && branching >= 2
               && w * branching <= REC_UNROLL_BUDGET) {
            w *= branching;
            k++;
        }
        TRACE(inlining, 0, std::string("rec_unroll_depth ") +
              std::string(f->id->get_str()) + " w0=" + std::to_string(w0) +
              " branching=" + std::to_string(branching) +
              " -> " + std::to_string(k) + " level(s)");
        rec_levels[f] = k;
        return k;
    }

public:

    explicit Inliner(int max_nodes, AnalysisInfo *a = nullptr,
                     EvalContext *prior_scope = nullptr, bool repl = false)
        /* `ac` folds specialization *clones*, not the original source, so it
         * must NOT record analysis (that would color the original body for one
         * specialized call). The Inliner records inline/specialize itself.
         * `prior_scope` (REPL): earlier inputs' globals, so a call to a
         * prior-input function inlines/specializes across inputs. */
        : max_nodes(max_nodes), cctx(nullptr, true), ac(), analysis(a),
          prior_scope(prior_scope), repl_mode(repl) { }

    void run(Block *root)
    {
        root_block = root;   /* for slotting spec-clone names in script mode */
        for (auto &e : root->elems) {
            auto *fd = dynamic_cast<FuncDeclStmt *>(e.get());
            if (!fd || !fd->id)
                continue;
            if (inlinable_decl(fd))
                add_unique(funcs, fd);
            else if (specializable_decl(fd))
                add_unique(spec_funcs, fd);
            /* independent: a block body can be both specialized and inlined.
             * But a func in `funcs` (the -it-gated EXPRESSION engine - a
             * single-return body, either spelling) stays OUT of the block
             * engine: it would bypass the -it gate (block-inline is
             * CALL_WEIGHT-gated), and the expression splice already covers
             * every call site. A single-return body the expr engine REFUSES
             * (recursive - the unroll; a param-mutator) still registers. */
            if (block_inlinable_decl(fd) && !funcs.count(fd->id->uid))
                add_unique(block_funcs, fd);
        }

        /* REPL: register earlier inputs' functions (their template/spec
         * instances too) so a call to one inlines/specializes ACROSS inputs -
         * the inliner only ever CLONES a callee body, so reusing a prior
         * input's retained decl is safe. Only for names this input did NOT
         * define, so a current redefinition wins (and a redirected call already
         * points at the current input's own instance). */
        if (prior_scope) {
            std::vector<std::pair<const UniqueId *, const LValue *>> syms;
            prior_scope->collect_symbols(syms);
            for (const auto &kv : syms) {
                if (funcs.count(kv.first) || spec_funcs.count(kv.first))
                    continue;
                const EvalValue &v = kv.second->get();
                if (!v.is<intrusive_ptr<FuncObject>>())
                    continue;
                FuncDeclStmt *fd = const_cast<FuncDeclStmt *>(
                    v.get<intrusive_ptr<FuncObject>>()->func->decl);
                /* only PURE prior functions: an impure one reads/writes mutable
                 * global state, so inlining it across inputs is unsound (the
                 * state may differ at the new site) - and its result isn't
                 * "known at compile time" anyway, so folding gains nothing. */
                if (!fd->id || !fd->desc->effective_pure)
                    continue;
                /* A prior body is POST-specialization, so it may hold a
                 * ForRangeStmt whose i_slot the inliner's substitution / tail
                 * re-resolution does not remap; don't inline/specialize such a
                 * body across inputs (it still runs correctly as a call). */
                if (has_for_range(fd->body.get()))
                    continue;
                if (inlinable_decl(fd))
                    funcs.emplace(kv.first, fd);
                else if (specializable_decl(fd))
                    spec_funcs.emplace(kv.first, fd);
                if (block_inlinable_decl(fd) && !funcs.count(kv.first))
                    block_funcs.emplace(kv.first, fd);
            }
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

        /* Simplify the int +/- chains the unroll/substitution created
         * ((n-1)-1 -> n-2), over the WHOLE tree incl. the just-registered
         * clones, so `:show fib` reads `fib(n-3)` not `fib(((n-1)-1)-1)`. Run
         * after the inline fixpoint (all levels spliced). */
        for (auto &e : root->elems)
            fold_int_arith(e);
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
        /* An EXPRESSION body: `=> expr`, now parsed as `{ return expr; }` -
         * func_expr_body looks through the sugar (and matches the
         * hand-written twin, deliberately: same spelling, same optimizer). */
        const Construct *eb = fd->body ? func_expr_body(fd) : nullptr;
        return eb != nullptr
            && (!fd->captures || fd->captures->elems.empty())
            /* No nested function: a closure in the body may capture this
             * function's parameters, which substitution would break. */
            && !contains_func(eb)
            /* not recursive / does not reference its own name */
            && count_uses(eb, fd->id->uid) == 0
            /* A body that REASSIGNS a SCALAR param - `x++`, `x = ...`,
             * `x += ...` where the target is the param itself - can't be
             * inlined: the param is a by-value copy, so the call leaves the
             * caller's variable untouched, but substituting the arg would
             * mutate it. (A mutation THROUGH a param - `p.f++`, `p[i]++` - is
             * NOT blocked: that already mutates the caller's object by
             * reference, so inlining gives the same effect. Tail-inline rejects
             * a reassigned param; specialization never seeds a written one.) */
            && !mutates_a_param(fd);
    }

    /*
     * A BLOCK-bodied function the inliner can splice in place via an
     * InlinedCallExpr. Like inlinable_decl but for `{...}` bodies:
     *  - RESOLVED: its params/locals are slotted, so splice_tail can substitute
     *    the params (by slot) and remap the locals into the caller's frame. (A
     *    0-param-0-local unresolved func is folded by AutoConst already, so
     *    requiring resolved loses nothing.)
     *  - non-capturing, no nested function (substitution would break a closure),
     *  - NON-recursive: a COMPLETE refs_uid check (count_uses misses uses buried
     *    inside the block); the recursion ban is lifted later for bounded
     *    unrolling,
     *  - does not reassign a scalar param,
     *  - body weight below ONE call (the benefit function - body_weight): the
     *    call overhead it removes outweighs the spliced body.
     * Locals are allowed (v2): try_inline_block remaps them, growing the caller
     * frame (capped at 64 slots).
     */
    /* Convert a guard-chain block - { if(c1) return a1; ...; return b; } - into
     * the equivalent ternary (c1 ? a1 : (... : b)), so an inlined guard body is
     * EXPRESSIBLE (real MyLang) instead of a block-with-returns sitting in
     * expression position (an InlinedCall). Returns null when the block is not a
     * pure guard chain: a leading non-if/return statement (a `var` local, a
     * loop, an arg-temp `$a = ...`), an `if` with an `else`, or an `if` whose
     * then-branch is not a single `return`. */
    static unique_ptr<Construct> guard_to_ternary(const Block *b)
    {
        if (!b || b->elems.empty())
            return nullptr;
        const size_t n = b->elems.size();
        auto *last = dynamic_cast<const ReturnStmt *>(b->elems[n - 1].get());
        if (!last || !last->elem)
            return nullptr;
        auto guard_ret = [](const Construct *thenB) -> const ReturnStmt * {
            if (auto *r = dynamic_cast<const ReturnStmt *>(thenB))
                return r;
            if (auto *blk = dynamic_cast<const Block *>(thenB))
                if (blk->elems.size() == 1)
                    return dynamic_cast<const ReturnStmt *>(
                        blk->elems[0].get());
            return nullptr;
        };
        for (size_t i = 0; i + 1 < n; i++) {
            auto *iff = dynamic_cast<const IfStmt *>(b->elems[i].get());
            if (!iff || iff->elseBlock)
                return nullptr;
            const ReturnStmt *r = guard_ret(iff->thenBlock.get());
            if (!r || !r->elem)
                return nullptr;
        }
        unique_ptr<Construct> acc = last->elem->clone();
        for (size_t i = n - 1; i-- > 0; ) {
            auto *iff = dynamic_cast<const IfStmt *>(b->elems[i].get());
            const ReturnStmt *r = guard_ret(iff->thenBlock.get());
            auto tern = make_unique<TernaryExpr>();
            tern->condExpr = iff->condExpr->clone();
            tern->thenExpr = r->elem->clone();
            tern->elseExpr = std::move(acc);
            acc = std::move(tern);
        }
        return acc;
    }

    /* A write to a resolved LOCAL slot (assignment LHS or ++/--). */
    static int count_slot_writes(Construct *c, int slot)
    {
        if (!c)
            return 0;
        int n = 0;
        if (auto *e14 = dynamic_cast<Expr14 *>(c)) {
            if (auto *id = dynamic_cast<Identifier *>(e14->lvalue.get()))
                if (id->sym.kind == SymKind::local && id->sym.slot == slot)
                    n++;
        } else if (auto *inc = dynamic_cast<IncDecExpr *>(c)) {
            if (auto *id = dynamic_cast<Identifier *>(inc->lvalue.get()))
                if (id->sym.kind == SymKind::local && id->sym.slot == slot)
                    n++;
        }
        for_each_child_slot(c,
            [&](unique_ptr<Construct> &ch) { n += count_slot_writes(ch.get(),
                                                                    slot); });
        return n;
    }

    /* Replace every READ of local `slot` with a clone of `repl` (keeping the
     * read's type hint + loc). Used after the slot's sole writer (its decl) has
     * been removed, so there are no write targets left to wrongly rewrite. */
    static void subst_local_reads(unique_ptr<Construct> &ref, int slot,
                                  const Construct *repl)
    {
        if (!ref)
            return;
        if (auto *id = dynamic_cast<Identifier *>(ref.get())) {
            if (id->sym.kind == SymKind::local && id->sym.slot == slot) {
                auto r = repl->clone();
                r->th = ref->th;
                r->start = ref->start;
                r->end = ref->end;
                ref = std::move(r);
            }
            return;
        }
        for_each_child_slot(ref.get(),
            [&](unique_ptr<Construct> &ch) { subst_local_reads(ch, slot,
                                                               repl); });
    }

    /* True if evaluating `c` could have a side effect (a call, an inlined body,
     * an assignment, or ++/--) - so it is NOT safe to re-evaluate. */
    static bool expr_has_side_effect(Construct *c)
    {
        if (!c)
            return false;
        if (dynamic_cast<CallExpr *>(c) || dynamic_cast<InlinedCallExpr *>(c)
                || dynamic_cast<IncDecExpr *>(c) || dynamic_cast<Expr14 *>(c))
            return true;
        bool se = false;
        for_each_child_slot(c,
            [&](unique_ptr<Construct> &ch) {
                if (expr_has_side_effect(ch.get())) se = true; });
        return se;
    }

    /* Copy-propagate a block's WRITE-ONCE, cheap, side-effect-free locals into
     * their uses (`var t = expr; ...t...` -> `...expr...`), then drop the decl.
     * This collapses a simple non-guard block body - the only thing that still
     * inlines as a non-expressible `InlinedCall(Block(...))` - into a guard chain
     * / single return that guard_to_ternary turns into EXPRESSIBLE code. Sound:
     * a write-once local whose rvalue is side-effect-free is just a name for that
     * expression, so substituting (and re-evaluating, for a cheap rvalue) is
     * exact. A reassigned / expensive / side-effecting local is left in place
     * (the body then stays an InlinedCall - rare, and still correct). */
    void collapse_locals(Block *body)
    {
        if (!body)
            return;
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < body->elems.size(); i++) {
                auto *e14 = dynamic_cast<Expr14 *>(body->elems[i].get());
                if (!e14 || !(e14->fl & pFlags::pInDecl)
                        || e14->op != Op::assign || !e14->rvalue)
                    continue;
                auto *lv = dynamic_cast<Identifier *>(e14->lvalue.get());
                if (!lv || lv->sym.kind != SymKind::local)
                    continue;
                if (body_weight(e14->rvalue.get()) > ARG_SUBST_MAX
                        || expr_has_side_effect(e14->rvalue.get()))
                    continue;
                const int slot = lv->sym.slot;
                if (count_slot_writes(body, slot) != 1)   /* not write-once */
                    continue;
                unique_ptr<Construct> rv = std::move(e14->rvalue);
                body->elems.erase(body->elems.begin() +
                                  static_cast<long>(i));
                for (size_t j = i; j < body->elems.size(); j++)
                    subst_local_reads(body->elems[j], slot, rv.get());
                changed = true;
                break;          /* indices shifted; restart the scan */
            }
        }
    }

    /* True if every operator in the +/- chain is + or -. */
    static bool is_addsub_chain(const MultiOpConstruct *mo)
    {
        for (size_t i = 1; i < mo->elems.size(); i++)
            if (mo->elems[i].first != Op::plus
                    && mo->elems[i].first != Op::minus)
                return false;
        return true;
    }

    /* Combine constant literals in an INT-typed +/- chain: (n-1)-1 -> n-2,
     * ((n-1)-1)-1 -> n-3. SOUND for int only - wraparound is associative mod
     * 2^64, so regrouping/reordering +/- is exact under -fwrapv - hence GATED on
     * th == i: a float chain (non-associative rounding) or a string `+` (concat)
     * is never touched. Bottom-up, so a nested chain folds before its parent.
     * This is what makes an unrolled recursion render `fib(n-3)` instead of the
     * literal `fib(((n-1)-1)-1)` the substitution produces. */
    /* Structural equality of two SIDE-EFFECT-FREE expressions (used to collect
     * like terms). Conservative: identifiers (same resolution), int literals,
     * and arithmetic chains over those; anything else compares unequal. */
    static bool expr_equal(const Construct *a, const Construct *b)
    {
        if (a == b)
            return true;
        if (!a || !b)
            return false;
        if (auto *ia = dynamic_cast<const Identifier *>(a)) {
            auto *ib = dynamic_cast<const Identifier *>(b);
            return ib && ia->uid == ib->uid
                && ia->sym.kind == ib->sym.kind
                && ia->sym.slot == ib->sym.slot;
        }
        if (auto *la = dynamic_cast<const LiteralInt *>(a)) {
            auto *lb = dynamic_cast<const LiteralInt *>(b);
            return lb && la->ival() == lb->ival();
        }
        if (auto *ma = dynamic_cast<const MultiOpConstruct *>(a)) {
            auto *mb = dynamic_cast<const MultiOpConstruct *>(b);
            if (!mb || ma->elems.size() != mb->elems.size())
                return false;
            for (size_t i = 0; i < ma->elems.size(); i++)
                if (ma->elems[i].first != mb->elems[i].first
                        || !expr_equal(ma->elems[i].second.get(),
                                       mb->elems[i].second.get()))
                    return false;
            return true;
        }
        return false;
    }

    /* True if every operator in the chain is `*` (a pure multiplication chain;
     * `/` and `%` are not associative under truncating int division). */
    static bool is_mul_chain(const MultiOpConstruct *mo)
    {
        for (size_t i = 1; i < mo->elems.size(); i++)
            if (mo->elems[i].first != Op::times)
                return false;
        return true;
    }

    /* Build an Expr03 `k * e` (k a fresh int literal, the base op Op::invalid). */
    static unique_ptr<Construct> make_int_mul(int_type k, unique_ptr<Construct> e)
    {
        auto lit = make_unique<LiteralInt>(k);
        lit->th = TypeHint::i;
        auto mul = make_unique<Expr03>();
        /* carry the TERM's loc + inlined-at chain onto the synthetic node -
         * a rebuilt chain with EMPTY base fields loses the caret AND the
         * virtual backtrace frames for any error thrown through it (the
         * tw-vs-VM backtrace divergence, task #75) */
        e->copy_base_fields(*mul);
        mul->th = TypeHint::i;
        mul->elems.emplace_back(Op::invalid, std::move(lit));
        mul->elems.emplace_back(Op::times, std::move(e));
        return mul;
    }

    /*
     * Algebraic simplification of an INT-typed `+`/`-` chain: combine constant
     * literals AND collect like terms (structurally-equal, side-effect-free
     * operands) by net coefficient. So `(n-1)-1`->`n-2`, `a+a`->`2*a`,
     * `a-a`->`0`, `a+b-a`->`b`, `x+1+y+2`->`x+y+3`. SOUND for int only:
     * wraparound `+`/`-` is associative & commutative mod 2^64 under -fwrapv, so
     * regrouping/reordering and merging duplicate side-effect-free operands is
     * exact (a side-effecting operand is kept verbatim, each occurrence, so its
     * effect count is unchanged). Gated on th == i by the caller.
     */
    void fold_addsub(unique_ptr<Construct> &slot)
    {
        auto *mo = dynamic_cast<Expr04 *>(slot.get());
        if (!mo || mo->elems.size() < 2 || !is_addsub_chain(mo))
            return;

        /* FLATTEN a nested +/- base (already folded bottom-up); its signs carry
         * directly (the base has sign +). */
        if (auto *base = dynamic_cast<Expr04 *>(mo->elems[0].second.get()))
            if (mo->elems[0].second->th == TypeHint::i
                    && is_addsub_chain(base)) {
                std::vector<std::pair<Op, unique_ptr<Construct>>> merged;
                for (auto &pr : base->elems)
                    merged.push_back(std::move(pr));
                for (size_t i = 1; i < mo->elems.size(); i++)
                    merged.push_back(std::move(mo->elems[i]));
                mo->elems = std::move(merged);
            }

        /* COLLECT: a constant + signed terms (like terms merged by coefficient
         * when side-effect-free). */
        int_type constant = 0;
        struct STerm { unique_ptr<Construct> expr; int_type coeff; bool grp; };
        std::vector<STerm> terms;
        auto add = [&](unique_ptr<Construct> e, int_type sign) {
            if (auto *li = dynamic_cast<LiteralInt *>(e.get())) {
                constant += sign * li->ival();
                return;
            }
            const bool grp = !expr_has_side_effect(e.get());
            if (grp)
                for (auto &t : terms)
                    if (t.grp && expr_equal(t.expr.get(), e.get())) {
                        t.coeff += sign;
                        return;
                    }
            terms.push_back({ std::move(e), sign, grp });
        };
        add(std::move(mo->elems[0].second), 1);
        for (size_t i = 1; i < mo->elems.size(); i++)
            add(std::move(mo->elems[i].second),
                mo->elems[i].first == Op::minus ? -1 : 1);

        /* nothing merged/cancelled and no flatten benefit: leave it (the common
         * case is already handled, and this avoids rebuilding identical trees) */

        /* OUT: each non-zero term as (sign, expr-or-(|c|*expr)); then the
         * constant. */
        std::vector<std::pair<int_type, unique_ptr<Construct>>> out;
        for (auto &t : terms) {
            if (t.coeff == 0)
                continue;
            const int_type s = t.coeff > 0 ? 1 : -1;
            const int_type mag = t.coeff > 0 ? t.coeff : -t.coeff;
            out.emplace_back(s, mag == 1 ? std::move(t.expr)
                                         : make_int_mul(mag, std::move(t.expr)));
        }
        if (constant != 0) {
            auto lit = make_unique<LiteralInt>(
                constant < 0 ? -constant : constant);
            lit->th = TypeHint::i;
            out.emplace_back(constant < 0 ? -1 : 1, std::move(lit));
        }

        /* REBUILD a +/- chain. The base (elems[0]) must be positive (op
         * Op::invalid); if none is positive, lead with a literal 0. */
        if (out.empty()) {
            auto z = make_unique<LiteralInt>(0);
            z->th = TypeHint::i;
            slot = std::move(z);
            return;
        }
        int base_i = -1;
        for (size_t i = 0; i < out.size(); i++)
            if (out[i].first > 0) { base_i = static_cast<int>(i); break; }

        auto chain = make_unique<Expr04>();
        /* base fields from the ORIGINAL chain (loc + inline_ctx - see the
         * make_int_mul note; slot is still the original node here) */
        slot->copy_base_fields(*chain);
        chain->th = TypeHint::i;
        if (base_i < 0) {                    /* all negative: 0 - a - b ... */
            auto z = make_unique<LiteralInt>(0);
            z->th = TypeHint::i;
            chain->elems.emplace_back(Op::invalid, std::move(z));
        } else {
            chain->elems.emplace_back(Op::invalid, std::move(out[base_i].second));
        }
        for (size_t i = 0; i < out.size(); i++) {
            if (static_cast<int>(i) == base_i)
                continue;
            chain->elems.emplace_back(out[i].first > 0 ? Op::plus : Op::minus,
                                      std::move(out[i].second));
        }
        if (chain->elems.size() == 1)
            slot = std::move(chain->elems[0].second);   /* a single positive term */
        else
            slot = std::move(chain);
    }

    /*
     * Algebraic simplification of an INT-typed pure `*` chain: combine the
     * constant FACTORS into one product. `x*2*3`->`x*6`, `x*1`->`x`,
     * `x*0`->`0` (only when the non-const factors are side-effect-free, so
     * dropping them is sound). SOUND for int: `*` is associative & commutative
     * mod 2^64 under -fwrapv. Gated on th == i by the caller.
     */
    void fold_mul(unique_ptr<Construct> &slot)
    {
        auto *mo = dynamic_cast<Expr03 *>(slot.get());
        if (!mo || mo->elems.size() < 2 || !is_mul_chain(mo))
            return;

        if (auto *base = dynamic_cast<Expr03 *>(mo->elems[0].second.get()))
            if (mo->elems[0].second->th == TypeHint::i && is_mul_chain(base)) {
                std::vector<std::pair<Op, unique_ptr<Construct>>> merged;
                for (auto &pr : base->elems)
                    merged.push_back(std::move(pr));
                for (size_t i = 1; i < mo->elems.size(); i++)
                    merged.push_back(std::move(mo->elems[i]));
                mo->elems = std::move(merged);
            }

        int_type product = 1;
        std::vector<unique_ptr<Construct>> factors;     /* non-constant */
        bool side_effects = false;
        for (auto &pr : mo->elems) {
            if (auto *li = dynamic_cast<LiteralInt *>(pr.second.get()))
                product *= li->ival();
            else {
                if (expr_has_side_effect(pr.second.get()))
                    side_effects = true;
                factors.push_back(std::move(pr.second));
            }
        }

        if (product == 0 && !side_effects) {            /* x * 0 -> 0 */
            auto z = make_unique<LiteralInt>(0);
            z->th = TypeHint::i;
            slot = std::move(z);
            return;
        }
        if (factors.empty())                            /* all constant */
            return;                                     /* const-fold owns this */

        /* rebuild factor0 (* factor1 ...) (* product) */
        auto chain = make_unique<Expr03>();
        slot->copy_base_fields(*chain);      /* loc + inline_ctx (see above) */
        chain->th = TypeHint::i;
        chain->elems.emplace_back(Op::invalid, std::move(factors[0]));
        for (size_t i = 1; i < factors.size(); i++)
            chain->elems.emplace_back(Op::times, std::move(factors[i]));
        if (product != 1)
            chain->elems.emplace_back(Op::times,
                [&] { auto l = make_unique<LiteralInt>(product);
                      l->th = TypeHint::i; return l; }());
        if (chain->elems.size() == 1)
            slot = std::move(chain->elems[0].second);        /* x * 1 -> x */
        else
            slot = std::move(chain);
    }

    /* Int-arithmetic algebraic simplification: combine constants, collect like
     * terms in +/- chains, and combine constant factors in * chains. Bottom-up,
     * so a nested chain is normalized before its parent. Gated on th == i (sound
     * for int only; see fold_addsub / fold_mul). */
    void fold_int_arith(unique_ptr<Construct> &slot)
    {
        if (!slot)
            return;
        for_each_child_slot(slot.get(),
            [&](unique_ptr<Construct> &ch) { fold_int_arith(ch); });

        if (slot->th != TypeHint::i)
            return;
        if (dynamic_cast<Expr04 *>(slot.get()))
            fold_addsub(slot);
        else if (dynamic_cast<Expr03 *>(slot.get()))
            fold_mul(slot);
    }

    bool block_inlinable_decl(const FuncDeclStmt *fd)
    {
        if (!fd->body || !fd->body->is_block() || !fd->id
                || !fd->desc->resolved)
            return false;
        if (!(!fd->captures || fd->captures->elems.empty())
                || contains_func(fd->body.get())
                || mutates_a_param(fd))
            return false;
        /*
         * A self-recursive func is inlinable only if it is TREE-recursive and
         * pure (func_is_cacheable_recursive) AND big enough to unroll at least
         * one level within the weight budget (rec_unroll_depth > 0): the inliner
         * UNROLLS it that many levels and the per-frame cache dedups the
         * duplicate self-calls (the v3 fib win). A huge recursive body unrolls
         * to 0 levels and is left a plain call (no benefit, just bloat). A
         * non-recursive func uses the cost-model weight gate (below one call).
         */
        if (refs_uid(fd->body.get(), fd->id->uid))
            return func_is_cacheable_recursive(fd) && rec_unroll_depth(fd) > 0;
        return body_weight(fd->body.get()) < CALL_WEIGHT;
    }

    /* True if the lvalue is DIRECTLY a param identifier (`x`), not a field /
     * element of one (`x.f`, `x[i]`). Only a direct reassignment is unsound to
     * inline; a through-the-param mutation has reference semantics. */
    static bool target_is_param(const Construct *lv,
            const std::unordered_set<const UniqueId *> &params)
    {
        auto *id = dynamic_cast<const Identifier *>(lv);
        return id && params.count(id->uid) != 0;
    }

    static bool body_mutates_param(const Construct *c,
            const std::unordered_set<const UniqueId *> &params)
    {
        if (!c)
            return false;
        if (auto *idc = dynamic_cast<const IncDecExpr *>(c))
            if (target_is_param(idc->lvalue.get(), params))
                return true;
        if (auto *e = dynamic_cast<const Expr14 *>(c))
            if (!(e->fl & pFlags::pInDecl)
                    && target_is_param(e->lvalue.get(), params))
                return true;
        bool found = false;
        for_each_child(const_cast<Construct *>(c),
            [&](Construct *ch) {
                if (body_mutates_param(ch, params)) found = true;
            });
        return found;
    }

    static bool mutates_a_param(const FuncDeclStmt *fd)
    {
        if (!fd->params)
            return false;
        std::unordered_set<const UniqueId *> params;
        for (auto &p : fd->params->elems)
            params.insert(p->uid);
        return body_mutates_param(fd->body.get(), params);
    }

    /* A COMPLETE search for a nested function (descends into the containers
     * for_each_child stops at - Block/Expr14/for/foreach/try). The plain
     * for_each_child form MISSED a closure that is the rvalue of a decl
     * (`var f = func[..]..;`), which let try_inline_block wrongly inline a body
     * whose closure captured a param - a real bug v2 exposed once bodies with
     * locals became eligible. */
    static bool contains_func(const Construct *c)
    {
        if (!c)
            return false;
        if (dynamic_cast<const FuncDeclStmt *>(c))
            return true;
        if (auto *b = dynamic_cast<const Block *>(c)) {
            for (auto &e : b->elems)
                if (contains_func(e.get())) return true;
            return false;
        }
        if (auto *f = dynamic_cast<const ForStmt *>(c))
            return contains_func(f->init.get()) || contains_func(f->cond.get())
                || contains_func(f->inc.get()) || contains_func(f->body.get());
        if (auto *fe = dynamic_cast<const ForeachStmt *>(c))
            return contains_func(fe->container.get())
                || contains_func(fe->body.get());
        if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
            if (contains_func(tc->tryBody.get())) return true;
            for (auto &p : tc->catchStmts)
                if (contains_func(p.second.get())) return true;
            return contains_func(tc->finallyBody.get());
        }
        if (auto *e = dynamic_cast<const Expr14 *>(c))
            return contains_func(e->lvalue.get())
                || contains_func(e->rvalue.get());
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
    void walk(unique_ptr<Construct> &slot, int depth, int *fsize,
              bool no_block = false)
    {
        if (!slot)
            return;

        /* A nested function owns a separate frame: walk its body with that
         * function's frame size (or no frame if unresolved). Its name, params
         * and capture list hold no calls. */
        if (auto *fd = dynamic_cast<FuncDeclStmt *>(slot.get())) {
            if (fd->body)
                walk(fd->body, depth,
                     fd->desc->resolved ? &fd->desc->frame_size : nullptr);
            return;
        }

        /*
         * A loop CONDITION: walk it with block-inline SUPPRESSED. The
         * specialize_types for-range pass (run later) caches a pure-call bound -
         * `for (i; i < f(n); i++)` evaluates f(n) ONCE - but it only recognizes
         * the call shape, not an opaque InlinedCallExpr; block-inlining f(n)
         * first would turn the ForRangeStmt into a per-iteration ForStmt (a
         * regression). Leaving the call there costs nothing (a plain loop's cond
         * runs every iteration anyway) and lets for-range claim it. Expression-
         * body inlining is NOT suppressed: it yields a for-range-recognizable
         * arithmetic expression, not an InlinedCallExpr.
         */
        if (auto *fs = dynamic_cast<ForStmt *>(slot.get())) {
            if (fs->init) walk(fs->init, depth, fsize, no_block);
            if (fs->cond) walk(fs->cond, depth, fsize, /*no_block*/true);
            if (fs->inc)  walk(fs->inc,  depth, fsize, no_block);
            if (fs->body) walk(fs->body, depth, fsize, no_block);
            return;
        }
        if (auto *ws = dynamic_cast<WhileStmt *>(slot.get())) {
            if (ws->condExpr) walk(ws->condExpr, depth, fsize, /*no_block*/true);
            if (ws->body)     walk(ws->body, depth, fsize, no_block);
            return;
        }

        /* Recurse into children at the same splice depth. */
        for_each_child_slot(slot.get(),
            [&](unique_ptr<Construct> &ch) { walk(ch, depth, fsize, no_block); });

        try_inline(slot, depth, fsize);   /* re-scans its splice (depth + 1) */
        try_inline_tail(slot, depth, fsize);   /* tail call to a block func */
        if (!no_block)
            try_inline_block(slot, depth, fsize); /* non-tail block func */
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

        /* The body EXPRESSION (the splice source): the `=> expr` sugar's
         * inner expr - the Block/Return wrapper is not spliced (it would put
         * a statement in expression position) and is not counted (the size
         * gate measures the expression, exactly as before the desugar).
         *
         * NULL is possible (#150): registration checked the body at run()
         * time, but a LATER pass of this same walk can TAIL-SPLICE a
         * callee into the registered function's own body - `{ return
         * aa(k); }` becomes `{ <aa's block> }`, no longer a single-return
         * shape - so the funcs entry goes stale. Seen with a lambda
         * template instance whose body tail-called one of a mutually-
         * recursive pair (mutual recursion is what makes the callee
         * tail-inlinable but not expr-inlinable). DECLINE, never assert:
         * the call stays a runtime call, which is always correct. */
        Construct *fexpr = func_expr_body(f);
        if (!fexpr)
            return;

        const int bsz = node_count(fexpr);
        if (bsz > max_nodes)
            return;

        for (size_t i = 0; i < nparams; i++) {
            const int uses = count_uses(fexpr,
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

        unique_ptr<Construct> body = fexpr->clone();

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
        slot = std::move(body);

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
                    ref = std::move(a);
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
     * Inline a NON-TAIL call to a block-bodied function (any expression
     * position) by replacing the CallExpr with an InlinedCallExpr: the callee's
     * cloned, param-substituted body runs behind its own FlowState boundary, so
     * its `return`s yield the call's value instead of returning from the caller.
     * No statement hoisting and no eval-order change (the node sits exactly
     * where the call was). Locals are remapped into the caller frame
     * (splice_tail); args are substituted directly when value-stable, else bound
     * to a fresh frame temp once ("args as locals" - see below). Bounded by
     * depth + inline_budget like try_inline; the spliced body is re-scanned
     * (depth+1) so nested calls collapse too.
     */
    void try_inline_block(unique_ptr<Construct> &slot, int depth, int *fsize)
    {
        auto *ce = dynamic_cast<CallExpr *>(slot.get());
        if (!ce)
            return;

        auto *callee = dynamic_cast<Identifier *>(ce->what.get());
        if (!callee || callee->sym.kind == SymKind::local)
            return;

        auto it = block_funcs.find(callee->uid);
        if (it == block_funcs.end() || !it->second)
            return;

        FuncDeclStmt *f = it->second;
        if (!f->desc->resolved)
            return;       /* its locals aren't slotted: nothing to remap into */

        /*
         * A SELF-RECURSIVE callee (block_inlinable_decl admitted it only if it
         * is tree-recursive + pure) is UNROLLED to `rec_unroll_depth(f)` levels
         * (chosen per-func from the weight budget). The bound is `rec_depth`
         * (this call's self-inline depth along the walk path), NOT body size: a
         * size cap stops mid-level and leaves some self-calls un-expanded
         * (LOPSIDED, dedups far worse), while a depth cap expands EVERY self-call
         * to the same depth (balanced) regardless of how many a body has. Each
         * self-call splices a clone of the ORIGINAL body (`rec_orig`, saved
         * before any unroll), NOT the in-place-growing body, so the unroll does
         * not COMPOUND. `REC_NODE_CAP` is a high backstop for a pathological
         * body. The duplicate frontier self-calls hit the per-frame pure-call
         * cache (cache_results).
         */
        const bool is_rec = refs_uid(f->body.get(), f->id->uid);
        if (is_rec) {
            if (rec_depth >= rec_unroll_depth(f)
                    || count_all_nodes(f->body.get()) >= REC_NODE_CAP)
                return;
            if (!rec_orig.count(f))
                rec_orig[f] = f->body->clone();
            f->desc->cache_results = true;
        }

        const int nparams = f->params
            ? static_cast<int>(f->params->elems.size()) : 0;
        if (static_cast<int>(ce->args->elems.size()) != nparams)
            return;   /* arity mismatch: let the runtime error survive */

        /*
         * Decide each param. A param REASSIGNED in the body can't be inlined (it
         * is a by-value copy; substituting the arg would wrongly alias it).
         * Otherwise: a VALUE-STABLE arg (`tail_arg_ok` - a caller LOCAL or const
         * literal the body can't reassign) is substituted DIRECTLY (cheap, read
         * at each param use). Any other arg (a non-trivial expression, a global,
         * a side-effecting call) is bound to a fresh frame TEMP once at the top
         * of the body (`$a = arg`) and the param reads the temp - "args as
         * locals". Evaluating once captures the call-time value, so a body that
         * mutates the global / a multi-use side-effecting arg stays sound; it
         * also lets `f(a+b)`, `f(g())`, `f(global)` inline (and is what the
         * recursion-unroll needs, since a self-call's arg is `n-1`). Use count
         * is by SLOT so a shadowing local doesn't mislead.
         */
        std::vector<bool> needs_temp(nparams, false);
        int ntemps = 0;
        for (int i = 0; i < nparams; i++) {
            if (i < static_cast<int>(f->slot_writes.size())
                    && f->slot_writes[i] != 0)
                return;       /* reassigned param: not inlinable */
            const int uses = count_matching(f->body.get(),
                [&](const Identifier *id) {
                    return id->sym.kind == SymKind::local
                        && id->sym.slot == i;
                });
            const Construct *arg = ce->args->elems[i].get();
            /* A cheap arg to a PURE callee is direct-substituted (no temp): the
             * callee's purity makes the arg side-effect-free, so re-evaluating
             * it at each param use is sound, and a temp-free body can become an
             * expressible ternary. Otherwise temp-bind (capture-once). */
            const bool subst = tail_arg_ok(uses, arg)
                || (f->desc->effective_pure
                    && body_weight(arg) <= ARG_SUBST_MAX);
            if (!subst) {
                needs_temp[i] = true;
                ntemps++;
            }
        }

        /* The body's locals are remapped into a fresh range at the top of the
         * caller's frame, and the arg temps above those; the frame grows by
         * both (capped at 64). With no caller frame (fsize null) nothing that
         * needs a slot can be inlined. */
        const int nlocals = f->desc->frame_size - nparams;
        const int grow = nlocals + ntemps;
        if (grow > 0 && !fsize)
            return;
        if (fsize && *fsize + grow > MAX_FRAME_SLOTS)
            return;       /* would overflow the 64-slot frame */

        const int bsz = count_all_nodes(f->body.get());
        if (depth >= MAX_INLINE_DEPTH || bsz > inline_budget)
            return;
        inline_budget -= bsz;

        /* Splice: substitute params (by slot) + remap locals to [*fsize, ...).
         * For a temp-bound param, the substituted value is a read of its temp
         * slot (allocated above the remapped locals). */
        const int off = fsize ? (*fsize - nparams) : 0;
        const int temp_base = fsize ? (*fsize + nlocals) : 0;
        std::vector<unique_ptr<Identifier>> temp_reads;  /* own the substitutes */
        std::vector<Construct *> args;
        int t = 0;
        for (int i = 0; i < nparams; i++) {
            if (needs_temp[i]) {
                /* name by slot ($a<slot>) so nested arg-temps don't collide in
                 * :show (the slot is what matters; the name is cosmetic) */
                auto id = make_unique<Identifier>(
                    "$a" + std::to_string(temp_base + t));
                id->sym = ResolvedSym{ SymKind::local, temp_base + t };
                id->th = ce->args->elems[i]->th;   /* keep M8 hint if any */
                temp_reads.push_back(std::move(id));
                args.push_back(temp_reads.back().get());
                t++;
            } else {
                args.push_back(ce->args->elems[i].get());
            }
        }

        /* For a recursive callee, splice the ORIGINAL (pre-unroll) body so the
         * in-place decl unroll does not compound; otherwise the current body. */
        unique_ptr<Construct> body =
            is_rec ? rec_orig.at(f)->clone() : f->body->clone();
        splice_tail(body, args, nparams, off);

        /* Prepend `$a = arg` for each temp-bound param, in PARAM ORDER so the
         * args evaluate left-to-right (insert at front in reverse). The arg is
         * the ORIGINAL caller expression (not remapped). */
        auto *blk = dynamic_cast<Block *>(body.get());
        ML_CHECK(blk != nullptr);   /* block_inlinable_decl required is_block */
        t = ntemps;
        for (int i = nparams - 1; i >= 0; i--) {
            if (!needs_temp[i])
                continue;
            t--;
            auto asn = make_unique<Expr14>();
            asn->op = Op::assign;
            auto lv = make_unique<Identifier>(
                "$a" + std::to_string(temp_base + t));
            lv->sym = ResolvedSym{ SymKind::local, temp_base + t };
            lv->th = ce->args->elems[i]->th;
            asn->lvalue = std::move(lv);
            asn->rvalue = ce->args->elems[i]->clone();
            blk->elems.insert(blk->elems.begin(), std::move(asn));
        }

        if (fsize)
            *fsize += grow;   /* the caller's frame absorbed locals + arg temps */

        const InlineCtx *ic = alloc_inline_ctx(
            { std::string(f->id->get_str()), param_names(f),
              ce->start, ce->inline_ctx });

        /* If the spliced body is a temp-free guard chain, emit an EXPRESSIBLE
         * ternary instead of an InlinedCall(Block(...return...)). The ternary is
         * a normal expression - it runs in the caller's frame (no flow boundary)
         * and the frontier self-calls in its branches still hit the per-frame
         * cache. Otherwise keep the InlinedCall (a body with locals / arg temps
         * / a loop is not yet expression-convertible). */
        unique_ptr<Construct> spliced;
        if (ntemps == 0) {
            /* copy-propagate write-once cheap locals so a simple body collapses
             * to a guard chain / single return -> an expressible ternary instead
             * of a non-expressible InlinedCall */
            collapse_locals(blk);
            if (auto tern = guard_to_ternary(blk)) {
                ce->copy_base_fields(*tern);   /* loc + th of the call site */
                tag_inline(tern.get(), ic);
                spliced = std::move(tern);
            }
        }
        if (!spliced) {
            tag_inline(body.get(), ic);
            auto ica = make_unique<InlinedCallExpr>();
            ce->copy_base_fields(*ica);   /* loc + inline_ctx of the call site */
            ica->elem = std::move(body);
            spliced = std::move(ica);
        }

        if (analysis)
            analysis->mark(callee->start,
                static_cast<int>(callee->get_str().length()),
                AnnoKind::inlined);

        TRACE(inlining, 0, std::string(f->id->get_str()) + "(" +
              std::to_string(nparams) + " arg(s))  block body " +
              std::to_string(bsz) + " nodes -> inline (+" +
              std::to_string(nlocals) + " local, +" +
              std::to_string(ntemps) + " arg-temp slot(s))");

        slot = std::move(spliced);
        /* Re-scan to collapse nested calls in the spliced body. For a recursive
         * self-inline, bump rec_depth around the re-scan so the next level's
         * self-calls see the deeper bound (and stop at REC_UNROLL_LEVELS). Every
         * self-call at a given level is expanded (the re-scan visits them all),
         * so the unroll stays BALANCED; the depth cap keeps it finite. */
        if (is_rec) {
            rec_depth++;
            walk(slot, depth + 1, fsize);
            rec_depth--;
        } else {
            walk(slot, depth + 1, fsize);
        }
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

        if (!f->desc->resolved)
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

        const int nlocals = f->desc->frame_size - nparams;
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

        slot = std::move(spliced);   /* the ReturnStmt becomes f's (spliced) body */
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
     * The inlining COST MODEL (the benefit function). A flat node count is the
     * wrong size metric: fib's body is dominated by its two CALLS, which are
     * ~20x an arith op, so a body containing a call must weigh much more than
     * one of plain arithmetic. node_weight gives each node type its measured
     * eval cost (xId-read), from the `--weights` calibration (run_weight_bench,
     * eval.cpp). body_weight sums them over the whole subtree; the inliner
     * splices a block body when its weight is below ONE call (CALL_WEIGHT).
     * Re-derive the numbers with `--weights` when the interpreter changes; the
     * benefit function itself is backend-independent (it carries over to the
     * eventual bytecode VM, only the weights change).
     */
    static const int CALL_WEIGHT = 21;   /* a 2-param call, ~21 id-reads */

    static int node_weight(const Construct *c)
    {
        if (dynamic_cast<const CallExpr *>(c))    return CALL_WEIGHT; /* +Direct*/
        if (dynamic_cast<const Expr14 *>(c))      return 11;  /* assignment    */
        if (dynamic_cast<const IfStmt *>(c))      return 7;
        if (dynamic_cast<const ReturnStmt *>(c))  return 3;
        /* id / literal / arith / compare / structural: ~1 */
        return 1;
    }

    /* Complete weighted sum over the subtree (descends into the containers
     * for_each_child stops at, like count_all_nodes). */
    static int body_weight(const Construct *c)
    {
        if (!c)
            return 0;
        int w = node_weight(c);
        if (auto *b = dynamic_cast<const Block *>(c)) {
            for (auto &e : b->elems) w += body_weight(e.get());
            return w;
        }
        if (auto *f = dynamic_cast<const ForStmt *>(c))
            return w + body_weight(f->init.get()) + body_weight(f->cond.get())
                 + body_weight(f->inc.get()) + body_weight(f->body.get());
        if (auto *fe = dynamic_cast<const ForeachStmt *>(c))
            return w + body_weight(fe->container.get())
                 + body_weight(fe->body.get());
        if (auto *tc = dynamic_cast<const TryCatchStmt *>(c)) {
            w += body_weight(tc->tryBody.get());
            for (auto &p : tc->catchStmts) w += body_weight(p.second.get());
            return w + body_weight(tc->finallyBody.get());
        }
        if (auto *e = dynamic_cast<const Expr14 *>(c))
            return w + body_weight(e->lvalue.get())
                 + body_weight(e->rvalue.get());
        for_each_child(const_cast<Construct *>(c),
            [&](Construct *ch) { w += body_weight(ch); });
        return w;
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
            || dynamic_cast<const TypedScalarExpr *>(c)  /* M8: cross-input */
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
            /* NEVER fold a LAZY builtin (defined/isconst/isconstdecl): its
             * arg is a NODE property. `defined(g)` in particular TOLERATES an
             * UndefinedId arg, so a cctx eval "succeeds" - answering false at
             * compile time for a global whose definedness is a RUNTIME,
             * execution-order-dependent property. (An inlined `return
             * defined(gg)` body exposed this; the resolver's try_fold_defined
             * already folded every soundly-foldable case.) */
            if (callee && is_lazy_builtin(callee->uid))
                return;
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
                slot = std::move(lit);
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
        if (!f->desc->resolved)
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
                        seed[static_cast<int>(i)] = std::move(v);
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

        /* Redirect to the clone (same args; the const ones are now ignored).
         * Carry the clone's resolved sym onto the new callee so the call reads
         * the clone's slot directly (no map walk); unresolved in the REPL. */
        auto what = make_unique<Identifier>(clone->id->get_str());
        what->start = ce->what->start;
        what->end = ce->what->end;
        what->sym = clone->id->sym;
        ce->what = std::move(what);

        /* Re-point the devirtualized-call slot at the CLONE (the resolver set it
         * to the original function before this redirect). Script mode: the clone
         * has a global slot; REPL: it is map-resident, so disable the fast path. */
        ce->direct_func_slot =
            clone->id->sym.kind == SymKind::global ? clone->id->sym.slot : -1;
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
        const std::string base = !f->desc->display_name.empty()
            ? f->desc->display_name : std::string(f->id->get_str());
        fc->desc->display_name = base;
        fc->id = make_unique<Identifier>(
            base + "$s" + std::to_string(spec_counter++));
        fc->sync_params();   /* re-snapshot desc->name from the synthetic id */

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

        /*
         * Give the clone's name a GLOBAL-table slot (script mode), so its decl
         * binds a slot - not the map - and the redirected call reads a slot.
         * This keeps the script-runtime symbols map EMPTY (the clone is inserted
         * after the resolver's hoist, so it would otherwise fall back to the
         * map). In the REPL top-level names stay map-resident (no global table),
         * so the clone is left unresolved and binds via the map there.
         */
        if (!repl_mode && root_block) {
            const int slot =
                static_cast<int>(root_block->global_func_names.size());
            root_block->global_func_names.push_back(fc->id->uid);
            fc->id->sym = ResolvedSym{ SymKind::global, slot };
        }

        FuncDeclStmt *raw = fc;
        new_funcs.push_back(std::move(clone));
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
                slot = std::move(a);
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
/*
 * Call devirtualization: replace each CallExpr whose callee resolved to a
 * global-table slot (direct_func_slot >= 0) with a DirectCallExpr, whose
 * do_eval reads the callee straight from the slot. A SEPARATE node (not a flag
 * on CallExpr's hot do_eval) so plain calls - builtin / closure / lambda - are
 * not perturbed. Slot-based (for_each_child_slot yields replaceable slots), so
 * the node is swapped in place. Runs after the inliner, so it also covers the
 * inliner's spec clones and their redirected calls.
 */
static void
devirtualize_calls(unique_ptr<Construct> &slot,
                   const std::unordered_set<int> &cacheable)
{
    if (!slot)
        return;

    if (auto *call = dynamic_cast<CallExpr *>(slot.get())) {
        /* Skip an already-specialized node (idempotent). */
        if (!dynamic_cast<DirectCallExpr *>(call) &&
            !dynamic_cast<DirectBuiltinCallExpr *>(call)) {

            auto *id = dynamic_cast<Identifier *>(call->what.get());

            if (id && id->sym.kind == SymKind::builtin) {
                /* Unshadowed builtin: bake its (immutable, singleton-table)
                 * function pointer. Never set in the REPL, where builtins stay
                 * map-resident (sym.kind isn't builtin). */
                auto d = make_unique<DirectBuiltinCallExpr>();
                call->copy_base_fields(*d);
                call->copy_call_fields(*d);   /* incl. tq_folded, vm_len_kind */
                d->lvalue_arg0 = is_lvalue_arg_builtin(id->get_str());
                d->lvalue_rest_native =
                    is_lvalue_rest_native_builtin(id->get_str());
                d->lvalue_rest_capable =
                    is_lvalue_rest_capable_builtin(id->get_str());
                d->map_filter_kind = id->get_str() == "map"    ? 1
                                   : id->get_str() == "filter" ? 2 : 0;
                d->what = std::move(call->what);
                d->args = std::move(call->args);
                d->builtin = builtin_slot(id->sym.slot).getval<Builtin>();
                slot = std::move(d);
            } else if (call->direct_func_slot >= 0) {
                /* Global-slot callee (function / struct / escaped global). A
                 * cacheable pure-recursive callee becomes a CachedCallExpr so
                 * its recursion's duplicate self-calls dedup in the per-frame
                 * cache; every other global call is a plain DirectCallExpr (no
                 * per-call cache check). */
                unique_ptr<DirectCallExpr> d =
                    cacheable.count(call->direct_func_slot)
                        ? unique_ptr<DirectCallExpr>(new CachedCallExpr())
                        : make_unique<DirectCallExpr>();
                call->copy_base_fields(*d);
                call->copy_call_fields(*d);
                d->what = std::move(call->what);
                d->args = std::move(call->args);
                slot = std::move(d);
            }
        }
    }

    for_each_child_slot(slot.get(),
        [&](unique_ptr<Construct> &ch) { devirtualize_calls(ch, cacheable); });
}

/* Collect the global-table slots of functions the inliner marked cache_results
 * (a pure, tree-recursive func it unrolled), so a call to one becomes a
 * CachedCallExpr. A complete walk (cacheable funcs may be nested). */
static void
collect_cacheable_slots(Construct *c, std::unordered_set<int> &out)
{
    if (!c)
        return;
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
        if (fd->id && fd->id->sym.kind == SymKind::global
                && fd->desc->cache_results)
            out.insert(fd->id->sym.slot);
        if (fd->body)
            collect_cacheable_slots(fd->body.get(), out);
        return;
    }
    if (auto *b = dynamic_cast<Block *>(c)) {
        for (auto &e : b->elems) collect_cacheable_slots(e.get(), out);
        return;
    }
    for_each_child(c, [&](Construct *ch) { collect_cacheable_slots(ch, out); });
}

/*
 * #93: stamp every function's per-parameter escape bits.
 *
 * ⛔ IT RUNS HERE, AT THE END OF resolve_names, AND NOT IN
 * process_function WHERE ITS SIBLING func_mutates_input LIVES - because
 * the answer depends on `SymKind::global`, which pass 2 has not stamped
 * yet when a body is walked. A first version did stamp it there and was
 * caught by the analysis' own test: `func f(a) { g = [9]; a[0] = 1; }`
 * came back with `a` marked non-escaping, because the write to the global
 * `g` still looked like a write to an unresolved name. That is the
 * audit-table stage trap in its usual shape - a pass reading a table one
 * stage before it is filled - and here the failure direction is a
 * use-after-free, so the stage matters more than the tidiness of sitting
 * next to the related analysis.
 */
#ifdef TESTS
unsigned long g_noescape_funcs = 0;    /* functions with >=1 marked param */
unsigned long g_noescape_marks = 0;    /* parameters marked, in total */
/* the DENOMINATOR, so the reach number can be read as a hit rate rather
 * than an absolute nobody can size: how many parameters were even
 * candidates (a reference param of an analysable function), and how many
 * candidate-bearing functions lost everything to the all-or-nothing
 * `unsafe` poison rather than to a per-parameter rule */
unsigned long g_noescape_cands = 0;
unsigned long g_noescape_unsafe = 0;
unsigned long g_esc_p_callee = 0, g_esc_p_gwrite = 0,
              g_esc_p_hobuiltin = 0, g_esc_p_shape = 0;
#endif

/* every named function body, plus which global slot names it and which
 * global slots some assignment writes (a slot a program reassigns cannot be
 * trusted to still hold the function whose name it is) */
static void esc_collect(Construct *c, std::vector<EscFn> &fns, EscWorld &w)
{
    if (!c)
        return;
    if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
        if (fd->desc) {
            if (fd->id && fd->id->sym.kind == SymKind::global)
                w.slot2fn[fd->id->sym.slot] = static_cast<int>(fns.size());
            w.fd2fn[fd] = static_cast<int>(fns.size());
            EscFn f;
            f.fd = fd;
            fns.push_back(f);
        }
        /*
         * ⛔ WALK THE BODY EXPLICITLY: `for_each_child` has NO FuncDeclStmt
         * arm, so the generic descent below sees a function declaration as
         * childless and this collection stopped at the TOP LEVEL. Two
         * things were wrong with that, one of them pre-dating the borrow:
         *
         *  - a LAMBDA or a nested named function never entered `fns` at
         *    all, so it could not be recognised as a callback (which is
         *    what made the higher-order-builtin rule unable to name any
         *    real callback), and its own parameters were never analysed;
         *  - `written_slots` - the reassigned-global-slot set that decides
         *    whether a call may be resolved through `slot2fn` at all - was
         *    collected from top-level statements ONLY. A function body
         *    doing `helper = other;` was invisible, so a call to `helper`
         *    elsewhere resolved to the ORIGINAL function and the fixpoint
         *    answered for a callee that may no longer be there. That is
         *    the unsound direction, and it is why this is a fix and not
         *    just an enabler.
         */
        esc_collect(fd->body.get(), fns, w);
        return;
    } else if (auto *sd = dynamic_cast<StructDeclStmt *>(c)) {
        if (sd->id && sd->id->sym.kind == SymKind::global)
            w.struct_slots.insert(sd->id->sym.slot);
    } else if (esc_writes_global(c)) {
        const Construct *lv = nullptr;
        if (auto *e = dynamic_cast<const Expr14 *>(c))
            lv = e->lvalue.get();
        else if (auto *idc = dynamic_cast<const IncDecExpr *>(c))
            lv = idc->lvalue.get();
        /* only a WHOLE-slot write can replace the function; `g[i] = v`
         * mutates the object the slot points at */
        if (auto *id = dynamic_cast<const Identifier *>(lv))
            w.written_slots.insert(id->sym.slot);
    }
    fmi_children(c, [&](Construct *ch) { esc_collect(ch, fns, w); });
}

/*
 * #93: stamp every function's per-parameter escape bits - see THE
 * PARAMETER ESCAPE ANALYSIS and THE CALL-GRAPH ESCAPE FIXPOINT above.
 *
 * ⛔ IT RUNS HERE, AT THE END OF resolve_names, AND NOT IN
 * process_function WHERE ITS SIBLING func_mutates_input LIVES - because
 * the answer depends on `SymKind::global`, which pass 2 has not stamped
 * yet when a body is walked, and (since the fixpoint) on
 * `direct_func_slot`, which `devirtualize_direct_calls` stamps one line
 * above this call. A first version did stamp it in process_function and
 * was caught by the analysis' own test: `func f(a) { g = [9]; a[0] = 1; }`
 * came back with `a` marked non-escaping, because the write to the global
 * `g` still looked like a write to an unresolved name. That is the
 * audit-table stage trap in its usual shape - a pass reading a table one
 * stage before it is filled - and here the failure direction is a
 * use-after-free, so the stage matters more than the tidiness of sitting
 * next to the related analysis.
 */
static void stamp_noescape_params(Construct *root)
{
    std::vector<EscFn> fns;
    EscWorld w;
    esc_collect(root, fns, w);

    for (EscFn &f : fns)
        esc_scan_body(f, w);

    /*
     * THE FIXPOINT. Two facts travel the call graph, and both only ever get
     * WORSE, so the iteration terminates: `unsafe` spreads from a callee to
     * its callers (whatever it may do to a global, calling it may do too),
     * and an edge "my param i is argument j of g" clears i as soon as g's
     * param j turns out to escape - or g turns out to be unsafe.
     */
    for (bool changed = true; changed; ) {
        changed = false;
        for (EscFn &f : fns) {
            for (int c : f.calls)
                if (fns[c].unsafe && !f.unsafe) {
                    f.unsafe = true;
                    changed = true;
                }
            for (const EscEdge &e : f.edges) {
                const EscFn &g = fns[e.callee];
                const bool ok = !g.unsafe
                    && e.argpos < 64
                    && (g.mask >> e.argpos & 1)
                    && !(g.written >> e.argpos & 1);
                if (!ok && (f.mask >> e.mine & 1)) {
                    f.mask &= ~(uint64_t(1) << e.mine);
                    changed = true;
                }
            }
        }
    }

    for (EscFn &f : fns) {
        f.fd->desc->noescape_params = esc_final_mask(f);
#ifdef TESTS
        for (uint64_t m = f.cands; m; m &= m - 1)
            g_noescape_cands++;
        if (f.cands && f.unsafe)
            g_noescape_unsafe++;
        if (f.fd->desc->noescape_params) {
            g_noescape_funcs++;
            for (uint64_t m = f.fd->desc->noescape_params; m; m &= m - 1)
                g_noescape_marks++;
        }
#endif
    }
}

static void
devirtualize_direct_calls(Construct *root)
{
    if (!root)
        return;
    std::unordered_set<int> cacheable;
    collect_cacheable_slots(root, cacheable);
    for_each_child_slot(root,
        [&](unique_ptr<Construct> &ch) { devirtualize_calls(ch, cacheable); });
}

void
resolve_names(Construct *root, bool enable_inline, int inline_threshold,
              AnalysisInfo *analysis, bool repl_mode, EvalContext *prior_pure)
{
    Resolver().run(root, analysis, repl_mode, prior_pure);

    if (enable_inline)
        if (auto *rb = dynamic_cast<Block *>(root))
            Inliner(inline_threshold, analysis, prior_pure, repl_mode).run(rb);

    /* Devirtualize direct (global-slot) calls into DirectCallExpr nodes. After
     * the inliner so spec clones + redirected calls are covered; before
     * specialize_types, which treats a DirectCallExpr as the CallExpr it is. */
    devirtualize_direct_calls(root);
    stamp_noescape_params(root);
}

void
run_optimizers(Construct *root, bool enable_inline, int inline_threshold,
               bool enable_specialize, bool repl_mode,
               EvalContext *prior_scope)
{
    resolve_names(root, enable_inline, inline_threshold, /*analysis=*/nullptr,
                  repl_mode, prior_scope);
    specialize_types(root, enable_specialize, prior_scope);
}

void
mark_implicit_globals(Construct *root,
                      const std::unordered_set<const UniqueId *> &known)
{
    Block *rb = dynamic_cast<Block *>(root);
    if (!rb)
        return;

    std::unordered_set<const UniqueId *> declared(known.begin(), known.end());

    /* Record a name an explicit decl / func / struct introduces, so a later
     * top-level assignment to it is recognized as an assignment, not a fresh
     * implicit var (`var a = 1; a = 2;` -> the second is an assignment). */
    auto record = [&](Construct *lv) {
        if (auto *id = dynamic_cast<Identifier *>(lv))
            declared.insert(id->uid);
        else if (auto *il = dynamic_cast<IdList *>(lv))
            for (auto &e : il->elems)
                declared.insert(e->uid);
    };

    for (auto &up : rb->elems) {
        Construct *c = up.get();

        if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {
            if (fd->id)
                declared.insert(fd->id->uid);
            continue;
        }
        if (auto *sd = dynamic_cast<StructDeclStmt *>(c)) {
            if (sd->id)
                declared.insert(sd->id->uid);
            continue;
        }

        auto *e = dynamic_cast<Expr14 *>(c);
        if (!e)
            continue;

        if (e->fl & pFlags::pInDecl) {        /* an explicit var/const decl */
            record(e->lvalue.get());
            continue;
        }

        /* A plain `name = expr` to an undeclared, non-builtin name: implicit
         * `var`. Only a bare identifier at the outermost scope qualifies. */
        if (e->op == Op::assign) {
            if (auto *id = dynamic_cast<Identifier *>(e->lvalue.get())) {
                if (!declared.count(id->uid) &&
                    builtin_slot_index(id->uid) < 0) {
                    e->fl |= pFlags::pInDecl;
                    declared.insert(id->uid);
                }
            }
        }
    }
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

            if (fd->id && fd->desc->effective_pure
                    && !fd->desc->explicit_pure)
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
