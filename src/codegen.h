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
/*
 * `ref_seeds` - slots the CALLER knows can hold a reference but no
 * instruction writes: the non-scalar PARAMETER slots, filled by the
 * bind. They must go IN rather than be unioned with the result, because
 * `ref_slots`' MoveV rule propagates from a source (see the note in
 * compute_ref_slots).
 */
Chunk codegen_chunk(const Block *block, int slot_count, bool jit = true,
                    const std::vector<int32_t> *ref_seeds = nullptr);

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
 * Lever A (dead-temp forwarding, plans/archived/unboxing.md): per-pc TEMP
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
 * #96 THE REGISTER ALLOCATOR'S LIVE RANGES - the same fixpoint as
 * jit_fwd_info, widened from "the temps, if there are at most 64" to
 * EVERY frame slot. Both are wrappers over one core (codegen.cpp), so
 * there is exactly one backward liveness and one use of visit_use_def:
 * a second enumeration is the audit-table trap waiting to happen, and
 * this one would rot silently (liveness that is WRONGLY conservative
 * costs an optimization and says nothing).
 *
 * An allocator needs three things this answers:
 *   - may a register holding slot s be dropped without a write-back?
 *     (only if s is dead - `live_out(pc, s)` false);
 *   - what must be written back at an exit?  (the live-out set there);
 *   - may one register serve two slots?  (disjoint ranges).
 *
 * ⛔ IT IS A **MAY** ANALYSIS AND MUST STAY ONE. "Live" means "read on
 * SOME path after here", so over-approximating is always safe and
 * under-approximating is a silent wrong answer. An op `visit_use_def`
 * does not know makes every covered slot live (`known == false` ->
 * all), exactly as the temp version does; handler bodies and every
 * branch target are absorbed the same way.
 *
 * `words` uint64 per pc, so there is NO 64-slot cliff. Returns false
 * only when the chunk has no slots at all, in which case everything is
 * empty and the caller must assume every slot live.
 */
struct SlotLiveness {
    int base = 0;                    /* first slot covered */
    int count = 0;                   /* how many slots from base */
    size_t words = 0;                /* uint64 per pc */
    std::vector<uint64_t> livein;    /* n_pc * words */
    std::vector<uint64_t> liveout;
    bool ok = false;

    bool covers(int slot) const
    { return ok && slot >= base && slot < base + count; }
    /* NOT-covered reads as LIVE: the may-analysis direction. */
    bool live_in(size_t pc, int slot) const
    { return bit(livein, pc, slot); }
    bool live_out(size_t pc, int slot) const
    { return bit(liveout, pc, slot); }

private:
    bool bit(const std::vector<uint64_t> &v, size_t pc, int slot) const
    {
        if (!covers(slot) || (pc + 1) * words > v.size())
            return true;
        const int b = slot - base;
        return (v[pc * words + b / 64] >> (b % 64)) & 1;
    }
};
bool jit_slot_liveness(const Chunk &chunk, SlotLiveness &out);

/*
 * D1 (plans/register-allocator-endgame.md): LIVE INTERVALS - the input
 * representation of the interval allocator. One entry per MAXIMAL
 * contiguous stretch of pcs over which a slot carries a value; a slot
 * whose liveness has holes gets one interval PER stretch, which is
 * what lets the allocator serve the same variable from different
 * registers (or none) in different parts of one run.
 *
 * THE SPEC, and the -rt check is derived from it, not from the
 * builder: an interval of slot s covers pc iff
 *     live_in(pc, s)  ||  s ∈ defs(pc)
 * (a def opens the stretch at the defining pc - live_in only becomes
 * true at the NEXT pc). Intervals of one slot are disjoint, sorted,
 * and maximal (the pc before a start and the end pc itself are not
 * covered). `weight` counts the uses+defs inside the stretch - the
 * allocator's ranking input. A barrier op (one visit_use_def does not
 * know) reads as using everything via the liveness fixpoint and
 * defining nothing - conservative in the safe direction.
 */
struct LiveInterval {
    int slot;
    uint32_t start, end;             /* [start, end) pcs in the chunk */
    long weight;
};
bool jit_build_intervals(const Chunk &chunk, size_t begin, size_t end,
                         const SlotLiveness &sl,
                         std::vector<LiveInterval> &out);

