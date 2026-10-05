/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NUMBER TEXT: the one reading of a number written as text - a literal in a
 * program (the parser) and the string `int()` / `float()` convert. README's
 * `int(value)` and `float(value)` are the spec: the WHOLE string is the
 * number, in the spelling a literal has, with optional surrounding ASCII
 * whitespace and an optional sign; `float()` also takes `inf`, `infinity`
 * and `nan` in any case. Anything else is not a number - C's strtoll /
 * strtod read a PREFIX and ignored the rest, so int("12abc") was 12 and
 * int("3.7") was 3 until 2026-10-05.
 *
 * A float that is a number but lies outside the double range says so
 * (overflow / underflow, with `out` holding what IEEE rounding gives: an
 * infinity, a signed zero); each caller decides what that means (a literal
 * refuses it, float() refuses it at compile time and rounds at run time). A
 * subnormal is in range: glibc's strtod reports one as a range error, which
 * made the smallest double, 4.9e-324, an unwritable literal.
 */

#pragma once

#include "defs.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

enum class NumText { ok, bad, overflow, underflow };

inline bool numtext_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

inline std::string_view numtext_trim(std::string_view s)
{
    while (!s.empty() && numtext_space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && numtext_space(s.back()))
        s.remove_suffix(1);
    return s;
}

inline bool numtext_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* `[+-]? digits`, the whole of the trimmed `s`. Junk is `bad` even when the
 * digits before it would not fit, so "99999999999999999999x" is not a
 * number rather than a too-big one. */
inline NumText numtext_int(std::string_view s, int_type &out)
{
    s = numtext_trim(s);

    bool neg = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    if (i == s.size())
        return NumText::bad;

    const uint64_t max = static_cast<uint64_t>(
        std::numeric_limits<int_type>::max());
    const uint64_t lim = neg ? max + 1 : max;
    uint64_t v = 0;
    bool over = false;

    for (; i < s.size(); i++) {
        if (!numtext_digit(s[i]))
            return NumText::bad;
        if (over)
            continue;
        const uint64_t d = static_cast<uint64_t>(s[i] - '0');
        if (v > (lim - d) / 10)             /* v * 10 + d > lim */
            over = true;
        else
            v = v * 10 + d;
    }

    if (over)
        return NumText::overflow;

    if (!neg)
        out = static_cast<int_type>(v);
    else if (v == lim)
        out = std::numeric_limits<int_type>::min();
    else
        out = -static_cast<int_type>(v);
    return NumText::ok;
}

/* case-insensitive equality with a lowercase word */
inline bool numtext_word(std::string_view s, const char *w)
{
    size_t i = 0;
    for (; w[i]; i++) {
        if (i == s.size())
            return false;
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != w[i])
            return false;
    }
    return i == s.size();
}

/*
 * `[+-]? ( digits [. digits?] | . digits ) ( [eE] [+-]? digits )?` or a
 * signed `inf` / `infinity` / `nan`, the whole of the trimmed `s` - every
 * spelling a float or an int LITERAL has, and no other (no hex float, no
 * `nan(...)` payload, no digit separator).
 */
inline NumText numtext_float(std::string_view s, float_type &out)
{
    s = numtext_trim(s);

    bool neg = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    const std::string_view body = s.substr(i);

    if (numtext_word(body, "inf") || numtext_word(body, "infinity")) {
        out = neg ? -std::numeric_limits<float_type>::infinity()
                  : std::numeric_limits<float_type>::infinity();
        return NumText::ok;
    }
    if (numtext_word(body, "nan")) {
        out = std::numeric_limits<float_type>::quiet_NaN();
        return NumText::ok;
    }

    size_t j = 0;
    bool digit = false, nonzero = false;
    for (; j < body.size() && numtext_digit(body[j]); j++) {
        digit = true;
        nonzero = nonzero || body[j] != '0';
    }
    if (j < body.size() && body[j] == '.') {
        for (j++; j < body.size() && numtext_digit(body[j]); j++) {
            digit = true;
            nonzero = nonzero || body[j] != '0';
        }
    }
    if (!digit)
        return NumText::bad;
    if (j < body.size() && (body[j] == 'e' || body[j] == 'E')) {
        j++;
        if (j < body.size() && (body[j] == '+' || body[j] == '-'))
            j++;
        const size_t k = j;
        while (j < body.size() && numtext_digit(body[j]))
            j++;
        if (j == k)
            return NumText::bad;
    }
    if (j != body.size())
        return NumText::bad;

    /* The spelling is strtod's too, so it reads all of it - in the C locale,
     * which nothing in the interpreter changes. Its errno is not consulted:
     * whether a subnormal result sets ERANGE differs between C libraries. */
    const std::string text(s);
    const float_type v = std::strtod(text.c_str(), nullptr);

    if (std::isinf(v)) {
        out = v;
        return NumText::overflow;
    }
    if (v == 0 && nonzero) {
        out = neg ? -0.0 : 0.0;
        return NumText::underflow;
    }
    out = v;
    return NumText::ok;
}
