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

struct LoopBreakEx { };
struct LoopContinueEx { };
struct ReturnEx { EvalValue value; };
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

static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const vector<unique_ptr<Construct>> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx)
{
    if (args.size() != funcParams.size())
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < args.size(); i++) {
        args_ctx->emplace(
            funcParams[i].get(),
            RValue(args[i]->eval(ctx)),
            ctx->const_ctx
        );
    }
}

static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const vector<EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx)
{
    if (args.size() != funcParams.size())
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < args.size(); i++) {
        args_ctx->emplace(
            funcParams[i].get(),
            args[i],
            ctx->const_ctx
        );
    }
}

static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const EvalValue &arg,
                    EvalContext *ctx,
                    EvalContext *args_ctx)
{
    if (funcParams.size() != 1)
        throw InvalidNumberOfArgsEx();

    args_ctx->emplace(
        funcParams[0].get(),
        arg,
        ctx->const_ctx
    );
}


static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const pair<EvalValue, EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx)
{
    if (funcParams.size() != 2)
        throw InvalidNumberOfArgsEx();

    args_ctx->emplace(
        funcParams[0].get(),
        args.first,
        ctx->const_ctx
    );

    args_ctx->emplace(
        funcParams[1].get(),
        args.second,
        ctx->const_ctx
    );
}

template <class ArgsVecT>
static EvalValue
do_func_call(EvalContext *ctx,
             FuncObject &obj,
             const ArgsVecT &args)
{
    EvalContext args_ctx(&obj.capture_ctx);

    if (obj.func->params) {
        const auto &funcParams = obj.func->params->elems;
        do_func_bind_params(funcParams, args, ctx, &args_ctx);
    }

    try {

        Block *block = nullptr;

        if (obj.func->body->is_block())
            block = static_cast<Block *>(obj.func->body.get());

        if (block) {

            for (const auto &e: block->elems) {

                if (e->is_ret()) {

                    /* Optimization: skip ReturnEx and eval the result directly */
                    ReturnStmt *ret = static_cast<ReturnStmt *>(e.get());

                    return do_func_return(
                        ret->elem->eval(&args_ctx),
                        ret->elem.get()
                    );
                }

                e->eval(&args_ctx);
            }

        } else {

            return do_func_return(
                obj.func->body->eval(&args_ctx),
                obj.func->body.get()
            );
        }

    } catch (ReturnEx &ret) {

        return move(ret.value);

    } catch (UndefinedVariableEx &undefEx) {

        if (obj.func->is_const)
            undefEx.in_pure_func = true;

        throw;
    }

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

EvalValue CallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &callable = RValue(what->eval(ctx));

    try {

        if (callable.is<Builtin>())
            return callable.get<Builtin>().func(ctx, args.get());

        if (callable.is<shared_ptr<FuncObject>>()) {
            return do_func_call(
                ctx,
                *callable.get<shared_ptr<FuncObject>>().get(),
                args->elems
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

EvalValue MultiOpConstruct::eval_first_rvalue(EvalContext *ctx) const
{
    assert(elems.size() >= 1 && elems[0].first == Op::invalid);

    const EvalValue &val = elems[0].second->eval(ctx);

    if (elems.size() > 1)
        return RValue(val);

    return val;
}

EvalValue Expr02::do_eval(EvalContext *ctx, bool rec) const
{
    assert(elems.size() == 1 || elems.size() == 2);
    const auto &[op, e] = elems[0];

    if (op == Op::invalid)
        return e->eval(ctx);

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
}

EvalValue Expr03::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::times:
                val.get_type()->mul(val, RValue(e->eval(ctx)));
                break;
            case Op::div:
                val.get_type()->div(val, RValue(e->eval(ctx)));
                break;
            case Op::mod:
                val.get_type()->mod(val, RValue(e->eval(ctx)));
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
                val.get_type()->add(val, RValue(e->eval(ctx)));
                break;
            case Op::minus:
                val.get_type()->sub(val, RValue(e->eval(ctx)));
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
                val.get_type()->lt(val, RValue(e->eval(ctx)));
                break;
            case Op::gt:
                val.get_type()->gt(val, RValue(e->eval(ctx)));
                break;
            case Op::le:
                val.get_type()->le(val, RValue(e->eval(ctx)));
                break;
            case Op::ge:
                val.get_type()->ge(val, RValue(e->eval(ctx)));
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
                val.get_type()->eq(val, RValue(e->eval(ctx)));
                break;
            case Op::noteq:
                val.get_type()->noteq(val, RValue(e->eval(ctx)));
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
                val.get_type()->land(val, RValue(e->eval(ctx)));
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
                val.get_type()->lor(val, RValue(e->eval(ctx)));
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
                newVal.get_type()->add(newVal, RValue(rval));
                break;
            case Op::subeq:
                newVal.get_type()->sub(newVal, RValue(rval));
                break;
            case Op::muleq:
                newVal.get_type()->mul(newVal, RValue(rval));
                break;
            case Op::diveq:
                newVal.get_type()->div(newVal, RValue(rval));
                break;
            case Op::modeq:
                newVal.get_type()->mod(newVal, RValue(rval));
                break;
            default:
                throw InternalErrorEx();
        }
    }

    lval.get<LValue *>()->put(newVal);
    return newVal;
}

static EvalValue
handle_single_expr14(EvalContext *ctx,
                     bool inDecl,
                     Op op,
                     Construct *lvalue,
                     const EvalValue &rval)
{
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

        if (ctx->const_ctx)
            throw InternalErrorEx();

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
    const EvalValue &rval = RValue(rvalue->eval(ctx));
    IdList *idlist = nullptr;

    if (inDecl && ctx->const_ctx && rval.is<shared_ptr<FuncObject>>()) {

        const FuncObject &obj = *rval.get<shared_ptr<FuncObject>>().get();

        if (obj.func->is_const) {

            /*
            * We cannot allow this because of an ownership problem:function
            * objects do NOT own their FuncDeclStmt object: it's owned by the
            * statement construct where it has been declared. In this case instead,
            * there's no "permanent" FuncDeclStmt that will live past this const
            * decl statement, so the FuncObject will have to take ownership of the
            * FuncDeclStmt, otherwise it will be destroyed after the current const
            * decl stmt. Implementing that ownership is tricky because unique_ptr<T>s
            * are used everywhere and, while in this case we could transfer
            * the ownership of the FuncDeclStmt to the FuncObject, in general
            * case we cannot do that.
            *
            * A naive solution might be to use shared_ptr<T> everywhere, but that
            * will have a much higher price at runtime and it will also destroy the
            * simple linear ownership model we currently use for syntax constructs.
            *
            * A much better solution would be to make pExpr14() return a construct
            * which will own the FuncDeclStmt object. But that's tricky to implement
            * in the general case: we don't know where in the `rvalue` syntax tree is
            * located the unique_ptr of the FuncDeclStmt that we'll need to move.
            * A way to overcome this issue is to add a virtual deep_clone() method in
            * the Construct interface and force all the constructs to implement it.
            * With it, we'll clone the FuncDeclStmt and store a unique_ptr of it in
            * our special func-owning const decl statement.
            *
            * But, it is really worth?
            */

           throw CannotBindPureFuncToConstEx(rvalue->start, rvalue->end);
        }
    }

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
    throw LoopBreakEx();
}

EvalValue ContinueStmt::do_eval(EvalContext *ctx, bool rec) const
{
    throw LoopContinueEx();
}

EvalValue ReturnStmt::do_eval(EvalContext *ctx, bool rec) const
{
    throw ReturnEx{
        elem ? RValue(elem->eval(ctx)) : none
    };
}

EvalValue RethrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    throw RethrowEx{start, end};
}

