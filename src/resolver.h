/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

class Construct;

/*
 * Name-resolution pass: run once on the parsed (and const-folded) syntax tree,
 * before evaluation. It assigns function params/locals and eligible top-level
 * variables fixed slot indices so that references become O(1) Frame slot reads
 * at runtime instead of scope-chain map lookups (see eval.h / resolver.cpp for
 * details), and records per-slot write counts as groundwork for auto-const.
 *
 * The top-level ("main") frame size is recorded on the root Block (slot_count);
 * the runtime builds that Frame in Block::do_eval. Anything unresolved is left
 * as-is and falls back to the runtime EvalContext map lookup, so the pass is
 * purely an optimization and is always safe to run.
 *
 * When `enable_inline` is true (the default), a final pass also inlines
 * eligible expression-bodied function calls (see the Inliner in resolver.cpp /
 * plans/function-inlining.md). The CLI's `-ni` disables it; `inline_threshold`
 * (CLI `-it N`) caps the inlined body size in nodes.
 */
struct AnalysisInfo;

/*
 * `repl_mode` (REPL only): keep EVERY top-level declaration in the map as a
 * persistent global - never slot it into the "main" frame and never
 * auto-const-promote it (unsound in the REPL's open world, where a later input
 * may reassign it). Nested function locals still slot normally.
 */
/*
 * `prior_pure` (REPL only): a scope holding earlier inputs' globals. Its
 * effectively-pure FUNCTIONS seed both the auto-pure recognition and the
 * const-fold context, so a call to a pure function from a prior input folds
 * across inputs (e.g. `func f2() => f(1,2)`). Only pure functions are used, so
 * folding stays sound.
 */
class EvalContext;
void resolve_names(Construct *root,
                   bool enable_inline = true,
                   int inline_threshold = 24,
                   AnalysisInfo *analysis = nullptr,
                   bool repl_mode = false,
                   EvalContext *prior_pure = nullptr);

/*
 * The post-inference optimizer pipeline run by BOTH drivers (mylang.cpp's
 * script path and the REPL's per-input do_eval): resolve_names (slotting +
 * auto-const + inlining + specialization) then specialize_types (M8 typed
 * scalars). Factored so the two stay in lock-step - a new pass added here
 * reaches the script and the REPL identically (the REPL must transform the
 * tree exactly like the script). `repl_mode`/`prior_scope` thread through to
 * resolve_names for the cross-input behavior; everything else is the same call.
 */
void run_optimizers(Construct *root,
                    bool enable_inline = true,
                    int inline_threshold = 24,
                    bool enable_specialize = true,
                    bool repl_mode = false,
                    EvalContext *prior_scope = nullptr);

/*
 * -a/--analyze: after resolve_names has run, record the resolver-decided
 * optimizations that survive as flags on the tree - auto-pure functions and
 * auto-const parameters (both yellow). The auto-const var / dead-code / inlined
 * / specialized / folded records are emitted *during* resolve_names via the
 * `analysis` argument above (those decisions remove or rewrite nodes, so they
 * must be captured as they happen). Separate from the inferencer's
 * array-storage colors (collect_array_analysis).
 */
void collect_resolver_analysis(Construct *root, AnalysisInfo &out);
