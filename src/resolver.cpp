/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"
#include "resolver.h"
#include "errors.h"

#include <functional>
#include <unordered_set>
#include <vector>

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

namespace {

/* Max slots per frame: Frame::live is one 64-bit word, one bit per slot. */
constexpr int MAX_SLOTS = 64;

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

/*
 * Public entry point: run the name-resolution pass over the parsed tree. The
 * top-level frame size is recorded on the root Block (its slot_count); the
 * runtime builds the "main" Frame from it in Block::do_eval.
 */
void
resolve_names(Construct *root)
{
    Resolver().run(root);
}
