/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"

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
    return 123; // TODO: implement
}

EvalValue CallExpr::eval(EvalContext *ctx) const
{
    return 1001;
}

EvalValue Expr03::eval(EvalContext *ctx) const
{
    long val = 1;
    long tmp;

    for (const auto &[op, e] : elems) {

        switch (op) {

            case Op::times:
                val *= EvalAs<long>(ctx, e.get());
                break;

            case Op::div:
                tmp = EvalAs<long>(ctx, e.get());

                if (tmp == 0)
                    throw DivisionByZeroEx();

                val /= tmp;
                break;

            case Op::invalid:
                val = EvalAs<long>(ctx, e.get());
                break;

            default:
                throw SyntaxErrorEx();
        }
    }

    return val;
}

EvalValue Expr04::eval(EvalContext *ctx) const
{
    long val = 0;

    for (const auto &[op, e] : elems) {

        switch (op) {
            case Op::plus:
                val += EvalAs<long>(ctx, e.get());
                break;
            case Op::minus:
                val -= EvalAs<long>(ctx, e.get());
                break;
            case Op::invalid:
                val = EvalAs<long>(ctx, e.get());
                break;
            default:
                SyntaxErrorEx();
        }
    }

    return val;
}

EvalValue Expr06::eval(EvalContext *ctx) const
{
    long val = 0;

    for (const auto &[op, e] : elems) {

        switch (op) {

            case Op::lt:
                val = val < EvalAs<long>(ctx, e.get());
                break;
            case Op::gt:
                val = val > EvalAs<long>(ctx, e.get());
                break;
            case Op::le:
                val = val <= EvalAs<long>(ctx, e.get());
                break;
            case Op::ge:
                val = val >= EvalAs<long>(ctx, e.get());
                break;
            case Op::invalid:
                val = EvalAs<long>(ctx, e.get());
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
