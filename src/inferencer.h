/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

class Construct;
class UniqueId;
class FuncDeclStmt;
class EvalContext;

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
void infer_types(Construct *root, bool enable = true, bool strict = true);

/*
 * --debug-ti: run inference (non-strict) and dump every declared identifier's
 * inferred type + use sites (machine-readable, tab-separated) to `os`. Used to
 * audit the corpus for spurious `dyn`s. See
 * plans/type-driven-specialization.md.
 */
void dump_type_info(Construct *root, std::ostream &os);

/*
 * M8 specialization pass: rewrite hot scalar expression nodes (int/float
 * arithmetic, comparison, logical, unary) that infer_types proved are typed
 * into TypedScalarExpr, which evaluates without num_bin_op dispatch or
 * intermediate EvalValue boxing. Run AFTER resolve_names (it benefits from
 * resolved slots via Identifier's typed fast paths). A no-op when inference is
 * disabled (no TypeHints are set). See plans/type-inference.md M8.
 */
void specialize_types(Construct *root, bool enable = true,
                      EvalContext *prior_scope = nullptr);

/*
 * REPL incremental type inference + checking. A persistent type-checker that
 * runs the REAL inference per input over an EXPANDABLE global scope: each input
 * is checked against the globals committed by prior inputs (their types PINNED,
 * so a committed global behaves like an annotation - the cross-input type
 * commitment), then its own new globals are committed. `check_input` throws a
 * compile-time exception (TypeMismatchEx, DynRequiredEx, ...) on a violation,
 * which the REPL catches to reject just that input. `undef_global` drops a name
 * from the committed set (so a later `var x` of a new type is fresh, not a
 * conflict). The one-shot `infer_types` (scripts + all tests) is untouched.
 * See plans/repl.md §3.1.
 */
class ReplInfer {

public:

    ReplInfer();
    ~ReplInfer();

    void check_input(Construct *input);
    void undef_global(const UniqueId *name);

    /* The inferred static type string of a committed global (or "" if the name
     * is not a committed inferred symbol - e.g. a const scalar folded away).
     * Backs the REPL :globals enrichment. */
    std::string global_type(const UniqueId *name);

    /* The inferred type of each parameter of `fn`, and `fn`'s inferred return
     * type (for :show); empty for an un-instantiated template / unknown func. */
    std::vector<std::string> func_param_types(const FuncDeclStmt *fn);
    std::string func_return_type(const FuncDeclStmt *fn);

    /* REPL instance GC - true if this template/spec instance is still consumed
     * by a function body (so redefining its base must not remove it). */
    bool instance_has_consumer(const FuncDeclStmt *fn);

private:

    struct Impl;
    std::unique_ptr<Impl> impl;
};
