/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <vector>

/*
 * The REPL's `:help` documentation system (see plans/repl-introspection.md).
 * A self-contained, static documentation database + a query front end - no
 * interpreter state, so it is trivially unit-testable. `repl_help` renders the
 * output for a `:help` argument:
 *
 *   ""                     -> the overview / index
 *   "builtins"             -> every builtin grouped by category (one-liners)
 *   "builtins <category>"  -> that builtin category expanded with signatures
 *   "language"             -> the language feature categories
 *   "<category>"           -> a language category's features
 *   "<name>"               -> a builtin or a language feature's full entry
 *
 * `color` wraps headers / signatures in ANSI escapes (off => plain text).
 */
std::string repl_help(const std::string &topic, bool color);

/*
 * Tab-completion candidates for `:help <prefix>`: the builtin names, the
 * language category ids, and the feature ids that start with `prefix`. Used by
 * the REPL completer; exposed for testing.
 */
std::vector<std::string> repl_help_topics(const std::string &prefix);
