/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "errors.h"
#include "syntax.h"
#include "lexer.h"

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

EvalContext::EvalContext(EvalContext *parent, bool const_ctx, bool func_ctx)
    : parent(parent)
    , const_ctx(const_ctx)
    , func_ctx(func_ctx)
    , frame(parent ? parent->frame : nullptr)
    , flow((parent && !func_ctx) ? parent->flow : &flow_state)
{
    if (!parent) {
        symbols.insert(const_builtins.begin(), const_builtins.end());

        if (!const_ctx) {
            symbols.insert(builtins.begin(), builtins.end());
        }
    }
}

LValue *EvalContext::lookup(const Identifier *id)
{
    auto &&it = symbols.find(id->uid);

    if (it != symbols.end())
        return &it->second;

    return nullptr;
}

bool EvalContext::erase(const Identifier *id)
{
    if (id->sym.kind == SymKind::local && frame) {

        /* undef() on a resolved local: clear its live bit (cannot store the
         * UndefinedId sentinel in a slot - LValue forbids it). */
        const uint64_t bit = static_cast<uint64_t>(1) << id->sym.slot;
        const bool was_defined = frame->live & bit;
        frame->live &= ~bit;
        return was_defined;
    }

    const auto &it = symbols.find(id->uid);

    if (it == symbols.end())
        return false;

    symbols.erase(it);
    return true;
}

void EvalContext::emplace(const Identifier *id, const EvalValue &val, bool is_const)
{
    symbols.emplace(id->uid, LValue(val, is_const));
}

void EvalContext::emplace(const Identifier *id, EvalValue &&val, bool is_const)
{
    symbols.emplace(id->uid, LValue(move(val), is_const));
}

void EvalContext::emplace(const std::string_view &id, EvalValue &&val, bool is_const)
{
    symbols.emplace(UniqueId::get(id), LValue(move(val), is_const));
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

        throw;
    }
}

LiteralStr::LiteralStr(const std::string_view &v)
    : value(v.empty() ? empty_str : EvalValue(SharedStr(unescape_str(v))))
{ }

EvalValue Identifier::do_eval(EvalContext *ctx, bool rec) const
{
    if (sym.kind == SymKind::local && ctx->frame) {

        /* Resolved local: an O(1) slot read instead of a scope-chain walk. */
        const uint64_t bit = static_cast<uint64_t>(1) << sym.slot;

        if (ctx->frame->live & bit)
            return EvalValue(&ctx->frame->slots[sym.slot]);

        return UndefinedId{get_str()};   /* slot exists but was undef()'d */
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

/*
 * Bind one parameter. When `frame` is set (the function was resolved), the
 * value goes into its fixed slot and the slot is marked live; otherwise it is
 * emplaced into the args context map (the unresolved / const-eval path).
 */
static inline void
bind_param(EvalContext *args_ctx,
           Frame *frame,
           int idx,
           const Identifier *param,
           EvalValue val,
           bool is_const)
{
    if (frame) {
        frame->slots[idx] = LValue(move(val), is_const);
        frame->live |= static_cast<uint64_t>(1) << idx;
    } else {
        args_ctx->emplace(param, move(val), is_const);
    }
}

/*
 * Bind call arguments to the function's parameters. There is one overload per
 * argument representation - unevaluated argument expressions (the normal call
 * path), an already-evaluated value vector, a single value, and a pair (used by
 * builtins that invoke a callback). Each evaluates/forwards the args and hands
 * the actual storage to bind_param (slot Frame when resolved, else the map).
 */
static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const vector<unique_ptr<Construct>> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    if (args.size() != funcParams.size())
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < args.size(); i++) {
        /*
         * A param's binding is const iff it was declared `const` - NOT merely
         * because we are const-evaluating. This lets a (pure) function reassign
         * its own by-value parameters during const-eval, while a `const` param
         * stays immutable everywhere.
         */
        bind_param(
            args_ctx, frame, i, funcParams[i].get(),
            RValue(args[i]->eval(ctx)), funcParams[i]->const_param
        );
    }
}

static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const vector<EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    if (args.size() != funcParams.size())
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < args.size(); i++) {
        bind_param(
            args_ctx, frame, i, funcParams[i].get(), args[i], ctx->const_ctx
        );
    }
}

