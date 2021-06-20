/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"

#include <string_view>
#include "operators.h"

struct Loc {

    int line;
    int col;

    Loc() : line(0), col(0) { }

    template <class T, class U>
    Loc(T line, U col)
        : line(static_cast<int>(line))
        , col(static_cast<int>(col))
    { }

    operator bool() const {
        return col != 0;
    }

    Loc operator+(size_t n) const {

        if (!col)
            return Loc();

        return Loc(line, col + static_cast<int>(n));
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
    const char *const msg;
    Loc loc_start;
    Loc loc_end;

    Exception(const char *name,
              const char *msg,
              Loc start = Loc(),
              Loc end = Loc())
        : name(name)
        , msg(msg)
        , loc_start(start)
        , loc_end(end)
    { }

    virtual ~Exception() = default;
};

struct RuntimeException : public Exception {

    RuntimeException(const char *name,
                     const char *msg,
                     Loc start = Loc(),
                     Loc end = Loc())
        : Exception(name, msg, start, end)
    { }

    virtual RuntimeException *clone() const = 0;
    [[ noreturn ]] virtual void rethrow() const = 0;
};

#define DECL_SIMPLE_EX(name, msg)                  \
                                                   \
    struct name : public Exception {               \
                                                   \
        name(Loc start = Loc(), Loc end = Loc())   \
            : Exception(#name, msg, start, end)    \
        { }                                        \
    };

#define DECL_RUNTIME_EX(name, msg)                        \
                                                          \
    struct name : public RuntimeException {               \
                                                          \
        name(Loc start = Loc(), Loc end = Loc())          \
            : RuntimeException(#name, msg, start, end)    \
        { }                                               \
                                                          \
        name(const char *custom_msg,                      \
             Loc s = Loc(), Loc e = Loc())                \
            : RuntimeException(#name, custom_msg, s, e)   \
        { }                                               \
                                                          \
        name *clone() const override {                    \
            return new name(*this);                       \
        }                                                 \
                                                          \
        [[ noreturn ]] void rethrow() const override {    \
            throw *this;                                  \
        }                                                 \
    };

struct InvalidTokenEx : public Exception {

    const std::string_view val;

    InvalidTokenEx(const std::string_view &val)
        : Exception("InvalidTokenEx", "Invalid token error")
        , val(val)
    { }
};

DECL_SIMPLE_EX(InternalErrorEx, "Internal error")
DECL_SIMPLE_EX(CannotRebindConstEx, "Cannot rebind const")
DECL_SIMPLE_EX(CannotRebindBuiltinEx, "Cannot rebind builtin")
DECL_SIMPLE_EX(ExpressionIsNotConstEx, "The expression is not const")
DECL_SIMPLE_EX(AlreadyDefinedEx, "Already defined error")
DECL_SIMPLE_EX(InvalidArgumentEx, "Invalid argument error")
DECL_SIMPLE_EX(InvalidNumberOfArgsEx, "Invalid number of arguments error")
DECL_SIMPLE_EX(CannotChangeConstEx, "Cannot change constant")

/* Runtime errors */
DECL_RUNTIME_EX(DivisionByZeroEx, "Division by zero")
DECL_RUNTIME_EX(AssertionFailureEx, "Assertion failure")
DECL_RUNTIME_EX(NotLValueEx, "Not an lvalue error")
DECL_RUNTIME_EX(TypeErrorEx, "Type error")
DECL_RUNTIME_EX(InvalidValueEx, "Invalid value error")
DECL_RUNTIME_EX(NotCallableEx, "Not a callable object")
DECL_RUNTIME_EX(OutOfBoundsEx, "Out of bounds error")
DECL_RUNTIME_EX(CannotOpenFileEx, "Cannot open file error")

struct UndefinedVariableEx : public Exception {

    const std::string_view name;
    bool in_pure_func;

    UndefinedVariableEx(const std::string_view &name, Loc start = Loc(), Loc end = Loc())
        : Exception("UndefinedVariable", nullptr, start, end)
        , name(name)
        , in_pure_func(false)
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
        : Exception("SyntaxError", nullptr, loc_start)
        , msg(msg)
        , tok(tok)
        , op(op)
    { }
};
