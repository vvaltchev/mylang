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
