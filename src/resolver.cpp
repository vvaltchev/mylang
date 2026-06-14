/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"
#include "resolver.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

/*
 * Slice 1 of the slot-indexing optimization: resolve function PARAMETERS to
 * Frame slots. The strategy is deliberately conservative and all-or-nothing per
 * function: a function's params are slotted only if no local variable anywhere
 * in its body re-declares a param name (no shadowing) and there are no duplicate
 * params. Under that condition any body reference whose name matches a param is
 * unambiguously that param, so resolution is a trivial name match with no need
 * to track block-by-block scope. Functions that don't qualify are simply left
 * unresolved and keep using the runtime map (correct, just not optimized).
 *
 * Locals, top-level symbols, captures and builtins are intentionally NOT slotted
 * here; they remain unresolved and fall back to the EvalContext map walk. Later
 * slices can extend coverage on top of this same pass.
 */

namespace {

typedef std::unordered_set<const UniqueId *> NameSet;

/*
 * Per-function resolution state, threaded through the walk. It is only
 * meaningful while `slottable` is true; for a non-slottable function `params`
 * stays empty and every identifier in the body is left unresolved.
 */
struct FuncState {
    bool slottable = false;
    FuncDeclStmt *fd = nullptr;
    std::unordered_map<const UniqueId *, int> params;   /* param name -> slot */
};

/* Max params we slot: Frame::live is a single 64-bit word, one bit per slot. */
constexpr int MAX_SLOTS = 64;

class Resolver {

public:

    /* Resolve the whole tree, starting outside any function (cur == nullptr). */
    void run(Construct *root) { walk(root, nullptr); }

private:

    void walk(Construct *c, FuncState *cur);
    void process_function(FuncDeclStmt *fd);
    void count_param_writes(Construct *lvalue, FuncState *cur);

    /*
     * Stamp an identifier reference with its param slot when it names a param of
     * the current (slottable) function. Everything else is left unresolved, so
     * it falls back to the runtime EvalContext map lookup.
     */
    void resolve_ref(FuncState *cur, Identifier *id) const
    {
        if (!cur || !cur->slottable || !id)
            return;

        auto it = cur->params.find(id->uid);

        if (it != cur->params.end())
            id->sym = ResolvedSym{ SymKind::local, it->second };
    }
};

/*
 * Invoke `fn` on every direct child Construct of `c`. This covers the generic,
 * scope-neutral nodes; the few nodes that introduce names or scopes (Expr14
 * declarations, FuncDeclStmt) are handled explicitly by the callers, not here.
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
    } else if (auto *n = dynamic_cast<ForStmt *>(c)) {
        fn(n->init.get());
        fn(n->cond.get());
        fn(n->inc.get());
        fn(n->body.get());
    } else if (auto *n = dynamic_cast<ForeachStmt *>(c)) {
        fn(n->ids.get());
        fn(n->container.get());
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
    } else if (auto *n = dynamic_cast<TryCatchStmt *>(c)) {
        fn(n->tryBody.get());
        for (auto &p : n->catchStmts) {
            fn(p.first.exList.get());
            fn(p.first.asId.get());
            fn(p.second.get());
        }
        fn(n->finallyBody.get());
    } else if (auto *n = dynamic_cast<MultiElemConstruct<Construct> *>(c)) {
        /* Block, ExprList, LiteralArray */
        for (auto &e : n->elems) {
            fn(e.get());
        }
    } else if (auto *n = dynamic_cast<MultiElemConstruct<Identifier> *>(c)) {
        /* IdList */
        for (auto &e : n->elems) {
            fn(e.get());
        }
    } else if (auto *n = dynamic_cast<MultiElemConstruct<LiteralDictKVPair> *>(c)) {
        /* LiteralDict */
        for (auto &e : n->elems) {
            fn(e.get());
        }
    }
    /* literals, Identifier and childless constructs have no children */
}

/*
 * Record the names introduced by a declaration's lvalue into `out`. The lvalue
 * is either a single Identifier (`var x = ...`) or an IdList for a multiple
 * declaration (`var a, b = ...`).
 */
void
add_lvalue_names(Construct *lvalue, NameSet &out)
{
    if (auto *id = dynamic_cast<Identifier *>(lvalue)) {
        out.insert(id->uid);
    } else if (auto *il = dynamic_cast<IdList *>(lvalue)) {
        for (auto &id : il->elems) {
            out.insert(id->uid);
        }
    }
}

/*
 * Collect every name a function body declares as a local: var/const decls,
 * foreach loop variables, catch variables and nested function names. It does
 * NOT descend into nested function bodies - those names belong to their own
 * scope. The result is used to decide whether any local shadows a param.
 */