static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const EvalValue &arg,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    if (funcParams.size() != 1)
        throw InvalidNumberOfArgsEx();

    bind_param(args_ctx, frame, 0, funcParams[0].get(), arg, ctx->const_ctx);
}


static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const pair<EvalValue, EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    if (funcParams.size() != 2)
        throw InvalidNumberOfArgsEx();

    bind_param(args_ctx, frame, 0, funcParams[0].get(), args.first,
               ctx->const_ctx);
    bind_param(args_ctx, frame, 1, funcParams[1].get(), args.second,
               ctx->const_ctx);
}

/*
 * Invoke `obj` with `args`. Builds the callee's argument context (its own
 * FlowState and, when the function was resolved, a flat slot Frame), binds the
 * params, evaluates the body, and returns what the body returned via the
 * FlowState (or none). An UndefinedVariableEx escaping a pure func is tagged so
 * the error message can point at the pure-func restriction. `call_site` (the
 * CallExpr's loc) is recorded into an unwinding exception's backtrace.
 */
template <class ArgsVecT>
static EvalValue
do_func_call(EvalContext *ctx,
             FuncObject &obj,
             const ArgsVecT &args,
             Loc call_site = Loc())
{
    /* func_ctx == true gives this call its own FlowState (see eval.h) */
    EvalContext args_ctx(&obj.capture_ctx, false, true);

    /*
     * When the function was resolved, params live in a flat slot Frame (O(1)
     * access) instead of the args context map. The Frame lives on this stack
     * frame for the whole call; nested blocks inherit the pointer.
     */
    Frame frame;

    if (obj.func->resolved) {
        frame.init(obj.func->frame_size);
        args_ctx.frame = &frame;
    }

    if (obj.func->params) {
        const auto &funcParams = obj.func->params->elems;
        do_func_bind_params(
            funcParams, args, ctx, &args_ctx,
            obj.func->resolved ? &frame : nullptr
        );
    }

    try {

        if (!obj.func->body->is_block()) {

            /* Single-expression body: `func f(...) => expr;` */
            return do_func_return(
                obj.func->body->eval(&args_ctx),
                obj.func->body.get()
            );
        }

        /*
         * Block body: statements run until one sets the FlowState to `ret`
         * (Block::do_eval stops there). No exception is thrown for `return`.
         */
        obj.func->body->eval(&args_ctx);

    } catch (Exception &e) {

        if (obj.func->is_const)
            if (auto *undefEx = dynamic_cast<UndefinedVariableEx *>(&e))
                undefEx->in_pure_func = true;

        /*
         * Record this frame as the exception unwinds (innermost first). Capture
         * the name/params as strings now: the AST is destroyed during unwinding
         * before the top-level handler builds the backtrace.
         */
        BacktraceFrame bf;
        bf.name = obj.func->id ? string(obj.func->id->get_str()) : "<lambda>";
        if (obj.func->params)
            for (const auto &p : obj.func->params->elems)
                bf.params.push_back(string(p->get_str()));
        bf.call_site = call_site;
        e.backtrace.push_back(move(bf));
        throw;
    }

    FlowState &fs = *args_ctx.flow;

    if (fs.type == FlowState::ret)
        return move(fs.value);

    return none;
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const vector<EvalValue> &args)
{
    return do_func_call(ctx, obj, args);
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const EvalValue &arg)
{
    return do_func_call(ctx, obj, arg);
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const pair<EvalValue, EvalValue> &args)
{
    return do_func_call(ctx, obj, args);
}

static void stamp_operand_loc(const Construct *c, Exception &e);

EvalValue CallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue callable_storage;

    /* Point an undefined-callee error at the callee, not the whole call. */
    try {
        callable_storage = RValue(what->eval(ctx));
    } catch (Exception &e) {
        stamp_operand_loc(what.get(), e);
        throw;
    }

    const EvalValue &callable = callable_storage;

    try {

        if (callable.is<Builtin>())
            return callable.get<Builtin>().func(ctx, args.get());

        if (callable.is<shared_ptr<FuncObject>>()) {
            return do_func_call(
                ctx,
                *callable.get<shared_ptr<FuncObject>>().get(),
                args->elems,
                start            /* call site = this CallExpr's location */
            );
        }

    } catch (Exception &e) {

        if (!e.loc_start) {
            e.loc_start = args->start;
            e.loc_end = args->end;
        }
        throw;
    }

    throw NotCallableEx(what->start, what->end);
}

