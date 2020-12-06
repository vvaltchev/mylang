/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "eval.h"

ParseContext::ParseContext(const TokenStream &ts, bool const_eval)
    : ts(ts)
    , const_eval(const_eval)
    , const_ctx(new EvalContext(true))
{

}

/*
 * ----------------- Recursive Descent Parser -------------------
 *
 * Parse functions using the C Operator Precedence.
 * Note: this simple language has just no operators for several levels.
 */

unique_ptr<Construct> pExpr01(ParseContext &c, unsigned fl); // ops: ()
unique_ptr<Construct> pExpr02(ParseContext &c, unsigned fl); // ops: + (unary), - (unary), !
unique_ptr<Construct> pExpr03(ParseContext &c, unsigned fl); // ops: *, /
unique_ptr<Construct> pExpr04(ParseContext &c, unsigned fl); // ops: +, -
unique_ptr<Construct> pExpr06(ParseContext &c, unsigned fl); // ops: <, >, <=, >=
unique_ptr<Construct> pExpr07(ParseContext &c, unsigned fl); // ops: ==, !=
unique_ptr<Construct> pExpr11(ParseContext &c, unsigned fl); // ops: &&
unique_ptr<Construct> pExpr12(ParseContext &c, unsigned fl); // ops: ||
unique_ptr<Construct> pExpr14(ParseContext &c, unsigned fl); // ops: = (assignment)
unique_ptr<Construct> pExpr15(ParseContext &c, unsigned fl); // ops: , (comma operator)

unique_ptr<Construct> pExprTop(ParseContext &c, unsigned fl);
unique_ptr<Construct> pStmt(ParseContext &c, unsigned fl);

bool
pAcceptIfStmt(ParseContext &c,
              unique_ptr<Construct> &ret,
              unsigned fl);

bool
pAcceptWhileStmt(ParseContext &c,
                 unique_ptr<Construct> &ret,
                 unsigned fl);

