/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "errors.h"
#include "syntax.h"
#include "lexer.h"
#include "backtrace.h"
#include "bitops.h"
#include "vm.h"   /* Phase 4: run a function body via the bytecode VM */

#include <cmath>
#include <chrono>

using std::pair;
using std::vector;
using std::string;
using std::string_view;

/* ------------------ string un-escaping -------------- */

string
unescape_str(const string_view &v)
{
    string s;
    s.reserve(v.size());

    for (size_t i = 0; i < v.size(); i++) {

        if (v[i] == '\\') {

            /*
             * We know FOR SURE that '\' is NOT the last char in the
             * literal simply because otherwise we'll have something like
             * "xxx\" and the tokenize will accept < xxx" > instead of < xxx\ >.
             */

            switch (v[i + 1]) {

                case '\\':
                    s += '\\';
                    break;
                case '\"':
                    s += '\"';
                    break;
                case 'r':
                    s += '\r';
                    break;
                case 'n':
                    s += '\n';
                    break;
                case 't':
                    s += '\t';
                    break;
                case 'v':
                    s += '\v';
                    break;
                case 'a':
                    s += '\a';
                    break;
                case 'b':
                    s += '\b';
                    break;

                default:
                    s += v[i];
                    s += v[i + 1];
                    break;
            }

            i++;

        } else {

            s += v[i];
        }
    }

    return s;
}

/* ------------------ EvalContext ------------------ */

EvalContext::EvalContext(EvalContext *parent, bool const_ctx, bool func_ctx,
                         bool repl)
    : parent(parent)
    , const_ctx(const_ctx)
    , func_ctx(func_ctx)
    , repl_mode(parent ? parent->repl_mode : repl)
    , frame(parent ? parent->frame : nullptr)
    , gfuncs(parent ? parent->gfuncs : nullptr)
    , captures(parent ? parent->captures : nullptr)
    , flow((parent && !func_ctx) ? parent->flow : &flow_state)
    , root(parent ? parent->root : this)     /* #114 */
{
    if (const_ctx)
        scope_alive = std::make_shared<bool>(true);

    /* Load builtins into the map ONLY for a context that resolves names through
     * the map: the const-evaluator (const_builtins) and the REPL (both). A
     * SCRIPT runtime root loads NOTHING - the resolver slotted every name (incl.
     * builtins), so its map stays empty (see the emplace/lookup asserts). */
    if (!parent) {
        if (const_ctx) {
            symbols.insert(const_builtins.begin(), const_builtins.end());
        } else if (repl_mode) {
            symbols.insert(const_builtins.begin(), const_builtins.end());
            symbols.insert(builtins.begin(), builtins.end());
        }
    }
}

EvalContext::~EvalContext()
{
    if (scope_alive)
        *scope_alive = false;
}

LValue *EvalContext::lookup(const Identifier *id)
{
    return lookup(id->uid);
}

LValue *EvalContext::lookup(const UniqueId *uid)
{
    /* The map is a resolution path ONLY in const-eval and the REPL. In a script
     * it must be empty (every name slotted), so this is reached only by a
     * genuinely-undefined name and must find nothing - assert that. */
    ML_CHECK(in_const_eval() || repl_mode || symbols.empty());
    auto &&it = symbols.find(uid);

    if (it != symbols.end())
        return &it->second;

    return nullptr;
}

bool EvalContext::erase(const Identifier *id)
{
    /* Only map-resident symbols are erasable (REPL globals: the `:undef`
     * command, and the redefinition / instance-GC paths). Slotted locals and
     * global-table functions have no removal mechanism - a script has no undef,
     * and the REPL keeps its top-level names in the map. */
    const auto &it = symbols.find(id->uid);

    if (it == symbols.end())
        return false;

    symbols.erase(it);
    return true;
}

/* The map is written only during const-eval (compile-time folding) and in the
 * REPL; a SCRIPT runtime never falls back to it (every name slotted). */
void EvalContext::emplace(const Identifier *id, const EvalValue &val, bool is_const)
{
    ML_CHECK(in_const_eval() || repl_mode);
    symbols.emplace(id->uid, LValue(val, is_const));
}

void EvalContext::emplace(const Identifier *id, EvalValue &&val, bool is_const)
{
    ML_CHECK(in_const_eval() || repl_mode);
    symbols.emplace(id->uid, LValue(std::move(val), is_const));
}

void EvalContext::emplace(const std::string_view &id, EvalValue &&val, bool is_const)
{
    ML_CHECK(in_const_eval() || repl_mode);
    symbols.emplace(UniqueId::get(id), LValue(std::move(val), is_const));
}

void EvalContext::emplace(const UniqueId *uid, EvalValue &&val, bool is_const)
{
    ML_CHECK(in_const_eval() || repl_mode);
    symbols.emplace(uid, LValue(std::move(val), is_const));
}

void EvalContext::collect_symbols(
    std::vector<std::pair<const UniqueId *, const LValue *>> &out) const
{
    for (const auto &kv : symbols)
        out.emplace_back(kv.first, &kv.second);
}

/* ------------------ Constructs ------------------- */

EvalValue Construct::eval(EvalContext *ctx, bool rec) const
{
    try {

        return do_eval(ctx, rec);

    } catch (Exception &e) {

        if (!e.loc_start) {
            e.loc_start = start;
            e.loc_end = end;
        }

        /*
         * If this node was spliced in by inlining, emit its virtual
         * ("inlined-at") frames now, so the physically-absent inlined calls
         * still show in the backtrace. This is keyed off
         * `inline_origin_emitted` rather than the loc-stamp above, because many
         * errors arrive with a loc already set (a builtin call, a not-an-lvalue
         * assignment, ...) and would otherwise slip past the loc once-guard and
         * lose their frames. The innermost inlined node wins; `do_func_call`
         * sets the same flag for a real call so the CallExpr doesn't re-emit.
         */
        if (inline_ctx && !e.inline_origin_emitted) {
            flush_inline_frames(inline_ctx, e);
            e.inline_origin_emitted = true;
        }

        throw;
    }
}

LiteralStr::LiteralStr(const std::string_view &v)
    : Literal(ConstructType::lit_str)
    , value(v.empty() ? empty_str : EvalValue(SharedStr(unescape_str(v))))
{ }

/*
 * Default typed evaluation: box through eval()/RValue. Works for any node (a
 * function call, a `dyn` value, ...), so a specialized node may call them on an
 * arbitrary child. eval()'s wrapper still stamps the source loc on errors.
 */
int_type Construct::eval_int(EvalContext *ctx) const
{
    const EvalValue v = RValue(eval(ctx));
    if (v.is<bool>())
        return v.get<bool>() ? 1 : 0;     /* bool promotes to int 0/1 */
    return v.get<int_type>();
}

float_type Construct::eval_float(EvalContext *ctx) const
{
    const EvalValue v = RValue(eval(ctx));
    if (v.is<int_type>())
        return static_cast<float_type>(v.get<int_type>());
    if (v.is<bool>())
        return v.get<bool>() ? 1.0 : 0.0;
    return v.get<float_type>();
}

/* Resolved-local fast paths: read the slot's scalar directly, skipping the
 * LValue wrapper and the EvalValue copy. Falls back to the default (map walk /
 * undefined-variable error) for non-slotted or undefined symbols. */
int_type Identifier::eval_int(EvalContext *ctx) const
{
    if (sym.kind == SymKind::local && ctx->frame) {
        const LValue &lv = ctx->frame->at(sym.slot);
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1 : 0;   /* bool slot -> int 0/1 */
        return lv.getval<int_type>();
    }
    if (sym.kind == SymKind::global && ctx->gfuncs &&
        ctx->gfuncs->defined[sym.slot]) {
        const LValue &lv = ctx->gfuncs->slots[sym.slot];
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1 : 0;
        return lv.getval<int_type>();
    }
    if (sym.kind == SymKind::capture && ctx->captures) {
        const LValue &lv = (*ctx->captures)[sym.slot];
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1 : 0;
        return lv.getval<int_type>();
    }
    return Construct::eval_int(ctx);
}

float_type Identifier::eval_float(EvalContext *ctx) const
{
    if (sym.kind == SymKind::local && ctx->frame) {
        const LValue &lv = ctx->frame->at(sym.slot);
        if (lv.is<int_type>())
            return static_cast<float_type>(lv.getval<int_type>());
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1.0 : 0.0;
        return lv.getval<float_type>();
    }
    if (sym.kind == SymKind::global && ctx->gfuncs &&
        ctx->gfuncs->defined[sym.slot]) {
        const LValue &lv = ctx->gfuncs->slots[sym.slot];
        if (lv.is<int_type>())
            return static_cast<float_type>(lv.getval<int_type>());
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1.0 : 0.0;
        return lv.getval<float_type>();
    }
    if (sym.kind == SymKind::capture && ctx->captures) {
        const LValue &lv = (*ctx->captures)[sym.slot];
        if (lv.is<int_type>())
            return static_cast<float_type>(lv.getval<int_type>());
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1.0 : 0.0;
        return lv.getval<float_type>();
    }
    return Construct::eval_float(ctx);
}

EvalValue Identifier::do_eval(EvalContext *ctx, bool rec) const
{
    if (sym.kind == SymKind::local && ctx->frame) {
        /* Resolved local: an O(1) slot read instead of a scope-chain walk. The
         * slot is always bound (a local resolves to its slot only after its
         * decl - no-hoist resolution), so no liveness check is needed. */
        return EvalValue(&ctx->frame->at(sym.slot));
    }

    if (sym.kind == SymKind::global && ctx->gfuncs) {

        /* Resolved global (a top-level function): an O(1) read of the global
         * function table, reachable from any call depth - no scope-chain map
         * walk. Not-yet-defined means the decl hasn't executed yet (a call that
         * reaches the function before its definition runs). */
        if (ctx->gfuncs->defined[sym.slot])
            return EvalValue(&ctx->gfuncs->slots[sym.slot]);

        /* TDZ, runtime half (#131): the name IS declared - it has a global
         * slot - but its declaration has not run yet. That is UNBOUND, not
         * undefined, and it is a catchable RuntimeException. */
        throw UnboundSymbolEx(
            intern_msg("'" + std::string(get_str()) + "' is not bound yet"),
            start, end);
    }

    if (sym.kind == SymKind::capture && ctx->captures) {
        /* A captured outer variable: an O(1) read of the closure's per-instance
         * capture vector. The LValue * makes it assignable (a closure may
         * mutate its captured-by-value copy). Always bound (filled at closure
         * creation before the body runs). */
        return EvalValue(&(*ctx->captures)[sym.slot]);
    }

    if (sym.kind == SymKind::builtin) {
        /* An unshadowed builtin: an O(1) read of the program-wide builtin
         * table, no scope-chain map walk. The slot is is_const-flagged, so an
         * attempt to assign to it falls through to the CannotRebindBuiltinEx
         * path.
         *
         * In a const-eval context (AutoConst / the inliner's refold) only CONST
         * builtins are visible - mirroring the const EvalContext, which loads
         * only const_builtins. A runtime builtin reads as undefined here, so an
         * append()/print() call stays unfoldable (those passes catch the
         * resulting UndefinedVariableEx to keep it a runtime call). */
        if (ctx && ctx->const_ctx && !builtin_is_const(sym.slot))
            return UndefinedId{get_str()};

        return EvalValue(&builtin_slot(sym.slot));
    }

    while (ctx) {

        LValue *lval = ctx->lookup(this);

        if (lval)
            return EvalValue(lval);

        ctx = ctx->parent;

        if (!rec)
            break;
    }

    return UndefinedId{get_str()};
}

/*
 * The descriptor-based twin of Identifier::do_eval (above): read a resolved
 * symbol by kind/slot, with the same map-walk fallback (by uid) for an
 * unresolved name. Keep the two in lockstep - the FuncObject ctor's capture
 * snapshot must read a captured name EXACTLY as an Identifier read would.
 */
/*
 * #114: see the declaration. The SAME dispatch as read_sym, handing
 * back the slot instead of an EvalValue wrapping it; null where
 * read_sym would have answered `UndefinedId`.
 *
 * ⛔ THE TWO MUST AGREE, and the shape that keeps them agreeing is
 * that read_sym is written IN TERMS OF THIS ONE below - there is one
 * dispatch, not two copies free to drift. The unbound-global throw
 * lives here, so both callers get it.
 */
LValue *read_sym_lv(EvalContext *ctx, SymKind kind, int slot,
                    const UniqueId *uid)
{
    if (kind == SymKind::local && ctx->frame)
        return &ctx->frame->at(slot);

    if (kind == SymKind::global && ctx->gfuncs) {
        if (ctx->gfuncs->defined[slot])
            return &ctx->gfuncs->slots[slot];

        /* See Identifier::do_eval: declared (it has a slot) but not bound. */
        throw UnboundSymbolEx(
            intern_msg("'" + std::string(uid->val) + "' is not bound yet"));
    }

    if (kind == SymKind::capture && ctx->captures)
        return &(*ctx->captures)[slot];

    if (kind == SymKind::builtin) {
        if (ctx && ctx->const_ctx && !builtin_is_const(slot))
            return nullptr;              /* read_sym: UndefinedId */

        return &builtin_slot(slot);
    }

    while (ctx) {

        LValue *lval = ctx->lookup(uid);

        if (lval)
            return lval;

        ctx = ctx->parent;
    }

    return nullptr;                      /* read_sym: UndefinedId */
}

EvalValue read_sym(EvalContext *ctx, SymKind kind, int slot,
                   const UniqueId *uid)
{
    if (LValue *lv = read_sym_lv(ctx, kind, slot, uid))
        return EvalValue(lv);

    return UndefinedId{uid->val};
}

/*
 * return / break / continue no longer use C++ exceptions: they set the
 * EvalContext's FlowState (see eval.h) and unwind via ordinary returns.
 * rethrow is genuinely exceptional (it re-throws a real exception object from
 * inside a catch block) and stays an exception.
 */
struct RethrowEx { Loc start; Loc end; };

static inline EvalValue
do_func_return(EvalValue &&tmp, Construct *retExpr)
{
    if (tmp.is<UndefinedId>()) {
        throw UndefinedVariableEx(
            tmp.get<UndefinedId>().id,
            retExpr->start,
            retExpr->end
        );
    }

    return RValue(tmp);
}

static EvalValue coerce_to_decl_type(const EvalValue &v, DeclType dt,
                                     int site_arg = -1);

/*
 * Bind one parameter. When `frame` is set (the function was resolved), the
 * value goes into its fixed slot and the slot is marked live; otherwise it is
 * emplaced into the args context map (the unresolved / const-eval path).
 *
 * A param with an explicit numeric type coerces a widening argument to it
 * (`func f(float x); f(3)` binds 3.0), via the same rule as a typed variable.
 *
 * The param is a FuncDescriptor::ParamDesc - the descriptor's parse-time
 * snapshot of the param Identifier - so binding reads no AST node.
 *
 * `site_arg` (RULE 2, 2026-09-20): the index of the CALL-SITE ARGUMENT this
 * bind consumes, for the coercion's throw to carry (Exception::bind_arg) -
 * only the two call-site overloads (argument expressions, the VM's arg run)
 * pass it; a builtin CALLBACK's bind passes -1, because its parameter index
 * names no argument of the builtin call the caret will land on (watched:
 * `sort(a, func(int x, int y) ...)` over a dyn string carets sort's whole
 * argument list in every engine, and must keep doing so).
 */
/* RULE 1: binding none to a parameter not declared `opt` (eval.h) */
TypeErrorEx none_bind_error(const UniqueId *param, int site_arg)
{
    std::string m = "parameter '";
    m += param->val;
    m += "' is not 'opt' and cannot be none";
    TypeErrorEx ex(intern_msg(m));
    ex.bind_arg = site_arg;
    return ex;
}

void throw_none_bind(const UniqueId *param, int site_arg)
{
    throw none_bind_error(param, site_arg);
}

static inline void
bind_param(EvalContext *args_ctx,
           Frame *frame,
           int idx,
           const FuncDescriptor::ParamDesc &param,
           EvalValue val,
           bool is_const,
           int site_arg)
{
    /* RULE 1: a non-opt parameter never holds none - refused here, at the
     * bind, before any coercion (the VM's none_bind_fault walks the same
     * order). An omitted trailing parameter is `opt` by construction. */
    if (!param.opt && val.is<NoneVal>())
        throw_none_bind(param.name, site_arg);
    if (param.decl_type == DeclType::f || param.decl_type == DeclType::i)
        val = coerce_to_decl_type(val, param.decl_type, site_arg);

    if (frame) {
        frame->at(idx) = LValue(std::move(val), is_const);
    } else {
        args_ctx->emplace(param.name, std::move(val), is_const);
    }
}

/*
 * Bind call arguments to the function's parameters. There is one overload per
 * argument representation - unevaluated argument expressions (the normal call
 * path), an already-evaluated value vector, a single value, and a pair (used by
 * builtins that invoke a callback). Each evaluates/forwards the args and hands
 * the actual storage to bind_param (slot Frame when resolved, else the map).
 *
 * The caller may pass between min_required_args() and funcParams.size()
 * arguments; any trailing `opt` parameter it omits is bound to `none`.
 */
static void
do_func_bind_params(const std::vector<FuncDescriptor::ParamDesc> &funcParams,
                    const vector<unique_ptr<Construct>> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame,
                    size_t min_args)   /* precomputed: FuncDescriptor::min_args */
{
    /*
     * RULE 2: EVERY argument is evaluated, left to right, before the arity
     * check and before any bind - the order the VM's call ops have (the
     * argument run is staged before the call op runs). This loop used to
     * check the arity first and evaluate each argument just before ITS
     * bind, so a later argument's side effect was skipped when an arity
     * error or an earlier bind refused - a difference only the tree-walker
     * had. The values live in a small inline buffer: no allocation for a
     * call of up to 8 arguments.
     */
    const size_t nargs = args.size();
    EvalValue small[8];
    std::vector<EvalValue> big;
    EvalValue *vals = small;
    if (nargs > 8) {
        big.resize(nargs);
        vals = big.data();
    }
    for (size_t i = 0; i < nargs; i++)
        vals[i] = RValue(args[i]->eval(ctx));

    const size_t nparams = funcParams.size();
    if (nargs > nparams || nargs < min_args)
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++) {
        /*
         * A param's binding is const iff it was declared `const` - NOT merely
         * because we are const-evaluating. This lets a (pure) function reassign
         * its own by-value parameters during const-eval, while a `const` param
         * stays immutable everywhere.
         */
        bind_param(
            args_ctx, frame, static_cast<int>(i),
            funcParams[i],
            i < nargs ? std::move(vals[i]) : EvalValue(),
            funcParams[i].cnst,
            static_cast<int>(i)              /* a call-site bind */
        );
    }
}

static void
do_func_bind_params(const std::vector<FuncDescriptor::ParamDesc> &funcParams,
                    const vector<EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame,
                    size_t min_args)
{
    const size_t nparams = funcParams.size();
    if (args.size() > nparams || args.size() < min_args)
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++) {
        bind_param(
            args_ctx, frame, static_cast<int>(i),
            funcParams[i],
            i < args.size() ? args[i] : EvalValue(), ctx->const_ctx,
            /*site_arg=*/-1                  /* a callback's bind */
        );
    }
}

/*
 * A view over the caller's frame arg slots for the VM's native call (CallV): no
 * vector allocation and no per-arg copy - bind reads each slot's value in
 * place.
 * This is the hot recursion path (a per-call vector was a ~30% regression).
 */
struct VmArgs {
    const LValue *slots;
    size_t n;
    size_t size() const { return n; }
    const EvalValue &operator[](size_t i) const { return slots[i].get(); }
};

static void
do_func_bind_params(const std::vector<FuncDescriptor::ParamDesc> &funcParams,
                    const VmArgs &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame,
                    size_t min_args)
{
    const size_t nparams = funcParams.size();
    if (args.size() > nparams || args.size() < min_args)
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++) {
        if (i < args.size())
            bind_param(args_ctx, frame, static_cast<int>(i),
                       funcParams[i],
                       args[i], ctx->const_ctx,
                       static_cast<int>(i)); /* a call-site bind */
        else
            bind_param(args_ctx, frame, static_cast<int>(i),
                       funcParams[i],
                       EvalValue(), ctx->const_ctx, /*site_arg=*/-1);
    }
}

static void
do_func_bind_params(const std::vector<FuncDescriptor::ParamDesc> &funcParams,
                    const EvalValue &arg,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame,
                    size_t min_args)
{
    const size_t nparams = funcParams.size();
    if (nparams < 1 || min_args > 1)
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++)
        bind_param(args_ctx, frame, static_cast<int>(i),
                   funcParams[i],
                   i == 0 ? arg : EvalValue(), ctx->const_ctx,
                   /*site_arg=*/-1);            /* a callback's bind */
}


static void
do_func_bind_params(const std::vector<FuncDescriptor::ParamDesc> &funcParams,
                    const pair<EvalValue, EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame,
                    size_t min_args)
{
    const size_t nparams = funcParams.size();
    if (nparams < 2 || min_args > 2)
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++)
        bind_param(args_ctx, frame, static_cast<int>(i),
                   funcParams[i],
                   i == 0 ? args.first : i == 1 ? args.second : EvalValue(),
                   ctx->const_ctx, /*site_arg=*/-1); /* a callback's bind */
}

/*
 * Record `obj`'s frame on the unwinding exception (innermost first) - the
 * shared body of do_func_call's C++-throw catch AND its Inc-v2 signal path (a
 * VM-body exception propagates as g_vm_exc_pending, not a C++ throw, so the
 * catch never fires and the frame must be captured on the signal path instead).
 * Captures the name/params as STRINGS now (the AST is torn down during
 * unwinding). The call-site caret is a loc-table lookup (the VM's AST-free
 * call) or the passed loc (the tree-walker).
 */
static void
vm_capture_frame(Exception &e, FuncObject &obj, const Chunk *call_ck,
                 size_t call_pc, Loc call_site, const InlineCtx *call_site_inl,
                 bool in_const_eval = false)
{
    if (obj.func->pure_ctx)
        if (auto *undefEx = dynamic_cast<UndefinedVariableEx *>(&e))
            undefEx->in_pure_func = true;

    Loc cs = call_site;
    if (call_ck) {
        Loc end_ignored;
        call_ck->loc_at(call_pc, cs, end_ignored);
    }
    if (!in_const_eval) {
        /* profile #3: the LAZY frame - no strings at capture time (the
         * descriptor outlives the render: AST/VmProgram-owned, and both are
         * declared OUTSIDE the driver's try; format_backtrace stringifies). */
        e.push_frame(obj.func, cs);
    } else {
        /* A COMPILE-TIME fold (AutoConst / the inliner's refold) may run a
         * THROWAWAY clone whose descriptor dies with the fold, while the
         * early-failure rule propagates the exception out of resolve_names -
         * a lazy frame would dangle (ASan-caught). Capture the strings
         * eagerly here; const-eval throws are one-shot compile aborts, so
         * the cost is irrelevant. */
        const FuncDescriptor *d = obj.func;
        std::vector<std::string> ps;
        ps.reserve(d->params.size());
        for (const auto &p : d->params)
            ps.push_back(string(p.name->val));
        e.push_frame(
            !d->display_name.empty()
                ? d->display_name
                : d->name ? string(d->name->val) : "<lambda>",
            std::move(ps), cs);
    }

    if (call_site_inl) {
        flush_inline_frames(call_site_inl, e);
        e.inline_origin_emitted = true;
    }
}

/*
 * Invoke `obj` with `args`. Builds the callee's argument context (its own
 * FlowState and, when the function was resolved, a flat slot Frame), binds the
 * params, evaluates the body, and returns what the body returned via the
 * FlowState (or none). An UndefinedVariableEx escaping a pure func is tagged so
 * the error message can point at the pure-func restriction. `call_site` (the
 * CallExpr's loc) is recorded into an unwinding exception's backtrace.
 *
 * `as_signal` (Inc v2): when the caller is a VM CALL OP (vm_call_func /
 * vm_cached_call), a VM-body exception with no local handler is left as the
 * g_vm_exc_pending SIGNAL for that op to check (propagate WITHOUT a C++ throw);
 * every other caller (a builtin callback, the tree-walker) leaves it false, so
 * the signal is converted back to a C++ throw here for them.
 */
template <class ArgsVecT>
static EvalValue
do_func_call(EvalContext *ctx,
             FuncObject &obj,
             const ArgsVecT &args,
             Loc call_site = Loc(),
             const InlineCtx *call_site_inl = nullptr,
             const Chunk *call_ck = nullptr,
             size_t call_pc = 0,
             bool as_signal = false)
{
    /* func_ctx == true gives this call its own FlowState (see eval.h).
     * The parent is the closure's captured ROOT (G2): the empty link context
     * that used to sit between them passed root's fields through unchanged -
     * see FuncObject::capture_root. */
    /*
     * A closure loaded from a .myv pool has NO root of its own (it was built
     * before any EvalContext existed - see FuncObject::capture_root). It is
     * capture-free, so its body reads only globals and builtins, and those
     * live in the CALLER's root: substituting it keeps the body's lookups
     * defined instead of handing it a parent-less context whose `gfuncs` is
     * null. Never taken for a closure the running program created.
     */
    EvalContext *croot = obj.capture_root;

    if (!croot && ctx)
        croot = get_root_ctx(ctx);

    /* #38 B: a const-eval call of a function created in a const scope sees
     * that scope (lexically), not only the root - FuncObject::const_scope */
    if (obj.const_scope && ctx && ctx->in_const_eval()
            && obj.const_scope_alive && *obj.const_scope_alive)
        croot = obj.const_scope;

    EvalContext args_ctx(croot, false, true);

    /*
     * A SymKind::capture reference in the body reads this closure's
     * per-instance capture vector (an O(1) slot, no map walk). The pointer is
     * stable for the call (the vector is fixed at closure creation and the
     * FuncObject outlives the call); nested blocks inherit it.
     */
    args_ctx.captures = &obj.capture_slots;

    /*
     * When the function was resolved, params live in a flat slot Frame (O(1)
     * access) instead of the args context map. The Frame lives on this stack
     * frame for the whole call; nested blocks inherit the pointer.
     */
    Frame frame;
    const Chunk *vm_ck = nullptr;

    /*
     * Phase 4: under -vm, run the body as a native chunk. Compiled AOT
     * (vm_precompile_all) and stamped on the DESCRIPTOR, so this is a field
     * read per call. NOT gated on `resolved`: a zero-slot function (a
     * param-less, local-less closure - next_slot == 0, so the resolver leaves
     * it unresolved) still has a chunk, and after the AST teardown the chunk
     * is the ONLY way to run it.
     */
    /*
     * NOT during const-eval: AutoConst / the inliner's refold fold pure
     * calls at COMPILE time (resolve_names), while the tree is still being
     * transformed - compiling a body into a chunk there would cache
     * (on the descriptor) pointers into nodes the inliner then frees, and
     * running it re-reads freed memory (a RECYCLE/ASan use-after-poison).
     * The VM is a RUNTIME engine; const-eval always uses the tree-walker.
     */
    if (g_exec_engine == ExecEngine::Vm && !ctx->in_const_eval()) {
        if (!obj.func->vm_chunk_tried) {
            obj.func->vm_chunk = vm_func_chunk(obj.func);
            obj.func->vm_chunk_tried = true;
        }
        vm_ck = static_cast<const Chunk *>(obj.func->vm_chunk);
    }

    /*
     * When the function was resolved, params live in a flat slot Frame (O(1)
     * access) instead of the args context map; the frame also carries the
     * chunk's register-machine temps. A CHUNKED body's frame is a WINDOW of
     * the VM activation's segmented slot stack (vm_window_push - no per-call
     * Frame construction; the guard pops it on every exit path, releasing
     * the window's references exactly where the Frame dtor did). Fallbacks:
     * no live activation -> a plain Frame; unchunked resolved body -> the
     * plain Frame as always. An unresolved-but-chunked body (the zero-slot
     * closure) gets a temps-only window.
     */
    struct WindowGuard {
        bool engaged = false;
        ~WindowGuard() { if (engaged) vm_window_pop(); }
    } wguard;
    Frame *bindframe = nullptr;

    if (vm_ck) {
        const int_type total =
            obj.func->frame_size + static_cast<int_type>(vm_ck->n_temps);
        if (Frame *w = vm_window_push(total, vm_ck)) {
            wguard.engaged = true;
            args_ctx.frame = w;
        } else {
            frame.init(static_cast<int>(total));
            args_ctx.frame = &frame;
        }
        if (obj.func->resolved)
            bindframe = args_ctx.frame;
    } else if (obj.func->resolved) {
        frame.init(obj.func->frame_size);
        args_ctx.frame = &frame;
        bindframe = &frame;
    }

    if (!obj.func->params.empty()) {
        do_func_bind_params(
            obj.func->params, args, ctx, &args_ctx,
            bindframe,
            static_cast<size_t>(obj.func->min_args)
        );
    }

    try {

        /* An expression body - `func f(...) => expr;`, now parsed as
         * `{ return expr; }` (and its hand-written twin): evaluate the inner
         * expression DIRECTLY, skipping the Block statement loop + FlowState
         * round-trip - the old `=>` fast path, via the shared looker-through.
         * Skipped when a compiled chunk exists (-vm: the ReturnV chunk IS the
         * native form of the same body). */
        Construct *eb;
        if (!vm_ck && obj.func->decl
                && (eb = func_expr_body(obj.func->decl)) != nullptr) {
            return do_func_return(eb->eval(&args_ctx), eb);
        }

        /*
         * Block body: statements run until one sets the FlowState to `ret`
         * (stopping there). No exception is thrown for `return`. Under -vm a
         * loop-bearing body runs as a native chunk (vm_run_chunk stops on the
         * same `ret`); otherwise the tree-walker runs it. Either way the
         * return value is read from the FlowState below.
         */
        if (vm_ck) {
            vm_run_chunk(*vm_ck, args_ctx);
        } else {
            /* Tree-walk the body. After the -vm AST teardown every callable
             * function has a chunk and decl is null - reaching here without
             * one is a compiler bug, not a fallback.
             *
             * #122 TIER 2: unless the bytecode came off a DISK, where it is
             * not a compiler bug but hostile input - a descriptor whose
             * has-chunk bit was cleared. The loader refuses that shape
             * (serialize.cpp, tier 1) whenever the is-template-base bit
             * still disagrees with it; a mutation that clears BOTH is
             * self-consistent and reaches here, so this is the residue.
             * It must NOT be the ML_CHECK below and nothing else: that one
             * is gated on ASSERTS, and the build with the most to lose - an
             * optimized release running a shipped image - is exactly the
             * build that compiles it out and dereferences the null. */
            ML_UNTRUSTED_CHECK(obj.func->decl,
                               "a called function has neither a chunk nor a "
                               "body");
            ML_CHECK_MSG(obj.func->decl,
                         "call to a chunk-less function after AST teardown");
            obj.func->decl->body->eval(&args_ctx);
        }

    } catch (Exception &e) {
        /* The tree-walker / expression-body path (a real C++ throw). Record
         * this frame + any inlined-call virtual frames, then re-throw. */
        vm_capture_frame(e, obj, call_ck, call_pc, call_site, call_site_inl,
                         ctx && ctx->in_const_eval());
        throw;
    }

    /*
     * Inc v2: a VM body whose exception found no handler in its own frame set
     * g_vm_exc_pending (its boundary converted the C++ throw to the signal) and
     * RETURNED - so the catch above never fired. Record this frame the same
     * way; then either keep the signal in flight (a VM call op checks it) or,
     * for a non-VM caller (a builtin callback / the tree-walker), convert it
     * back to a C++ throw here. A cross-frame throw thus pays ONE landing-pad.
     */
    if (g_vm_exc_pending) {
        vm_capture_frame(*g_vm_exc_pending, obj, call_ck, call_pc, call_site,
                         call_site_inl);
        if (!as_signal) {
            std::unique_ptr<RuntimeException> ex = std::move(g_vm_exc_pending);
            ex->rethrow();
        }
        return none;                 /* signal in flight; caller checks it */
    }

    FlowState &fs = *args_ctx.flow;

    if (fs.type == FlowState::ret)
        return std::move(fs.value);

    return none;
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const vector<EvalValue> &args)
{
    EvalValue r;
    if (vm_try_invoke(ctx, obj, args.data(), args.size(), r))
        return r;

    return do_func_call(ctx, obj, args);
}

/* The VM's native-call entry (CallV): the args are already evaluated into a
 * contiguous run of the CALLER's frame slots, so bind them in place via a
 * VmArgs view - no per-call vector allocation (the hot recursion path).
 * `call_site` is the CallExpr's loc, for the backtrace. */
EvalValue vm_call_func(EvalContext *ctx,
                       FuncObject &obj,
                       const LValue *argslots,
                       size_t n,
                       const Chunk *ck, size_t pc)
{
    try {
        return do_func_call(ctx, obj, VmArgs{argslots, n}, Loc(), nullptr,
                            ck, pc, /*as_signal=*/true);
    } catch (Exception &e) {
        /* the bind runs BEFORE do_func_call's try, so its throw escapes
         * loc-less: the argument list's caret, as the tree-walker's
         * CallExpr::do_eval stamps it (an interpreted op passes its chunk;
         * the JIT's boundary path passes none and stamps in its own
         * conveyance) */
        if (ck)
            vm_stamp_setup_caret(e, *ck, pc);
        throw;
    }
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const EvalValue &arg)
{
    EvalValue r;
    if (vm_try_invoke(ctx, obj, &arg, 1, r))
        return r;
    return do_func_call(ctx, obj, arg);
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const pair<EvalValue, EvalValue> &args)
{
    EvalValue r;
    const EvalValue argv[2] = { args.first, args.second };
    if (vm_try_invoke(ctx, obj, argv, 2, r))
        return r;
    return do_func_call(ctx, obj, args);
}

