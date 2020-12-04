/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "errors.h"
#include "syntax.h"
#include "lexer.h"
#include "type_int.cpp.h"

const array<Type *, Type::t_count> AllTypes = {
    new Type(Type::t_none),
    new Type(Type::t_lval),
    new Type(Type::t_undefid),
    new TypeInt(),
};

const EvalContext::SymbolsType EvalContext::builtins;

EvalContext::EvalContext()
{
    /* Copy the builtins in the current EvalContext */
    symbols = builtins;
}

EvalValue
RValue(EvalValue v)
{
    if (v.is<LValue *>())
        return v.get<LValue *>()->eval();

    if (v.is<UndefinedId>())
        throw UndefinedVariableEx{v.get<UndefinedId>().id};

    return v;
}

EvalValue Identifier::eval(EvalContext *ctx) const
{
    auto it = ctx->symbols.find(value);

    if (it == ctx->symbols.end())
        return UndefinedId{value};

    return EvalValue(it->second.get());
}

EvalValue CallExpr::eval(EvalContext *ctx) const
{
    if (id->value == "print") {

        for (const auto &e: args->elems) {
            cout << RValue(e->eval(ctx)) << " ";
        }

        cout << endl;
        return EvalValue();

    } else {

        throw UndefinedVariableEx{id->value};
    }
}

EvalValue MultiOpConstruct::eval_first_rvalue(EvalContext *ctx) const
{
    if (!elems.size() || elems[0].first != Op::invalid)
        throw InternalErrorEx();

    EvalValue val = elems[0].second->eval(ctx);

    if (elems.size() > 1)
        val = RValue(val);

    return val;
}

EvalValue Expr02::eval(EvalContext *ctx) const
{
    if (!(elems.size() == 1 || elems.size() == 2))
        throw InternalErrorEx();

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
            val.type->opneg(val);
            break;
        case Op::lnot:
            /* Unary operator '!': logial not */
            val.type->lnot(val);
            break;
        default:
            throw InternalErrorEx();
    }

    return val;
}

EvalValue Expr03::eval(EvalContext *ctx) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::times:
                val.type->mul(val, RValue(e->eval(ctx)));
                break;
            case Op::div:
                val.type->div(val, RValue(e->eval(ctx)));
                break;
            case Op::mod:
                val.type->mod(val, RValue(e->eval(ctx)));
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return val;
}

EvalValue Expr04::eval(EvalContext *ctx) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::plus:
                val.type->add(val, RValue(e->eval(ctx)));
                break;
            case Op::minus:
                val.type->sub(val, RValue(e->eval(ctx)));
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return val;
}

EvalValue Expr06::eval(EvalContext *ctx) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::lt:
                val.type->lt(val, RValue(e->eval(ctx)));
                break;
            case Op::gt:
                val.type->gt(val, RValue(e->eval(ctx)));
                break;
            case Op::le:
                val.type->le(val, RValue(e->eval(ctx)));
                break;
            case Op::ge:
                val.type->ge(val, RValue(e->eval(ctx)));
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return val;
}

EvalValue Expr07::eval(EvalContext *ctx) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::eq:
                val.type->eq(val, RValue(e->eval(ctx)));
                break;
            case Op::noteq:
                val.type->noteq(val, RValue(e->eval(ctx)));
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return val;
}

EvalValue Expr11::eval(EvalContext *ctx) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::land:
                val.type->land(val, RValue(e->eval(ctx)));
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return val;
}

EvalValue Expr12::eval(EvalContext *ctx) const
{
    EvalValue val = eval_first_rvalue(ctx);

    for (auto it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::lor:
                val.type->lor(val, RValue(e->eval(ctx)));
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return val;
}

EvalValue Expr14::eval(EvalContext *ctx) const
{
    EvalValue lval = lvalue->eval(ctx);
    EvalValue rval = rvalue->eval(ctx);

    if (lval.is<UndefinedId>()) {

        ctx->symbols.emplace(
            lval.get<UndefinedId>().id,
            make_shared<LValue>(RValue(rval))
        );

    } else if (lval.is<LValue *>()) {

        EvalValue newVal;

        if (op == Op::assign) {

            newVal = RValue(rval);

        } else {

            newVal = RValue(lval.get<LValue *>()->eval());

            switch (op) {
                case Op::addeq:
                    newVal.type->add(newVal, RValue(rval));
                    break;
                case Op::subeq:
                    newVal.type->sub(newVal, RValue(rval));
                    break;
                case Op::muleq:
                    newVal.type->mul(newVal, RValue(rval));
                    break;
                case Op::diveq:
                    newVal.type->div(newVal, RValue(rval));
                    break;
                case Op::modeq:
                    newVal.type->mod(newVal, RValue(rval));
                    break;
                default:
                    throw InternalErrorEx();
            }
        }

        lval.get<LValue *>()->put(newVal);

    } else {

        throw NotLValueEx{move(lvalue)};
    }

    return rval;
}

EvalValue IfStmt::eval(EvalContext *ctx) const
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

EvalValue BreakStmt::eval(EvalContext *ctx) const
{
    throw LoopBreakEx();
}

EvalValue ContinueStmt::eval(EvalContext *ctx) const
{
    throw LoopContinueEx();
}

EvalValue Block::eval(EvalContext *ctx) const
{
    EvalValue val;

    for (const auto &e : elems) {
        val = e->eval(ctx);
    }

    return val;
}

EvalValue WhileStmt::eval(EvalContext *ctx) const
{
    while (is_true(condExpr->eval(ctx))) {

        try {

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