EvalValue LiteralArray::do_eval(EvalContext *ctx, bool rec) const
{
    SharedArrayObj::vec_type vec;

    if (!elems.size())
        return empty_arr;

    vec.reserve(elems.size());

    for (const auto &e : elems) {
        vec.emplace_back(RValue(e->eval(ctx)), ctx->const_ctx);
    }

    return SharedArrayObj(move(vec));
}

/*
 * Produce a fresh, fully-mutable deep copy of a const-evaluated container
 * value. This is what LiteralObj::do_eval hands out (see syntax.h): each
 * evaluation gets an independent, mutable array/dict - exactly what the old
 * per-element LiteralArray/LiteralDict produced at runtime - so writing through
 * a `var` bound to it works (no const element flags) and re-evaluating the node
 * never observes a prior mutation (nested containers are copied too). Scalars
 * and strings are returned as-is (trivially immutable / copy-on-write). Empty
 * arrays collapse to the shared empty_arr singleton, matching LiteralArray.
 */
static EvalValue
make_mutable_clone(const EvalValue &v)
{
    if (v.is<SharedArrayObj>()) {

        const ArrayConstView &view = v.get<SharedArrayObj>().get_view();

        if (!view.size())
            return empty_arr;

        SharedArrayObj::vec_type vec;
        vec.reserve(view.size());

        for (unsigned i = 0; i < view.size(); i++)
            vec.emplace_back(make_mutable_clone(view[i].get()), false);

        return SharedArrayObj(move(vec));
    }

    if (v.is<shared_ptr<DictObject>>()) {

        DictObject::inner_type data;
        const DictObject::inner_type &src =
            v.get<shared_ptr<DictObject>>()->get_ref();

        for (const auto &p : src) {
            data.emplace(
                p.first,
                LValue(make_mutable_clone(p.second.get()), false)
            );
        }

        return shared_ptr<DictObject>(make_shared<DictObject>(move(data)));
    }

    return v;
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

        const ArrayConstView &view = v.get<SharedArrayObj>().get_view();

        SharedArrayObj::vec_type vec;
        vec.reserve(view.size());

        for (unsigned i = 0; i < view.size(); i++)
            vec.emplace_back(make_const_clone(view[i].get()), false);

        SharedArrayObj arr(move(vec));
        arr.set_readonly();
        return arr;
    }

    if (v.is<shared_ptr<DictObject>>()) {

        DictObject::inner_type data;
        const DictObject::inner_type &src =
            v.get<shared_ptr<DictObject>>()->get_ref();

        for (const auto &p : src) {
            data.emplace(
                p.first,
                LValue(make_const_clone(p.second.get()), false)
            );
        }

        auto obj = make_shared<DictObject>(move(data));
        obj->set_readonly();
        return shared_ptr<DictObject>(obj);
    }

    return v;
}

EvalValue LiteralObj::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * A const-decl target (immutable) shares the baked value directly: it was
     * baked deep read-only (make_const_clone, in the parser), so it can't be
     * mutated and re-entry can safely observe the same object - no per-eval
     * copy, and the const symbol and this node hold one buffer, not two. Any
     * other target (a `var` or a read-only consumer) gets a fresh mutable deep
     * copy, so writes work and re-entry never sees a prior mutation.
     */
    return immutable ? value : make_mutable_clone(value);
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
    try {
        if (op == Op::land)
            acc.get_type()->land(acc, RValue(operand->eval(ctx)));
        else
            acc.get_type()->lor(acc, RValue(operand->eval(ctx)));
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
                /* Unary operator '+': do nothing */
                break;
            case Op::minus:
                /* Unary operator '-': negate */
                val.get_type()->opneg(val);
                break;
            case Op::lnot:
                /* Unary operator '!': logial not */
                val.get_type()->lnot(val);
                break;
            default:
                throw InternalErrorEx();
        }

        return move(val);

    } catch (Exception &ex) {
        stamp_operand_loc(e.get(), ex);
        throw;
    }
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

    return move(val);
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

    return move(val);
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

    return move(val);
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

    return move(val);
}