/*
 * A call to a pure, self-recursive function the inliner unrolled
 * (obj.func->cache_results) caches its result in the CALLER's frame
 * (PureCache, eval.h). When the unroll splices the recursion into one frame,
 * the duplicate self-calls land here and compute ONCE - a per-frame "limited
 * memoization" that is SOUND (lazy: only calls actually made are stored, so it
 * never evaluates a call the program wouldn't) and NOT global (the cache dies
 * with the frame). Args are evaluated ONCE (for the key AND the bind), so a
 * side-effecting arg is not duplicated. Falls back to do_func_call when there
 * is no frame to cache in.
 */
/* Disabled by the `-npc` CLI flag, to measure the recursion unroll WITHOUT the
 * per-frame cache (the unroll still happens; only the dedup is off). */
bool g_pure_cache_enabled = true;

/*
 * The per-frame pure-call cache lookup given already-evaluated arg values:
 * {func, vals} -> result. A hit reuses it; a miss calls and caches a SCALAR
 * result (a fresh mutable container is never cached - it would alias across
 * callers; the un-cached call gives each its own). Shared by cached_call
 * (tree-walker) and vm_cached_call (the VM's CachedCallV). The caller has
 * already checked ctx->frame && g_pure_cache_enabled.
 */
static EvalValue
pure_cache_call(EvalContext *ctx, FuncObject &obj,
                const vector<EvalValue> &vals, Loc call_site,
                const InlineCtx *inl,
                const Chunk *ck = nullptr, size_t pc = 0,
                bool as_signal = false)
{
    PureCache &cache = ctx->frame->ensure_pure_cache();
    PureCacheKey key{ obj.func, vals };
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    EvalValue r = do_func_call(ctx, obj, vals, call_site, inl, ck, pc,
                               as_signal);
    /* Inc v2: a signaled (cross-frame unwinding) call must NOT be cached - its
     * result is a sentinel. Only reachable with as_signal (a VM caller); the
     * tree-walker's cached_call passes false, so do_func_call C++-throws and
     * this is never a signal. */
    if (g_vm_exc_pending)
        return r;
    if (r.get_type()->t < Type::t_str)
        cache.emplace(std::move(key), r);
    return r;
}

static EvalValue
cached_call(EvalContext *ctx, FuncObject &obj,
            const vector<unique_ptr<Construct>> &args,
            Loc call_site, const InlineCtx *inl)
{
    if (!ctx->frame || !g_pure_cache_enabled)
        return do_func_call(ctx, obj, args, call_site, inl);

    vector<EvalValue> vals;
    vals.reserve(args.size());
    for (const auto &a : args)
        vals.push_back(RValue(a->eval(ctx)));

    return pure_cache_call(ctx, obj, vals, call_site, inl);
}

/* The VM's CachedCallV: cached_call but the args are already evaluated into the
 * caller's frame slots (no node->eval). Same per-frame dedup. */
EvalValue
vm_cached_call(EvalContext *ctx, FuncObject &obj,
               const LValue *argslots, size_t n, const Chunk *ck, size_t pc)
{
    if (!ctx->frame || !g_pure_cache_enabled)
        return do_func_call(ctx, obj, VmArgs{argslots, n},
                            Loc(), nullptr, ck, pc, /*as_signal=*/true);

    vector<EvalValue> vals;
    vals.reserve(n);
    for (size_t i = 0; i < n; i++)
        vals.push_back(argslots[i].get());

    return pure_cache_call(ctx, obj, vals, Loc(), nullptr, ck, pc,
                           /*as_signal=*/true);
}

static void stamp_operand_loc(const Construct *c, Exception &e);
static bool is_lvalue_rooted(const Construct *c);

/*
 * Build a struct instance from a (positional, already-desugared) argument list.
 * v1 storage is boxed: one LValue slot per field, in declaration order. A
 * numeric field coerces a widening argument (int field <- bool, float field <-
 * int/bool), like a typed parameter. The inferencer has already type-checked
 * the arguments against the field types (for a statically-known callee), so
 * this is the construction, not the validation.
 */
/* Coerce + runtime-validate one field value. The inferencer already checks a
 * statically-known construction, but this guards a `dyn`-laundered value and
 * makes a parse-time (const) construction type-safe (so it can fold).
 * Non-static: the VM's vm_struct_ctor (vm.cpp) pre-coerces with it (H1). */
EvalValue
coerce_struct_field(const FieldDef &fd, EvalValue v, Loc s, Loc e)
{
    if (v.is<NoneVal>()) {
        if (fd.is_opt)
            return v;
        throw TypeErrorEx(intern_msg("struct field '" +
                          string(fd.name->val) + "' cannot be none"), s, e);
    }

    switch (fd.kind) {
        case FieldKind::f_dyn:
            return v;
        case FieldKind::f_int:
            if (v.is<bool>() || v.is<int_type>())
                return coerce_to_decl_type(v, DeclType::i);
            break;
        case FieldKind::f_float:
            if (v.is<bool>() || v.is<int_type>() || v.is<float_type>())
                return coerce_to_decl_type(v, DeclType::f);
            break;
        case FieldKind::f_bool:
            if (v.is<bool>())
                return v;
            break;
        case FieldKind::f_str:
            if (v.is<SharedStr>())
                return v;
            break;
        case FieldKind::f_array:
            if (v.is<SharedArrayObj>())
                return v;
            break;
        case FieldKind::f_dict:
            if (v.is<intrusive_ptr<DictObject>>())
                return v;
            break;
        case FieldKind::f_struct:
            if (v.is<intrusive_ptr<StructObject>>() &&
                v.get<intrusive_ptr<StructObject>>()->def->name == fd.struct_ty)
                return v;
            break;
    }

    throw TypeErrorEx(intern_msg("struct field '" + string(fd.name->val) +
                      "' got a value of the wrong type"), s, e);
}

/*
 * Native composite types for reflection (see plans/archived/reflection.md). Built once
 * (lazy static, program-lifetime), boxed (they have str/array fields), slots in
 * declaration order. `StructLayout`'s `fields` carries a TypeAnnot of
 * `array<StructField>` so the inferencer types `layout(S).fields[0]` as a
 * StructField.
 */
const StructTypeDef *native_struct_field_def()
{
    static StructTypeDef *const def = []() {
        auto *d = new StructTypeDef();
        d->name = UniqueId::get("StructField");
        auto add = [&](const char *n, FieldKind k) {
            FieldDef f;
            f.name = UniqueId::get(n);
            f.kind = k;
            f.slot = static_cast<int>(d->fields.size());
            d->fields.push_back(f);
        };
        add("name",   FieldKind::f_str);
        add("type",   FieldKind::f_str);
        add("offset", FieldKind::f_int);
        add("size",   FieldKind::f_int);
        add("align",  FieldKind::f_int);
        d->compute_layout();
        return d;
    }();
    return def;
}

const StructTypeDef *native_struct_layout_def()
{
    static StructTypeDef *const def = []() {
        auto *d = new StructTypeDef();
        d->name = UniqueId::get("StructLayout");
        auto add = [&](const char *n, FieldKind k,
                       std::shared_ptr<TypeAnnot> annot) {
            FieldDef f;
            f.name = UniqueId::get(n);
            f.kind = k;
            f.annot = std::move(annot);
            f.slot = static_cast<int>(d->fields.size());
            d->fields.push_back(f);
        };
        add("name",  FieldKind::f_str,  nullptr);
        add("size",  FieldKind::f_int,  nullptr);
        add("align", FieldKind::f_int,  nullptr);
        add("pod",   FieldKind::f_bool, nullptr);
        /* fields: array<StructField> */
        auto elem = std::make_shared<TypeAnnot>();
        elem->kind = DeclType::strct;
        elem->strct = native_struct_field_def();
        auto arr = std::make_shared<TypeAnnot>();
        arr->kind = DeclType::arr;
        arr->elem = elem;
        add("fields", FieldKind::f_array, arr);
        d->compute_layout();
        return d;
    }();
    return def;
}

const StructTypeDef *native_struct_type_def()
{
    static StructTypeDef *const def = []() {
        auto *d = new StructTypeDef();
        d->name = UniqueId::get("Type");
        auto scalar = [&](const char *n, FieldKind k) {
            FieldDef f;
            f.name = UniqueId::get(n);
            f.kind = k;
            f.slot = static_cast<int>(d->fields.size());
            d->fields.push_back(f);
        };
        scalar("kind",     FieldKind::f_str);
        scalar("name",     FieldKind::f_str);
        scalar("nullable", FieldKind::f_bool);
        /* elem / key / val: `opt Type` (self-ref; opt breaks the recursion) */
        auto self = [&](const char *n) {
            FieldDef f;
            f.name = UniqueId::get(n);
            f.kind = FieldKind::f_struct;
            f.struct_ty = d->name;   /* "Type" */
            f.struct_def = d;        /* self */
            f.is_opt = true;
            f.slot = static_cast<int>(d->fields.size());
            d->fields.push_back(f);
        };
        self("elem");
        self("key");
        self("val");
        d->compute_layout();   /* boxed */
        return d;
    }();
    return def;
}

/* Construct a POD struct from already-evaluated field VALUES (the VM's
 * StructCtorV, whose field args were compiled into a register run). Mirrors
 * construct_struct's POD path but takes values, not an ExprList to eval. The
 * codegen emits StructCtorV only when nargs == nfields and every arg is a typed
 * scalar the inferencer proved assignable, so coerce_struct_field cannot throw
 * here; a defensive throw is caught + given the construction's loc by the VM
 * handler (empty locs below). */
intrusive_ptr<StructObject>
construct_struct_from_values(StructTypeDef *def,
                            const EvalValue *vals, size_t n)
{
    auto obj = make_intrusive<StructObject>(def);
    for (size_t i = 0; i < n; i++) {
        const FieldDef &fd = def->fields[i];
        if (field_exact_scalar(fd, vals[i])) {          /* G3 */
            obj->pod_set(static_cast<int>(i), vals[i]);
            continue;
        }
        EvalValue v = coerce_struct_field(fd, vals[i], Loc(), Loc());
        obj->pod_set(static_cast<int>(i), v);
    }
    return intrusive_ptr<StructObject>(obj);
}

intrusive_ptr<StructObject>
construct_struct_boxed_from_values(StructTypeDef *def, const EvalValue *vals,
                                  size_t nargs, const ArgLoc *locs)
{
    const size_t nfields = def->fields.size();
    auto obj = make_intrusive<StructObject>(def);

    /* A POD def reaches here too (StructCtorBoxedV doubles as the CHECKED
     * POD ctor - a dyn/unproven field arg whose coerce CAN throw and must
     * report the arg's caret, which StructCtorV's typed-only gate cannot):
     * coerce with the carets, then store into the POD bytes. */
    if (def->is_pod()) {
        for (size_t i = 0; i < nfields; i++) {
            const Loc s = (i < nargs && locs) ? locs[i].start : Loc();
            const Loc e = (i < nargs && locs) ? locs[i].end : Loc();
            EvalValue v = coerce_struct_field(def->fields[i],
                                              i < nargs ? vals[i]
                                                        : EvalValue(),
                                              s, e);
            obj->pod_set(static_cast<int>(i), v);
        }
        return intrusive_ptr<StructObject>(obj);
    }

    obj->fields.reserve(nfields);
    for (size_t i = 0; i < nfields; i++) {
        const FieldDef &fd = def->fields[i];
        const Loc s = (i < nargs && locs) ? locs[i].start : Loc();
        const Loc e = (i < nargs && locs) ? locs[i].end : Loc();
        /* an omitted trailing opt field binds to none */
        EvalValue v = i < nargs ? vals[i] : EvalValue();
        obj->fields.emplace_back(coerce_struct_field(fd, std::move(v), s, e), false);
    }
    return intrusive_ptr<StructObject>(obj);
}

EvalValue vm_make_struct_array(StructTypeDef *def, size_t n,
                               const EvalValue *vals)
{
    const size_t M = def->fields.size();
    const int stride = def->size;
    /* Coerce every struct's fields STRAIGHT into the contiguous flat buffer -
     * no per-element StructObject (the whole point of the fused op). vals is
     * interleaved: struct i's field j at vals[i*M + j]. */
    std::vector<char> buf(n * static_cast<size_t>(stride));
    for (size_t i = 0; i < n; i++) {
        char *base = buf.data() + i * static_cast<size_t>(stride);
        for (size_t j = 0; j < M; j++) {
            EvalValue v = coerce_struct_field(def->fields[j], vals[i * M + j],
                                              Loc(), Loc());
            pod_store_field(def->fields[j], base, v);
        }
    }
    return SharedArrayObj(
        SharedArrayObj::svec_type(std::move(buf), def, stride));
}

static EvalValue
construct_struct(EvalContext *ctx, StructTypeDef *def, ExprList *args)
{
    const size_t nfields = def->fields.size();
    const size_t nargs = args->elems.size();

    /* A trailing opt field may be omitted (binds to none); an interior skipped
     * one was already filled with an explicit `none` by the desugar. */
    size_t min_args = 0;
    for (size_t i = 0; i < nfields; i++)
        if (!def->fields[i].is_opt)
            min_args = i + 1;

    if (nargs < min_args || nargs > nfields)
        throw InvalidNumberOfArgsEx(args->start, args->end);

    auto obj = make_intrusive<StructObject>(def);   /* resizes bytes if POD */

    if (def->is_pod()) {
        /* POD has no opt fields, so nargs == nfields; store each scalar into
         * its byte slot. */
        for (size_t i = 0; i < nfields; i++) {
            EvalValue v = coerce_struct_field(
                def->fields[i], RValue(args->elems[i]->eval(ctx)),
                args->elems[i]->start, args->elems[i]->end);
            obj->pod_set(static_cast<int>(i), v);
        }
        return intrusive_ptr<StructObject>(obj);
    }

    obj->fields.reserve(nfields);

    for (size_t i = 0; i < nfields; i++) {
        const FieldDef &fd = def->fields[i];
        const Loc s = i < nargs ? args->elems[i]->start : args->start;
        const Loc e = i < nargs ? args->elems[i]->end : args->end;
        /* an omitted trailing opt field binds to none */
        EvalValue v = i < nargs ? RValue(args->elems[i]->eval(ctx))
                                : EvalValue();
        obj->fields.emplace_back(coerce_struct_field(fd, std::move(v), s, e), false);
    }

    return intrusive_ptr<StructObject>(obj);
}

/*
 * The VALUES twin of construct_struct, for an INDIRECT (dyn-callee)
 * construction (`var dyn c = P; c(1, 2)` - the VM's CallValueGenericV): the
 * args are pre-evaluated (raw - an arg may be LValue*-boxed; RValue'd here),
 * UNVALIDATED at compile time, so the full runtime checks run - the same
 * arity range, trailing-opt none-fill, and per-field coerce (with the pooled
 * per-arg carets) construct_struct does, mirrored line for line.
 */
EvalValue construct_struct_v(StructTypeDef *def, const ArgLocs *al,
                             const EvalValue *args, size_t nargs)
{
    const size_t nfields = def->fields.size();

    size_t min_args = 0;
    for (size_t i = 0; i < nfields; i++)
        if (!def->fields[i].is_opt)
            min_args = i + 1;

    if (nargs < min_args || nargs > nfields)
        throw InvalidNumberOfArgsEx(al->start, al->end);

    auto obj = make_intrusive<StructObject>(def);   /* resizes bytes if POD */

    if (def->is_pod()) {
        /* POD has no opt fields, so nargs == nfields. */
        for (size_t i = 0; i < nfields; i++) {
            EvalValue v = coerce_struct_field(
                def->fields[i], RValue(args[i]),
                al->arg(i)->start, al->arg(i)->end);
            obj->pod_set(static_cast<int>(i), v);
        }
        return EvalValue(intrusive_ptr<StructObject>(obj));
    }

    obj->fields.reserve(nfields);

    for (size_t i = 0; i < nfields; i++) {
        const FieldDef &fd = def->fields[i];
        const Loc s = i < nargs ? al->arg(i)->start : al->start;
        const Loc e = i < nargs ? al->arg(i)->end : al->end;
        /* an omitted trailing opt field binds to none */
        EvalValue v = i < nargs ? RValue(args[i]) : EvalValue();
        obj->fields.emplace_back(coerce_struct_field(fd, std::move(v), s, e),
                                 false);
    }

    return EvalValue(intrusive_ptr<StructObject>(obj));
}

void pod_store_field(const FieldDef &f, char *base, const EvalValue &v)
{
    char *p = base + f.offset;
    switch (f.kind) {
        case FieldKind::f_bool:
            *p = v.get<bool>() ? 1 : 0;
            break;
        case FieldKind::f_int: {
            int_type x = v.get<int_type>();
            std::memcpy(p, &x, sizeof x);
            break;
        }
        case FieldKind::f_float: {
            float_type x = v.get<float_type>();
            std::memcpy(p, &x, sizeof x);
            break;
        }
        case FieldKind::f_struct: {
            const StructObject &o = *v.get<intrusive_ptr<StructObject>>().get();
            std::memcpy(p, o.bytes.data(), f.struct_def->size);
            break;
        }
        default:
            throw InternalErrorEx();
    }
}

static inline void
apply_compound_op(EvalValue &acc, const EvalValue &rhs, Op op);

/* A POD field's value from raw struct bytes - StructObject::pod_get's read,
 * for bytes no StructObject owns (a POD place). */
static EvalValue pod_load_field(const FieldDef &f, const char *base)
{
    const char *p = base + f.offset;
    if (f.kind == FieldKind::f_bool)
        return EvalValue(static_cast<unsigned char>(*p) != 0);
    if (f.kind == FieldKind::f_int) {
        int_type v;
        std::memcpy(&v, p, sizeof v);
        return EvalValue(v);
    }
    if (f.kind == FieldKind::f_float) {
        float_type v;
        std::memcpy(&v, p, sizeof v);
        return EvalValue(v);
    }
    /* else an inline POD struct: a POD field is nothing else */
    StructTypeDef *nd = const_cast<StructTypeDef *>(f.struct_def);
    auto obj = make_intrusive<StructObject>(nd);
    std::memcpy(obj->bytes.data(), p, nd->size);
    return intrusive_ptr<StructObject>(obj);
}

/* STRUCT VALUE SEMANTICS (eval.h) */
StructObject &struct_own(LValue *&holder)
{
    if (holder->is_borrowed()) {
        /* the caller's slot holds the counted reference: a write here
         * would change the caller's struct, whatever the count says */
        intrusive_ptr<StructObject> c = make_intrusive<StructObject>(
            *holder->get().get_ref<intrusive_ptr<StructObject>>());
        c->clear_readonly();
        StructObject &r = *c;
        holder->frame_release();                /* abandons the borrow */
        holder->put(EvalValue(std::move(c)));
        return r;
    }
    holder = holder->write_target();
    intrusive_ptr<StructObject> &p =
        holder->getval<intrusive_ptr<StructObject>>();
    if (p.use_count() == 1 && !p->is_readonly())
        return *p;
    /* shallow: the fields are copied, a nested struct shares its object
     * until a write owns it too, an array or a dict stays shared - they
     * are references - and keeps its own read-only flag */
    intrusive_ptr<StructObject> c = make_intrusive<StructObject>(*p);
    c->clear_readonly();
    p = std::move(c);
    return *p;
}

void struct_own_if(LValue *&holder)
{
    if (holder->is<intrusive_ptr<StructObject>>())
        struct_own(holder);
}

/*
 * POD PLACES (eval.h). The walk only ever enters bytes it will STORE into,
 * so the flat array is detached on the way in (a slice made standalone, an
 * alias's slices cloned), exactly as flat_store_core detaches it for a
 * whole-element store; a rooted POD instance is entered only after its
 * holder OWNS it (struct_own), so the bytes are its alone.
 */
bool pod_place_elem(LValue *blv, const EvalValue &key, PodPlace &out)
{
    if (!blv->is<SharedArrayObj>())
        return false;
    SharedArrayObj &arr = blv->getval<SharedArrayObj>();
    if (arr.skind() != SharedArrayObj::Storage::structs ||
        blv->is_const_var() || arr.is_readonly())
        return false;

    /* TypeArr::subscript's checks, in its order, loc-less */
    if (!key.is<int_type>())
        throw TypeErrorEx("Expected integer as subscript");
    int_type idx = key.get<int_type>();
    ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
    if (idx < 0)
        idx += arr.size();
    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx();

    if (arr.is_slice())
        arr.clone_internal_vec();
    else if (arr.use_count() > 1)
        arr.clone_aliased_slices(arr.offset() + idx);
    arr.invalidate_hash();

    auto &sv = arr.flat_structs();
    out.bytes = sv.buf.data() + (arr.offset() + idx) * sv.stride;
    out.def = sv.def;
    return true;
}

bool pod_place_step(PodPlace &p, const UniqueId *memUid)
{
    const int slot = p.def->slot_of(memUid);
    if (slot < 0)
        return false;
    /* a POD struct's struct field is an inline POD struct (compute_layout) */
    const FieldDef &f = p.def->fields[static_cast<size_t>(slot)];
    if (f.kind != FieldKind::f_struct)
        return false;
    p.bytes += f.offset;
    p.def = f.struct_def;
    return true;
}

bool pod_place_rooted_field(LValue *blv, const UniqueId *memUid,
                            PodPlace &out)
{
    if (!blv->is<intrusive_ptr<StructObject>>())
        return false;
    StructObject &obj = *blv->getval<intrusive_ptr<StructObject>>().get();
    /* read-only: not yet the slot's own (struct_own comes first) */
    if (!obj.is_pod() || obj.is_readonly())
        return false;
    PodPlace p;
    p.bytes = obj.bytes.data();
    p.def = obj.def;
    if (!pod_place_step(p, memUid))
        return false;
    out = p;
    return true;
}

EvalValue pod_place_value(const PodPlace &p)
{
    StructTypeDef *nd = const_cast<StructTypeDef *>(p.def);
    auto obj = make_intrusive<StructObject>(nd);
    std::memcpy(obj->bytes.data(), p.bytes, nd->size);
    return intrusive_ptr<StructObject>(obj);
}

EvalValue pod_place_store(const PodPlace &p, int slot, Op op,
                          const EvalValue &rval, Loc ms, Loc me,
                          EvalValue *old_out)
{
    const FieldDef &f = p.def->fields[static_cast<size_t>(slot)];
    const EvalValue r = RValue(rval);
    EvalValue newval;
    if (op == Op::assign) {
        newval = r;
    } else {
        newval = pod_load_field(f, p.bytes);
        if (old_out)
            *old_out = newval;
        apply_compound_op(newval, r, op);
    }
    /* coerce + runtime-validate to the field's type (member_store's rule) */
    newval = coerce_struct_field(f, std::move(newval), ms, me);
    pod_store_field(f, p.bytes, newval);
    return newval;
}

EvalValue pod_place_incdec(const PodPlace &p, int slot, bool is_inc,
                           bool is_prefix, Loc ms, Loc me,
                           Loc id_start, Loc id_end)
{
    const FieldDef &f = p.def->fields[static_cast<size_t>(slot)];
    const EvalValue old = pod_load_field(f, p.bytes);
    if (!old.is<int_type>() && !old.is<float_type>())
        throw TypeErrorEx("'++'/'--' requires an int or float",
                          id_start, id_end);
    EvalValue nv = old;
    apply_compound_op(nv, EvalValue(static_cast<int_type>(1)),
                      is_inc ? Op::addeq : Op::subeq);
    nv = coerce_struct_field(f, std::move(nv), ms, me);
    pod_store_field(f, p.bytes, nv);
    return is_prefix ? nv : old;
}

/*
 * The build-hot fast path behind `append(arr, Point(...))`: when appending a
 * struct constructor call to a flat POD-struct array of that exact type,
 * construct the element STRAIGHT into the array's byte buffer - no temporary
 * StructObject (which would be two heap allocations per element, the dominant
 * cost of building an array<Struct>). Returns false (caller falls back to the
 * normal `eval arg -> append value` path) unless every condition holds.
 *
 * Field args are evaluated/coerced into a stack buffer FIRST, so a throw mid-
 * construction (a type error, a side-effecting arg) leaves the array unchanged;
 * only on full success is the slot committed (resize + copy). Declared in
 * structtype.h so builtin_append (a different TU) can call it.
 */
bool try_construct_into_struct_array(EvalContext *ctx, SharedArrayObj &arr,
                                     Construct *arg);
bool try_construct_into_struct_array(EvalContext *ctx, SharedArrayObj &arr,
                                     Construct *arg)
{
    auto *cc = dynamic_cast<CallExpr *>(arg);
    if (!cc || !cc->args)
        return false;

    const EvalValue cv = RValue(cc->what->eval(ctx));
    if (!cv.is<StructTypeDef *>())
        return false;

    StructTypeDef *def = cv.get<StructTypeDef *>();
    auto &sv = arr.flat_structs();
    if (def != sv.def || !def->is_pod())
        return false;

    const size_t nfields = def->fields.size();
    /* POD has no opt fields: an exact arity is required. A mismatch falls back
     * so the normal path raises the proper InvalidNumberOfArgsEx. */
    if (cc->args->elems.size() != nfields)
        return false;

    /* Build into a stack buffer (POD structs are small); cap defensively. */
    constexpr int STACK_CAP = 512;
    if (def->size > STACK_CAP)
        return false;
    char tmp[STACK_CAP];

    for (size_t i = 0; i < nfields; i++) {
        EvalValue v = coerce_struct_field(
            def->fields[i], RValue(cc->args->elems[i]->eval(ctx)),
            cc->args->elems[i]->start, cc->args->elems[i]->end);
        pod_store_field(def->fields[i], tmp, v);
    }

    const size_t at = sv.buf.size();
    sv.buf.resize(at + sv.stride);
    std::memcpy(sv.buf.data() + at, tmp, sv.stride);
    return true;
}

/*
 * VM Phase 2b: append a POD struct built from PRE-EVALUATED field values `vals`
 * (the ctor's args, compiled into a VM register run) to the array `target`,
 * without a temporary StructObject on the hot path. The values-driven twin of
 * builtin_append's construct-in-place (try_construct_into_struct_array): the
 * FAST path coerces the values straight into a flat array<Struct>'s bytes; a
 * non-flat target (an array<dyn> holding structs) FALLS BACK to building the
 * StructObject and a general append. AST-FREE: `cdef` is the ctor's POD def
 * (inferencer-stamped at codegen), `flocs` the per-field coerce carets, and
 * `a0s`/`a0e` the container arg's caret (the lvalue/array errors) - all from
 * the VM's emplace_sites pool. Result matches the tree-walker's append
 * byte-for-byte (the differential proves it).
 */
EvalValue vm_emplace_struct(EvalContext *ctx, LValue *target,
                            Loc a0s, Loc a0e, const StructTypeDef *cdef,
                            const ArgLoc *flocs, const EvalValue *vals,
                            size_t n)
{
    if (!target)
        throw NotLValueEx(a0s, a0e);
    if (!target->is<SharedArrayObj>())
        throw TypeErrorEx("Expected array", a0s, a0e);
    if (target->is_const_var())
        throw CannotChangeConstEx(a0s, a0e);

    SharedArrayObj &arr = target->getval<SharedArrayObj>();
    if (arr.is_readonly())
        throw CannotChangeConstEx(a0s, a0e);
    if (arr.is_slice())
        arr.clone_internal_vec();

    StructTypeDef *def = const_cast<StructTypeDef *>(cdef);
    ML_CHECK(def && def->is_pod() && n == def->fields.size());

    /* FAST: a flat POD-struct array of this exact def - coerce into the bytes,
     * no temporary StructObject (mirrors try_construct_into_struct_array). */
    if (arr.skind() == SharedArrayObj::Storage::structs &&
        arr.flat_structs().def == def && def->size <= 512) {

        char tmp[512];
        for (size_t i = 0; i < n; i++) {
            EvalValue v = coerce_struct_field(
                def->fields[i], vals[i],
                flocs[i].start, flocs[i].end);
            pod_store_field(def->fields[i], tmp, v);
        }
        auto &sv = arr.flat_structs();
        const size_t at = sv.buf.size();
        sv.buf.resize(at + sv.stride);
        std::memcpy(sv.buf.data() + at, tmp, sv.stride);
        arr.invalidate_hash();
        return target->get();
    }

    /* FALLBACK: build the StructObject from the values, then append it - a flat
     * array<Struct> (huge struct / same def) memcpys its bytes, a general array
     * stores the value. */
    auto obj = make_intrusive<StructObject>(def);
    for (size_t i = 0; i < n; i++)
        obj->pod_set(static_cast<int>(i), coerce_struct_field(
            def->fields[i], vals[i],
            flocs[i].start, flocs[i].end));
    const EvalValue elem(intrusive_ptr<StructObject>{obj});

    if (arr.skind() == SharedArrayObj::Storage::structs &&
        arr.flat_structs().def == def) {
        auto &sv = arr.flat_structs();
        const size_t at = sv.buf.size();
        sv.buf.resize(at + sv.stride);
        std::memcpy(sv.buf.data() + at, obj->bytes.data(), sv.stride);
    } else if (arr.skind() == SharedArrayObj::Storage::general) {
        arr.get_vec().emplace_back(elem, ctx->const_ctx);
    } else {
        throw TypeErrorEx("Cannot append a struct to this array",
                          a0s, a0e);
    }
    arr.invalidate_hash();
    return target->get();
}

/*
 * F1 step 2: the VALUES dispatch of an INDIRECT builtin call, shared by the
 * tree-walker's dyn-callee branch (map/filter only there - the other kinds
 * keep their node path, whose adapters are byte-identical to this) and the
 * VM's CallValueGenericV (all kinds). `args` are RAW values: arg0 may be an
 * LValue*-boxed value (the lvalue-preserving compile / Identifier::do_eval)
 * for a func_lv callee; every by-value consumer RValues (which also turns an
 * UndefinedId into UndefinedVariableEx, like the adapters).
 */
EvalValue dispatch_builtin_values(EvalContext *ctx, const Builtin &b,
                                  const ArgLocs *al, const EvalValue *args,
                                  size_t n)
{
    switch (b.kind) {

    case Builtin::Kind::value: {
        /* The generic func_v adapter's exact behavior: RValue each arg
         * left-to-right, then the native form. */
        EvalValue stackbuf[8];
        std::vector<EvalValue> heapbuf;
        EvalValue *buf = stackbuf;
        if (n > 8) {
            heapbuf.resize(n);
            buf = heapbuf.data();
        }
        for (size_t i = 0; i < n; i++)
            buf[i] = RValue(args[i]);
        return b.func_v(ctx, al, buf, n);
    }

    case Builtin::Kind::lvalue: {
        /* The lvalue adapters' exact behavior: arg0's LValue* if it IS one
         * (else null -> the builtin throws NotLValueEx / its arity error),
         * the rest RValued by value. */
        LValue tmp;
        LValue *target = n
            ? builtin_lv_target(b, args[0].is<LValue *>()
                                       ? args[0].get<LValue *>() : nullptr,
                                args[0], tmp)
            : nullptr;
        const size_t n_rest = n ? n - 1 : 0;
        EvalValue stackbuf[8];
        std::vector<EvalValue> heapbuf;
        EvalValue *buf = stackbuf;
        if (n_rest > 8) {
            heapbuf.resize(n_rest);
            buf = heapbuf.data();
        }
        for (size_t i = 0; i < n_rest; i++)
            buf[i] = RValue(args[i + 1]);
        return b.func_lv(ctx, al, target, n_rest ? buf : nullptr, n_rest);
    }

    case Builtin::Kind::map:
    case Builtin::Kind::filter: {
        /* EAGER-ARGS by language rule (INDIRECT calls only; the direct form
         * keeps builtin_map's validate-before-arg1 order): arity first (like
         * builtin_map, before any evaluation), then the func check with
         * arg0's caret, then the shared core. */
        if (n != 2)
            throw InvalidArgumentEx(al->start, al->end);
        const EvalValue f = RValue(args[0]);
        if (!f.is<intrusive_ptr<FuncObject>>())
            throw TypeErrorEx("Expected function",
                              al->arg(0)->start, al->arg(0)->end);
        const EvalValue c = RValue(args[1]);
        /* an indirect call has no destination hint: a general result */
        return vm_map_filter(ctx, f, c, b.kind == Builtin::Kind::filter,
                             al->arg(1)->start, al->arg(1)->end,
                             al->arg(1)->start, 0);
    }

    case Builtin::Kind::lazy:
    case Builtin::Kind::node:
        /* Unreachable from a SCRIPT: a lazy builtin can't be a value (the F1
         * step 1 compile reject) and no node-kind builtin remains; the REPL
         * dispatches these via its retained node, never through here. A
         * loud, honest error as the tripwire. */
        throw TypeErrorEx(
            "this builtin cannot be called indirectly", al->start, al->end);
    }

    throw InternalErrorEx();   /* unreachable (all kinds handled) */
}

/*
 * THE CALL'S SETUP CARET (RULE 2, 2026-09-20). A loc-less exception out of
 * a call's SETUP - an arity throw, a bind coercion, the window push - is
 * stamped here, by the call site, since the throw itself knows no source
 * position. Two spans, chosen by what failed:
 *  - a BIND COERCION (`func f(int k)` handed a `dyn` float) carets the
 *    FAILING ARGUMENT's own expression - the bind recorded its index in
 *    Exception::bind_arg (bind_param's `site_arg`);
 *  - anything else (an arity error is about the LIST; the window push is
 *    about no argument) carets the whole argument list.
 * Shared by CallExpr::do_eval's dispatch (dispatch_call_value's catch) and
 * the DEVIRTUALIZED nodes (DirectCallExpr/CachedCallExpr), which must
 * reproduce it exactly: before they did, `fi(x)` marked the whole call
 * from a named function and the argument list from a closure. The VM's
 * twin is vm_stamp_setup_caret (base_locs + arg_locs), the generic
 * dyn-callee op's is vm_stamp_args_caret (its CallSite), and the JIT's
 * conveyance stamps it in emitted code (emit_exc_stamp's args form). An
 * out-of-range index (a stale value from an unrelated bind) takes the
 * list span - a caret is never invented from it.
 */
