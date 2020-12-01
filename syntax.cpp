/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"

template <class T>
void generic_serialize(const T *obj,
                       const char *name,
                       ostream &s,
                       int level)
{
    string indent(level * 2, ' ');

    s << indent;
    s << name;
    s << "(\n";

    for (const auto &[op, e] : obj->elems) {

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

void Expr01::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "Expr01(" << endl;
    value->serialize(s, level + 1);
    s << endl;
    s << string(level * 2, ' ');
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

void Expr03::serialize(ostream &s, int level) const
{
    generic_serialize(this, "Expr03", s, level);
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

void Expr04::serialize(ostream &s, int level) const
{
    generic_serialize(this, "Expr04", s, level);
}
