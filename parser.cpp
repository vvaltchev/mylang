/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"

/*
 * ----------------- Recursive Descent Parser -------------------
 *
 * Parse functions using the C Operator Precedence.
 * Note: this simple language has just no operators for several levels.
 */

unique_ptr<Construct> pExpr01(ParseContext &c); // ops: ()
unique_ptr<Construct> pExpr02(ParseContext &c); // ops: + (unary), - (unary), !
unique_ptr<Construct> pExpr03(ParseContext &c); // ops: *, /
unique_ptr<Construct> pExpr04(ParseContext &c); // ops: +, -
unique_ptr<Construct> pExpr06(ParseContext &c); // ops: <, >, <=, >=
unique_ptr<Construct> pExpr07(ParseContext &c); // ops: ==, !=
unique_ptr<Construct> pExpr14(ParseContext &c); // ops: =
unique_ptr<Construct> pStmt(ParseContext &c);   // expression statment e.g. "a = 3;"
unique_ptr<Construct> pBlock(ParseContext &c);  // code block { }

inline unique_ptr<Construct> pExprTop(ParseContext &c) { return pExpr14(c); }

bool pAcceptLiteralInt(ParseContext &c, unique_ptr<Construct> &v)
{
    if (*c == TokType::num) {
        v.reset(new LiteralInt{ stol(string(c.get_str())) });
        c++;
        return true;
    }

    return false;
}

bool pAcceptId(ParseContext &c, unique_ptr<Construct> &v)
{
    if (*c == TokType::id) {
        v.reset(new Identifier(c.get_str()));
        c++;
        return true;
    }

    return false;
}


bool pAcceptOp(ParseContext &c, Op exp)
{
    if (*c == exp) {
        c++;
        return true;
    }

    return false;
}

bool pAcceptKeyword(ParseContext &c, Keyword exp)
{
    if (*c == exp) {
        c++;
        return true;
    }

    return false;
}

void pExpectLiteralInt(ParseContext &c, unique_ptr<Construct> &v)
{
    if (!pAcceptLiteralInt(c, v))
        throw SyntaxErrorEx(c.get_loc(), "Expected integer literal");
}

void pExpectOp(ParseContext &c, Op exp)
{
    if (!pAcceptOp(c, exp))
        throw SyntaxErrorEx(c.get_loc(), "Expected operator", &c.get_tok(), exp);
}

Op AcceptOneOf(ParseContext &c, initializer_list<Op> list)
{
    for (auto op : list) {
        if (pAcceptOp(c, op))
            return op;
    }

    return Op::invalid;
}

unique_ptr<ExprList>
pExprList(ParseContext &c)
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
pAcceptCallExpr(ParseContext &c, unique_ptr<Construct> &id, unique_ptr<Construct> &ret)
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

void noExprError(ParseContext &c)
{
    throw SyntaxErrorEx(
        c.get_loc(),
        "Expected expression, got",
        &c.get_tok()
    );
}

unique_ptr<Construct>
pExpr01(ParseContext &c)
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

        return nullptr;
    }

    return ret;
}

template <class ExprT>
unique_ptr<Construct>
pExprGeneric(ParseContext &c,
             unique_ptr<Construct> (*lowerExpr)(ParseContext&),
             initializer_list<Op> ops)
{
    Op op;
    unique_ptr<ExprT> ret;
    unique_ptr<Construct> lowerE = lowerExpr(c);

    if (!lowerE)
        return nullptr;

    while ((op = AcceptOneOf(c, ops)) != Op::invalid) {

        if (!ret) {
            ret.reset(new ExprT);
            ret->elems.emplace_back(Op::invalid, move(lowerE));
        }

        lowerE = lowerExpr(c);

        if (!lowerE)
            noExprError(c);

        ret->elems.emplace_back(op, move(lowerE));
    }

    if (!ret)
        return lowerE;

    return ret;
}

unique_ptr<Construct>
pExpr02(ParseContext &c)
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

        if (!elem)
            noExprError(c);


    } else {

        elem = pExpr01(c);
    }

    if (!elem || op == Op::invalid)
        return elem;

    ret.reset(new Expr02);
    ret->elems.emplace_back(op, move(elem));
    return ret;
}

unique_ptr<Construct>
pExpr03(ParseContext &c)
{
    return pExprGeneric<Expr03>(
        c, pExpr02, {Op::times, Op::div, Op::mod}
    );
}

unique_ptr<Construct>
pExpr04(ParseContext &c)
{
    return pExprGeneric<Expr04>(
        c, pExpr03, {Op::plus, Op::minus}
    );
}

unique_ptr<Construct>
pExpr06(ParseContext &c)
{
    return pExprGeneric<Expr06>(
        c, pExpr04, {Op::lt, Op::gt, Op::le, Op::ge}
    );
}

unique_ptr<Construct>
pExpr07(ParseContext &c)
{
    return pExprGeneric<Expr07>(
        c, pExpr06, {Op::eq, Op::noteq}
    );
}

unique_ptr<Construct> pExpr14(ParseContext &c)
{
    unique_ptr<Construct> lside, e;
    Op op = Op::invalid;

    lside = pExpr07(c);

    if (!lside)
        return nullptr;

    if ((op = AcceptOneOf(c, {Op::assign})) != Op::invalid) {

        unique_ptr<Expr14> ret(new Expr14);

        ret->op = op;
        ret->lvalue = move(lside);
        ret->rvalue = pExpr14(c);

        if (!ret->rvalue)
            noExprError(c);

        return ret;

    } else {

        return lside;
    }
}

bool
pAcceptBracedBlock(ParseContext &c, unique_ptr<Construct> &ret)
{
    if (pAcceptOp(c, Op::braceL)) {
        ret = pBlock(c);
        pExpectOp(c, Op::braceR);
        return true;
    }

    return false;
}

bool
pAcceptIfStmt(ParseContext &c, unique_ptr<Construct> &ret)
{
    if (pAcceptKeyword(c, Keyword::kw_if)) {

        unique_ptr<IfStmt> ifstmt(new IfStmt);
        pExpectOp(c, Op::parenL);

        ifstmt->condExpr = pExprTop(c);

        if (!ifstmt->condExpr)
            noExprError(c);

        pExpectOp(c, Op::parenR);

        if (!pAcceptBracedBlock(c, ifstmt->thenBlock))
            ifstmt->thenBlock = pStmt(c);

        if (pAcceptKeyword(c, Keyword::kw_else)) {
            if (!pAcceptBracedBlock(c, ifstmt->elseBlock))
                ifstmt->elseBlock = pStmt(c);
        }

        ret = move(ifstmt);
        return true;
    }

    return false;
}

unique_ptr<Construct>
pStmt(ParseContext &c)
{
    unique_ptr<Construct> subStmt;

    if (pAcceptIfStmt(c, subStmt)) {

        return subStmt;

    } else {

        unique_ptr<Construct> lowerE = pExprTop(c);

        if (!lowerE)
            return nullptr;

        unique_ptr<Stmt> ret(new Stmt);
        ret->elem = move(lowerE);
        pExpectOp(c, Op::semicolon);
        return ret;
    }
}

unique_ptr<Construct>
pBlock(ParseContext &c)
{
    unique_ptr<Block> ret(new Block);
    unique_ptr<Construct> stmt;

    if (!c.eoi()) {

        while ((stmt = pStmt(c)))
            ret->elems.emplace_back(move(stmt));

        while (*c == Op::semicolon)
            c++;    /* skip multiple ';' */
    }

    return ret;
}
