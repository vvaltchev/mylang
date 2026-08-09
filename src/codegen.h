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
 * Lever A (dead-temp forwarding, plans/unboxing.md): per-pc TEMP
 * live-out AND live-in masks + branch-target flags over the chunk's
 * FINAL code, for the JIT's adjacent-pair forwarding. Computed HERE,
 * with the audited enumerations (visit_use_def / visit_pc_fields / the
 * handler table) the E1 liveness already uses, so the emitter cannot
 * grow a second, drifting copy of them.
 * `liveout[pc] & (1 << (t - slot_count))` == temp t is read on some
 * path AFTER op pc (throw-resume paths included via the handler
 * absorption) - the deadness test for a write elision.
 * `livein[pc]` is the same mask BEFORE op pc - "a value from outside
 * reaches this pc", which is the test an ENTRY pc needs (C4a-i's
 * elision gate; using liveout there was a real bug - at a run head
 * whose first op writes the temp, live-out includes that very temp, so
 * every float temp read as excluded and the elision measured flat).
 * `is_tgt[pc]` == some branch or handler RESUME can enter at pc
 * (post-call resume stubs are the JIT's own and are checked there).
 * Returns false when temp liveness is not computable (no temps, or
 * > 64): both vectors are empty and the caller may forward reads but
 * never elide a write. is_tgt is filled either way. Runs at JIT time on
 * the final code, AFTER any bytecode splice - never cached across
 * transformations.
 */
bool jit_fwd_info(const Chunk &chunk, std::vector<uint64_t> &liveout,
                  std::vector<uint64_t> &livein,
                  std::vector<char> &is_tgt);

/*
 * C4d (plans/typed-invariant-arrays.md): the per-pc STRUCT-IDENTITY facts
 * a PLANNED StructCtorV establishes, so a baked member read on the same
 * slot can skip the type-tag + def-identity guards it would otherwise
 * re-check on every single field read.
 *
 * `fact_slot[f]`/`fact_def[f]` name fact f; `in[pc] & (1 << f)` == on
 * EVERY path reaching pc, that slot holds a struct of that def. A forward
 * MUST dataflow (meet = intersection) over the chunk's FINAL code, on the
 * same audited enumerations as jit_fwd_info - see the definition for the
 * GEN/KILL argument and why an unaudited op is a barrier. `entry_pcs` is
 * the JIT's per-pc entry-stub set: a resume arrives with no history, so
 * those are bottom like the handler pcs.
 *
 * Returns false (and empties everything) when there is nothing to say -
 * no planned ctor, or more than 32 distinct facts.
 */
bool jit_struct_facts(const Chunk &chunk, const std::vector<int> &entry_pcs,
                      std::vector<int> &fact_slot,
                      std::vector<const StructTypeDef *> &fact_def,
                      std::vector<uint32_t> &in);

/*
 * C4e: one op's frame-slot reads and writes, from the SAME audited
 * enumeration (visit_use_def) the peephole and jit_fwd_info use. Returns
 * false for an op the table does not know - the caller must then treat it
 * as touching every slot. Exported so an emitter policy that needs
 * per-slot facts cannot grow a second, drifting copy of the table.
 */
bool jit_op_slot_refs(const Instr &in, std::vector<int> &uses,
                      std::vector<int> &defs);

/*
 * C5: does this opcode write its dst as a plain SCALAR (int/float/bool)?
 *
 * THE SAME predicate compute_ref_slots uses to build `ref_slots` - a slot
 * is ref-listed precisely because some op that is NOT in this set writes
 * it (or because a barrier op forced the whole frame onto the list).
 * Exported so an emitter policy that needs "after this op the slot
 * provably holds a trivial value" asks the one table that already
 * defines that, rather than growing a second one free to drift from it.
 */
bool op_writes_scalar(OpCode op);

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

/*
 * THE PRE-SPLICE SNAPSHOT of one callee body, and the map of them.
 *
 * The splice reads a callee's finished chunk, but the pass mutates chunks
 * as it goes - so reading LIVE makes the result depend on the order the
 * caller iterates its chunk map. That map is an `unordered_map`, so the
 * order varies run to run and two compiles of one program could differ,
 * which "compiling twice is byte-identical" forbids. It is not a
 * correctness hazard (a spliced callee gains `inline_ctxs` entries, which
 * the gate rejects, so the worst case is an inline that silently does not
 * happen) but it is a reproducibility one.
 *
 * Snapshotting every body BEFORE any of them is touched fixes both halves
 * at once: the order stops mattering, and every splice takes the ORIGINAL
 * body - which is what "ONE level" already claimed and only accidentally
 * got. `eligible` is the gate's verdict on the PRISTINE body for the same
 * reason.
 */
struct BcInlineSnapshot {
    std::vector<Instr> code;
    std::vector<Chunk::LocEntry> locs;
    std::vector<Chunk::LocEntry> base_locs;   /* #127: the store-base carets */
    std::vector<int32_t> ref_slots;
    int slot_count = 0;
    int n_temps = 0;
    bool eligible = false;
};
typedef std::unordered_map<const Chunk *, BcInlineSnapshot> BcInlineSnapshots;

/* Record `ck`'s pristine body into `out`. Call for EVERY chunk of the
 * program before the first bc_inline_chunk. */
void bc_inline_snapshot(const Chunk &ck, BcInlineSnapshots &out);

/*
 * THE SPLICE. Replace every inline-able CallV in `ck` with the callee's
 * body - arg binds as MoveVs, the body slot-remapped into a fresh range
 * above the caller's frame, each ReturnV rewritten to "move the result to
 * the call's dst, jump to the join". ONE level, from the pre-pass SNAPSHOT
 * of the callee's code, so neither a self-recursive body nor an
 * already-spliced callee can compound.
 *
 * Runs after every chunk is codegen'd and before any is jit'd
 * (vm_precompile_all), because it needs the callees' finished chunks and
 * produces bytecode the JIT then sees. Returns true if it changed `ck`.
 */
bool bc_inline_chunk(Chunk &ck,
                     const std::vector<const FuncDescriptor *> &slot_desc,
                     const BcInlineSnapshots &snaps);

/* The splice's kill switch (-nbi / MYLANG_BCINLINE=0): the same-binary
 * A/B, since the un-inlined bytecode is the only oracle for a splice. */
extern bool g_bc_inline_enabled;

/* Execution proof for the caller-frame path - see codegen.cpp. */
extern unsigned long g_bc_inline_caller_frames;
/* Total call sites spliced - the shape matrix's non-vacuity check. */
extern unsigned long g_bc_inline_splices;
extern unsigned long g_ref_slots_proven_excluded;   /* C3 (TESTS) */
