/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "eval.h"
#include "syntax.h"

using std::string;

ParseContext::ParseContext(const TokenStream &ts, bool const_eval)
    : const_ctx_owner(new EvalContext(nullptr, true))
    , ts(ts)
    , const_eval(const_eval)
    , const_ctx(const_ctx_owner.get())
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

unique_ptr<Construct> pExprTop(ParseContext &c, unsigned fl);
unique_ptr<Construct> pStmt(ParseContext &c, unsigned fl);

bool
pAcceptKeyword(ParseContext &c, Keyword exp);

bool
pAcceptOp(ParseContext &c, Op exp);

void
pExpectOp(ParseContext &c, Op exp);

bool
pAcceptIfStmt(ParseContext &c,
              unique_ptr<Construct> &ret,
              unsigned fl);

bool
pAcceptWhileStmt(ParseContext &c,
                 unique_ptr<Construct> &ret,
                 unsigned fl);

bool
pAcceptForeachStmt(ParseContext &c,
                   unique_ptr<Construct> &ret,
                   unsigned fl);

bool
pAcceptBracedBlock(ParseContext &c,
                   unique_ptr<Construct> &ret,
                   unsigned fl);

bool
pAcceptFuncDecl(ParseContext &c,
                unique_ptr<Construct> &ret,
                unsigned fl);

bool
pAcceptTryCatchStmt(ParseContext &c,
                    unique_ptr<Construct> &ret,
                    unsigned fl);

bool
pAcceptForStmt(ParseContext &c,
               unique_ptr<Construct> &ret,
               unsigned fl);

bool
MakeConstructFromConstVal(const EvalValue &v,
                          unique_ptr<Construct> &out,
                          bool process_arrays = false);

bool
pAcceptLiteralInt(ParseContext &c, unique_ptr<Construct> &v)
{
    const Loc start = c.get_loc();

    if (*c == TokType::integer) {

        const string s(c.get_str());

        v.reset(new LiteralInt(stol(s)));
        v->start = start;
        v->end = c.get_loc() + (s.length() + 1);
        c++;
        return true;

    } else if (pAcceptKeyword(c, Keyword::kw_true)) {

        v.reset(new LiteralInt(1));
        v->start = start;
        v->end = c.get_loc() + 5;
        return true;

    } else if (pAcceptKeyword(c, Keyword::kw_false)) {

        v.reset(new LiteralInt(0));
        v->start = start;
        v->end = c.get_loc() + 6;
        return true;
    }

    return false;
}

