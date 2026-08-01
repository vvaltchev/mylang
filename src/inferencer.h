/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

class Construct;
class UniqueId;
class FuncDeclStmt;
class EvalContext;
struct AnalysisInfo;

/*
 * Whole-program static type inference + checking (see plans/archived/type-inference.md).
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
 * plans/archived/type-driven-specialization.md.
 */
void dump_type_info(Construct *root, std::ostream &os);

/*
 * The COMPLETE per-node child visitor (every Construct edge, incl. try/catch
 * bodies, slices, dict literals) - the inferencer's own walker, exported for
 * walks that must not miss a node: the VM's AOT precompile and the
 * descriptor/struct-def ownership transfer (vm_compile). Calls `f` on each
 * DIRECT child of `c` (some may be null-skipped internally).
 */
void for_each_child_of(Construct *c,
                       const std::function<void (Construct *)> &f);

/*
 * M8 specialization pass: rewrite hot scalar expression nodes (int/float
 * arithmetic, comparison, logical, unary) that infer_types proved are typed
 * into TypedScalarExpr, which evaluates without num_bin_op dispatch or
 * intermediate EvalValue boxing. Run AFTER resolve_names (it benefits from
 * resolved slots via Identifier's typed fast paths). A no-op when inference is
 * disabled (no TypeHints are set). See plans/archived/type-inference.md M8.
 *
 * `analyze` (the `-a`/`:analyze` pipeline only; null in a normal run) gets a
 * counted_for annotation on each `for` keyword this pass rewrites to a
 * ForRangeStmt, so the colored view can mark a specialized counted loop green.
 */
void specialize_types(Construct *root, bool enable = true,
                      EvalContext *prior_scope = nullptr,
                      AnalysisInfo *analyze = nullptr);

/*
 * PER-PASS KILL SWITCHES for the AST transforms specialize_types performs.
 *
 * These exist for TESTABILITY as much as debugging. An AST transform rewrites
 * the tree BEFORE either engine sees it, so the tree-walker-vs-VM differential
 * - the project's main correctness net - cannot see a bug in one: both engines
 * faithfully run the same wrong tree. The only oracle is the SAME program with
 * the transform turned off, which is what these switches provide inside ONE
 * binary (the `-nj` JIT kill switch is the same idea one layer down).
 *
 * `opt_layer_equivalence` (-rt) runs a corpus through every single-pass-off
 * configuration AND the all-off one, on BOTH engines, requiring identical
 * output and identical exceptions. A new AST transform belongs here on the day
 * it is written - see CLAUDE.md "Testing an AST transform".
 *
 * CLI: `--no-opt <name>[,<name>...]` (mylang.cpp).
 */
enum OptPass : unsigned {
    opt_licm        = 1u << 0,   /* try_hoist_loop_subscripts (LICM)     */
    opt_slice_hoist = 1u << 1,   /* try_hoist_loop_slices                */
    opt_for_range   = 1u << 2,   /* try_for_range -> ForRangeStmt        */
    opt_typed       = 1u << 3,   /* M8 TypedScalarExpr specialization    */
    opt_all_passes  = 0xfu,
};

/* Bitmask of DISABLED passes; 0 (the default) = everything on. */
extern unsigned g_opt_disabled;

/* Map a CLI name ("licm", "slice-hoist", "for-range", "typed", "all") to its
 * bit. Returns false for an unknown name. `names` for --help/diagnostics. */
bool opt_pass_bit(const std::string &name, unsigned &bit);
const char *opt_pass_names();

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
 * See plans/repl.md section 3.1.
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
