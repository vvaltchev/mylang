/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"

void LiteralInt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Int(");
    s << to_string(value);
    s << ")";
}

void Factor::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "Factor(" << endl;
    value->serialize(s, level + 1);
    s << endl;
    s << string(level * 2, ' ');
    s << ")";
}

EvalValue Term::eval(EvalContext *ctx) const
{
    long val = 1;

    for (const auto &[op, f] : factors) {

        if (op == Op::times) {

            val *= f->eval(ctx).value;

        } else if (op == Op::div) {

            long e = f->eval(ctx).value;

            if (e == 0)
                throw DivisionByZeroEx();

            val /= e;

        } else {

            val = f->eval(ctx).value;
        }
    }

    return val;
}

void Term::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "Term(\n";

    for (const auto &[op, f] : factors) {

        if (op != Op::invalid) {
            s << string((level + 1) * 2, ' ');
            s << "Op '" << OpToString[(int)op] << "'";
            s << endl;
        }

        f->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}

EvalValue Expr::eval(EvalContext *ctx) const
{
    long val = 0;

    for (const auto &[op, t] : terms) {

        if (op == Op::plus)
            val += t->eval(ctx).value;
        else if (op == Op::minus)
            val -= t->eval(ctx).value;
        else if (op == Op::invalid)
            val = t->eval(ctx).value;

    }

    return val;
}

void Expr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "Expr(";
    s << endl;

    for (const auto &[op, t] : terms) {

        if (op != Op::invalid) {
            s << string((level + 1) * 2, ' ');
            s << "Op '" << OpToString[(int)op] << "'";
            s << endl;
        }

        t->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}
