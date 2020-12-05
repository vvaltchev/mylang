/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "evalvalue.h"
#include <map>

using namespace std;

class LValue {

    EvalValue val;

    void type_checks() const {
        assert(val.get_type()->t != Type::t_lval);
        assert(val.get_type()->t != Type::t_undefid);
    }

public:

    const bool is_const;

    LValue(const EvalValue &val, bool is_const = false)
        : val(val)
        , is_const(is_const)
    {
        type_checks();
    }

    LValue(EvalValue &&val, bool is_const = false)
        : val(forward<EvalValue>(val))
        , is_const(is_const)
    {
        type_checks();
    }

    LValue(const LValue &rhs) = delete;
    LValue(LValue &&rhs) = delete;
    LValue &operator=(const LValue &rhs) = delete;
    LValue &operator=(LValue &&rhs) = delete;

    void put(const EvalValue &v) { val = v; type_checks(); }
    void put(EvalValue &&v) { val = forward<EvalValue>(v); type_checks(); }

    EvalValue eval() const { return val; }
};

class EvalContext {

public:

    typedef map<string, shared_ptr<LValue>, less<>> SymbolsType;
    const bool const_ctx;

    EvalContext(bool const_ctx = false);
    SymbolsType symbols;

    static const SymbolsType builtins;
};
