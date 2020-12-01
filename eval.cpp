/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "syntax.h"
#include "lexer.h"

template <class T>
T EvalAs(EvalContext *ctx, Construct *c)
{
    try {

        return get<T>(c->eval(ctx).value);

    } catch (bad_variant_access *) {

        throw TypeErrorEx();
    }
}

void CallExpr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    id->serialize(s, level + 1);
    s << endl;
    args->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

EvalValue Identifier::eval(EvalContext *ctx) const
{
    throw UndefinedVariableEx{value};
}

EvalValue CallExpr::eval(EvalContext *ctx) const
{
    if (id->value == "print") {

        for (const auto &e: args->elems) {
            cout << e->eval(ctx) << " ";
        }

        cout << endl;
        return EvalValue();

    } else {

        throw UndefinedVariableEx{id->value};
    }
}

EvalValue Expr03::eval(EvalContext *ctx) const
{
    EvalValue val;
    long tmp;

    for (const auto &[op, e] : elems) {

        switch (op) {

            case Op::times:
                val = val.get<long>() * EvalAs<long>(ctx, e.get());
                break;

            case Op::div:
                tmp = EvalAs<long>(ctx, e.get());

                if (tmp == 0)
                    throw DivisionByZeroEx();

                val = val.get<long>() / tmp;
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
                val = val.get<long>() + EvalAs<long>(ctx, e.get());
                break;
            case Op::minus:
                val = val.get<long>() - EvalAs<long>(ctx, e.get());
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
                val = val.get<long>() < EvalAs<long>(ctx, e.get());
                break;
            case Op::gt:
                val = val.get<long>() > EvalAs<long>(ctx, e.get());
                break;
            case Op::le:
                val = val.get<long>() <= EvalAs<long>(ctx, e.get());
                break;
            case Op::ge:
                val = val.get<long>() >= EvalAs<long>(ctx, e.get());
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

EvalValue Block::eval(EvalContext *ctx) const
{
    EvalValue val;

    for (const auto &e : elems) {
        val = e->eval(ctx);
    }

    return val;
}
