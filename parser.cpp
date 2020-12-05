/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "eval.h"

ParseContext::ParseContext(const TokenStream &ts)
    : ts(ts)
    , const_ctx(new EvalContext(true))
{

}

/*
 * ----------------- Recursive Descent Parser -------------------
 *
 * Parse functions using the C Operator Precedence.
 * Note: this simple language has just no operators for several levels.
 */

enum pFlags : unsigned {

    pNone           = 1 << 0,
    pInDecl         = 1 << 1,
    pInConstDecl    = 1 << 2,
    pInLoop         = 1 << 3,
};

unique_ptr<Construct> pExpr01(ParseContext &c); // ops: ()
unique_ptr<Construct> pExpr02(ParseContext &c); // ops: + (unary), - (unary), !
unique_ptr<Construct> pExpr03(ParseContext &c); // ops: *, /
unique_ptr<Construct> pExpr04(ParseContext &c); // ops: +, -
unique_ptr<Construct> pExpr06(ParseContext &c); // ops: <, >, <=, >=
unique_ptr<Construct> pExpr07(ParseContext &c); // ops: ==, !=
unique_ptr<Construct> pExpr11(ParseContext &c); // ops: &&
unique_ptr<Construct> pExpr12(ParseContext &c); // ops: ||
unique_ptr<Construct> pExpr14(ParseContext &c, unsigned fl);  // ops: =
unique_ptr<Construct> pExprTop(ParseContext &c, unsigned fl = pFlags::pNone);
unique_ptr<Construct> pStmt(ParseContext &c, unsigned fl);

bool
pAcceptIfStmt(ParseContext &c,
              unique_ptr<Construct> &ret,
              unsigned fl = pFlags::pNone);

bool
pAcceptWhileStmt(ParseContext &c, unique_ptr<Construct> &ret);

unique_ptr<Construct>
MakeConstructFromConstVal(EvalValue v);

bool
pAcceptLiteralInt(ParseContext &c, unique_ptr<Construct> &v)
{
    if (*c == TokType::num) {
        v.reset(new LiteralInt{ stol(string(c.get_str())) });
        c++;
        return true;
    }

    return false;
}

bool
pAcceptId(ParseContext &c, unique_ptr<Construct> &v, bool resolve_const = true)
{
    if (*c == TokType::id) {

        v.reset(new Identifier(c.get_str()));

        if (resolve_const) {

            EvalValue const_value = v->eval(c.const_ctx);

            if (const_value.type->t == Type::t_lval) {
                v = MakeConstructFromConstVal(RValue(const_value));
            }
        }

        c++;
        return true;
    }

    return false;
}

bool
pAcceptOp(ParseContext &c, Op exp)
{
    if (*c == exp) {
        c++;
        return true;
    }

    return false;
}

bool
pAcceptKeyword(ParseContext &c, Keyword exp)
{
    if (*c == exp) {
        c++;
        return true;
    }

    return false;
}

void
pExpectLiteralInt(ParseContext &c, unique_ptr<Construct> &v)
{
    if (!pAcceptLiteralInt(c, v))
        throw SyntaxErrorEx(c.get_loc(), "Expected integer literal");
}

void pExpectOp(ParseContext &c, Op exp)
{
    if (!pAcceptOp(c, exp))
        throw SyntaxErrorEx(c.get_loc(), "Expected operator", &c.get_tok(), exp);
}

Op
AcceptOneOf(ParseContext &c, initializer_list<Op> list)
{
    for (auto op : list) {
        if (pAcceptOp(c, op))
            return op;
    }

    return Op::invalid;
}

void
noExprError(ParseContext &c)
{
    throw SyntaxErrorEx(
        c.get_loc(),
        "Expected expression, got",
        &c.get_tok()
    );
}

