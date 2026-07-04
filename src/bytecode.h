/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "syntax.h"

#include <vector>

/*
 * The runtime bytecode for the -vm execution engine (see plans/bytecode-vm.md).
 * The VM consumes the ALREADY-OPTIMIZED AST (post infer / resolve_names /
 * specialize_types) and lowers it to a flat instruction list, removing the
 * tree-walker's per-node virtual-dispatch tax. It is built strictly
 * incrementally behind an AST-fallback opcode: EvalStmt (and, later, EvalExpr)
 * just call the node's existing eval(), so the VM runs the WHOLE language from
 * day one, and native opcodes replace the fallbacks one tested step at a time.
 *
 * Phase 0: EvalStmt + Halt. Phase 1 adds native control flow (Jump /
 * JumpIfFalse / LoopBackEdge). Phase 2 adds a REGISTER machine over the frame
 * slots (the VM's registers ARE the resolved-local slots, so there is no value
 * stack) with native int ops (IntBin / JumpUnlessIntCmp) and fused
 * superinstructions, so a resolved-local int scalar loop runs with no
 * tree-walker fallback. See plans/bytecode-vm.md.
 *
 * (Named OpCode, not Op: `Op` is already the operator enum in operators.h.)
 */
enum class OpCode : unsigned char {

    /*
     * AST fallback (the incremental-safety pillar): evaluate `node` with the
     * tree-walker. EvalStmt runs a statement - its value is discarded, and an
     * UndefinedId result throws UndefinedVariableEx as Block::do_eval. It does
     * NOT act on FlowState: an in-flight break/continue set by a loop body is
     * consumed by the following LoopBackEdge, not here.
     */
    EvalStmt,

    /* Unconditional jump: pc = `target`. */
    Jump,

    /*
     * Evaluate `node` as a condition (fallback: the same typed-or-boxed path as
     * the tree-walker's eval_cond). If FALSE, pc = `target`; else fall through.
     */
    JumpIfFalse,

    /*
     * Placed right after a loop body; reads ctx->flow and branches, mirroring
     * While/ForStmt::do_eval exactly. `target` = the continue destination (a
     * while's re-test, a for's increment), `target2` = the loop exit:
     *   ret  -> pc = target2 (leave flow set; it propagates out of the loop)
     *   brk  -> flow = none, pc = target2
     *   cont -> flow = none, pc = target
     *   none -> pc = target
     */
    LoopBackEdge,

    /*
     * Native 3-address int op (register machine): slot[target] = a <aop> b,
     * where a/b are Operands (a frame slot or an int immediate) and aop is an
     * arithmetic/bitwise Op. `node` carries the source statement's Loc for a
     * div/mod-by-zero error. No value stack - operands and result are frame
     * slots. This is what `s += i` / `i++` compile to.
     */
    IntBin,

    /*
     * Fused compare-and-branch: if NOT (a <aop> b) then pc = target, else fall
     * through. a/b are int Operands, aop a comparison Op. One dispatch replaces
     * the tree-walker's eval_cond -> TypedScalarExpr -> Identifier chain (this
     * is what a `while (i < N)` header compiles to: jump out unless i < N).
     */
    JumpUnlessIntCmp,

    /* The FLOAT analogues of IntBin / JumpUnlessIntCmp: operands are read as
     * float (an int/bool slot promotes), the arithmetic is `double`, the result
     * is written to a float slot. div/mod (fmod) keep the zero check. For a
     * pure-float scalar loop. */
    FloatBin,
    JumpUnlessFloatCmp,

    /*
     * Fused counted-loop back-edge (the register-machine superinstruction for a
     * `for`): in ONE dispatch, i += step (or -= for a descending loop), then if
     * (i <aop> bound) pc = target (loop back to the body), else fall through
     * (exit) - matching the tree-walker's raw-C ForRangeStmt counter so a `for`
     * doesn't regress. Fields: `target2` = i's slot, `b` = step, `a` = bound,
     * `aop` = the comparison Op (lt/le ascend +step, ge/gt descend -step),
     * `target` = the loop body. int-only (ForRangeStmt is always int).
     */
    ForLoopStep,