unique_ptr<Construct>
MakeConstructFromConstVal(const EvalValue &v);

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
pAcceptLiteralStr(ParseContext &c, unique_ptr<Construct> &v)
{
    if (*c == TokType::str) {
        v.reset(new LiteralStr(c.get_str()));
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

        if (c.const_eval && resolve_const) {

            EvalValue const_value = v->eval(c.const_ctx);

            if (const_value.get_type()->t == Type::t_lval) {
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
pExprList(ParseContext &c, unsigned fl)
{
    unique_ptr<ExprList> ret(new ExprList);
    unique_ptr<Construct> subexpr;

    subexpr = pExpr14(c, fl);

    if (subexpr) {

        ret->elems.emplace_back(move(subexpr));

        while (*c == Op::comma) {

            c++;
            subexpr = pExpr14(c, fl);

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
                unique_ptr<Construct> &ret,
                unsigned fl)
{
    if (pAcceptOp(c, Op::parenL)) {

        unique_ptr<CallExpr> expr(new CallExpr);

        expr->id.reset(static_cast<Identifier *>(id.release()));
        expr->args = pExprList(c, fl);
        ret = move(expr);
        pExpectOp(c, Op::parenR);
        return true;
    }

    return false;
}

unique_ptr<Construct>
pExpr01(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> ret;
    unique_ptr<Construct> main;
    unique_ptr<Construct> callExpr;

    if (pAcceptLiteralInt(c, main)) {

        ret = move(main);
        ret->is_const = true;

    } else if (pAcceptLiteralStr(c, main)) {

        ret = move(main);
        ret->is_const = true;

    } else if (pAcceptKeyword(c, Keyword::kw_none)) {

        ret.reset(new LiteralNone());

    } else if (pAcceptOp(c, Op::parenL)) {

        ret = pExprTop(c, fl);

        if (!ret)
            noExprError(c);

        pExpectOp(c, Op::parenR);

    } else if (pAcceptId(c, main)) {

        if (!main->is_const && pAcceptCallExpr(c, main, callExpr, fl))
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
             unique_ptr<Construct> (*lowerExpr)(ParseContext&, unsigned),
             initializer_list<Op> ops,
             unsigned fl)
{
    Op op;
    unique_ptr<ExprT> ret;
    unique_ptr<Construct> lowerE = lowerExpr(c, fl);
    bool is_const;

    if (!lowerE)
        return nullptr;

    is_const = lowerE->is_const;

    while ((op = AcceptOneOf(c, ops)) != Op::invalid) {

        if (!ret) {
            ret.reset(new ExprT);
            ret->elems.emplace_back(Op::invalid, move(lowerE));
        }

        lowerE = lowerExpr(c, fl);

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
pExpr02(ParseContext &c, unsigned fl)
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

        elem = pExpr02(c, fl);

        if (!elem)
            noExprError(c);

    } else {

        elem = pExpr01(c, fl);
    }

    if (!elem || op == Op::invalid)
        return elem;

    ret.reset(new Expr02);
    ret->is_const = elem->is_const;
    ret->elems.emplace_back(op, move(elem));
    return ret;
}

unique_ptr<Construct>
pExpr03(ParseContext &c, unsigned fl)
{
    return pExprGeneric<Expr03>(
        c, pExpr02, {Op::times, Op::div, Op::mod}, fl
    );
}

unique_ptr<Construct>
pExpr04(ParseContext &c, unsigned fl)
{
    return pExprGeneric<Expr04>(
        c, pExpr03, {Op::plus, Op::minus}, fl
    );
}

unique_ptr<Construct>
pExpr06(ParseContext &c, unsigned fl)
{
    return pExprGeneric<Expr06>(
        c, pExpr04, {Op::lt, Op::gt, Op::le, Op::ge}, fl
    );
}

unique_ptr<Construct>
pExpr07(ParseContext &c, unsigned fl)
{
    return pExprGeneric<Expr07>(
        c, pExpr06, {Op::eq, Op::noteq}, fl
    );
}

unique_ptr<Construct>
pExpr11(ParseContext &c, unsigned fl)
{
    return pExprGeneric<Expr11>(
        c, pExpr07, {Op::land}, fl
    );
}

unique_ptr<Construct>
pExpr12(ParseContext &c, unsigned fl)
{
    return pExprGeneric<Expr12>(
        c, pExpr11, {Op::lor}, fl
    );
}

unique_ptr<Construct>
pExpr14(ParseContext &c, unsigned fl)
{
    static const initializer_list<Op> valid_ops = {
        Op::assign, Op::addeq, Op::subeq, Op::muleq, Op::diveq, Op::modeq
    };

    unique_ptr<Construct> lside;
    unique_ptr<Expr14> ret;
    Op op = Op::invalid;

    if (fl & pFlags::pInDecl) {

        if (!pAcceptId(c, lside, false /* resolve_const */)) {

           /*
            * If the current statement is a declaration, require
            * strictly an identifier instead of a generic expression.
            */

            throw SyntaxErrorEx(
                c.get_loc(),
                "Expected identifier, got",
                &c.get_tok()
            );
        }

    } else {

        lside = pExpr12(c, fl & ~(pInDecl | pInConstDecl));
    }

    if (!lside)
        return nullptr;

    if ((op = AcceptOneOf(c, valid_ops)) != Op::invalid) {

        if (fl & pInDecl && op != Op::assign) {
            throw SyntaxErrorEx(
                c.get_loc(),
                "Operator '=' is required when declaring a variable or a constant"
            );
        }

    } else {

        /* No valid operator: this is not an assignment expr */

        if (fl & pFlags::pInDecl) {

            /* But we're in a decl (var or const): assume `none` as rvalue */

            ret.reset(new Expr14);
            ret->op = Op::assign;
            ret->lvalue = move(lside);
            ret->rvalue.reset(new LiteralNone());

        } else {

            /* Just return lside (doing const eval if possible) */

            if (c.const_eval && lside->is_const)
                return MakeConstructFromConstVal(lside->eval(c.const_ctx));

            return lside;
        }
    }

    if (!ret) {
        ret.reset(new Expr14);
        ret->op = op;
        ret->lvalue = move(lside);
        ret->rvalue = pExpr14(c, fl & ~(pInDecl | pInConstDecl));
    }

    ret->fl = fl & pFlags::pInDecl;

    if (!ret->rvalue)
        noExprError(c);

    if (c.const_eval && ret->rvalue->is_const) {
        ret->rvalue = MakeConstructFromConstVal(
            RValue(ret->rvalue->eval(c.const_ctx))
        );
    }

    if (c.const_eval && fl & pFlags::pInConstDecl) {

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
pExpr15(ParseContext &c, unsigned fl)
{
    return pExprGeneric<Expr15>(
        c, pExpr14, {Op::comma}, fl
    );
}

unique_ptr<Construct>
pExprTop(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> e = pExpr15(c, fl);

    if (c.const_eval && e && e->is_const) {
        return MakeConstructFromConstVal(e->eval(c.const_ctx));
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

    } if (pAcceptWhileStmt(c, subStmt, fl)) {

        return subStmt;

    } else {

        if (pAcceptKeyword(c, Keyword::kw_var))
            fl |= pFlags::pInDecl;
        else if (pAcceptKeyword(c, Keyword::kw_const))
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
                   unsigned fl)
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

        ifstmt->condExpr = pExprTop(c, fl);

        if (!ifstmt->condExpr)
            noExprError(c);

        pExpectOp(c, Op::parenR);

        if (!pAcceptBracedBlock(c, ifstmt->thenBlock, fl))
            ifstmt->thenBlock = pStmt(c, fl);

        if (pAcceptKeyword(c, Keyword::kw_else)) {
            if (!pAcceptBracedBlock(c, ifstmt->elseBlock, fl))
                ifstmt->elseBlock = pStmt(c, fl);
        }

        if (c.const_eval && ifstmt->condExpr->is_const) {

            const EvalValue &v = ifstmt->condExpr->eval(c.const_ctx);

            if (v.get_type()->is_true(v))
                ret = move(ifstmt->thenBlock);
            else
                ret = move(ifstmt->elseBlock);

        } else {
            ret = move(ifstmt);
        }

        return true;
    }

    return false;
}

bool
pAcceptWhileStmt(ParseContext &c, unique_ptr<Construct> &ret, unsigned fl)
{
    if (pAcceptKeyword(c, Keyword::kw_while)) {

        unique_ptr<WhileStmt> whileStmt(new WhileStmt);
        pExpectOp(c, Op::parenL);

        whileStmt->condExpr = pExprTop(c, fl);

        if (!whileStmt->condExpr)
            noExprError(c);

        pExpectOp(c, Op::parenR);

        if (!pAcceptBracedBlock(c, whileStmt->body, pFlags::pInLoop))
            whileStmt->body = pStmt(c, pFlags::pInLoop);

        if (c.const_eval && whileStmt->condExpr->is_const) {

            const EvalValue &v = whileStmt->condExpr->eval(c.const_ctx);

            if (!v.get_type()->is_true(v)) {
                ret.reset();
                return true;
            }
        }

        ret = move(whileStmt);
        return true;
    }

    return false;
}

unique_ptr<Construct>
MakeConstructFromConstVal(const EvalValue &v)
{
    if (v.is<long>())
        return make_unique<LiteralInt>(v.get<long>());
    else if (v.is<NoneVal>())
        return make_unique<LiteralNone>();
    else if (v.is<SharedStrWrapper>())
        return make_unique<LiteralStr>(v);

    throw InternalErrorEx();
}
