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

/*
 * The lexer scans the WHOLE source buffer at once (not line by line), tracking
 * the current line + the offset of that line's start so each token's Loc is
 * (line, column). This is what lets a string literal or a C-style block
 * comment span newlines: the embedded '\n' is ordinary content, the line just
 * advances. `\n` outside a string/comment is whitespace; `cur_loc()` gives a
 * token's start Loc, captured into `tok_loc` when the token begins (so a
 * multi-line string reports the loc of its opening quote, not its close).
 */
struct lexer_ctx {

    /* Input params */
    const string_view in_str;
    std::vector<Tok> &result;

    /* State variables */
    size_type i = 0;
    size_type tok_start = 0;
    int cur_line;                 /* line of the char at `i` (1-based) */
    size_type line_start = 0;     /* offset where the current line begins */
    Loc tok_loc;                  /* the in-progress token's start Loc */
    bool float_exp = false;       /* an 'e' has been seen in this float */
    bool exp_sign_ok = false;     /* next char may be the exponent's +/- sign */
    bool exp_need_digit = false;  /* the exponent still needs a digit */
    TokType tok_type = TokType::invalid;

    lexer_ctx(const string_view &in_str, int line, std::vector<Tok> &result)
        : in_str(in_str)
        , result(result)
        , cur_line(line)
    { }

    Loc cur_loc() const { return Loc(cur_line, i - line_start + 1); }

    void accept_token();
    void flush_pending();
    [[noreturn]] void invalid_token();
    void newline();               /* bump the line counter at a '\n' */

    void handle_in_str();
    void handle_space_or_op();
    void handle_alphanum();
    void handle_other();
    void skip_block_comment();
};

void
lexer_ctx::newline()
{
    cur_line++;
    line_start = i + 1;
}

void
lexer_ctx::invalid_token()
{
    /* Carry the offending token's source span so the error renders a caret
     * like every other one (end = last-char col + 2, the project convention -
     * the bad token is in_str[tok_start .. i]); tok_loc is its start. */
    throw InvalidTokenEx(in_str.substr(tok_start, i - tok_start + 1),
                         tok_loc, Loc(cur_line, i - line_start + 3));
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
            result.emplace_back(TokType::kw, tok_loc, kw);
            return;
        }
    }

    result.emplace_back(tok_type, tok_loc, val);
}

/* Accept any in-progress non-string token (called before a comment starts). */
void
lexer_ctx::flush_pending()
{
    if (tok_type != TokType::invalid) {
        accept_token();
        tok_type = TokType::invalid;
    }
}

void
lexer_ctx::handle_in_str()
{
    const char c = in_str[i];

    if (c == '"') {

        accept_token();                  /* value may span newlines */
        tok_type = TokType::invalid;

    } else if (c == '\\') {

        if (i + 1 >= in_str.length())
            return;                      /* trailing '\' at EOF: unterminated */

        /*
         * Skip the escaped char, whatever it is, so it cannot end the
         * string. In particular `\\` must consume the second backslash,
         * otherwise it would later be treated as an escape and could
         * swallow the closing quote. This matches unescape_str(), which
         * treats `\X` as a 2-char unit for every X.
         */
        i++;
    }
    /* any other char (incl. '\n') is part of the string; the main loop's i++
     * consumes it and bumps the line counter for a '\n'. */
}

void
lexer_ctx::handle_space_or_op()
{
    const char c = in_str[i];

    if (tok_type != TokType::invalid) {

        accept_token();
        tok_type = TokType::invalid;
    }

    if (isspace(static_cast<unsigned char>(c)))
        return;

    /*
     * Capture the operator's starting Loc now: for a two-char operator `i` is
     * advanced below, so computing the Loc afterwards would point at the
     * operator's second char instead of its first.
     */
    const Loc op_loc = cur_loc();
    string_view op = in_str.substr(i, 1);

    if (i + 1 < in_str.length()) {

        if (c == '.') {

            if (isdigit(static_cast<unsigned char>(in_str[i + 1]))) {

                tok_start = i;
                tok_loc = op_loc;
                tok_type = TokType::floatnum;
                i++;
                return;
            }
        }

        /*
         * Maximal-munch: try a 3-char operator (`>>>`) first, then 2-char.
         * Note: each requires its shorter prefixes to exist as operators too
         * (`>>>` needs `>>` and `>`, `<=` needs `<`), because the 1-char `op`
         * default above is what bootstraps the scan.
         */
        if (i + 2 < in_str.length() && is_operator(in_str.substr(i, 3))) {

            op = in_str.substr(i, 3);
            i += 2;

        } else if (is_operator(in_str.substr(i, 2))) {

            op = in_str.substr(i, 2);
            i++;
        }
    }

    result.emplace_back(TokType::op, op_loc, get_op_type(op));
}

