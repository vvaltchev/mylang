/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "syntax.h"

using std::string;
using std::string_view;

string
escape_str(const string_view &v)
{
    string s;
    s.reserve(v.size() * 2);

    for (char c : v) {

        switch (c) {

            case '\"':
                s += "\\\"";
                break;
            case '\\':
                s += "\\\\";
                break;
            case '\r':
                s += "\\r";
                break;
            case '\n':
                s += "\\n";
                break;
            case '\t':
                s += "\\t";
                break;
            case '\v':
                s += "\\v";
                break;
            case '\a':
                s += "\\a";
                break;
            case '\b':
                s += "\\b";
                break;

            default:
                s += c;
        }
    }

    return s;
}

ostream &operator<<(ostream &s, const EvalValue &c)
{
    return s << c.to_string();
}

void ChildlessConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');
    s << indent;
    s << name;
}

static void
generic_single_child_serialize(const char *name,
                               const unique_ptr<Construct> &elem,
                               ostream &s,
                               int level)
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(";

    if (elem) {

        if (!elem->is_const)
            s << endl;

        elem->serialize(s, elem->is_const ? 0 : level + 1);

        if (!elem->is_const) {
            s << endl;
            s << indent;
        }

    } else {

        s << "<NoElem>";
    }

    s << ")";
}

void SingleChildConstruct::serialize(ostream &s, int level) const
{
    generic_single_child_serialize(name, elem, s, level);
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

void LiteralInt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Int(");
    s << std::to_string(value);
    s << ")";
}

void LiteralFloat::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Float(");
    s << std::to_string(value);
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
    s << escape_str(value.get<SharedStr>().get_view());
    s << "\"";
}

void LiteralDictKVPair::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "KVPair(\n";

    key->serialize(s, level + 1);
    s << endl;

    value->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

void MemberExpr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "MemberExpr(\n";

    what->serialize(s, level + 1);
    s << endl;

    s << string((level + 1) * 2, ' ');
    s << "Id(\"" << memId << "\")";
    s << endl;

    s << indent;
    s << ")";
}

void Identifier::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Id(\"");
    s << get_str();
    s << "\")";
}

void CallExpr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    what->serialize(s, level + 1);
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

    if (fl & pFlags::pInDecl) {

        if (fl & pFlags::pInConstDecl)
            s << "ConstDecl";
        else
            s << "VarDecl";

    } else {
        s << name;
    }

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

void FuncDeclStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    if (id) {

        id->serialize(s, level + 1);
        cout << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoName>" << endl;
    }

    if (captures) {

        captures->serialize(s, level + 1);
        cout << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoCaptures>" << endl;
    }

    if (params) {

        params->serialize(s, level + 1);
        cout << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoParams>" << endl;
    }

    body->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

void ReturnStmt::serialize(ostream &s, int level) const
{
    generic_single_child_serialize(name, elem, s, level);
}

void Subscript::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";
    what->serialize(s, level+1);
    s << endl;
    index->serialize(s, level+1);
    s << endl;
    s << indent;
    s << ")";
}

void Slice::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";
    what->serialize(s, level+1);
    s << endl;

    if (start_idx) {

        start_idx->serialize(s, level+1);
        s << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoStartIndex>" << endl;
    }

    if (end_idx) {

        end_idx->serialize(s, level+1);
        s << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoEndIndex>" << endl;
    }

    s << indent;
    s << ")";
}

void TryCatchStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    tryBody->serialize(s, level+1);
    s << endl;

    for (const auto &p : catchStmts) {

        IdList *exList = p.first.exList.get();
        Identifier *asId = p.first.asId.get();
        Construct *body = p.second.get();

        s << string((level + 1) * 2, ' ');
        s << "Catch( ";

        if (exList) {

            for (const unique_ptr<Identifier> &e : exList->elems) {
                cout << e->get_str() << " ";
            }

            if (asId) {
                cout << "as " << asId->get_str() << " ";
            }

        } else {
            s << "<anything>";
        }

        s << ") (\n";
        body->serialize(s, level+2);
        s << endl;
        s << string((level + 1) * 2, ' ');
        s << ")\n";
    }

    if (finallyBody) {
        s << string((level + 1) * 2, ' ');
        s << "Finally( ";

        finallyBody->serialize(s, level+2);
        s << endl;
        s << ")\n";
    }

    s << indent;
    s << ")";
}

void ForeachStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    ids->serialize(s, level + 1);
    s << endl;

    container->serialize(s, level + 1);
    s << endl;

    if (body) {

        body->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoBody>";
    }

    s << endl;
    s << indent;
    s << ")";
}

void ForStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    if (init) {

        init->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoInit>";
    }

    s << endl;

    if (cond) {

        cond->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoCond>";
    }

    s << endl;

    if (inc) {

        inc->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoInc>";
    }

    s << endl;

    if (body) {

        body->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoBody>";
    }

    s << endl;
    s << indent;
    s << ")";
}
