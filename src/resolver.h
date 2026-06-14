/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

class Construct;

/*
 * Name-resolution pass: run once on the parsed (and const-folded) syntax tree,
 * before evaluation. It assigns function parameters fixed slot indices so that
 * references to them become O(1) Frame slot reads at runtime instead of
 * scope-chain map lookups (see eval.h / resolver.cpp for details), and records
 * per-parameter write counts as groundwork for auto-const detection.
 *
 * Anything it does not resolve is left as-is and falls back to the existing
 * runtime EvalContext map lookup, so the pass is purely an optimization and is
 * always safe to run.
 */
void resolve_names(Construct *root);
