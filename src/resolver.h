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
 * plans/function-inlining.md). The CLI's `-ni` disables it.
 */
void resolve_names(Construct *root, bool enable_inline = true);
