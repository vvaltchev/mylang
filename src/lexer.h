/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"

#include <array>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <string_view>
#include <cassert>

enum class TokType : int {

    invalid     = 0,   /* no token */

    integer     = 1,   /* literal integer */
    id          = 2,   /* identifier (e.g. a, x, my_var) */
    op          = 3,   /* operator (e.g. +, -, *, /) */
    kw          = 4,   /* keyword (e.g. if, else, while) */
    str         = 5,   /* string literal (e.g. "hello") */
    floatnum    = 6,   /* literal real number (floating point, e.g. 1.23) */
    unknown     = 7,   /* something else (e.g. ?) */

    count       = 8,
};

static const std::array<std::string, (int)Op::op_count> OpString =
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
    "&",
    "|",
    "~",
    "&&",
    "||",
    "+=",
    "-=",
    "*=",
    "/=",
    "%=",
    "[",
    "]",
    "=>",
    ":",
    ".",
};

enum class Keyword : int {

    kw_invalid  = 0,

    kw_if       = 1,
    kw_else     = 2,
    kw_while    = 3,
    kw_for      = 4,
    kw_break    = 5,
    kw_continue = 6,
    kw_const    = 7,
    kw_var      = 8,
    kw_none     = 9,
    kw_func     = 10,
    kw_return   = 11,
    kw_try      = 12,
    kw_catch    = 13,
    kw_finally  = 14,
    kw_rethrow  = 15,
    kw_throw    = 16,
    kw_as       = 17,
    kw_true     = 18,
    kw_false    = 19,
    kw_foreach  = 20,
    kw_in       = 21,
    kw_indexed  = 22,
    kw_pure     = 23,

    kw_count    = 24,
};

static const std::array<std::string, (int)Keyword::kw_count> KwString =
{
    "invalid",

    "if",
    "else",
    "while",
    "for",
    "break",
    "continue",
    "const",
    "var",
    "none",
    "func",
    "return",
    "try",
    "catch",
    "finally",
    "rethrow",
    "throw",
    "as",
    "true",
    "false",
    "foreach",
    "in",
    "indexed",
    "pure",
};

std::ostream &operator<<(std::ostream &s, TokType t);

class Tok {

public:

    const TokType type;
    const Loc loc;
    const std::string_view value;
    const Op op;
    const Keyword kw;

    Tok() : type(TokType::invalid), op(Op::invalid), kw(Keyword::kw_invalid) { }

    Tok(TokType type, Loc loc, std::string_view value)
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

extern const Tok invalid_tok;

std::ostream &operator<<(std::ostream &s, const Tok &t);
void lexer(std::string_view in_str, int line, std::vector<Tok> &result);

