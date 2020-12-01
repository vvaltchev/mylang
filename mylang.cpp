/* SPDX-License-Identifier: BSD-2-Clause */

#include "defs.h"
#include "syntax.h"

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