static inline void stamp_args_loc(Exception &e, const ExprList *args)
{
    if (e.loc_start)
        return;
    const int i = e.bind_arg;
    if (i >= 0 && static_cast<size_t>(i) < args->elems.size()
            && args->elems[static_cast<size_t>(i)]->start) {
        /* (an argument with no span of its own - a const-folded literal -
         * takes the list's, exactly as codegen records it for the VM) */
        e.loc_start = args->elems[static_cast<size_t>(i)]->start;
        e.loc_end = args->elems[static_cast<size_t>(i)]->end;
        return;
    }
    e.loc_start = args->start;
    e.loc_end = args->end;
}

/*
 * Shared call DISPATCH: `callable` is the ALREADY-evaluated callee value, `node`
 * the CallExpr (for its args + carets). A Builtin runs its ExprList ABI -
 * EXCEPT an INDIRECT (vm_dyn_callee) call of a non-lazy builtin, which is
 * EAGER-ARGS by language rule (F1 step 2) and
 * routes through the shared values dispatch so both engines agree; a
 * FuncObject calls do_func_call (its body runs native under -vm via the hook), a
 * struct descriptor constructs; anything else is NotCallableEx at the callee's
 * caret. Reused by the tree-walker's CallExpr::do_eval (ck=null, as_signal=false)
 * AND the VM's generic value-call op (CallValueGenericV: &chunk, pc,
 * as_signal=true) so the two engines dispatch byte-identically.
 */
/*
 * #32: arg0 of an INDIRECT call to an in-place (lvalue) builtin, by
 * reference: an access chain rooted at a variable that steps into a
 * member is walked as a STORE base (store_base_value - each struct it
 * enters owned), so the builtin works on this variable's struct and not
 * on one a copy still shares, as the direct call's adapter does and the
 * VM's arg0 descriptors do; anything else is read as before (a value
 * stays a value - the builtin's "not an lvalue"). An optional member is
 * not a step a store walk has.
 */
static EvalValue indirect_lv_arg0(EvalContext *ctx, const Construct *e,
                                  LValue &hold)
{
    bool member = false;
    const Construct *r = e;
    for (;;) {
        if (ctag(r) == ConstructType::member) {
            auto *m = static_cast<const MemberExpr *>(r);
            if (m->optional)
                return e->eval(ctx);
            member = true;
            r = m->what.get();
        } else if (ctag(r) == ConstructType::subscript) {
            r = static_cast<const Subscript *>(r)->what.get();
        } else {
            break;
        }
    }
    if (!member || ctag(r) != ConstructType::id)
        return e->eval(ctx);
    return store_base_value(ctx, e, hold);
}

EvalValue dispatch_call_value(EvalContext *ctx, const EvalValue &callable,
                              const CallExpr *node, const Chunk *ck,
                              size_t pc, bool as_signal)
{
    try {

        if (callable.is<Builtin>()) {
            const Builtin &b = callable.get<Builtin>();
            /* The eager path applies ONLY to a genuinely INDIRECT call (a
             * dyn-typed callee - the inferencer's vm_dyn_callee stamp): this
             * dispatch ALSO serves every tree-walked DIRECT builtin call
             * (const-eval, the REPL, an unspecialized CallExpr), which must
             * keep the node ABI - map's validate-before-arg1, sort's custom
             * value-or-lvalue arg0 - unchanged. */
            if (node->vm_dyn_callee
                && b.kind != Builtin::Kind::lazy
                && b.kind != Builtin::Kind::node) {
                /* An INDIRECT builtin call is EAGER-ARGS by language rule
                 * (F1 step 2): evaluate the args RAW, left-to-right (an
                 * id/subscript/member arg keeps its LValue* / UndefinedId -
                 * the by-ref encoding), then the SHARED values dispatch -
                 * the same code the VM's CallValueGenericV runs, so the two
                 * engines agree on the func_lv by-ref arg0, the map/filter
                 * order, and where an undefined name surfaces (the
                 * consumer). The lazy kinds keep the node path (the REPL's
                 * indirect defined/isconst/show - scripts compile-reject
                 * their value uses). */
                ExprList *ael = node->args.get();
                const size_t n = ael->elems.size();
                EvalValue vbuf[8];
                ArgLoc lbuf[8];
                std::vector<EvalValue> vheap;
                std::vector<ArgLoc> lheap;
                EvalValue *argv = vbuf;
                ArgLoc *locs = lbuf;
                if (n > 8) {
                    vheap.resize(n);
                    lheap.resize(n);
                    argv = vheap.data();
                    locs = lheap.data();
                }
                LValue hold0;   /* indirect_lv_arg0's (a variable root:
                                 * never filled) */
                for (size_t i = 0; i < n; i++) {
                    argv[i] = i == 0 && b.kind == Builtin::Kind::lvalue
                        ? indirect_lv_arg0(ctx, ael->elems[0].get(), hold0)
                        : ael->elems[i]->eval(ctx);
                    locs[i] = ArgLoc{ael->elems[i]->start,
                                     ael->elems[i]->end};
                }
                ArgLocs al;
                al.start = ael->start;
                al.end = ael->end;
                al.args = locs;
                al.nargs = n;
                al.arr_hint = ael->arr_hint;
                return dispatch_builtin_values(ctx, b, &al, argv, n);
            }
            return b.func(ctx, node->args.get());
        }

        if (callable.is<intrusive_ptr<FuncObject>>()) {
            return do_func_call(
                ctx,
                *callable.get<intrusive_ptr<FuncObject>>().get(),
                node->args->elems,
                node->start,     /* call site = the CallExpr's location */
                node->inline_ctx,/* virtual frames if this call is inlined */
                ck, pc, as_signal
            );
        }

        /* Calling a struct type descriptor constructs an instance. By this
         * point a named call has been desugared to positional. */
        if (callable.is<StructTypeDef *>())
            return construct_struct(ctx, callable.get<StructTypeDef *>(),
                                    node->args.get());

    } catch (Exception &e) {
        stamp_args_loc(e, node->args.get());     /* RULE 2: see above */
        throw;
    }

    throw NotCallableEx(node->what->start, node->what->end);
}

EvalValue CallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /* A FOLDED type query (type/decltype/typestr/kindstr): the inferencer baked
     * the answer into args[0], so the call just returns it - hand it back
     * WITHOUT dispatching to the builtin. Consistent with the VM's codegen
     * elision; and it is what lets the builtin `func` always build-from-value
     * (the non-folded case), since the folded literal never reaches it. */
    if (tq_folded)
        return RValue(args->elems[0]->eval(ctx));

    EvalValue callable_storage;

    /* Point an undefined-callee error at the callee, not the whole call. */
    try {
        callable_storage = RValue(what->eval(ctx));
    } catch (Exception &e) {
        stamp_operand_loc(what.get(), e);
        throw;
    }

    return dispatch_call_value(ctx, callable_storage, this,
                               nullptr, 0, false);
}

/*
 * Devirtualized direct call. The resolver proved the callee is a global-table
 * slot (a top-level / scoped function, an escaped global, or a struct
 * descriptor) and recorded it; the swap pass (resolver.cpp) turned that CallExpr
 * into this node so its do_eval is SEPARATE code - CallExpr::do_eval is left
 * byte-for-byte unchanged, so plain calls (incl. builtin calls in tight loops)
 * are not perturbed. Read the callee straight from the slot: no callee eval (and
 * its Construct::eval wrapper), no RValue copy / refcount bump, no Builtin/Func/
 * Struct dispatch. The is<FuncObject> check keeps it sound - a struct
 * construction, a slot reassigned to a non-function, an undefined slot, or the
 * REPL (no global table) falls back to the full CallExpr path.
 */
EvalValue DirectCallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    if (ctx->gfuncs && ctx->gfuncs->defined[direct_func_slot]) {
        const EvalValue &fv = ctx->gfuncs->slots[direct_func_slot].get();
        if (fv.is<intrusive_ptr<FuncObject>>()) {
            try {
                return do_func_call(
                    ctx,
                    *fv.get<intrusive_ptr<FuncObject>>().get(),
                    args->elems,
                    start,
                    inline_ctx
                );
            } catch (Exception &e) {
                stamp_args_loc(e, args.get());
                throw;
            }
        }
    }
    return CallExpr::do_eval(ctx, rec);
}

/*
 * Devirtualized call to a CACHEABLE pure recursive function (see PureCache):
 * like DirectCallExpr but routes through cached_call so the per-frame cache
 * dedups the recursion's duplicate self-calls. A SEPARATE node so the plain
 * DirectCallExpr path pays NO per-call cache check (the cost model that made
 * DirectCallExpr worth it). Created by the devirt pass only for a global func
 * the inliner marked cache_results; falls back to CallExpr if the slot isn't a
 * FuncObject (a struct ctor, a reassigned slot, the REPL).
 */
EvalValue CachedCallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    if (ctx->gfuncs && ctx->gfuncs->defined[direct_func_slot]) {
        const EvalValue &fv = ctx->gfuncs->slots[direct_func_slot].get();
        if (fv.is<intrusive_ptr<FuncObject>>()) {
            try {
                return cached_call(
                    ctx,
                    *fv.get<intrusive_ptr<FuncObject>>().get(),
                    args->elems,
                    start,
                    inline_ctx
                );
            } catch (Exception &e) {
                stamp_args_loc(e, args.get());   /* RULE 2: see above */
                throw;
            }
        }
    }
    return CallExpr::do_eval(ctx, rec);
}

/*
 * Devirtualized builtin call (see DirectBuiltinCallExpr). Calls the baked
 * builtin function pointer directly - no callee eval, RValue, or dispatch. The
 * builtin gets the caller's ctx + the unevaluated args, exactly as the generic
 * path. The try/catch reproduces the generic path's behavior of stamping the
 * argument-list loc onto a loc-less error from the builtin, so error reporting
 * is identical.
 */
EvalValue DirectBuiltinCallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /* A folded type query returns its baked args[0] (see CallExpr::do_eval). */
    if (tq_folded)
        return RValue(args->elems[0]->eval(ctx));

    try {
        return builtin.func(ctx, args.get());
    } catch (Exception &e) {
        if (!e.loc_start) {
            e.loc_start = args->start;
            e.loc_end = args->end;
        }
        throw;
    }
}

/*
 * An inlined block-bodied call. Run the (already param-substituted) body with
 * its OWN FlowState boundary, so a `return` inside the body terminates HERE and
 * yields this expression's value - it does NOT return from the caller's
 * function. The boundary is a stack-local FlowState that we point ctx->flow at
 * for the body's duration, then restore (also on an exception). No child
 * EvalContext is built - the body runs in the SAME ctx (its param refs were
 * substituted away and v1 bodies have no locals, so there are no callee-frame
 * slots to bind; the body block is scope_free, so it executes in place). Far
 * cheaper than a real call: no EvalContext, no Frame::init, no param binding,
 * no callee lookup. A fall-through body yields `none`, matching call semantics.
 */
EvalValue InlinedCallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    FlowState my_flow;
    FlowState *const saved = ctx->flow;
    ctx->flow = &my_flow;

    try {
        elem->eval(ctx);
    } catch (...) {
        ctx->flow = saved;
        throw;
    }

    ctx->flow = saved;
    return my_flow.type == FlowState::ret ? my_flow.value : none;
}

/*
 * Build an array VALUE from `n` already-evaluated element values, honoring the
 * `hint` (flat int/float/bool/struct storage vs general). Shared by the
 * tree-walker (LiteralArray::do_eval, which evaluates its element nodes into a
 * buffer first) and the VM's MakeArrayV op (which reads the values from
 * frame-slot registers) - so both build byte-identically with no per-element
 * node re-eval on the VM side. The only context it needs is `is_const` (whether
 * the elements' LValues are read-only, i.e. we are in a const-eval context).
 */
EvalValue build_array_from_values(const EvalValue *vals, size_t n,
                                  ArrHint hint,
                                  const StructTypeDef *hint_struct,
                                  bool is_const)
{
    if (!n) {
        /* An empty array with a flat destination type (`array<int> a;`,
         * `array<POD struct> a;`, ...): start flat so a built-up
         * `append(a, ...)` stays unboxed - the destination type, not the
         * (empty) value, drives the representation. */
        if (hint == ArrHint::flat_s && hint_struct) {
            StructTypeDef *def = const_cast<StructTypeDef *>(hint_struct);
            return SharedArrayObj(
                SharedArrayObj::svec_type({}, def, def->size));
        }
        if (hint == ArrHint::flat_i)
            return SharedArrayObj(SharedArrayObj::ivec_type{});
        if (hint == ArrHint::flat_f)
            return SharedArrayObj(SharedArrayObj::fvec_type{});
        if (hint == ArrHint::flat_b)
            return SharedArrayObj(SharedArrayObj::bvec_type{});
        /* A general empty `[]` must be a FRESH MUTABLE array, not the shared
         * read-only empty_arr singleton (like the flat hints above) - else a
         * later append() COW-clones instead of mutating in place (a function
         * appending to a passed-in `[]` wouldn't grow the caller). */
        return SharedArrayObj(SharedArrayObj::vec_type{});
    }

    /*
     * Build flat (unboxed) int/float storage when every element is that one
     * scalar kind - a literal's element types ARE its type, so this
     * value-driven check yields exactly the type-driven representation (plans/
     * type-driven-specialization.md). Optimistic: accumulate into the unboxed
     * vector while the kind holds, spilling to a general vector<LValue> at the
     * first off-kind element. A mixed literal is general.
     */
    SharedArrayObj::ivec_type ivec;
    SharedArrayObj::fvec_type fvec;
    SharedArrayObj::bvec_type bvec;
    SharedArrayObj::vec_type  gvec;
    /* mode 5: a flat array of same-type POD structs (their bytes packed) */
    std::vector<char> svecbuf;
    StructTypeDef *sdef = nullptr;
    int sstride = 0;

    auto is_pod_struct_of = [](const EvalValue &v, StructTypeDef *d) -> bool {
        return v.is<intrusive_ptr<StructObject>>() &&
               v.get<intrusive_ptr<StructObject>>()->is_pod() &&
               (d == nullptr || v.get<intrusive_ptr<StructObject>>()->def == d);
    };
    auto append_struct_bytes = [&](const EvalValue &v) {
        const StructObject &o = *v.get<intrusive_ptr<StructObject>>().get();
        const size_t at = svecbuf.size();
        svecbuf.resize(at + sstride);
        std::memcpy(svecbuf.data() + at, o.bytes.data(), sstride);
    };
    /*
     * 0 = empty, 1 = ints, 2 = floats, 4 = bools, 3 = general. Type-driven: a
     * literal bound to a dynamically-typed destination (arr_hint general, set by
     * the inferencer) is built general from the start, so a later mixed write to
     * it never has to promote. (The flat_i/flat_f/flat_b hints need no special
     * case - the value-driven scan already produces flat for an all-one-kind
     * literal, which is exactly when those hints are set.)
     */
    int mode = hint == ArrHint::general ? 3 : 0;
    if (mode == 3)
        gvec.reserve(n);

    auto spill_to_general = [&]() {
        gvec.reserve(n);
        if (mode == 1)
            for (int_type x : ivec)
                gvec.emplace_back(EvalValue(x), is_const);
        else if (mode == 2)
            for (float_type x : fvec)
                gvec.emplace_back(EvalValue(x), is_const);
        else if (mode == 4)
            for (unsigned char x : bvec)
                gvec.emplace_back(EvalValue(static_cast<bool>(x)), is_const);
        else if (mode == 5) {
            const size_t cnt = sstride ? svecbuf.size() / sstride : 0;
            for (size_t i = 0; i < cnt; i++) {
                auto o = make_intrusive<StructObject>(sdef);
                std::memcpy(o->bytes.data(),
                            svecbuf.data() + i * sstride, sstride);
                gvec.emplace_back(EvalValue(intrusive_ptr<StructObject>(o)),
                                  is_const);
            }
        }
        ivec.clear();
        fvec.clear();
        bvec.clear();
        svecbuf.clear();
        mode = 3;
    };

    for (size_t i = 0; i < n; i++) {

        const EvalValue &v = vals[i];

        if (mode == 0) {
            if (v.is<int_type>()) {
                mode = 1; ivec.push_back(v.get<int_type>());
            } else if (v.is<float_type>()) {
                mode = 2; fvec.push_back(v.get<float_type>());
            } else if (v.is<bool>()) {
                mode = 4; bvec.push_back(v.get<bool>() ? 1 : 0);
            } else if (is_pod_struct_of(v, nullptr)) {
                mode = 5;
                sdef = v.get<intrusive_ptr<StructObject>>()->def;
                sstride = sdef->size;
                svecbuf.reserve(n * sstride);
                append_struct_bytes(v);
            } else {
                mode = 3; gvec.reserve(n);
                gvec.emplace_back(v, is_const);
            }
        } else if (mode == 1 && v.is<int_type>()) {
            ivec.push_back(v.get<int_type>());
        } else if (mode == 2 && v.is<float_type>()) {
            fvec.push_back(v.get<float_type>());
        } else if (mode == 4 && v.is<bool>()) {
            bvec.push_back(v.get<bool>() ? 1 : 0);
        } else if (mode == 5 && is_pod_struct_of(v, sdef)) {
            append_struct_bytes(v);
        } else {
            if (mode != 3)
                spill_to_general();
            gvec.emplace_back(v, is_const);
        }
    }

    if (mode == 1) return SharedArrayObj(std::move(ivec));
    if (mode == 2) return SharedArrayObj(std::move(fvec));
    if (mode == 4) return SharedArrayObj(std::move(bvec));
    if (mode == 5)
        return SharedArrayObj(
            SharedArrayObj::svec_type(std::move(svecbuf), sdef, sstride));
    return SharedArrayObj(std::move(gvec));
}

/* A container literal's numeric WIDENING (LiteralArray::elem_coerce,
 * LiteralDict::val_coerce): int/bool -> float, bool -> int; every other
 * value (none included) as it is. The VM applies the same rule with
 * CoerceNumV on the element's run slot. */
static EvalValue literal_widen(EvalValue v, DeclType dt)
{
    if (dt == DeclType::f) {
        if (v.is<int_type>())
            return EvalValue(static_cast<float_type>(v.get<int_type>()));
        if (v.is<bool>())
            return EvalValue(static_cast<float_type>(v.get<bool>() ? 1 : 0));
    } else if (dt == DeclType::i && v.is<bool>()) {
        return EvalValue(static_cast<int_type>(v.get<bool>() ? 1 : 0));
    }
    return v;
}

/* a fully CONSTANT literal's values, widened by their own join
 * (const_values_widen, below arr_elem_boxed) */
static void const_values_widen(EvalValue *vals, size_t n, size_t stride,
                               bool is_const);

EvalValue LiteralArray::do_eval(EvalContext *ctx, bool rec) const
{
    /* Evaluate the element nodes into a buffer (stack for the common small
     * literal, heap for a large one), then hand off to the shared builder -
     * the same builder the VM's MakeArrayV op calls with register values. */
    const size_t n = elems.size();

    /* widened by the inferencer's stamp, or - a fully CONSTANT literal
     * built by the const evaluator, before any stamp exists - by the join
     * of its own values (const_values_widen) */
    const auto widen = [&](EvalValue *buf) {
        if (elem_coerce != DeclType::none) {
            for (size_t i = 0; i < n; i++)
                buf[i] = literal_widen(std::move(buf[i]), elem_coerce);
        } else if (is_const && !unpacked && ctx->in_const_eval()) {
            /* only the PARSE-TIME build: at run time an unstamped literal
             * means no inference ran (-nti), so no typed reader trusts
             * a type, and the VM builds it unwidened too */
            const_values_widen(buf, n, 1, ctx->const_ctx);
        }
    };
    if (n <= 16) {
        EvalValue buf[16];
        for (size_t i = 0; i < n; i++)
            buf[i] = RValue(elems[i]->eval(ctx));
        widen(buf);
        return build_array_from_values(buf, n, arr_hint, arr_hint_struct,
                                       ctx->const_ctx);
    }

    std::vector<EvalValue> buf(n);
    for (size_t i = 0; i < n; i++)
        buf[i] = RValue(elems[i]->eval(ctx));
    widen(buf.data());
    return build_array_from_values(buf.data(), n, arr_hint, arr_hint_struct,
                                   ctx->const_ctx);
}

/*
 * Produce a fresh, mutable copy of a const-evaluated container value.
 *
 * `through_readonly` controls how read-only (const-backed) sub-objects are
 * handled:
 *   - false (make_mutable_clone): a read-only sub-object is *shared* as-is, not
 *     copied. So a fresh mutable top is built, but const sub-objects stay const
 *     and shared. This keeps clone() shallow (a const inside the result remains
 *     read-only) and makes const-ness propagate into fresh literals
 *     (`var a = [y]` with `y` const keeps `a[0]` read-only). Mutable
 *     sub-objects are still copied fresh, so re-evaluating the node never
 *     observes a prior mutation.
 *   - true (make_deep_mutable_clone): every level is copied and made mutable
 *     (read-only dropped), yielding a fully independent, writable value. This
 *     backs deepclone().
 * Scalars and strings are returned as-is. An empty array collapses to the
 * shared empty_arr singleton, matching LiteralArray.
 */
static EvalValue
clone_to_mutable(const EvalValue &v, bool through_readonly)
{
    if (v.is<SharedArrayObj>()) {

        const SharedArrayObj &arr = v.get<SharedArrayObj>();

        if (arr.is_readonly() && !through_readonly)
            return v;   /* share the const sub-object, don't copy it */

        /* An empty array must NOT collapse to the shared read-only empty_arr
         * singleton here: this is a MUTABLE clone (make_mutable_clone /
         * make_deep_mutable_clone), so `var x = []` needs a FRESH MUTABLE empty
         * of the source's kind - else `x` aliases the read-only singleton and a
         * later append() COW-clones instead of mutating in place (so a function
         * that appends to a passed-in `[]` won't grow the caller's array). The
         * empty flows through the kind-specific copies below, which build a
         * fresh empty vector of the right storage. (empty_arr stays the shared
         * for CONST empties, via make_const_clone - a different path.) */

        /*
         * Flat homogeneous array: copy into fresh mutable unboxed storage. The
         * elements are scalars (nothing to recurse into), so this is sound for
         * both make_mutable_clone and make_deep_mutable_clone.
         */
        if (arr.skind() == SharedArrayObj::Storage::ints) {
            const auto &iv = arr.flat_ints();
            return SharedArrayObj(SharedArrayObj::ivec_type(
                iv.cbegin() + arr.offset(),
                iv.cbegin() + arr.offset() + arr.size()
            ));
        }

        if (arr.skind() == SharedArrayObj::Storage::floats) {
            const auto &fv = arr.flat_floats();
            return SharedArrayObj(SharedArrayObj::fvec_type(
                fv.cbegin() + arr.offset(),
                fv.cbegin() + arr.offset() + arr.size()
            ));
        }

        if (arr.skind() == SharedArrayObj::Storage::bools) {
            const auto &bv = arr.flat_bools();
            return SharedArrayObj(SharedArrayObj::bvec_type(
                bv.cbegin() + arr.offset(),
                bv.cbegin() + arr.offset() + arr.size()
            ));
        }

        /* Flat POD-struct array: a byte copy keeps it flat (POD bytes hold no
         * references, so it is a full mutable copy). */
        if (arr.skind() == SharedArrayObj::Storage::structs) {
            const auto &sv = arr.flat_structs();
            std::vector<char> nb(
                sv.buf.cbegin() + arr.offset() * sv.stride,
                sv.buf.cbegin() + (arr.offset() + arr.size()) * sv.stride
            );
            return SharedArrayObj(
                SharedArrayObj::svec_type(std::move(nb), sv.def, sv.stride));
        }

        /* Flat STRING array: strings are immutable, so a handle copy IS a
         * full mutable-top copy - stays flat. */
        if (arr.skind() == SharedArrayObj::Storage::strs) {
            const auto &tv = arr.flat_strs();
            return SharedArrayObj(SharedArrayObj::tvec_type(
                tv.cbegin() + arr.offset(),
                tv.cbegin() + arr.offset() + arr.size()
            ));
        }

        const ArrayConstView &view = arr.get_view();

        SharedArrayObj::vec_type vec;
        vec.reserve(view.size());

        for (unsigned i = 0; i < view.size(); i++) {
            vec.emplace_back(
                clone_to_mutable(view[i].get(), through_readonly), false
            );
        }

        return SharedArrayObj(std::move(vec));
    }

    if (v.is<intrusive_ptr<DictObject>>()) {

        const auto &obj = v.get<intrusive_ptr<DictObject>>();

        if (obj->is_readonly() && !through_readonly)
            return v;   /* share the const sub-object, don't copy it */

        DictObject::inner_type data;

        for (const auto &p : obj->get_ref()) {
            data.emplace(
                p.first,
                LValue(
                    clone_to_mutable(p.second.get(), through_readonly),
                    false
                )
            );
        }

        auto out = make_intrusive<DictObject>(std::move(data));
        if (obj->get_has_default())   /* preserve the default-dict default */
            out->set_default(
                clone_to_mutable(obj->get_default(), through_readonly));
        return intrusive_ptr<DictObject>(out);
    }

    if (v.is<intrusive_ptr<StructObject>>()) {

        const auto &obj = v.get<intrusive_ptr<StructObject>>();

        if (obj->is_readonly() && !through_readonly)
            return v;   /* share the const sub-object, don't copy it */

        /* POD: a byte copy is a full mutable copy (no nested references). */
        if (obj->is_pod()) {
            auto out = make_intrusive<StructObject>(*obj);   /* copies bytes */
            out->clear_readonly();
            return intrusive_ptr<StructObject>(out);
        }

        auto out = make_intrusive<StructObject>(obj->def);
        out->fields.reserve(obj->fields.size());
        for (const auto &f : obj->fields)
            out->fields.emplace_back(
                clone_to_mutable(f.get(), through_readonly), false);
        return intrusive_ptr<StructObject>(out);
    }

    return v;
}

EvalValue make_mutable_clone(const EvalValue &v)
{
    return clone_to_mutable(v, false);
}

EvalValue make_deep_mutable_clone(const EvalValue &v)
{
    return clone_to_mutable(v, true);
}

/*
 * Produce a deep, read-only copy of a const-evaluated container value: every
 * array/dict in the result (recursively) is flagged read-only, so writing
 * through ANY alias of it - including a non-const function parameter bound to a
 * const argument - is rejected. This is what makes `const` deep read-only at
 * runtime; previously const-ness was enforced only by parse-time folding of
 * direct reads, leaving the runtime value mutable through aliasing. Scalars and
 * strings are returned as-is (already immutable). An empty array gets its own
 * read-only object (not the shared empty_arr singleton, which must stay
 * mutable).
 */
EvalValue
make_const_clone(const EvalValue &v)
{
    if (v.is<SharedArrayObj>()) {

        const SharedArrayObj &src = v.get<SharedArrayObj>();

        /*
         * Flat homogeneous array: bake a flat read-only copy, keeping the
         * unboxed storage. The elements are scalars, so there is nothing to
         * recurse into - a const flat int/float array stays flat (and so does
         * everything cloned from it, since clone_internal_vec is kind-aware).
         */
        if (src.skind() == SharedArrayObj::Storage::ints) {
            const auto &iv = src.flat_ints();
            SharedArrayObj::ivec_type nv(
                iv.cbegin() + src.offset(),
                iv.cbegin() + src.offset() + src.size()
            );
            SharedArrayObj arr(std::move(nv));
            arr.set_readonly();
            return arr;
        }

        if (src.skind() == SharedArrayObj::Storage::floats) {
            const auto &fv = src.flat_floats();
            SharedArrayObj::fvec_type nv(
                fv.cbegin() + src.offset(),
                fv.cbegin() + src.offset() + src.size()
            );
            SharedArrayObj arr(std::move(nv));
            arr.set_readonly();
            return arr;
        }

        if (src.skind() == SharedArrayObj::Storage::bools) {
            const auto &bv = src.flat_bools();
            SharedArrayObj::bvec_type nv(
                bv.cbegin() + src.offset(),
                bv.cbegin() + src.offset() + src.size()
            );
            SharedArrayObj arr(std::move(nv));
            arr.set_readonly();
            return arr;
        }

        /* Flat POD-struct array: a deep read-only byte copy (POD bytes hold no
         * nested references to recurse into). */
        if (src.skind() == SharedArrayObj::Storage::structs) {
            const auto &sv = src.flat_structs();
            std::vector<char> nb(
                sv.buf.cbegin() + src.offset() * sv.stride,
                sv.buf.cbegin() + (src.offset() + src.size()) * sv.stride
            );
            SharedArrayObj arr(
                SharedArrayObj::svec_type(std::move(nb), sv.def, sv.stride));
            arr.set_readonly();
            return arr;
        }

        /* Flat STRING array: strings are immutable - nothing to recurse
         * into; a handle copy + readonly is the deep const clone. */
        if (src.skind() == SharedArrayObj::Storage::strs) {
            const auto &tv = src.flat_strs();
            SharedArrayObj arr(SharedArrayObj::tvec_type(
                tv.cbegin() + src.offset(),
                tv.cbegin() + src.offset() + src.size()
            ));
            arr.set_readonly();
            return arr;
        }

        const ArrayConstView &view = src.get_view();

        SharedArrayObj::vec_type vec;
        vec.reserve(view.size());

        for (unsigned i = 0; i < view.size(); i++)
            vec.emplace_back(make_const_clone(view[i].get()), false);

        SharedArrayObj arr(std::move(vec));
        arr.set_readonly();
        return arr;
    }

    if (v.is<intrusive_ptr<DictObject>>()) {

        const DictObject &src_obj = *v.get<intrusive_ptr<DictObject>>().get();
        DictObject::inner_type data;

        for (const auto &p : src_obj.get_ref()) {
            data.emplace(
                p.first,
                LValue(make_const_clone(p.second.get()), false)
            );
        }

        auto obj = make_intrusive<DictObject>(std::move(data));
        if (src_obj.get_has_default())   /* preserve the default-dict default */
            obj->set_default(make_const_clone(src_obj.get_default()));
        obj->set_readonly();
        return intrusive_ptr<DictObject>(obj);
    }

    if (v.is<intrusive_ptr<StructObject>>()) {

        const StructObject &src = *v.get<intrusive_ptr<StructObject>>().get();

        /* POD: bytes hold no references, so a byte copy is a deep copy. */
        if (src.is_pod()) {
            auto obj = make_intrusive<StructObject>(src);   /* copies bytes */
            obj->set_readonly();
            return intrusive_ptr<StructObject>(obj);
        }

        auto obj = make_intrusive<StructObject>(src.def);
        obj->fields.reserve(src.fields.size());
        for (const auto &f : src.fields)
            obj->fields.emplace_back(make_const_clone(f.get()), false);
        obj->set_readonly();
        return intrusive_ptr<StructObject>(obj);
    }

    return v;
}

/*
 * Build a fresh, mutable GENERAL array from a flat (unboxed int/float) array,
 * reading its scalar elements directly (no promotion of the source). Used to
 * materialize a folded array literal whose destination is dynamically typed
 * (arr_hint general): the array is born general so a later mixed write to it
 * never has to promote. Only a flat source reaches here - an already-general
 * baked value is handled by make_mutable_clone, which keeps it general.
 */
static EvalValue
make_general_array_clone(const SharedArrayObj &src)
{
    const size_type m = src.size();
    SharedArrayObj::vec_type gv;
    gv.reserve(m);

    if (src.skind() == SharedArrayObj::Storage::ints) {
        const auto &iv = src.flat_ints();
        for (size_type i = 0; i < m; i++)
            gv.emplace_back(EvalValue(iv[src.offset() + i]), false);
    } else if (src.skind() == SharedArrayObj::Storage::floats) {
        const auto &fv = src.flat_floats();
        for (size_type i = 0; i < m; i++)
            gv.emplace_back(EvalValue(fv[src.offset() + i]), false);
    } else if (src.skind() == SharedArrayObj::Storage::strs) {
        const auto &tv = src.flat_strs();
        for (size_type i = 0; i < m; i++)
            gv.emplace_back(EvalValue(SharedStr(tv[src.offset() + i])),
                            false);
    } else {
        const auto &bv = src.flat_bools();
        for (size_type i = 0; i < m; i++)
            gv.emplace_back(EvalValue(static_cast<bool>(bv[src.offset() + i])),
                            false);
    }

    return SharedArrayObj(std::move(gv));
}

/*
 * Read array element i (slice-relative) as a boxed value without promoting flat
 * (unboxed int/float) storage - the eval.cpp counterpart of types/arr.cpp.h's
 * arr_elem_at (a separate translation unit). For a general array get_view()
 * doesn't promote.
 */
static EvalValue
arr_elem_boxed(const SharedArrayObj &a, size_type i)
{
    switch (a.skind()) {
        case SharedArrayObj::Storage::ints:
            return EvalValue(a.flat_ints()[a.offset() + i]);
        case SharedArrayObj::Storage::floats:
            return EvalValue(a.flat_floats()[a.offset() + i]);
        case SharedArrayObj::Storage::bools:
            return EvalValue(static_cast<bool>(a.flat_bools()[a.offset() + i]));
        case SharedArrayObj::Storage::structs: {
            const auto &sv = a.flat_structs();
            auto obj = make_intrusive<StructObject>(sv.def);
            std::memcpy(obj->bytes.data(),
                        sv.buf.data() + (a.offset() + i) * sv.stride,
                        sv.stride);
            return intrusive_ptr<StructObject>(obj);
        }
        case SharedArrayObj::Storage::strs:
            return EvalValue(SharedStr(a.flat_strs()[a.offset() + i]));
        default:
            return a.get_view()[i].get();
    }
}

