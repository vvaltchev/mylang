/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <vector>

class Construct;
class FuncDeclStmt;

/*
 * Render the FINAL optimized AST of a function back into synthetic MyLang-like
 * code (see plans/repl-introspection.md) - the "decompiler" behind the `show()`
 * builtin and the REPL `:show` command. It walks the tree AFTER parsing,
 * const-folding, inference, resolve_names (inlining/specialization) and
 * specialize_types, so what you see is what actually runs:
 *
 *   - dead code is already gone (the optimizer removed it);
 *   - folded constants appear as literals (`print(3)`, not `print(f(1,2))`);
 *   - an inlined call body is spliced in place and annotated with an
 *     "inlined <name>" comment;
 *   - a flat array's element type shows as `array<int>` (informative, not
 *     valid script syntax); explicit annotations and proven typed-scalar hints
 *     surface as the variable's type.
 *
 * It is best-effort, not a round-trippable pretty-printer: an unhandled node
 * falls back to a comment placeholder rather than failing.
 */
std::string render_func_code(const FuncDeclStmt *fn,
                             const std::vector<std::string> &param_types = {});

/* Render an arbitrary construct (a statement or expression) - used by the
 * above and by the unit tests. */
std::string render_construct_code(const Construct *c);
