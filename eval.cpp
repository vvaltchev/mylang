/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "syntax.h"
#include "lexer.h"

static inline EvalValue
RValue(EvalValue v)
{
    if (v.isLValueP())
        return v.get<LValue *>()->eval();

    return v;
}

template <class T>
inline T RValueAs(EvalValue v)
{
    return RValue(v).get<T>();
}

template <class T>
inline T EvalAs(EvalContext *ctx, Construct *c)
{
    try {

        return RValueAs<T>(c->eval(ctx));

    } catch (bad_variant_access *) {

        throw TypeErrorEx();
    }
}

EvalValue Identifier::eval(EvalContext *ctx) const
{
    auto it = ctx->vars.find(value);

    if (it == ctx->vars.end())
        throw UndefinedVariableEx{value};

    return EvalValue(&it->second);
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

EvalValue Expr02::eval(EvalContext *ctx) const
{
    EvalValue val = 0;
    const auto &[op, e] = elems.at(0);

    switch (op) {
        case Op::plus:
            val = EvalAs<long>(ctx, e.get());
            break;
        case Op::minus:
            val = - EvalAs<long>(ctx, e.get());
            break;
        case Op::opnot:
            val = !EvalAs<long>(ctx, e.get());
            break;
        case Op::invalid:
            val = e->eval(ctx);
            break;
        default:
            SyntaxErrorEx();
    }

    return val;
}

EvalValue Expr03::eval(EvalContext *ctx) const
{
    EvalValue val;
    long tmp;

    for (const auto &[op, e] : elems) {

        switch (op) {

            case Op::times:
                val = RValueAs<long>(val) * EvalAs<long>(ctx, e.get());
                break;

            case Op::mod:   /* fall-through */
            case Op::div:
                tmp = EvalAs<long>(ctx, e.get());

                if (tmp == 0)
                    throw DivisionByZeroEx();

                if (op == Op::div)
                    val = RValueAs<long>(val) / tmp;
                else
                    val = RValueAs<long>(val) % tmp;

                break;

            case Op::invalid:
                val = e->eval(ctx);
                break;

            default:
                throw SyntaxErrorEx();
        }
    }

    return val;
}

EvalValue Expr04::eval(EvalContext *ctx) const
{
    EvalValue val;

    for (const auto &[op, e] : elems) {

        switch (op) {
            case Op::plus:
                val = RValueAs<long>(val) + EvalAs<long>(ctx, e.get());
                break;
            case Op::minus:
                val = RValueAs<long>(val) - EvalAs<long>(ctx, e.get());
                break;
            case Op::invalid:
                val = e->eval(ctx);
                break;
            default:
                SyntaxErrorEx();
        }
    }

    return val;
}

EvalValue Expr06::eval(EvalContext *ctx) const
{
    EvalValue val;

    for (const auto &[op, e] : elems) {

        switch (op) {

            case Op::lt:
                val = RValueAs<long>(val) < EvalAs<long>(ctx, e.get());
                break;
            case Op::gt:
                val = RValueAs<long>(val) > EvalAs<long>(ctx, e.get());
                break;
            case Op::le:
                val = RValueAs<long>(val) <= EvalAs<long>(ctx, e.get());
                break;
            case Op::ge:
                val = RValueAs<long>(val) >= EvalAs<long>(ctx, e.get());
                break;
            case Op::invalid:
                val = e->eval(ctx);
                break;
            default:
                throw SyntaxErrorEx();
        }
    }

    return val;
}

EvalValue Expr14::eval(EvalContext *ctx) const
{
    EvalValue lval, rval = rvalue->eval(ctx);
    string new_id;

    try {
        lval = lvalue->eval(ctx);
    } catch (UndefinedVariableEx e) {
        new_id = e.name;
    }

    if (!new_id.empty()) {

        if (rval.is<long>())
            ctx->vars.emplace(new_id, rval.get<long>());
        else
            throw TypeErrorEx();

    } else if (lval.isLValueP()) {

        lval.get<LValue *>()->put(rval);

    } else {

        throw NotLValue();
    }

    return rval;
}

EvalValue Block::eval(EvalContext *ctx) const
{
    EvalValue val;

    for (const auto &e : elems) {
        val = e->eval(ctx);
    }

    return val;
}