EvalValue Expr11::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::land:
                logop_loc(val, e.get(), ctx, op);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return move(val);
}

EvalValue Expr12::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::lor:
                logop_loc(val, e.get(), ctx, op);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return move(val);
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

        switch (op) {
            case Op::addeq:
                num_bin_op(newVal, RValue(rval), &Type::add);
                break;
            case Op::subeq:
                num_bin_op(newVal, RValue(rval), &Type::sub);
                break;
            case Op::muleq:
                num_bin_op(newVal, RValue(rval), &Type::mul);
                break;
            case Op::diveq:
                num_bin_op(newVal, RValue(rval), &Type::div);
                break;
            case Op::modeq:
                num_bin_op(newVal, RValue(rval), &Type::mod);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    lval.get<LValue *>()->put(newVal);
    return newVal;
}

/* Return `lvalue` as an Identifier resolved to a slot, or nullptr otherwise. */
static inline const Identifier *
as_resolved_local(const Construct *lvalue)
{
    const Identifier *id = dynamic_cast<const Identifier *>(lvalue);
    return (id && id->sym.kind == SymKind::local) ? id : nullptr;
}

static EvalValue
handle_single_expr14(EvalContext *ctx,
                     bool inDecl,
                     Op op,
                     Construct *lvalue,
                     const EvalValue &rval)
{
    /*
     * Fast path: declaring a resolved local. Write its slot and mark it live.
     * The resolver already rejected illegal same-block redeclarations and the
     * enclosing block cleared this slot's live bit on entry, so we just
     * (over)write - which is also exactly what a loop re-entry needs. The
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
            f->live |= static_cast<uint64_t>(1) << id->sym.slot;
            return rval;
        }
    }

    const EvalValue &lval = lvalue->eval(ctx);

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
                /* We're re-defining the same variable, in the same block */
                throw AlreadyDefinedEx(lvalue->start, lvalue->end);
            }

            /* We're re-declaring a symbol already declared outside */
            ctx->emplace(
                local_lval.get<UndefinedId>().id,
                RValue(rval),
                ctx->const_ctx
            );

        } else {
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

    /* Evaluate the rhs first; point errors (undefined var, ...) at the rhs
     * itself, not at the whole `lhs = rhs` assignment. */
    EvalValue rval_storage;
    try {
        rval_storage = RValue(rvalue->eval(ctx));
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

            for (const auto &e: idlist->elems)
                handle_single_expr14(ctx, inDecl, op, e.get(), rval);

        } else {

            const ArrayConstView &view = rval.get<SharedArrayObj>().get_view();

            for (size_type i = 0; i < idlist->elems.size(); i++) {

                handle_single_expr14(
                    ctx,
                    inDecl,
                    op,
                    idlist->elems[i].get(),
                    i < view.size() ? view[i].get() : none
                );
            }
        }

        return none;

    } else {

        return handle_single_expr14(ctx, inDecl, op, lvalue.get(), rval);
    }
}

