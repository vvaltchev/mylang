/* SPDX-License-Identifier: BSD-2-Clause */

#include "highlight.h"
#include "defs.h"
#include "errors.h"         /* Loc - needed by lexer.h */
#include "operators.h"      /* Op  - needed by lexer.h */
#include "lexer.h"          /* KwString - the keyword set */

#include <cctype>
#include <string>

using std::string;

namespace {

const char *const RESET   = "\033[0m";
const char *const C_KW    = "\033[38;5;39m";   /* keyword  - blue   */
const char *const C_STR   = "\033[38;5;114m";  /* string   - green  */
const char *const C_NUM   = "\033[38;5;215m";  /* number   - orange */
const char *const C_COM   = "\033[38;5;244m";  /* comment  - gray   */
const char *const C_TYPE  = "\033[38;5;79m";   /* type kw  - teal   */

bool g_enabled = true;

bool is_kw(const string &w)
{
    for (int i = 1; i < static_cast<int>(Keyword::kw_count); i++)
        if (w == KwString[i])
            return true;
    return false;
}

/* The primitive type names: not lexer keywords (they are builtins int()/...),
 * but in a type-annotation position they read as types, so color them. */
bool is_type_word(const string &w)
{
    return w == "int" || w == "float" || w == "bool" || w == "str" ||
           w == "array" || w == "dict";
}

bool id_start(unsigned char c) { return isalpha(c) || c == '_'; }
/* '$' is a valid identifier char (the compiler's `f$0` clone names - see the
 * lexer), so a synthetic name highlights as one token. */
bool id_char(unsigned char c)  { return isalnum(c) || c == '_' || c == '$'; }

}  /* namespace */

void set_highlight_enabled(bool on) { g_enabled = on; }

string
highlight_line(const string &src)
{
    if (!g_enabled)
        return src;

    string out;
    const size_t n = src.size();
    size_t i = 0;

    while (i < n) {

        const unsigned char c = static_cast<unsigned char>(src[i]);

        /* line comment: '#' to end of line */
        if (c == '#') {
            out += C_COM;
            while (i < n && src[i] != '\n')
                out += src[i++];
            out += RESET;
            continue;
        }

        /* C-style block comment - real MyLang source has none (it uses #), but
         * the :show decompiler annotates with them, so color them gray. */
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            out += C_COM;
            out += src[i++];                    /* '/' */
            out += src[i++];                    /* '*' */
            while (i < n &&
                   !(src[i] == '*' && i + 1 < n && src[i + 1] == '/'))
                out += src[i++];
            if (i + 1 < n) {                    /* the closing '*'/ */
                out += src[i++];
                out += src[i++];
            } else if (i < n) {
                out += src[i++];
            }
            out += RESET;
            continue;
        }

        /* string literal "..." (tolerate it being unterminated mid-edit) */
        if (c == '"') {
            out += C_STR;
            out += src[i++];
            while (i < n && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < n)
                    out += src[i++];            /* keep an escaped char pair */
                out += src[i++];
            }
            if (i < n)                          /* the closing quote */
                out += src[i++];
            out += RESET;
            continue;
        }

        /* number (int or float) */
        if (isdigit(c)) {
            out += C_NUM;
            while (i < n &&
                   (isdigit(static_cast<unsigned char>(src[i])) ||
                    src[i] == '.'))
                out += src[i++];
            out += RESET;
            continue;
        }

        /* identifier or keyword (a trailing '!' is part of the name, unless
         * it begins '!=' - the lexer's bang convention) */
        if (id_start(c)) {
            string w;
            while (i < n && id_char(static_cast<unsigned char>(src[i])))
                w += src[i++];
            if (i < n && src[i] == '!' && !(i + 1 < n && src[i + 1] == '='))
                w += src[i++];

            if (is_kw(w))
                out += C_KW + w + RESET;
            else if (is_type_word(w))
                out += C_TYPE + w + RESET;
            else
                out += w;                       /* plain identifier */
            continue;
        }

        out += src[i++];                        /* operators / punctuation */
    }

    return out;
}