void
collect_decl_names(Construct *c, NameSet &out)
{
    if (!c)
        return;

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {

        /* The nested function's NAME is a local here, but its params/body are
         * a separate scope, so don't descend into them. */
        if (fd->id)
            out.insert(fd->id->uid);

        return;
    }

    if (auto *e = dynamic_cast<Expr14 *>(c)) {

        /* Only a declaration introduces names; a plain assignment does not. */
        if (e->fl & pFlags::pInDecl)
            add_lvalue_names(e->lvalue.get(), out);

        collect_decl_names(e->rvalue.get(), out);   /* rvalue may hold func exprs */
        return;
    }

    if (auto *fe = dynamic_cast<ForeachStmt *>(c)) {

        if (fe->idsVarDecl && fe->ids) {
            for (auto &id : fe->ids->elems) {
                out.insert(id->uid);
            }
        }

        collect_decl_names(fe->container.get(), out);
        collect_decl_names(fe->body.get(), out);
        return;
    }

    if (auto *tc = dynamic_cast<TryCatchStmt *>(c)) {

        collect_decl_names(tc->tryBody.get(), out);

        for (auto &p : tc->catchStmts) {
            if (p.first.asId)
                out.insert(p.first.asId->uid);
            collect_decl_names(p.second.get(), out);
        }

        collect_decl_names(tc->finallyBody.get(), out);
        return;
    }

    /* Generic node: recurse into all children. */
    for_each_child(c, [&](Construct *ch) { collect_decl_names(ch, out); });
}

/*
 * Bump the write count for any param assigned by `lvalue` (an Identifier, or an
 * IdList for multiple assignment). Only resolved params are counted; the count
 * staying 0 means the param is never reassigned in the body (write-once), which
 * is the signal a future auto-const pass will use.
 */
void
Resolver::count_param_writes(Construct *lvalue, FuncState *cur)
{
    auto bump = [&](Identifier *id) {
        if (id && id->sym.kind == SymKind::local)
            cur->fd->param_writes[id->sym.slot]++;
    };

    if (auto *id = dynamic_cast<Identifier *>(lvalue)) {
        bump(id);
    } else if (auto *il = dynamic_cast<IdList *>(lvalue)) {
        for (auto &id : il->elems) {
            bump(id.get());
        }
    }
}

/*
 * Decide whether `fd`'s params can be slotted and, if so, set it up: mark the
 * FuncDeclStmt resolved, size its Frame, allocate the per-param write counters,
 * and build the name->slot map. Then walk the body so param references get
 * stamped. Params are slotted only when there are no duplicates and no local
 * shadows a param (see the file header for why that makes resolution trivial).
 */
void
Resolver::process_function(FuncDeclStmt *fd)
{
    FuncState st;
    st.fd = fd;

    const int nparams = fd->params ? static_cast<int>(fd->params->elems.size()) : 0;

    NameSet declNames;
    if (fd->body)
        collect_decl_names(fd->body.get(), declNames);

    bool ok = nparams > 0 && nparams <= MAX_SLOTS;

    if (ok) {

        /* Reject duplicate params and params shadowed by a body-local decl. */
        NameSet seen;

        for (int i = 0; i < nparams; i++) {

            const UniqueId *p = fd->params->elems[i]->uid;

            if (!seen.insert(p).second || declNames.count(p)) {
                ok = false;
                break;
            }
        }
    }

    if (ok) {

        st.slottable = true;

        for (int i = 0; i < nparams; i++) {
            st.params[fd->params->elems[i]->uid] = i;
        }

        fd->resolved = true;
        fd->frame_size = nparams;
        fd->param_writes.assign(nparams, 0);
    }

    /* Even a non-slottable function is walked: its body may contain nested
     * functions that are themselves slottable. */
    if (fd->body)
        walk(fd->body.get(), &st);
}

/*
 * Recursive driver of the pass. `cur` is the function whose params are in scope
 * for plain identifier references (nullptr at top level). FuncDeclStmt and
 * Expr14 are special-cased because they introduce scopes / declarations; every
 * other node is traversed generically via for_each_child.
 */
void
Resolver::walk(Construct *c, FuncState *cur)
{
    if (!c)
        return;

    if (auto *fd = dynamic_cast<FuncDeclStmt *>(c)) {

        /* The capture list is evaluated in the ENCLOSING scope, so resolve it
         * against `cur` (this is what lets a closure read an enclosing param
         * slot at creation time). */
        if (fd->captures) {
            for (auto &cap : fd->captures->elems) {
                resolve_ref(cur, cap.get());
            }
        }

        process_function(fd);
        return;
    }

    if (auto *id = dynamic_cast<Identifier *>(c)) {
        resolve_ref(cur, id);
        return;
    }

    if (auto *e = dynamic_cast<Expr14 *>(c)) {

        walk(e->rvalue.get(), cur);
        walk(e->lvalue.get(), cur);

        /* A non-decl Expr14 is an assignment: count writes to any param it
         * targets (declarations introduce new locals, never write a param). */
        if (!(e->fl & pFlags::pInDecl) && cur && cur->slottable)
            count_param_writes(e->lvalue.get(), cur);

        return;
    }

    for_each_child(c, [&](Construct *ch) { walk(ch, cur); });
}

} // anonymous namespace

/*
 * Public entry point: run the name-resolution pass over the parsed tree.
 */
void
resolve_names(Construct *root)
{
    Resolver().run(root);
}
