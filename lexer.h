/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <array>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <string_view>
#include <cassert>

/*
 * For small C++ projects, often using std everywhere is better because
 * it reduces the clutter (no "std::") and makes the code look cleaner.
 */
using namespace std;

enum class TokType : int {

    invalid = 0,   /* no token */

    num = 1,       /* literal integer */
    id = 2,        /* identifier (e.g. a, x, my_var) */
    op = 3,        /* operator (e.g. +, -, *, /) */
    unknown = 4,   /* something else (e.g. ?) */
};

enum class Op : int {

    invalid = 0,

    plus = 1,
    minus = 2,
    times = 3,
    div = 4,
    parenL = 5,
    parenR = 6,
    lt = 7,
    gt = 8,
    le = 9,
    ge = 10,
    semicolon = 11,
    comma = 12,
    mod = 13,
    opnot = 14,
    assign = 15,
    eq = 16,
    noteq = 17,
};

static const array<string, 18> OpString =
{
    "invalid",

    "+",
    "-",
    "*",
    "/",
    "(",
    ")",
    "<",
    ">",
    "<=",
    ">=",
    ";",
    ",",
    "%",
    "!",
    "=",
    "==",
    "!=",
};


Op get_op_type(string_view val);
ostream &operator<<(ostream &s, TokType t);

class Tok {

public:

    const TokType type;
    const Loc loc;
    const string_view value;
    const Op op;

    Tok(TokType type = TokType::invalid, Loc loc = Loc(), string_view value = string_view())
        : type(type)
        , loc(loc)
        , value(value)
        , op(value.empty() ? Op::invalid : get_op_type(value))

    { }

    Tok(Op op, Loc loc)
        : type(TokType::op)
        , loc(loc)
        , op(op)
    { }

    Tok(const Tok &rhs) = default;
    Tok &operator=(const Tok &rhs) = delete;

    bool operator==(Op rhs) const {
        return op == rhs;
    }

    bool operator!=(Op rhs) const {
        return op != rhs;
    }

    bool operator==(TokType t) const {
        return type == t;
    }

    bool operator!=(TokType t) const {
        return type != t;
    }
};

static const Tok invalid_tok;

inline ostream &operator<<(ostream &s, const Tok &t)
{
    return s << "Tok(" << t.type << "): '" << t.value << "'";
}

void lexer(string_view in_str, int line, vector<Tok> &result);

