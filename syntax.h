/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "defs.h"

class Literal;
class LiteralInt;
class Expr01;
class Expr03;
class Expr04;
class Expr06;
class Stmt;
class Block;

class EvalValue {

public:
    long value;

    EvalValue(long v) : value(v) { }
};

class EvalContext {

public:
};

class Construct {

public:
    Construct() = default;
    virtual ~Construct() = default;
    virtual EvalValue eval(EvalContext *) const = 0;
    virtual void serialize(ostream &s, int level = 0) const = 0;
};

class SingleChildConstruct : public Construct {

public:
    unique_ptr<Construct> elem;

    virtual EvalValue eval(EvalContext *ctx) const {
        return elem->eval(ctx);
    }

    virtual void serialize(ostream &s, int level = 0) const;
};


class MultiOpConstruct : public Construct {

public:
    vector<pair<Op, unique_ptr<Construct>>> elems;
    virtual void serialize(ostream &s, int level = 0) const;
};

class MultiElemConstruct : public Construct {

public:
    vector<unique_ptr<Construct>> elems;
    virtual void serialize(ostream &s, int level = 0) const;
};

inline ostream &operator<<(ostream &s, const Construct &c)
{
    c.serialize(s);
    return s;
}

class Literal : public Construct {

public:
    long value;

    Literal(long v) : value(v) { }
};

class LiteralInt : public Literal {

public:

    LiteralInt(long v) : Literal(v) { }

    virtual EvalValue eval(EvalContext *ctx) const {
        return value;
    }

    virtual void serialize(ostream &s, int level = 0) const;
};


class Expr01 : public SingleChildConstruct { };

class Expr03 : public MultiOpConstruct {

public:
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr04 : public MultiOpConstruct {

public:
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr06 : public MultiOpConstruct {

public:
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Stmt : public SingleChildConstruct { };

class Block : public MultiElemConstruct {

public:
    virtual EvalValue eval(EvalContext *ctx) const;
};
