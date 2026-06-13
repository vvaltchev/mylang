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
    size_type i = 0;
    size_type tok_start = 0;
    bool float_exp = false;       /* an 'e' has been seen in the current float */
    bool exp_sign_ok = false;     /* next char may be the exponent's +/- sign */
    bool exp_need_digit = false;  /* the exponent still needs at least one digit */
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
    if (tok_type == TokType::floatnum && exp_need_digit)
        invalid_token(); /* float exponent with no digits, e.g. "1e" or "1e-" */

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

        /*
         * Skip the escaped char, whatever it is, so it cannot end the
         * string. In particular `\\` must consume the second backslash,
         * otherwise it would later be treated as an escape and could
         * swallow the closing quote. This matches unescape_str(), which
         * treats `\X` as a 2-char unit for every X.
         */
        i++;
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

    if (isspace(c))
        return;

    string_view op = in_str.substr(i, 1);

    if (i + 1 < in_str.length()) {

        if (c == '.') {

            if (isdigit(in_str[i + 1])) {

                tok_start = i;
                tok_type = TokType::floatnum;
                i++;
                return;
            }
        }

        if (is_operator(in_str.substr(i, 2))) {

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
    }

    result.emplace_back(TokType::op, Loc(line, i+1), get_op_type(op));
}

void
lexer_ctx::handle_alphanum()
{
    const char c = in_str[i];

    if (tok_type == TokType::invalid) {

        tok_start = i;
        float_exp = false;
        exp_sign_ok = false;
        exp_need_digit = false;

        if (isdigit(c))
            tok_type = TokType::integer;
        else if (c == '.')
            tok_type = TokType::floatnum;
        else
            tok_type = TokType::id;

    } else if (tok_type == TokType::integer) {

        if (c == '.') {

            tok_type = TokType::floatnum;

        } else if (c == 'e') {

            tok_type = TokType::floatnum;
            float_exp = true;
            exp_sign_ok = true;
            exp_need_digit = true;

        } else if (!isdigit(c)) {

            invalid_token();
        }

    } else if (tok_type == TokType::floatnum) {

        if (c == 'e') {

            if (float_exp)
                invalid_token(); /* a second 'e' in the same float */

            float_exp = true;
            exp_sign_ok = true;
            exp_need_digit = true;

        } else if (exp_sign_ok && (c == '+' || c == '-')) {

            /* a +/- sign is allowed only right after the exponent's 'e' */
            exp_sign_ok = false;

        } else if (isdigit(c)) {

            exp_sign_ok = false;
            exp_need_digit = false;

        } else {

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
            const bool exp_sign = ctx.exp_sign_ok && (c == '+' || c == '-');

            if (!exp_sign && (isspace(c) || (is_op && (!in_integer || c != '.'))))
                ctx.handle_space_or_op();
            else if (exp_sign || isalnum(c) || c == '_' || c == '.')
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
