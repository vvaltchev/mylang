/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>

/*
 * Highlighter carry state across lines: a string ("...") or block comment
 * (slash-star ... star-slash) may span lines, so coloring one line needs to
 * know whether the previous line ended inside one. HL_NONE at a line start
 * means ordinary code.
 */
enum HlState { HL_NONE = 0, HL_STRING = 1, HL_COMMENT = 2 };

/*
 * Syntax-highlight a line of mylang source by wrapping tokens in ANSI color
 * escapes (keywords, strings, numbers, comments). Used by the REPL line editor
 * to recolor the buffer on every keystroke, so it is a self-contained scanner
 * (not the real lexer): it must tolerate partial / mid-edit input (an unclosed
 * string, a trailing operator) without throwing, and it preserves the input
 * byte-for-byte apart from the inserted zero-width escapes - so cursor columns
 * are unaffected. Returns the original string unchanged when colors are off.
 *
 * The two-arg form threads the cross-line `state` (an HlState): it is read at
 * the line start and updated to what the line ends in (so a string / block
 * comment is colored across rows). The one-arg form is the stateless wrapper
 * (a single isolated line).
 */
std::string highlight_line(const std::string &src, int &state);
std::string highlight_line(const std::string &src);

/* Toggle coloring globally (honored by highlight_line). Off => identity. */
void set_highlight_enabled(bool on);
