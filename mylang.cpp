/* SPDX-License-Identifier: BSD-2-Clause */

#include "defs.h"

#include <map>
#include <list>
#include <array>
#include <tuple>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

class TokenStream {

private:

    typename vector<Tok>::const_iterator pos;
    typename vector<Tok>::const_iterator end;

public:

    TokenStream(const vector<Tok> &tokens)
        : pos(tokens.cbegin())
        , end(tokens.cend()) { }

    Tok get() const {

        if (pos != end)
            return *pos;

        return Tok();
    }

    void next() {

        if (pos != end)
            pos++;
    }
};

class Context {

public:
    TokenStream ts;

    /* token operations */
    Tok operator*() const { return ts.get(); }
    Op get_op() const { return ts.get().op; }
    string_view get_str() const { return ts.get().value; }

    /* token operations with side-effect */
    Tok operator++(int dummy) { Tok val = ts.get(); ts.next(); return val; }
    void next() { ts.next(); }
};

// ----------- Recursive Descent Parser -------------------

class LiteralInt;
class Factor;
class Term;
class Expr;

class EvalValue {

public:
    long value;

    EvalValue(long v) : value(v) { }
};

class EvalContext {

public:
};

class Construct {

public:
    Construct() = default;
    virtual ~Construct() = default;
    virtual EvalValue eval(EvalContext *) const = 0;
    virtual void serialize(ostream &s, int level = 0) const = 0;
};

ostream &operator<<(ostream &s, const Construct &c)
{
    c.serialize(s);
    return s;
}


class LiteralInt : public Construct {

public:
    long value;

    LiteralInt(long v) : value(v) { }

    virtual ~LiteralInt() = default;

    virtual EvalValue eval(EvalContext *ctx) const {
        return value;
    }

    virtual void serialize(ostream &s, int level = 0) const {

        string indent(level * 2, ' ');

        s << indent;
        s << string("Int(");
        s << to_string(value);
        s << ")";
    }
};


class Factor : public Construct {

public:
    Construct *value;

    virtual ~Factor() = default;

    virtual EvalValue eval(EvalContext *ctx) const {
        return value->eval(ctx);
    }

    virtual void serialize(ostream &s, int level = 0) const {

        string indent(level * 2, ' ');

        s << indent;
        s << "Factor(" << endl;
        value->serialize(s, level + 1);
        s << endl;
        s << string(level * 2, ' ');
        s << ")";
    }
};

class Term : public Construct {

public:
    vector<pair<Op, Factor *>> factors;

    virtual ~Term() = default;

    virtual EvalValue eval(EvalContext *ctx) const {

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

    virtual void serialize(ostream &s, int level = 0) const {

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
};

class Expr : public Construct {

public:
    vector<pair<Op, Term *>> terms;

    virtual ~Expr() = default;

    virtual EvalValue eval(EvalContext *ctx) const {

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

    virtual void serialize(ostream &s, int level = 0) const {

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
};

bool pAcceptLiteralInt(Context &c, Construct *&v);
bool pAcceptOp(Context &c, Op exp);
void pExpectLiteralInt(Context &c, Construct *&v);
void pExpectOp(Context &c, Op exp);

Factor *pFactor(Context &c);
Term *pTerm(Context &c);
Expr *pExpr(Context &c);

bool pAcceptLiteralInt(Context &c, Construct *&v)
{
    if (*c == TokType::num) {
        v = new LiteralInt{ stol(string(c.get_str())) };
        c++;
        return true;
    }

    return false;
}

bool pAcceptOp(Context &c, Op exp)
{
    if (*c == exp) {
        c++;
        return true;
    }

    return false;
}

void pExpectLiteralInt(Context &c, Construct *&v)
{
    if (!pAcceptLiteralInt(c, v))
        throw SyntaxErrorEx();
}

void pExpectOp(Context &c, Op exp)
{
    if (!pAcceptOp(c, exp))
        throw SyntaxErrorEx();
}

Factor *pFactor(Context &c)
{
    Factor *ret = new Factor;
    Construct *v;

    if (pAcceptLiteralInt(c, v)) {

        ret->value = v;

    } else if (pAcceptOp(c, Op::parenL)) {

        ret->value = pExpr(c);
        pExpectOp(c, Op::parenR);

    } else {

        throw SyntaxErrorEx();
    }

    return ret;
}

Term *pTerm(Context &c)
{
    Term *ret = new Term;
    Factor *f;

    f = pFactor(c);
    ret->factors.emplace_back(Op::invalid, f);

    while (*c == Op::times || *c == Op::div) {
        Op op = c.get_op();
        c++;
        f = pFactor(c);
        ret->factors.emplace_back(op, f);
    }

    return ret;
}

Expr *pExpr(Context &c)
{
    Expr *e = new Expr();
    Op op = Op::invalid;
    Term *t;

    if (*c == Op::plus || *c == Op::minus) {
        op = c.get_op();
        c++;
    }

    t = pTerm(c);
    e->terms.emplace_back(op, t);

    while (*c == Op::plus || *c == Op::minus) {
        op = c.get_op();
        c++;
        t = pTerm(c);
        e->terms.emplace_back(op, t);
    }

    return e;
}

// ----------- Recursive Descent Parser [end] -------------

void help()
{
    cout << "syntax:" << endl;
    cout << "   mylang < FILE" << endl;
    cout << "   mylang -e EXPRESSION" << endl;
    cout << endl;
}

void
parse_args(int argc,
           char **argv,
           vector<string> &lines,
           vector<Tok> &tokens)
{
    if (argc >= 3) {

        string a1 = argv[1];

        if (a1 == "-e") {

            lines.emplace_back(argv[2]);
            lexer(lines[0], tokens);

        } else {

            help();
            exit(1);
        }

    } else if (argc > 1) {

        string a1 = argv[1];

        if (a1 == "-h" || a1 == "--help") {
            help();
            exit(0);
        }
    }
}

void read_input(vector<string>& lines, vector<Tok> &tokens)
{
    if (tokens.empty()) {

        string line;

        while (getline(cin, line))
            lines.push_back(move(line));

        for (const auto &s: lines)
            lexer(s, tokens);

        if (tokens.empty()) {
            help();
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    vector<string> lines;
    vector<Tok> tokens;

    try {

        parse_args(argc, argv, lines, tokens);
        read_input(lines, tokens);

        Context ctx{TokenStream(tokens)};

        if (0) {
            cout << "Tokens" << endl;
            cout << "--------------------------" << endl;

            for (const auto &tok : tokens) {
                cout << tok << endl;
            }
        }

        cout << "Syntax tree" << endl;
        cout << "--------------------------" << endl;

        Construct *c = pExpr(ctx);
        cout << *c << endl;

        cout << endl;
        cout << "Value" << endl;
        cout << "--------------------------" << endl;

        cout << c->eval(nullptr).value << endl;


    } catch (InvalidTokenEx e) {

        cout << "Invalid token: " << e.val << endl;

    } catch (SyntaxErrorEx e) {

        cout << "SyntaxError" << endl;

    } catch (DivisionByZeroEx e) {

        cout << "DivisionByZeroEx" << endl;
    }

    return 0;
}