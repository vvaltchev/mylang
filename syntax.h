/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "defs.h"

class LiteralInt;
class Expr01;
class Expr03;
class Expr04;

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


class Expr01 : public Construct {

public:
    Construct *value;

    virtual ~Expr01() = default;

    virtual EvalValue eval(EvalContext *ctx) const {
        return value->eval(ctx);
    }

    virtual void serialize(ostream &s, int level = 0) const;
};

class Expr03 : public Construct {

public:
    vector<pair<Op, Expr01 *>> elems;

    virtual ~Expr03() = default;
    virtual EvalValue eval(EvalContext *ctx) const;
    virtual void serialize(ostream &s, int level = 0) const;
};

class Expr04 : public Construct {

public:
    vector<pair<Op, Expr03 *>> elems;

    virtual ~Expr04() = default;
    virtual EvalValue eval(EvalContext *ctx) const;
    virtual void serialize(ostream &s, int level = 0) const;
};
