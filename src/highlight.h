/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>

/*
 * Syntax-highlight a line of mylang source by wrapping tokens in ANSI color
 * escapes (keywords, strings, numbers, comments). Used by the REPL line editor
 * to recolor the buffer on every keystroke, so it is a self-contained scanner
 * (not the real lexer): it must tolerate partial / mid-edit input (an unclosed
 * string, a trailing operator) without throwing, and it preserves the input
 * byte-for-byte apart from the inserted zero-width escapes - so cursor columns
 * are unaffected. Returns the original string unchanged when colors are off.
 */
std::string highlight_line(const std::string &src);

/* Toggle coloring globally (honored by highlight_line). Off => identity. */
void set_highlight_enabled(bool on);