/* The numeric SHAPE a constant value's static type has - the part of
 * static_type_from_value (inferencer.cpp) the widening needs: the numeric
 * join bool <= int <= float, through arrays' elements and dicts' values,
 * `none` joining as nothing (it only makes a type opt), anything else
 * `other` (the join is dyn, and nothing widens). A constant literal is
 * built at parse time, before inference exists, so this is how its
 * values reach the type the inferencer will then give it (#75). */
namespace {
struct NumShape {
    enum Kind { bot, b, i, f, arr, dict, other } k = bot;
    std::unique_ptr<NumShape> sub;       /* arr: element, dict: value */
};

void shape_join(NumShape &a, const NumShape &x)
{
    using K = NumShape::Kind;
    if (x.k == K::bot || a.k == K::other)
        return;
    if (a.k == K::bot) {
        a.k = x.k;
        if (x.sub) {
            a.sub = std::make_unique<NumShape>();
            shape_join(*a.sub, *x.sub);
        }
        return;
    }
    const bool an = a.k == K::b || a.k == K::i || a.k == K::f;
    const bool xn = x.k == K::b || x.k == K::i || x.k == K::f;
    if (an && xn) {
        a.k = std::max(a.k, x.k);
    } else if (a.k == x.k && (a.k == K::arr || a.k == K::dict)) {
        if (x.sub)
            shape_join(*a.sub, *x.sub);
    } else {
        a.k = K::other;
        a.sub.reset();
    }
}

NumShape shape_of(const EvalValue &v)
{
    NumShape s;
    if (v.is<NoneVal>())
        return s;
    if (v.is<bool>()) { s.k = NumShape::b; return s; }
    if (v.is<int_type>()) { s.k = NumShape::i; return s; }
    if (v.is<float_type>()) { s.k = NumShape::f; return s; }
    if (v.is<SharedArrayObj>()) {
        const auto &a = v.get_ref<SharedArrayObj>();
        s.k = NumShape::arr;
        s.sub = std::make_unique<NumShape>();
        for (size_type i = 0; i < a.size(); i++)
            shape_join(*s.sub, shape_of(arr_elem_boxed(a, i)));
        return s;
    }
    if (v.is<intrusive_ptr<DictObject>>()) {
        s.k = NumShape::dict;
        s.sub = std::make_unique<NumShape>();
        for (const auto &kv : v.get_ref<intrusive_ptr<DictObject>>()
                                   ->get_ref())
            shape_join(*s.sub, shape_of(kv.second.get()));
        return s;
    }
    s.k = NumShape::other;
    return s;
}

/* `v` widened to shape `s`; `changed` says whether anything moved. A
 * container is REBUILT, never edited: it may be a shared constant
 * reached through a name (`const N = [[1, 2.5], A];` - A keeps its own
 * type) */
EvalValue shape_widen(const EvalValue &v, const NumShape &s, bool is_const,
                      bool &changed)
{
    changed = false;
    if (s.k == NumShape::f && (v.is<int_type>() || v.is<bool>())) {
        changed = true;
        return EvalValue(static_cast<float_type>(
            v.is<bool>() ? (v.get<bool>() ? 1 : 0) : v.get<int_type>()));
    }
    if (s.k == NumShape::i && v.is<bool>()) {
        changed = true;
        return EvalValue(static_cast<int_type>(v.get<bool>() ? 1 : 0));
    }
    if (s.k == NumShape::arr && v.is<SharedArrayObj>()) {
        const auto &a = v.get_ref<SharedArrayObj>();
        std::vector<EvalValue> buf(a.size());
        bool any = false;
        for (size_type i = 0; i < a.size(); i++) {
            bool c;
            buf[i] = shape_widen(arr_elem_boxed(a, i), *s.sub, is_const, c);
            any |= c;
        }
        if (!any)
            return v;
        changed = true;
        EvalValue r = build_array_from_values(buf.data(), buf.size(),
                                              ArrHint::dflt, nullptr,
                                              is_const);
        if (a.is_readonly())
            r = make_const_clone(r);
        return r;
    }
    if (s.k == NumShape::dict && v.is<intrusive_ptr<DictObject>>()) {
        const auto &d = *v.get_ref<intrusive_ptr<DictObject>>();
        if (d.get_has_default())
            return v;
        std::vector<EvalValue> buf;
        buf.reserve(2 * d.get_ref().size());
        bool any = false;
        for (const auto &kv : d.get_ref()) {
            bool c;
            buf.push_back(kv.first);
            buf.push_back(shape_widen(kv.second.get(), *s.sub, is_const, c));
            any |= c;
        }
        if (!any)
            return v;
        changed = true;
        EvalValue r = build_dict_from_pairs(buf.data(), buf.size() / 2,
                                            is_const);
        if (d.is_readonly())
            r = make_const_clone(r);
        return r;
    }
    return v;
}
} // namespace

static void const_values_widen(EvalValue *vals, size_t n, size_t stride,
                               bool is_const)
{
    NumShape s;
    for (size_t i = 0; i < n; i++)
        shape_join(s, shape_of(vals[i * stride]));
    if (s.k == NumShape::bot || s.k == NumShape::other)
        return;
    for (size_t i = 0; i < n; i++) {
        bool c;
        EvalValue w = shape_widen(vals[i * stride], s, is_const, c);
        if (c)
            vals[i * stride] = std::move(w);
    }
}

/*
 * The value a LiteralObj (a baked const array/dict/struct) materializes -
 * factored out of LiteralObj::do_eval so the VM's LoadLiteralObjV op reproduces
 * it BYTE-for-BYTE from a baked pool entry (value + immutable + arr_hint +
 * arr_hint_struct), AST-free (no LiteralObj node needed at run time).
 */
EvalValue eval_literal_obj(const EvalValue &value, bool immutable,
                           ArrHint arr_hint,
                           const StructTypeDef *arr_hint_struct)
{
    /*
     * A const-decl target (immutable) shares the baked value directly: it was
     * baked deep read-only (make_const_clone, in the parser), so it can't be
     * mutated and re-entry can safely observe the same object - no per-eval
     * copy, and the const symbol and this node hold one buffer, not two. Any
     * other target (a `var` or a read-only consumer) gets a fresh mutable deep
     * copy, so writes work and re-entry never sees a prior mutation.
     *
     * Type-driven: when the destination is dynamically typed (arr_hint general)
     * but the baked array is flat (a homogeneous literal like [1,2,3] later
     * widened to array<dyn> by a mixed write), materialize it general from the
     * start so that write never promotes.
     */
    if (!immutable && arr_hint == ArrHint::general && value.is<SharedArrayObj>()
        && value.get<SharedArrayObj>().skind()
               != SharedArrayObj::Storage::general)
        return make_general_array_clone(value.get<SharedArrayObj>());

    /* An empty baked array bound to an array<POD struct> destination starts
     * flat (the const-fold erased the element type, so the hint restores it),
     * so a built-up `var a = []; append(a, S(..))` stays unboxed. */
    if (arr_hint == ArrHint::flat_s && arr_hint_struct &&
        value.is<SharedArrayObj>() && value.get<SharedArrayObj>().size() == 0) {
        StructTypeDef *def = const_cast<StructTypeDef *>(arr_hint_struct);
        return SharedArrayObj(SharedArrayObj::svec_type({}, def, def->size));
    }

    /* The SCALAR twin (2026-07-18 profile #1): an empty baked `[]` whose
     * destination is a known array<int>/<float>/<bool> - annotated OR
     * inferred (`var a = []; push(a, i)` fixpoints to array<int>) - starts
     * FLAT. The const-fold baked it before inference (element type
     * unknowable), so only the hint can restore the representation; without
     * this arm the entire grow-from-empty class ran on 48-byte general
     * LValues (bench 13's shape). */
    if (value.is<SharedArrayObj>()
        && value.get<SharedArrayObj>().size() == 0) {
        if (arr_hint == ArrHint::flat_i)
            return SharedArrayObj(SharedArrayObj::ivec_type{});
        if (arr_hint == ArrHint::flat_f)
            return SharedArrayObj(SharedArrayObj::fvec_type{});
        if (arr_hint == ArrHint::flat_b)
            return SharedArrayObj(SharedArrayObj::bvec_type{});
    }

    return immutable ? value : make_mutable_clone(value);
}

EvalValue LiteralObj::do_eval(EvalContext *ctx, bool rec) const
{
    return eval_literal_obj(value, immutable, arr_hint, arr_hint_struct);
}

/*
 * Attach `c`'s source location to an in-flight exception that has none, so a
 * caret points at the offending sub-expression (operand) instead of the whole
 * enclosing expression. Used wherever an operand's RValue or operator
 * application can throw (undefined variable, type mismatch, division by zero).
 */
static void
stamp_operand_loc(const Construct *c, Exception &e)
{
    if (!e.loc_start) {
        e.loc_start = c->start;
        e.loc_end = c->end;
    }

    /*
     * Operator-ladder errors are stamped at the offending operand, but the
     * inlined-at frames are emitted by the enclosing node's Construct::eval
     * (keyed off inline_origin_emitted, which the loc-stamp above does not
     * gate), so there's nothing to flush here.
     */
}

/* `acc OP= operand`, with operand-precise error locations. */
static void
num_binop_loc(EvalValue &acc, const Construct *operand, EvalContext *ctx,
              NumBinOp op)
{
    try {
        num_bin_op(acc, RValue(operand->eval(ctx)), op);
    } catch (Exception &e) {
        stamp_operand_loc(operand, e);
        throw;
    }
}

/* Same, for the short-circuiting logical operators (&& and ||). */
static void
logop_loc(EvalValue &acc, const Construct *operand, EvalContext *ctx, Op op)
{
    /*
     * `&&` / `||` operate on truthiness (the unchanged truthy rules:
     * 0/none/[]/{} are false, everything else true) and yield a bool. This
     * works for any operand type, not just int, and returns a real bool.
     */
    try {
        const bool a = acc.is_true();
        const bool b = RValue(operand->eval(ctx)).is_true();
        acc = EvalValue(op == Op::land ? (a && b) : (a || b));
    } catch (Exception &e) {
        stamp_operand_loc(operand, e);
        throw;
    }
}

EvalValue MultiOpConstruct::eval_first_rvalue(EvalContext *ctx) const
{
    assert(elems.size() >= 1 && elems[0].first == Op::invalid);

    if (elems.size() == 1)
        return elems[0].second->eval(ctx);

    /* Stamp the first operand's loc on an undefined-variable error. */
    try {
        return RValue(elems[0].second->eval(ctx));
    } catch (Exception &e) {
        stamp_operand_loc(elems[0].second.get(), e);
        throw;
    }
}

EvalValue Expr02::do_eval(EvalContext *ctx, bool rec) const
{
    assert(elems.size() == 1 || elems.size() == 2);
    const auto &[op, e] = elems[0];

    if (op == Op::invalid)
        return e->eval(ctx);

    try {

        EvalValue &&val = RValue(e->eval(ctx)).clone();

        switch (op) {
            case Op::plus:
                /* Unary operator '+': promote a bool to int 0/1, else no-op */
                if (val.is<bool>())
                    val = static_cast<int_type>(val.get<bool>() ? 1 : 0);
                break;
            case Op::minus:
                /* Unary operator '-': negate (a bool promotes to int first) */
                if (val.is<bool>())
                    val = static_cast<int_type>(val.get<bool>() ? 1 : 0);
                val.get_type()->opneg(val);
                break;
            case Op::lnot:
                /* Unary '!': logical not of the truthiness, yielding a bool */
                val = EvalValue(!val.is_true());
                break;
            case Op::bnot:
                /* Unary '~': bitwise NOT (a bool promotes to int 0/1 first) */
                if (val.is<bool>())
                    val = static_cast<int_type>(val.get<bool>() ? 1 : 0);
                val.get_type()->bnot(val);
                break;
            default:
                throw InternalErrorEx();
        }

        return std::move(val);

    } catch (Exception &ex) {
        stamp_operand_loc(e.get(), ex);
        throw;
    }
}

EvalValue Expr05::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::shl:  num_binop_loc(val, e.get(), ctx, &Type::shl);  break;
            case Op::shr:  num_binop_loc(val, e.get(), ctx, &Type::shr);  break;
            case Op::ushr: num_binop_loc(val, e.get(), ctx, &Type::ushr); break;
            default:       throw InternalErrorEx();
        }
    }

    return std::move(val);
}

EvalValue Expr08::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        if (op != Op::band)
            throw InternalErrorEx();

        num_binop_loc(val, e.get(), ctx, &Type::band);
    }

    return std::move(val);
}

EvalValue Expr09::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        if (op != Op::bxor)
            throw InternalErrorEx();

        num_binop_loc(val, e.get(), ctx, &Type::bxor);
    }

    return std::move(val);
}

EvalValue Expr10::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        if (op != Op::bor)
            throw InternalErrorEx();

        num_binop_loc(val, e.get(), ctx, &Type::bor);
    }

    return std::move(val);
}

EvalValue Expr03::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::times:
                num_binop_loc(val, e.get(), ctx, &Type::mul);
                break;
            case Op::div:
                num_binop_loc(val, e.get(), ctx, &Type::div);
                break;
            case Op::mod:
                num_binop_loc(val, e.get(), ctx, &Type::mod);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return std::move(val);
}

EvalValue Expr04::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::plus:
                num_binop_loc(val, e.get(), ctx, &Type::add);
                break;
            case Op::minus:
                num_binop_loc(val, e.get(), ctx, &Type::sub);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return std::move(val);
}

EvalValue Expr06::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::lt:
                num_binop_loc(val, e.get(), ctx, &Type::lt);
                break;
            case Op::gt:
                num_binop_loc(val, e.get(), ctx, &Type::gt);
                break;
            case Op::le:
                num_binop_loc(val, e.get(), ctx, &Type::le);
                break;
            case Op::ge:
                num_binop_loc(val, e.get(), ctx, &Type::ge);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    /* an ordering comparison (<, >, <=, >=) yields a bool */
    return EvalValue(val.is_true());
}

EvalValue Expr07::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::eq:
                num_binop_loc(val, e.get(), ctx, &Type::eq);
                break;
            case Op::noteq:
                num_binop_loc(val, e.get(), ctx, &Type::noteq);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    /* `==` / `!=` yield a bool */
    return EvalValue(val.is_true());
}

EvalValue Expr11::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::land:
                /* SHORT-CIRCUIT (#138): a false accumulator DETERMINES the
                 * result, so the right operand is not evaluated at all - its
                 * side effects do not happen and its errors are not raised.
                 * That is what makes `p != none && p.x > 0` a guard. */
                if (!val.is_true())
                    return EvalValue(false);
                logop_loc(val, e.get(), ctx, op);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return std::move(val);
}

EvalValue Expr12::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::lor:
                /* SHORT-CIRCUIT (#138) - the `||` twin: a TRUE accumulator
                 * determines the result. */
                if (val.is_true())
                    return EvalValue(true);
                logop_loc(val, e.get(), ctx, op);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return std::move(val);
}

/*
 * Evaluate a condition to a boolean. When the inferencer proved the condition
 * is a (non-null) int, take the unboxed eval_int() path - no LValue wrapper, no
 * EvalValue copy, no is_true() virtual. Otherwise the general path.
 */
static inline bool eval_cond(const Construct *c, EvalContext *ctx)
{
    if (c->th == TypeHint::i)
        return c->eval_int(ctx) != 0;
    return RValue(c->eval(ctx)).is_true();
}

/* --------------- TypedScalarExpr (M8 specialized scalar eval) ------------- */

template <class T>
static inline int_type typed_cmp(Op op, T a, T b)
{
    switch (op) {
        case Op::lt:    return a <  b;
        case Op::gt:    return a >  b;
        case Op::le:    return a <= b;
        case Op::ge:    return a >= b;
        case Op::eq:    return a == b;
        case Op::noteq: return a != b;
        default:        throw InternalErrorEx();
    }
}

int_type TypedScalarExpr::eval_int_body(EvalContext *ctx) const
{
    switch (cat) {

        case Cat::neg:
            if (kind == TypeHint::f)
                return static_cast<int_type>(-elems[0].second->eval_float(ctx));
            {
                const int_type v = elems[0].second->eval_int(ctx);
                ML_INT_ONLY(int_vc_arith('n', v, 0);)
                return -v;
            }

        case Cat::lnot:
            return elems[0].second->eval_int(ctx) == 0 ? 1 : 0;

        case Cat::arith: {
            if (kind == TypeHint::f)
                return static_cast<int_type>(eval_float_body(ctx));
            int_type acc = elems[0].second->eval_int(ctx);
            for (size_t i = 1; i < elems.size(); i++) {
                const int_type r = elems[i].second->eval_int(ctx);
                switch (elems[i].first) {
                    case Op::plus:
                        ML_INT_ONLY(int_vc_arith('+', acc, r);)
                        acc += r; break;
                    case Op::minus:
                        ML_INT_ONLY(int_vc_arith('-', acc, r);)
                        acc -= r; break;
                    case Op::times:
                        ML_INT_ONLY(int_vc_arith('*', acc, r);)
                        acc *= r; break;
                    /* div0 carets the DIVISOR operand (#76) - the boxed
                     * ladder's operand-precise convention, so the engines
                     * agree regardless of which path lowered the chain */
                    case Op::div:
                        ML_INT_ONLY(int_vc_divmod(false, acc, r);)
                        if (r == 0)
                            throw DivisionByZeroEx(elems[i].second->start,
                                                   elems[i].second->end);
                        check_int_div_overflow(acc, r,
                                               elems[i].second->start,
                                               elems[i].second->end);
                        acc /= r; break;
                    case Op::mod:
                        ML_INT_ONLY(int_vc_divmod(true, acc, r);)
                        if (r == 0)
                            throw DivisionByZeroEx(elems[i].second->start,
                                                   elems[i].second->end);
                        check_int_div_overflow(acc, r,
                                               elems[i].second->start,
                                               elems[i].second->end);
                        acc %= r; break;
                    case Op::band: acc &= r; break;
                    case Op::bor:  acc |= r; break;
                    case Op::bxor: acc ^= r; break;
                    case Op::shl:  acc = bit_shl(acc, r);  break;
                    case Op::shr:  acc = bit_shr(acc, r);  break;
                    case Op::ushr: acc = bit_ushr(acc, r); break;
                    default: throw InternalErrorEx();
                }
            }
            return acc;
        }

        case Cat::cmp:
            /* the operands LEFT TO RIGHT, explicitly: as two arguments of
             * typed_cmp their order was unspecified, and a GCC build
             * evaluated the RIGHT one first - `a() < b()` printed b before
             * a in the tree-walker alone (RULE 2) */
            if (kind == TypeHint::f) {
                const float_type x = elems[0].second->eval_float(ctx);
                const float_type y = elems[1].second->eval_float(ctx);
                ML_INT_ONLY(int_vc_float_cmp(x, y);)
                return typed_cmp<float_type>(elems[1].first, x, y);
            } else {
                const int_type x = elems[0].second->eval_int(ctx);
                const int_type y = elems[1].second->eval_int(ctx);
                return typed_cmp<int_type>(elems[1].first, x, y);
            }

        case Cat::logical: {
            int_type acc = elems[0].second->eval_int(ctx);
            for (size_t i = 1; i < elems.size(); i++) {
                /* SHORT-CIRCUIT (#138): stop at the operand that DETERMINES
                 * the result - false for `&&`, true for `||` - so the rest is
                 * never evaluated. The M8 form must agree with the boxed one
                 * operand for operand, since which one a node gets is an
                 * inference detail the program cannot see. */
                if (elems[i].first == Op::land) {
                    if (!acc)
                        return 0;
                } else if (acc) {
                    return 1;
                }
                const int_type r = elems[i].second->eval_int(ctx);
                acc = (elems[i].first == Op::land) ? (acc && r) : (acc || r);
            }
            return acc;
        }
    }

    return 0;
}

float_type TypedScalarExpr::eval_float_body(EvalContext *ctx) const
{
    switch (cat) {

        case Cat::neg: {
            const float_type v = elems[0].second->eval_float(ctx);
            ML_INT_ONLY(int_vc_float_neg(v);)
            return -v;
        }

        case Cat::arith: {
            float_type acc = elems[0].second->eval_float(ctx);
            for (size_t i = 1; i < elems.size(); i++) {
                const float_type r = elems[i].second->eval_float(ctx);
                switch (elems[i].first) {
                    case Op::plus:
                        ML_INT_ONLY(int_vc_float_op(acc, r, acc + r);)
                        acc += r; break;
                    case Op::minus:
                        ML_INT_ONLY(int_vc_float_op(acc, r, acc - r);)
                        acc -= r; break;
                    case Op::times:
                        ML_INT_ONLY(int_vc_float_op(acc, r, acc * r);)
                        acc *= r; break;
                    case Op::div:
                        ML_INT_ONLY(int_vc_float_div0(false, r);
                                    if (r != 0.0)
                                        int_vc_float_op(acc, r, acc / r);)
                        if (r == 0.0)
                            throw DivisionByZeroEx(elems[i].second->start,
                                                   elems[i].second->end);
                        acc /= r; break;
                    case Op::mod:
                        ML_INT_ONLY(int_vc_float_div0(true, r);
                                    if (r != 0.0)
                                        int_vc_float_op(acc, r,
                                                        fmod(acc, r));)
                        if (r == 0.0)
                            throw DivisionByZeroEx(elems[i].second->start,
                                                   elems[i].second->end);
                        acc = fmod(acc, r); break;
                    default: throw InternalErrorEx();
                }
            }
            return acc;
        }

        default:   /* cmp / logical / lnot yield int */
            return static_cast<float_type>(eval_int_body(ctx));
    }
}

/* Task #75: the typed evals bypass Construct::eval, so a node spliced from
 * an inlined body must flush its own "inlined-at" chain when an exception
 * unwinds through it - the innermost typed node on the unwind path wins
 * (exactly Construct::eval's protocol, keyed off inline_origin_emitted).
 * A chain-less node (the overwhelmingly common case) takes the direct
 * call - no try scaffolding on its path. */
int_type TypedScalarExpr::eval_int(EvalContext *ctx) const
{
    if (!inline_ctx)
        return eval_int_body(ctx);
    try {
        return eval_int_body(ctx);
    } catch (Exception &ex) {
        if (!ex.inline_origin_emitted) {
            flush_inline_frames(inline_ctx, ex);
            ex.inline_origin_emitted = true;
        }
        throw;
    }
}

float_type TypedScalarExpr::eval_float(EvalContext *ctx) const
{
    if (!inline_ctx)
        return eval_float_body(ctx);
    try {
        return eval_float_body(ctx);
    } catch (Exception &ex) {
        if (!ex.inline_origin_emitted) {
            flush_inline_frames(inline_ctx, ex);
            ex.inline_origin_emitted = true;
        }
        throw;
    }
}

EvalValue TypedScalarExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /* Comparisons / logical / `!` yield a bool (computed unboxed as 0/1 by
     * eval_int, then boxed as a bool). Arithmetic / negation keep int/float. */
    if (cat == Cat::cmp || cat == Cat::logical || cat == Cat::lnot)
        return EvalValue(eval_int(ctx) != 0);
    if ((cat == Cat::arith || cat == Cat::neg) && kind == TypeHint::f)
        return EvalValue(eval_float(ctx));
    return EvalValue(eval_int(ctx));
}

/* Apply a compound-assignment op (`+=`, `-=`, ...) to `acc` in place.
 * A loc-less throw out of the operation is marked Exception::op_caret:
 * it carets the WHOLE compound expression, not the lvalue (RULE 2 - the
 * tree-walker's Expr14 is the first node to stamp it; a VM store op
 * selects its op_locs entry on the flag, bytecode.h). */
static inline void
apply_compound_op_raw(EvalValue &acc, const EvalValue &rhs, Op op)
{
    switch (op) {
        case Op::addeq:  num_bin_op(acc, rhs, &Type::add);  break;
        case Op::subeq:  num_bin_op(acc, rhs, &Type::sub);  break;
        case Op::muleq:  num_bin_op(acc, rhs, &Type::mul);  break;
        case Op::diveq:  num_bin_op(acc, rhs, &Type::div);  break;
        case Op::modeq:  num_bin_op(acc, rhs, &Type::mod);  break;
        /* int-only (like the binary forms): a non-int operand reaches the
         * base Type's virtual, which throws TypeErrorEx - num_bin_op's
         * float promotion routes a float there, exactly as `x << 2.5`. */
        case Op::shleq:  num_bin_op(acc, rhs, &Type::shl);  break;
        case Op::shreq:  num_bin_op(acc, rhs, &Type::shr);  break;
        case Op::ushreq: num_bin_op(acc, rhs, &Type::ushr); break;
        case Op::bandeq: num_bin_op(acc, rhs, &Type::band); break;
        case Op::boreq:  num_bin_op(acc, rhs, &Type::bor);  break;
        case Op::bxoreq: num_bin_op(acc, rhs, &Type::bxor); break;
        default:         throw InternalErrorEx();
    }
}

static inline void
apply_compound_op(EvalValue &acc, const EvalValue &rhs, Op op)
{
    try {
        apply_compound_op_raw(acc, rhs, op);
    } catch (Exception &e) {
        if (!e.loc_start)
            e.op_caret = 1;
        throw;
    }
}

static EvalValue
doAssign(const EvalValue &lval, const EvalValue &rval, Op op)
{
    EvalValue newVal;

    if (lval.get<LValue *>()->is<Builtin>())
        throw CannotRebindBuiltinEx();

    if (op == Op::assign) {

        newVal = RValue(rval);

    } else {

        newVal = lval.get<LValue *>()->get();
        apply_compound_op(newVal, RValue(rval), op);
    }

    lval.get<LValue *>()->put(newVal);
    return newVal;
}

/* Return `lvalue` as an Identifier resolved to a slot, or nullptr otherwise.
 * The cheap ct tag (is_id) avoids a dynamic_cast on the hot assignment path. */
static inline const Identifier *
as_resolved_local(const Construct *lvalue)
{
    if (!lvalue->is_id())
        return nullptr;

    const Identifier *id = static_cast<const Identifier *>(lvalue);
    return id->sym.kind == SymKind::local ? id : nullptr;
}

/* Same, for a name resolved to a GLOBAL-table slot (a top-level function or an
 * escaped top-level variable). */
static inline const Identifier *
as_resolved_global(const Construct *lvalue)
{
    if (!lvalue->is_id())
        return nullptr;

    const Identifier *id = static_cast<const Identifier *>(lvalue);
    return id->sym.kind == SymKind::global ? id : nullptr;
}

/* Same, for a name resolved to a closure CAPTURE slot. */
static inline const Identifier *
as_resolved_capture(const Construct *lvalue)
{
    if (!lvalue->is_id())
        return nullptr;

    const Identifier *id = static_cast<const Identifier *>(lvalue);
    return id->sym.kind == SymKind::capture ? id : nullptr;
}

/*
 * Read-modify-write a resolved slot's LValue in place and return the new value.
 * Shared by the local / global / capture assignment fast paths in
 * handle_single_expr14. For a plain assign it overwrites; for a compound op it
 * fast-paths int += / -= / *= directly on the slot's int (no num_bin_op PMF
 * dispatch, no copy in/out) and falls back to apply_compound_op otherwise
 * (div/mod keep the zero check; a non-int operand promotes correctly). The
 * caller has already verified the slot is non-const.
 */
static inline EvalValue
slot_rmw(LValue &lv, Op op, const EvalValue &rval)
{
    /*
     * An ARRAY ELEMENT's write goes through put(), which detaches the array
     * first (LValue::write_target) - and may leave `lv` stale, or freed,
     * since a slice's element then lives in new storage. So the result is
     * never read back from `lv` after a put, and the in-place int fast
     * path is for a slot that is not an element: on a slice's element it
     * skipped the detach, so `s[0] += 1` changed the array `s` was sliced
     * from (the VM and the JIT, until 2026-10-08; the tree-walker's
     * doAssign put the value).
     */
    if (op == Op::assign) {

        EvalValue v = RValue(rval);
        lv.put(v);
        return v;
    }

    const EvalValue r = RValue(rval);

    if (!lv.container && lv.is<int_type>() && r.is<int_type>() &&
        (op == Op::addeq || op == Op::subeq || op == Op::muleq)) {

        int_type &v = lv.getval<int_type>();
        const int_type n = r.get<int_type>();

        if (op == Op::addeq)      v += n;
        else if (op == Op::subeq) v -= n;
        else                      v *= n;

        return lv.get();
    }

    EvalValue nv = lv.get();
    apply_compound_op(nv, r, op);
    lv.put(nv);
    return nv;
}

/*
 * True if evaluating `c` can have no side effect (an id, a literal, a
 * subscript / member / arithmetic chain over those) - so evaluating it twice
 * is unobservable. Its users read a base a second time for speed
 * (member_pod_array_scalar), or tell the codegen a base may be read twice.
 * A STORE does not ask it: an access chain's base is evaluated exactly once
 * whatever it contains (access_store, below).
 */
static bool
no_side_effects(const Construct *c)
{
    if (c->is_id())
        return true;
    if (dynamic_cast<const Literal *>(c))   /* scalar literals are pure */
        return true;
    if (c->is_subscript()) {
        auto *s = static_cast<const Subscript *>(c);
        return no_side_effects(s->what.get()) &&
               no_side_effects(s->index.get());
    }
    if (auto *m = dynamic_cast<const MemberExpr *>(c))
        return no_side_effects(m->what.get());
    if (auto *mo = dynamic_cast<const MultiOpConstruct *>(c)) {
        for (const auto &pr : mo->elems)
            if (!no_side_effects(pr.second.get()))
                return false;
        return true;          /* pure arithmetic/comparison index, e.g. i+1 */
    }
    if (auto *t = dynamic_cast<const TypedScalarExpr *>(c)) {
        for (const auto &pr : t->elems)
            if (!no_side_effects(pr.second.get()))
                return false;
        return true;          /* specialized form of the above */
    }
    return false;
}

bool construct_no_side_effects(const Construct *c)
{
    return no_side_effects(c);
}

/*
 * Flat-array element store CORE: `blv` holds a flat (non-general, non-const,
 * non-read-only) SharedArrayObj `arr`; store `rval` (op) at index `idx_v`. The
 * caller has already proven blv is a flat writable array. Shared by the AST
 * path (subscript_store, base `a` from the AST) and the VM's NESTED
 * store (vm_nested_subscript_store, base `a[i]` an inner element LValue).
 * Returns true (and sets `out`) on a store; false ONLY for a compound op on a
 * flat struct array (structs have no `+=`), so the caller defers to the general
 * path. `sub_*` is the subscript's caret (OOB / type errors), `idx_*` the
 * index's (the "Expected integer" error). A compound op stores the element
 * it read in `*old_out` when given - the value a postfix `++` / `--` yields.
 */
static bool
flat_store_core(LValue *blv, SharedArrayObj &arr, const EvalValue &idx_v,
                const EvalValue &rval, Op op, EvalValue &out,
                Loc sub_start, Loc sub_end, Loc idx_start, Loc idx_end,
                EvalValue *old_out = nullptr)
{
    /*
     * Flat POD-struct array: `a[i] = <matching POD struct>` stores the value's
     * bytes (a compound op defers; structs have no `+=`). A non-matching value
     * is the dyn-launder case (errors like the scalar kinds).
     */
    if (arr.skind() == SharedArrayObj::Storage::structs) {

        if (op != Op::assign)
            return false;

        const EvalValue r = RValue(rval);
        const auto &sv0 = arr.flat_structs();

        if (!idx_v.is<int_type>())
            throw TypeErrorEx("Expected integer as subscript",
                              idx_start, idx_end);
        int_type idx = idx_v.get<int_type>();
        ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
        if (idx < 0)
            idx += arr.size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
            throw OutOfBoundsEx(sub_start, sub_end);

        if (!r.is<intrusive_ptr<StructObject>>() ||
            !r.get<intrusive_ptr<StructObject>>()->is_pod() ||
            r.get<intrusive_ptr<StructObject>>()->def != sv0.def)
            throw TypeErrorEx(
                "Cannot store a value of a different type in a flat (typed) "
                "array; declare the array dyn for a polymorphic array",
                sub_start, sub_end);

        if (arr.is_slice())
            arr.clone_internal_vec();
        else if (arr.use_count() > 1)
            arr.clone_aliased_slices(arr.offset() + idx);

        auto &sv = arr.flat_structs();
        const StructObject &o = *r.get<intrusive_ptr<StructObject>>().get();
        std::memcpy(sv.buf.data() + (arr.offset() + idx) * sv.stride,
                    o.bytes.data(), sv.stride);
        out = r;
        return true;
    }

    /*
     * Flat STRING array (top-10 #7, the value-driven model): a plain store
     * of a STRING keeps it flat; ANYTHING else (a non-string value, a
     * compound op) PROMOTES to general and defers to the general path -
     * never the flat scalars' dyn-launder error.
     */
    if (arr.skind() == SharedArrayObj::Storage::strs) {

        const EvalValue r0 = RValue(rval);
        if (op != Op::assign || !r0.is<SharedStr>()) {
            arr.promote_strs_to_general();
            return false;                 /* general path handles it */
        }

        if (!idx_v.is<int_type>())
            throw TypeErrorEx("Expected integer as subscript",
                              idx_start, idx_end);
        int_type idx = idx_v.get<int_type>();
        ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
        if (idx < 0)
            idx += arr.size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
            throw OutOfBoundsEx(sub_start, sub_end);

        if (arr.is_slice())
            arr.clone_internal_vec();
        else if (arr.use_count() > 1)
            arr.clone_aliased_slices(arr.offset() + idx);

        arr.flat_strs()[arr.offset() + idx] = SharedStr(r0.get<SharedStr>());
        out = r0;
        return true;
    }

    const bool kind_int = arr.skind() == SharedArrayObj::Storage::ints;
    const bool kind_bool = arr.skind() == SharedArrayObj::Storage::bools;

    if (!idx_v.is<int_type>())
        throw TypeErrorEx("Expected integer as subscript",
                          idx_start, idx_end);

    int_type idx = idx_v.get<int_type>();
    ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
    if (idx < 0)
        idx += arr.size();
    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx(sub_start, sub_end);

    const size_type at0 = arr.offset() + idx;
    const EvalValue r = RValue(rval);

    /* Compute the value to store (compound ops read the current element). */
    EvalValue newval;
    if (op == Op::assign) {
        newval = r;
    } else {
        newval = arr_elem_boxed(arr, idx);   /* read current element, any kind */
        if (old_out)
            *old_out = newval;
        apply_compound_op(newval, r, op);
    }

    /*
     * #96: a BOOL value WIDENS into numeric storage (0/1 into ints,
     * 0.0/1.0 into floats) - the promotion chain bool <= int <= float,
     * the same widen coerce_to_decl_type and coerce_struct_field
     * already do (arrays were the outlier; the static checker accepts
     * the assignment, so refusing at runtime contradicted it - and the
     * VM's typed StoreElemInt path, which pre-widens a bool literal to
     * a 0/1 operand, DIVERGED from this refusal). The reverse direction
     * (an int into a BOOL array) stays an error: that is a narrowing,
     * which the language never does implicitly.
     */
    const bool fits = kind_int
        ? (newval.is<int_type>() || newval.is<bool>())
        : kind_bool
            ? newval.is<bool>()
            : (newval.is<float_type>() || newval.is<int_type>()
               || newval.is<bool>());

    if (!fits) {
        /*
         * The new element type doesn't fit the flat array's scalar kind. mylang
         * does not promote (flat->general) in place - the representation is
         * fixed at creation from the proven static type. This is reachable only
         * by laundering a typed array through `dyn` (declare it dyn from the
         * start for a polymorphic array). Same message as arr.cpp.h's
         * flat_array_violation_msg (a separate translation unit).
         */
        throw TypeErrorEx(
            "Cannot store a value of a different type in a flat (typed) array; "
            "declare the array dyn for a polymorphic array",
            sub_start, sub_end);
    }

    /*
     * COW, matching the general element write (LValue::write_target):
     * a slice clones itself; a non-slice that is aliased clones any live slices
     * so they don't observe the write, but writes in place otherwise (plain
     * handle aliases share the mutation - MyLang assignment aliases).
     */
    if (arr.is_slice())
        arr.clone_internal_vec();            /* keep-flat; now standalone */
    else if (arr.use_count() > 1)
        arr.clone_aliased_slices(at0);

    const size_type at = arr.offset() + idx;
    if (kind_int) {
        arr.flat_ints()[at] = newval.is<bool>()
            ? (newval.get<bool>() ? 1 : 0)
            : newval.get<int_type>();
    } else if (kind_bool) {
        arr.flat_bools()[at] = newval.get<bool>() ? 1 : 0;
    } else {
        arr.flat_floats()[at] = newval.is<int_type>()
            ? static_cast<float_type>(newval.get<int_type>())
            : newval.is<bool>()
                ? (newval.get<bool>() ? 1.0 : 0.0)
                : newval.get<float_type>();
    }

    arr.invalidate_hash();   /* an element write changes the array's hash */
    out = newval;
    return true;
}

