/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "evalvalue.h"

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
unique_ptr<Construct> pExpr11(ParseContext &c); // ops: &&
unique_ptr<Construct> pExpr12(ParseContext &c); // ops: ||
unique_ptr<Construct> pExpr14(ParseContext &c); // ops: =
unique_ptr<Construct> pStmt(ParseContext &c, bool loop = false);
unique_ptr<Construct> pExprTop(ParseContext &c);

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
pAcceptCallExpr(ParseContext &c,
                unique_ptr<Construct> &id,
                unique_ptr<Construct> &ret)
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
    unique_ptr<Construct> main;
    unique_ptr<Construct> callExpr;

    if (pAcceptLiteralInt(c, main)) {

        ret = move(main);
        ret->is_literal = true;

    } else if (pAcceptOp(c, Op::parenL)) {

        unique_ptr<Expr01> expr(new Expr01);
        expr->elem = pExprTop(c);

        if (!expr->elem)
            noExprError(c);

        if (expr->elem->is_literal)
            expr->is_literal = true;

        pExpectOp(c, Op::parenR);
        ret = move(expr);

    } else if (pAcceptId(c, main)) {

        if (pAcceptCallExpr(c, main, callExpr))
            ret = move(callExpr);
        else
            ret = move(main);

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
    bool is_literal;

    if (!lowerE)
        return nullptr;

    is_literal = lowerE->is_literal;

    while ((op = AcceptOneOf(c, ops)) != Op::invalid) {

        if (!ret) {
            ret.reset(new ExprT);
            ret->elems.emplace_back(Op::invalid, move(lowerE));
        }

        lowerE = lowerExpr(c);

        if (!lowerE)
            noExprError(c);

        is_literal = is_literal && lowerE->is_literal;
        ret->elems.emplace_back(op, move(lowerE));
    }

    if (!ret)
        return lowerE;

    ret->is_literal = is_literal;
    return ret;
}

unique_ptr<Construct>
pExpr02(ParseContext &c)
{
    unique_ptr<Expr02> ret;
    unique_ptr<Construct> elem;
    Op op = AcceptOneOf(c, {Op::plus, Op::minus, Op::lnot});

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
    ret->is_literal = elem->is_literal;
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

unique_ptr<Construct>
pExpr11(ParseContext &c)
{
    return pExprGeneric<Expr11>(
        c, pExpr07, {Op::land}
    );
}

unique_ptr<Construct>
pExpr12(ParseContext &c)
{
    return pExprGeneric<Expr12>(
        c, pExpr11, {Op::lor}
    );
}

unique_ptr<Construct> pExpr14(ParseContext &c)
{
    static const initializer_list<Op> valid_ops = {
        Op::assign, Op::addeq, Op::subeq, Op::muleq, Op::diveq, Op::modeq
    };

    unique_ptr<Construct> lside, e;
    Op op = Op::invalid;

    lside = pExpr12(c);

    if (!lside)
        return nullptr;

    if ((op = AcceptOneOf(c, valid_ops)) != Op::invalid) {

        unique_ptr<Expr14> ret(new Expr14);

        ret->op = op;
        ret->lvalue = move(lside);
        ret->rvalue = pExprTop(c);

        if (!ret->rvalue)
            noExprError(c);

        return ret;

    } else {

        return lside;
    }
}

unique_ptr<Construct> pExprTop(ParseContext &c) {

    unique_ptr<Construct> e = pExpr14(c);

    if (e && e->is_literal) {

        EvalValue v = e->eval(nullptr);

        if (v.is<long>())
            return make_unique<LiteralInt>(v.get<long>());
    }

    return e;
}

unique_ptr<Construct>
pBlock(ParseContext &c, bool loop)
{
    unique_ptr<Block> ret(new Block);
    unique_ptr<Construct> stmt;

    if (!c.eoi()) {

        while ((stmt = pStmt(c, loop)))
            ret->elems.emplace_back(move(stmt));

        while (*c == Op::semicolon)
            c++;    /* skip multiple ';' */
    }

    return ret;
}

bool
pAcceptBracedBlock(ParseContext &c,
                   unique_ptr<Construct> &ret,
                   bool loop)
{
    if (pAcceptOp(c, Op::braceL)) {
        ret = pBlock(c, loop);
        pExpectOp(c, Op::braceR);
        return true;
    }

    return false;
}

bool
pAcceptIfStmt(ParseContext &c, unique_ptr<Construct> &ret, bool loop)
{
    if (pAcceptKeyword(c, Keyword::kw_if)) {

        unique_ptr<IfStmt> ifstmt(new IfStmt);
        pExpectOp(c, Op::parenL);

        ifstmt->condExpr = pExprTop(c);

        if (!ifstmt->condExpr)
            noExprError(c);

        pExpectOp(c, Op::parenR);

        if (!pAcceptBracedBlock(c, ifstmt->thenBlock, loop))
            ifstmt->thenBlock = pStmt(c);

        if (pAcceptKeyword(c, Keyword::kw_else)) {
            if (!pAcceptBracedBlock(c, ifstmt->elseBlock, loop))
                ifstmt->elseBlock = pStmt(c);
        }

        ret = move(ifstmt);
        return true;
    }

    return false;
}

bool
pAcceptWhileStmt(ParseContext &c, unique_ptr<Construct> &ret)
{
    if (pAcceptKeyword(c, Keyword::kw_while)) {

        unique_ptr<WhileStmt> whileStmt(new WhileStmt);
        pExpectOp(c, Op::parenL);

        whileStmt->condExpr = pExprTop(c);

        if (!whileStmt->condExpr)
            noExprError(c);

        pExpectOp(c, Op::parenR);

        if (!pAcceptBracedBlock(c, whileStmt->body, true))
            whileStmt->body = pStmt(c);

        ret = move(whileStmt);
        return true;
    }

    return false;
}

unique_ptr<Construct>
pStmt(ParseContext &c, bool loop)
{
    unique_ptr<Construct> subStmt;

    if (loop) {

        if (pAcceptKeyword(c, Keyword::kw_break))
            return make_unique<BreakStmt>();
        else if (pAcceptKeyword(c, Keyword::kw_continue))
            return make_unique<ContinueStmt>();
    }

    if (pAcceptIfStmt(c, subStmt, loop)) {

        return subStmt;

    } if (pAcceptWhileStmt(c, subStmt)) {

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
