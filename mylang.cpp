/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include <initializer_list>

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
    bool eoi() const { return ts.get() == TokType::invalid; }

    /* token operations with side-effect */
    Tok operator++(int) { Tok val = ts.get(); ts.next(); return val; }
    void next() { ts.next(); }
};

// ----------- Recursive Descent Parser -------------------

bool pAcceptLiteralInt(Context &c, Construct *&v);
bool pAcceptOp(Context &c, Op exp);
void pExpectLiteralInt(Context &c, Construct *&v);
void pExpectOp(Context &c, Op exp);

/*
 * Parse functions using the C Operator Precedence.
 * Note: this simple language has just no operators for several levels.
 */

unique_ptr<Construct> pExpr01(Context &c); // ops: ()
unique_ptr<Construct> pExpr02(Context &c); // ops: + (unary), - (unary), !
unique_ptr<Construct> pExpr03(Context &c); // ops: *, /
unique_ptr<Construct> pExpr04(Context &c); // ops: +, -
unique_ptr<Construct> pExpr06(Context &c); // ops: <, >, <=, >=
unique_ptr<Construct> pExpr07(Context &c); // ops: ==, !=
unique_ptr<Construct> pExpr14(Context &c); // ops: =


inline unique_ptr<Construct> pExprTop(Context &c) { return pExpr14(c); }

bool pAcceptLiteralInt(Context &c, unique_ptr<Construct> &v)
{
    if (*c == TokType::num) {
        v.reset(new LiteralInt{ stol(string(c.get_str())) });
        c++;
        return true;
    }

    return false;
}