/* True if `blv` holds a flat (non-general), writable (non-const/read-only)
 * SharedArrayObj; sets `arr` to it. The shared "is this a flat store" gate. */
static bool
flat_writable_array(LValue *blv, SharedArrayObj *&arr)
{
    if (!blv->is<SharedArrayObj>())
        return false;
    SharedArrayObj &a = blv->getval<SharedArrayObj>();
    if (a.skind() == SharedArrayObj::Storage::general)
        return false;            /* not flat: let the general path handle it */
    /* A const/read-only array: defer so the general path raises the right error
     * (CannotChangeConstEx / NotLValueEx) with the proper loc. */
    if (blv->is_const_var() || a.is_readonly())
        return false;
    arr = &a;
    return true;
}

/* What Construct::eval does to an exception leaving node `n`: stamp its
 * span when the exception carries none, and emit its inlined-at frames once.
 * For code that runs a node's work without going through its eval. */
static void stamp_like_eval(const Construct *n, Exception &e)
{
    if (!e.loc_start) {
        e.loc_start = n->start;
        e.loc_end = n->end;
    }
    if (n->inline_ctx && !e.inline_origin_emitted) {
        flush_inline_frames(n->inline_ctx, e);
        e.inline_origin_emitted = true;
    }
}

/*
 * A store through a value that is not a variable - a call's result, a
 * literal, a ternary, a slice (`f()[0] = v`, `(c ? a : b).x += 1`) - goes
 * into that value, as if it were first bound to a fresh variable:
 * `f()[0] = v` is `{ var t = f(); t[0] = v; }`. `hold` is that variable,
 * the caller's, alive for the whole store. A container is a reference, so
 * the write lands in the object f() returned; a slice detaches the hold
 * (copy on write) exactly as it would a variable; a string or a scalar
 * meets the error a variable of it meets. Until 2026-10-06 the tree-walker
 * stored through such a value into a dict only, raised NotLValueEx for an
 * array or a struct, and the VM refused to compile it - while an inlined
 * `f` turned the same store into one through a variable (RULE 2). The VM's
 * twin is the codegen's hold_store_base.
 */
static EvalValue hold_store_base(const EvalValue &base, LValue &hold)
{
    if (base.is<LValue *>() || base.is<UndefinedId>())
        return base;
    hold = LValue(base, false);
    return EvalValue(&hold);
}

/* The general tail of a store: the formed reference is an LValue to write,
 * or the target is not a location. `*old_out`, when given, receives the
 * value the store replaced (as do the stores below). */
static EvalValue
assign_through(EvalContext *ctx, const Construct *lvalue,
               const EvalValue &lval, const EvalValue &rval, Op op,
               EvalValue *old_out = nullptr)
{
    if (!lval.is<LValue *>())
        throw NotLValueEx(lvalue->start, lvalue->end);

    /* The parser never evaluates an assignment during const evaluation. */
    if (ctx->const_ctx)
        throw InternalErrorEx();

    LValue *lv = lval.get<LValue *>();
    if (lv->is_const_var()) {
        if (lv->is<Builtin>())
            throw CannotRebindBuiltinEx(lvalue->start, lvalue->end);
        throw CannotRebindConstEx(lvalue->start, lvalue->end);
    }
    if (old_out)
        *old_out = lv->get();
    return doAssign(lval, rval, op);
}

/*
 * A store THROUGH an access chain - `x[k] = v` / `x[k] OP= v`, `x.f = v` /
 * `x.f OP= v`, and `++` / `--` on a typed one (IncDecExpr routes `+= 1`
 * here). The base and the key are evaluated EXACTLY ONCE, left to right,
 * and the store goes to what they named:
 *   - a flat (unboxed) array's element is written straight into the vector
 *     (flat_store_core) - it has no LValue, so no general path can write
 *     it; a value that does not fit the flat kind promotes or throws there;
 *   - a POD struct's field is written into its bytes (it has no LValue
 *     either), coerced to the field's type;
 *   - anything else forms the element / member reference the way
 *     Subscript::do_eval / MemberExpr::do_eval would (a plain `=` may
 *     auto-vivify a dict key) and assigns through it.
 * Until 2026-10-05 the first two were gated on a side-effect-free base -
 * the general path then evaluated the whole lvalue a second time - so
 * `a[f()][0] += 5` raised NotLValueEx in the tree-walker while the VM, which
 * computes each key into a temp once, stored it (RULE 2).
 */
static EvalValue
subscript_store(EvalContext *ctx, const Subscript *sub, Op op,
                const EvalValue &rval, EvalValue *old_out = nullptr)
{
    EvalValue base, key;
    LValue hold;
    try {
        base = store_base_value(ctx, sub->what.get(), hold);
        if (base.is<UndefinedId>())
            throw UndefinedVariableEx(base.get<UndefinedId>().id,
                                      sub->what->start, sub->what->end);
        key = literal_widen(RValue(sub->index->eval(ctx)), sub->key_coerce);
    } catch (Exception &e) {
        stamp_like_eval(sub, e);
        throw;
    }

    SharedArrayObj *arr;
    if (!ctx->const_ctx && base.is<LValue *>()
            && flat_writable_array(base.get<LValue *>(), arr)) {
        EvalValue out;
        if (flat_store_core(base.get<LValue *>(), *arr, key, rval, op, out,
                            sub->start, sub->end,
                            sub->index->start, sub->index->end, old_out))
            return out;
    }

    EvalValue elv;
    try {
        Type *t = base.is<LValue *>()
            ? base.get<LValue *>()->get().get_type()
            : base.get_type();
        elv = t->subscript(base, key, /*for_write=*/op == Op::assign);
    } catch (Exception &e) {
        stamp_like_eval(sub, e);
        throw;
    }
    return assign_through(ctx, sub, elv, rval, op, old_out);
}

/*
 * The BASE of a store, evaluated ONCE, for writing: `x` in `x.f = v`,
 * `x[k] = v`, `x.f++`, `append(x.f, v)`. A member step into a struct OWNS
 * the struct first (struct_own): the store below writes into it, and a
 * struct is a value - its other holders must not see the write. An access
 * chain with no member step is evaluated as a read evaluates it. Where the
 * struct a step enters lives in bytes no LValue covers - a flat struct
 * array's element, an inline POD field - the walk continues INSIDE them
 * (`place`, eval.h's POD places); a step the bytes cannot take (not an
 * inline POD struct) reads the struct as a value from there on. Else
 * `base` is the slot or the value the store's own access starts from.
 */
struct MemberTarget {
    EvalValue base;
    PodPlace place;
};

static bool chain_has_member(const Construct *e)
{
    for (;;) {
        if (ctag(e) == ConstructType::member)
            return true;
        if (ctag(e) != ConstructType::subscript)
            return false;
        e = static_cast<const Subscript *>(e)->what.get();
    }
}

/* One member step of a store walk (MemberExpr::do_eval's work, for_write
 * false, the struct owned first). The caller stamps a throw. */
static void member_store_step(MemberTarget &t, const MemberExpr *s)
{
    if (t.place.bytes) {
        if (pod_place_step(t.place, s->memUid))
            return;
        t.base = pod_place_value(t.place);
        t.place = PodPlace();
    }
    if (t.base.is<LValue *>()) {
        LValue *h = t.base.get<LValue *>();
        struct_own_if(h);
        t.base = EvalValue(h);
        if (pod_place_rooted_field(h, s->memUid, t.place))
            return;
    }
    /* a field of a VALUE is not a location: only a slot's struct gives
     * its field's LValue (a store through a value holds it first) */
    const bool rooted = t.base.is<LValue *>();
    t.base = s->access(RValue(t.base), /*for_write=*/false, rooted);
}

static void store_walk(EvalContext *ctx, const Construct *e, LValue &hold,
                       MemberTarget &t)
{
    if (ctag(e) == ConstructType::member) {
        const MemberExpr *s = static_cast<const MemberExpr *>(e);
        store_walk(ctx, s->what.get(), hold, t);
        try {
            member_store_step(t, s);
        } catch (Exception &ex) {   /* INT-COV-EXEMPT: its type test fails */
            stamp_like_eval(s, ex);
            throw;
        }
        return;
    }
    if (ctag(e) == ConstructType::subscript) {
        /* Subscript::do_eval's work, by hand: a writable flat struct
         * array's element is entered in place, not read as a copy, and a
         * member step in its base owns what it enters */
        const Subscript *sub = static_cast<const Subscript *>(e);
        if (chain_has_member(sub->what.get())) {
            store_walk(ctx, sub->what.get(), hold, t);
            if (t.place.bytes) {
                t.base = pod_place_value(t.place);
                t.place = PodPlace();
            }
        } else {
            try {
                t.base = hold_store_base(sub->what->eval(ctx), hold);
            } catch (Exception &ex) {
                stamp_like_eval(sub, ex);
                throw;
            }
        }
        try {
            if (t.base.is<UndefinedId>())
                throw UndefinedVariableEx(t.base.get<UndefinedId>().id,
                                          sub->what->start, sub->what->end);
            const EvalValue key = literal_widen(
                RValue(sub->index->eval(ctx)), sub->key_coerce);
            if (!t.base.is<LValue *>() ||
                !pod_place_elem(t.base.get<LValue *>(), key, t.place)) {
                Type *ty = t.base.is<LValue *>()
                    ? t.base.get<LValue *>()->get().get_type()
                    : t.base.get_type();
                t.base = ty->subscript(t.base, key, /*for_write=*/false);
            }
        } catch (Exception &ex) {
            stamp_like_eval(sub, ex);
            throw;
        }
        return;
    }
    t.base = hold_store_base(e->eval(ctx), hold);
}

/* The base of a store `<e>.f` / `<e>[k]`: store_walk for a chain that
 * steps into a member, else `e` evaluated as a read, a value held. */
static MemberTarget store_base(EvalContext *ctx, const Construct *e,
                               LValue &hold)
{
    MemberTarget t;
    if (chain_has_member(e))
        store_walk(ctx, e, hold, t);
    else
        t.base = hold_store_base(e->eval(ctx), hold);
    return t;
}

/* The base of a member store `... .f = v`: store_walk over `mem->what`. */
static MemberTarget member_target(EvalContext *ctx, const MemberExpr *mem,
                                  LValue &hold)
{
    MemberTarget t;
    store_walk(ctx, mem->what.get(), hold, t);
    return t;
}

/* The value-or-slot form of a store base (store_base, its POD place read
 * as a value): the base of `<e>[k] = v`, the target of a mutating builtin.
 * Exported (eval.h) for the tree-walker's builtin adapters. */
EvalValue store_base_value(EvalContext *ctx, const Construct *e, LValue &hold)
{
    MemberTarget t = store_base(ctx, e, hold);
    if (t.place.bytes)
        return pod_place_value(t.place);
    return t.base;
}

static EvalValue
member_store(EvalContext *ctx, const MemberExpr *mem, Op op,
             const EvalValue &rval, EvalValue *old_out = nullptr)
{
    MemberTarget t;
    LValue hold;
    try {
        t = member_target(ctx, mem, hold);
    } catch (Exception &e) {
        stamp_like_eval(mem, e);
        throw;
    }

    /* the store itself stays outside that catch, as the POD path below:
     * an operation's error (`p.x /= 0`) keeps the whole expression's caret */
    if (t.place.bytes) {
        const int slot = t.place.def->slot_of(mem->memUid);
        if (slot >= 0)
            return pod_place_store(t.place, slot, op, rval,
                                   mem->start, mem->end, old_out);
        t.base = pod_place_value(t.place);
    }
    EvalValue base = t.base;

    /* A struct in a slot: OWNED first (struct_own - its other holders must
     * not see the write), then a POD field is stored into its bytes. (A
     * `const` parameter's struct is refused at compile time when its type
     * is known; at run time the parameter is the call's copy, as in every
     * engine.) */
    const bool rooted = base.is<LValue *>();
    if (rooted) {
        LValue *blv = base.get<LValue *>();
        if (blv->is<intrusive_ptr<StructObject>>()) {
            StructObject &obj = struct_own(blv);
            base = EvalValue(blv);
            const int slot = obj.is_pod()
                ? obj.def->slot_of(mem->memUid) : -1;
            if (slot >= 0) {
                const EvalValue r = RValue(rval);
                EvalValue newval;
                if (op == Op::assign) {
                    newval = r;
                } else {
                    newval = obj.pod_get(slot);
                    if (old_out)
                        *old_out = newval;
                    apply_compound_op(newval, r, op);
                }
                /* coerce + runtime-validate to the field's scalar type
                 * (throws on a mismatch, e.g. a dyn-laundered wrong
                 * type) */
                newval = coerce_struct_field(obj.def->fields[slot],
                                             std::move(newval),
                                             mem->start, mem->end);
                obj.pod_set(slot, newval);
                return newval;
            }
        }
    }

    EvalValue lval;
    try {
        lval = mem->access(RValue(base), /*for_write=*/op == Op::assign,
                           rooted);
    } catch (Exception &e) {
        stamp_like_eval(mem, e);
        throw;
    }
    return assign_through(ctx, mem, lval, rval, op, old_out);
}

/*
 * Coerce a value to a declared scalar type's storage on a typed assignment /
 * declaration: a `float` variable stores an int/bool as a float, an `int`
 * variable stores a bool as an int (the numeric-widening coercions of the
 * `bool <= int <= float` chain). So `float f = 3;` and a later `f = 5;` actually
 * hold a float, not an int. Other declared types (str/bool/array/dict) and
 * non-widening values pass through unchanged; the inferencer has already
 * rejected anything not assignable to the declared type.
 */
static EvalValue coerce_to_decl_type(const EvalValue &v, DeclType dt,
                                     int site_arg)
{
    /*
     * `site_arg` (RULE 2, 2026-09-20): the CALL-SITE argument index when
     * this coercion binds a parameter for a call site, else -1 - the throw
     * carries it (Exception::bind_arg) so the call's setup catch can caret
     * that argument alone. An assignment's coercion passes nothing.
     *
     * Coerce a value to a typed variable's/param's declared type. WIDENING is
     * implicit (int/bool -> float, bool -> int); NARROWING is NOT (a `float`
     * into an `int` THROWS, it never truncates) - use an explicit int(x) to
     * narrow. A statically-typed rvalue can only be a widening one (the check
     * pass rejects a narrowing assignment/arg at compile time), so a throw here
     * fires ONLY for a `dyn` value whose RUNTIME type doesn't fit: `int s = 0;
     * s = s + x` over a `dyn` x holding a float -> a runtime error (`x` isn't
     * int). `none` passes through - nullability (opt) is checked separately.
     */
    if (dt == DeclType::f) {
        if (v.is<float_type>() || v.is<NoneVal>())
            return v;
        if (v.is<int_type>())
            return EvalValue(static_cast<float_type>(v.get<int_type>()));
        if (v.is<bool>())
            return EvalValue(static_cast<float_type>(v.get<bool>() ? 1 : 0));
        TypeErrorEx ex(
            "cannot store a non-numeric value in a 'float' variable "
            "(use float(...) to convert)");
        ex.bind_arg = site_arg;
        throw ex;
    }
    if (dt == DeclType::i) {
        if (v.is<int_type>() || v.is<NoneVal>())
            return v;
        if (v.is<bool>())
            return EvalValue(static_cast<int_type>(v.get<bool>() ? 1 : 0));
        TypeErrorEx ex(
            "cannot store a non-int value in an 'int' variable "
            "(a float doesn't narrow implicitly - use int(...) to convert)");
        ex.bind_arg = site_arg;
        throw ex;
    }
    return v;
}

/* VM (CoerceNumV): the typed-store numeric coerce - the SAME
 * coerce_to_decl_type the tree-walker's op==assign path runs, so the widen /
 * pass-none / narrowing-throw behavior is byte-identical by construction. */
EvalValue vm_coerce_decl_num(const EvalValue &v, bool is_float,
                             int site_arg)
{
    return coerce_to_decl_type(v, is_float ? DeclType::f : DeclType::i,
                               site_arg);
}

/*
 * THE ONE place that turns a function's parameter list into its bind plan.
 * Sets `fast_bind` (no parameter needs coercion, so a bind is a plain copy)
 * and, when it does not hold, `bind_req` - the Type singleton each parameter
 * requires for the copy to still be exact (see FuncDescriptor::bind_req).
 *
 * It exists as ONE function because the two are derived from the SAME read of
 * `decl_type` and there are four callers (vm_bind_chunk, vm_precompile_all's
 * pass A, the -vd dump's splice pass, and the .myv reader). The flag used to
 * be recomputed inline at three of them; a fourth consumer that had to agree
 * with it is exactly the shape CLAUDE.md's audit-table trap warns about.
 */
void compute_bind_flags(const FuncDescriptor *d)
{
    bool fast = true;

    for (const auto &p : d->params)
        if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
            fast = false;

    d->fast_bind = fast;
    d->bind_req.clear();

    if (fast)
        return;                     /* nothing reads bind_req then */

    d->bind_req.reserve(d->params.size());

    for (const auto &p : d->params) {
        const void *req = nullptr;
        if (p.decl_type == DeclType::i)
            req = EvalValue(static_cast<int_type>(0)).get_type();
        else if (p.decl_type == DeclType::f)
            req = EvalValue(static_cast<float_type>(0)).get_type();
        d->bind_req.push_back(req);
    }
}

/*
 * VM P2/P4: native SUBSCRIPT element store `c[k] = v` / `c[k] OP= v` for a dict
 * (P2) OR a general array (P4). `base_lv` = the container's LValue (a slot),
 * `key`/`value` the pre-evaluated operands (an int index for an array, any key
 * for a dict), `op` the Expr14 op (assign / addeq / ...). Reuses the shared
 * Type::subscript(for_write) lvalue path - which is type-dispatched, so it does
 * the dict's auto-vivify-none-on-plain-miss + container-key freeze OR the
 * array's bounds check, and the COW, exactly as the tree-walker's
 * Subscript::do_eval - plus slot_rmw (the SAME assign/compound). So the result
 * and the throws (missing dict key on a compound, out-of-bounds array index, a
 * read-only container) match the tree-walker byte-for-byte.
 */
EvalValue vm_subscript_store(LValue *base_lv, const EvalValue &key,
                             const EvalValue &value, Op op,
                             Loc lstart, Loc lend)
{
    /* A FLAT array element (int/float/bool/struct) has no boxed LValue - it's a
     * scalar/bytes in the flat vector - so the general subscript(for_write) path
     * below would wrongly raise NotLValueEx. Store straight into the flat buffer
     * via the shared flat_store_core (COW + type check + the dyn-launder error),
     * exactly as the tree-walker's subscript_store. This is what makes
     * StoreElemValue a UNIVERSAL store (any base: flat / general / dict), so the
     * codegen can emit it for a dyn/unproven base. flat_store_core returns false
     * only for a compound op on a flat STRUCT array (defer to the general path,
     * which raises the same error). */
    if (base_lv->is<SharedArrayObj>()) {
        SharedArrayObj *arr;
        if (flat_writable_array(base_lv, arr)) {
            EvalValue out;
            if (flat_store_core(base_lv, *arr, key, value, op, out,
                                lstart, lend, lstart, lend))
                return out;
        }
    }

    const bool for_write = (op == Op::assign);
    EvalValue elv;
    try {
        elv = base_lv->get().get_type()->subscript(
            EvalValue(base_lv), key, for_write);
    } catch (Exception &e) {
        /* reaching the element is the LVALUE's error (an OOB, a missing
         * key): its caret, as Subscript::do_eval stamps it - when the
         * caller has one (the JIT helpers pass none; their conveyance
         * stamps the op's `locs` entry, which is the same span) */
        if (!e.loc_start) {
            e.loc_start = lstart;
            e.loc_end = lend;
        }
        throw;
    }
    if (!elv.is<LValue *>())
        throw NotLValueEx(lstart, lend);
    return slot_rmw(*elv.get<LValue *>(), op, value);
}

/*
 * `++` / `--` on an ELEMENT or a MEMBER nothing proved the type of (a `dyn`
 * base, or any base under -nti): the checked read-modify-write that every
 * engine's dyn path shares - IncDecExpr::do_eval's, the VM's statement ops
 * (vm_incdec_elem / vm_incdec_member) and vm_incdec_final's tier 3. Each
 * sets `old` and returns the new value.
 *
 * A general array's element, a dict's value and a ROOTED boxed struct's
 * field are LValues, changed in place. A FLAT array's element and a POD
 * field are not: a rooted, writable one is read, checked and stored back
 * through flat_store_core / vm_member_store - what `+= 1` does
 * (subscript_store, member_store). Until 2026-10-05 every
 * engine raised NotLValueEx there - for `d[0]++` on a dyn holding an int
 * array, and for EVERY `a[i]++` on a flat array under -nti - while `+= 1`
 * worked; the four copies of this logic agreed on the throw.
 *
 * Carets: an access error (out of bounds, a non-int index, a missing key or
 * member) gets the access's (`a*`; "Expected dict object" the base's,
 * `b*`); the inc-dec's own NotLValue / const / type error gets `id*`.
 */
static EvalValue
dyn_incdec_finish(LValue *lv, bool is_inc, EvalValue &old,
                  Loc id_start, Loc id_end)
{
    if (lv->is_const_var())
        throw CannotChangeConstEx(id_start, id_end);
    old = lv->get();
    if (!old.is<int_type>() && !old.is<float_type>())
        throw TypeErrorEx("'++'/'--' requires an int or float",
                          id_start, id_end);
    EvalValue nv = old;
    apply_compound_op(nv, EvalValue(static_cast<int_type>(1)),
                      is_inc ? Op::addeq : Op::subeq);
    lv->put(nv);
    return nv;
}

static EvalValue
dyn_incdec_elem(const EvalValue &cur, const EvalValue &key, bool is_inc,
                EvalValue &old, Loc astart, Loc aend,
                Loc id_start, Loc id_end)
{
    SharedArrayObj *arr;

    if (cur.is<LValue *>() && flat_writable_array(cur.get<LValue *>(), arr)) {

        /* The checks a general array's subscript makes, in its order and
         * with its caret - then the element's type, before any write. */
        if (!key.is<int_type>())
            throw TypeErrorEx("Expected integer as subscript", astart, aend);
        int_type idx = key.get<int_type>();
        if (idx < 0)
            idx += arr->size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr->size())
            throw OutOfBoundsEx(astart, aend);

        old = arr_elem_boxed(*arr, static_cast<size_type>(idx));
        if (!old.is<int_type>() && !old.is<float_type>())
            throw TypeErrorEx("'++'/'--' requires an int or float",
                              id_start, id_end);

        /* an int or float element: an ints or floats array, which
         * flat_store_core always stores */
        EvalValue nv;
        if (!flat_store_core(cur.get<LValue *>(), *arr, key,
                             EvalValue(static_cast<int_type>(1)),
                             is_inc ? Op::addeq : Op::subeq, nv,
                             astart, aend, astart, aend))
            throw InternalErrorEx();
        return nv;
    }

    EvalValue elv;
    try {
        Type *ct = cur.is<LValue *>()
            ? cur.get<LValue *>()->get().get_type()
            : cur.get_type();
        elv = ct->subscript(cur, key, /*for_write=*/false);
    } catch (Exception &e) {
        if (!e.loc_start) {
            e.loc_start = astart;
            e.loc_end = aend;
        }
        throw;
    }
    if (!elv.is<LValue *>())
        throw NotLValueEx(id_start, id_end);
    return dyn_incdec_finish(elv.get<LValue *>(), is_inc, old,
                             id_start, id_end);
}

static EvalValue
dyn_incdec_member(const EvalValue &base, const EvalValue &memId,
                  const UniqueId *memUid, bool is_inc, EvalValue &old,
                  Loc astart, Loc aend, Loc bstart, Loc bend,
                  Loc id_start, Loc id_end)
{
    EvalValue cur = base;
    const bool rooted = cur.is<LValue *>();
    const EvalValue &cval = rooted ? cur.get<LValue *>()->get() : cur;

    if (rooted && cval.is<intrusive_ptr<StructObject>>()) {
        const StructObject &obj =
            *cval.get_ref<intrusive_ptr<StructObject>>().get();
        const int slot = obj.def->slot_of(memUid);
        if (obj.is_pod() && slot >= 0) {
            old = obj.pod_get(slot);
            if (!old.is<int_type>() && !old.is<float_type>())
                throw TypeErrorEx("'++'/'--' requires an int or float",
                                  id_start, id_end);
            /* the store owns the struct (struct_own) */
            return vm_member_store(cur.get<LValue *>(), memUid,
                                   is_inc ? Op::addeq : Op::subeq,
                                   EvalValue(static_cast<int_type>(1)),
                                   astart, aend, bstart, bend);
        }
    }

    /* A field of a value's struct is a value, as MemberExpr reads it; a
     * dict's value is an LValue either way. */
    LValue *lv = vm_member_lvalue_ref(cur, memId, memUid,
                                      /*for_write=*/false, astart, aend);
    if (!lv) {
        /* the read's own error first (no such member, not a struct or a
         * dict), as the tree-walker's MemberExpr raises it */
        const EvalValue &v =
            cur.is<LValue *>() ? cur.get<LValue *>()->get() : cur;
        member_read_core(v, memId, memUid, false, astart, aend,
                         bstart, bend);
        throw NotLValueEx(id_start, id_end);
    }
    return dyn_incdec_finish(lv, is_inc, old, id_start, id_end);
}

/*
 * VM IncDecElemCheckedV: `c[k]++` / `c[k]--` on a DYN/unproven base, as a
 * statement (the value is discarded): dyn_incdec_elem. The subscript caret
 * (`sub_*`) marks an access error, the inc-dec caret (`id_*`) its own.
 */
void vm_incdec_elem(LValue *base_lv, const EvalValue &key, bool is_inc,
                    Loc sub_start, Loc sub_end, Loc id_start, Loc id_end)
{
    EvalValue old;
    dyn_incdec_elem(EvalValue(base_lv), key, is_inc, old, sub_start, sub_end,
                    id_start, id_end);
}

/*
 * VM IncDecMemberCheckedV: `d.f++` / `d.f--` on a DYN/unproven base. Forms the
 * member LValue exactly like MemberExpr::do_eval's for_write=false path for a
 * ROOTED base (the base is a slot, so is_lvalue_rooted holds): a mutable boxed
 * STRUCT field, or a DICT value (present / default-vivified; a missing key with
 * no default throws KeyNotFoundEx with the MEMBER loc). A POD field / readonly
 * instance / non-struct-non-dict base has no lvalue -> NotLValueEx (the INC-DEC
 * loc), matching the tree-walker. Then int/float-checked ±1 (discarded). Two
 * carets like vm_incdec_elem: the MEMBER loc for a KeyNotFound, the INC-DEC loc
 * for its own NotLValue/const/TypeError.
 */
/*
 * Form the LValue* of `dval.member` exactly like MemberExpr::do_eval's for a
 * ROOTED base (the base is a slot, so is_lvalue_rooted holds): a mutable boxed
 * STRUCT field, or a DICT value (present / default-vivified; a missing key with
 * no default vivifies `none` when `for_write`, else throws KeyNotFoundEx with
 * the MEMBER loc). Returns nullptr when the member is a VALUE read (a POD field,
 * a readonly instance, a non-struct-non-dict base) - the caller turns that into
 * NotLValueEx. Shared by vm_incdec_member and vm_lvalue_chain_store.
 */
LValue *vm_member_lvalue_ref(EvalValue &cur, const EvalValue &memId,
                             const UniqueId *memUid, bool for_write,
                             Loc mstart, Loc mend)
{
    const EvalValue &dval =
        cur.is<LValue *>() ? cur.get<LValue *>()->get() : cur;
    if (dval.is<intrusive_ptr<StructObject>>()) {
        /* a struct's field is a location only in a writable slot's
         * struct - owned before the field is handed out */
        if (!cur.is<LValue *>())
            return nullptr;
        LValue *h = cur.get<LValue *>();
        const StructObject &obj =
            *dval.get_ref<intrusive_ptr<StructObject>>().get();
        const int slot = obj.def->slot_of(memUid);
        if (slot < 0 || obj.is_pod())
            return nullptr;
        StructObject &own = struct_own(h);
        cur = EvalValue(h);
        return &own.fields[slot];
    }
    if (dval.is<intrusive_ptr<DictObject>>()) {
        const auto &obj = dval.get<intrusive_ptr<DictObject>>();
        if (!obj->is_readonly()) {
            const DictObject::mut_iterator it = obj->find_mut(memId);
            if (it != obj->mut_end())
                return &it->second;
            if (obj->get_has_default())      /* a new key (Cursor) */
                return obj->insert_new(EvalValue(memId),
                                       LValue(obj->get_default(), false));
            if (for_write)
                return obj->insert_new(EvalValue(memId),
                                       LValue(none, false));
            throw KeyNotFoundEx(mstart, mend);
        }
    }
    return nullptr;
}

void vm_incdec_member(LValue *base_lv, const EvalValue &memId,
                      const UniqueId *memUid, bool is_inc,
                      Loc mstart, Loc mend, Loc bstart, Loc bend,
                      Loc id_start, Loc id_end)
{
    /* "Expected dict object" marks the base, as the tree-walker's member
     * read does (the site pool carries its caret since myv v28). */
    EvalValue old;
    dyn_incdec_member(EvalValue(base_lv), memId, memUid, is_inc, old,
                      mstart, mend, bstart, bend, id_start, id_end);
}

/*
 * VM IncDecChainV's FINAL step (see eval.h): IncDecExpr::do_eval's exact tier
 * semantics over the walked-to container ref. `cur` is an LValue* wrapper or a
 * plain VALUE (the chain-walk convention: a value-walked chain reproduces the
 * tree-walker's rvalue-ness, so the final ref of an rvalue base is an rvalue).
 *
 * Tier 2 (a proven int/float lvalue) == handle_single_expr14(`±= 1`)
 * (subscript_store / member_store), each store reporting the value it
 * replaced, which postfix returns (it was derived as new ∓ 1 until
 * 2026-10-05: wrong for a float, (0.1 + 1) - 1 != 0.1, and for -0.0). The
 * walk already evaluated every key once, so the final step takes the same
 * stores:
 *   - final SUBSCRIPT: a rooted flat array's element through flat_store_core,
 *     else the general subscript(for_write=false) lvalue + slot_rmw (a value
 *     base's element is an RVALUE -> NotLValueEx at the LVALUE caret);
 *   - final MEMBER: a rooted POD struct's field through vm_member_store, else
 *     the general member lvalue (vm_member_lvalue_ref; a readonly field or a
 *     value base's is a VALUE read - member_read_core for its non-container
 *     TypeError, then NotLValueEx).
 * Until 2026-10-05 the flat and POD stores were gated on a side-effect-free
 * base AST (`allow_flat` / `allow_pod`, codegen-proven) to mirror the
 * tree-walker's old gate - so `var u = a[f()][0]++` raised NotLValueEx.
 *
 * Tier 3 (dyn / un-hinted) == dyn_incdec_elem / dyn_incdec_member, the
 * tree-walker's own: an access error at the access caret (`b*` is a member
 * step's BASE caret, for "Expected dict object"), NotLValue / const /
 * non-int-float at the INC-DEC caret (`id_*`); returns old (postfix) / new
 * (prefix).
 */
