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
void resolve_names(Construct *root,
                   bool enable_inline = true,
                   int inline_threshold = 24,
                   AnalysisInfo *analysis = nullptr,
                   bool repl_mode = false);

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