bool pAcceptId(Context &c, unique_ptr<Construct> &v)
{
    if (*c == TokType::id) {
        v.reset(new Identifier(c.get_str()));
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

void pExpectLiteralInt(Context &c, unique_ptr<Construct> &v)
{
    if (!pAcceptLiteralInt(c, v))
        throw SyntaxErrorEx();
}

void pExpectOp(Context &c, Op exp)
{
    if (!pAcceptOp(c, exp))
        throw SyntaxErrorEx();
}

Op AcceptOneOf(Context &c, initializer_list<Op> list)
{
    for (auto op : list) {
        if (pAcceptOp(c, op))
            return op;
    }

    return Op::invalid;
}

unique_ptr<ExprList>
pExprList(Context &c)
{
    unique_ptr<ExprList> ret(new ExprList);

    if (*c == Op::parenR)
        return ret;

    ret->elems.emplace_back(pExprTop(c));

    while (*c == Op::comma) {

        c++;
        ret->elems.emplace_back(pExprTop(c));
    }

    return ret;
}

bool
pAcceptCallExpr(Context &c, unique_ptr<Construct> &id, unique_ptr<Construct> &ret)
{
    if (pAcceptOp(c, Op::parenL)) {

        unique_ptr<CallExpr> expr(new CallExpr);

        expr->id.reset(static_cast<Identifier *>(id.release()));
        expr->args = pExprList(c);
        ret = move(expr);
        pExpectOp(c, Op::parenR);
        return true;
    }

    return false;
}

unique_ptr<Construct>
pExpr01(Context &c)
{
    unique_ptr<Construct> ret;
    unique_ptr<Construct> e, e2;

    if (pAcceptLiteralInt(c, e)) {

        ret = move(e);

    } else if (pAcceptOp(c, Op::parenL)) {

        unique_ptr<Expr01> expr(new Expr01);
        expr->elem = pExprTop(c);
        pExpectOp(c, Op::parenR);
        ret = move(expr);

    } else if (pAcceptId(c, e)) {

        if (pAcceptCallExpr(c, e, e2))
            ret = move(e2);
        else
            ret = move(e);

    } else {

        throw SyntaxErrorEx();
    }

    return ret;
}

template <class ExprT>
unique_ptr<Construct>
pExprGeneric(Context &c,
             unique_ptr<Construct> (*lowerExpr)(Context&),
             initializer_list<Op> ops)
{
    Op op;
    unique_ptr<ExprT> ret;
    unique_ptr<Construct> lowerE = lowerExpr(c);

    while ((op = AcceptOneOf(c, ops)) != Op::invalid) {

        if (!ret) {
            ret.reset(new ExprT);
            ret->elems.emplace_back(Op::invalid, move(lowerE));
        }

        ret->elems.emplace_back(op, lowerExpr(c));
    }

    if (!ret)
        return lowerE;

    return ret;
}

unique_ptr<Construct>
pExpr02(Context &c)
{
    unique_ptr<Expr02> ret;
    unique_ptr<Construct> elem;
    Op op = AcceptOneOf(c, {Op::plus, Op::minus, Op::opnot});

    if (op != Op::invalid) {

        /*
         * Note: using direct recursion.
         * It allows us to handle things like:
         *
         *      --1, !+1, !-1, !!1
         */

        elem = pExpr02(c);

    } else {

        elem = pExpr01(c);
    }

    if (op == Op::invalid)
        return elem;

    ret.reset(new Expr02);
    ret->elems.emplace_back(op, move(elem));
    return ret;
}

unique_ptr<Construct>
pExpr03(Context &c)
{
    return pExprGeneric<Expr03>(
        c, pExpr02, {Op::times, Op::div, Op::mod}
    );
}

unique_ptr<Construct>
pExpr04(Context &c)
{
    return pExprGeneric<Expr04>(
        c, pExpr03, {Op::plus, Op::minus}
    );
}

unique_ptr<Construct>
pExpr06(Context &c)
{
    return pExprGeneric<Expr06>(
        c, pExpr04, {Op::lt, Op::gt, Op::le, Op::ge}
    );
}

unique_ptr<Construct>
pExpr07(Context &c)
{
    return pExprGeneric<Expr07>(
        c, pExpr06, {Op::eq, Op::noteq}
    );
}

unique_ptr<Construct> pExpr14(Context &c)
{
    unique_ptr<Construct> lside, e;
    Op op = Op::invalid;

    lside = pExpr07(c);

    if ((op = AcceptOneOf(c, {Op::assign})) != Op::invalid) {

        unique_ptr<Expr14> ret(new Expr14);

        ret->lvalue = move(lside);
        ret->rvalue = pExpr07(c);
        ret->op = op;
        return ret;

    } else {

        return lside;
    }
}

unique_ptr<Construct>
pStmt(Context &c)
{
    unique_ptr<Stmt> ret(new Stmt);
    ret->elem = pExprTop(c);
    return ret;
}

unique_ptr<Construct>
pBlock(Context &c)
{
    unique_ptr<Block> ret(new Block);

    if (c.eoi())
        return ret;

    ret->elems.emplace_back(pStmt(c));

    while (pAcceptOp(c, Op::semicolon) && !c.eoi()) {
        ret->elems.emplace_back(pStmt(c));
    }

    return ret;
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

            if (tokens.empty())
                tokens.emplace_back();

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

        if (1) {
            cout << "Tokens" << endl;
            cout << "--------------------------" << endl;

            for (const auto &tok : tokens) {
                cout << tok << endl;
            }
        }

        cout << "Syntax tree" << endl;
        cout << "--------------------------" << endl;

        unique_ptr<Construct> root(pBlock(ctx));

        if (!ctx.eoi())
            throw SyntaxErrorEx();

        cout << *root << endl;
        cout << endl;

        /* Eval the script */
        EvalContext evalCtx;
        root->eval(&evalCtx);

    } catch (InvalidTokenEx e) {

        cout << "Invalid token: " << e.val << endl;

    } catch (SyntaxErrorEx e) {

        cout << "SyntaxError" << endl;

    } catch (DivisionByZeroEx e) {

        cout << "DivisionByZeroEx" << endl;

    } catch (UndefinedVariableEx e) {

        cout << "Undefined variable '" << e.name << "'" << endl;
    }

    return 0;
}