EvalValue vm_incdec_final(EvalValue &cur, bool is_member,
                          const EvalValue &memId, const UniqueId *memUid,
                          const EvalValue &key,
                          bool tier2, bool is_inc, bool is_prefix,
                          Loc lstart, Loc lend, Loc kstart, Loc kend,
                          Loc bstart, Loc bend, Loc id_start, Loc id_end)
{
    const Op cop = is_inc ? Op::addeq : Op::subeq;
    const EvalValue one{static_cast<int_type>(1)};

    if (tier2) {

        EvalValue nv, old;
        bool have_nv = false;

        if (is_member) {

            const EvalValue &cval =
                cur.is<LValue *>() ? cur.get<LValue *>()->get() : cur;

            if (cur.is<LValue *>() &&
                cval.is<intrusive_ptr<StructObject>>() &&
                cval.get<intrusive_ptr<StructObject>>()->is_pod()) {

                /* member_store's case: a rooted POD struct - byte-store the
                 * field (a missing member throws in vm_member_store). */
                const StructObject &obj =
                    *cval.get_ref<intrusive_ptr<StructObject>>().get();
                const int slot = obj.def->slot_of(memUid);
                if (slot >= 0)
                    old = obj.pod_get(slot);
                nv = vm_member_store(cur.get<LValue *>(), memUid, cop, one,
                                     lstart, lend, lstart, lend);
                have_nv = true;

            } else {

                /* The general path: the member lvalue (boxed field / dict
                 * value); a VALUE read (readonly, a value base, or a
                 * non-container base's TypeError) fails NotLValue at the
                 * LVALUE caret, exactly as the tree-walker's member_store. */
                LValue *lv = vm_member_lvalue_ref(cur, memId, memUid,
                                                  /*for_write=*/false,
                                                  lstart, lend);
                if (!lv) {
                    const EvalValue &v = cur.is<LValue *>()
                        ? cur.get<LValue *>()->get() : cur;
                    member_read_core(v, memId, memUid, false,
                                     lstart, lend, lstart, lend);
                    throw NotLValueEx(lstart, lend);
                }
                old = lv->get();
                nv = slot_rmw(*lv, cop, one);
                have_nv = true;
            }

        } else {

            if (cur.is<LValue *>()) {
                SharedArrayObj *arr;
                if (flat_writable_array(cur.get<LValue *>(), arr)) {
                    EvalValue out;
                    if (flat_store_core(cur.get<LValue *>(), *arr, key, one,
                                        cop, out, lstart, lend, kstart, kend,
                                        &old)) {
                        nv = out;
                        have_nv = true;
                    }
                }
            }

            if (!have_nv) {
                EvalValue elv;
                try {
                    Type *ct = cur.is<LValue *>()
                        ? cur.get<LValue *>()->get().get_type()
                        : cur.get_type();
                    elv = ct->subscript(cur, key, /*for_write=*/false);
                } catch (Exception &e) {
                    if (!e.loc_start) {
                        e.loc_start = lstart;
                        e.loc_end = lend;
                    }
                    throw;
                }
                if (!elv.is<LValue *>())
                    throw NotLValueEx(lstart, lend);
                old = elv.get<LValue *>()->get();
                nv = slot_rmw(*elv.get<LValue *>(), cop, one);
                have_nv = true;
            }
        }

        return is_prefix ? nv : old;
    }

    /* Tier 3 (dyn): the checked read-modify-write every engine shares. */
    EvalValue old;
    const EvalValue nv = is_member
        ? dyn_incdec_member(cur, memId, memUid, is_inc, old, lstart, lend,
                            bstart, bend, id_start, id_end)
        : dyn_incdec_elem(cur, key, is_inc, old, lstart, lend,
                          id_start, id_end);
    return is_prefix ? nv : old;
}

/*
 * VM StoreMemberV: native `s.member = v` / `s.member OP= v` for a STRUCT base (a
 * dict member store goes through DictStore). Mirrors the tree-walker's
 * member_store (a POD field: coerce + byte store; a boxed field: the field
 * lvalue + slot_rmw), but AST-free: `base_lv` from a slot, the member's
 * uid + carets from the member-key pool. Returns the stored value. A const /
 * read-only struct throws NotLValueEx (the tree-walker's general-path error).
 */
/* The boxed field LValue* of `base.member` for a MUTATING builtin arg0
 * (`append(s.f, x)` — the VM's CallBuiltinLVMember). Throws exactly like the
 * tree-walker's MemberExpr on a non-struct base / missing member / const|
 * read-only|POD field (a POD scalar field has no LValue). Mirrors
 * vm_member_store's boxed branch. */
LValue *vm_member_lvalue(LValue *base_lv, const UniqueId *memUid,
                         const Loc &mstart, const Loc &mend,
                         const Loc &bstart, const Loc &bend, LValue &hold)
{
    const EvalValue &dval = base_lv->get();
    if (!dval.is<intrusive_ptr<StructObject>>())
        throw TypeErrorEx("Expected struct object", bstart, bend);
    StructObject &obj = *dval.get<intrusive_ptr<StructObject>>().get();
    const int slot = obj.def->slot_of(memUid);
    if (slot < 0)
        throw TypeErrorEx(
            intern_msg("Struct '" + string(obj.def->name->val) +
                       "' has no member '" + string(memUid->val) + "'"),
            mstart, mend);
    /* a POD struct's field: not a location - the member read gives a
     * VALUE, and the builtin works on it held, as the tree-walker's
     * (lv_builtin_target) */
    if (obj.is_pod()) {
        hold = LValue(obj.is_pod() ? obj.pod_get(slot) : obj.fields[slot].get(),
                      false);
        return &hold;
    }
    /* the builtin may change the field (a slice detached, a value put):
     * the struct is the slot's own first (struct_own) */
    return &struct_own(base_lv).fields[slot];
}

EvalValue vm_member_store(LValue *base_lv, const UniqueId *memUid, Op op,
                          const EvalValue &value,
                          const Loc &mstart, const Loc &mend,
                          const Loc &bstart, const Loc &bend,
                          const StructTypeDef *bake_def, int bake_slot)
{
    const EvalValue &dval = base_lv->get();
    if (!dval.is<intrusive_ptr<StructObject>>())
        throw TypeErrorEx("Expected struct object", bstart, bend);

    StructObject &obj = *dval.get_ref<intrusive_ptr<StructObject>>().get();
    /* the compile-baked field slot (behind the def-identity check) skips
     * the slot_of name scan - the 64_struct_create fix */
    const int slot = obj.def == bake_def && bake_slot >= 0
        ? bake_slot : obj.def->slot_of(memUid);
    if (slot < 0)
        throw TypeErrorEx(
            intern_msg("Struct '" + string(obj.def->name->val) +
                       "' has no member '" + string(memUid->val) + "'"),
            mstart, mend);

    /* the struct is the slot's own before the write (struct_own): a copy
     * of it elsewhere - another variable, an element, a parameter's
     * caller - must not see the write */
    StructObject &own = struct_own(base_lv);

    if (own.is_pod()) {
        const EvalValue r = RValue(value);
        EvalValue newval;
        if (op == Op::assign) {
            newval = r;
        } else {
            newval = own.pod_get(slot);
            apply_compound_op(newval, r, op);
        }
        newval = coerce_struct_field(own.def->fields[slot], std::move(newval),
                                     mstart, mend);
        own.pod_set(slot, newval);
        return newval;
    }

    return slot_rmw(own.fields[slot], op, value);   /* boxed field lvalue */
}

/*
 * VM StoreElem2V: native NESTED general store `a[i][j] = v` / `a[i][j] OP= v`
 * (a Subscript lvalue whose base is another Subscript over a slotted base). The
 * inner `a[i]` is READ as a reference (for_write=false) - exactly like
 * Subscript::do_eval consuming assign_target only at the OUTERMOST subscript, so
 * a missing dict key in the nested base throws/defaults on the read - then the
 * outer `[j]` stores into that reference (COW writes back through the inner
 * element's container back-pointer, so the write is visible in `a`). This is
 * the two-level form of vm_subscript_store; the dispatch mirrors the
 * tree-walker's `t = lval.is<LValue*>() ? ... : lval.get_type()`.
 */
EvalValue vm_nested_subscript_store(LValue *outer_base, const EvalValue &key1,
                                    const EvalValue &key2,
                                    const EvalValue &value, Op op,
                                    const std::pair<Loc, Loc> *locs)
{
    /* `locs[0]` = the INNER subscript caret, `locs[1]` = the OUTER's - read ONLY
     * on the throw path (a pointer keeps the hot store cheap). The INNER read
     * `a[i]` throws locs[0], the FINAL store `[j]` locs[1] - byte-identical to
     * the tree-walker's per-node stamp. */
    EvalValue inner;
    try {
        inner = outer_base->get().get_type()->subscript(
            EvalValue(outer_base), key1, /*for_write=*/false);
    } catch (Exception &e) {
        if (!e.loc_start) { e.loc_start = locs[0].first;
                            e.loc_end = locs[0].second; }
        throw;
    }

    /* A FLAT inner array (`[[1,2],[3,4]]`: general outer, flat int inners) has
     * no element LValue - store the scalar straight into its buffer, exactly
     * like the single-level flat store (COW-correct). */
    if (inner.is<LValue *>()) {
        SharedArrayObj *arr;
        if (flat_writable_array(inner.get<LValue *>(), arr)) {
            EvalValue fout;
            if (flat_store_core(inner.get<LValue *>(), *arr, key2, value, op,
                                fout, locs[1].first, locs[1].second,
                                locs[1].first, locs[1].second))
                return fout;
            /* a compound op on a flat struct array: defer to the general path
             * below, which raises the same error as the tree-walker. */
        }
    }

    /* GENERAL inner (array<dyn>/array<array>): the element IS an LValue. The
     * final store's loc-less throws are stamped the OUTER subscript's caret. */
    try {
        Type *t = inner.is<LValue *>()
            ? inner.get<LValue *>()->get().get_type()
            : inner.get_type();
        const bool for_write = (op == Op::assign);
        EvalValue elv = t->subscript(inner, key2, for_write);
        if (!elv.is<LValue *>())
            throw NotLValueEx(locs[1].first, locs[1].second);
        return slot_rmw(*elv.get<LValue *>(), op, value);
    } catch (Exception &e) {
        /* an OPERATION error (op_caret) is not the outer subscript's: it
         * stays loc-less for the op's op_locs caret (the handler's /
         * the JIT conveyance's stamp) */
        if (!e.loc_start && !e.op_caret) {
            e.loc_start = locs[1].first;
            e.loc_end = locs[1].second;
        }
        throw;
    }
}

/*
 * GENERIC N-level nested store `a[k0][k1]...[kn] = v` / `OP= v` (the VM's
 * StoreElemChainV; vm_nested_subscript_store is the n==2 special case): walk
 * keys[0 .. nkeys-2] as READS (for_write=false), descending to the innermost
 * container exactly like the tree-walker's nested Subscript chain (assign_target
 * is consumed only at the OUTERMOST), then store keys[nkeys-1] into it - a FLAT
 * inner via flat_store_core, a GENERAL inner's element via slot_rmw. `keys` are
 * in BASE-to-innermost order (keys[0] applies to the base). COW propagates back
 * through the element LValues' container back-pointers, byte-identical to the
 * tree-walker.
 */
EvalValue vm_subscript_chain_store(LValue *base, const EvalValue *keys,
                                   size_t nkeys, const EvalValue &value, Op op,
                                   const std::pair<Loc, Loc> *steplocs)
{
    EvalValue cur = EvalValue(base);   /* wraps the current container LValue* */
    /* Each INTERMEDIATE read throws with ITS subscript node's loc (byte-
     * identical to the tree-walker's per-node stamp); a loc-less internal OOB /
     * KeyNotFound is stamped here. The FINAL store's throws are loc-less and the
     * CALLER stamps them the outer/side-table loc (== the outermost subscript
     * = steplocs[nkeys-1]). */
    /*
     * An intermediate step may yield a VALUE rather than an element LValue -
     * a READ-ONLY container (a const, reached through a parameter) hands out
     * rvalues, and so does a flat element. The walk CONTINUES on the value,
     * exactly like the tree-walker's chained Subscript::do_eval: a read-only
     * inner is only refused at the FINAL store (NotLValueEx at the whole
     * lvalue), and a scalar raises its own TypeErrorEx at the step that
     * indexes it. Refusing at the step (as this did) carets `p[1]` where the
     * tree-walker carets `p[1][0][0]` - a RULE 2 break (#38).
     */
    for (size_t k = 0; k + 1 < nkeys; k++) {
        const Loc ks = steplocs[k].first, ke = steplocs[k].second;
        Type *t = cur.is<LValue *>()
            ? cur.get<LValue *>()->get().get_type() : cur.get_type();
        EvalValue next;
        try {
            next = t->subscript(cur, keys[k], /*for_write=*/false);
        } catch (Exception &e) {
            if (!e.loc_start) { e.loc_start = ks; e.loc_end = ke; }
            throw;
        }
        cur = std::move(next);
    }

    const Loc fs = steplocs[nkeys - 1].first, fe = steplocs[nkeys - 1].second;
    LValue *inner = cur.is<LValue *>() ? cur.get<LValue *>() : nullptr;
    SharedArrayObj *arr;
    if (inner && flat_writable_array(inner, arr)) {
        EvalValue fout;
        if (flat_store_core(inner, *arr, keys[nkeys - 1], value, op,
                            fout, fs, fe, fs, fe))
            return fout;
    }
    /* The FINAL store's throws (subscript OOB/KeyNotFound, slot_rmw type) are
     * loc-less; stamp them the OUTERMOST subscript's caret. */
    try {
        Type *t = inner ? inner->get().get_type() : cur.get_type();
        const bool for_write = (op == Op::assign);
        EvalValue elv = t->subscript(cur, keys[nkeys - 1], for_write);
        if (!elv.is<LValue *>())
            throw NotLValueEx(fs, fe);
        return slot_rmw(*elv.get<LValue *>(), op, value);
    } catch (Exception &e) {
        /* an OPERATION error keeps no step caret: see the nested store */
        if (!e.loc_start && !e.op_caret) {
            e.loc_start = fs;
            e.loc_end = fe;
        }
        throw;
    }
}

/* Field #fidx of element `idx` of a GENERAL array whose elements are POD
 * structs (a promoted array<PodStruct>): the boxed StructObject's field, in
 * its own kind (int / float / bool -> 0/1). An element that is not a struct,
 * or a field index it does not have, can only come from a corrupt image and
 * reads 0 - the defined value of the untrusted tier (#142). */
template <class T>
static T general_struct_field(const SharedArrayObj &arr, size_type idx,
                              int_type fidx)
{
    const EvalValue &ev = arr.get_vec()[arr.offset() + idx].get();
    if (!ev.is<intrusive_ptr<StructObject>>())
        return static_cast<T>(0);
    const StructObject &so = *ev.get_ref<intrusive_ptr<StructObject>>();
    if (!so.def || fidx < 0
        || static_cast<size_t>(fidx) >= so.def->fields.size()
        || !so.def->is_pod())
        return static_cast<T>(0);
    const FieldKind k = so.def->fields[fidx].kind;
    if (k != FieldKind::f_int && k != FieldKind::f_float
        && k != FieldKind::f_bool)
        return static_cast<T>(0);
    const EvalValue fv = so.pod_get(static_cast<int>(fidx));
    if (fv.is<float_type>())
        return static_cast<T>(fv.get<float_type>());
    if (fv.is<int_type>())
        return static_cast<T>(fv.get<int_type>());
    return static_cast<T>(fv.get_type()->is_true(fv) ? 1 : 0);   /* bool */
}

/* Read scalar field #fidx of element `idx` of an array<PodStruct> (the VM's
 * LoadStructFieldInt/Float, the struct-foreach direct read). `arrv` is the
 * array value; the codegen proved `idx` in range (the counted loop over an
 * inert body). A bool field reads as 0/1.
 *
 * ⛔ The codegen did NOT prove the STORAGE flat - only the static type
 * array<P>, and a flat struct array AUTO-PROMOTES to general storage on a
 * cold op (insert/sort/map, via get_vec()) anywhere BEFORE the loop. This
 * read used to return 0 for every element of such an array (a silent wrong
 * answer, found by the #54 -nc audit: an un-folded `var pts = []` was built
 * general). A general array is read the boxed way, as G4's subscript form
 * below always did. */
int_type vm_struct_field_int(const EvalValue &arrv, int_type idx,
                    int_type fidx)
{
    /*
     * #137 tier 2 + #142: `fidx` is an INSTRUCTION operand and the def is
     * whatever the slot holds at RUN time, so verify_chunk can bound it by
     * nothing tighter than the widest struct in the program - a narrow
     * struct read with a wide one's field index arrives here in range for
     * tier 1 and out of range for this vector.
     *
     * It must not THROW to say so: jit_load_struct_field and
     * jit_struct_field_add_int are `noexcept` with no catch, so an
     * exception here is std::terminate. A defined value it is - the same
     * bargain the rest of the untrusted tier makes.
     */
    if (!arrv.is<SharedArrayObj>())
        return 0;
    const SharedArrayObj &arr = arrv.get_ref<SharedArrayObj>();
    if (idx < 0 || static_cast<size_type>(idx) >= arr.size())
        return 0;
    if (arr.skind() == SharedArrayObj::Storage::general)
        return general_struct_field<int_type>(
            arr, static_cast<size_type>(idx), fidx);
    if (arr.skind() != SharedArrayObj::Storage::structs)
        return 0;
    const auto &sv = arr.flat_structs();
    if (fidx < 0 || static_cast<size_t>(fidx) >= sv.def->fields.size())
        return 0;
    const FieldDef &f = sv.def->fields[fidx];
    const char *p =
        sv.buf.data() + (arr.offset() + idx) * sv.stride + f.offset;
    if (f.kind == FieldKind::f_bool)
        return static_cast<unsigned char>(*p) != 0 ? 1 : 0;
    int_type v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

float_type vm_struct_field_float(const EvalValue &arrv, int_type idx,
                      int_type fidx)
{
    /*
     * #137 tier 2 + #142: `fidx` is an INSTRUCTION operand and the def is
     * whatever the slot holds at RUN time, so verify_chunk can bound it by
     * nothing tighter than the widest struct in the program - a narrow
     * struct read with a wide one's field index arrives here in range for
     * tier 1 and out of range for this vector.
     *
     * It must not THROW to say so: jit_load_struct_field and
     * jit_struct_field_add_int are `noexcept` with no catch, so an
     * exception here is std::terminate. A defined value it is - the same
     * bargain the rest of the untrusted tier makes.
     */
    if (!arrv.is<SharedArrayObj>())
        return 0.0;
    const SharedArrayObj &arr = arrv.get_ref<SharedArrayObj>();
    if (idx < 0 || static_cast<size_type>(idx) >= arr.size())
        return 0.0;
    if (arr.skind() == SharedArrayObj::Storage::general)   /* see above */
        return general_struct_field<float_type>(
            arr, static_cast<size_type>(idx), fidx);
    if (arr.skind() != SharedArrayObj::Storage::structs)
        return 0.0;
    const auto &sv = arr.flat_structs();
    if (fidx < 0 || static_cast<size_t>(fidx) >= sv.def->fields.size())
        return 0.0;
    const FieldDef &f = sv.def->fields[fidx];
    const char *p =
        sv.buf.data() + (arr.offset() + idx) * sv.stride + f.offset;
    float_type v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

/*
 * G4: the CHECKED `a[i].f` scalar read - the SUBSCRIPT form of
 * LoadStructFieldInt/Float.
 *
 * The foreach form above is unchecked because its gate proved both facts it
 * needs: flat-struct storage, and an in-range loop counter. A SUBSCRIPT base
 * proves NEITHER - a flat array<PodStruct> AUTO-PROMOTES to general storage
 * on any cold op (insert/sort/map, via get_vec()), and the index is
 * arbitrary - so this does the negative-index wrap and the bounds check
 * itself and serves BOTH storages.
 *
 * The element is a POD struct either way (the codegen gate requires it), so
 * the general arm reads the same FIELD INDEX out of the boxed StructObject.
 * OutOfBoundsEx is thrown LOC-LESS: the interpreter stamps it from the loc
 * side table and the JIT re-raises it through EnterNative, so both engines
 * report the op's own caret (the SUBSCRIPT's span - see task #125).
 */
template <class T>
static T vm_struct_elem_field(const EvalValue &arrv, int_type idx,
                              int_type fidx)
{
    const SharedArrayObj &arr = arrv.get_ref<SharedArrayObj>();

    if (idx < 0)
        idx += static_cast<int_type>(arr.size());
    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx();

    if (arr.skind() == SharedArrayObj::Storage::structs) {
        const auto &sv = arr.flat_structs();
        /* #137 tier 2 - see the twin in vm_struct_field_int above. The
         * OutOfBoundsEx raised earlier is a LANGUAGE error and stays a
         * throw (its caller conveys it); a bad field index is CORRUPTION
         * and takes the defined value instead. */
        if (fidx < 0 || static_cast<size_t>(fidx) >= sv.def->fields.size())
            return static_cast<T>(0);
        const FieldDef &f = sv.def->fields[fidx];
        const char *p = sv.buf.data()
            + (arr.offset() + static_cast<size_type>(idx)) * sv.stride
            + f.offset;
        if (f.kind == FieldKind::f_bool)
            return static_cast<T>(static_cast<unsigned char>(*p) != 0 ? 1 : 0);
        if (f.kind == FieldKind::f_float) {
            float_type v;
            std::memcpy(&v, p, sizeof v);
            return static_cast<T>(v);
        }
        int_type v;
        std::memcpy(&v, p, sizeof v);
        return static_cast<T>(v);
    }

    /* PROMOTED (or built) general: a boxed StructObject per element. The
     * same FIELD INDEX applies - the element is a POD struct either way -
     * and pod_get hands back the field's own kind, so one arm per kind
     * covers int / float / bool for BOTH the int and float readers (an
     * int field feeding a float read is try_member_scalar's promoting
     * form). */
    const EvalValue &ev =
        arr.get_vec()[arr.offset() + static_cast<size_type>(idx)].get();
    const EvalValue fv =
        ev.get_ref<intrusive_ptr<StructObject>>()->pod_get(
            static_cast<int>(fidx));
    if (fv.is<float_type>())
        return static_cast<T>(fv.get<float_type>());
    if (fv.is<int_type>())
        return static_cast<T>(fv.get<int_type>());
    return static_cast<T>(fv.get_type()->is_true(fv) ? 1 : 0);   /* bool */
}

int_type vm_struct_elem_field_int(const EvalValue &arrv, int_type idx,
                                  int_type fidx)
{
    return vm_struct_elem_field<int_type>(arrv, idx, fidx);
}

float_type vm_struct_elem_field_float(const EvalValue &arrv, int_type idx,
                                      int_type fidx)
{
    return vm_struct_elem_field<float_type>(arrv, idx, fidx);
}

/* Materialize element `idx` of a flat array<PodStruct> as a FRESH StructObject
 * (the VM's LoadStructElemV - the whole-`p` foreach bind). Byte-identical to the
 * tree-walker's reused-object bind (its COW guard only avoids overwriting a
 * captured/stored element - a fresh alloc satisfies that trivially). `idx` is
 * loop-bounded, so no bounds check. */
EvalValue vm_struct_elem(const EvalValue &arrv, int_type idx)
{
    const SharedArrayObj &arr = arrv.get_ref<SharedArrayObj>();
    /* #53: the index is bounded by the length at the loop's START, and the
     * body may have shrunk the array (the foreach's defined OutOfBoundsEx,
     * loc-less - the op's caret is stamped by the raise) or PROMOTED it to
     * general storage (a cold op on a struct array; the element is then
     * read the general way, as the tree-walker does). */
    if (idx < 0 || static_cast<size_type>(idx) >= arr.size())
        throw OutOfBoundsEx();
    if (arr.skind() == SharedArrayObj::Storage::general)
        return arr_elem_boxed(arr, static_cast<size_type>(idx));
    const auto &sv = arr.flat_structs();   /* any other kind: an image's */
    auto obj = make_intrusive<StructObject>(sv.def);
    std::memcpy(obj->bytes.data(),
                sv.buf.data() + (arr.offset() + idx) * sv.stride, sv.stride);
    return EvalValue(intrusive_ptr<StructObject>(obj));
}

/* #110: the whole-`p` foreach bind INTO the loop variable's slot. The
 * tree-walker reuses one StructObject across iterations (Foreach's
 * `reuse`); here the slot itself is the holder: when it still holds the
 * previous iteration's object and NOTHING ELSE references it - use_count
 * 1, i.e. the body neither stored, captured, appended nor aliased it -
 * the element's bytes are copied over it in place, and no object is
 * allocated or freed. Anything else (a captured element, a different
 * def, a read-only value the body assigned, a general array) takes the
 * fresh-object path, so a captured element keeps its value. */
void vm_struct_elem_into(LValue &dst, const EvalValue &arrv, int_type idx)
{
    const SharedArrayObj &arr = arrv.get_ref<SharedArrayObj>();
    if (idx >= 0 && static_cast<size_type>(idx) < arr.size()
        && arr.skind() == SharedArrayObj::Storage::structs
        && dst.get().is<intrusive_ptr<StructObject>>()) {
        const auto &sv = arr.flat_structs();
        const intrusive_ptr<StructObject> &o =
            dst.get().get_ref<intrusive_ptr<StructObject>>();
        if (o.use_count() == 1 && o->def == sv.def && !o->readonly
            && o->bytes.size() == static_cast<size_t>(sv.stride)) {
            std::memcpy(o->bytes.data(),
                        sv.buf.data() + (arr.offset() + idx) * sv.stride,
                        sv.stride);
            return;
        }
    }
    dst.put(vm_struct_elem(arrv, idx));
}

/*
 * One assignment to `lvalue`. `old_out`, when given (a compound store only:
 * IncDecExpr's typed `±= 1`), receives the value the store replaced - read
 * at the store itself, so postfix `++` / `--` yields it exactly (deriving it
 * as new -/+ 1 was wrong for a float: (0.1 + 1) - 1 is not 0.1).
 */
static EvalValue
handle_single_expr14(EvalContext *ctx,
                     bool inDecl,
                     Op op,
                     Construct *lvalue,
                     const EvalValue &rval_in,
                     EvalValue *old_out = nullptr)
{
    /*
     * For a plain `=` to an explicitly-typed scalar variable, coerce a widening
     * value to the declared type (float <- int/bool, int <- bool). The decl's
     * type is on the Identifier (the resolver also copied it to every use, so a
     * later reassignment coerces too). Compound ops (`+=`) already produce the
     * right type via num_bin_op, so only `assign` is handled here.
     */
    EvalValue rval_storage;
    const EvalValue *rvalp = &rval_in;
    if (op == Op::assign && lvalue->is_id()) {
        const DeclType dt = static_cast<const Identifier *>(lvalue)->decl_type;
        if (dt == DeclType::f || dt == DeclType::i) {
            rval_storage = coerce_to_decl_type(RValue(rval_in), dt);
            rvalp = &rval_storage;
        }
    }
    const EvalValue &rval = *rvalp;

    /*
     * Fast path: declaring a resolved local. Write its slot. The resolver
     * already rejected illegal same-block redeclarations, so we just
     * (over)write - which is also exactly what a loop re-entry needs (the decl
     * re-binds the slot each iteration). The
     * dynamic_cast is gated on inDecl so plain assignments (the hot path, e.g.
     * `s += i`) never pay for it; an assignment to a resolved local instead
     * flows through the normal lvalue->eval() -> LValue* -> doAssign path
     * below.
     */
    if (inDecl) {

        if (const Identifier *id = as_resolved_local(lvalue)) {

            Frame *f = ctx->frame;
            f->slots[id->sym.slot] =
                LValue(RValue(rval), ctx->const_ctx || lvalue->is_const);
            return rval;
        }

        /* Declaring an escaped top-level variable: bind its global-table slot
         * (the analogue of the local-frame write above). A global symbol implies
         * the table exists; guard anyway so a null gfuncs falls through. */
        if (const Identifier *id = as_resolved_global(lvalue)) {

            if (GlobalFuncTable *gf = ctx->gfuncs) {
                gf->define(id->sym.slot,
                           LValue(RValue(rval),
                                  ctx->const_ctx || lvalue->is_const));
                return rval;
            }
        }

    } else if (!ctx->const_ctx) {

        /*
         * Fast path: an assignment / compound-assignment to a resolved, live,
         * non-const local. The slot's LValue has no `container` (only array
         * elements do), so we read-modify-write it in place, skipping the
         * lvalue->eval() -> LValue* wrapping and the doAssign() dispatch the
         * general path below would run. Falls through to that path when the
         * slot is not live (so an undefined-variable error is still raised) or
         * is const (so the rebind error is still raised), keeping behavior
         * identical. The same in-place op (apply_compound_op / RValue) is used.
         */
        if (const Identifier *id = as_resolved_local(lvalue)) {

            if (Frame *f = ctx->frame) {

                LValue &lv = f->slots[id->sym.slot];

                if (!lv.is_const_var()) {
                    if (old_out)
                        *old_out = lv.get();
                    return slot_rmw(lv, op, rval);
                }
            }
        }

        /*
         * Fast path: an assignment / compound-assignment to an escaped
         * top-level variable (a global-table slot). Mirrors the local slot path
         * above. Falls through to the general path when the slot is undefined
         * (use-before-decl -> the undefined-variable error) or const (rebind).
         */
        if (const Identifier *id = as_resolved_global(lvalue)) {

            GlobalFuncTable *gf = ctx->gfuncs;

            if (gf && gf->defined[id->sym.slot]) {

                LValue &lv = gf->slots[id->sym.slot];

                if (!lv.is_const_var()) {
                    if (old_out)
                        *old_out = lv.get();
                    return slot_rmw(lv, op, rval);
                }
            }
        }

        /*
         * Fast path: an assignment / compound-assignment to a closure CAPTURE
         * slot (e.g. a counter closure's `start++`). The write persists in the
         * FuncObject's capture vector across calls. Falls through when the
         * capture is const (the rebind error).
         */
        if (const Identifier *id = as_resolved_capture(lvalue)) {

            if (ctx->captures) {

                LValue &lv = (*ctx->captures)[id->sym.slot];

                if (!lv.is_const_var()) {
                    if (old_out)
                        *old_out = lv.get();
                    return slot_rmw(lv, op, rval);
                }
            }
        }

    }

    /* A store through an access chain: its base and key once, then the
     * flat / POD / general store (subscript_store, member_store). */
    if (!inDecl) {
        if (lvalue->is_subscript())
            return subscript_store(ctx, static_cast<Subscript *>(lvalue),
                                   op, rval, old_out);
        if (ctag(lvalue) == ConstructType::member)
            return member_store(ctx, static_cast<MemberExpr *>(lvalue),
                                op, rval, old_out);
    }

    /*
     * Mark the lvalue eval as a plain-assignment target (op == assign) so a
     * dict subscript/member auto-vivifies a missing key instead of throwing
     * (`d[k] = v` inserts; a compound `d[k] += v` or a read does not - those
     * throw on a missing key in a plain dict). The outermost subscript/member
     * consumes the flag; reset it afterwards for non-subscript lvalues.
     */
    ctx->assign_target = (!inDecl && op == Op::assign);
    const EvalValue &lval = lvalue->eval(ctx);
    ctx->assign_target = false;

    if (lval.is<UndefinedId>()) {

        if (!inDecl)
            throw UndefinedVariableEx{ lval.get<UndefinedId>().id };

        ctx->emplace(
            lval.get<UndefinedId>().id,
            RValue(rval),
            ctx->const_ctx || lvalue->is_const
        );

    } else if (lval.is<LValue *>()) {

        if (ctx->const_ctx) {

            /*
             * We're in const ctx, and we're trying to change the value of a
             * symbol. That should not be possible. The parser won't try to evaluate
             * an assignment statement during const evaluation. If that happens,
             * a bug has been just introduced, in a recent changed.
             */
            throw InternalErrorEx();

        } else if (lval.get<LValue *>()->is_const_var()) {

            if (lval.get<LValue *>()->is<Builtin>())
                throw CannotRebindBuiltinEx(lvalue->start, lvalue->end);

            throw CannotRebindConstEx(lvalue->start, lvalue->end);
        }

        if (inDecl) {

            const EvalValue &local_lval = lvalue->eval(ctx, false);

            if (!local_lval.is<UndefinedId>()) {

                /* REPL: a re-declaration of an existing global rebinds it
                 * (fresh value + const-ness) - see
                 * EvalContext::allow_redeclare.
                 * Otherwise, re-defining the same variable in the same scope is
                 * an error (the script rule). */
                if (ctx->allow_redeclare && lvalue->is_id()) {
                    const Identifier *id =
                        static_cast<const Identifier *>(lvalue);
                    ctx->erase(id);
                    ctx->emplace(id, RValue(rval), lvalue->is_const);
                    return rval;
                }

                throw AlreadyDefinedEx(lvalue->start, lvalue->end);
            }

            /* We're re-declaring a symbol already declared outside */
            ctx->emplace(
                local_lval.get<UndefinedId>().id,
                RValue(rval),
                ctx->const_ctx
            );

        } else {
            if (old_out)
                *old_out = lval.get<LValue *>()->get();
            return doAssign(lval, rval, op);
        }

    } else {
        throw NotLValueEx(lvalue->start, lvalue->end);
    }

    return rval;
}

EvalValue Expr14::do_eval(EvalContext *ctx, bool rec) const
{
    const bool inDecl = fl & pFlags::pInDecl;

    /*
     * Fast path for `local += N` / `-= N` / `*= N` where N is an int literal
     * and the slot currently holds a live, non-const int. Mutates the slot's
     * int in place, skipping both the rvalue-node eval (the literal) and the
     * num_bin_op dispatch. add/sub/mul can't fault (they wrap, -fwrapv), so no
     * check is needed; div/mod fall through to the general path (zero check).
     * This is what an `i++` / `i += 1` increment would compile to.
     */
    if (!inDecl && !ctx->const_ctx &&
        (op == Op::addeq || op == Op::subeq || op == Op::muleq)) {

        if (const Identifier *id = as_resolved_local(lvalue.get())) {

            /*
             * The rhs is an int when it is an int literal OR a node the
             * inferencer proved is a non-null int (th == i) - e.g. a POD struct
             * field `p.x`. The latter reads UNBOXED via eval_int() (no
             * pod_get/EvalValue), which is what makes `sx += p.x` over a flat
             * struct array a true unboxed reduction (plans/archived/structs.md phase 8).
             */
            const bool rhs_int =
                rvalue->is_lit_int() || rvalue->th == TypeHint::i;

            if (rhs_int) {

                Frame *f = ctx->frame;

                if (f) {

                    LValue &lv = f->slots[id->sym.slot];

                    if (!lv.is_const_var() && lv.is<int_type>()) {

                        const int_type n = rvalue->is_lit_int()
                            ? static_cast<const LiteralInt *>(
                                  rvalue.get())->ival()
                            : rvalue->eval_int(ctx);   /* unboxed */

                        int_type &v = lv.getval<int_type>();

                        if (op == Op::addeq)      v += n;
                        else if (op == Op::subeq) v -= n;
                        else                      v *= n;

                        return v;
                    }
                }
            }
        }
    }

    /* Evaluate the rhs first; point errors (undefined var, ...) at the rhs
     * itself, not at the whole `lhs = rhs` assignment. */
    EvalValue rval_storage;
    try {
        rval_storage = literal_widen(RValue(rvalue->eval(ctx)), rv_coerce);
    } catch (Exception &e) {
        stamp_operand_loc(rvalue.get(), e);
        throw;
    }
    const EvalValue &rval = rval_storage;
    IdList *idlist = nullptr;

    if (lvalue->is_idlist())
        idlist = static_cast<IdList *>(lvalue.get());

    if (idlist) {

        if (!rval.is<SharedArrayObj>()) {

            for (const auto &e: idlist->elems) {
                if (e->is_id()
                    && static_cast<Identifier *>(e.get())->is_underscore())
                    continue;   /* `_` placeholder: skip */
                handle_single_expr14(ctx, inDecl, op, e.get(), rval);
            }

        } else {

            const SharedArrayObj &arr = rval.get<SharedArrayObj>();
            const size_type asz = arr.size();

            /*
             * STRICT array destructuring: the array must have EXACTLY as many
             * elements as there are targets (same rule as foreach unpack). A
             * length mismatch is an error, not a silent none-pad (too few) or
             * dropped extras (too many). The non-array case above still spreads
             * the same value to each target - a deliberate convenience kept.
             */
            if (asz != idlist->elems.size())
                throw TypeErrorEx(
                    intern_msg("cannot unpack an array of length " +
                               std::to_string(asz) + " into " +
                               std::to_string(idlist->elems.size()) +
                               " variables"),
                    lvalue->start, lvalue->end);

            for (size_type i = 0; i < idlist->elems.size(); i++) {

                if (idlist->elems[i]->is_id()
                    && static_cast<Identifier *>(idlist->elems[i].get())
                           ->is_underscore())
                    continue;   /* `_` placeholder: skip this array slot */

                handle_single_expr14(
                    ctx,
                    inDecl,
                    op,
                    idlist->elems[i].get(),
                    arr_elem_boxed(arr, i)
                );
            }
        }

        return none;

    } else {

        EvalValue r = handle_single_expr14(ctx, inDecl, op, lvalue.get(),
                                           rval);
        /* A plain store's VALUE into an element or a field is the rvalue
         * converted to the target's STATIC type (val_widen; a dict's
         * rv_coerce already widened `rval`) - not whatever conversion the
         * container ran at run time, which a dyn target cannot predict
         * statically: `var z = (dp.x = true)` with a dyn `dp` is the bool
         * the inferencer typed, though the int field stored 1. */
        if (op == Op::assign && !inDecl
                && (lvalue->is_subscript()
                    || ctag(lvalue.get()) == ConstructType::member))
            return literal_widen(rval, val_widen);
        return r;
    }
}

/*
 * C-style ++ / -- on an int/float lvalue. Two paths, both evaluating the
 * operand exactly ONCE:
 *
 *  - statically int/float (th stamped by the inferencer - the usual case,
 *    incl. flat-array elements and POD struct fields, which have no LValue):
 *    route the mutation through handle_single_expr14 (`operand += 1`), which
 *    reuses every store fast path (slot, flat array, COW, struct). It returns
 *    the NEW value and reports the value the store replaced, which postfix
 *    returns (deriving it as new -/+ 1 was wrong for a float);
 *
 *  - a `dyn` / un-hinted operand (always LValue-backed - a dyn value is never
 *    flat): read-modify-write through the LValue so the int/float requirement
 *    can be enforced at runtime (a `dyn` holding a string/bool throws here).
 *
 * The inferencer rejects a non-lvalue / const / non-numeric operand at compile
 * time; the runtime checks are the `dyn` safety net.
 */
/* A value-selecting expression (`c ? a : b`, `a ?? b`) whose arms join
 * to a numeric type WIDENS the arm it picked to that type (task #75):
 * `c ? 1 : 2.5` is a float, and the VM's typed paths read it as one - the
 * raw int was the divergence. th == i without th_bool is an int (a bool
 * arm widens); a th_bool node is a real bool and keeps its arms. */
static EvalValue select_widen(const Construct *self, EvalValue v)
{
    if (self->th == TypeHint::f)
        return literal_widen(std::move(v), DeclType::f);
    if (self->th == TypeHint::i && !self->th_bool)
        return literal_widen(std::move(v), DeclType::i);
    return v;
}

EvalValue TernaryExpr::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue c = RValue(condExpr->eval(ctx));
    if (c.get_type()->is_true(c))
        return select_widen(this, RValue(thenExpr->eval(ctx)));
    return select_widen(this, RValue(elseExpr->eval(ctx)));
}

EvalValue CoalesceExpr::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue l = RValue(lhs->eval(ctx));
    if (l.is<NoneVal>())
        return select_widen(this, RValue(rhs->eval(ctx)));
    return select_widen(this, std::move(l));
}

