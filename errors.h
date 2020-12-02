/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <string_view>

#include "operators.h"

using namespace std;

struct Loc {

    int line;
    int col;

    Loc() : line(0), col(0) { }
    Loc(int line, int col): line(line), col(col) { }
};

struct InvalidTokenEx { string_view val; };
struct DivisionByZeroEx { };
struct TypeErrorEx { };
struct UndefinedVariableEx { string_view name; };
struct NotLValueEx { };
struct InternalErrorEx { };

class Tok;

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
