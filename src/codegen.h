/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"

#include <vector>

class Block;
class Construct;
class FuncDeclStmt;

/*
 * Collect every FuncDeclStmt reachable from `c` - a COMPLETE walk, so a lambda
 * in ANY expression position (a `return func[..]{..}`, a `var f = func..`, a
 * call arg, a ternary branch, ...) is found too, not only top-level / body-
 * statement functions. Each FuncDeclStmt's own body is recursed for nested
 * closures. Used by the VM's AOT precompile and the -vd dump.
 */
void collect_funcs(const Construct *c,
                   std::vector<const FuncDeclStmt *> &out);

/*
 * Lower a Block's statements to a bytecode Chunk. `slot_count` is the frame's
 * resolved-local count (register-machine temps grow above it, at
 * [slot_count, slot_count + chunk.n_temps)). Used for both the root/main block
 * and a function body (Phase 4). See plans/archived/bytecode-vm.md.
 */
/*
 * `jit` (default true): run jit_compile_chunk at the end (native-AOT). The VM
 * precompile passes FALSE so it can codegen ALL bodies first (every
 * `native_leaf` flag set - it is computed here regardless of `jit`) and jit
 * them in a SECOND pass, so a caller's native-call gate sees every callee's
 * flag (#55 STEP 2's ordering fix). Disasm / -rt / the default keep true.
 */
Chunk codegen_chunk(const Block *block, int slot_count, bool jit = true);

/* The root/main wrapper: codegen_chunk(root, root->slot_count). */
Chunk codegen_program(const Block *root, bool jit = true);

/*
 * Compile ONE function's body to a runnable chunk, applying the VM's
 * compile gate. Returns true (and fills `out`) iff the body is a natively-
 * runnable chunk; false when the function must tree-walk instead:
 *   - a BASE TEMPLATE (`is_template_base`): a monomorphization source, never
 *     called → never compiled (so it is absent from the compiled chunk set, and
 *     hence from -vd — faithfully, not filtered);
 *   - a non-block or NON-scope-free body (a closure / nested-func body needs its
 *     own child EvalContext, which vm_run_chunk doesn't build);
 *   - a body with no real op (only control-flow ops): running it via
 *     the VM would only add dispatch over the tree-walker, no win.
 * This is the single source of truth for "which functions have bytecode",
 * shared by the VM's AOT precompile (vm.cpp) and the -vd dump (disasm.cpp).
 */
bool codegen_func_body(const FuncDeclStmt *fn, Chunk &out, bool jit = true);

/*
 * Build `chunk.boxed_ops` (the JIT-bakeable operand pool for BinOpV / CmpV /
 * CompoundV / LogV / UnaryV + a compound global/capture store) and stamp each
 * such op's otherwise-unused `target2` with its pool index. See
 * Chunk::BoxedOp.
 *
 * DERIVED DATA: it is a pure function of the FINAL code plus the loc side
 * table, so the `.myv` loader does NOT store the pool - it calls THIS
 * function after reading a chunk. Exposed for exactly that reason: the loader
 * must rebuild what codegen built, and a second implementation would be free
 * to drift (the round-trip dump oracle would catch it, but only after the
 * fact). Runs at the END of codegen (post-peephole, post-extract_locs) and,
 * at load, after `locs` is read.
 */
void build_boxed_ops(Chunk &chunk);

/*
 * #78 step B: assert the HANDLER TABLE still describes the interpreted
 * CatchTest/Reraise chain (see Chunk::handler_sites). Called after every
 * pc-moving transformation while both representations exist - the
 * peephole's compaction and both JIT remaps - so a missed remap is a loud
 * compile-time abort, never a wrong catch at runtime. ASSERTS-only.
 */
void verify_handler_sites(const Chunk &chunk);