bool
pAcceptLiteralFloat(ParseContext &c, unique_ptr<Construct> &v)
{
    const Loc start = c.get_loc();

    if (*c == TokType::floatnum) {

        const string s(c.get_str());

        v.reset(new LiteralFloat(stold(s)));
        v->start = start;
        v->end = c.get_loc() + (s.length() + 1);
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
        v->start = c.get_loc();
        v->end = c.get_loc() + (c.get_str().length() + 1);
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

            /*
             * The const evaluation is enabled and we've been asked to resolve
             * identifier, if possible. Steps:
             *
             *      1) Eval it in the const EvalContext
             *
             *      2) If we got an LValue, we've found a constant, which
             *         can contain either a literal or point to a const builtin
             *         function like len().
             *
             *      3) Call MakeConstructFromConstVal() to get a literal
             *         construct for this ID's value and replace the current
             *         identifier in `v`. If it fails, it just means that
             *         it was a const builtin function: it will be const
             *         evaluated later. For the moment, keep the ID, but mark
             *         it as `const`.
             */
            const EvalValue &const_value = v->eval(c.const_ctx);

            if (const_value.get_type()->t == Type::t_lval) {

                MakeConstructFromConstVal(RValue(const_value), v);
                v->is_const = true;

            } else if (const_value.is<FlatSharedFuncObj>()) {

                const FuncObject &obj =
                    const_value.get<FlatSharedFuncObj>().get();

                if (obj.func->is_const)
                    v->is_const = true;
            }
        }

        v->start = c.get_loc();
        v->end = c.get_loc() + (c.get_str().length() + 1);
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

void pExpectOp(ParseContext &c, Op exp)
{
    if (!pAcceptOp(c, exp))
        throw SyntaxErrorEx(c.get_loc(), "Expected operator", &c.get_tok(), exp);
}

Op
AcceptOneOf(ParseContext &c, std::initializer_list<Op> list)
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

unique_ptr<Identifier>
pIdentifier(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> ret;

    if (!pAcceptId(c, ret))
        return nullptr;

    return unique_ptr<Identifier>(static_cast<Identifier *>(ret.release()));
}

template <class T>
unique_ptr<T>
pList(ParseContext &c,
      unsigned fl,
      unique_ptr<typename T::ElemType> (*lowerE)(ParseContext&, unsigned))
{
    unique_ptr<T> ret(new T);
    unique_ptr<typename T::ElemType> subexpr;
    bool is_const = true;

    ret->start = c.get_loc();
    subexpr = lowerE(c, fl);

    if (subexpr) {

        is_const = is_const && subexpr->is_const;
        ret->elems.emplace_back(move(subexpr));

        while (*c == Op::comma) {

            c++;
            subexpr = lowerE(c, fl);

            if (!subexpr)
                noExprError(c);

            is_const = is_const && subexpr->is_const;
            ret->elems.emplace_back(move(subexpr));
        }
    }

    ret->end = c.get_loc() + 1;
    ret->is_const = is_const;
    return ret;
}

bool
pAcceptCallExpr(ParseContext &c,
                unique_ptr<Construct> &what,
                unique_ptr<Construct> &ret,
                unsigned fl)
{
    if (pAcceptOp(c, Op::parenL)) {

        unique_ptr<CallExpr> expr(new CallExpr);

        ret.reset();
        expr->start = what->start;
        expr->what = move(what);
        expr->args = pList<ExprList>(c, fl, pExpr14);

        if (c.const_eval && expr->what->is_const && expr->args->is_const) {

            expr->is_const = true;

            MakeConstructFromConstVal(
                expr->eval(c.const_ctx),
                ret,
                fl & pFlags::pInConstDecl
            );
        }

        if (!ret) {
            expr->end = c.get_loc();
            ret = move(expr);
        }

        pExpectOp(c, Op::parenR);
        return true;
    }

    return false;
}

bool
pAcceptSubscript(ParseContext &c,
                 unique_ptr<Construct> &what,
                 unique_ptr<Construct> &ret,
                 unsigned fl)
{
    if (pAcceptOp(c, Op::bracketL)) {

        unique_ptr<Construct> start = pExprTop(c, fl);
        bool in_slice = false;

        if (pAcceptOp(c, Op::colon)) {

            unique_ptr<Slice> s(new Slice);

            s->what = move(what);
            s->start_idx = move(start);
            s->end_idx = pExprTop(c, fl);

            if (s->what->is_const) {
                if (!s->start_idx || s->start_idx->is_const)
                    if (!s->end_idx || s->end_idx->is_const)
                        s->is_const = true;
            }

            ret = move(s);
            in_slice = true;

        } else {

            unique_ptr<Subscript> s(new Subscript);

            if (!start)
                noExprError(c);

            s->what = move(what);
            s->index = move(start);

            if (s->what->is_const && s->index->is_const)
                s->is_const = true;

            ret = move(s);
        }

        if (c.const_eval && ret->is_const) {

            if (!in_slice || fl & pFlags::pInConstDecl) {

                unique_ptr<Construct> const_construct;
                const EvalValue &v = RValue(ret->eval(c.const_ctx));

                if (MakeConstructFromConstVal(v, const_construct, true))
                    ret = move(const_construct);
            }
        }

        pExpectOp(c, Op::bracketR);
        return true;
    }

    return false;
}

unique_ptr<Construct>
pArray(ParseContext &c, unsigned fl)
{
    return pList<LiteralArray>(c, fl, pExpr14);
}

unique_ptr<LiteralDictKVPair>
pDictKVPair(ParseContext &c, unsigned fl)
{
    unique_ptr<LiteralDictKVPair> p(new LiteralDictKVPair);

    p->key = pExpr14(c, fl);

    if (!p->key) {

        if (*c == Op::braceR)
            return nullptr;

        noExprError(c);
    }

    pExpectOp(c, Op::colon);

    p->value = pExpr14(c, fl);

    if (!p->value)
        noExprError(c);

    if (p->key->is_const && p->value->is_const)
        p->is_const = true;

    return p;
}

unique_ptr<Construct>
pDict(ParseContext &c, unsigned fl)
{
    return pList<LiteralDict>(c, fl, pDictKVPair);
}

bool
pAcceptMember(ParseContext &c,
              unique_ptr<Construct> &what,
              unique_ptr<Construct> &ret,
              unsigned fl)
{
    if (!pAcceptOp(c, Op::dot))
        return false;

    unique_ptr<MemberExpr> mem(new MemberExpr);
    mem->is_const = what->is_const;
    mem->start = what->start;
    mem->what = move(what);

    unique_ptr<Construct> tmpId;

    if (!pAcceptId(c, tmpId, false)) {
        throw SyntaxErrorEx(c.get_loc(), "Expected identifier, got", &c.get_tok());
    }

    mem->end = c.get_loc();
    unique_ptr<Identifier> id(dynamic_cast<Identifier *>(tmpId.release()));

    if (!id)
        throw InternalErrorEx(mem->start, mem->end);

    mem->memId = SharedStr(string(id->get_str()));
    ret = move(mem);
    return true;
}

unique_ptr<Construct>
pExpr01(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> main;
    unique_ptr<Construct> otherExpr;
    const Loc start = c.get_loc();

    if (pAcceptLiteralInt(c, main)) {

        return main;     /* Not subscriptable, nor callable */

    } else if (pAcceptLiteralFloat(c, main)) {

        return main;

    } else if (pAcceptKeyword(c, Keyword::kw_none)) {

        main.reset(new LiteralNone());
        main->start = start;
        main->end = c.get_loc();
        return main;     /* Not subscriptable, nor callable */
    }

    if (pAcceptLiteralStr(c, main)) {

        main->is_const = true;

    } else if (pAcceptOp(c, Op::parenL)) {

        main = pExprTop(c, fl);

        if (!main)
            noExprError(c);

        pExpectOp(c, Op::parenR);

    } else if (pAcceptOp(c, Op::bracketL)) {

        main = pArray(c, fl);
        pExpectOp(c, Op::bracketR);

    } else if (pAcceptOp(c, Op::braceL)) {

        main = pDict(c, fl);
        pExpectOp(c, Op::braceR);

    } else if (pAcceptId(c, main)) {

        /* do nothing */

    } else {

        return nullptr;
    }

    while (pAcceptCallExpr(c, main, otherExpr, fl)  ||
           pAcceptSubscript(c, main, otherExpr, fl) ||
           pAcceptMember(c, main, otherExpr, fl))
    {
        main = move(otherExpr);
    }

    return main;
}

template <class ExprT>
unique_ptr<Construct>
pExprGeneric(ParseContext &c,
             unique_ptr<Construct> (*lowerExpr)(ParseContext&, unsigned),
             std::initializer_list<Op> ops,
             unsigned fl)
{
    Op op;
    const Loc start = c.get_loc();
    unique_ptr<Construct> lowerE = lowerExpr(c, fl);
    unique_ptr<ExprT> ret;
    bool is_const;

    if (!lowerE || lowerE->is_nop())
        return lowerE;

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

    ret->start = start;
    ret->end = c.get_loc();
    ret->is_const = is_const;
    return ret;
}

unique_ptr<Construct>
pExpr02(ParseContext &c, unsigned fl)
{
    unique_ptr<Expr02> ret;
    unique_ptr<Construct> elem;
    const Loc start = c.get_loc();
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
    ret->start = start;
    ret->end = c.get_loc();
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

static void
declExprCheckId(ParseContext &c, Construct *id)
{
    const EvalValue &val = id->eval(c.const_ctx);

    if (!val.is<UndefinedId>()) {

        if (val.is<LValue *>()) {
            if (val.get<LValue *>()->is<Builtin>())
                throw CannotRebindBuiltinEx(c.get_loc());
        }

        throw CannotRebindConstEx(c.get_loc());
    }
}

unique_ptr<Construct>
pExpr14(ParseContext &c, unsigned fl)
{
    static const std::initializer_list<Op> valid_ops = {
        Op::assign, Op::addeq, Op::subeq, Op::muleq, Op::diveq, Op::modeq
    };

    const Loc start = c.get_loc();
    unique_ptr<Construct> lside;
    unique_ptr<Expr14> ret;
    bool in_idlist = false;
    Op op;

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

        /*
         * lside is just an identifier, so it's fine to evaluate it even if
         * we're not sure it's const: at most, it will evaluate to
         * UndefinedId.
         */

        declExprCheckId(c, lside.get());

    } else {

        if (pAcceptFuncDecl(c, lside, fl & ~pFlags::pInStmt))
            return lside;


        lside = pExpr12(c, fl & ~pFlags::pInStmt);
    }

    if (!lside)
        return nullptr;

    if (fl & pFlags::pInStmt) {

        Identifier *first_id = dynamic_cast<Identifier *>(lside.get());

        if (first_id && pAcceptOp(c, Op::comma)) {

            unique_ptr<IdList> idlist(new IdList);
            lside.release();
            idlist->elems.emplace_back(first_id);

            unique_ptr<IdList> tmp = pList<IdList>(c, fl, pIdentifier);

            idlist->elems.insert(
                idlist->elems.end(),
                make_move_iterator(tmp->elems.begin()),
                make_move_iterator(tmp->elems.end())
            );

            if (fl & pFlags::pInDecl) {
                for (const auto &e : idlist->elems)
                    declExprCheckId(c, e.get());
            }

            lside = move(idlist);
            in_idlist = true;
        }
    }

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

        } else if (in_idlist) {

            throw SyntaxErrorEx(
                c.get_loc(),
                "Operator '=' is required when the left side is an ID list"
            );

        } else {

            /* Just return lside (doing const eval if possible) */

            if (c.const_eval && lside->is_const)
                MakeConstructFromConstVal(lside->eval(c.const_ctx), lside);

            return lside;
        }
    }

    if (!ret) {
        ret.reset(new Expr14);
        ret->op = op;
        ret->lvalue = move(lside);
        ret->rvalue = pExpr14(c, fl & ~pFlags::pInDecl);
    }

    ret->fl = fl & pFlags::pInDecl;
    ret->start = start;
    ret->end = c.get_loc();

    if (!ret->rvalue)
        noExprError(c);

    if (c.const_eval && ret->rvalue->is_const) {
        MakeConstructFromConstVal(
            RValue(ret->rvalue->eval(c.const_ctx)),
            ret->rvalue,
            true
        );
    }

    if (c.const_eval && fl & pFlags::pInConstDecl) {

        if (!ret->rvalue->is_const)
            throw ExpressionIsNotConstEx(ret->rvalue->start, ret->rvalue->end);

        /*
         * Save the const declaration by evaluating the assignment
         * in our special `const_ctx` EvalContext.
         */

        const EvalValue &rvalue = ret->eval(c.const_ctx);

        if (!rvalue.is<SharedArrayObj>() && !rvalue.is<FlatSharedDictObj>()) {

            /*
             * In all the cases, except SharedArrayObj and FlatSharedDictObj,
             * we just return a NopConstruct. Note: we cannot return nullptr,
             * otherwise it would seem that we matched nothing and pAcceptBracedBlock()
             * will expect "}".
             */
            return make_unique<NopConstruct>();
        }

        ret->lvalue->is_const = true;
    }

    return ret;
}

