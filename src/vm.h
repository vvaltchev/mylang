/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

class Construct;

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
