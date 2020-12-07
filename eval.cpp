/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "errors.h"
#include "syntax.h"
#include "lexer.h"
#include "evaltypes.cpp.h"
#include "type_int.cpp.h"
#include "type_str.cpp.h"

EvalValue builtin_print(EvalContext *ctx, ExprList *exprList)
{
    for (const auto &e: exprList->elems) {
        cout << RValue(e->eval(ctx)) << " ";
    }

    cout << endl;
    return EvalValue();
}

EvalValue builtin_len(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidArgumentEx();

    const EvalValue &e = RValue(exprList->elems[0]->eval(ctx));
    return e.get_type()->len(e);
}

const string &
find_builtin_name(const Builtin &b)
{
    for (const auto &[k, v]: EvalContext::builtins) {

        if (v->eval().get<Builtin>().func == b.func)
            return k;
    }

    throw InternalErrorEx();
}

const array<Type *, Type::t_count> AllTypes = {
    new TypeNone(),
    new Type(Type::t_lval),       /* internal type: not visible from outside */
    new Type(Type::t_undefid),    /* internal type: not visible from outside */
    new TypeInt(),
    new TypeBuiltin(),
    new TypeStr(),
};

/*
 * NOTE: these definitions *MUST FOLLOW* the definition of `AllTypes`
 * simply because the creation of LValue's contents does a lookup
 * in AllTypes.
 */

const EvalContext::SymbolsType EvalContext::const_builtins =
{
    make_pair("len", make_shared<LValue>(Builtin{builtin_len})),
};

const EvalContext::SymbolsType EvalContext::builtins =
{
    make_pair("print", make_shared<LValue>(Builtin{builtin_print})),
};

EvalContext::EvalContext(EvalContext *parent, bool const_ctx)
    : parent(parent)
    , const_ctx(const_ctx)
{
    if (!parent) {
        symbols.insert(const_builtins.begin(), const_builtins.end());

        if (!const_ctx) {
            symbols.insert(builtins.begin(), builtins.end());
        }
    }
}

EvalValue
RValue(const EvalValue &v)
{
    if (v.is<LValue *>())
        return v.get<LValue *>()->eval();

    if (v.is<UndefinedId>())
        throw UndefinedVariableEx{v.get<UndefinedId>().id};

    return v;
}

EvalValue Identifier::eval(EvalContext *ctx, bool rec) const
{
    while (ctx) {

        auto it = ctx->symbols.find(value);

        if (it != ctx->symbols.end())
            return EvalValue(it->second.get());

        ctx = ctx->parent;

        if (!rec)
            break;
    }

    return UndefinedId{value};
}

EvalValue CallExpr::eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &callable = RValue(id->eval(ctx));

    if (callable.is<UndefinedId>())
        throw UndefinedVariableEx{id->value};

    if (callable.is<Builtin>())
        return callable.get<Builtin>().func(ctx, args.get());

    throw TypeErrorEx();
}

EvalValue MultiOpConstruct::eval_first_rvalue(EvalContext *ctx) const
{
    assert(elems.size() >= 1 && elems[0].first == Op::invalid);

    EvalValue val = elems[0].second->eval(ctx);

    if (elems.size() > 1)
        val = RValue(val);

    return val;
}

EvalValue Expr02::eval(EvalContext *ctx, bool rec) const
{
    assert(elems.size() == 1 || elems.size() == 2);
    const auto &[op, e] = elems[0];

    if (op == Op::invalid)
        return e->eval(ctx);

    EvalValue val = RValue(e->eval(ctx));

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

EvalValue Expr03::eval(EvalContext *ctx, bool rec) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

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

EvalValue Expr04::eval(EvalContext *ctx, bool rec) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

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

EvalValue Expr06::eval(EvalContext *ctx, bool rec) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

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

EvalValue Expr07::eval(EvalContext *ctx, bool rec) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

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

EvalValue Expr11::eval(EvalContext *ctx, bool rec) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

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

EvalValue Expr12::eval(EvalContext *ctx, bool rec) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

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

static void
doAssign(const EvalValue &lval, const EvalValue &rval, Op op)
{
    EvalValue newVal;

    if (op == Op::assign) {

        newVal = RValue(rval);

    } else {

        newVal = RValue(lval.get<LValue *>()->eval());

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
}

EvalValue Expr14::eval(EvalContext *ctx, bool rec) const
{
    const bool inDecl = fl & pFlags::pInDecl;
    const EvalValue &lval = lvalue->eval(ctx);
    const EvalValue &rval = rvalue->eval(ctx);

    if (lval.is<UndefinedId>()) {

        if (!inDecl)
            throw UndefinedVariableEx{ lval.get<UndefinedId>().id };

        ctx->symbols.emplace(
            lval.get<UndefinedId>().id,
            make_shared<LValue>(RValue(rval), ctx->const_ctx)
        );

    } else if (lval.is<LValue *>()) {

        if (ctx->const_ctx)
            throw InternalErrorEx();

        if (inDecl) {

            const EvalValue &local_lval = lvalue->eval(ctx, false);

            if (!local_lval.is<UndefinedId>()) {
                /* We're re-defining the same variable, in the same block */
                throw AlreadyDefinedEx();
            }

            /* We're re-declaring a symbol already declared outside */
            ctx->symbols.emplace(
                local_lval.get<UndefinedId>().id,
                make_shared<LValue>(RValue(rval), ctx->const_ctx)
            );

        } else {
            doAssign(lval, rval, op);
        }

    } else {
        throw NotLValueEx{move(lvalue)};
    }

    return rval;
}

EvalValue Expr15::eval(EvalContext *ctx, bool rec) const
{
    EvalValue val;
    assert(elems.size() > 0);
    val = elems[0].second->eval(ctx);

    for (auto it = elems.begin()+1; it != elems.end(); it++) {

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

EvalValue IfStmt::eval(EvalContext *ctx, bool rec) const
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

EvalValue BreakStmt::eval(EvalContext *ctx, bool rec) const
{
    throw LoopBreakEx();
}

EvalValue ContinueStmt::eval(EvalContext *ctx, bool rec) const
{
    throw LoopContinueEx();
}

EvalValue Block::eval(EvalContext *ctx, bool rec) const
{
    EvalContext curr(ctx, ctx->const_ctx);

    for (const auto &e: elems)
        e->eval(&curr);

    return EvalValue();
}

EvalValue WhileStmt::eval(EvalContext *ctx, bool rec) const
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
