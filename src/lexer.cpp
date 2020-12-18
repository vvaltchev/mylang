/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "lexer.h"

#include <map>
#include <vector>

#include <cctype>

using namespace std;

static const map<string, Op, less<>> operators = [] {

    map<string, Op, less<>> ret;

    for (size_t i = 1; i < OpString.size(); i++)
        ret.emplace(OpString[i], (Op)i);

    return ret;
}();

static const map<string, Keyword, less<>> keywords = [] {

    map<string, Keyword, less<>> ret;

    for (size_t i = 1; i < KwString.size(); i++)
        ret.emplace(KwString[i], (Keyword)i);

    return ret;
}();

static Keyword get_keyword(string_view val)
{
    if (!val.empty()) {

        auto it = keywords.find(val);

        if (it != keywords.end())
            return it->second;
    }

    return Keyword::kw_invalid;
}

static Op get_op_type(string_view val)
{
    if (!val.empty()) {

        auto it = operators.find(val);

        if (it != operators.end())
            return it->second;
    }

    return Op::invalid;
}

inline bool is_operator(string_view s)
{
    return get_op_type(s) != Op::invalid;
}

inline bool is_keyword(string_view s)
{
    return get_keyword(s) != Keyword::kw_invalid;
}

ostream &operator<<(ostream &s, TokType t)
{
    static const char *tt_str[] =
    {
        "inv",
        "integer",
        "id_",
        "op_",
        "kw_",
        "str",
        "unk",
    };

    return s << tt_str[(int)t];
}

ostream &operator<<(ostream &s, const Tok &t)
{
    s << "Tok(" << t.type << "): '";

    if (t.type == TokType::op)
        s << OpString[(int)t.op];
    else if (t.type == TokType::kw)
        s << KwString[(int)t.kw];
    else
        s << t.value;

    s << "'";
    return s;
}

static void
append_token(vector<Tok> &result, TokType tt, int ln, int start, string_view val)
{
    if (tt == TokType::id) {

        Keyword kw = get_keyword(val);

        if (kw != Keyword::kw_invalid) {
            result.emplace_back(TokType::kw, Loc(ln, start + 1), kw);
            return;
        }
    }

    result.emplace_back(
        tt,
        Loc(ln, start + 1),
        val
    );
}

void
lexer(string_view in_str, int line, vector<Tok> &result)
{
    size_t i, tok_start = 0;
    TokType tok_type = TokType::invalid;

    for (i = 0; i < in_str.length(); i++) {

        const char c = in_str[i];

        if (tok_type == TokType::str) {

            if (c == '"') {

                append_token(result,
                             TokType::str,
                             line,
                             tok_start,
                             in_str.substr(tok_start, i - tok_start));

                tok_type = TokType::invalid;

            } else if (c == '\\') {

                if (i == in_str.length() - 1)
                    throw InvalidTokenEx{in_str.substr(tok_start, 1 + i - tok_start)};

                if (in_str[i + 1] == '"') {
                    i++; /* ignore \" instead of ending the token */
                }
            }

            continue;
        }

        if (c == '#')
            break; /* comment: stop the lexer, until the end of the line */

        if (tok_type == TokType::invalid) {

            tok_start = i;

            if (c == '"') {
                tok_type = TokType::str;
                tok_start++;
                continue;
            }
        }

        const string_view val = in_str.substr(tok_start, i - tok_start + 1);

        if (isspace(c) || is_operator(string_view(&c, 1))) {

            if (tok_type != TokType::invalid) {

                append_token(result,
                             tok_type,
                             line,
                             tok_start,
                             in_str.substr(tok_start, i - tok_start));

                tok_type = TokType::invalid;
            }

            if (!isspace(c)) {

                string_view op = in_str.substr(i, 1);

                if (i + 1 < in_str.length() && is_operator(in_str.substr(i, 2)))
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

                result.emplace_back(TokType::op, Loc(line, i+1), get_op_type(op));
            }

        } else if (isalnum(c) || c == '_') {

            if (tok_type == TokType::invalid) {

                tok_start = i;

                if (isdigit(c))
                    tok_type = TokType::integer;
                else
                    tok_type = TokType::id;

            } else if (tok_type == TokType::integer) {

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

    if (tok_type != TokType::invalid) {

        append_token(result,
                     tok_type,
                     line,
                     tok_start,
                     in_str.substr(tok_start, i - tok_start));
    }
}
