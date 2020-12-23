/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
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
    pInCatchBody    = 1 << 6,
};

enum class ConstructType {

    other,
    nop,
    ret,
    idlist,
    block,
};

class Construct {

public:
    const char *const name;
    const ConstructType ct;     /* Purpose: avoid dynamic_cast in some cases */

    bool is_const;
    Loc start;
    Loc end;

    Construct(const char *name,
              bool is_const = false,
              ConstructType ct = ConstructType::other)
        : name(name)
        , ct(ct)
        , is_const(is_const)
        , start(Loc())
        , end(Loc())
    { }

    bool is_nop() const { return ct == ConstructType::nop; }
    bool is_ret() const { return ct == ConstructType::ret; }
    bool is_idlist() const { return ct == ConstructType::idlist; }
    bool is_block() const { return ct == ConstructType::block; }

    virtual ~Construct() = default;

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return EvalValue();
    }

    virtual void serialize(ostream &s, int level = 0) const = 0;
    virtual EvalValue eval(EvalContext *ctx, bool rec = true) const;
};

class ChildlessConstruct : public Construct {

public:

    ChildlessConstruct(const char *name, Loc start = Loc(), Loc end = Loc())
        : Construct(name)
    {
        this->start = start;
        this->end = end;
    }

    void serialize(ostream &s, int level = 0) const override;
};

class SingleChildConstruct : public Construct {

public:
    unique_ptr<Construct> elem;

    SingleChildConstruct(const char *name) : Construct(name) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return elem->do_eval(ctx);
    }

    void serialize(ostream &s, int level = 0) const override;
};

class MultiOpConstruct : public Construct {

public:
    std::vector<std::pair<Op, unique_ptr<Construct>>> elems;

    MultiOpConstruct(const char *name) : Construct(name) { }
    void serialize(ostream &s, int level = 0) const override;

    /* Special methods */
    EvalValue eval_first_rvalue(EvalContext *ctx) const;
};

template <class ElemT = Construct>
class MultiElemConstruct : public Construct {

public:
    typedef ElemT ElemType;
    std::vector<unique_ptr<ElemType>> elems;

    MultiElemConstruct(const char *name, ConstructType ct = ConstructType::other)
        : Construct(name, false, ct)
    { }

    void serialize(ostream &s, int level = 0) const override;
};

