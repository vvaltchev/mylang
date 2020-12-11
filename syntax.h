/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "evalvalue.h"
#include "parser.h"

class Literal;
class LiteralInt;
class ExprList;
class CallExpr;
class Expr01;
class Expr03;
class Expr04;
class Expr06;
class Stmt;
class IfStmt;
class Block;
class LValue;
class Identifier;
class EvalContext;

enum pFlags : unsigned {

    pNone           = 1 << 0,
    pInDecl         = 1 << 1,
    pInConstDecl    = 1 << 2,
    pInLoop         = 1 << 3,
    pInStmt         = 1 << 4,
    pInFuncBody     = 1 << 5,
};

class Construct {

public:
    const char *const name;
    const bool is_nop;      // Purpose: avoid dynamic_cast<NopConstruct *>
    const bool is_ret;      // Purpose: avoid dynamic_cast<ReturnStmt *>
    bool is_const;
    Loc start;
    Loc end;

    Construct(const char *name,
              bool is_const = false,
              bool is_nop = false,
              bool is_ret = false)
        : name(name)
        , is_nop(is_nop)
        , is_ret(is_ret)
        , is_const(is_const)
        , start(Loc())
        , end(Loc())
    { }

    virtual ~Construct() = default;
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const = 0;
    virtual void serialize(ostream &s, int level = 0) const = 0;

    virtual EvalValue eval(EvalContext *ctx, bool rec = true) const;
};

class ChildlessConstruct : public Construct {

public:

    ChildlessConstruct(const char *name) : Construct(name) { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return EvalValue();
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class SingleChildConstruct : public Construct {

public:
    unique_ptr<Construct> elem;

    SingleChildConstruct(const char *name) : Construct(name) { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return elem->do_eval(ctx);
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class MultiOpConstruct : public Construct {

public:
    vector<pair<Op, unique_ptr<Construct>>> elems;

    MultiOpConstruct(const char *name) : Construct(name) { }
    virtual void serialize(ostream &s, int level = 0) const;

    /* Special methods */
    EvalValue eval_first_rvalue(EvalContext *ctx) const;
};

template <class ElemT = Construct>
class MultiElemConstruct : public Construct {

public:
    typedef ElemT ElemType;
    vector<unique_ptr<ElemType>> elems;

    MultiElemConstruct(const char *name) : Construct(name) { }
    virtual void serialize(ostream &s, int level = 0) const;
};

template <class T>
void MultiElemConstruct<T>::serialize(ostream &s, int level) const
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

inline ostream &operator<<(ostream &s, const Construct &c)
{
    c.serialize(s);
    return s;
}

class Literal : public Construct {

public:

    Literal() : Construct("Literal", true) { }
};

class LiteralInt : public Literal {

    const long value;

public:

    LiteralInt(long v) : value(v) { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return value;
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class LiteralNone : public Literal {

public:

    LiteralNone() : Literal() { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return EvalValue();
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class NopConstruct : public Construct {

public:

    NopConstruct() : Construct("nop", true, true) { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return EvalValue();
    }

    virtual void serialize(ostream &s, int level = 0) const {
        /* NopConstructs should never remain in the final syntax tree */
        throw InternalErrorEx();
    }
};


class LiteralStr : public Literal {

    EvalValue value;

public:

    LiteralStr(string_view v);

    LiteralStr(const EvalValue &v) : value(v) { }
    LiteralStr(EvalValue &&v) : value(move(v)) { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return value;
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class Identifier : public Construct {

public:
    string_view value;

    template <class T>
    Identifier(T &&arg) : Construct("Id"), value(forward<T>(arg)) { }

    virtual void serialize(ostream &s, int level = 0) const;
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class ExprList : public MultiElemConstruct<> {

public:

    ExprList() : MultiElemConstruct<>("ExprList") { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return EvalValue();
    }
};

class IdList : public MultiElemConstruct<Identifier> {

public:

    IdList() : MultiElemConstruct<Identifier>("IdList") { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return EvalValue();
    }
};

class CallExpr : public Construct {

public:
    unique_ptr<Construct> what;
    unique_ptr<ExprList> args;

    CallExpr() : Construct("CallExpr") { }
    virtual void serialize(ostream &s, int level = 0) const;
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr01 : public SingleChildConstruct {

public:
    Expr01() : SingleChildConstruct("Expr01") { }
};

class Expr02 : public MultiOpConstruct {

public:

    Expr02() : MultiOpConstruct("Expr02") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};


class Expr03 : public MultiOpConstruct {

public:

    Expr03() : MultiOpConstruct("Expr03") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr04 : public MultiOpConstruct {

public:

    Expr04() : MultiOpConstruct("Expr04") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr06 : public MultiOpConstruct {

public:

    Expr06() : MultiOpConstruct("Expr06") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr07 : public MultiOpConstruct {

public:

    Expr07() : MultiOpConstruct("Expr07") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr11 : public MultiOpConstruct {

public:

    Expr11() : MultiOpConstruct("Expr11") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr12 : public MultiOpConstruct {

public:

    Expr12() : MultiOpConstruct("Expr12") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr14 : public Construct {

public:
    unique_ptr<Construct> lvalue;
    unique_ptr<Construct> rvalue;
    unsigned fl;
    Op op;

    Expr14() : Construct("Expr14"), fl(pNone), op(Op::invalid) { }
    virtual void serialize(ostream &s, int level = 0) const;
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Expr15 : public MultiOpConstruct {

public:

    Expr15() : MultiOpConstruct("Expr15") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class Stmt : public SingleChildConstruct {

public:
    Stmt() : SingleChildConstruct("Stmt") { }
};

class IfStmt : public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> thenBlock;
    unique_ptr<Construct> elseBlock;

    IfStmt() : Construct("IfStmt") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
    virtual void serialize(ostream &s, int level = 0) const;
};

class Block : public MultiElemConstruct<> {

public:
    Block() : MultiElemConstruct("Block") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class BreakStmt : public ChildlessConstruct {

public:
    BreakStmt(): ChildlessConstruct("BreakStmt") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class ContinueStmt : public ChildlessConstruct {

public:
    ContinueStmt(): ChildlessConstruct("ContinueStmt") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
};

class ReturnStmt : public Construct {

public:
    unique_ptr<Construct> elem;

    ReturnStmt(): Construct("ReturnStmt", false, false, true) { }

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
    virtual void serialize(ostream &s, int level = 0) const;
};

class WhileStmt : public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> body;

    WhileStmt() : Construct("WhileStmt") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
    virtual void serialize(ostream &s, int level = 0) const;
};

class FuncDeclStmt : public Construct {

public:
    unique_ptr<Identifier> id;  /* NULL when the func is defined inside an expr */
    unique_ptr<IdList> captures;
    unique_ptr<IdList> params;
    unique_ptr<Construct> body;

    FuncDeclStmt() : Construct("FuncDeclStmt") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
    virtual void serialize(ostream &s, int level = 0) const;
};

class Subscript : public Construct {

public:

    unique_ptr<Construct> what;
    unique_ptr<Construct> index;

    Subscript() : Construct("Subscript") { }
    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const;
    virtual void serialize(ostream &s, int level = 0) const;
};
