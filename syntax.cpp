/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"

void SingleChildConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');
    bool is_literal = dynamic_cast<Literal *>(elem.get()) != nullptr;

    s << indent;
    s << "Expr(";

    if (!is_literal)
        s << endl;

    elem->serialize(s, is_literal ? 0 : level + 1);

    if (!is_literal) {
        s << endl;
        s << indent;
    }

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
            s << "Op '" << OpString[(int)op] << "'";
            s << endl;
        }

        e->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}

void MultiElemConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "MultiElem(\n";

    for (const auto &e: elems) {
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
    long tmp;

    for (const auto &[op, e] : elems) {

        switch (op) {

            case Op::times:
                val *= e->eval(ctx).value;
                break;

            case Op::div:
                tmp = e->eval(ctx).value;

                if (tmp == 0)
                    throw DivisionByZeroEx();

                val /= tmp;
                break;

            case Op::invalid:
                val = e->eval(ctx).value;
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
                val += e->eval(ctx).value;
                break;
            case Op::minus:
                val -= e->eval(ctx).value;
                break;
            case Op::invalid:
                val = e->eval(ctx).value;
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
                val = val < e->eval(ctx).value;
                break;
            case Op::gt:
                val = val > e->eval(ctx).value;
                break;
            case Op::le:
                val = val <= e->eval(ctx).value;
                break;
            case Op::ge:
                val = val >= e->eval(ctx).value;
                break;
            case Op::invalid:
                val = e->eval(ctx).value;
                break;
            default:
                throw SyntaxErrorEx();
        }
    }

    return val;
}

EvalValue Block::eval(EvalContext *ctx) const
{
    for (const auto &e : elems) {
        e->eval(ctx);
    }

    return 0;
}
