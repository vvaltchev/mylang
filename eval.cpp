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

struct ReturnEx { EvalValue value; };

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

static EvalValue
do_func_call(EvalContext *ctx, FuncObject &obj, const ExprList *args)
{
    EvalContext args_ctx(&obj.capture_ctx);

    if (obj.func->params) {

        const auto &funcParams = obj.func->params->elems;

        if (args->elems.size() != funcParams.size())
            throw InvalidNumberOfArgsEx();

        for (size_t i = 0; i < args->elems.size(); i++) {
            args_ctx.symbols.emplace(
                funcParams[i]->value,
                LValue(RValue(args->elems[i]->eval(ctx)))
            );
        }
    }

    try {

        Block *block = dynamic_cast<Block *>(obj.func->body.get());

        if (block) {

            for (const auto &e: block->elems) {

                if (e->is_ret) {
                    /* Optimization: skip ReturnEx and eval the result directly */
                    ReturnStmt *ret = dynamic_cast<ReturnStmt *>(e.get());
                    assert(ret != nullptr);

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

EvalValue CallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &id_val = id->eval(ctx);

    if (id_val.is<UndefinedId>())
        throw UndefinedVariableEx(id->value, id->start, id->end);

    if (id_val.is<LValue *>()) {

        const EvalValue &callable = id_val.get<LValue *>()->get();

        if (callable.is<UndefinedId>())
            throw UndefinedVariableEx(id->value, id->start, id->end);

        try {

            if (callable.is<Builtin>())
                return callable.get<Builtin>().func(ctx, args.get());

            if (callable.is<SharedFuncObjWrapper>()) {
                return do_func_call(
                    ctx,
                    callable.get<SharedFuncObjWrapper>().get(),
                    args.get()
                );
            }

        } catch (Exception &e) {

            if (!e.loc_start) {
                e.loc_start = args->start;
                e.loc_end = args->end;
            }
            throw;
        }
    }

    throw NotCallableEx(id->start, id->end);
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

    EvalValue &&val = RValue(e->eval(ctx));

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

    return val;
}

EvalValue Expr03::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

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

    return val;
}

EvalValue Expr04::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

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

    return val;
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

    return val;
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

    return val;
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

    return val;
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

    return val;
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

EvalValue Expr14::do_eval(EvalContext *ctx, bool rec) const
{
    const bool inDecl = fl & pFlags::pInDecl;
    const EvalValue &lval = lvalue->eval(ctx);
    const EvalValue &rval = rvalue->eval(ctx);

    if (lval.is<UndefinedId>()) {

        if (!inDecl)
            throw UndefinedVariableEx{ lval.get<UndefinedId>().id };

        ctx->symbols.emplace(
            lval.get<UndefinedId>().id,
            LValue(RValue(rval), ctx->const_ctx)
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

EvalValue Expr15::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue val;
    assert(elems.size() > 0);
    val = elems[0].second->eval(ctx);

    for (auto &&it = elems.begin()+1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::comma:
                val = e->eval(ctx);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return val;
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

struct LoopBreakEx { };
struct LoopContinueEx { };

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
        SharedFuncObjWrapper(make_shared<FuncObject>(this, ctx))
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
