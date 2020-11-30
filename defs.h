/* SPDX-License-Identifier: BSD-2-Clause */

#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <string_view>
#include <sstream>

#include <cassert>

using namespace std;

enum class TokType {
    invalid = 0,
    num = 1,
    id = 2,
    op = 3,
    unknown = 4,
};

enum class Op {

    invalid,

    plus,
    minus,
    times,
    div,
    parenL,
    parenR,
};

static const string OpToString[] =
{
    "invalid",

    "+",
    "-",
    "*",
    "/",
    "(",
    ")",
};

struct InvalidTokenEx {
    string_view val;
};

struct SyntaxErrorEx { };
struct DivisionByZeroEx { };

Op get_op_type(string_view val);
ostream &operator<<(ostream &s, TokType t);

class Tok {

public:

    const string_view value;
    const TokType type;
    const Op op;

    Tok(TokType type = TokType::invalid, string_view value = string_view())
        : type(type)
        , value(value)
        , op(value.empty() ? Op::invalid : get_op_type(value))
    { }

    Tok(Op op)
        : type(TokType::op)
        , op(op)
    { }

    Tok(const Tok &rhs) = default;
    Tok &operator=(const Tok &rhs) = delete;

    bool operator==(Op rhs) const {
        return op == rhs;
    }

    bool operator==(TokType t) const {
        return type == t;
    }

    bool operator==(const Tok &rhs) const {

        if (type == TokType::op)
            return op == rhs.op;

        return type == rhs.type;
    }

    // operator string() const {
    //     stringstream ss;
    //     ss << "Tok(" << type << "): '" << value << "'";
    //     return ss.str();
    // }
};

inline ostream &operator<<(ostream &s, const Tok &t)
{
    return s << "Tok(" << t.type << "): '" << t.value << "'";
}

static const set<string, less<>> operators = {
    "+", "-", "*", "/", "(", ")",
};

inline bool is_operator(string_view s)
{
    return operators.find(s) != operators.end();
}

void lexer(string_view in_str, vector<Tok> &result);