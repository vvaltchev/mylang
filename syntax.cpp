/* SPDX-License-Identifier: BSD-2-Clause */

#include "syntax.h"

ostream &operator<<(ostream &s, const EvalValue &c)
{
    if (holds_alternative<nullptr_t>(c.value))
        s << "<NoValue>";
    else if (holds_alternative<long>(c.value))
        s << get<long>(c.value);

    return s;
}

void SingleChildConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');
    bool is_literal = dynamic_cast<Literal *>(elem.get()) != nullptr;

    s << indent;
    s << name << "(";

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
    s << name << "(\n";

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
    s << name << "(\n";

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

void Identifier::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Id(");
    s << value;
    s << ")";
}

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
