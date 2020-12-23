/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "lexer.h"

#include <map>
#include <vector>
#include <cctype>

const Tok invalid_tok;

using std::string;
using std::string_view;

static const std::map<string, Op, std::less<>> operators = [] {

    std::map<string, Op, std::less<>> ret;

    for (size_t i = 1; i < OpString.size(); i++)
        ret.emplace(OpString[i], (Op)i);

    return ret;
}();

static const std::map<string, Keyword, std::less<>> keywords = [] {

    std::map<string, Keyword, std::less<>> ret;

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
        "int",
        "id_",
        "op_",
        "kw_",
        "str",
        "flt",
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

struct lexer_ctx {

    /* Input params */
    const string_view in_str;
    std::vector<Tok> &result;
    const int line;

    /* State variables */
    unsigned i = 0;
    unsigned tok_start = 0;
    bool float_exp = false;
    TokType tok_type = TokType::invalid;

    lexer_ctx(const string_view &in_str, int line, std::vector<Tok> &result)
        : in_str(in_str)
        , result(result)
        , line(line)
    { }

    void accept_token();
    void invalid_token();

    void handle_in_str();
    void handle_space_or_op();
    void handle_alphanum();
    void handle_other();
};

void
lexer_ctx::invalid_token()
{
    throw InvalidTokenEx(in_str.substr(tok_start, i - tok_start + 1));
}

void
lexer_ctx::accept_token()
{
    const string_view &val = in_str.substr(tok_start, i - tok_start);

    if (tok_type == TokType::id) {

        Keyword kw = get_keyword(val);

        if (kw != Keyword::kw_invalid) {
            result.emplace_back(TokType::kw, Loc(line, tok_start + 1), kw);
            return;
        }
    }

    result.emplace_back(
        tok_type,
        Loc(line, tok_start + 1),
        val
    );
}

void
lexer_ctx::handle_in_str()
{
    const char c = in_str[i];

    if (c == '"') {

        accept_token();
        tok_type = TokType::invalid;

    } else if (c == '\\') {

        if (i == in_str.length() - 1)
            invalid_token();

        if (in_str[i + 1] == '"')
            i++; /* ignore \" instead of ending the token */
    }
}

void
lexer_ctx::handle_space_or_op()
{
    const char c = in_str[i];

    if (tok_type != TokType::invalid) {

        accept_token();
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
}

void
lexer_ctx::handle_alphanum()
{
    const char c = in_str[i];

    if (tok_type == TokType::invalid) {

        tok_start = i;

        if (isdigit(c))
            tok_type = TokType::integer;
        else if (c == '.')
            tok_type = TokType::floatnum;
        else
            tok_type = TokType::id;

    } else if (tok_type == TokType::integer) {

        if (c == '.' || c == 'e') {

            tok_type = TokType::floatnum;
            float_exp = c == 'e';

        } else {

            if (!isdigit(c))
                invalid_token();
        }

    } else if (tok_type == TokType::floatnum) {

        if (c == 'e') {

            if (!float_exp) {

                float_exp = true;

            } else {

                invalid_token();
            }

        } else {

            if (!isdigit(c))
                invalid_token();
        }
    }
}

void
lexer_ctx::handle_other()
{
    const char c = in_str[i];

    if (tok_type == TokType::invalid) {

        if (c == '"') {

            /* Start a string token */
            tok_type = TokType::str;
            tok_start++;

        } else {

            /* Start token of unknown type */
            tok_start = i;
            tok_type = TokType::unknown;
        }

    } else {

        /*
         * When we have a valid token, we should never get here in valid
         * programs. When this happens, it means that we hit an invalid token
         * (e.g. abc$): until the prev. char (abc) it was a valid ID, then we
         * hit "$" and that's made the whole token invalid.
         */
        invalid_token();
    }
}

void
lexer(string_view in_str, int line, std::vector<Tok> &result)
{
    lexer_ctx ctx(in_str, line, result);

    for (ctx.i = 0; ctx.i < in_str.length(); ctx.i++) {

        const char c = in_str[ctx.i];

        if (ctx.tok_type == TokType::str) {

            ctx.handle_in_str();

        } else {

            if (c == '#')
                break; /* comment: stop the lexer, until the end of the line */

            if (ctx.tok_type == TokType::invalid)
                ctx.tok_start = ctx.i;

            const bool is_op = is_operator(string_view(&c, 1));
            const bool in_integer = ctx.tok_type == TokType::integer;

            if (isspace(c) || (is_op && (!in_integer || c != '.')))
                ctx.handle_space_or_op();
            else if (isalnum(c) || c == '_' || c == '.')
                ctx.handle_alphanum();
            else
                ctx.handle_other();
        }
    }

    if (ctx.tok_type != TokType::invalid) {

        if (ctx.tok_type == TokType::str) {
            /* This happens in case of an unterminated string literal */
            assert(ctx.tok_start > 0);
            ctx.tok_start--; /* Include " in the invalid token */
            ctx.invalid_token();
        }

        ctx.accept_token();
    }
}
