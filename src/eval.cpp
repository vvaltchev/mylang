/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "errors.h"
#include "syntax.h"
#include "lexer.h"

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

EvalValue Identifier::do_eval(EvalContext *ctx, bool rec) const
{
    while (ctx) {

        const auto &&it = ctx->symbols.find(value);

        if (it != ctx->symbols.end())
            return EvalValue(&it->second);

        ctx = ctx->parent;

        if (!rec)
            break;
    }

    return UndefinedId{value};
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
        args_ctx->symbols.emplace(
            funcParams[i]->value,
            LValue(RValue(args[i]->eval(ctx)), ctx->const_ctx)
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
        args_ctx->symbols.emplace(
            funcParams[i]->value,
            LValue(args[i], ctx->const_ctx)
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

    args_ctx->symbols.emplace(
        funcParams[0]->value,
        LValue(arg, ctx->const_ctx)
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

    args_ctx->symbols.emplace(
        funcParams[0]->value,
        LValue(args.first, ctx->const_ctx)
    );

    args_ctx->symbols.emplace(
        funcParams[1]->value,
        LValue(args.second, ctx->const_ctx)
    );
}

template <class ArgsVecT> static
EvalValue do_func_call(EvalContext *ctx,
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
    }

    return EvalValue();
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

        if (callable.is<FlatSharedFuncObj>()) {
            return do_func_call(
                ctx,
                callable.get<FlatSharedFuncObj>().get(),
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
    FlatSharedArray::vec_type vec;
    vec.reserve(elems.size());

    for (const auto &e : elems) {
        vec.emplace_back(RValue(e->eval(ctx)), ctx->const_ctx);
    }

    return FlatSharedArray(move(vec));
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

        ctx->symbols.emplace(
            lval.get<UndefinedId>().id,
            LValue(RValue(rval), ctx->const_ctx || lvalue->is_const)
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
            ctx->symbols.emplace(
                local_lval.get<UndefinedId>().id,
                LValue(RValue(rval), ctx->const_ctx)
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

    if (lvalue->is_idlist())
        idlist = static_cast<IdList *>(lvalue.get());

    if (idlist) {

        if (!rval.is<FlatSharedArray>()) {

            for (const auto &e: idlist->elems)
                handle_single_expr14(ctx, inDecl, op, e.get(), rval);

        } else {

            const ArrayConstView &view = rval.get<FlatSharedArray>().get_view();

            for (unsigned i = 0; i < idlist->elems.size(); i++) {

                handle_single_expr14(
                    ctx,
                    inDecl,
                    op,
                    idlist->elems[i].get(),
                    i < view.size() ? view[i].get() : EvalValue()
                );
            }
        }

        return EvalValue();

    } else {

        return handle_single_expr14(ctx, inDecl, op, lvalue.get(), rval);
    }
}

EvalValue IfStmt::do_eval(EvalContext *ctx, bool rec) const
{
    if (is_true(condExpr->eval(ctx))) {

        if (thenBlock)
            thenBlock->eval(ctx);

    } else {

        if (elseBlock)
            elseBlock->eval(ctx);
    }

    return EvalValue();
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
    throw ReturnEx{ RValue(elem->eval(ctx)) };
}

EvalValue RethrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    throw RethrowEx{start, end};
}

EvalValue ThrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &e = RValue(elem->eval(ctx));

    if (!e.is<FlatSharedException>())
        throw TypeErrorEx(elem->start, elem->end);

    throw e.get<FlatSharedException>().get_ref();
}

EvalValue Block::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext curr(ctx, ctx ? ctx->const_ctx : false);

    for (const auto &e: elems) {

        EvalValue &&tmp = e->eval(&curr);

        if (tmp.is<UndefinedId>())
            throw UndefinedVariableEx(tmp.get<UndefinedId>().id, e->start, e->end);
    }

    return EvalValue();
}

EvalValue WhileStmt::do_eval(EvalContext *ctx, bool rec) const
{
    while (is_true(condExpr->eval(ctx))) {

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

    return EvalValue();
}

EvalValue FuncDeclStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue func(
        FlatSharedFuncObj(make_shared<FuncObject>(this, ctx))
    );

    if (id) {

        if (!id->eval(ctx).is<UndefinedId>())
            throw AlreadyDefinedEx(id->start, id->end);

        ctx->symbols.emplace(
            id->value,
            LValue(move(func), ctx->const_ctx)
        );

        return EvalValue();
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
        start_idx ? RValue(start_idx->eval(ctx)) : EvalValue(),
        end_idx ? RValue(end_idx->eval(ctx)) : EvalValue()
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

        if (id->value != ex_name)
            continue;

        try {

            EvalContext catch_ctx(ctx);

            if (asId) {

                FlatSharedException flatEx(
                    exObj
                        ? *exObj
                        : ExceptionObject(saved_ex->name)
                );

                catch_ctx.symbols.emplace(
                    asId->value,
                    LValue(move(flatEx), ctx->const_ctx)
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
        return EvalValue();

    for (const auto &p : catchStmts) {

        IdList *exList = p.first.exList.get();
        Identifier *asId = p.first.asId.get();
        Construct *catchBody = p.second.get();

        if (do_catch(ctx, saved_ex.get(), exList, asId, catchBody))
            return EvalValue();
    }

    saved_ex->rethrow();
    return EvalValue(); /* Make compilers unaware of [[noreturn]] happy */
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

    assert(container->is<FlatSharedArray>());

    if (container->valtype()->is_slice(container->val)) {

        *container = container->clone();
        return container->getval<FlatSharedArray>().get_ref()[container_idx].val;
    }

    if (container->valtype()->use_count(container->val) > 1)
        container->getval<FlatSharedArray>().clone_aliased_slices(container_idx);

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
ForeachStmt::do_iter(EvalContext *ctx, unsigned index, const EvalValue &elem) const
{
    const bool decl = index == 0 ? idsVarDecl : false;
    unsigned id_start = 0;

    if (indexed) {

        handle_single_expr14(
            ctx,
            decl,
            Op::assign,
            ids->elems[0].get(),
            static_cast<long>(index)
        );

        id_start++;
    }

    if (elem.is<FlatSharedArray>()) {

        const ArrayConstView &view = elem.get<FlatSharedArray>().get_view();

        for (unsigned i = id_start; i < ids->elems.size(); i++) {

            const unsigned val_i = i - id_start;

            handle_single_expr14(
                ctx,
                decl,
                Op::assign,
                ids->elems[i].get(),
                val_i < view.size() ? view[val_i].get() : EvalValue()
            );
        }

    } else {

        handle_single_expr14(ctx, decl, Op::assign, ids->elems[id_start].get(), elem);

        for (unsigned i = id_start+1; i < ids->elems.size(); i++)
            handle_single_expr14(ctx, decl, Op::assign, ids->elems[i].get(), EvalValue());
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

    if (cval.is<FlatSharedArray>()) {

        const ArrayConstView &view = cval.get<FlatSharedArray>().get_view();

        for (unsigned i = 0; i < view.size(); i++) {
            if (!do_iter(&loopCtx, i, view[i].get()))
                break;
        }

    } else if (cval.is<FlatSharedStr>()) {

        const string_view &view = cval.get<FlatSharedStr>().get_view();

        for (unsigned i = 0; i < view.size(); i++) {
            if (!do_iter(&loopCtx, i, FlatSharedStr(string(&view[i], 1))))
                break;
        }

    } else {

        throw TypeErrorEx(container->start, container->end);
    }

    return EvalValue();
}
