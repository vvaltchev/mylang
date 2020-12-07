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
};

class Tok;
class Construct;

struct Exception { };

struct InternalErrorEx : public Exception { };

struct InvalidTokenEx : public Exception {
    const string_view val;
    InvalidTokenEx(const string_view &val) : val(val) { }
};

struct CannotRebindConstEx : public Exception {
    const Loc loc;
    CannotRebindConstEx(Loc loc = Loc()) : loc(loc) { }
};

struct ExpressionIsNotConstEx : public Exception {
    const Loc loc;
    ExpressionIsNotConstEx(Loc loc = Loc()) : loc(loc) { }
};

struct DivisionByZeroEx : public Exception {
    const Loc loc;
    DivisionByZeroEx(Loc loc = Loc()) : loc(loc) { }
};

struct TypeErrorEx : public Exception {
    const Loc loc;
    TypeErrorEx(Loc loc = Loc()) : loc(loc) { }
};

struct AlreadyDefinedEx : public Exception {
    const Loc loc;
    AlreadyDefinedEx(Loc loc = Loc()) : loc(loc) { }
};

struct InvalidArgumentEx : public Exception {
    const Loc loc;
    InvalidArgumentEx(Loc loc = Loc()) : loc(loc) { }
};

struct AssertionFailureEx : public Exception {
    const Loc loc;
    AssertionFailureEx(Loc loc = Loc()) : loc(loc) { }
};

struct NotLValueEx : public Exception {
    const Loc loc;
    NotLValueEx(Loc loc = Loc()) : loc(loc) { }
};

struct UndefinedVariableEx : public Exception {
    const string_view name;
    const Loc loc;
    UndefinedVariableEx(const string_view &name, Loc loc = Loc())
        : name(name), loc(loc) { }
};

struct SyntaxErrorEx {

    const Loc loc;
    const char *const msg;
    const Tok *const tok;
    const Op op;

    SyntaxErrorEx(Loc loc,
                  const char *msg,
                  const Tok *tok = nullptr,
                  Op op = Op::invalid)
        : loc(loc)
        , msg(msg)
        , tok(tok)
        , op(op)
    { }
};
