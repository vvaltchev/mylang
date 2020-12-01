/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "lexer.h"

#include <set>
#include <vector>

#include <cctype>

using namespace std;

static const set<string, less<>> operators = [] {
    return set<string, less<>>(
        OpString.begin() + 1, OpString.end()
    );
}();

inline bool is_operator(string_view s)
{
    return operators.find(s) != operators.end();
}

Op get_op_type(string_view val)
{
    if (val.empty())
        return Op::invalid;

    switch (val[0]) {

        case '+':
            return Op::plus;

        case '-':
            return Op::minus;

        case '*':
            return Op::times;

        case '/':
            return Op::div;

        case '(':
            return Op::parenL;

        case ')':
            return Op::parenR;

        case ';':
            return Op::semicolon;

        case ',':
            return Op::comma;

        case '%':
            return Op::mod;

        case '!':
            return Op::opnot;

        case '<':

            if (val.length() > 1 && val[1] == '=')
                return Op::le;

            return Op::lt;

        case '>':

            if (val.length() > 1 && val[1] == '=')
                return Op::ge;

            return Op::gt;

        default:
            return Op::invalid;
    }
}

ostream &operator<<(ostream &s, TokType t)
{
    static const char *tt_str[] =
    {
        "inv",
        "num",
        "id_",
        "op_",
        "unk",
    };

    return s << tt_str[(int)t];
}

void
lexer(string_view in_str, vector<Tok> &result)
{
    size_t i, tok_start = 0;
    TokType tok_type = TokType::invalid;

    for (i = 0; i < in_str.length(); i++) {

        if (tok_type == TokType::invalid)
            tok_start = i;

        const char c = in_str[i];
        string_view val = in_str.substr(tok_start, i - tok_start + 1);
        string_view val_until_prev = in_str.substr(tok_start, i - tok_start);

        if (isspace(c) || is_operator(string_view(&c, 1))) {

            if (tok_type != TokType::invalid) {
                result.emplace_back(tok_type, val_until_prev);
                tok_type = TokType::invalid;
            }

            if (!isspace(c)) {

                string_view op = in_str.substr(i, 1);

                if (i + 1 < in_str.length() &&
                    is_operator(in_str.substr(i, 2)))
                {
                    /*
                     * Handle two-chars wide operators. Note: it is required,
                     * with the current implementation, an 1-char prefix operator to
                     * exist for each one of them. For example:
                     *
                     *      <= requires '<' to exist independently
                     *      += requires '+' to exist independently
                     *
                     * Reason: the check `is_operator(string_view(&c, 1))` above.
                     */
                    op = in_str.substr(i, 2);
                    i++;
                }

                result.emplace_back(TokType::op, op);
            }

        } else if (isalnum(c) || c == '_') {

            if (tok_type == TokType::invalid) {

                tok_start = i;

                if (isdigit(c))
                    tok_type = TokType::num;
                else
                    tok_type = TokType::id;

            } else if (tok_type == TokType::num) {

                if (!isdigit(c))
                    throw InvalidTokenEx{val};
            }

        } else {

            if (tok_type != TokType::invalid)
                throw InvalidTokenEx{val};

            tok_start = i;
            tok_type = TokType::unknown;
        }
    }

    if (tok_type != TokType::invalid)
        result.emplace_back(tok_type, in_str.substr(tok_start, i - tok_start));
}
