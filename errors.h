/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

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

    Loc operator+(size_t n) const {

        if (!col)
            return Loc();

        return Loc(line, col + n);
    }

    Loc operator+(int n) const {

        if (!col)
            return Loc();

        return Loc(line, col + n);
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


#define DECL_SIMPLE_EX(name, human_name)           \
                                                   \
    struct name : public Exception {               \
                                                   \
        name(Loc start = Loc(), Loc end = Loc())   \
            : Exception(human_name, start, end)    \
        { }                                        \
    };

struct InvalidTokenEx : public Exception {

    const string_view val;

    InvalidTokenEx(const string_view &val)
        : Exception("InvalidTokenEx")
        , val(val)
    { }
};

DECL_SIMPLE_EX(InternalErrorEx, "Internal error")
DECL_SIMPLE_EX(CannotRebindConstEx, "Cannot rebind const")
DECL_SIMPLE_EX(CannotRebindBuiltinEx, "Cannot rebind builtin")
DECL_SIMPLE_EX(ExpressionIsNotConstEx, "The expression is not const")
DECL_SIMPLE_EX(DivisionByZeroEx, "Division by zero")
DECL_SIMPLE_EX(AlreadyDefinedEx, "Already defined error")
DECL_SIMPLE_EX(AssertionFailureEx, "Assertion failure")
DECL_SIMPLE_EX(NotLValueEx, "Not an lvalue error")
DECL_SIMPLE_EX(InvalidArgumentEx, "Invalid argument error")
DECL_SIMPLE_EX(TooManyArgsEx, "Too many arguments error")
DECL_SIMPLE_EX(TooFewArgsEx, "Too few arguments error")
DECL_SIMPLE_EX(TypeErrorEx, "Type error")
DECL_SIMPLE_EX(NotCallableEx, "Not a callable object")

struct UndefinedVariableEx : public Exception {

    const string_view name;

    UndefinedVariableEx(const string_view &name, Loc start = Loc(), Loc end = Loc())
        : Exception("UndefinedVariableEx", start, end)
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