unique_ptr<Construct>
pExprTop(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> e = pExpr14(c, fl);

    if (c.const_eval && e && e->is_const && !e->is_nop())
        MakeConstructFromConstVal(e->eval(c.const_ctx), e);

    return e;
}

bool
pAcceptReturnStmt(ParseContext &c,
                  unique_ptr<Construct> &ret,
                  unsigned fl)
{
    const Loc start = c.get_loc();

    if (fl & pFlags::pInFuncBody && pAcceptKeyword(c, Keyword::kw_return)) {

        unique_ptr<ReturnStmt> stmt(new ReturnStmt);

        stmt->elem = pExpr14(c, fl);
        pExpectOp(c, Op::semicolon);
        stmt->start = start;
        stmt->end = c.get_loc();
        ret = move(stmt);
        return true;
    }

    return false;
}

bool
pAcceptThrowStmt(ParseContext &c,
                 unique_ptr<Construct> &ret,
                 unsigned fl)
{
    const Loc start = c.get_loc();

    if (pAcceptKeyword(c, Keyword::kw_throw)) {

        unique_ptr<ThrowStmt> t(new ThrowStmt);
        t->elem = pExprTop(c, fl);

        if (!t->elem)
            noExprError(c);

        t->start = start;
        t->end = c.get_loc();
        ret = move(t);
        return true;
    }

    return false;
}