EvalValue IncDecExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * Fast path: a resolved, live, non-const LOCAL holding an int or float -
     * mutate the slot's scalar straight in place and return old (postfix) / new
     * (prefix), with NO handle_single_expr14 call and NO num_bin_op. This is
     * the loop-counter / scalar-var case; it must be at least as fast as
     * `i += 1` (and is slightly faster - no literal-1 operand to read).
     */
    if (!ctx->const_ctx) {
        if (const Identifier *id = as_resolved_local(lvalue.get())) {
            Frame *f = ctx->frame;
            if (f) {
                LValue &lv = f->slots[id->sym.slot];
                if (!lv.is_const_var()) {
                    if (lv.is<int_type>()) {
                        int_type &v = lv.getval<int_type>();
                        const int_type old = v;
                        v += is_inc ? 1 : -1;
                        return is_prefix ? EvalValue(v) : EvalValue(old);
                    }
                    if (lv.is<float_type>()) {
                        float_type &v = lv.getval<float_type>();
                        const float_type old = v;
                        v += is_inc ? 1.0 : -1.0;
                        return is_prefix ? EvalValue(v) : EvalValue(old);
                    }
                    /* another type (only reachable via a dyn alias): fall
                     * through to the general path, which raises the error. */
                }
            }
        }
    }

    const Op cop = is_inc ? Op::addeq : Op::subeq;
    const EvalValue one{static_cast<int_type>(1)};

    if (th == TypeHint::i || th == TypeHint::f) {

        /* The store reports the value it replaced: postfix yields exactly
         * that (a float's old cannot be derived from new - (0.1 + 1) - 1
         * is not 0.1, nor -1 + 1 the -0.0 it was). */
        EvalValue old;
        const EvalValue nv =
            handle_single_expr14(ctx, false, cop, lvalue.get(), one, &old);
        return is_prefix ? nv : old;
    }

    /*
     * dyn / un-hinted. An element or a member is the read-modify-write the
     * VM's dyn paths share (dyn_incdec_elem / dyn_incdec_member), with the
     * base and the key evaluated once, in the order Subscript / MemberExpr
     * evaluate them. A variable is an LValue.
     */
    if (lvalue->is_subscript()) {
        const Subscript *sub = static_cast<const Subscript *>(lvalue.get());
        EvalValue cur, key;
        LValue hold;
        try {
            cur = store_base_value(ctx, sub->what.get(), hold);
            if (cur.is<UndefinedId>())
                throw UndefinedVariableEx(cur.get<UndefinedId>().id,
                                          sub->what->start, sub->what->end);
            key = literal_widen(RValue(sub->index->eval(ctx)),
                                sub->key_coerce);
        } catch (Exception &e) {
            stamp_operand_loc(lvalue.get(), e);
            throw;
        }
        EvalValue old;
        const EvalValue nv = dyn_incdec_elem(cur, key, is_inc, old,
                                             sub->start, sub->end,
                                             start, end);
        return is_prefix ? nv : old;
    }

    if (ctag(lvalue.get()) == ConstructType::member &&
        !static_cast<const MemberExpr *>(lvalue.get())->optional) {
        const MemberExpr *m = static_cast<const MemberExpr *>(lvalue.get());
        MemberTarget t;
        LValue hold;
        try {
            t = member_target(ctx, m, hold);
            if (!t.place.bytes && t.base.is<UndefinedId>())
                throw UndefinedVariableEx(t.base.get<UndefinedId>().id,
                                          m->start, m->end);
        } catch (Exception &e) {
            stamp_operand_loc(lvalue.get(), e);
            throw;
        }
        /* a field in bytes no LValue covers (a flat struct array's element,
         * an inline POD field): the read-modify-write in place */
        if (t.place.bytes) {
            const int slot = t.place.def->slot_of(m->memUid);
            if (slot >= 0)
                return pod_place_incdec(t.place, slot, is_inc, is_prefix,
                                        m->start, m->end, start, end);
            t.base = pod_place_value(t.place);
        }
        EvalValue cur = t.base;
        EvalValue old;
        const EvalValue nv = dyn_incdec_member(cur, m->memId, m->memUid,
                                               is_inc, old, m->start, m->end,
                                               m->what->start, m->what->end,
                                               start, end);
        return is_prefix ? nv : old;
    }

    EvalValue lref;
    try {
        lref = lvalue->eval(ctx);
    } catch (Exception &e) {
        stamp_operand_loc(lvalue.get(), e);
        throw;
    }

    if (lref.is<UndefinedId>())
        throw UndefinedVariableEx(lref.get<UndefinedId>().id, start, end);

    if (!lref.is<LValue *>())
        throw NotLValueEx(start, end);

    LValue *lv = lref.get<LValue *>();

    /* the builtin's own name; a variable HOLDING a builtin is a type error
     * below, as in the VM */
    if (lv->is<Builtin>() && lv->is_const_var())
        throw CannotRebindBuiltinEx();

    if (lv->is_const_var())
        throw CannotChangeConstEx(start, end);

    EvalValue old = lv->get();

    if (!old.is<int_type>() && !old.is<float_type>())
        throw TypeErrorEx("'++'/'--' requires an int or float", start, end);

    EvalValue nv = old;
    apply_compound_op(nv, one, cop);
    lv->put(nv);

    return is_prefix ? nv : old;
}

EvalValue IfStmt::do_eval(EvalContext *ctx, bool rec) const
{
    if (eval_cond(condExpr.get(), ctx)) {

        if (thenBlock)
            thenBlock->eval(ctx);

    } else {

        if (elseBlock)
            elseBlock->eval(ctx);
    }

    return none;
}

EvalValue BreakStmt::do_eval(EvalContext *ctx, bool rec) const
{
    ctx->flow->type = FlowState::brk;
    return none;
}

EvalValue ContinueStmt::do_eval(EvalContext *ctx, bool rec) const
{
    ctx->flow->type = FlowState::cont;
    return none;
}

EvalValue ReturnStmt::do_eval(EvalContext *ctx, bool rec) const
{
    /* RValue() throws UndefinedVariableEx (with this stmt's loc) if needed */
    ctx->flow->value = elem
        ? literal_widen(RValue(elem->eval(ctx)), ret_coerce) : none;
    ctx->flow->type = FlowState::ret;
    return none;
}

EvalValue RethrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    throw RethrowEx{start, end};
}

EvalValue ThrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &e = RValue(elem->eval(ctx));

    /*
     * Throwing a struct instance: the struct IS the exception. Wrap it as a
     * named exception whose name is the struct's type (so `catch (T)` matches
     * by type) carrying the instance as the payload (so `catch (T as v)` binds
     * the instance and `v.field` reads it).
     */
    if (e.is<intrusive_ptr<StructObject>>()) {
        throw ExceptionObject(
            string(e.get<intrusive_ptr<StructObject>>()->def->name->val),
            e,
            e.get<intrusive_ptr<StructObject>>()->def->name
        );
    }

    /*
     * Re-throwing a caught built-in exception value (bound by `catch (X as e)`,
     * which hands back an exception object for a payload-less built-in).
     */
    if (e.is<intrusive_ptr<ExceptionObject>>())
        throw *e.get<intrusive_ptr<ExceptionObject>>().get();

    throw TypeErrorEx(
        "Can only throw a struct instance",
        elem->start,
        elem->end
    );
}

/*
 * Is this statement a NAMED func/struct declaration - one that must be BOUND
 * at scope entry rather than when the statement is reached (#134)?
 *
 * Sound because such a declaration cannot depend on the enclosing frame: the
 * grammar REJECTS a capture list on a named func (`func f[a](x)` is a
 * SyntaxError - captures are for closure EXPRESSIONS only), and a named
 * func's body is parented to `capture_root`, the program root, so it cannot
 * read an enclosing local either (verified: it reports "Undefined variable").
 * A struct is simpler still - its descriptor is computed at parse time.
 *
 * So creating the FuncObject / binding the descriptor at scope entry instead
 * of at the statement is UNOBSERVABLE, and it is what makes a call to a
 * function declared BELOW the call work - mutual recursion, "helpers at the
 * bottom", a struct constructed above its `struct` line.
 *
 * A LAMBDA is deliberately NOT included: `var f = func(x) {...}` is an
 * `Expr14` var declaration, not a FuncDeclStmt statement, so it keeps its
 * declaration-point binding and its temporal dead zone - its capture
 * snapshot must happen exactly where it is written.
 */
static bool is_hoistable_decl(const Construct *c)
{
    if (const auto *fd = dynamic_cast<const FuncDeclStmt *>(c))
        return fd->id != nullptr;
    if (const auto *sd = dynamic_cast<const StructDeclStmt *>(c))
        return sd->id != nullptr;
    return false;
}

/* Bind this block's named func/struct declarations before any statement runs
 * (see is_hoistable_decl). Re-run on every ENTRY, so a decl in a loop body is
 * still re-bound per iteration exactly as it was when the statement did it. */
static void bind_hoistable_decls(const std::vector<unique_ptr<Construct>> &el,
                                 EvalContext *ctx)
{
    for (const auto &e : el)
        if (is_hoistable_decl(e.get()))
            e->eval(ctx);
}

EvalValue Block::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * Scope-free fast path: this block declares only frame slots, so it never
     * uses the EvalContext map. Run its statements directly in the parent
     * context, skipping the per-entry EvalContext construction/destruction.
     * A re-entered block's locals are re-bound by their decls (which re-run
     * each iteration), so no slot clearing is needed. The root block
     * (ctx == nullptr) always takes the full path below, since it owns the
     * program's context and frame.
     */
    if (scope_free && ctx) {

        bind_hoistable_decls(elems, ctx);

        for (const auto &e : elems) {

            if (is_hoistable_decl(e.get()))
                continue;               /* already bound above */

            EvalValue &&tmp = e->eval(ctx);

            if (tmp.is<UndefinedId>())
                throw UndefinedVariableEx(
                    tmp.get<UndefinedId>().id, e->start, e->end);

            if (ctx->flow->type != FlowState::none)
                break;
        }

        return none;
    }

    EvalContext curr(ctx, ctx ? ctx->const_ctx : false);

    /*
     * The root block (ctx == nullptr) is the program's implicit "main": build
     * its Frame here so slotted top-level variables get O(1) slots. curr IS the
     * root context, so unslotted globals still live in curr's map where
     * functions reach them (via get_root_ctx). The Frame lives for the whole
     * program (this do_eval spans it).
     */
    unique_ptr<Frame> root_frame;

    if (!ctx && slot_count) {
        root_frame = make_unique<Frame>();
        root_frame->init(slot_count);
        curr.frame = root_frame.get();
    }

    /*
     * The root block owns the program-wide table of top-level functions (a
     * SymKind::global slot reads it from any call depth). Sized once to the
     * static function count the resolver computed; lives for the whole program.
     */
    unique_ptr<GlobalFuncTable> gtable;

    if (!ctx && !global_func_names.empty()) {
        gtable = make_unique<GlobalFuncTable>();
        gtable->init(global_func_names);
        curr.gfuncs = gtable.get();
    }

    /*
     * No per-block slot reset on entry: a re-entered block (a loop body)
     * re-binds its locals via their decls, which re-run each iteration, and a
     * local resolves to its slot only after its decl - so the stale prior value
     * is never observed.
     */

    bind_hoistable_decls(elems, &curr);

    for (const auto &e: elems) {

        if (is_hoistable_decl(e.get()))
            continue;                   /* already bound above */

        EvalValue &&tmp = e->eval(&curr);

        if (tmp.is<UndefinedId>())
            throw UndefinedVariableEx(tmp.get<UndefinedId>().id, e->start, e->end);

        /*
         * A return/break/continue fired in this statement (possibly nested in
         * ifs): stop running the block and let the signal propagate upward to
         * the enclosing loop or function boundary.
         */
        if (curr.flow->type != FlowState::none)
            break;
    }

    return none;
}

EvalValue WhileStmt::do_eval(EvalContext *ctx, bool rec) const
{
    FlowState &fs = *ctx->flow;

    while (eval_cond(condExpr.get(), ctx)) {

        if (body)
            body->eval(ctx);

        if (fs.type == FlowState::ret)
            break;                              /* propagate to the function */

        if (fs.type == FlowState::brk) {
            fs.type = FlowState::none;
            break;
        }

        if (fs.type == FlowState::cont)
            fs.type = FlowState::none;          /* consume; loop again */
    }

    return none;
}

EvalValue StructDeclStmt::do_eval(EvalContext *ctx, bool rec) const
{
    /* Bind the struct name to its type descriptor (a const t_structtype value
     * holding the AST-owned StructTypeDef*), like a func name. */
    EvalValue desc(def);

    if (id) {

        /* A hoisted struct name binds its descriptor into the global table (an
         * O(1) slot, no map) - like a top-level function. Script-only (the REPL
         * keeps structs in the redefinable map; gfuncs is null there). */
        if (id->sym.kind == SymKind::global && ctx->gfuncs) {
            ctx->gfuncs->define(id->sym.slot,
                                LValue(std::move(desc), true /* const */));
            return none;
        }

        if (!id->eval(ctx).is<UndefinedId>()) {
            if (!ctx->allow_redeclare)        /* REPL: redefining replaces */
                throw AlreadyDefinedEx(id->start, id->end);
            ctx->erase(id.get());
        }

        ctx->emplace(id.get(), std::move(desc), true /* const */);
        return none;
    }

    return desc;
}

EvalValue FuncDeclStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue func(
        intrusive_ptr<FuncObject>(make_intrusive<FuncObject>(desc, ctx))
    );

    if (id) {

        /*
         * A top-level function with a global slot binds directly into the root
         * frame (so calls reach it via an O(1) global-slot read, no map). The
         * resolver already rejected a same-scope duplicate at compile time, so
         * no runtime redecl check is needed here (and the REPL keeps functions
         * in the map, so this path is script-only).
         */
        if (id->sym.kind == SymKind::global && ctx->gfuncs) {
            ctx->gfuncs->define(id->sym.slot,
                                LValue(std::move(func), ctx->const_ctx));
            return none;
        }

        if (!id->eval(ctx).is<UndefinedId>()) {
            /* REPL: re-defining a function replaces it (the edit-and-resubmit
             * workflow); a script rejects the duplicate. */
            if (!ctx->allow_redeclare)
                throw AlreadyDefinedEx(id->start, id->end);
            ctx->erase(id.get());
        }

        /* An explicit `pure func`'s name is a compile-time binding, like a
         * struct's: in the REPL (whose names stay in this map) `a = 3` is a
         * CannotRebindConstEx, as the resolver makes it in a script.
         * Redefining the function is still allowed (erased above). */
        ctx->emplace(
            id.get(),
            std::move(func),
            ctx->const_ctx || desc->explicit_pure
        );

        return none;
    }

    return func;
}

EvalValue Subscript::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * Consume the plain-assignment-target flag here (before evaluating `what`
     * and the index), so a nested base like `d[k1]` in `d[k1][k2] = v` is read
     * normally (throws/defaults on a missing key) while only this outermost
     * subscript may auto-vivify. See EvalContext::assign_target.
     */
    const bool for_write = ctx->assign_target;
    ctx->assign_target = false;

    const EvalValue &lval = what->eval(ctx);

    Type *t = lval.is<LValue *>()
        ? lval.get<LValue *>()->get().get_type()
        : lval.get_type();

    if (t->t == Type::t_undefid) {
        throw UndefinedVariableEx(
            lval.get<UndefinedId>().id, what->start, what->end
        );
    }

    return t->subscript(lval,
                        literal_widen(RValue(index->eval(ctx)), key_coerce),
                        for_write);
}

/*
 * Typed array element read: skip the virtual subscript dispatch and the
 * EvalValue result construction, reading the element's scalar directly. Only
 * for an array base (the inferencer proved array<int>/array<float>); anything
 * else (dict, str, dyn) falls back to the boxed path.
 */
/* The stored value of a present dict key, else nullptr (defined below near
 * MemberExpr). Shared by the typed dict fast paths of Subscript and MemberExpr;
 * a missing key falls back to do_eval. */
const EvalValue *
dict_present_value(const intrusive_ptr<DictObject> &obj, const EvalValue &key);

int_type Subscript::eval_int(EvalContext *ctx) const
{
    const EvalValue &lval = what->eval(ctx);
    const EvalValue &base = lval.is<LValue *>()
        ? lval.get<LValue *>()->get() : lval;

    if (base.is<SharedArrayObj>()) {
        const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
        int_type idx = index->eval_int(ctx);
        ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
        if (idx < 0)
            idx += arr.size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
            throw OutOfBoundsEx(start, end);
        const size_type at = arr.offset() + idx;
        if (arr.skind() == SharedArrayObj::Storage::ints)
            return arr.flat_ints()[at];     /* unboxed: no promotion */
        if (arr.skind() == SharedArrayObj::Storage::bools)
            return arr.flat_bools()[at] ? 1 : 0;   /* bool elem -> int 0/1 */
        return arr.get_vec()[at].getval<int_type>();
    }
    /* typed dict read `d[k]` (present key): look the value up directly instead
     * of Construct::eval_int, which would re-evaluate `what` (the dict). */
    if (base.is<intrusive_ptr<DictObject>>()) {
        const EvalValue key = RValue(index->eval(ctx));
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), key)) {
            if (v->is<bool>())
                return v->get<bool>() ? 1 : 0;
            return v->get<int_type>();
        }
    }
    return Construct::eval_int(ctx);   /* missing key / non-dict: do_eval */
}

float_type Subscript::eval_float(EvalContext *ctx) const
{
    const EvalValue &lval = what->eval(ctx);
    const EvalValue &base = lval.is<LValue *>()
        ? lval.get<LValue *>()->get() : lval;

    if (base.is<SharedArrayObj>()) {
        const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
        int_type idx = index->eval_int(ctx);
        ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
        if (idx < 0)
            idx += arr.size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
            throw OutOfBoundsEx(start, end);
        const size_type at = arr.offset() + idx;
        if (arr.skind() == SharedArrayObj::Storage::floats)
            return arr.flat_floats()[at];   /* unboxed: no promotion */
        if (arr.skind() == SharedArrayObj::Storage::ints)
            return static_cast<float_type>(arr.flat_ints()[at]);
        if (arr.skind() == SharedArrayObj::Storage::bools)
            return arr.flat_bools()[at] ? 1.0 : 0.0;
        const LValue &el = arr.get_vec()[at];
        if (el.is<int_type>())
            return static_cast<float_type>(el.getval<int_type>());
        return el.getval<float_type>();
    }
    if (base.is<intrusive_ptr<DictObject>>()) {
        const EvalValue key = RValue(index->eval(ctx));
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), key)) {
            if (v->is<int_type>())
                return static_cast<float_type>(v->get<int_type>());
            if (v->is<bool>())
                return v->get<bool>() ? 1.0 : 0.0;
            return v->get<float_type>();
        }
    }
    return Construct::eval_float(ctx);   /* missing key / non-dict: do_eval */
}

EvalValue Slice::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &lval = what->eval(ctx);

    Type *t = lval.is<LValue *>()
        ? lval.get<LValue *>()->get().get_type()
        : lval.get_type();

    if (t->t == Type::t_undefid) {
        throw UndefinedVariableEx(
            lval.get<UndefinedId>().id, what->start, what->end
        );
    }

    /* the bounds LEFT TO RIGHT, explicitly: as two arguments of one call
     * their order was unspecified, and a GCC build evaluated the END first
     * - `a[f():g()]` called g before f in the tree-walker alone (RULE 2) */
    const EvalValue start_v = start_idx ? RValue(start_idx->eval(ctx)) : none;
    const EvalValue end_v = end_idx ? RValue(end_idx->eval(ctx)) : none;
    return t->slice(lval, start_v, end_v);
}

static bool
do_catch(EvalContext *ctx,
         RuntimeException *saved_ex,
         IdList *exList,
         Identifier *asId,
         Construct *catchBody)
{
    /* See vm_catch_match: an UNCATCHABLE runtime exception is never handled
     * by a script, and the check must precede the catch-all. */
    if (!saved_ex->is_catchable())
        return false;

    if (!exList) {

        /* Catch-anything block */
        try {
            catchBody->eval(ctx);
        } catch (const RethrowEx &re) {
            saved_ex->loc_start = re.start;
            saved_ex->loc_end = re.end;
            saved_ex->rethrow();
        }

        return true;
    }

    ExceptionObject *exObj = saved_ex->is_exception_object()
        ? static_cast<ExceptionObject *>(saved_ex) : nullptr;
    string_view ex_name = exObj ? exObj->get_name() : saved_ex->name;
    /* interned-pointer matching when available (#74 inc 3); the catch
     * clause ids are interned already, so pointer == string equality */
    const UniqueId *ex_uid = saved_ex->match_uid();

    for (const unique_ptr<Identifier> &id : exList->elems) {

        if (ex_uid ? (id->uid != ex_uid) : (id->get_str() != ex_name))
            continue;

        try {

            EvalContext catch_ctx(ctx);

            if (asId) {

                /*
                 * Bind the catch variable. A thrown struct carries the
                 * instance as its payload, so `e` IS the struct (and `e.field`
                 * works). A payload-less built-in exception binds to a small
                 * exception object instead (printable, re-throwable).
                 */
                EvalValue bind_val =
                    (exObj && exObj->get_data()
                                  .is<intrusive_ptr<StructObject>>())
                        ? exObj->get_data()
                        : EvalValue(make_intrusive<ExceptionObject>(
                              exObj
                                  ? *exObj
                                  : ExceptionObject(saved_ex->name)));

                if (asId->sym.kind == SymKind::local && catch_ctx.frame) {

                    /* Resolved catch variable: bind it into its slot. */
                    Frame *f = catch_ctx.frame;
                    f->slots[asId->sym.slot] =
                        LValue(std::move(bind_val), ctx->const_ctx);

                } else {

                    catch_ctx.emplace(
                        asId,
                        std::move(bind_val),
                        ctx->const_ctx
                    );
                }
            }

            catchBody->eval(&catch_ctx);

        } catch (const RethrowEx &re) {
            saved_ex->loc_start = re.start;
            saved_ex->loc_end = re.end;
            saved_ex->rethrow();
        }

        return true;
    }

    return false;
}

EvalValue TryCatchStmt::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * Run the try body + catch clauses. Returns normally (body completed, or a
     * catch handled the exception - possibly leaving a return/break/continue in
     * ctx->flow), or THROWS (an unhandled exception re-raised, or a NEW
     * exception a catch body / `rethrow` threw). A lambda so `finally` can run
     * it directly (no finally) or wrapped in a try (finally present).
     *
     * `finally` runs EXPLICITLY here, NOT in a scope-guard destructor: a
     * destructor is implicitly noexcept, so a throw from the finally body used
     * to hit std::terminate. Running it explicitly lets a throwing finally
     * propagate (superseding any pending exception/flow), matching Python/C#/
     * Java - and the VM.
     */
    auto run_try_catches = [&]() {

        unique_ptr<RuntimeException> saved_ex;

        try {
            tryBody->eval(ctx);
        } catch (const RuntimeException &e) {
            saved_ex.reset(e.clone());
        }

        if (!saved_ex)
            return;

        for (const auto &p : catchStmts) {
            if (do_catch(ctx, saved_ex.get(), p.first.exList.get(),
                         p.first.asId.get(), p.second.get()))
                return;                    /* handled (do_catch may itself throw) */
        }

        saved_ex->rethrow();               /* no clause matched -> re-raise */
    };

    if (!finallyBody) {
        run_try_catches();
        return none;
    }

    /*
     * With a `finally`: capture any exception AND any return/break/continue the
     * try/catch left, run finally, then resume it - unless finally throws or
     * raises its own flow signal, which SUPERSEDES the pending one.
     */
    unique_ptr<RuntimeException> pending;

    try {
        run_try_catches();
    } catch (const RuntimeException &e) {
        pending.reset(e.clone());
    }

    FlowState &fs = *ctx->flow;
    const FlowState::Type saved_type = fs.type;
    EvalValue saved_val = std::move(fs.value);
    fs.type = FlowState::none;

    finallyBody->eval(ctx);                /* may throw / set its own flow */

    if (fs.type == FlowState::none) {
        fs.type = saved_type;              /* resume the suspended signal */
        fs.value = std::move(saved_val);
    } else {
        pending.reset();                   /* finally's own flow supersedes */
    }

    if (pending)
        pending->rethrow();

    return none; /* Make compilers unaware of [[noreturn]] happy */
}

LValue LValue::clone()
{
    LValue nl(valtype()->clone(val), is_const);
    nl.container = container;
    nl.container_idx = container_idx;
    return nl;
}

LValue *LValue::write_target()
{
    if (!container)
        return this;

    LValue *const cont = container;
    assert(cont->is<SharedArrayObj>());

    if (cont->valtype()->is_slice(cont->val)) {
        /*
         * `this` lives in the slice's storage, and making the slice
         * standalone RELEASES that storage - FREES it, when the slice was
         * its only owner (a slice returned from a function whose array
         * died with it). So everything is read from `this` first, and
         * nothing after: until 2026-10-08 the index was adjusted on
         * `this` and the type check read `this->val`, both after the free
         * (an ASan heap-use-after-free in both engines).
         */
        const size_type idx =
            container_idx - cont->getval<SharedArrayObj>().offset();
        *cont = cont->clone();
        LValue *el = &cont->getval<SharedArrayObj>().get_vec()[idx];
        el->container = cont;
        el->container_idx = idx;
        return el;
    }

    if (cont->valtype()->use_count(cont->val) > 1)
        cont->getval<SharedArrayObj>().clone_aliased_slices(container_idx);

    /* an in-place element write changes the array's hash (the slice path above
     * returns a fresh clone, which is already hash-invalid). */
    cont->getval<SharedArrayObj>().invalidate_hash();
    return this;
}

void LValue::put_slow(const EvalValue &v)
{
    LValue *t = write_target();    /* `this` may be freed (write_target) */
    t->val = v;
    t->type_checks();
}

void LValue::put_slow(EvalValue &&v)
{
    LValue *t = write_target();
    t->val = std::move(v);
    t->type_checks();
}

/*
 * Bind a foreach loop variable to `val`. Fast path: a resolved-local loop var
 * is a plain frame slot, so write it directly (mark live), skipping the general
 * lvalue->eval() + doAssign() machinery handle_single_expr14 would run on every
 * iteration. Correct for every iteration (not just the decl): the loop var is a
 * fresh `var` each pass, so overwriting the slot is exactly the semantics. The
 * loop var elements (`ids->elems`) are Identifiers, so no dynamic_cast is
 * needed. Non-resolved (map-based) loop vars fall back to the general path.
 */
static inline void
bind_loop_var(EvalContext *ctx, bool decl, Identifier *id, const EvalValue &val)
{
    if (id->is_underscore())   /* the `_` placeholder binds nothing */
        return;

    if (id->sym.kind == SymKind::local && ctx->frame) {
        Frame *f = ctx->frame;
        f->slots[id->sym.slot] = LValue(val, id->is_const);
        return;
    }

    handle_single_expr14(ctx, decl, Op::assign, id, val);
}

