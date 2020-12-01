/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "defs.h"

class LiteralInt;
class Factor;
class Term;
class Expr;

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

inline ostream &operator<<(ostream &s, const Construct &c)
{
    c.serialize(s);
    return s;
}

class LiteralInt : public Construct {

public:
    long value;

    LiteralInt(long v) : value(v) { }

    virtual ~LiteralInt() = default;

    virtual EvalValue eval(EvalContext *ctx) const {
        return value;
    }

    virtual void serialize(ostream &s, int level = 0) const;
};


class Factor : public Construct {

public:
    Construct *value;

    virtual ~Factor() = default;

    virtual EvalValue eval(EvalContext *ctx) const {
        return value->eval(ctx);
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class Term : public Construct {

public:
    vector<pair<Op, Factor *>> factors;

    virtual ~Term() = default;
    virtual EvalValue eval(EvalContext *ctx) const;
    virtual void serialize(ostream &s, int level = 0) const;
};

class Expr : public Construct {

public:
    vector<pair<Op, Term *>> terms;

    virtual ~Expr() = default;
    virtual EvalValue eval(EvalContext *ctx) const;
    virtual void serialize(ostream &s, int level = 0) const;
};