void
lexer_ctx::handle_alphanum()
{
    const char c = in_str[i];

    if (tok_type == TokType::invalid) {

        tok_start = i;
        tok_loc = cur_loc();
        float_exp = false;
        exp_sign_ok = false;
        exp_need_digit = false;

        if (isdigit(static_cast<unsigned char>(c)))
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

        } else if (!isdigit(static_cast<unsigned char>(c))) {

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

        } else if (isdigit(static_cast<unsigned char>(c))) {

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

            /* Start a string token (tok_start was set to i; the value begins
             * after the quote, but tok_loc points AT the quote). */
            tok_loc = cur_loc();
            tok_type = TokType::str;
            tok_start++;

        } else {

            /* Start token of unknown type */
            tok_start = i;
            tok_loc = cur_loc();
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

/*
 * Skip a C-style block comment (slash-star ... star-slash), which MAY span
 * newlines. On entry `i` is at the leading slash; this advances `i` to the
 * slash of the closing delimiter (the main loop's i++ then steps past it).
 * Throws an unterminated-token error (flagged so the REPL keeps reading) if
 * EOF is reached with no close.
 */
void
lexer_ctx::skip_block_comment()
{
    const Loc cstart = cur_loc();
    i += 2;                                  /* past the opening delimiter */

    while (i + 1 < in_str.length()) {

        if (in_str[i] == '*' && in_str[i + 1] == '/') {
            i++;                             /* at '/'; loop's i++ skips it */
            return;
        }

        if (in_str[i] == '\n')
            newline();
        i++;
    }

    throw InvalidTokenEx(string_view("/*"), cstart, Loc(),
                         /*unterminated=*/true);
}

void
lexer(string_view in_str, int line, std::vector<Tok> &result)
{
    lexer_ctx ctx(in_str, line, result);
    const size_type len = in_str.length();

    for (ctx.i = 0; ctx.i < len; ctx.i++) {

        const char c = in_str[ctx.i];

        if (ctx.tok_type == TokType::str) {

            ctx.handle_in_str();
            if (c == '\n')
                ctx.newline();
            continue;
        }

        if (c == '#') {            /* line comment: skip to end of line */
            ctx.flush_pending();
            while (ctx.i + 1 < len && in_str[ctx.i + 1] != '\n')
                ctx.i++;
            continue;              /* the '\n' (if any) is handled next round */
        }

        if (c == '/' && ctx.i + 1 < len && in_str[ctx.i + 1] == '*') {
            ctx.flush_pending();
            ctx.skip_block_comment();
            continue;
        }

        /*
         * A trailing '!' is part of an identifier (Ruby/Scheme "bang"
         * convention, e.g. `get!`), but only when it is not the start of
         * `!=` - so `x!=y` still lexes as `x != y`. The '!' is the last
         * char of the id; the next char ends the token.
         */
        if (ctx.tok_type == TokType::id && c == '!' &&
            (ctx.i + 1 >= len || in_str[ctx.i + 1] != '='))
            continue;

        if (ctx.tok_type == TokType::invalid) {
            ctx.tok_start = ctx.i;
            ctx.tok_loc = ctx.cur_loc();
        }

        const bool is_op = is_operator(string_view(&c, 1));
        const bool in_integer = ctx.tok_type == TokType::integer;
        const bool exp_sign = ctx.exp_sign_ok && (c == '+' || c == '-');

        if (!exp_sign && (isspace(static_cast<unsigned char>(c)) ||
                          (is_op && (!in_integer || c != '.'))))
            ctx.handle_space_or_op();
        else if (exp_sign || isalnum(static_cast<unsigned char>(c)) ||
                 c == '_' || c == '.' || c == '$')
            /* '$' is a valid identifier char (not at the start of a
             * number): the compiler names its synthetic clones `<name>$N`
             * (template instances) / `<name>$sN` (specializations), so a
             * user can reference and inspect them, e.g. typestr(f$0). */
            ctx.handle_alphanum();
        else
            ctx.handle_other();

        if (c == '\n')
            ctx.newline();
    }

    if (ctx.tok_type != TokType::invalid) {

        if (ctx.tok_type == TokType::str) {
            /* Unterminated string literal (EOF before the closing quote). The
             * opening quote is at tok_start - 1; flag it unterminated so the
             * REPL keeps reading more lines instead of erroring. */
            const size_type q = ctx.tok_start > 0 ? ctx.tok_start - 1 : 0;
            throw InvalidTokenEx(in_str.substr(q, ctx.i - q),
                                 ctx.tok_loc, Loc(), /*unterminated=*/true);
        }

        ctx.accept_token();
    }
}