unique_ptr<Construct>
pStmt(ParseContext &c, unsigned fl)
{
    unique_ptr<Construct> subStmt;
    const Loc start = c.get_loc();
    fl |= pFlags::pInStmt;

    if (fl & pFlags::pInLoop) {

        if (pAcceptKeyword(c, Keyword::kw_break))
            return make_unique<BreakStmt>();
        else if (pAcceptKeyword(c, Keyword::kw_continue))
            return make_unique<ContinueStmt>();
    }

    if (fl & pFlags::pInCatchBody) {

        if (pAcceptKeyword(c, Keyword::kw_rethrow))
            return make_unique<RethrowStmt>(start, c.get_loc());
    }

    if (pAcceptIfStmt(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptWhileStmt(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptFuncDecl(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptReturnStmt(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptTryCatchStmt(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptThrowStmt(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptForeachStmt(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptBracedBlock(c, subStmt, fl)) {

        return subStmt;

    } else if (pAcceptForStmt(c, subStmt, fl)) {

        return subStmt;

    } else {

        if (pAcceptKeyword(c, Keyword::kw_var))
            fl |= pFlags::pInDecl;
        else if (pAcceptKeyword(c, Keyword::kw_const))
            fl |= pFlags::pInDecl | pFlags::pInConstDecl;

        unique_ptr<Construct> lowerE = pExprTop(c, fl);

        if (lowerE)
           pExpectOp(c, Op::semicolon);

        return lowerE;
    }
}

unique_ptr<Construct>
pBlock(ParseContext &c, unsigned fl)
{
    unique_ptr<Block> ret(new Block);
    unique_ptr<Construct> stmt, tmp;
    bool added_elem;

    ret->start = c.get_loc();
    EvalContext block_const_ctx(c.const_ctx, true);
    c.const_ctx = &block_const_ctx; // push a new const eval context

    if (!c.eoi()) {

        do {

            added_elem = false;

            if (pAcceptBracedBlock(c, tmp, fl)) {
                ret->elems.emplace_back(move(tmp));
                added_elem = true;
            }

            while ((stmt = pStmt(c, fl))) {

                if (!stmt->is_nop())
                    ret->elems.emplace_back(move(stmt));

                added_elem = true;

                while (*c == Op::semicolon)
                    c++;    /* skip multiple ';' */
            }

        } while (added_elem);
    }

    ret->end = c.get_loc();
    c.const_ctx = c.const_ctx->parent; // restore the previous const ctx
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
    const Loc start = c.get_loc();

    if (!pAcceptKeyword(c, Keyword::kw_if))
        return false;

    unique_ptr<IfStmt> ifstmt(new IfStmt);
    pExpectOp(c, Op::parenL);

    ifstmt->condExpr = pExprTop(c, fl);

    if (!ifstmt->condExpr)
        noExprError(c);

    pExpectOp(c, Op::parenR);

    if (!pAcceptBracedBlock(c, ifstmt->thenBlock, fl)) {

        unique_ptr<Construct> stmt = pStmt(c, fl);

        if (stmt && !stmt->is_nop())
            ifstmt->thenBlock = move(stmt);
    }

    if (pAcceptKeyword(c, Keyword::kw_else)) {

        if (!pAcceptBracedBlock(c, ifstmt->elseBlock, fl)) {

            unique_ptr<Construct> stmt = pStmt(c, fl);

            if (stmt && !stmt->is_nop())
                ifstmt->elseBlock = move(stmt);
        }
    }

    ifstmt->start = start;
    ifstmt->end = c.get_loc();

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

bool
pAcceptWhileStmt(ParseContext &c, unique_ptr<Construct> &ret, unsigned fl)
{
    const Loc start = c.get_loc();

    if (!pAcceptKeyword(c, Keyword::kw_while))
        return false;

    unique_ptr<WhileStmt> whileStmt(new WhileStmt);
    pExpectOp(c, Op::parenL);

    whileStmt->condExpr = pExprTop(c, fl);

    if (!whileStmt->condExpr)
        noExprError(c);

    pExpectOp(c, Op::parenR);

    if (!pAcceptBracedBlock(c, whileStmt->body, fl | pFlags::pInLoop))
        whileStmt->body = pStmt(c, fl | pFlags::pInLoop);

    whileStmt->start = start;
    whileStmt->end = c.get_loc();

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

bool
pAcceptFuncDecl(ParseContext &c,
                unique_ptr<Construct> &ret,
                unsigned fl)
{
    const Loc start = c.get_loc();
    bool is_pure = false;

    if (pAcceptKeyword(c, Keyword::kw_pure))
        is_pure = true;

    if (!pAcceptKeyword(c, Keyword::kw_func)) {

        if (is_pure) {
            throw SyntaxErrorEx(
                c.get_loc(),
                "Expected keyword `func` after `pure`, got",
                &c.get_tok()
            );
        }

        return false;
    }

    unique_ptr<FuncDeclStmt> func(new FuncDeclStmt);
    func->start = start;
    func->is_const = is_pure;

    if (fl & pFlags::pInStmt) {

        fl &= ~pFlags::pInStmt;
        func->id = pIdentifier(c, fl);

        if (!func->id)
            throw SyntaxErrorEx(c.get_loc(), "Expected identifier, got", &c.get_tok());

    } else {

        if (pAcceptOp(c, Op::bracketL)) {

            if (is_pure) {
                throw SyntaxErrorEx(
                    c.get_loc(),
                    "Capture list NOT allowed in PURE functions"
                );
            }

            func->captures = pList<IdList>(c, fl, pIdentifier);
            pExpectOp(c, Op::bracketR);
        }
    }

    if (pAcceptOp(c, Op::parenL)) {
        func->params = pList<IdList>(c, fl, pIdentifier);
        pExpectOp(c, Op::parenR);
    }

    if (pAcceptOp(c, Op::arrow)) {

        func->body = pExpr14(c, fl);

    } else if (!pAcceptBracedBlock(c, func->body, fl | pFlags::pInFuncBody)) {

        throw SyntaxErrorEx(
            c.get_loc(),
            "Expected { } block or => expr, got",
            &c.get_tok()
        );
    }

    func->end = c.get_loc() + 1;

    if (c.const_eval && is_pure && func->id)
        func->eval(c.const_ctx);

    ret = move(func);
    return true;
}

bool
MakeConstructFromConstVal(const EvalValue &v,
                          unique_ptr<Construct> &out,
                          bool process_arrays)
{
    if (v.is<int_type>()) {
        out = make_unique<LiteralInt>(v.get<int_type>());
        return true;
    }

    if (v.is<float_type>()) {
        out = make_unique<LiteralFloat>(v.get<float_type>());
        return true;
    }

    if (v.is<NoneVal>()) {
        out = make_unique<LiteralNone>();
        return true;
    }

    if (v.is<SharedStr>()) {
        out = make_unique<LiteralStr>(v);
        return true;
    }

    if (process_arrays) {

        if (v.is<SharedArrayObj>()) {

            const ArrayConstView &view = v.get<SharedArrayObj>().get_view();
            unique_ptr<LiteralArray> litarr(new LiteralArray);

            for (unsigned i = 0; i < view.size(); i++) {

                unique_ptr<Construct> construct;

                if (!MakeConstructFromConstVal(view[i].get(), construct, true))
                    return false;

                litarr->elems.push_back(move(construct));
            }

            litarr->is_const = true;
            out = move(litarr);
            return true;

        } else if (v.is<FlatSharedDictObj>()) {

            const DictObject::inner_type &data =
                v.get<FlatSharedDictObj>()->get_ref();

            unique_ptr<LiteralDict> litDict(new LiteralDict);

            for (const auto &p : data) {

                unique_ptr<LiteralDictKVPair> kvpair(new LiteralDictKVPair);

                if (!MakeConstructFromConstVal(p.first, kvpair->key, true))
                    return false;

                if (!MakeConstructFromConstVal(p.second.get(), kvpair->value, true))
                    return false;

                litDict->elems.push_back(move(kvpair));
            }

            litDict->is_const = true;
            out = move(litDict);
            return true;
        }
    }

    return false;
}

bool
pAcceptTryCatchStmt(ParseContext &c, unique_ptr<Construct> &ret, unsigned fl)
{
    if (!pAcceptKeyword(c, Keyword::kw_try))
        return false;

    unique_ptr<TryCatchStmt> stmt(new TryCatchStmt);
    bool have_catch_anything = false;

    if (!pAcceptBracedBlock(c, stmt->tryBody, fl))
        throw SyntaxErrorEx(c.get_loc(), "Expected { } block, got", &c.get_tok());

    while (pAcceptKeyword(c, Keyword::kw_catch)) {

        unique_ptr<IdList> exList;
        unique_ptr<Identifier> asId;
        unique_ptr<Construct> body;

        if (have_catch_anything) {
            throw SyntaxErrorEx(
                c.get_loc(),
                "At most one catch-anything block is allowed"
            );
        }

        if (pAcceptOp(c, Op::parenL)) {

            exList = pList<IdList>(c, fl, pIdentifier);

            if (exList->elems.size() == 0) {

                throw SyntaxErrorEx(
                    c.get_loc(),
                    "Expected non-empty exception list, got",
                    &c.get_tok()
                );
            }

            if (pAcceptKeyword(c, Keyword::kw_as)) {

                unique_ptr<Construct> id;

                if (!pAcceptId(c, id, false)) {
                    throw SyntaxErrorEx(
                        c.get_loc(),
                        "Expected identifier, got",
                        &c.get_tok()
                    );
                }

                asId.reset(static_cast<Identifier *>(id.release()));
            }

            pExpectOp(c, Op::parenR);

        } else {

            have_catch_anything = true;
        }

        if (!pAcceptBracedBlock(c, body, fl | pFlags::pInCatchBody))
            throw SyntaxErrorEx(c.get_loc(), "Expected { } block, got", &c.get_tok());

        stmt->catchStmts.emplace_back(
            AllowedExList{move(exList), move(asId)},
            move(body)
        );
    };

    if (pAcceptKeyword(c, Keyword::kw_finally)) {

        if (!pAcceptBracedBlock(c, stmt->finallyBody, fl))
            throw SyntaxErrorEx(c.get_loc(), "Expected { } block, got", &c.get_tok());
    }

    if (!stmt->catchStmts.size() && !stmt->finallyBody) {
        throw SyntaxErrorEx(
            c.get_loc(),
            "At least one catch block or a finally block is required"
        );
    }

    ret = move(stmt);
    return true;
}

bool
pAcceptForeachStmt(ParseContext &c,
                   unique_ptr<Construct> &ret,
                   unsigned fl)
{
    const Loc start = c.get_loc();

    if (!pAcceptKeyword(c, Keyword::kw_foreach))
        return false;

    unique_ptr<ForeachStmt> stmt(new ForeachStmt);

    pExpectOp(c, Op::parenL);

    if (pAcceptKeyword(c, Keyword::kw_var))
        stmt->idsVarDecl = true;

    stmt->ids = pList<IdList>(c, fl, pIdentifier);

    if (stmt->ids->elems.size() == 0)
        throw SyntaxErrorEx(c.get_loc(), "Expected at least one identifier");

    if (!pAcceptKeyword(c, Keyword::kw_in))
        throw SyntaxErrorEx(c.get_loc(), "Expected keyword `in`, got", &c.get_tok());

    if (pAcceptKeyword(c, Keyword::kw_indexed))
        stmt->indexed = true;

    stmt->container = pExpr01(c, fl);

    if (!stmt->container)
        noExprError(c);

    pExpectOp(c, Op::parenR);
    stmt->start = start;
    stmt->end = c.get_loc();

    if (!pAcceptBracedBlock(c, stmt->body, fl | pFlags::pInLoop))
        stmt->body = pStmt(c, fl | pFlags::pInLoop);

    if (c.const_eval && stmt->container->is_const) {

        const EvalValue &v = RValue(stmt->container->eval(c.const_ctx));

        try {

            if (v.get_type()->len(v) == 0) {
                ret.reset();
                return true;
            }

        } catch (Exception &e) {
            e.loc_start = stmt->container->start;
            e.loc_end = stmt->container->end;
            throw;
        }
    }

    ret = move(stmt);
    return true;
}

bool
pAcceptForStmt(ParseContext &c,
               unique_ptr<Construct> &ret,
               unsigned fl)
{
    if (!pAcceptKeyword(c, Keyword::kw_for))
        return false;

    unique_ptr<ForStmt> stmt(new ForStmt);
    pExpectOp(c, Op::parenL);

    {
        unsigned init_fl = fl;

        if (pAcceptKeyword(c, Keyword::kw_var))
            init_fl |= pFlags::pInDecl;

        stmt->init = pExprTop(c, init_fl);
        pExpectOp(c, Op::semicolon);
    }

    stmt->cond = pExprTop(c, fl);
    pExpectOp(c, Op::semicolon);

    stmt->inc = pExprTop(c, fl);
    pExpectOp(c, Op::parenR);

    if (!pAcceptBracedBlock(c, stmt->body, fl | pFlags::pInLoop))
        stmt->body = pStmt(c, fl);

    ret = move(stmt);
    return true;
}