EvalValue ThrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &e = RValue(elem->eval(ctx));

    if (!e.is<FlatSharedException>()) {
        throw TypeErrorEx(
            "Expected an exception object",
            elem->start,
            elem->end
        );
    }

    throw e.get<FlatSharedException>().get();
}

EvalValue Block::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext curr(ctx, ctx ? ctx->const_ctx : false);

    for (const auto &e: elems) {

        EvalValue &&tmp = e->eval(&curr);

        if (tmp.is<UndefinedId>())
            throw UndefinedVariableEx(tmp.get<UndefinedId>().id, e->start, e->end);
    }

    return none;
}

EvalValue WhileStmt::do_eval(EvalContext *ctx, bool rec) const
{
    while (RValue(condExpr->eval(ctx)).is_true()) {

        try {

            if (body)
                body->eval(ctx);

        } catch (LoopBreakEx) {

            break;

        } catch (LoopContinueEx) {

            /*
             * Do nothing. Note: we cannot avoid this exception simply because
             * we can have `continue` inside one or multiple levels of nested
             * IF statements inside the loop, and we have to skip all of them
             * to jump back here and restart the loop.
             */
        }
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

                FlatSharedException flatEx(
                    exObj
                        ? *exObj
                        : ExceptionObject(saved_ex->name)
                );

                catch_ctx.emplace(
                    asId,
                    move(flatEx),
                    ctx->const_ctx
                );
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

            if (finallyBody)
                finallyBody->eval(ctx);
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

    try {

        if (body)
            body->eval(ctx);

    } catch (LoopBreakEx) {

        return false;

    } catch (LoopContinueEx) {

        /*
        * Do nothing. Note: we cannot avoid this exception simply because
        * we can have `continue` inside one or multiple levels of nested
        * IF statements inside the loop, and we have to skip all of them
        * to jump back here and restart the loop.
        */
    }

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

    DictObject::inner_type &data = dval.get<shared_ptr<DictObject>>()->get_ref();

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

    while (true) {

        if (cond && !RValue(cond->eval(&loop_ctx)).is_true())
            break;

        try {

            if (body)
                body->eval(&loop_ctx);

        } catch (LoopBreakEx) {

            break;

        } catch (LoopContinueEx) {

            /*
             * Do nothing. Note: we cannot avoid this exception simply because
             * we can have `continue` inside one or multiple levels of nested
             * IF statements inside the loop, and we have to skip all of them
             * to jump back here and restart the loop.
             */
        }

        if (inc)
            inc->eval(&loop_ctx);
    }

    return none;
}