bool
ForeachStmt::do_iter(EvalContext *ctx,
                     size_type index,
                     const EvalValue *elems,
                     size_type count) const
{
    const bool decl = index == 0 ? idsVarDecl : false;
    size_type id_start = 0;

    if (indexed) {

        bind_loop_var(
            ctx, decl, ids->elems[0].get(), static_cast<int_type>(index)
        );

        id_start++;
    }

    if (count == 1) {

        const size_type nvars = ids->elems.size() - id_start;

        if (nvars > 1) {

            /*
             * Array DESTRUCTURING: the element must be an array of EXACTLY
             * `nvars` (STRICT, like Python). A non-array element or a
             * length mismatch is an error - the old lenient behavior (pad the
             * missing with none, silently drop extras, treat a scalar as the
             * first value) is gone; it hid bugs and could not be nativized to
             * plain scalar reads. Well-formed data is unaffected.
             */
            if (!elems[0].is<SharedArrayObj>())
                throw TypeErrorEx(
                    intern_msg("foreach: cannot unpack a non-array element "
                               "into " + std::to_string(nvars) + " variables"),
                    container->start, container->end);

            const SharedArrayObj &arr = elems[0].get<SharedArrayObj>();

            if (arr.size() != nvars)
                throw TypeErrorEx(
                    intern_msg("foreach: cannot unpack an array of length " +
                               std::to_string(arr.size()) + " into " +
                               std::to_string(nvars) + " variables"),
                    container->start, container->end);

            for (size_type i = id_start; i < ids->elems.size(); i++)
                bind_loop_var(ctx, decl, ids->elems[i].get(),
                              arr_elem_boxed(arr, i - id_start));

        } else {

            /* A single loop var (or the value slot of an `indexed` loop).
             * A 1-var `indexed` loop has NO value var (ids[0] IS the index,
             * bound above) - bind nothing, consistent with the dict path's
             * empty bind loop for the same shape. (Was an out-of-bounds
             * `ids->elems[1]` read -> an abort under container hardening /
             * UB in a plain release.) */
            if (id_start < ids->elems.size())
                bind_loop_var(ctx, decl, ids->elems[id_start].get(),
                              elems[0]);
        }

    } else {

        for (size_type i = id_start; i < ids->elems.size(); i++) {

            const size_type val_i = i - id_start;

            bind_loop_var(
                ctx,
                decl,
                ids->elems[i].get(),
                val_i < count ? elems[val_i] : none
            );
        }
    }

    if (body)
        body->eval(ctx);

    FlowState &fs = *ctx->flow;

    if (fs.type == FlowState::ret)
        return false;                       /* stop; propagate up */

    if (fs.type == FlowState::brk) {
        fs.type = FlowState::none;
        return false;                       /* stop iterating */
    }

    if (fs.type == FlowState::cont)
        fs.type = FlowState::none;          /* consume; advance to next item */

    return true;
}

EvalValue
ForeachStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext loopCtx(ctx, ctx->const_ctx);
    const EvalValue &cval = RValue(container->eval(ctx));

    if (cval.is<SharedArrayObj>()) {

        const SharedArrayObj &arr = cval.get<SharedArrayObj>();

        /*
         * #53: the body may MUTATE the array it walks (append/pop/erase, an
         * element store, a promotion of flat strs/struct storage to general),
         * so nothing derived from its storage - a vector reference, a view, a
         * size - survives an iteration. The DEFINED semantics, shared by
         * every engine: the loop runs over the length the array had when it
         * STARTED (an appended element is not visited); each element is read
         * when its turn comes (a store the body made to a later element is
         * seen); and if the body MOVED elements - pop, erase, an insert
         * not at the end, through any alias - the loop raises
         * OutOfBoundsEx at the container at its next step (option 1: the
         * storage's shift epoch, recorded here; a slice view never shifts,
         * so fe_mark answers -1 for one and nothing is checked). Without
         * it, removing an element already visited silently SKIPPED the
         * next one. The length test stays as the memory-safety net.
         *
         * So the storage is re-derived per element (`arr` shares it with the
         * program's variables - the handle is the loop's pin). The flat
         * scalar kinds cannot change kind (no promotion), the rest dispatch
         * per element.
         */
        const size_type n = arr.size();
        const int_type mark = arr.fe_mark();
        intrusive_ptr<StructObject> reuse;

        for (size_type i = 0; i < n; i++) {

            if (arr.fe_shifted(mark) || i >= arr.size())
                throw OutOfBoundsEx(container->start, container->end);

            const size_type at = arr.offset() + i;
            EvalValue elem;

            switch (arr.skind()) {
            case SharedArrayObj::Storage::ints:
                elem = EvalValue(arr.flat_ints()[at]);
                break;
            case SharedArrayObj::Storage::floats:
                elem = EvalValue(arr.flat_floats()[at]);
                break;
            case SharedArrayObj::Storage::bools:
                elem = EvalValue(static_cast<bool>(arr.flat_bools()[at]));
                break;
            case SharedArrayObj::Storage::structs: {
                /*
                 * Flat POD-struct array: rather than heap-allocate a
                 * StructObject per element, reuse ONE across iterations -
                 * overwrite its bytes in place and hand it to the body. COW
                 * guard: if the previous body captured the element (so
                 * something other than the loop var still holds it:
                 * use_count() > 2 == this local + the loop var's slot + a
                 * capture), allocate a fresh one so the capture keeps its
                 * value.
                 */
                const auto &sv = arr.flat_structs();
                if (!reuse || reuse.use_count() > 2 || reuse->def != sv.def)
                    reuse = intrusive_ptr<StructObject>(
                        make_intrusive<StructObject>(sv.def));
                std::memcpy(reuse->bytes.data(),
                            sv.buf.data() + at * sv.stride, sv.stride);
                elem = EvalValue(reuse);
                break;
            }
            default:
                /* general / strs (a handle copy, not a byte copy) */
                elem = arr_elem_boxed(arr, i);
                break;
            }

            if (!do_iter(&loopCtx, i, &elem, 1))
                break;
        }

    } else if (cval.is<SharedStr>()) {

        const string_view view = cval.get<SharedStr>().get_view();

        for (size_type i = 0; i < view.size(); i++) {

            const EvalValue &elem = SharedStr(string(&view[i], 1));

            if (!do_iter(&loopCtx, i, &elem, 1))
                break;
        }

    } else if (cval.is<intrusive_ptr<DictObject>>()) {

        /* #53: through a Cursor, so a body that inserts or erases keys of
         * this dict is defined (DictObject::Cursor states the semantics).
         * `cval` pins the dict for the loop. */
        DictObject &dobj = *cval.get_ref<intrusive_ptr<DictObject>>();
        DictObject::Cursor cur;
        cur.start(dobj);

        size_type i = 0;

        while (const auto *p = cur.next_entry(dobj)) {

            const EvalValue elems[2] = { p->first, p->second.get() };

            if (!do_iter(&loopCtx, i, elems, 2))
                break;

            i++;
        }

    } else {

        throw TypeErrorEx(
            "Unsupported container type by foreach()",
            container->start,
            container->end
        );
    }

    return none;
}

/*
 * Build a dict VALUE from `npairs` already-evaluated key/value pairs, stored
 * INTERLEAVED in `pairs` ([k0, v0, k1, v1, ...]). Shared by the tree-walker
 * (LiteralDict::do_eval) and the VM's MakeDictV op. Each key is FROZEN
 * (make_const_clone) so a mutable container key can't later be mutated and
 * corrupt the dict (see TypeDict::subscript) - which is why this helper lives
 * in eval.cpp, and the VM only ever passes it values.
 */
EvalValue build_dict_from_pairs(const EvalValue *pairs, size_t npairs,
                                bool is_const)
{
    DictObject::inner_type data;
    data.reserve(npairs);   /* profile #5: kill the insert rehash chain */
    for (size_t i = 0; i < npairs; i++)
        data.emplace(make_const_clone(pairs[2 * i]),
                     LValue(pairs[2 * i + 1], is_const));
    return intrusive_ptr<DictObject>(make_intrusive<DictObject>(std::move(data)));
}

EvalValue LiteralDict::do_eval(EvalContext *ctx, bool rec) const
{
    /* Evaluate the key/value nodes into an interleaved buffer (stack for the
     * common small literal, heap for a large one), then the shared builder -
     * the same builder the VM's MakeDictV op calls with register values. */
    const size_t np = elems.size();
    /* the VALUES widen as LiteralArray's elements do: by the stamp, or -
     * a fully CONSTANT literal - by their own join */
    const auto widen_vals = [&](EvalValue *buf) {
        if (key_coerce != DeclType::none) {
            for (size_t i = 0; i < np; i++)
                buf[2 * i] = literal_widen(std::move(buf[2 * i]), key_coerce);
        } else if (is_const && ctx->in_const_eval()) {
            const_values_widen(buf, np, 2, ctx->const_ctx);
        }
        if (val_coerce != DeclType::none) {
            for (size_t i = 0; i < np; i++)
                buf[2 * i + 1] =
                    literal_widen(std::move(buf[2 * i + 1]), val_coerce);
        } else if (is_const && ctx->in_const_eval()) {
            const_values_widen(buf + 1, np, 2, ctx->const_ctx);
        }
    };

    if (np <= 8) {
        EvalValue buf[16];
        for (size_t i = 0; i < np; i++) {
            buf[2 * i]     = RValue(elems[i]->key->eval(ctx));
            buf[2 * i + 1] = RValue(elems[i]->value->eval(ctx));
        }
        widen_vals(buf);
        return build_dict_from_pairs(buf, np, ctx->const_ctx);
    }

    std::vector<EvalValue> buf(2 * np);
    for (size_t i = 0; i < np; i++) {
        buf[2 * i]     = RValue(elems[i]->key->eval(ctx));
        buf[2 * i + 1] = RValue(elems[i]->value->eval(ctx));
    }
    widen_vals(buf.data());
    return build_dict_from_pairs(buf.data(), np, ctx->const_ctx);
}

/* True if `c` is rooted at a variable (an identifier, or a member/subscript
 * chain ending at one), so an lvalue derived from it outlives this evaluation;
 * a temporary (a call/literal result) is not. */
static bool is_lvalue_rooted(const Construct *c)
{
    if (c->is_id())
        return true;
    if (auto *m = dynamic_cast<const MemberExpr *>(c))
        return is_lvalue_rooted(m->what.get());
    if (auto *s = dynamic_cast<const Subscript *>(c))
        return is_lvalue_rooted(s->what.get());
    return false;
}

/*
 * The VALUE-read path of a member access `base.member` - a struct field / POD
 * field / struct const / struct-type const / dict key / optional-`?.` none.
 * Shared by MemberExpr::do_eval and the boxed VM's MemberV op. ALWAYS returns a
 * value (never an assignable LValue*); the lvalue / auto-vivify WRITE paths
 * stay in do_eval (they need `what` for rooting and the for_write flag). An
 * error carries the MemberExpr's loc.
 */
/*
 * The value-read of `base.member`, factored to take the member's fields
 * DIRECTLY (name-as-key, interned uid, optional flag, and the two carets it
 * throws with) instead of a MemberExpr*. So the VM's MemberV can call it from a
 * member-key pool entry with no AST node, while the tree-walker's wrapper below
 * still passes a MemberExpr's fields - one shared implementation.
 */
EvalValue member_read_core(const EvalValue &dval, const EvalValue &memId,
                           const UniqueId *memUid, bool optional,
                           const Loc &mstart, const Loc &mend,
                           const Loc &bstart, const Loc &bend)
{
    if (optional && dval.is<NoneVal>())
        return EvalValue();

    if (dval.is<intrusive_ptr<StructObject>>()) {

        const auto &obj = dval.get_ref<intrusive_ptr<StructObject>>();
        const int slot = obj->def->slot_of(memUid);

        if (slot >= 0)
            return obj->is_pod() ? obj->pod_get(slot)
                                 : obj->fields[slot].get();

        if (const EvalValue *cv = obj->def->const_of(memUid))
            return *cv;

        throw TypeErrorEx(
            intern_msg("Struct '" + string(obj->def->name->val) +
                       "' has no member '" + string(memUid->val) + "'"),
            mstart, mend);
    }

    /* A struct TYPE descriptor: only its `const` members (no instance). */
    if (dval.is<StructTypeDef *>()) {

        StructTypeDef *def = dval.get<StructTypeDef *>();

        if (const EvalValue *cv = def->const_of(memUid))
            return *cv;

        if (def->slot_of(memUid) >= 0)
            throw TypeErrorEx(
                intern_msg("Field '" + string(memUid->val) +
                           "' needs an instance"), mstart, mend);

        throw TypeErrorEx(
            intern_msg("Struct '" + string(def->name->val) +
                       "' has no member '" + string(memUid->val) + "'"),
            mstart, mend);
    }

    if (!dval.is<intrusive_ptr<DictObject>>())
        throw TypeErrorEx("Expected dict object", bstart, bend);

    const auto &obj = dval.get<intrusive_ptr<DictObject>>();
    const DictObject::inner_type &data = obj->get_ref();
    const auto &it = data.find(memId);

    if (it != data.end())            /* present key -> the value */
        return it->second.get();
    if (obj->get_has_default())      /* a default dict -> its default */
        return obj->get_default();
    throw KeyNotFoundEx(mstart, mend);   /* missing -> throw */
}

EvalValue member_read(const EvalValue &dval, const MemberExpr *m)
{
    return member_read_core(dval, m->memId, m->memUid, m->optional,
                            m->start, m->end, m->what->start, m->what->end);
}

EvalValue MemberExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /* Consume the plain-assignment-target flag (see Subscript::do_eval). */
    const bool for_write = ctx->assign_target;
    ctx->assign_target = false;

    return access(RValue(what->eval(ctx)), for_write,
                  is_lvalue_rooted(what.get()));
}

EvalValue MemberExpr::access(const EvalValue &dval, bool for_write,
                             bool rooted) const
{
    /*
     * The assignable-lvalue / auto-vivify paths need `what` (rooting) or the
     * for_write flag; every other case is a VALUE read via member_read (shared
     * with the boxed VM's MemberV op).
     */
    if (dval.is<intrusive_ptr<StructObject>>()) {

        const auto &obj = dval.get_ref<intrusive_ptr<StructObject>>();
        /* the compile-baked field slot (def-identity-checked): no name scan */
        const int slot = obj->def == base_struct_def && field_slot >= 0
            ? field_slot : obj->def->slot_of(memUid);

        /*
         * A rooted, mutable, boxed field -> an assignable field lvalue (so
         * `s.f = v` / `s.f += v` work). A POD field (bytes, no per-field
         * LValue), a read-only instance, or a temporary base (`Point(1,2).x`
         * read, not stored: a store HOLDS its base) is a value read -
         * member_read handles those.
         */
        if (slot >= 0 && !obj->is_pod() && !obj->is_readonly() && rooted)
            return &obj->fields[slot];

    } else if (dval.is<intrusive_ptr<DictObject>>()) {

        const auto &obj = dval.get<intrusive_ptr<DictObject>>();
        const DictObject::mut_iterator it = obj->find_mut(memId);
        const bool present = it != obj->mut_end();

        /*
         * `d.key` mirrors `d[key]` (TypeDict::subscript): present -> the value
         * (an lvalue when mutable, so `d.k = v` / `d.k += v` work); missing ->
         * the default (default dict), else auto-vivify on a plain-assignment
         * target, else throw. A readonly dict's read (present / default /
         * missing) is member_read's job.
         */
        if (!obj->is_readonly()) {
            if (present)
                return &it->second;
            if (obj->get_has_default())      /* a new key (Cursor) */
                return obj->insert_new(EvalValue(memId),
                                       LValue(obj->get_default(), false));
            if (for_write)
                return obj->insert_new(EvalValue(memId),
                                       LValue(none, false));
            throw KeyNotFoundEx(start, end);
        }
        /* A readonly dict + a for_write missing-key (no default) -> `none`, so
         * the ensuing write fails NotLValueEx (not a KeyNotFound read). */
        if (for_write && !present && !obj->get_has_default())
            return none;
    }

    return member_read(dval, this);
}

/*
 * Typed (unboxed) reads of a POD struct field (the M8 fast path, plans/
 * structs.md phase 8): the inferencer stamps `th` on `s.x` when the field is a
 * non-null int/float, and the specializer / compound-assign fast path then call
 * these instead of do_eval() -> pod_get() -> a boxed EvalValue.
 *
 * Two cases: `a[i].field` on a flat POD-struct array reads the scalar STRAIGHT
 * from the array bytes, with NO per-element StructObject materialized at all
 * (guarded by no_side_effects so the array base is evaluated once); any other
 * base falls back to evaluating it to a StructObject and reading its bytes.
 */
template <class T>
static bool member_pod_array_scalar(const Subscript *sub, EvalContext *ctx,
                                    const UniqueId *memUid, Loc s, Loc e,
                                    T &out)
{
    if (!no_side_effects(sub->what.get()))
        return false;

    const EvalValue av = sub->what->eval(ctx);
    const EvalValue &arrv = av.is<LValue *>() ? av.get<LValue *>()->get() : av;
    if (!arrv.is<SharedArrayObj>())
        return false;

    const SharedArrayObj &arr = arrv.get_ref<SharedArrayObj>();
    if (arr.skind() != SharedArrayObj::Storage::structs)
        return false;

    const auto &sv = arr.flat_structs();
    const FieldDef *f = sv.def->field_of(memUid);
    if (!f || f->offset < 0)
        return false;

    int_type idx = sub->index->eval_int(ctx);
    ML_INT_ONLY(int_vc_index(false, idx, arr.size());)
    if (idx < 0)
        idx += arr.size();
    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx(s, e);

    const char *p =
        sv.buf.data() + (arr.offset() + idx) * sv.stride + f->offset;

    switch (f->kind) {
        case FieldKind::f_int: {
            int_type v;
            std::memcpy(&v, p, sizeof v);
            out = static_cast<T>(v);
            return true;
        }
        case FieldKind::f_float: {
            float_type v;
            std::memcpy(&v, p, sizeof v);
            out = static_cast<T>(v);
            return true;
        }
        case FieldKind::f_bool:
            out = static_cast<T>(static_cast<unsigned char>(*p) != 0 ? 1 : 0);
            return true;
        default:
            return false;
    }
}

/*
 * The stored value of a PRESENT dict key, or nullptr if absent. Returns a
 * pointer into the live dict (the caller's `base` keeps it alive). The typed
 * fast paths (Subscript/MemberExpr eval_int/eval_float) use this for the common
 * present-key case so a typed `d.key` / `d[k]` reads the value WITHOUT
 * re-evaluating the base (the old code fell through to Construct::eval_int,
 * which re-ran do_eval and re-fetched the dict). A missing key falls back to
 * do_eval, preserving the exact default-dict vivify / key-freeze / KeyNotFound
 * behavior unchanged.
 */
const EvalValue *
dict_present_value(const intrusive_ptr<DictObject> &obj, const EvalValue &key)
{
    const DictObject::inner_type &data = obj->get_ref();
    const auto it = data.find(key);
    return it != data.end() ? &it->second.get() : nullptr;
}

int_type MemberExpr::eval_int(EvalContext *ctx) const
{
    if (auto *sub = dynamic_cast<const Subscript *>(what.get())) {
        int_type v;
        /* #125: the OOB caret is the SUBSCRIPT's span, not this
         * MemberExpr's - the fault is the INDEX, not the field, and it is
         * what every other element read reports (and what the VM has always
         * reported for this shape). Passing `start, end` here spanned
         * `row[i].y` and made the two engines disagree; nothing caught it,
         * because the differential compares exception TYPES, not columns. */
        if (member_pod_array_scalar(sub, ctx, memUid,
                                    sub->start, sub->end, v))
            return v;
    }

    const EvalValue base = RValue(what->eval(ctx));
    if (base.is<intrusive_ptr<StructObject>>()) {
        const StructObject &o =
            *base.get_ref<intrusive_ptr<StructObject>>().get();
        /* the compile-baked field slot (def-identity-checked): no name scan */
        const int slot = o.def == base_struct_def && field_slot >= 0
            ? field_slot : o.def->slot_of(memUid);
        if (slot >= 0 && o.is_pod()) {
            const FieldDef &f = o.def->fields[slot];
            const char *p = o.bytes.data() + f.offset;
            if (f.kind == FieldKind::f_int) {
                int_type v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            if (f.kind == FieldKind::f_bool)
                return static_cast<unsigned char>(*p) != 0 ? 1 : 0;
        }
    }
    if (base.is<intrusive_ptr<DictObject>>()) {
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), memId)) {
            if (v->is<bool>())
                return v->get<bool>() ? 1 : 0;
            return v->get<int_type>();
        }
    }
    return Construct::eval_int(ctx);   /* missing key / non-dict: do_eval */
}

float_type MemberExpr::eval_float(EvalContext *ctx) const
{
    if (auto *sub = dynamic_cast<const Subscript *>(what.get())) {
        float_type v;
        /* #125: the OOB caret is the SUBSCRIPT's span, not this
         * MemberExpr's - the fault is the INDEX, not the field, and it is
         * what every other element read reports (and what the VM has always
         * reported for this shape). Passing `start, end` here spanned
         * `row[i].y` and made the two engines disagree; nothing caught it,
         * because the differential compares exception TYPES, not columns. */
        if (member_pod_array_scalar(sub, ctx, memUid,
                                    sub->start, sub->end, v))
            return v;
    }

    const EvalValue base = RValue(what->eval(ctx));
    if (base.is<intrusive_ptr<StructObject>>()) {
        const StructObject &o =
            *base.get_ref<intrusive_ptr<StructObject>>().get();
        /* the compile-baked field slot (def-identity-checked): no name scan */
        const int slot = o.def == base_struct_def && field_slot >= 0
            ? field_slot : o.def->slot_of(memUid);
        if (slot >= 0 && o.is_pod()) {
            const FieldDef &f = o.def->fields[slot];
            const char *p = o.bytes.data() + f.offset;
            if (f.kind == FieldKind::f_float) {
                float_type v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            if (f.kind == FieldKind::f_int) {
                int_type v;
                std::memcpy(&v, p, sizeof v);
                return static_cast<float_type>(v);
            }
            if (f.kind == FieldKind::f_bool)
                return static_cast<unsigned char>(*p) != 0 ? 1.0 : 0.0;
        }
    }
    if (base.is<intrusive_ptr<DictObject>>()) {
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), memId)) {
            if (v->is<int_type>())
                return static_cast<float_type>(v->get<int_type>());
            if (v->is<bool>())
                return v->get<bool>() ? 1.0 : 0.0;
            return v->get<float_type>();
        }
    }
    return Construct::eval_float(ctx);   /* missing key / non-dict: do_eval */
}

EvalValue ForStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext loop_ctx(ctx, ctx->const_ctx);

    if (init)
        init->eval(&loop_ctx);

    FlowState &fs = *loop_ctx.flow;

    while (true) {

        if (cond && !eval_cond(cond.get(), &loop_ctx))
            break;

        if (body)
            body->eval(&loop_ctx);

        if (fs.type == FlowState::ret)
            break;                              /* propagate to the function */

        if (fs.type == FlowState::brk) {
            fs.type = FlowState::none;
            break;
        }

        if (fs.type == FlowState::cont)
            fs.type = FlowState::none;          /* consume; still run `inc` */

        if (inc)
            inc->eval(&loop_ctx);
    }

    return none;
}

/*
 * Specialized counted loop (see syntax.h). After `init` declares the int slot,
 * `bound` and `step` are evaluated ONCE; the per-iteration condition test and
 * increment are then plain C on the slot's int_type - no expression eval, no
 * num_bin_op, no TypedScalarExpr dispatch. The slot ref is re-fetched each
 * iteration so a body that reassigns `i` is still respected.
 */
EvalValue ForRangeStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext loop_ctx(ctx, ctx->const_ctx);

    init->eval(&loop_ctx);                 /* declares i in frame slot i_slot */

    Frame *f = loop_ctx.frame;
    ML_CHECK(f);              /* i is a resolved slot -> the frame must exist */

    const int_type bound_val = RValue(bound->eval(&loop_ctx)).get<int_type>();
    const int_type step_val =
        step ? RValue(step->eval(&loop_ctx)).get<int_type>()
             : static_cast<int_type>(1);
    /* lt/le ascend (+step), ge/gt descend (-step). */
    const bool asc = (cmp_op == Op::lt || cmp_op == Op::le);
    const int_type delta = asc ? step_val : -step_val;

    FlowState &fs = *loop_ctx.flow;

    while (true) {

        const int_type iv = f->slots[i_slot].getval<int_type>();
        bool go;
        switch (cmp_op) {
            case Op::lt: go = iv <  bound_val; break;
            case Op::le: go = iv <= bound_val; break;
            case Op::ge: go = iv >= bound_val; break;
            default:     go = iv >  bound_val; break;   /* Op::gt */
        }
        if (!go)
            break;

        if (body)
            body->eval(&loop_ctx);

        if (fs.type == FlowState::ret)
            break;                          /* propagate to the function */
        if (fs.type == FlowState::brk) {
            fs.type = FlowState::none;
            break;
        }
        if (fs.type == FlowState::cont)
            fs.type = FlowState::none;      /* consume; still run the step */

        f->slots[i_slot].getval<int_type>() += delta;
    }

    return none;
}

/* ===================================================================== *
 *  Inlining cost-model calibration  (`--weights`)
 *
 *  Measures the per-node-type eval cost of the tree-walker by evaluating
 *  HAND-BUILT AST nodes (constructed here in C++, never parsed) in a tight
 *  C++ loop - so no optimizer pass (fold / inline / specialize) can perturb
 *  the measured nodes or the iteration count. Per-node "weights" are isolated
 *  by subtracting child-subtree costs, then dumped relative to a slot read.
 *  The CALL machinery is the reference: the inliner's benefit function inlines
 *  a body unconditionally when the sum of its node weights is below the call
 *  weight. Re-run whenever the interpreter changes (and, later, for the
 *  bytecode VM - the weights change, the benefit function does not).
 * ===================================================================== */

static volatile int_type g_wb_sink = 0;

namespace {

using wb_clock = std::chrono::steady_clock;

static unique_ptr<Identifier> wb_id(int slot)
{
    auto id = make_unique<Identifier>("x");
    id->sym = ResolvedSym{ SymKind::local, slot };
    id->th = TypeHint::i;
    return id;
}

static unique_ptr<TypedScalarExpr>
wb_bin(TypedScalarExpr::Cat cat, Op op,
       unique_ptr<Construct> a, unique_ptr<Construct> b)
{
    auto e = make_unique<TypedScalarExpr>(cat, TypeHint::i);
    e->th = TypeHint::i;
    e->elems.emplace_back(Op::invalid, std::move(a));
    e->elems.emplace_back(op, std::move(b));
    return e;
}

static unique_ptr<Construct>
wb_assign(int dst, unique_ptr<Construct> rhs)
{
    auto a = make_unique<Expr14>();
    a->op = Op::assign;
    a->lvalue = wb_id(dst);
    a->rvalue = std::move(rhs);
    return a;
}

/* ns/eval via eval_int() (the M8 fast path, used for arithmetic operands). */
static double wb_time_int(const Construct *n, EvalContext *ctx, long M)
{
    double best = 1e18;
    for (int r = 0; r < 5; r++) {
        auto t0 = wb_clock::now();
        int_type acc = 0;
        for (long i = 0; i < M; i++)
            acc += n->eval_int(ctx);
        g_wb_sink += acc;
        auto t1 = wb_clock::now();
        double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / M;
        if (ns < best) best = ns;
    }
    return best;
}

/* ns/eval via eval() (the statement path, with the Construct::eval wrapper).
 * Flow is reset each iteration so a return/if measures a clean dispatch. */
static double wb_time_eval(const Construct *n, EvalContext *ctx, long M)
{
    double best = 1e18;
    for (int r = 0; r < 5; r++) {
        auto t0 = wb_clock::now();
        for (long i = 0; i < M; i++) {
            ctx->flow->type = FlowState::none;
            n->eval(ctx);
        }
        ctx->flow->type = FlowState::none;
        g_wb_sink += ctx->frame->at(2).getval<int_type>();
        auto t1 = wb_clock::now();
        double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / M;
        if (ns < best) best = ns;
    }
    return best;
}

} // namespace

void run_weight_bench()
{
    const long M = 20000000;

    /* A clean func-context root with an 8-slot int frame (slots pre-bound). */
    EvalContext ctx(nullptr, false, true);
    Frame frame;
    frame.init(8);
    ctx.frame = &frame;
    for (int i = 0; i < 8; i++)
        frame.slots[i] = LValue(EvalValue(static_cast<int_type>(i + 2)), false);

    /* leaves */
    auto idn = wb_id(0);
    double t_id  = wb_time_int(idn.get(), &ctx, M);
    auto litn = make_unique<LiteralInt>(7);
    double t_lit = wb_time_int(litn.get(), &ctx, M);

    /* arith / compare (each reads two operands) */
    auto addn = wb_bin(TypedScalarExpr::Cat::arith, Op::plus, wb_id(0), wb_id(1));
    double t_add = wb_time_int(addn.get(), &ctx, M);
    auto cmpn = wb_bin(TypedScalarExpr::Cat::cmp, Op::lt, wb_id(0), wb_id(1));
    double t_cmp = wb_time_int(cmpn.get(), &ctx, M);

    /* assignment `r = a + b` (statement path) */
    auto asn = wb_assign(2,
        wb_bin(TypedScalarExpr::Cat::arith, Op::plus, wb_id(0), wb_id(1)));
    double t_asn = wb_time_eval(asn.get(), &ctx, M);

    /* return `return a` */
    auto retn = make_unique<ReturnStmt>();
    retn->elem = wb_id(0);
    double t_ret = wb_time_eval(retn.get(), &ctx, M);

    /* if `if (a < b) r = a + b;` */
    auto iff = make_unique<IfStmt>();
    iff->condExpr = wb_bin(TypedScalarExpr::Cat::cmp, Op::lt, wb_id(0), wb_id(1));
    {
        auto thn = make_unique<Block>();
        thn->elems.push_back(wb_assign(2,
            wb_bin(TypedScalarExpr::Cat::arith, Op::plus, wb_id(0), wb_id(1))));
        iff->thenBlock = std::move(thn);
    }
    double t_if = wb_time_eval(iff.get(), &ctx, M);

    /* CALL `func bench(a, b) { return a + b; }` invoked f(s0, s1) */
    auto fd = make_unique<FuncDeclStmt>();
    fd->id = make_unique<Identifier>("bench");
    fd->params = make_unique<IdList>();
    fd->params->elems.push_back(make_unique<Identifier>("a"));
    fd->params->elems.push_back(make_unique<Identifier>("b"));
    {
        auto body = make_unique<Block>();
        auto r = make_unique<ReturnStmt>();
        r->elem = wb_bin(TypedScalarExpr::Cat::arith, Op::plus,
                         wb_id(0), wb_id(1));
        body->elems.push_back(std::move(r));
        fd->body = std::move(body);
    }
    fd->sync_params();
    fd->desc->resolved = true;
    fd->desc->frame_size = 2;
    FuncObject fobj(fd->desc, &ctx);
    std::vector<unique_ptr<Construct>> cargs;
    cargs.push_back(wb_id(0));
    cargs.push_back(wb_id(1));
    double t_call = 1e18;
    for (int r = 0; r < 5; r++) {
        auto t0 = wb_clock::now();
        int_type acc = 0;
        for (long i = 0; i < M; i++)
            acc += do_func_call(&ctx, fobj, cargs).get<int_type>();
        g_wb_sink += acc;
        auto t1 = wb_clock::now();
        double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / M;
        if (ns < t_call) t_call = ns;
    }

    /* --- isolate marginal per-node costs (subtract child-subtree totals) --- */
    const double w_id   = t_id;
    const double w_lit  = t_lit;
    const double w_add  = t_add - 2 * t_id;          /* the + op only        */
    const double w_cmp  = t_cmp - 2 * t_id;          /* the < op only        */
    const double w_asn  = t_asn - t_add;             /* assign + slot write  */
    const double w_ret  = t_ret - t_id;              /* return dispatch      */
    const double w_if   = t_if  - t_cmp - t_asn;     /* if dispatch          */
    /* call machinery = total - args(2 ids) - body(return + add total). The
     * args are the caller's (substituted away by inlining), so they are not
     * part of the machinery the inline saves. */
    const double body   = w_ret + t_add;             /* `return a+b` body    */
    const double w_call = t_call - 2 * t_id - body;  /* the call overhead    */

    auto rel = [&](double w) { return w / w_id; };
    printf("\nInlining cost-model weights (ns/eval, best of 5, %ldM iters)\n",
           M / 1000000);
    printf("  built from hand-constructed AST nodes (optimizer-immune)\n\n");
    printf("  %-22s %8s %8s\n", "node", "ns", "xId");
    printf("  ------------------------------------------\n");
    printf("  %-22s %8.2f %8.1f\n", "id (slot read)",  w_id,  rel(w_id));
    printf("  %-22s %8.2f %8.1f\n", "literal",          w_lit, rel(w_lit));
    printf("  %-22s %8.2f %8.1f\n", "arith op (+)",     w_add, rel(w_add));
    printf("  %-22s %8.2f %8.1f\n", "compare (<)",      w_cmp, rel(w_cmp));
    printf("  %-22s %8.2f %8.1f\n", "assignment",       w_asn, rel(w_asn));
    printf("  %-22s %8.2f %8.1f\n", "return",           w_ret, rel(w_ret));
    printf("  %-22s %8.2f %8.1f\n", "if",               w_if,  rel(w_if));
    printf("  %-22s %8.2f %8.1f  <- threshold\n",
           "CALL (2-param)",  w_call, rel(w_call));
    printf("\n  benefit function: inline a body when the sum of its node\n");
    printf("  weights is below the CALL weight (%.1f xId).\n", rel(w_call));
    printf("\n  suggested integer weights (xId, rounded):\n");
    printf("    id=%d lit=%d add=%d cmp=%d assign=%d return=%d if=%d CALL=%d\n",
           1,
           std::max(1, (int)(rel(w_lit)  + 0.5)),
           std::max(1, (int)(rel(w_add)  + 0.5)),
           std::max(1, (int)(rel(w_cmp)  + 0.5)),
           std::max(1, (int)(rel(w_asn)  + 0.5)),
           std::max(1, (int)(rel(w_ret)  + 0.5)),
           std::max(1, (int)(rel(w_if)   + 0.5)),
           std::max(1, (int)(rel(w_call) + 0.5)));
}
