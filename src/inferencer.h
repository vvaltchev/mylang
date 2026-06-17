/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

class Construct;

/*
 * Whole-program static type inference + checking (see plans/type-inference.md).
 *
 * Runs after parsing (and parse-time const-folding) but BEFORE resolve_names,
 * on the clean source tree. It infers a fixed static type for every variable,
 * parameter and function return (whole-program, multi-pass / fixpoint), then
 * checks every operation, assignment, call and return, throwing a compile-time
 * TypeMismatchEx / NullabilityEx / WrongArgCountEx on a violation. Those are
 * plain Exceptions (not RuntimeExceptions), so script try/catch cannot catch
 * them: a type error fails the build, like a syntax error.
 *
 * It is a no-op when `enable` is false (the CLI's -nti flag). The inferencer
 * owns its own type arena and side tables and stores nothing on the AST, so it
 * leaves the tree untouched for resolve_names / evaluation.
 */
void infer_types(Construct *root, bool enable = true);