    /*
     * Native array-element read `a[i]` into a temp (Phase 5): `target2` = the
     * array's frame slot, `a` = the int index operand, `target` = the dst
     * temp slot; `node` is the Subscript (for the OutOfBoundsEx loc, and the
     * fallback below). When the slot holds a flat array it reads the element
     * directly - no Identifier/Subscript dispatch, mirroring
     * Subscript::eval_int/eval_float (negative index wraps, bounds throw). When
     * it's NOT an array (a dict / a general array), it falls back to
     * `node->eval_int` / `eval_float`. Int / float element variants.
     */
    LoadElemInt,
    LoadElemFloat,

    /*
     * Native array-VALUE-element read `a[i]` into a temp slot (Phase 5, for a
     * nested subscript `a[i][j]`): `target2` = the (general) array slot, `a` =
     * the int index, `target` = the dst temp; `node` = the Subscript. Reads the
     * i-th element VALUE (itself an array, since `a` is array<array<..>> - a
     * GENERAL array) straight from the element vector into the temp, so the
     * outer `a[i]` of a 2-D read is native; the temp is then the array slot of
     * an inner LoadElem/etc. A non-array / non-general base falls back to
     * `node->eval`. READ path only - a 2-D *write* through a temp would COW the
     * temp and not write back, so those stay fallback.
     */
    LoadElemValue,

    /*
     * Native array-element store `a[i] = v` / `a[i] OP= v` (Phase 5): `target2`
     * = the array slot, `a` = the int index operand, `b` = the value operand,
     * `aop` = the store op (`Op::invalid` = plain assign, else a compound arith
     * op: a[i] = a[i] <aop> v), `node` = the Expr14 (loc/fallback). For a FLAT
     * mutable int/float array it stores/updates the scalar directly, mirroring
     * try_flat_subscript_store (bounds, negative wrap, div/mod-by-zero checked
     * BEFORE any clone like the tree-walker, COW: a slice clones, an aliased
     * non-slice clones its live slices; then invalidate the cached hash). For a
     * const/read-only / general / bool / dyn-laundered base it falls back to
     * `node->eval` - SOUND because a compiled rvalue has no side effects, so
     * re-evaluating it is exact. The value ops are emitted before the index ops
     * so a both-throw case matches the tree-walker (rhs before index). Int /
     * float value variants.
     */
    StoreElemInt,
    StoreElemFloat,

    /*
     * Evaluate a scalar-returning CALL (a builtin - via the baked
     * DirectBuiltinCallExpr fn pointer - or a user function) into a temp slot
     * as an RValue (Phase 5): `target` = the dst temp, `node` = the CallExpr.
     * The result is then an ordinary int/float operand, so a loop body like
     * `s += sqrt(i)` / `s += f(i)` goes native instead of falling back whole.
     * The call still evaluates its own args (the builtin ABI takes the
     * unevaluated ExprList) - a fully Construct*-free builtin dispatch (args
     * pre-evaluated into slots) is later work.
     */
    EvalToSlot,

    /*
     * Load an immediate into a frame slot: slot[target] = <int/float literal>
     * (`a.lit` / `a.flit`). This is the clean "move a constant to a slot" that
     * a `var i = 0` / `x = 5` compiles to (vs an `IntBin dst = imm + 0`
     * add-with-zero). Trivial, so it is NOT counted by the has_native gate (a
     * body of only constant loads isn't worth running via the VM chunk).
     */
    LoadImmInt,
    LoadImmFloat,

    /*
     * The BOXED general-value path (the zero-fallback / dyn tier). These
     * operate on EvalValue frame slots (the same slots the typed ops use, read
     * as boxed values) and call the RUNTIME directly - num_bin_op / the Type
     * vtable - so a `dyn` / string / bool scalar expr runs native instead of
     * falling back to node->eval. No Construct* (a machine-code backend lowers
     * these to runtime-library calls). Args pre-evaluated into slots.
     *
     * LoadConstV  slot[target] = consts[target2]           (a baked literal)
     * MoveV       slot[target] = RValue(slot[target2])     (alias, no clone)
     * BinOpV      slot[target] = clone(RValue(slot[a])) <aop> RValue(slot[b])
     *             via num_bin_op (arith/bitwise Op); mirrors the tree-walker
     *             (clone the left operand, then mutate it - so `a+b` doesn't
     *             corrupt a), incl. int/float promotion + string `+` concat.
     */
    LoadConstV,
    MoveV,
    BinOpV,

    /*
     * Boxed compound-assign `dst OP= b` (dst = a frame slot, b a slot operand,
     * aop the BASE arith Op): copies the lvalue's value (NOT clone - so a
     * container mutates IN PLACE, matching the tree-walker's `newVal =
     * slot->get(); apply_compound_op(newVal, rhs, op)`), applies num_bin_op,
     * stores back. `node` = the statement (for the error loc). Only for a
     * dyn/string lvalue - a typed int/float compound is the IntBin/FloatBin
     * fast path.
     */
    CompoundV,