/*
 * #96: the SPILL heuristic's input - for each pc in [begin,end) and each
 * covered slot, how many instructions until its next USE, scanning
 * forward in pc order (JIT_NO_NEXT_USE == none in the run).
 *
 * ⛔ THIS IS A HEURISTIC, NOT A FACT, and the difference is the whole
 * reason it is a separate function from the liveness above. Straight-line
 * pc order is not execution order: a BACK EDGE means a slot used at the
 * top of a loop is next used almost immediately, while this reports "not
 * again in this run". Evicting the wrong register costs a reload, never
 * a wrong answer - so a heuristic is the right shape here - but nothing
 * that must be CORRECT may read it. Use SlotLiveness for that.
 */
enum { JIT_NO_NEXT_USE = 1 << 30 };
void jit_next_use(const Chunk &chunk, size_t begin, size_t end,
                  int base, int count, std::vector<int> &dist);

/*
 * C4d (plans/archived/typed-invariant-arrays.md): the per-pc STRUCT-IDENTITY facts
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
 * #137: the bounds every operand of a chunk's instructions is measured
 * against. Everything here is EXTERNAL to the Chunk - the frame the VM will
 * actually build, and three program-wide tables - so a corrupt image cannot
 * widen its own limits by lying about them. vm_verify_program (vm.h) fills
 * one in from the SAME expressions vm_run sizes the frame with.
 */
struct ChunkLimits {
    int nslots = 0;           /* frame_size + n_temps: the frame's real size */
    size_t nglobals = 0;      /* the program's global-slot table */
    size_t ncaptures = 0;     /* this chunk's closure captures (0 for main) */
    size_t nbuiltins = 0;     /* the program-wide builtin table */
    size_t max_fields = 0;    /* the widest struct in the program (see
                               * `field` in verify_chunk: a per-def bound is
                               * not a compile-time fact, this is) */
};

/*
 * #137: REFUSE a structurally impossible chunk, before anything indexes it.
 *
 * A `.myv` stores instructions FIELD-WISE - op, flags, then whatever fields
 * are present - so the reader cannot know that some field is a pool index
 * and others are frame slots or pcs. Nothing validated them, and a mutated
 * byte therefore reached `struct_defs[target2]` inside the load-time JIT (a
 * 4-billion-element read) or an out-of-range frame slot inside the
 * interpreter. This is the pass that closes that: one switch over EVERY
 * opcode, checking each field against the limit it belongs to.
 *
 * THROWS (a plain "MyvError" Exception) rather than asserting - it guards
 * HOSTILE INPUT, not an internal invariant, so it must be on in a release
 * build too. Cost is one pass over the code, at load only.
 *
 * ⛔ THE SWITCH HAS NO `default` CASE, DELIBERATELY. This is the
 * AUDIT-TABLE STAGE TRAP (CLAUDE.md) in its most dangerous form: a stale
 * entry here means SILENT acceptance of an unchecked operand. With no
 * default, adding an opcode fails the BUILD (-Werror=switch) until it is
 * classified. Do not "fix" that build error with a default case.
 */
void verify_chunk(const Chunk &chunk, const ChunkLimits &lim);

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

/* The splice's per-OP whitelist behind bc_inline_callee_ok, exported for
 * the #98 census ratchet (opcode_table_census, tests.cpp), which walks
 * the whole opcode enum against it. */
bool bc_inline_op_ok(OpCode op);

/* The arg-staging retarget whitelist (`<produce t>; MoveV rArg = t` ->
 * `<produce rArg>`, emit_args_range) - exported for the same census
 * under a shim name (the table itself lives in codegen.cpp's anonymous
 * namespace; a same-name global declaration made every internal call
 * ambiguous). Sound only for a SOLE-producer op; see the MoveV/LogV
 * exclusion notes at the definition (both were wrong-code bugs). */
bool bc_test_op_writes_pure_target(OpCode op);

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
/*
 * #97: slots the MoveV rule kept OUT of `ref_slots` - a move's dst is a
 * reference only if its SOURCE can hold one. An EMIT-time counter,
 * because the thing it proves is an ABSENCE: the dst's ref-check, the
 * bind's per-argument test and the frame pop's release scan are simply
 * not emitted for it, so no emitted-code counter could see it.
 */
extern unsigned long g_ref_slots_move_excluded;    /* #97 (TESTS) */
