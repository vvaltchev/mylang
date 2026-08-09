/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"

#include <string>
#include <vector>

class Block;
class Construct;
class FuncDeclStmt;
struct FuncDescriptor;

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

/*
 * THE BYTECODE-LEVEL INLINER's gate (plans/bytecode-inliner.md).
 *
 * True iff `callee` may be SPLICED into a caller at a CallV: every op is on
 * the whitelist (its slot fields are enumerated by the remapper AND it
 * carries no pool index), it ends in ReturnV with no Halt, it owns no
 * per-frame side state, and it is small enough. `why`, when non-null, is
 * filled with the FIRST blocking reason - that is what the corpus audit
 * prints.
 *
 * A WHITELIST on purpose: an op nobody has classified declines the inline
 * instead of corrupting a frame or reading the wrong pool entry. The three
 * per-op tables this codebase already has (visit_use_def, op_writes_scalar,
 * visit_pc_fields) have all gone stale at least once when an op was added;
 * here that failure mode costs an optimization and nothing else.
 */
bool bc_inline_callee_ok(const Chunk &callee, std::string *why);

/*
 * The corpus audit (env MYLANG_INLAUDIT=1, the MYLANG_DELAUDIT pattern):
 * report each CallV in `caller` with its callee's size and the gate's
 * verdict, so the histogram says what the inliner reaches and what to
 * unblock next. Dev-only; zero cost when the env is unset.
 */
void bc_inline_audit(const Chunk &caller, const char *caller_name,
                     const std::vector<const FuncDescriptor *> &slot_desc);
