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

    num     = 1,   /* literal integer */
    id      = 2,   /* identifier (e.g. a, x, my_var) */
    op      = 3,   /* operator (e.g. +, -, *, /) */
    kw      = 4,   /* keyword (e.g. if, else, while) */
    unknown = 5,   /* something else (e.g. ?) */
};

static const array<string, 20> OpString =
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
    "{",
    "}",
};

enum class Keyword : int {

    kw_invalid  = 0,

    kw_if       = 1,
    kw_else     = 2,
    kw_while    = 3,
    kw_for      = 4,
};

static const array<string, 18> KwString =
{
    "invalid",

    "if",
    "else",
    "while",
    "for",
};

ostream &operator<<(ostream &s, TokType t);

class Tok {

public:

    const TokType type;
    const Loc loc;
    const string_view value;
    const Op op;
    const Keyword kw;

    Tok() : type(TokType::invalid), op(Op::invalid), kw(Keyword::kw_invalid) { }

    Tok(TokType type, Loc loc, string_view value)
        : type(type)
        , loc(loc)
        , value(value)
        , op(Op::invalid)
        , kw(Keyword::kw_invalid)
    { }

    Tok(TokType type, Loc loc, Op op)
        : type(type)
        , loc(loc)
        , op(op)
        , kw(Keyword::kw_invalid)
    { }

    Tok(TokType type, Loc loc, Keyword kw)
        : type(type)
        , loc(loc)
        , op(Op::invalid)
        , kw(kw)
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

    bool operator==(Keyword rhs) const {
        return kw == rhs;
    }

    bool operator!=(Keyword rhs) const {
        return kw != rhs;
    }
};

static const Tok invalid_tok;

ostream &operator<<(ostream &s, const Tok &t);
void lexer(string_view in_str, int line, vector<Tok> &result);