    /*
     * Boxed comparison `dst = (a <cmp> b)` -> a bool. Copies a, num_bin_op with
     * the comparison Type method (lt/gt/le/ge/eq/noteq), stores
     * EvalValue(result.is_true()) - exactly Expr06/Expr07::do_eval. `node` =
     * the right operand (error loc). A dyn/string cmp the typed path can't do.
     */
    CmpV,

    /*
     * Boxed logical `dst = a <&&|||> b` -> a bool. MyLang's `&&`/`||` do NOT
     * short-circuit at runtime (both operands always evaluate - only the
     * compile-time folder short-circuits const cases), so this is EAGER:
     * EvalValue(a.is_true() <op> b.is_true()), matching logop_loc /
     * Expr11/Expr12. `aop` = Op::land or Op::lor. No error path (is_true is
     * total).
     */
    LogV,

    /*
     * Boxed NON-LOCAL leaf loads (the boxed path's operand can be a global /
     * captured / builtin value, not only a resolved-local slot): read the value
     * into a temp `target` from `target2` = the table index. Mirror
     * Identifier::do_eval.
     *   LoadGlobalV   gfuncs->slots[target2] (throws UndefinedVariableEx via
     *                 `node` if !gfuncs->defined - a read before the decl ran)
     *   LoadCaptureV  (*captures)[target2] (a closure's per-instance capture)
     *   LoadBuiltinV  builtin_slot(target2) (an unshadowed builtin as a value)
     */
    LoadGlobalV,
    LoadCaptureV,
    LoadBuiltinV,

    /*
     * Boxed subscript READ `dst = base[idx]` (a general array / dict / string
     * element - the typed flat-array path is LoadElem*). `target2` = the base
     * slot, `a` = the index slot, `node` = the Subscript (error loc). Calls the
     * runtime Type::subscript(for_write=false) with an LValue* to the base slot
     * (matching Subscript::do_eval) and RValues the result into `target`.
     */
    SubscriptV,

    /*
     * Branch on a BOXED bool slot: if NOT slot[target2].is_true(), pc = target.
     * A boxed condition (`if (a == b)`, `while (x != none)`, `if (x)`) compiles
     * to <boxed expr into a slot> + this. Mirrors the tree-walker's is_true()
     * truthiness (0/none/[]/{} false).
     */
    JumpUnlessTrueV,

    /* Stop the program. */
    Halt,
};

/*
 * A register-machine operand: a resolved-local frame SLOT (the VM's registers
 * are the frame slots) or an immediate int literal. Read by the native int ops.
 */
struct Operand {
    bool is_lit = false;
    int slot = -1;         /* frame slot index when !is_lit */
    /* The immediate when is_lit. An int op (IntBin) reads `lit`, a float op
     * (FloatBin) reads `flit`; the two are mutually exclusive, so they share
     * storage - the operand is written and read as one kind throughout. */
    union {
        int_type lit = 0;
        float_type flit;
    };
};

struct Instr {
    OpCode op;
    const Construct *node = nullptr;  /* fallback op: the AST node; null else */
    int target = -1;    /* Jump/JumpIfFalse dest; LoopBackEdge cont; IntBin dst
                         * slot; JumpUnlessIntCmp jump dest */
    int target2 = -1;   /* LoopBackEdge exit dest */
    Op aop = Op::invalid;   /* IntBin: arith op; JumpUnlessIntCmp: compare op */
    Operand a;              /* IntBin / JumpUnlessIntCmp: left operand */
    Operand b;              /* IntBin / JumpUnlessIntCmp: right operand */
};

struct Chunk {
    std::vector<Instr> code;
    /*
     * Scratch frame slots the register machine needs for expression
     * intermediates, laid out ABOVE the resolved locals: temps occupy
     * [slot_count, slot_count + n_temps). vm_execute sizes the frame to fit
     * them. Zero when no native expression needed a temp.
     */
    int n_temps = 0;
    /*
     * The BOXED general-value path's constant pool: literal EvalValues baked at
     * codegen (a machine-code backend would put these in the data section),
     * each referenced by index from a LoadConstV. Empty until a boxed op needs
     * a literal operand.
     */
    std::vector<EvalValue> consts;
};
