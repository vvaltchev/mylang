/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

struct Exception;   /* a struct (errors.h); MSVC mangles struct != class */

/*
 * Shared error rendering, used by both the file driver (mylang.cpp) and the
 * REPL (repl.cpp). Writes to a caller-supplied stream over a caller-supplied
 * source-line vector, so neither the destination (cerr vs a captured string)
 * nor the source storage is hard-wired. Caret convention matches the rest of
 * the codebase (loc_end.col is last-char + 2).
 */

/* One source line + a caret row marking columns [from, to] (1-based, inclusive;
 * to == 0 means "to end of line"). Leading whitespace kept for alignment. */
void dump_line_with_caret(std::ostream &o, const std::string &ln,
                          int from, int to);

/* " at line L, col C[:E]" then the source line(s) with carets for `e`'s loc. */
void dump_loc_in_error(std::ostream &o, const Exception &e,
                       const std::vector<std::string> &lines);

/*
 * Format any thrown Exception (dispatching on its concrete type for the
 * SyntaxError op/token detail, an undefined-variable name, a user exception's
 * payload, etc.) plus its backtrace, into `o`.
 */
void format_exception(std::ostream &o, const Exception &e,
                      const std::vector<std::string> &lines);
