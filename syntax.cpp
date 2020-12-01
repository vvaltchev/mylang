/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"

void SingleChildConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "Expr(" << endl;
    elem->serialize(s, level + 1);
    s << endl;
    s << string(level * 2, ' ');
    s << ")";
}

void
MultiOpConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "MultiOpExpr(\n";

    for (const auto &[op, e] : elems) {

        if (op != Op::invalid) {
            s << string((level + 1) * 2, ' ');
            s << "Op '" << OpToString[(int)op] << "'";
            s << endl;
        }

        e->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}

void LiteralInt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Int(");
    s << to_string(value);
    s << ")";
}

EvalValue Expr03::eval(EvalContext *ctx) const
{
    long val = 1;

    for (const auto &[op, e] : elems) {

        if (op == Op::times) {

            val *= e->eval(ctx).value;

        } else if (op == Op::div) {

            long val = e->eval(ctx).value;

            if (val == 0)
                throw DivisionByZeroEx();

            val /= val;

        } else if (op == Op::invalid) {

            val = e->eval(ctx).value;
        }
    }

    return val;
}

EvalValue Expr04::eval(EvalContext *ctx) const
{
    long val = 0;

    for (const auto &[op, e] : elems) {

        if (op == Op::plus)
            val += e->eval(ctx).value;
        else if (op == Op::minus)
            val -= e->eval(ctx).value;
        else if (op == Op::invalid)
            val = e->eval(ctx).value;
    }

    return val;
}
