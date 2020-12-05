/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "lexer.h"
#include "evalvalue.h"

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
};

class Construct {

public:
    const char *const name;
    bool is_const;

    Construct(const char *name, bool is_const = false)
        : name(name), is_const(is_const) { }

    virtual ~Construct() = default;
    virtual EvalValue eval(EvalContext *) const = 0;
    virtual void serialize(ostream &s, int level = 0) const = 0;
};

class ChildlessConstruct : public Construct {

public:

    ChildlessConstruct(const char *name) : Construct(name) { }

    virtual EvalValue eval(EvalContext *ctx) const {
        return EvalValue();
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class SingleChildConstruct : public Construct {

public:
    unique_ptr<Construct> elem;

    SingleChildConstruct(const char *name) : Construct(name) { }

    virtual EvalValue eval(EvalContext *ctx) const {
        return elem->eval(ctx);
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

class MultiElemConstruct : public Construct {

public:
    vector<unique_ptr<Construct>> elems;

    MultiElemConstruct(const char *name) : Construct(name) { }
    virtual void serialize(ostream &s, int level = 0) const;
};

inline ostream &operator<<(ostream &s, const Construct &c)
{
    c.serialize(s);
    return s;
}

class Literal : public Construct {

public:
    const long value;

    Literal(long v) : Construct("Literal", true), value(v) { }
};

class LiteralInt : public Literal {

public:

    LiteralInt(long v) : Literal(v) { }

    virtual EvalValue eval(EvalContext *ctx) const {
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
    virtual EvalValue eval(EvalContext *ctx) const;
};

class ExprList : public MultiElemConstruct {

public:

    ExprList() : MultiElemConstruct("ExprList") { }

    virtual EvalValue eval(EvalContext *ctx) const {
        return EvalValue();
    }
};

class CallExpr : public Construct {

public:
    unique_ptr<Identifier> id;
    unique_ptr<ExprList> args;

    CallExpr() : Construct("CallExpr") { }
    virtual void serialize(ostream &s, int level = 0) const;
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr01 : public SingleChildConstruct {

public:
    Expr01() : SingleChildConstruct("Expr01") { }
};

class Expr02 : public MultiOpConstruct {

public:

    Expr02() : MultiOpConstruct("Expr02") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};


class Expr03 : public MultiOpConstruct {

public:

    Expr03() : MultiOpConstruct("Expr03") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr04 : public MultiOpConstruct {

public:

    Expr04() : MultiOpConstruct("Expr04") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr06 : public MultiOpConstruct {

public:

    Expr06() : MultiOpConstruct("Expr06") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr07 : public MultiOpConstruct {

public:

    Expr07() : MultiOpConstruct("Expr07") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr11 : public MultiOpConstruct {

public:

    Expr11() : MultiOpConstruct("Expr11") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr12 : public MultiOpConstruct {

public:

    Expr12() : MultiOpConstruct("Expr12") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr14 : public Construct {

public:
    /* lvalue is mutable because it can be moved in case of NotLValueEx */
    mutable unique_ptr<Construct> lvalue;
    unique_ptr<Construct> rvalue;
    Op op;
    unsigned fl;

    Expr14() : Construct("Expr14"), op(Op::invalid), fl(pNone) { }
    virtual void serialize(ostream &s, int level = 0) const;
    virtual EvalValue eval(EvalContext *ctx) const;
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
    virtual void serialize(ostream &s, int level = 0) const;
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Block : public MultiElemConstruct {

public:
    Block() : MultiElemConstruct("Block") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class BreakStmt : public ChildlessConstruct {

public:
    BreakStmt(): ChildlessConstruct("BreakStmt") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class ContinueStmt : public ChildlessConstruct {

public:
    ContinueStmt(): ChildlessConstruct("ContinueStmt") { }
    virtual EvalValue eval(EvalContext *ctx) const;
};

class WhileStmt : public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> body;

    WhileStmt() : Construct("WhileStmt") { }
    virtual void serialize(ostream &s, int level = 0) const;
    virtual EvalValue eval(EvalContext *ctx) const;
};