EvalValue IfStmt::do_eval(EvalContext *ctx, bool rec) const
{
    if (RValue(condExpr->eval(ctx)).is_true()) {

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
    ctx->flow->value = elem ? RValue(elem->eval(ctx)) : none;
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

    if (!e.is<shared_ptr<ExceptionObject>>()) {
        throw TypeErrorEx(
            "Expected an exception object",
            elem->start,
            elem->end
        );
    }

    throw *e.get<shared_ptr<ExceptionObject>>().get();
}

EvalValue Block::do_eval(EvalContext *ctx, bool rec) const
{
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
     * Reset this block's resolved locals to "undefined" on entry. The slots
     * persist for the whole call, so without this a re-entered block (a loop
     * body, say) would still see the previous iteration's bindings live. The
     * resolver guarantees a block's slots are a contiguous range, so one mask
     * op clears them; slot_count == 0 (no slotted locals / unresolved function)
     * makes this a no-op. Params live below the body block's range, so they are
     * never cleared here.
     */
    if (curr.frame && slot_count) {

        const uint64_t range = slot_count >= 64
            ? ~static_cast<uint64_t>(0)
            : (((static_cast<uint64_t>(1) << slot_count) - 1) << slot_start);

        curr.frame->live &= ~range;
    }

    for (const auto &e: elems) {

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

    while (RValue(condExpr->eval(ctx)).is_true()) {

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

EvalValue FuncDeclStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue func(
        shared_ptr<FuncObject>(make_shared<FuncObject>(this, ctx))
    );

    if (id) {

        if (!id->eval(ctx).is<UndefinedId>())
            throw AlreadyDefinedEx(id->start, id->end);

        ctx->emplace(
            id.get(),
            move(func),
            ctx->const_ctx
        );

        return none;
    }

    return func;
}

EvalValue Subscript::do_eval(EvalContext *ctx, bool rec) const
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

    return t->subscript(lval, RValue(index->eval(ctx)));
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

    return t->slice(
        lval,
        start_idx ? RValue(start_idx->eval(ctx)) : none,
        end_idx ? RValue(end_idx->eval(ctx)) : none
    );
}

static bool
do_catch(EvalContext *ctx,
         RuntimeException *saved_ex,
         IdList *exList,
         Identifier *asId,
         Construct *catchBody)
{
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

    ExceptionObject *exObj = dynamic_cast<ExceptionObject *>(saved_ex);
    string_view ex_name = exObj ? exObj->get_name() : saved_ex->name;

    for (const unique_ptr<Identifier> &id : exList->elems) {

        if (id->get_str() != ex_name)
            continue;

        try {

            EvalContext catch_ctx(ctx);

            if (asId) {

                shared_ptr<ExceptionObject> shared_ex =
                    make_shared<ExceptionObject>(
                        exObj
                            ? *exObj
                            : ExceptionObject(saved_ex->name)
                    );

                if (asId->sym.kind == SymKind::local && catch_ctx.frame) {

                    /* Resolved catch variable: bind it into its slot. */
                    Frame *f = catch_ctx.frame;
                    f->slots[asId->sym.slot] =
                        LValue(move(shared_ex), ctx->const_ctx);
                    f->live |= static_cast<uint64_t>(1) << asId->sym.slot;

                } else {

                    catch_ctx.emplace(
                        asId,
                        move(shared_ex),
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
    struct trivial_scope_guard {

        EvalContext *ctx;
        Construct *finallyBody;

        trivial_scope_guard(EvalContext *ctx, Construct *body)
            : ctx(ctx), finallyBody(body) { }

        ~trivial_scope_guard() {

            if (!finallyBody)
                return;

            FlowState &fs = *ctx->flow;

            /*
             * A return/break/continue may be in flight out of the try or catch
             * block. Suspend it so the finally body runs to completion, then
             * resume it - unless finally raised its own control-flow signal,
             * which then takes over (as in C#/Java).
             */
            const FlowState::Type saved_type = fs.type;
            EvalValue saved_val = move(fs.value);
            fs.type = FlowState::none;

            finallyBody->eval(ctx);

            if (fs.type == FlowState::none) {
                fs.type = saved_type;
                fs.value = move(saved_val);
            }
        }
    };

    trivial_scope_guard on_exit(ctx, finallyBody.get());
    unique_ptr<RuntimeException> saved_ex;

    try {

        tryBody->eval(ctx);

    } catch (const RuntimeException &e) {

        saved_ex.reset(e.clone());
    }

    if (!saved_ex)
        return none;

    for (const auto &p : catchStmts) {

        IdList *exList = p.first.exList.get();
        Identifier *asId = p.first.asId.get();
        Construct *catchBody = p.second.get();

        if (do_catch(ctx, saved_ex.get(), exList, asId, catchBody))
            return none;
    }

    saved_ex->rethrow();
    return none; /* Make compilers unaware of [[noreturn]] happy */
}

LValue LValue::clone()
{
    LValue nl(valtype()->clone(val), is_const);
    nl.container = container;
    nl.container_idx = container_idx;
    return nl;
}

EvalValue &LValue::get_value_for_put()
{
    if (!container)
        return val;

    assert(container->is<SharedArrayObj>());

    if (container->valtype()->is_slice(container->val)) {
        const size_type off = container->getval<SharedArrayObj>().offset();
        *container = container->clone();
        container_idx -= off;
        return container->getval<SharedArrayObj>().get_vec()[container_idx].val;
    }

    if (container->valtype()->use_count(container->val) > 1)
        container->getval<SharedArrayObj>().clone_aliased_slices(container_idx);

    return val;
}

void LValue::put(const EvalValue &v)
{
    get_value_for_put() = v;
    type_checks();
}

void LValue::put(EvalValue &&v)
{
    get_value_for_put() = move(v);
    type_checks();
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

        handle_single_expr14(
            ctx,
            decl,
            Op::assign,
            ids->elems[0].get(),
            static_cast<int_type>(index)
        );

        id_start++;
    }

    if (count == 1) {

        if (elems[0].is<SharedArrayObj>() && ids->elems.size() > (1 + id_start)) {

            const ArrayConstView &view =
                elems[0].get<SharedArrayObj>().get_view();

            for (size_type i = id_start; i < ids->elems.size(); i++) {

                const size_type val_i = i - id_start;

                handle_single_expr14(
                    ctx,
                    decl,
                    Op::assign,
                    ids->elems[i].get(),
                    val_i < view.size() ? view[val_i].get() : none
                );
            }

        } else {

            handle_single_expr14(
                ctx, decl, Op::assign, ids->elems[id_start].get(), elems[0]
            );

            for (size_type i = id_start+1; i < ids->elems.size(); i++) {
                handle_single_expr14(
                    ctx, decl, Op::assign, ids->elems[i].get(), none
                );
            }
        }

    } else {

        for (size_type i = id_start; i < ids->elems.size(); i++) {

            const size_type val_i = i - id_start;

            handle_single_expr14(
                ctx,
                decl,
                Op::assign,
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

        const ArrayConstView &view = cval.get<SharedArrayObj>().get_view();

        for (size_type i = 0; i < view.size(); i++) {

            const EvalValue &elem = view[i].get();

            if (!do_iter(&loopCtx, i, &elem, 1))
                break;
        }

    } else if (cval.is<SharedStr>()) {

        const string_view &view = cval.get<SharedStr>().get_view();

        for (size_type i = 0; i < view.size(); i++) {

            const EvalValue &elem = SharedStr(string(&view[i], 1));

            if (!do_iter(&loopCtx, i, &elem, 1))
                break;
        }

    } else if (cval.is<shared_ptr<DictObject>>()) {

        const DictObject::inner_type &data
            = cval.get<shared_ptr<DictObject>>()->get_ref();

        size_type i = 0;

        for (const auto &p : data) {

            const EvalValue elems[2] = { p.first, p.second.get() };

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

EvalValue LiteralDict::do_eval(EvalContext *ctx, bool rec) const
{
    DictObject::inner_type data;

    for (const auto &e : elems) {

        data.emplace(
            RValue(e->key->eval(ctx)),
            LValue(RValue(e->value->eval(ctx)), ctx->const_ctx)
        );
    }

    return shared_ptr<DictObject>(make_shared<DictObject>(move(data)));
}

EvalValue MemberExpr::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&dval = RValue(what->eval(ctx));

    if (!dval.is<shared_ptr<DictObject>>())
        throw TypeErrorEx("Expected dict object", what->start, what->end);

    const shared_ptr<DictObject> &obj = dval.get<shared_ptr<DictObject>>();
    DictObject::inner_type &data = obj->get_ref();

    if (obj->is_readonly()) {

        /*
         * Read-only dict: never hand out an assignable lvalue and never
         * auto-vivify. A read of a missing member yields `none`; a write
         * (`d.k = ...`) sees an rvalue and fails with NotLValueEx.
         */
        const auto &it = data.find(memId);
        return it != data.end() ? it->second.get() : none;
    }

    return &(
        *data.emplace(
            memId, LValue(none, false)
        ).first
    ).second;
}

EvalValue ForStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext loop_ctx(ctx, ctx->const_ctx);

    if (init)
        init->eval(&loop_ctx);

    FlowState &fs = *loop_ctx.flow;

    while (true) {

        if (cond && !RValue(cond->eval(&loop_ctx)).is_true())
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