template <class T>
void MultiElemConstruct<T>::serialize(ostream &s, int level) const
{
    std::string indent(level * 2, ' ');

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

class LiteralInt final: public Literal {

    const int_type value;

public:

    LiteralInt(int_type v) : value(v) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    void serialize(ostream &s, int level = 0) const override;
};

class LiteralFloat final: public Literal {

    const float_type value;

public:

    LiteralFloat(float_type v) : value(v) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    void serialize(ostream &s, int level = 0) const override;
};

class LiteralNone final: public Literal {

public:

    LiteralNone() : Literal() { }
    void serialize(ostream &s, int level = 0) const override;
};

class NopConstruct final: public Construct {

public:

    NopConstruct() : Construct("nop", true, ConstructType::nop) { }

    virtual void serialize(ostream &s, int level = 0) const {
        /* NopConstructs should never remain in the final syntax tree */
        throw InternalErrorEx();
    }
};

class LiteralStr final: public Literal {

    EvalValue value;

public:

    LiteralStr(std::string_view v);

    LiteralStr(const EvalValue &v) : value(v) { }
    LiteralStr(EvalValue &&v) : value(move(v)) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    void serialize(ostream &s, int level = 0) const override;
};

class LiteralArray final: public MultiElemConstruct<> {

public:

    LiteralArray() : MultiElemConstruct<>("LiteralArray") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class LiteralDictKVPair final: public Construct {

public:
    unique_ptr<Construct> key;
    unique_ptr<Construct> value;

    LiteralDictKVPair() : Construct("LiteralDictKVPair") { }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        throw InternalErrorEx(); /* Construct not meant to be evaluated directly */
    }
};

class LiteralDict final: public MultiElemConstruct<LiteralDictKVPair> {

public:

    LiteralDict() : MultiElemConstruct<LiteralDictKVPair>("LiteralDict") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Identifier final: public Construct {

public:
    std::string_view value;

    template <class T>
    Identifier(T &&arg) : Construct("Id"), value(forward<T>(arg)) { }

    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ExprList final: public MultiElemConstruct<> {

public:

    ExprList() : MultiElemConstruct<>("ExprList") { }
};

class IdList final: public MultiElemConstruct<Identifier> {

public:

    IdList()
        : MultiElemConstruct<Identifier>("IdList", ConstructType::idlist)
    { }
};

class CallExpr final: public Construct {

public:
    unique_ptr<Construct> what;
    unique_ptr<ExprList> args;

    CallExpr() : Construct("CallExpr") { }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr01 final: public SingleChildConstruct {

public:
    Expr01() : SingleChildConstruct("Expr01") { }
};

class Expr02 final: public MultiOpConstruct {

public:

    Expr02() : MultiOpConstruct("Expr02") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};


class Expr03 final: public MultiOpConstruct {

public:

    Expr03() : MultiOpConstruct("Expr03") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr04 final: public MultiOpConstruct {

public:

    Expr04() : MultiOpConstruct("Expr04") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr06 final: public MultiOpConstruct {

public:

    Expr06() : MultiOpConstruct("Expr06") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr07 final: public MultiOpConstruct {

public:

    Expr07() : MultiOpConstruct("Expr07") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr11 final: public MultiOpConstruct {

public:

    Expr11() : MultiOpConstruct("Expr11") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr12 final: public MultiOpConstruct {

public:

    Expr12() : MultiOpConstruct("Expr12") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr14 final: public Construct {

public:
    unique_ptr<Construct> lvalue;
    unique_ptr<Construct> rvalue;
    unsigned fl;
    Op op;

    Expr14() : Construct("Expr14"), fl(pNone), op(Op::invalid) { }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class IfStmt final: public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> thenBlock;
    unique_ptr<Construct> elseBlock;

    IfStmt() : Construct("IfStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class Block final: public MultiElemConstruct<> {

public:
    Block() : MultiElemConstruct("Block", ConstructType::block) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class BreakStmt final: public ChildlessConstruct {

public:
    BreakStmt(): ChildlessConstruct("BreakStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ContinueStmt final: public ChildlessConstruct {

public:
    ContinueStmt(): ChildlessConstruct("ContinueStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ReturnStmt final: public Construct {

public:
    unique_ptr<Construct> elem;

    ReturnStmt(): Construct("ReturnStmt", false, ConstructType::ret) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class WhileStmt final: public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> body;

    WhileStmt() : Construct("WhileStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class FuncDeclStmt final: public Construct {

public:
    unique_ptr<Identifier> id;  /* NULL when the func is defined inside an expr */
    unique_ptr<IdList> captures;
    unique_ptr<IdList> params;
    unique_ptr<Construct> body;

    FuncDeclStmt() : Construct("FuncDeclStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class Subscript final: public Construct {

public:

    unique_ptr<Construct> what;
    unique_ptr<Construct> index;

    Subscript() : Construct("Subscript") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class Slice final: public Construct {

public:

    unique_ptr<Construct> what;
    unique_ptr<Construct> start_idx;
    unique_ptr<Construct> end_idx;

    Slice() : Construct("Slice") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

struct AllowedExList {

    unique_ptr<IdList> exList;
    unique_ptr<Identifier> asId;
};

class TryCatchStmt final: public Construct {

public:

    unique_ptr<Construct> tryBody;
    unique_ptr<Construct> finallyBody;
    std::vector<std::pair<AllowedExList, unique_ptr<Construct>>> catchStmts;

    TryCatchStmt() : Construct("TryCatchStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class RethrowStmt final: public ChildlessConstruct {

public:

    RethrowStmt(Loc start = Loc(), Loc end = Loc())
        : ChildlessConstruct("RethrowStmt", start, end) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ThrowStmt final: public SingleChildConstruct {

public:
    ThrowStmt(): SingleChildConstruct("ThrowStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ForeachStmt final: public Construct {

    bool do_iter(EvalContext *ctx,
                 unsigned index,
                 const EvalValue *elems,
                 unsigned count) const;

public:
    unique_ptr<IdList> ids;
    unique_ptr<Construct> container;
    unique_ptr<Construct> body;
    bool idsVarDecl;
    bool indexed;

    ForeachStmt() : Construct("ForeachStmt"), idsVarDecl(false), indexed(false) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class MemberExpr final: public Construct {

public:

    unique_ptr<Construct> what;
    EvalValue memId;

    MemberExpr() : Construct("MemberExpr") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};
