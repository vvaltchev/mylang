/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "evalvalue.h"
#include <map>

class ExprList;

struct Builtin {
    EvalValue (*func)(ExprList *);
};

class LValue {

    EvalValue val;

    void type_checks() const {
        assert(val.type->t != Type::t_lval);
        assert(val.type->t != Type::t_undefid);
    }

public:

    LValue(const EvalValue &val) : val(val) { type_checks(); }
    LValue(EvalValue &&val) : val(forward<EvalValue>(val)) { type_checks(); }
    LValue(const LValue &rhs) = delete;
    LValue(LValue &&rhs) = delete;
    LValue &operator=(const LValue &rhs) = delete;
    LValue &operator=(LValue &&rhs) = delete;

    void put(const EvalValue &v) { val = v; type_checks(); }
    void put(EvalValue &&v) { val = v; type_checks(); }

    EvalValue eval() const { return val; }
};

ostream &operator<<(ostream &s, const EvalValue &c);

class EvalContext {

public:

    typedef map<string, shared_ptr<LValue>, less<>> SymbolsType;

    EvalContext();
    SymbolsType symbols;

    static const SymbolsType builtins;
};
