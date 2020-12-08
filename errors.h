/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <memory>
#include <string_view>

#include "operators.h"

using namespace std;

struct Loc {

    int line;
    int col;

    Loc() : line(0), col(0) { }
    Loc(int line, int col): line(line), col(col) { }

    operator bool() const {
        return col != 0;
    }
};

class Tok;
class Construct;

struct Exception {

    const char *const name;
    Loc loc_start;
    Loc loc_end;

    Exception(const char *name, Loc start = Loc(), Loc end = Loc())
        : name(name)
        , loc_start(start)
        , loc_end(end)
    { }

    virtual ~Exception() = default;
};


#define DECL_SIMPLE_EX(name)                       \
                                                   \
    struct name : public Exception {               \
                                                   \
        name(Loc start = Loc(), Loc end = Loc())   \
            : Exception(#name, start, end)         \
        { }                                        \
    };

struct InvalidTokenEx : public Exception {

    const string_view val;

    InvalidTokenEx(const string_view &val)
        : Exception("InvalidTokenEx")
        , val(val)
    { }
};

DECL_SIMPLE_EX(InternalErrorEx)
DECL_SIMPLE_EX(CannotRebindConstEx)
DECL_SIMPLE_EX(ExpressionIsNotConstEx)
DECL_SIMPLE_EX(DivisionByZeroEx)
DECL_SIMPLE_EX(TypeErrorEx)
DECL_SIMPLE_EX(AlreadyDefinedEx)
DECL_SIMPLE_EX(InvalidArgumentEx)
DECL_SIMPLE_EX(AssertionFailureEx)
DECL_SIMPLE_EX(NotLValueEx)

struct UndefinedVariableEx : public Exception {

    const string_view name;

    UndefinedVariableEx(const string_view &name, Loc start = Loc(), Loc end = Loc())
        : Exception("UndefinedVariableEx")
        , name(name)
    { }
};

struct SyntaxErrorEx : public Exception {

    const char *const msg;
    const Tok *const tok;
    const Op op;

    SyntaxErrorEx(Loc loc_start,
                  const char *msg,
                  const Tok *tok = nullptr,
                  Op op = Op::invalid)
        : Exception("SyntaxError", loc_start)
        , msg(msg)
        , tok(tok)
        , op(op)
    { }
};
