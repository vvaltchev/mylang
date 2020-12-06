/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "syntax.h"

ostream &operator<<(ostream &s, const EvalValue &c)
{
    return s << c.get_type()->to_string(c);
}

void ChildlessConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');
    s << indent;
    s << name;
}

void SingleChildConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');
    bool is_const = dynamic_cast<Literal *>(elem.get()) != nullptr;

    s << indent;
    s << name << "(";

    if (!is_const)
        s << endl;

    elem->serialize(s, is_const ? 0 : level + 1);

    if (!is_const) {
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

void LiteralNone::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("None");
}

void LiteralStr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "\"";
    s << value;
    s << "\"";
}

LiteralStr::LiteralStr(string_view v)
     : value (make_shared<string>(v))
{

}

void Identifier::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Id(\"");
    s << value;
    s << "\")";
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

void Expr14::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;

    if (fl & pFlags::pInDecl)
        s << "VarDecl";
    else
        s << name;

    s << "(\n";
    lvalue->serialize(s, level + 1);
    s << endl;

    s << string((level + 1) * 2, ' ');
    s << "Op '" << OpString[(int)op] << "'";
    s << endl;

    rvalue->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

void IfStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    condExpr->serialize(s, level+1);
    s << endl;

    if (thenBlock) {

        thenBlock->serialize(s, level+1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoThenBlock>";
    }

    s << endl;

    if (elseBlock) {

        elseBlock->serialize(s, level+1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoElseBlock>";
    }

    s << endl;
    s << indent;
    s << ")";
}

void WhileStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";
    condExpr->serialize(s, level+1);
    s << endl;

    if (body) {

        body->serialize(s, level+1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoBody>";
    }

    s << endl;
    s << indent;
    s << ")";
}