unique_ptr<ExprList>
pExprList(ParseContext &c)
{
    unique_ptr<ExprList> ret(new ExprList);
    unique_ptr<Construct> subexpr;

    subexpr = pExprTop(c);

    if (subexpr) {

        ret->elems.emplace_back(move(subexpr));

        while (*c == Op::comma) {

            c++;
            subexpr = pExprTop(c);

            if (!subexpr)
                noExprError(c);

            ret->elems.emplace_back(move(subexpr));
        }
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

unique_ptr<Construct>
pExpr01(ParseContext &c)
{
    unique_ptr<Construct> ret;
    unique_ptr<Construct> main;
    unique_ptr<Construct> callExpr;

    if (pAcceptLiteralInt(c, main)) {

        ret = move(main);
        ret->is_const = true;

    } else if (pAcceptOp(c, Op::parenL)) {

        unique_ptr<Expr01> expr(new Expr01);
        expr->elem = pExprTop(c);

        if (!expr->elem)
            noExprError(c);

        if (expr->elem->is_const)
            expr->is_const = true;

        pExpectOp(c, Op::parenR);
        ret = move(expr);

    } else if (pAcceptId(c, main)) {

        if (!main->is_const && pAcceptCallExpr(c, main, callExpr))
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
    bool is_const;

    if (!lowerE)
        return nullptr;

    is_const = lowerE->is_const;

    while ((op = AcceptOneOf(c, ops)) != Op::invalid) {

        if (!ret) {
            ret.reset(new ExprT);
            ret->elems.emplace_back(Op::invalid, move(lowerE));
        }

        lowerE = lowerExpr(c);

        if (!lowerE)
            noExprError(c);

        is_const = is_const && lowerE->is_const;
        ret->elems.emplace_back(op, move(lowerE));
    }

    if (!ret)
        return lowerE;

    ret->is_const = is_const;
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
    ret->is_const = elem->is_const;
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

unique_ptr<Construct>
pExpr14(ParseContext &c, unsigned fl)
{
    static const initializer_list<Op> valid_ops = {
        Op::assign, Op::addeq, Op::subeq, Op::muleq, Op::diveq, Op::modeq
    };

    unique_ptr<Construct> lside;
    Op op = Op::invalid;

    if (fl & pFlags::pInConstDecl) {

        if (!pAcceptId(c, lside, false /* resolve_const */)) {

           /*
            * If the current statement is a const declaration, require
            * strictly an identifier instead of a generic expression.
            */

            throw SyntaxErrorEx(
                c.get_loc(),
                "Expected identifier, got",
                &c.get_tok()
            );
        }

    } else {

        lside = pExpr12(c);
    }

    if (!lside || (op = AcceptOneOf(c, valid_ops)) == Op::invalid) {

        /*
         * An empty statement or any expression that's just not an
         * assignment expression. Return the sub-expression.
         */
        return lside;
    }

    unique_ptr<Expr14> ret(new Expr14);

    ret->op = op;
    ret->lvalue = move(lside);
    ret->rvalue = pExprTop(c);

    if (!ret->rvalue)
        noExprError(c);

    if (ret->rvalue->is_const) {
        ret->rvalue = MakeConstructFromConstVal(
            RValue(ret->rvalue->eval(c.const_ctx))
        );
    }

    if (fl & pFlags::pInConstDecl) {

        if (op != Op::assign)
            throw ConstNotAllowedEx{c.get_loc()};

        if (!ret->rvalue->is_const)
            throw ExpressionIsNotConstEx{c.get_loc()};

        try {

            /*
             * Save the const declaration by evaluating the assignment
             * in our special `const_ctx` EvalContext. In case a const
             * declaration related to the same symbol already exists,
             * we'll get a CannotRebindConstEx exception.
             */
            ret->eval(c.const_ctx);
            return move(ret->rvalue);

        } catch (CannotRebindConstEx) {
            throw CannotRebindConstEx{c.get_loc()};
        }
    }

    return ret;
}

unique_ptr<Construct>
pExprTop(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> e = pExpr14(c, fl);

    if (e && e->is_const) {
        EvalValue v = e->eval(c.const_ctx);
        return MakeConstructFromConstVal(v);
    }

    return e;
}

unique_ptr<Construct>
pStmt(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> subStmt;

    if (fl & pFlags::pInLoop) {

        if (pAcceptKeyword(c, Keyword::kw_break))
            return make_unique<BreakStmt>();
        else if (pAcceptKeyword(c, Keyword::kw_continue))
            return make_unique<ContinueStmt>();
    }

    if (pAcceptIfStmt(c, subStmt, fl)) {

        return subStmt;

    } if (pAcceptWhileStmt(c, subStmt)) {

        return subStmt;

    } else {

        if (pAcceptKeyword(c, Keyword::kw_const))
            fl |= pFlags::pInDecl | pFlags::pInConstDecl;

        unique_ptr<Construct> lowerE = pExprTop(c, fl);

        if (!lowerE)
            return nullptr;

        unique_ptr<Stmt> ret(new Stmt);
        ret->elem = move(lowerE);
        pExpectOp(c, Op::semicolon);
        return ret;
    }
}

unique_ptr<Construct>
pBlock(ParseContext &c, unsigned fl)
{
    unique_ptr<Block> ret(new Block);
    unique_ptr<Construct> stmt;

    if (!c.eoi()) {

        while ((stmt = pStmt(c, fl))) {

            ret->elems.emplace_back(move(stmt));

            while (*c == Op::semicolon)
                c++;    /* skip multiple ';' */
        }
    }

    return ret;
}

bool
pAcceptBracedBlock(ParseContext &c,
                   unique_ptr<Construct> &ret,
                   unsigned fl = pFlags::pNone)
{
    if (pAcceptOp(c, Op::braceL)) {
        ret = pBlock(c, fl);
        pExpectOp(c, Op::braceR);
        return true;
    }

    return false;
}

bool
pAcceptIfStmt(ParseContext &c, unique_ptr<Construct> &ret, unsigned fl)
{
    if (pAcceptKeyword(c, Keyword::kw_if)) {

        unique_ptr<IfStmt> ifstmt(new IfStmt);
        pExpectOp(c, Op::parenL);

        ifstmt->condExpr = pExprTop(c);

        if (!ifstmt->condExpr)
            noExprError(c);

        pExpectOp(c, Op::parenR);

        if (!pAcceptBracedBlock(c, ifstmt->thenBlock, fl))
            ifstmt->thenBlock = pStmt(c, fl);

        if (pAcceptKeyword(c, Keyword::kw_else)) {
            if (!pAcceptBracedBlock(c, ifstmt->elseBlock, fl))
                ifstmt->elseBlock = pStmt(c, fl);
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

        if (!pAcceptBracedBlock(c, whileStmt->body, pFlags::pInLoop))
            whileStmt->body = pStmt(c, pFlags::pInLoop);

        ret = move(whileStmt);
        return true;
    }

    return false;
}

unique_ptr<Construct>
MakeConstructFromConstVal(EvalValue v)
{
    if (v.is<long>())
        return make_unique<LiteralInt>(v.get<long>());

    throw InternalErrorEx();
}
