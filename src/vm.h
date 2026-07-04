/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"   /* Chunk */

class Construct;
class FuncDeclStmt;
class EvalContext;

/*
 * The compiled body chunk for a block-bodied function (Phase 4), lazily built +
 * cached (the storage lives in vm.cpp, cleared per run). Returns null when the
 * body isn't worth running natively - an expression body, or a block with NO
 * register ops - so it stays tree-walked; that keeps expression/recursion-heavy
 * functions off the VM path (no per-call cost). do_func_call caches the result
 * on the FuncDeclStmt so this runs at most once per function per run.
 */
const Chunk *vm_func_chunk(const FuncDeclStmt *fdecl);

/*
 * Drive a chunk against `ctx` (a function body's args context, or main). Runs
 * until Halt or an in-flight `return`; the caller reads ctx->flow->value.
 */
void vm_run_chunk(const Chunk &chunk, EvalContext &ctx);

/*
 * The runtime bytecode VM - the -vm execution engine (plans/bytecode-vm.md).
 * `root` is the OPTIMIZED program AST (post infer / resolve_names /
 * specialize_types), exactly what the tree-walker's root->eval(nullptr) runs.
 * vm_execute lowers it to bytecode and drives it through the SAME root context
 * the tree-walker builds, so observable behavior is identical.
 */
void vm_execute(const Construct *root);

/*
 * Which engine the test harness (tests.cpp check()) runs a program with, so the
 * SAME functional suite runs under BOTH the tree-walker (the oracle) and the VM
 * and must match - the differential-testing pillar. The script / -e path
 * selects the engine directly in mylang.cpp; this global is only the harness's
 * switch.
 */
enum class ExecEngine { TreeWalk, Vm };
extern ExecEngine g_exec_engine;
