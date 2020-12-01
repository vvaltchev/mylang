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

class SingleChildConstruct : public Construct {

public:
    Construct *elem;

    virtual ~SingleChildConstruct() {
        delete elem;
    }

    virtual void serialize(ostream &s, int level = 0) const;
};


class MultiOpConstruct : public Construct {

public:
    vector<pair<Op, Construct *>> elems;

    virtual ~MultiOpConstruct() {

        for (const auto &[op, e] : elems) {
            delete e;
        }
    }

    virtual void serialize(ostream &s, int level = 0) const;
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

    virtual EvalValue eval(EvalContext *ctx) const {
        return value;
    }

    virtual void serialize(ostream &s, int level = 0) const;
};


class Expr01 : public SingleChildConstruct {

public:
    virtual EvalValue eval(EvalContext *ctx) const {
        return elem->eval(ctx);
    }
};

class Expr03 : public MultiOpConstruct {

public:
    virtual EvalValue eval(EvalContext *ctx) const;
};

class Expr04 : public MultiOpConstruct {

public:
    virtual EvalValue eval(EvalContext *ctx) const;
};
