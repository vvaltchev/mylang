/* SPDX-License-Identifier: BSD-2-Clause */

#include "codegen.h"
#include "inferencer.h"
#include "syntax.h"

/* eval.cpp's AST-shape purity check (see eval.h) - declared here to avoid
 * pulling the whole eval.h into the codegen TU. */
bool construct_no_side_effects(const Construct *c);

/*
 * The NO-FAIL contract: after show() is compile-time-folded
 * (fold_show_calls, inferencer.cpp), EVERY construct a script can produce
 * has a native lowering - so a statement none of the compilers accept is a
 * CODEGEN BUG, not a "fallback": throw loudly (always-on, not an assert; a
 * release build must refuse too) instead of emitting a tree-walking op.
 * This replaced the EvalStmt safety nets - the opcode itself is DELETED, so
 * a compiled chunk structurally CANNOT re-enter the AST.
 */
struct NotLoweredEx : public Exception {
    explicit NotLoweredEx(const Construct *s)
        : Exception("InternalErrorEx",
                    intern_msg("codegen: construct not lowered natively: "
                               + std::string(s->name)),
                    s->start, s->end) { }
};

[[noreturn]] static void throw_not_lowered(const Construct *s)
{
    throw NotLoweredEx(s);
}
#ifdef ML_DBG_FB
#include "coderender.h"
#endif

#include <vector>
#include <algorithm>   /* std::count (peephole_chunk) */

namespace {

/*
 * Collect resolved-LOCAL slot -> source name into `names` (debug info for the
 * -vd disassembler ONLY; the VM never reads it). A recursive walk over the
 * common node kinds - via the generic base classes (SingleChild / MultiOp /
 * MultiElem) plus the multi-field statements - so `for (var i...) s += i` reads
 * with names. It need not be exhaustive: a local under a node kind not covered
 * here simply shows as a scratch-style `rN` (harmless for a debug dump).
 */
void collect_slot_names(const Construct *c, std::vector<std::string> &names)
{
    if (!c)
        return;
    if (auto *id = dynamic_cast<const Identifier *>(c)) {
        if (id->sym.kind == SymKind::local && id->sym.slot >= 0) {
            if (static_cast<size_t>(id->sym.slot) >= names.size())
                names.resize(id->sym.slot + 1);
            if (names[id->sym.slot].empty())
                names[id->sym.slot] = std::string(id->get_str());
        }
        return;
    }
    auto rec = [&](const Construct *ch) { collect_slot_names(ch, names); };
    if (auto *sc = dynamic_cast<const SingleChildConstruct *>(c)) {
        rec(sc->elem.get()); return;
    }
    if (auto *mo = dynamic_cast<const MultiOpConstruct *>(c)) {
        for (auto &p : mo->elems) rec(p.second.get());
        return;
    }
    if (auto *me = dynamic_cast<const MultiElemConstruct<> *>(c)) {
        for (auto &e : me->elems) rec(e.get());
        return;
    }
    if (auto *e = dynamic_cast<const Expr14 *>(c)) {
        rec(e->lvalue.get()); rec(e->rvalue.get()); return;
    }
    if (auto *ce = dynamic_cast<const CallExpr *>(c)) {
        rec(ce->what.get()); rec(ce->args.get()); return;
    }
    if (auto *sub = dynamic_cast<const Subscript *>(c)) {
        rec(sub->what.get()); rec(sub->index.get()); return;
    }
    if (auto *m = dynamic_cast<const MemberExpr *>(c)) {
        rec(m->what.get()); return;
    }
    if (auto *iff = dynamic_cast<const IfStmt *>(c)) {
        rec(iff->condExpr.get()); rec(iff->thenBlock.get());
        rec(iff->elseBlock.get()); return;
    }
    if (auto *w = dynamic_cast<const WhileStmt *>(c)) {
        rec(w->condExpr.get()); rec(w->body.get()); return;
    }
    if (auto *f = dynamic_cast<const ForStmt *>(c)) {
        rec(f->init.get()); rec(f->cond.get());
        rec(f->inc.get()); rec(f->body.get()); return;
    }
    if (auto *fr = dynamic_cast<const ForRangeStmt *>(c)) {
        rec(fr->init.get()); rec(fr->bound.get());
        rec(fr->step.get()); rec(fr->body.get()); return;
    }
    if (auto *fe = dynamic_cast<const ForeachStmt *>(c)) {
        rec(fe->container.get()); rec(fe->body.get()); return;
    }
    if (auto *te = dynamic_cast<const TernaryExpr *>(c)) {
        rec(te->condExpr.get()); rec(te->thenExpr.get());
        rec(te->elseExpr.get()); return;
    }
}

/*
 * Body analysis for the struct-foreach direct read (try_native_struct_foreach):
 * true iff EVERY use of the loop-var slot `loop_slot` in `c` is a READ of a POD
 * SCALAR field (`p.x`). Then the loop var need not be materialized - each `p.x`
 * compiles to a direct byte read of the array element. Bails (false) on: a
 * whole-`p` use (a bare identifier - passed, assigned, compared, captured), a
 * `p.field` WRITE (`p.field = ..` / `p.field++`, which must NOT hit the array -
 * foreach `p` is a copy), a non-scalar field, or any node type we don't
 * recognize (an unhandled node could hide a `p` use). `lval` marks an
 * assignment-target context.
 */
static bool struct_fe_body_ok(const Construct *c, int loop_slot,
                              const StructTypeDef *def, bool lval)
{
    if (!c)
        return true;
    if (auto *id = dynamic_cast<const Identifier *>(c))
        return !(id->sym.kind == SymKind::local && id->sym.slot == loop_slot);
    if (auto *m = dynamic_cast<const MemberExpr *>(c)) {
        auto *bid = dynamic_cast<const Identifier *>(m->what.get());
        if (bid && bid->sym.kind == SymKind::local
            && bid->sym.slot == loop_slot) {
            if (lval)
                return false;                    /* a p.field WRITE */
            const FieldDef *f = def->field_of(m->memUid);
            if (!f || f->offset < 0)
                return false;
            return f->kind == FieldKind::f_int
                || f->kind == FieldKind::f_float
                || f->kind == FieldKind::f_bool;
        }
        return struct_fe_body_ok(m->what.get(), loop_slot, def, false);
    }
    if (auto *e = dynamic_cast<const Expr14 *>(c))
        return struct_fe_body_ok(e->lvalue.get(), loop_slot, def, true)
            && struct_fe_body_ok(e->rvalue.get(), loop_slot, def, false);
    if (auto *inc = dynamic_cast<const IncDecExpr *>(c))
        return struct_fe_body_ok(inc->lvalue.get(), loop_slot, def, true);
    auto all = [&](std::initializer_list<const Construct *> cs) {
        for (const Construct *ch : cs)
            if (!struct_fe_body_ok(ch, loop_slot, def, false))
                return false;
        return true;
    };
    if (auto *mo = dynamic_cast<const MultiOpConstruct *>(c)) {
        for (const auto &pr : mo->elems)
            if (!struct_fe_body_ok(pr.second.get(), loop_slot, def, false))
                return false;
        return true;
    }
    if (auto *ts = dynamic_cast<const TypedScalarExpr *>(c)) {
        for (const auto &pr : ts->elems)
            if (!struct_fe_body_ok(pr.second.get(), loop_slot, def, false))
                return false;
        return true;
    }
    if (auto *me = dynamic_cast<const MultiElemConstruct<> *>(c)) {
        for (const auto &el : me->elems)
            if (!struct_fe_body_ok(el.get(), loop_slot, def, false))
                return false;
        return true;
    }
    if (auto *sc = dynamic_cast<const SingleChildConstruct *>(c))
        return struct_fe_body_ok(sc->elem.get(), loop_slot, def, false);
    if (auto *ce = dynamic_cast<const CallExpr *>(c))
        return all({ce->what.get(), ce->args.get()});
    if (auto *sub = dynamic_cast<const Subscript *>(c))
        return all({sub->what.get(), sub->index.get()});
    if (auto *iff = dynamic_cast<const IfStmt *>(c))
        return all({iff->condExpr.get(), iff->thenBlock.get(),
                    iff->elseBlock.get()});
    if (auto *w = dynamic_cast<const WhileStmt *>(c))
        return all({w->condExpr.get(), w->body.get()});
    if (auto *f = dynamic_cast<const ForStmt *>(c))
        return all({f->init.get(), f->cond.get(), f->inc.get(),
                    f->body.get()});
    if (auto *fr = dynamic_cast<const ForRangeStmt *>(c))
        return all({fr->init.get(), fr->bound.get(), fr->step.get(),
                    fr->body.get()});
    if (auto *fe2 = dynamic_cast<const ForeachStmt *>(c))
        return all({fe2->container.get(), fe2->body.get()});
    if (auto *te = dynamic_cast<const TernaryExpr *>(c))
        return all({te->condExpr.get(), te->thenExpr.get(),
                    te->elseExpr.get()});
    if (auto *co = dynamic_cast<const CoalesceExpr *>(c))
        return all({co->lhs.get(), co->rhs.get()});
    if (auto *ret = dynamic_cast<const ReturnStmt *>(c))
        return struct_fe_body_ok(ret->elem.get(), loop_slot, def, false);
    if (dynamic_cast<const Literal *>(c)
        || dynamic_cast<const ChildlessConstruct *>(c))
        return true;                             /* a childless leaf */
    return false;                                /* unrecognized -> bail */
}

/*
 * True for an op that WRITES its result into `.target` as a pure value and does
 * not also READ `.target` - so it is safe to retarget its `.target` to a
 * different (fresh) slot. Used to fuse away `<produce t>; MoveV rD = t` into a
 * single `<produce rD>` (see emit_args_range). Excludes a jump/branch (whose
 * `.target` is a code label), a store (writes memory), CompoundV (reads its
 * target), and the loop back-edges.
 */
bool op_writes_pure_target(OpCode op)
{
    switch (op) {
    case OpCode::LoadConstV:  case OpCode::LoadImmInt:
    case OpCode::LoadLiteralObjV:
    case OpCode::LoadImmFloat: case OpCode::LoadGlobalV:
    case OpCode::LoadCaptureV: case OpCode::LoadBuiltinV:
    case OpCode::MoveV:       case OpCode::BinOpV:
    case OpCode::CmpV:        case OpCode::LogV:
    case OpCode::IntBin:      case OpCode::FloatBin:
    case OpCode::SubscriptV:  case OpCode::MemberV:
    case OpCode::SliceV:
    case OpCode::CallV:       case OpCode::CachedCallV:
    case OpCode::CallBuiltinV: case OpCode::CallBuiltinLV:
    case OpCode::ArrLen:
    case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
    case OpCode::LoadElemValue: case OpCode::MakeArrayV:
    case OpCode::MakeDictV:      case OpCode::MakeClosureV:
    case OpCode::StructCtorV:
        return true;
    default:
        return false;
    }
}

Operand int_lit(int_type v)
{
    Operand o;
    o.is_lit = true;
    o.lit_kind = Operand::LitKind::i;
    o.lit = v;
    return o;
}

Operand bool_lit(bool v)
{
    Operand o;
    o.is_lit = true;
    o.lit_kind = Operand::LitKind::b;
    o.lit = v ? 1 : 0;
    return o;
}

Operand slot_op(int slot)
{
    Operand o;
    o.is_lit = false;
    o.slot = slot;
    return o;
}

/* A struct-ctor field arg the POD coerce can't throw on: a typed int/float
 * (th) OR a scalar LITERAL (int/float/bool) - the latter covers an auto-const-
 * folded arg whose literal never got a `th` stamp (auto-const runs after
 * inference). A scalar literal's type is self-evident and inference already
 * rejected a non-fitting one, so coerce is safe. Shared by the StructCtorV and
 * MakeStructArrayV gates. */
bool is_typed_scalar_arg(const Construct *a)
{
    return a->th == TypeHint::i || a->th == TypeHint::f
        || dynamic_cast<const LiteralInt *>(a)
        || dynamic_cast<const LiteralFloat *>(a)
        || dynamic_cast<const LiteralBool *>(a);
}

/* A field arg for POD field `fd` that `coerce_struct_field` provably cannot
 * throw on: either a typed scalar (above) for a scalar field, OR a NESTED POD
 * struct construction `Q(..)` whose type IS the field's struct type (an
 * embedded `f_struct` field of a POD parent). The nested StructCtorV produces
 * exactly a `Q` StructObject, so the parent's `coerce_struct_field` f_struct
 * branch (def->name == fd.struct_ty) always passes - no per-arg loc needed.
 * (A dyn arg, a non-matching struct, or a general/boxed field arg fails and the
 * whole ctor falls back to the tree-walker, which reports the exact arg loc.) */
bool pod_ctor_arg_safe(const Construct *a, const FieldDef &fd)
{
    if (fd.kind == FieldKind::f_struct) {
        const CallExpr *ce = dynamic_cast<const CallExpr *>(a);
        return ce && ce->vm_struct_ctor_def
            && ce->vm_struct_ctor_def->name == fd.struct_ty;
    }
    return is_typed_scalar_arg(a);
}

/*
 * Read `e` as a LEAF int operand: a resolved-local int frame slot, or an int
 * literal. Returns false for anything else. Only `SymKind::local` slots are
 * registers here; a bool slot also carries `th == i` and is read defensively
 * (as 0/1) at runtime.
 */
bool as_int_operand(const Construct *e, Operand &out)
{
    if (const LiteralInt *li = dynamic_cast<const LiteralInt *>(e)) {
        out = int_lit(li->ival());
        return true;
    }
    if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
        if (id->sym.kind == SymKind::local && id->th == TypeHint::i) {
            out = slot_op(id->sym.slot);
            return true;
        }
    }
    return false;
}

/* An inc-dec `x++`/`--a[i]` USED AS A VALUE lowers to read-lvalue + mutate (or
 * mutate + read for prefix), which evaluates the lvalue's slot/index TWICE (once
 * to read, once in the store). That is sound only if the lvalue has no side
 * effect and reads the SAME storage both times - so gate on a bare resolved-id
 * scalar (local/global/capture; the slot itself), or a flat int/float subscript
 * whose index is an `as_int_operand` leaf (a local slot or int literal, so the
 * two evals agree). A dict/member/complex-index inc-dec-as-value falls back. */
/* A mutating builtin that REQUIRES its arg0 to be an lvalue and throws
 * NotLValueEx on a value — append/push/pop/insert/erase/intptr. EXCLUDES
 * sort/rev_sort/reverse (also lvalue-arg builtins, but they ACCEPT a value arg0
 * — a const's copy — so a non-lvalue arg0 there is not an error). Used to decide
 * whether a provably-non-lvalue arg0 is an always-throw (ThrowRuntimeV). */
bool builtin_requires_lvalue_arg0(std::string_view name)
{
    return name == "append" || name == "push" || name == "pop"
        || name == "insert" || name == "erase" || name == "intptr";
}

bool incdec_lvalue_pure(const Construct *lv)
{
    if (const Identifier *id = dynamic_cast<const Identifier *>(lv))
        return !id->is_const
            && (id->sym.kind == SymKind::local
                || id->sym.kind == SymKind::global
                || id->sym.kind == SymKind::capture);
    if (const Subscript *sub = dynamic_cast<const Subscript *>(lv)) {
        Operand idx;
        /* A flat `a[i]` (slot base + immediate index), OR a NESTED `a[i][j]`
         * whose inner base `a[i]` is a Subscript over a slotted array with an
         * immediate index - both re-eval the same side-effect-free slot/index
         * in the read and the store. */
        if (const Subscript *inner =
                dynamic_cast<const Subscript *>(sub->what.get())) {
            Operand k1, k2;
            return dynamic_cast<const Identifier *>(inner->what.get())
                && as_int_operand(inner->index.get(), k1)
                && as_int_operand(sub->index.get(), k2);
        }
        return sub->base_array
            && (sub->th == TypeHint::i || sub->th == TypeHint::f)
            && as_int_operand(sub->index.get(), idx);
    }
    /* A struct/dict field on a slotted-id base (`p.x`, `d.k`): the value read
     * (MemberV) + the mutate (StoreMemberV/DictStore) both re-eval `p`, a
     * side-effect-free slot read. The emit_mut `th`-gate keeps a dyn field on
     * the fallback (inc-dec is int/float-only). */
    if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(lv)) {
        const Identifier *base = dynamic_cast<const Identifier *>(m->what.get());
        return (m->base_struct || m->base_dict) && base
            && (base->sym.kind == SymKind::local
                || base->sym.kind == SymKind::global
                || base->sym.kind == SymKind::capture);
    }
    return false;
}

Operand float_lit(float_type v)
{
    Operand o;
    o.is_lit = true;
    o.lit_kind = Operand::LitKind::f;
    o.flit = v;
    return o;
}

/* The array base of a subscript `a[i]` as a resolved-local slot (whatever it
 * holds - the LoadElem op checks it's an array at runtime, else falls back). A
 * nested base (`a[i][j]`, `obj.arr[i]`) is not a bare local slot -> false. */
bool as_array_slot(const Construct *e, int &slot)
{
    if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
        if (id->sym.kind == SymKind::local) {
            slot = id->sym.slot;
            return true;
        }
    }
    return false;
}

/*
 * Like as_array_slot, but ALSO accepts a GLOBAL (a top-level container a
 * function reads) or CAPTURE base, returning `kind` (0 local / 1 global / 2
 * capture) so a store op can form the base LValue* from the right table (the
 * VM's vm_store_base). Used by the container-STORE ops (a[i]/d[k]/s.f), where
 * mutating through a global/capture base is sound (an array COWs in place
 * through the slot's LValue, a dict mutates the shared object). Not for the
 * flat READ fast paths, which stay local-only.
 */
bool as_container_base(const Construct *e, int &slot, int &kind)
{
    if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
        switch (id->sym.kind) {
        case SymKind::local:   kind = 0; slot = id->sym.slot; return true;
        case SymKind::global:  kind = 1; slot = id->sym.slot; return true;
        case SymKind::capture: kind = 2; slot = id->sym.slot; return true;
        default: break;
        }
    }
    return false;
}

/*
 * Read `e` as a LEAF float operand: an int/float literal (converted to float),
 * or a resolved-local slot (read as float at runtime - an int/bool slot
 * promotes). Returns false otherwise. Accepts a `th == i` operand too, since a
 * float expression promotes an int operand.
 */
bool as_float_operand(const Construct *e, Operand &out)
{
    if (const LiteralInt *li = dynamic_cast<const LiteralInt *>(e)) {
        out = float_lit(static_cast<float_type>(li->ival()));
        return true;
    }
    if (const LiteralFloat *lf = dynamic_cast<const LiteralFloat *>(e)) {
        out = float_lit(lf->fval());
        return true;
    }
    if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
        if (id->sym.kind == SymKind::local
            && (id->th == TypeHint::f || id->th == TypeHint::i)) {
            out = slot_op(id->sym.slot);
            return true;
        }
    }
    return false;
}

/*
 * Is `e` a DEFINITELY-int (never bool) expression? An arithmetic or unary-minus
 * TypedScalarExpr always yields int (arith promotes bool operands to int); an
 * int literal is int. A bare leaf Identifier is ambiguous (`th == i` also
 * covers bool) and a comparison/logical yields bool - both excluded. Used to
 * decide a PLAIN assignment `x = rhs` is safe (writing an int result to a bool
 * slot would corrupt it; but a valid `x = <int>` requires x to accept int, so x
 * is int - never bool).
 */
bool definitely_int(const Construct *e)
{
    if (dynamic_cast<const LiteralInt *>(e))
        return true;
    const TypedScalarExpr *t = dynamic_cast<const TypedScalarExpr *>(e);
    return t && t->kind == TypeHint::i
        && (t->cat == TypedScalarExpr::Cat::arith
            || t->cat == TypedScalarExpr::Cat::neg);
}

/* An arith/bitwise Op the boxed BinOpV handles (has a binop_pmf). Comparisons
 * and logical ops use a different runtime dispatch - the boxed path grows to
 * cover them later. */
bool is_boxed_binop(Op op)
{
    switch (op) {
        case Op::plus:  case Op::minus: case Op::times:
        case Op::div:   case Op::mod:
        case Op::band:  case Op::bor:   case Op::bxor:
        case Op::shl:   case Op::shr:   case Op::ushr:
            return true;
        default:
            return false;
    }
}

/* A comparison Op the boxed CmpV handles (has a cmp_pmf). */
bool is_boxed_cmp(Op op)
{
    switch (op) {
        case Op::lt: case Op::gt: case Op::le:
        case Op::ge: case Op::eq: case Op::noteq:
            return true;
        default:
            return false;
    }
}

/* The BASE arith Op of a boxed compound-assign (`+=` -> plus), or Op::invalid
 * if `op` isn't a compound-assign the boxed CompoundV handles. */
Op compound_base_op(Op op)
{
    switch (op) {
        case Op::addeq: return Op::plus;
        case Op::subeq: return Op::minus;
        case Op::muleq: return Op::times;
        case Op::diveq: return Op::div;
        case Op::modeq: return Op::mod;
        default:        return Op::invalid;
    }
}

/* Bake a scalar/string literal into an EvalValue for the boxed const pool.
 * Returns false for a non-scalar literal (an array/dict LiteralObj) - later. */
bool boxed_literal(const Construct *e, EvalValue &out)
{
    if (const LiteralInt *li = dynamic_cast<const LiteralInt *>(e)) {
        out = EvalValue(li->ival());
        return true;
    }
    if (const LiteralFloat *lf = dynamic_cast<const LiteralFloat *>(e)) {
        out = EvalValue(lf->fval());
        return true;
    }
    if (const LiteralBool *lb = dynamic_cast<const LiteralBool *>(e)) {
        out = EvalValue(lb->bval());
        return true;
    }
    if (const LiteralStr *ls = dynamic_cast<const LiteralStr *>(e)) {
        out = ls->strval();
        return true;
    }
    if (dynamic_cast<const LiteralNone *>(e)) {
        out = EvalValue();   /* none */
        return true;
    }
    return false;
}

/*
 * Lowers a statement list to a Chunk. `if`/`while` become native jumps
 * (Phase 1); a `while` whose condition + body are resolved-local int ops
 * compiles to the register machine (Phase 2), the VM's registers being the
 * frame slots. Nested int expressions use scratch TEMP slots laid out above the
 * resolved locals. See plans/bytecode-vm.md.
 */
struct Codegen {

    Chunk chunk;

    /* Temp (scratch register) allocator. temp_base == the frame's slot_count;
     * temps grow above it. Reset to base per statement (a statement's temps are
     * dead once its result is stored); max_temp is the high-water mark that
     * sizes the frame. */
    int temp_base = 0;
    int next_temp = 0;
    int max_temp = 0;
    int max_dict_iters = 0;   /* # native dict foreachs -> n_dict_iters */
    int max_dyn_iters = 0;    /* # native dyn foreachs -> n_dyn_iters */

    /* Active struct-foreach direct-read mapping: while compiling a
     * try_native_struct_foreach body, a MemberExpr reading `sfe_loop_slot`'s
     * scalar field compiles to a LoadStructField* (a direct byte read of
     * sfe_arr_slot[sfe_ctr_slot].field) instead of materializing the loop var.
     * sfe_loop_slot == -1 when inactive. */
    int sfe_loop_slot = -1;
    int sfe_arr_slot = -1;
    int sfe_ctr_slot = -1;
    const StructTypeDef *sfe_def = nullptr;

    int here() const { return static_cast<int>(chunk.code.size()); }

    /* CODEGEN-SCRATCH node registry: `Instr::node_idx` indexes it - the
     * splice-stable handle an op uses to reach its node before its final pc
     * is known (ops grow + roll back; an index survives that where a pc
     * would not). It exists ONLY until extract_locs harvests the locs;
     * verify_ast_free then asserts every node_idx was nulled. It lives on
     * the CODEGEN object, NOT the Chunk - the finished chunk holds NO
     * Construct* (EvalStmt / node_table are deleted, so nothing at runtime
     * can reference a node). */
    std::vector<const Construct *> ast_nodes;

    /* Register an op's AST node for loc extraction; -1 for null. */
    int add_ast_node(const Construct *n)
    {
        if (!n)
            return -1;
        ast_nodes.push_back(n);
        return static_cast<int>(ast_nodes.size()) - 1;
    }

    /* Pool a value-ABI builtin call's AST-free data (the Builtin + the ArgLocs
     * carets/hint pulled off the DirectBuiltinCallExpr) and return its
     * Chunk::builtin_calls index - so CallBuiltinV carries a serializable index,
     * not a `node`. See Chunk::BuiltinCall. */
    int add_builtin_call(const DirectBuiltinCallExpr *dc)
    {
        Chunk::BuiltinCall bc;
        bc.builtin = dc->builtin;
        bc.name = nullptr;
        if (const Identifier *id =
                dynamic_cast<const Identifier *>(dc->what.get()))
            bc.name = id->uid;
        bc.start = dc->args->start;
        bc.end = dc->args->end;
        bc.arr_hint = dc->args->arr_hint;
        bc.args.reserve(dc->args->elems.size());
        for (const auto &el : dc->args->elems)
            bc.args.push_back(ArgLoc{el->start, el->end});
        chunk.builtin_calls.push_back(std::move(bc));
        return static_cast<int>(chunk.builtin_calls.size()) - 1;
    }

    size_t emit(OpCode op, const Construct *node = nullptr,
                int target = -1, int target2 = -1)
    {
        Instr in;
        in.op = op;
        in.node_idx = add_ast_node(node);
        in.target = target;
        in.target2 = target2;
        chunk.code.push_back(in);
        return chunk.code.size() - 1;
    }

    int alloc_temp()
    {
        const int t = next_temp++;
        if (next_temp > max_temp)
            max_temp = next_temp;
        return t;
    }

    /* Emit a ThrowRuntimeV for an always-throwing construct: pool the exception
     * kind + caret (+ name) and append the op. AST-free. See Chunk::ThrowSite. */
    void emit_throw(Chunk::ThrowKind kind, Loc s, Loc e,
                    const UniqueId *name, std::vector<Instr> &ops)
    {
        Instr in;
        in.op = OpCode::ThrowRuntimeV;
        in.target = static_cast<int>(chunk.throws.size());
        chunk.throws.push_back({ kind, s, e, name });
        ops.push_back(in);
    }

    void reset_temps() { next_temp = temp_base; }

    /* A distinct persistent dict-iterator state slot per native dict foreach.
     * NEVER reset - nested/sequential dict foreachs in a chunk each keep their
     * own live-iterator slot (sized into Chunk::n_dict_iters). */
    int alloc_dict_iter()
    {
        const int id = max_dict_iters++;
        return id;
    }

    /* A distinct persistent dyn-foreach iterator state slot (like
     * alloc_dict_iter, sized into Chunk::n_dyn_iters). */
    int alloc_dyn_iter()
    {
        const int id = max_dyn_iters++;
        return id;
    }

    /*
     * Native break/continue (Gap B): a loop body's `break`/`continue` compiles
     * to a Jump, recorded here and backpatched when the loop closes - `breaks`
     * to the loop exit (Lend), `conts` to the loop's continue point (a while's
     * cond re-test, a for's increment / the fused ForLoopStep). A stack, so a
     * break in a NESTED loop targets the innermost loop. (`return` needs no
     * entry: its flow==ret stops the chunk - see
     * compile_scalar_body / vm_run_chunk.)
     */
    struct LoopFrame {
        std::vector<size_t> breaks;
        std::vector<size_t> conts;
        /* trys.size() when this loop was pushed: a break/continue inside it
         * crosses only the trys pushed AFTER this (trys[try_depth..]), which is
         * how `while{try{break}}` (crosses the try) differs from
         * `try{while{break}}` (does not). */
        size_t try_depth = 0;
    };
    std::vector<LoopFrame> loops;

    /* P8 Inc 2c: the enclosing try regions between the current point and the
     * function boundary (innermost last). A break/continue/return crossing them
     * must pop each handler + run each finally. `to_fin` collects the Jumps
     * that a return in this try routes to its finally (backpatched to Lfin);
     * `in_catch` = we're compiling a catch body (this try's handler was already
     * popped by the exception dispatch, so a return must NOT pop it again). */
    struct TryFrame {
        bool has_finally;
        std::vector<size_t> *to_fin;   /* normal/reraise exits -> shared Lfin */
        bool in_catch;
        /* the finally body (or null): a flow op crossing this try INLINES it at
         * the flow-op site (Inc 2c), so it needs the AST here. */
        const Construct *finally_body = nullptr;
    };
    std::vector<TryFrame> trys;

    /* An INLINED-CALL return boundary (InlinedCallExpr): the callee's body runs
     * with its OWN return scope - a `return v` inside it yields THIS expression's
     * value (into `rslot`) and JUMPs to the body's end, rather than ReturnV-ing
     * the whole chunk (mirrors InlinedCallExpr::do_eval's FlowState swap). Innermost
     * last (a nested inlined call pushes another). `try_base` = trys.size() when
     * the boundary opened: a return that crosses a finally INSIDE the boundary
     * (deeper than try_base) can't be redirected cheaply, so it bails the inline
     * (rare). `jumps` collects the redirected returns' Jumps, backpatched to the
     * body end. */
    struct InlineRet {
        int rslot;
        size_t try_base;
        std::vector<size_t> jumps;
    };
    std::vector<InlineRet> inline_returns;

    /* Push a loop frame, recording the current try-nesting depth so a
     * break/continue inside can tell which trys it crosses (step 3). */
    void push_loop()
    {
        loops.push_back({});
        loops.back().try_depth = trys.size();
    }

    void pop_loop(int lend, int lcont)
    {
        for (size_t j : loops.back().breaks)
            chunk.code[j].target = lend;
        for (size_t j : loops.back().conts)
            chunk.code[j].target = lcont;
        loops.pop_back();
    }

    /* A loop's init (`var i = start`), emitted ONCE: native (a LoadImmInt /
     * store) when it is a resolved-local scalar decl.
     * Native-izing the once-run init doesn't speed the loop, but it keeps the
     * `i = start` off the tree-walker and reads cleanly in the disassembly. */
    bool emit_init(const Construct *init)
    {
        reset_temps();
        const size_t mark = chunk.code.size();
        if (compile_int_stmt(init, chunk.code))
            return true;
        chunk.code.resize(mark);
        reset_temps();
        if (compile_float_stmt(init, chunk.code))
            return true;
        chunk.code.resize(mark);
        reset_temps();
        /* Boxed decl-init: `var k = i` (an int/bool leaf, or a dyn/string
         * value) - the int path rejects a bare identifier rhs (it can't prove
         * int-not-bool for a raw int store), but a boxed move preserves the
         * rhs's real type. An UNCOMPILABLE init (exotic) fails the whole
         * loop to a NotLoweredEx compile abort - no per-init fallback. */
        if (compile_boxed_stmt(init, chunk.code))
            return true;
        chunk.code.resize(mark);
        return false;
    }

    int add_const(const EvalValue &v)
    {
        chunk.consts.push_back(v);
        return static_cast<int>(chunk.consts.size()) - 1;
    }

    /* Pull a MemberExpr's read data (name key/uid, optional flag, both carets)
     * into the member-key pool so MemberV is a bare index, no `node`. */
    int add_member_key(const MemberExpr *m)
    {
        chunk.member_keys.push_back(
            {m->memId, m->memUid, m->optional,
             m->start, m->end, m->what->start, m->what->end});
        return static_cast<int>(chunk.member_keys.size()) - 1;
    }

    /* Pool a checked inc-dec's dual carets (+ the member key for the member
     * form): lvalue-child caret + the whole inc-dec caret. Returns the index
     * (carried in Instr::b), so the op is AST-free. */
    int add_incdec_site(const Construct *lchild, const IncDecExpr *inc,
                        const MemberExpr *m = nullptr)
    {
        Chunk::IncDecSite s;
        s.lstart = lchild->start;
        s.lend = lchild->end;
        s.istart = inc->start;
        s.iend = inc->end;
        if (m) {
            s.memId = m->memId;
            s.memUid = m->memUid;
        }
        chunk.incdec_sites.push_back(std::move(s));
        return static_cast<int>(chunk.incdec_sites.size()) - 1;
    }

    /* Pool the per-step subscript carets of a nested store (StoreElem2V /
     * StoreElemChainV), INSIDE-OUT to match the keys. Returns the pool index. */
    int add_chain_locs(const std::vector<const Construct *> &nodes)
    {
        std::vector<std::pair<Loc, Loc>> v;
        v.reserve(nodes.size());
        for (const Construct *n : nodes)
            v.push_back({n->start, n->end});
        chunk.chain_locs.push_back(std::move(v));
        return static_cast<int>(chunk.chain_locs.size()) - 1;
    }

    /*
     * BOXED general-value expression (the zero-fallback / dyn tier) -> ops,
     * result left in `out_slot` (a resolved-local slot, or a temp). Handles a
     * resolved-local leaf (its slot, no op), a scalar/string literal (into
     * the const pool + LoadConstV), and an arith/bitwise binary op (a BinOpV
     * chain, each into a temp). Returns false for anything else - a
     * global/capture/builtin leaf, a comparison/logical op, a call, subscript,
     * member - which the boxed path grows to cover. The ops call the RUNTIME
     * (num_bin_op) directly, no node->eval, so a `dyn`/string scalar expr is
     * native. Self-truncating: on a nested failure the caller discards.
     */
    /*
     * The FUSED flat-struct-array literal `[P(a,b), P(c,d), ..]` -> a single
     * MakeStructArrayV (F-4, erases the split-op regression). Every element must
     * be a ctor of the SAME POD struct (vm_struct_ctor_def), arity == nfields,
     * every field arg a typed scalar (is_typed_scalar_arg - so coerce can't
     * throw). The N structs' field args are compiled INTERLEAVED into one run
     * [base, base + N*M) (struct i's field j at base + i*M + j), and the op
     * coerces them straight into a flat byte buffer. A mixed / nested / non-
     * scalar-arg literal declines here and falls to StructCtorV + MakeArrayV.
     */
    bool try_make_struct_array(const LiteralArray *la, int &out_slot,
                               std::vector<Instr> &ops)
    {
        const int n = static_cast<int>(la->elems.size());
        if (n == 0)
            return false;
        const StructTypeDef *sdef = nullptr;
        for (const auto &el : la->elems) {
            const CallExpr *ce = dynamic_cast<const CallExpr *>(el.get());
            if (!ce || !ce->vm_struct_ctor_def || !ce->args)
                return false;
            if (!sdef)
                sdef = ce->vm_struct_ctor_def;
            else if (ce->vm_struct_ctor_def != sdef)
                return false;
            if (ce->args->elems.size() != sdef->fields.size())
                return false;
            for (const auto &a : ce->args->elems)
                if (!is_typed_scalar_arg(a.get()))
                    return false;
        }
        const int M = static_cast<int>(sdef->fields.size());

        const size_t mark = ops.size();
        const size_t cmark = chunk.consts.size();
        const int save_top = next_temp;
        const int base = next_temp;
        next_temp += n * M;
        if (next_temp > max_temp)
            max_temp = next_temp;

        for (int i = 0; i < n; i++) {
            const CallExpr *ce =
                static_cast<const CallExpr *>(la->elems[i].get());
            for (int j = 0; j < M; j++)
                if (!compile_to_run_slot(ce->args->elems[j].get(),
                                         base + i * M + j, ops)) {
                    ops.resize(mark);
                    next_temp = save_top;
                    chunk.consts.resize(cmark);
                    return false;
                }
        }

        const int dst = alloc_temp();
        Instr in;
        in.op = OpCode::MakeStructArrayV;
        in.node_idx = add_ast_node(la->elems[0].get());   /* a ctor: defensive coerce loc (nulled) */
        in.target = dst;
        in.set_a(int_lit(base));
        in.set_b(int_lit(n));
        in.target2 = static_cast<int>(chunk.struct_defs.size());
        chunk.struct_defs.push_back(sdef);
        ops.push_back(in);
        out_slot = dst;
        return true;
    }

    bool compile_boxed_expr(const Construct *e, int &out_slot,
                            std::vector<Instr> &ops)
    {
        /*
         * Typed-arg lowering: a th==i/f node computes its int/float value via
         * the UNBOXED typed path (IntBin/FloatBin/CallV/LoadElem...), which
         * writes a VALID int/float EvalValue into the result slot - so a boxed
         * consumer reads it fine. Prefer that over a boxed BinOpV+num_bin_op:
         * `fib(n-1) + fib(n-2)` in a return becomes an IntBin, not a `bin.v`.
         * If the typed path can't lower this node (a comparison-as-value, an
         * un-typed shape, a global/capture leaf), roll back what it emitted and
         * fall through to the boxed cases below - so this only ever turns a
         * boxed op into the equivalent typed one, never changes behavior.
         */
        if (e->th == TypeHint::i || e->th == TypeHint::f) {
            const size_t omark = ops.size();
            const size_t cmark = chunk.consts.size();
            const int save_top = next_temp;
            Operand top;
            const bool ok = e->th == TypeHint::i
                ? compile_int_expr(e, top, ops)
                : compile_float_expr(e, top, ops);
            if (ok) {
                if (!top.is_lit) {
                    out_slot = top.slot;
                    return true;
                }
                /* an immediate result (a bare literal) -> materialize a slot */
                const int t = alloc_temp();
                Instr ld;
                ld.op = e->th == TypeHint::i ? OpCode::LoadImmInt
                                             : OpCode::LoadImmFloat;
                ld.target = t;
                ld.set_a(top);
                ops.push_back(ld);
                out_slot = t;
                return true;
            }
            ops.resize(omark);
            chunk.consts.resize(cmark);
            next_temp = save_top;
        }

        /* An inc-dec USED AS A VALUE (`y = x++`, `y = a[i]++`, `y = ++x`): the
         * result is the OLD value (postfix) or the NEW value (prefix). Lower it
         * to read-lvalue + mutate (postfix) / mutate + read (prefix), reusing the
         * statement compilers for the in-place mutation. Gated on a side-effect-
         * free lvalue (incdec_lvalue_pure) so the read and the store's second
         * eval of the slot/index agree; a MoveV copies the scalar, so the
         * captured `dst` is unaffected by the mutation that follows. */
        if (const IncDecExpr *inc = dynamic_cast<const IncDecExpr *>(e)) {
            if (!incdec_lvalue_pure(inc->lvalue.get()))
                return try_incdec_chain(inc, out_slot, ops);
            const size_t omark = ops.size();
            const size_t cmark = chunk.consts.size();
            const int st = next_temp;
            const int dst = alloc_temp();
            auto emit_mut = [&]() {
                return compile_int_stmt(inc, ops)
                    || compile_float_stmt(inc, ops)
                    || compile_boxed_stmt(inc, ops);
            };
            const bool ok = inc->is_prefix
                ? (emit_mut()
                   && compile_to_run_slot(inc->lvalue.get(), dst, ops))
                : (compile_to_run_slot(inc->lvalue.get(), dst, ops)
                   && emit_mut());
            if (!ok) {
                ops.resize(omark);
                chunk.consts.resize(cmark);
                next_temp = st;
                /* a pure lvalue the statement/read compilers still declined:
                 * the chain op is the catch-all there too. */
                return try_incdec_chain(inc, out_slot, ops);
            }
            out_slot = dst;
            return true;
        }

        /* A NON-TAIL block-body inline (`y = f(args)` where f block-inlines with
         * a residual that couldn't collapse to a ternary): the callee's body
         * runs behind its OWN return boundary - a `return v` inside it yields
         * THIS expression's value, not the enclosing function's (mirrors
         * InlinedCallExpr::do_eval's FlowState swap). Lower it to a scoped return
         * boundary: init a result slot to `none` (a fall-through body yields
         * none), compile the body's statements inline, and redirect each `return`
         * to "MoveV into the result slot + Jump to the body end" (try_native_
         * return, gated on inline_returns). A body statement or return that
         * can't lower (e.g. a try crossed inside the boundary) fails the whole
         * inline -> the tree-walker runs the InlinedCallExpr (byte-identical). */
        if (const InlinedCallExpr *ic =
                dynamic_cast<const InlinedCallExpr *>(e)) {
            const size_t omark = ops.size();
            const size_t cmark = chunk.consts.size();
            const int st = next_temp;
            const int rslot = alloc_temp();
            Instr ld;
            ld.op = OpCode::LoadConstV;
            ld.target = rslot;
            ld.target2 = add_const(EvalValue());   /* none (fall-through) */
            ops.push_back(ld);
            /* Reserve rslot below temp_base so the body's per-stmt reset_temps
             * can't reuse it. */
            const int saved_base = temp_base;
            temp_base = next_temp;
            if (temp_base > max_temp)
                max_temp = temp_base;
            inline_returns.push_back({rslot, trys.size(), {}});
            const bool ok = compile_scalar_body(body_stmts(ic->elem.get()),
                                                /*is_loop_body=*/false);
            const InlineRet ir = inline_returns.back();
            inline_returns.pop_back();
            temp_base = saved_base;
            if (!ok) {
                ops.resize(omark);
                chunk.consts.resize(cmark);
                next_temp = st;
                return false;
            }
            const int lend = here();
            for (size_t j : ir.jumps)
                ops[j].target = lend;
            next_temp = rslot + 1;      /* keep rslot live for the consumer */
            out_slot = rslot;
            return true;
        }

        /* A CALL whose callee is an UNRESOLVED name (`undef(5)`) throws
         * UndefinedVariableEx when the callee evaluates — BEFORE the args (the
         * tree-walker's `what->eval` first). Native ThrowRuntimeV with the
         * callee's caret; the args are never compiled (never evaluated). */
        if (const CallExpr *ce = dynamic_cast<const CallExpr *>(e)) {
            const Identifier *callee =
                dynamic_cast<const Identifier *>(ce->what.get());
            if (callee && callee->sym.kind == SymKind::unresolved) {
                emit_throw(Chunk::ThrowKind::undefined_var,
                           callee->start, callee->end, callee->uid, ops);
                out_slot = alloc_temp();   /* dead: the op always throws */
                return true;
            }
        }

        if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
            if (id->sym.kind == SymKind::local) {
                out_slot = id->sym.slot;
                return true;   /* the slot IS the operand - no op */
            }
            /* An UNRESOLVED name in an rvalue position always throws
             * UndefinedVariableEx (the tree-walker's RValue of the UndefinedId
             * sentinel) -> a native ThrowRuntimeV with the id's own caret. (A
             * bare `foobar;` discarded statement is skipped by the discarded-
             * expr path's leaf guard, so this only fires in a genuine rvalue
             * position - an assignment rhs, an operand, an arg, a callee.) */
            if (id->sym.kind == SymKind::unresolved) {
                emit_throw(Chunk::ThrowKind::undefined_var,
                           id->start, id->end, id->uid, ops);
                out_slot = alloc_temp();   /* dead: the op always throws */
                return true;
            }
            /* A global / capture / builtin leaf -> load into a temp. */
            OpCode op;
            switch (id->sym.kind) {
                case SymKind::global:  op = OpCode::LoadGlobalV;  break;
                case SymKind::capture: op = OpCode::LoadCaptureV; break;
                case SymKind::builtin: op = OpCode::LoadBuiltinV; break;
                default: return false;
            }
            const int t = alloc_temp();
            Instr in;
            in.op = op;
            in.node_idx = add_ast_node(id);              /* for the undefined-global loc/name */
            in.target = t;
            in.target2 = id->sym.slot;
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        EvalValue lit;
        if (boxed_literal(e, lit)) {
            const int t = alloc_temp();
            Instr in;
            in.op = OpCode::LoadConstV;
            in.node_idx = add_ast_node(e);
            in.target = t;
            in.target2 = add_const(lit);
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        /* A baked const array/dict/struct literal (LiteralObj) - what a
         * `var a = [1,2,3]` / `d = {}` const-literal rvalue folds to ->
         * LoadLiteralObjV materializes it from the pool (immutable share vs a
         * fresh mutable clone, plus the general/flat_s cases). Node-free. */
        if (const LiteralObj *lo = dynamic_cast<const LiteralObj *>(e)) {
            const int t = alloc_temp();
            Instr in;
            in.op = OpCode::LoadLiteralObjV;
            in.target = t;
            in.target2 = static_cast<int>(chunk.literal_objs.size());
            chunk.literal_objs.push_back(
                {lo->literal_value(), lo->is_immutable(), lo->arr_hint,
                 lo->arr_hint_struct});
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        /* A lambda `func [caps] (params) {..}` in EXPRESSION position (id ==
         * null - a named nested func decl is a statement) -> MakeClosureV: it
         * creates the FuncObject + snapshots captures from ctx, like
         * FuncDeclStmt::do_eval; the Instr carries only the pool index. */
        if (const FuncDeclStmt *fd = dynamic_cast<const FuncDeclStmt *>(e)) {
            if (fd->id)
                return false;
            const int t = alloc_temp();
            Instr in;
            in.op = OpCode::MakeClosureV;
            in.target = t;
            in.target2 = static_cast<int>(chunk.closure_defs.size());
            chunk.closure_defs.push_back(fd->desc);
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        /* A standalone POD struct construction `P(x, y)` -> StructCtorV:
         * the field args into a register run, then coerce them into the POD
         * compile the field args into a register run, then coerce them into
         * the POD bytes. Gated on a POD ctor (vm_struct_ctor_def), nargs ==
         * nfields (no
         * skipped-opt fill - POD has no opt fields), a small field count, and
         * EVERY arg a typed scalar (th==i/f). The typed-arg gate is what keeps
         * coerce from throwing (the inferencer already rejected a non-fitting
         * typed arg), so no per-arg loc is needed; a nested-struct-field arg (a
         * `Q(..)`, th==none) or a dyn arg fails the gate and falls back to the
         * tree-walker, which reports the exact arg loc. The append-fused
         * ctor is
         * EmplaceStruct. */
        if (const CallExpr *ce = dynamic_cast<const CallExpr *>(e)) {
            const StructTypeDef *sdef = ce->vm_struct_ctor_def;
            if (sdef && ce->args
                && ce->args->elems.size() == sdef->fields.size()
                && ce->args->elems.size() <= 16) {
                /* Every arg must be one coerce can't throw on
                 * (pod_ctor_arg_safe): a typed scalar for a scalar field, OR a
                 * NESTED POD-struct construction of the exact field type for an
                 * embedded struct field (`L(P(1,2), P(3,4))`). A dyn/general arg
                 * fails and falls back. */
                bool all_typed = true;
                for (size_t ai = 0; ai < ce->args->elems.size(); ai++)
                    if (!pod_ctor_arg_safe(ce->args->elems[ai].get(),
                                           sdef->fields[ai])) {
                        all_typed = false;
                        break;
                    }
                if (all_typed) {
                    const size_t cmark = chunk.consts.size();
                    int base;
                    if (!emit_args_range(ce->args->elems, base, ops)) {
                        chunk.consts.resize(cmark);
                        return false;
                    }
                    const int dst = alloc_temp();
                    Instr in;
                    in.op = OpCode::StructCtorV;
                    in.node_idx = add_ast_node(ce);   /* loc for a defensive throw (nulled) */
                    in.target = dst;
                    in.set_a(int_lit(base));
                    in.set_b(int_lit(
                        static_cast<int>(ce->args->elems.size())));
                    in.target2 = static_cast<int>(chunk.struct_defs.size());
                    chunk.struct_defs.push_back(sdef);
                    ops.push_back(in);
                    out_slot = dst;
                    return true;
                }
            }
        }

        /* A BOXED (non-POD) struct construction `B(a, x)` -> StructCtorBoxedV.
         * Every field arg compiles into the register run (compile_to_run_slot
         * handles ids / subscripts / nested ctors / literals); the boxed_ctors
         * pool carries the def + per-arg carets (a field coerce CAN throw on a
         * dyn-laundered value, and must report the arg's caret). A const-arg
         * boxed ctor already folded to a LiteralObj, so only a runtime-arg one
         * reaches here. */
        if (const CallExpr *ce = dynamic_cast<const CallExpr *>(e)) {
            /* ...and the CHECKED POD ctor: a POD construction whose typed
             * gate failed above (a dyn/general arg - the coerce CAN throw
             * and needs the arg's caret) takes the same pooled-caret op;
             * construct_struct_boxed_from_values dispatches on is_pod. */
            const StructTypeDef *bdef = ce->vm_struct_boxed_def
                ? ce->vm_struct_boxed_def
                : (ce->vm_struct_ctor_def && ce->args
                       && ce->args->elems.size()
                              == ce->vm_struct_ctor_def->fields.size()
                       ? ce->vm_struct_ctor_def : nullptr);
            if (bdef && ce->args && ce->args->elems.size() <= 16) {
                const size_t cmark = chunk.consts.size();
                int base;
                if (!emit_args_range(ce->args->elems, base, ops)) {
                    chunk.consts.resize(cmark);
                    return false;
                }
                Chunk::BoxedCtor bc;
                bc.def = bdef;
                for (const auto &a : ce->args->elems)
                    bc.arg_locs.push_back({ a->start, a->end });
                const int dst = alloc_temp();
                Instr in;
                in.op = OpCode::StructCtorBoxedV;
                in.target = dst;
                in.set_a(int_lit(base));
                in.target2 = static_cast<int>(chunk.boxed_ctors.size());
                chunk.boxed_ctors.push_back(std::move(bc));
                ops.push_back(in);
                out_slot = dst;
                return true;
            }
        }

        /* A FLAT STRUCT array literal `[P(a,b), P(c,d)]` (arr_hint flat_s, F-4)
         * whose elements are all same-POD-struct ctors with all-scalar field
         * args -> the FUSED MakeStructArrayV (coerce the field args straight
         * into the flat buffer, no intermediate StructObject per element). This
         * BEATS the tree-walker; try it before the per-element StructCtorV +
         * MakeArrayV path below (which stays the fallback for a mixed / nested /
         * non-scalar-arg literal). */
        if (const LiteralArray *la = dynamic_cast<const LiteralArray *>(e))
            if (la->arr_hint == ArrHint::flat_s) {
                int r;
                if (try_make_struct_array(la, r, ops)) {
                    out_slot = r;
                    return true;
                }
            }

        /* An array LITERAL `[a, b, ..]` whose elements aren't all const ->
         * MakeArrayV: compile the element run, then build the array. A
         * fully-const literal is a baked LoadConstV (boxed_literal above) or a
         * LiteralObj (left to the fallback), so only a literal with element
         * NODES reaches here. A FLAT STRUCT array whose fused op above declined
         * (a nested/dyn field arg) still lowers here: each `P(..)` element
         * compiles to a StructCtorV producing a POD StructObject, and
         * build_array_from_values packs a run of same-type POD structs into flat
         * mode-5 storage VALUE-DRIVEN (the def comes off the first element). If a
         * struct-ctor element can't lower either, emit_args_range fails and this
         * bails to the fallback. */
        if (const LiteralArray *la = dynamic_cast<const LiteralArray *>(e)) {
            const size_t cmark = chunk.consts.size();
            int base;
            if (!emit_args_range(la->elems, base, ops)) {
                chunk.consts.resize(cmark);   /* emit_args_range won't undo */
                return false;
            }
            const int dst = alloc_temp();
            Instr in;
            in.op = OpCode::MakeArrayV;
            in.target = dst;
            in.set_a(int_lit(base));
            in.set_b(int_lit(static_cast<int>(la->elems.size())));
            in.target2 = static_cast<int>(la->arr_hint);
            ops.push_back(in);
            out_slot = dst;
            return true;
        }

        /* A dict LITERAL `{k0: v0, ..}` (elements not all const) -> MakeDictV:
         * compile the key/value pairs INTERLEAVED into a register run [base,
         * base + 2*npairs) (key at even, value at odd), then build. Same
         * const/LiteralObj caveat as the array case. */
        if (const LiteralDict *ld = dynamic_cast<const LiteralDict *>(e)) {
            const size_t mark = ops.size();
            const size_t cmark = chunk.consts.size();
            const int save_top = next_temp;
            const int npairs = static_cast<int>(ld->elems.size());
            const int base = next_temp;
            next_temp += 2 * npairs;
            if (next_temp > max_temp)
                max_temp = next_temp;
            bool ok = true;
            for (int i = 0; i < npairs && ok; i++)
                ok = compile_to_run_slot(ld->elems[i]->key.get(),
                                         base + 2 * i, ops)
                  && compile_to_run_slot(ld->elems[i]->value.get(),
                                         base + 2 * i + 1, ops);
            if (!ok) {
                ops.resize(mark);
                next_temp = save_top;
                chunk.consts.resize(cmark);
                return false;
            }
            const int dst = alloc_temp();
            Instr in;
            in.op = OpCode::MakeDictV;
            in.target = dst;
            in.set_a(int_lit(base));
            in.set_b(int_lit(npairs));
            ops.push_back(in);
            out_slot = dst;
            return true;
        }

        /* A subscript READ `a[i]` (general array / dict / string element) ->
         * SubscriptV via the runtime Type::subscript. (A slice is a separate
         * node, not handled here.) */
        if (const Subscript *sub = dynamic_cast<const Subscript *>(e)) {
            int base_slot, idx_slot;
            if (!compile_boxed_expr(sub->what.get(), base_slot, ops)
                || !compile_boxed_expr(sub->index.get(), idx_slot, ops))
                return false;
            const int t = alloc_temp();
            Instr in;
            in.op = OpCode::SubscriptV;
            in.node_idx = add_ast_node(sub);
            in.target = t;
            in.target2 = base_slot;
            in.set_a(slot_op(idx_slot));
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        /* A member READ `obj.f` / `d.k` -> MemberV (shared member_read). */
        if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(e)) {
            int base_slot;
            if (!compile_boxed_expr(m->what.get(), base_slot, ops))
                return false;
            const int t = alloc_temp();
            Instr in;
            in.op = OpCode::MemberV;
            in.node_idx = add_ast_node(m);                 /* extract_locs nulls it */
            in.target = t;
            in.target2 = base_slot;
            in.set_a(int_lit(add_member_key(m)));   /* AST-free: pool index */
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        /* A slice READ `base[start:end]` -> SliceV. Compile base, then the
         * optional bounds (in eval order: what, start, end), each into a slot;
         * an absent bound is a slot of -1. The op calls the runtime slice(). */
        if (const Slice *sl = dynamic_cast<const Slice *>(e)) {
            int base_slot;
            if (!compile_boxed_expr(sl->what.get(), base_slot, ops))
                return false;
            int start_slot = -1, end_slot = -1;
            if (sl->start_idx
                && !compile_boxed_expr(sl->start_idx.get(), start_slot, ops))
                return false;
            if (sl->end_idx
                && !compile_boxed_expr(sl->end_idx.get(), end_slot, ops))
                return false;
            const int t = alloc_temp();
            Instr in;
            in.op = OpCode::SliceV;
            in.node_idx = add_ast_node(sl);                /* extract_locs nulls it */
            in.target = t;
            in.target2 = base_slot;
            in.set_a(slot_op(start_slot));  /* -1 == absent */
            in.set_b(slot_op(end_slot));
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        /* A native user-function call `f(args...)` -> CallV. */
        if (const DirectCallExpr *dc = dynamic_cast<const DirectCallExpr *>(e))
            if (try_native_call(dc, out_slot, ops))
                return true;

        /* A FOLDED type query (type/decltype/typestr/kindstr): the inferencer
         * BAKED the answer into args[0] (a LiteralStr/LiteralObj) and the
         * builtin just returns it - so ELIDE the call and compile that baked
         * literal directly (LoadConstV / LoadLiteralObjV), no builtin call,
         * AST-free. (tq_folded is set only when the fold ran, so a -nti
         * `typestr("hi")` is NOT elided - it stays a real call building from the
         * value.) The NON-folded type query is a value-ABI builtin below. */
        if (const DirectBuiltinCallExpr *bc =
                dynamic_cast<const DirectBuiltinCallExpr *>(e))
            if (bc->tq_folded && bc->args && bc->args->elems.size() == 1)
                return compile_boxed_expr(bc->args->elems[0].get(), out_slot,
                                          ops);

        /* A native builtin call. map/filter get the validate-first sequence
         * (CheckFuncV + MapFilterV); value-ABI builtins get CallBuiltinV. */
        if (const DirectBuiltinCallExpr *bc =
                dynamic_cast<const DirectBuiltinCallExpr *>(e)) {
            if (try_native_defined_global(bc, out_slot, ops))
                return true;
            if (try_native_defined_expr(bc, out_slot, ops))
                return true;
            if (try_native_map_filter(bc, out_slot, ops))
                return true;
            if (try_native_builtin(bc, out_slot, ops))
                return true;
        }

        /* An indirect call of a func VALUE (a closure / lambda / func var):
         * a plain CallExpr whose callee is Func-typed -> CallValueV. */
        if (const CallExpr *call = dynamic_cast<const CallExpr *>(e))
            if (try_native_value_call(call, out_slot, ops))
                return true;

        /* A ternary `cond ? a : b` as a VALUE: compute cond, branch on it, and
         * evaluate exactly ONE of the two arms into `dst`. This is what makes a
         * recursion-unroll return (fib: `return (n-1<2 ? .. : f(..)+f(..))..`)
         * go native - each arm's own calls become CallV. `dst` is reserved
         * BELOW the arms' scratch so neither arm clobbers it. */
        if (const TernaryExpr *t = dynamic_cast<const TernaryExpr *>(e)) {
            const size_t mark = ops.size();
            const int save_top = next_temp;
            const int dst = alloc_temp();
            const int scratch = next_temp;

            int cslot;
            if (!compile_boxed_expr(t->condExpr.get(), cslot, ops)) {
                ops.resize(mark); next_temp = save_top; return false;
            }
            Instr jf;
            jf.op = OpCode::JumpUnlessTrueV;
            jf.target2 = cslot;
            const size_t jf_i = ops.size();
            ops.push_back(jf);
            next_temp = scratch;

            int aslot;
            if (!compile_boxed_expr(t->thenExpr.get(), aslot, ops)) {
                ops.resize(mark); next_temp = save_top; return false;
            }
            Instr mva;
            mva.op = OpCode::MoveV; mva.target = dst; mva.target2 = aslot;
            ops.push_back(mva);
            const size_t jmp_i = ops.size();
            Instr jend; jend.op = OpCode::Jump;
            ops.push_back(jend);
            next_temp = scratch;

            ops[jf_i].target = static_cast<int>(ops.size());   /* else arm */

            int bslot;
            if (!compile_boxed_expr(t->elseExpr.get(), bslot, ops)) {
                ops.resize(mark); next_temp = save_top; return false;
            }
            Instr mvb;
            mvb.op = OpCode::MoveV; mvb.target = dst; mvb.target2 = bslot;
            ops.push_back(mvb);
            next_temp = scratch;

            ops[jmp_i].target = static_cast<int>(ops.size());   /* merge */
            out_slot = dst;
            return true;
        }

        /* Null-coalescing `a ?? b` as a VALUE (R1 of the Tier-1 endgame):
         * evaluate the lhs into `dst`; a NON-none lhs skips the rhs entirely
         * (the short-circuit - CoalesceExpr::do_eval's exact semantics, the
         * rhs never evaluated); else the rhs lands in `dst`. `dst` is
         * reserved below both sides' scratch, like the ternary.
         * JumpIfNotNoneV is a pure none test - no node, no loc. */
        if (const CoalesceExpr *co = dynamic_cast<const CoalesceExpr *>(e)) {
            const size_t mark = ops.size();
            const int save_top = next_temp;
            const int dst = alloc_temp();
            const int scratch = next_temp;

            int lslot;
            if (!compile_boxed_expr(co->lhs.get(), lslot, ops)) {
                ops.resize(mark); next_temp = save_top; return false;
            }
            Instr mvl;
            mvl.op = OpCode::MoveV; mvl.target = dst; mvl.target2 = lslot;
            ops.push_back(mvl);
            Instr jn;
            jn.op = OpCode::JumpIfNotNoneV;
            jn.set_a(slot_op(dst));
            const size_t jn_i = ops.size();
            ops.push_back(jn);
            next_temp = scratch;

            int rslot;
            if (!compile_boxed_expr(co->rhs.get(), rslot, ops)) {
                ops.resize(mark); next_temp = save_top; return false;
            }
            Instr mvr;
            mvr.op = OpCode::MoveV; mvr.target = dst; mvr.target2 = rslot;
            ops.push_back(mvr);
            next_temp = scratch;

            ops[jn_i].target = static_cast<int>(ops.size());   /* merge */
            out_slot = dst;
            return true;
        }

        /* An ASSIGNMENT as an EXPRESSION (`x = y = 5`, `var z = (q += 7)` -
         * R3 of the Tier-1 endgame): compile the assignment via the ordinary
         * STATEMENT dispatch (int/float/boxed - the same three tiers
         * gen_stmt uses), then the expression's value is the target's stored
         * value: Expr14::do_eval returns the (coerced) rval, and after the
         * store a LOCAL target's slot holds exactly that - the same handle
         * for a container, so aliasing (`a = (b = [1,2])`, same intptr) is
         * identical. Only a resolved-LOCAL identifier target; a global/
         * capture/IdList target declines (the readback would need an extra
         * load - rare shapes). */
        if (const Expr14 *e14 = dynamic_cast<const Expr14 *>(e)) {
            const Identifier *lv =
                dynamic_cast<const Identifier *>(e14->lvalue.get());
            if (!lv || lv->sym.kind != SymKind::local || lv->is_const)
                return false;
            const size_t mark = ops.size();
            const int save_top = next_temp;
            if (compile_int_stmt(e14, ops)) {
                out_slot = lv->sym.slot;
                return true;
            }
            ops.resize(mark);
            next_temp = save_top;
            if (compile_float_stmt(e14, ops)) {
                out_slot = lv->sym.slot;
                return true;
            }
            ops.resize(mark);
            next_temp = save_top;
            if (compile_boxed_stmt(e14, ops)) {
                out_slot = lv->sym.slot;
                return true;
            }
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }

        /* An arith (`a+b`) / comparison (`a<b`) / logical (`a&&b`) chain, as a
         * raw ExprNN OR a TypedScalarExpr - `A && B` of comparisons specializes
         * to a logical even when the comparisons are boxed over a dyn operand,
         * so both forms reach here. emit_boxed_chain handles both. */
        if (const TypedScalarExpr *t =
                dynamic_cast<const TypedScalarExpr *>(e)) {
            /* A typed `!x` (Cat::lnot) as a VALUE boxes a real BOOL - the
             * boxed UnaryV (its !is_true == the typed eval's `!= 0` boxing).
             * Was undetected while expr-bodied functions had no chunks
             * (`func neg(bool b) => !b` never compiled); the no-fail contract
             * flushed it out. */
            if (t->cat == TypedScalarExpr::Cat::lnot && t->elems.size() == 1) {
                int oslot;
                if (!compile_boxed_expr(t->elems[0].second.get(), oslot, ops))
                    return false;
                const int dst = alloc_temp();
                Instr in;
                in.op = OpCode::UnaryV;
                in.node_idx = add_ast_node(e);
                in.target = dst;
                in.set_a(slot_op(oslot));
                in.aop = Op::lnot;
                ops.push_back(in);
                out_slot = dst;
                return true;
            }
            const char k = t->cat == TypedScalarExpr::Cat::arith   ? 'a'
                         : t->cat == TypedScalarExpr::Cat::cmp     ? 'c'
                         : t->cat == TypedScalarExpr::Cat::logical ? 'l'
                                                                   : 0;
            return k && emit_boxed_chain(t->elems, k, e, out_slot, ops);
        }
        char k = 0;
        /* Boxed UNARY (`!x`/`-x`/`~x`/`+x`) over a dyn/general operand -> UnaryV
         * (a typed int/float unary is the M8 path at the top). Expr02 is a
         * MultiOpConstruct with a single (op, operand): op==invalid is just the
         * operand (compile it), else the unary op. */
        if (const Expr02 *u = dynamic_cast<const Expr02 *>(e)) {
            if (u->elems.size() != 1)
                return false;
            const Op op = u->elems[0].first;
            const Construct *operand = u->elems[0].second.get();
            if (op == Op::invalid)
                return compile_boxed_expr(operand, out_slot, ops);
            if (op != Op::lnot && op != Op::minus && op != Op::bnot
                && op != Op::plus)
                return false;
            int oslot;
            if (!compile_boxed_expr(operand, oslot, ops))
                return false;
            const int t = alloc_temp();
            Instr in;
            in.op = OpCode::UnaryV;
            in.node_idx = add_ast_node(e);   /* loc for a `-str`/`~str` error */
            in.target = t;
            in.set_a(slot_op(oslot));
            in.aop = op;
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        if (dynamic_cast<const Expr03 *>(e) || dynamic_cast<const Expr04 *>(e)
            || dynamic_cast<const Expr05 *>(e)
            || dynamic_cast<const Expr08 *>(e)
            || dynamic_cast<const Expr09 *>(e)
            || dynamic_cast<const Expr10 *>(e))
            k = 'a';
        else if (dynamic_cast<const Expr06 *>(e)
                 || dynamic_cast<const Expr07 *>(e))
            k = 'c';
        else if (dynamic_cast<const Expr11 *>(e)
                 || dynamic_cast<const Expr12 *>(e))
            k = 'l';
        else
            return false;
        return emit_boxed_chain(
            static_cast<const MultiOpConstruct *>(e)->elems, k, e,
            out_slot, ops);
    }

    /*
     * The shared boxed chain builder: `a OP b OP ...` -> a BinOpV (k='a'),
     * CmpV (k='c', 2-operand only), or LogV (k='l') chain, each result into a
     * temp, left-associative. Every operand compiles via compile_boxed_expr
     * (a leaf / literal / nested chain). An arith/cmp error is stamped at the
     * RIGHT operand (matching num_binop_loc -> stamp_operand_loc); a logical
     * has no error path. Returns false if an op isn't of the kind or an operand
     * can't compile. Used for both the raw ExprNN and TypedScalarExpr forms.
     */
    bool emit_boxed_chain(
        const std::vector<std::pair<Op, unique_ptr<Construct>>> &elems,
        char k, const Construct *node, int &out_slot, std::vector<Instr> &ops)
    {
        if (elems.empty() || elems[0].first != Op::invalid)
            return false;
        for (size_t i = 1; i < elems.size(); i++) {
            const Op op = elems[i].first;
            const bool ok = k == 'a' ? is_boxed_binop(op)
                          : k == 'c' ? is_boxed_cmp(op)
                                     : (op == Op::land || op == Op::lor);
            if (!ok)
                return false;
        }
        Operand acc_op;
        if (!boxed_operand(elems[0].second.get(), acc_op, ops))
            return false;
        for (size_t i = 1; i < elems.size(); i++) {
            Operand rhs_op;
            if (!boxed_operand(elems[i].second.get(), rhs_op, ops))
                return false;
            const int t = alloc_temp();
            Instr in;
            in.op = k == 'a' ? OpCode::BinOpV
                  : k == 'c' ? OpCode::CmpV
                             : OpCode::LogV;
            in.node_idx = add_ast_node(k == 'l' ? node : elems[i].second.get());
            in.target = t;
            in.set_a(acc_op);
            in.set_b(rhs_op);
            in.aop = elems[i].first;
            ops.push_back(in);
            acc_op = slot_op(t);   /* the result temp feeds the next op */
        }
        out_slot = acc_op.slot;
        return true;
    }

    /*
     * A boxed operand: an int/float/bool LITERAL becomes an IMMEDIATE operand
     * (no LoadConstV - a boxed op materializes it, like a CPU immediate); any
     * other expression compiles to a slot. Returns false if a non-literal can't
     * lower.
     */
    bool boxed_operand(const Construct *e, Operand &out,
                       std::vector<Instr> &ops)
    {
        if (auto *li = dynamic_cast<const LiteralInt *>(e)) {
            out = int_lit(li->ival());
            return true;
        }
        if (auto *lf = dynamic_cast<const LiteralFloat *>(e)) {
            out = float_lit(lf->fval());
            return true;
        }
        if (auto *lb = dynamic_cast<const LiteralBool *>(e)) {
            out = bool_lit(lb->bval());
            return true;
        }
        int slot;
        if (!compile_boxed_expr(e, slot, ops))
            return false;
        out = slot_op(slot);
        return true;
    }

    /*
     * BOXED general-value ASSIGNMENT `local = <boxed expr>` -> ops. Fires only
     * for a resolved-local lvalue whose decl needs NO numeric coercion
     * (decl_type none / dyn), so a plain slot write matches the tree-walker's
     * doAssign (a typed int/float/bool/str decl coerces via coerce_to_decl_type
     * - left to the fallback). Covers a decl, a plain assign (both write the
     * slot), AND a compound-assign (`+=` -> a CompoundV read-modify-write). A
     * plain assign's producing op is retargeted to write the lvalue directly (a
     * leaf copy uses MoveV, an alias matching doAssign). Rolls back the const
     * pool on failure.
     */
    /*
     * Multi-assign destructure of an ARRAY LITERAL: `a, b, c = [e0, e1, e2]` ->
     * compile each element into a snapshot temp, then distribute the snapshots
     * to the target slots. This ELIMINATES the array entirely (no MakeArrayV
     * heap alloc per iteration - the real win over building-then-unpacking) and
     * is box-free for int/float elements. Snapshot-FIRST (all elements compiled
     * before any target write) makes it swap-safe (`a, b = [b, a]`), matching
     * the tree-walker (build array, then bind each). Returns false (-> the
     * statement uses MultiUnpackV, the tree-walker's strict
     * handle_single_expr14) unless: the rvalue is a LiteralArray of EXACTLY the
     * target count (an arity mismatch stays a runtime strict error via the
     * fallback), and every target is a real (non-`_`), resolved-local,
     * non-const identifier with no type-coercion. So a scalar-spread (`a,b=0`),
     * a
     * `_` placeholder, a non-literal array (`a,b = f()`), and a const/typed
     * target all fall back - byte-identical under the differential.
     */
    bool try_multi_literal_store(const Expr14 *e, const IdList *il,
                                 std::vector<Instr> &ops)
    {
        const LiteralArray *la =
            dynamic_cast<const LiteralArray *>(e->rvalue.get());
        if (!la)
            return false;
        const int n = static_cast<int>(il->elems.size());
        if (n == 0 || static_cast<int>(la->elems.size()) != n)
            return false;
        for (const auto &t : il->elems) {
            if (t->is_underscore() || t->sym.kind != SymKind::local
                || t->is_const
                || (t->decl_type != DeclType::none
                    && t->decl_type != DeclType::dyn))
                return false;
        }

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();
        const int save_top = next_temp;
        const int snap_base = next_temp;
        next_temp += n;
        if (next_temp > max_temp)
            max_temp = next_temp;

        for (int i = 0; i < n; i++) {
            if (!compile_to_run_slot(la->elems[i].get(), snap_base + i, ops)) {
                ops.resize(omark);
                next_temp = save_top;
                chunk.consts.resize(cmark);
                return false;
            }
        }
        /* distribute: every snapshot is now computed, so the writes are
         * simultaneous (swap-safe). */
        for (int i = 0; i < n; i++) {
            Instr mv;
            mv.op = OpCode::MoveV;
            mv.node_idx = add_ast_node(e);
            mv.target = il->elems[i]->sym.slot;
            mv.target2 = snap_base + i;
            ops.push_back(mv);
        }
        next_temp = save_top;   /* free the snapshot run */
        return true;
    }

    /* Multi-assign SCALAR SPREAD `a, b, c = <non-array scalar>` -> the strict
     * rule spreads the SAME value to every target, so compile it ONCE into a
     * temp and MoveV (copy/alias) it to each target slot - no array, no runtime
     * destructure-vs-spread branch. Gated on a provably-non-array rvalue: a
     * proven int/float (th) or a SCALAR literal (`Literal` - LiteralInt/Bool/
     * Float/Str/None; NOT LiteralObj, which is an array/dict). An array / dyn
     * rvalue falls back to the strict `handle_single_expr14` destructure. */
    bool try_multi_scalar_spread(const Expr14 *e, const IdList *il,
                                 std::vector<Instr> &ops)
    {
        const Construct *rv = e->rvalue.get();
        const bool scalar = rv->th == TypeHint::i || rv->th == TypeHint::f
                            || dynamic_cast<const Literal *>(rv);
        if (!scalar)
            return false;
        const int n = static_cast<int>(il->elems.size());
        if (n == 0)
            return false;
        for (const auto &t : il->elems) {
            if (t->is_underscore() || t->sym.kind != SymKind::local
                || t->is_const
                || (t->decl_type != DeclType::none
                    && t->decl_type != DeclType::dyn))
                return false;
        }

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();
        const int save_top = next_temp;
        const int vslot = alloc_temp();
        if (!compile_to_run_slot(rv, vslot, ops)) {
            ops.resize(omark);
            next_temp = save_top;
            chunk.consts.resize(cmark);
            return false;
        }
        for (int i = 0; i < n; i++) {
            Instr mv;
            mv.op = OpCode::MoveV;
            mv.node_idx = add_ast_node(e);
            mv.target = il->elems[i]->sym.slot;
            mv.target2 = vslot;
            ops.push_back(mv);
        }
        next_temp = save_top;   /* free the value temp */
        return true;
    }

    /* Multi-assign `a, b, c = <rvalue>` GENERAL (F-1): the array-elide fast
     * paths (literal / scalar-spread) didn't apply - the rvalue is a CONST
     * array literal (a folded LiteralObj), a runtime array VALUE (a subscript /
     * var / call), or a dyn. Compile the rvalue into a temp + emit MultiUnpackV
     * (target slots in `chunk.unpack_targets`); the op does the tree-walker's
     * STRICT destructure (array -> length-checked distribute; else spread).
     * Every target must be a real or `_` slot, resolved-local, non-const,
     * non-typed (or dyn) - a typed/const/map target falls back. */
    bool try_multi_unpack(const Expr14 *e, const IdList *il,
                          std::vector<Instr> &ops,
                          Op compound_op = Op::invalid)
    {
        const size_t n = il->elems.size();
        if (n == 0)
            return false;
        bool any_coerce = false;
        for (const auto &t : il->elems) {
            if (t->is_underscore())
                continue;               /* `_` is a skipped slot */
            if (t->sym.kind != SymKind::local || t->is_const)
                return false;
            /* A typed int/float target coerces per stored value on a PLAIN
             * assign (R5); every other declared type is a no-op coerce. A
             * COMPOUND doesn't coerce (op==assign only - measured), so its
             * typed targets lower as-is. */
            if (compound_op == Op::invalid
                && (t->decl_type == DeclType::i
                    || t->decl_type == DeclType::f))
                any_coerce = true;
        }

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();
        const int save_top = next_temp;
        int rslot;
        if (!compile_boxed_expr(e->rvalue.get(), rslot, ops)) {
            ops.resize(omark);
            next_temp = save_top;
            chunk.consts.resize(cmark);
            return false;
        }

        std::vector<int32_t> targets;
        targets.reserve(n);
        for (const auto &t : il->elems)
            targets.push_back(t->is_underscore()
                                  ? -1 : static_cast<int32_t>(t->sym.slot));
        Instr in;
        in.op = OpCode::MultiUnpackV;
        /* The strict-length caret matches the tree-walker: its IdList lvalue
         * carries no loc, so the error stamps the enclosing Expr14's span
         * (`a, b, c = <rvalue>`) via Construct::eval - so record `e`, not il. */
        in.node_idx = add_ast_node(e);
        in.aop = compound_op;   /* invalid == plain assign; else `t OP= elem` */
        in.set_a(slot_op(rslot));
        in.target = static_cast<int>(chunk.unpack_targets.size());
        chunk.unpack_targets.push_back(std::move(targets));
        if (any_coerce) {
            std::vector<unsigned char> kinds;
            kinds.reserve(n);
            for (const auto &t : il->elems)
                kinds.push_back(
                    t->is_underscore()               ? 0
                    : t->decl_type == DeclType::i    ? 1
                    : t->decl_type == DeclType::f    ? 2
                                                     : 0);
            in.set_b(int_lit(static_cast<int>(chunk.unpack_coerce.size())));
            chunk.unpack_coerce.push_back(std::move(kinds));
        }
        ops.push_back(in);

        next_temp = save_top;
        return true;
    }

    bool compile_boxed_stmt(const Construct *s, std::vector<Instr> &ops)
    {
        /* A global `g++`/`g--` or closure-capture `cap++`/`cap--` statement ->
         * a compound StoreGlobalV/StoreCaptureV (x += 1 / x -= 1). A LOCAL
         * inc-dec is handled earlier (compile_int/float_stmt); a subscript one
         * in the store codegen; a global/capture id reaches here. A typed /
         * const operand falls back. */
        if (const IncDecExpr *inc = dynamic_cast<const IncDecExpr *>(s)) {
            const Identifier *id =
                dynamic_cast<const Identifier *>(inc->lvalue.get());
            if (id && !id->is_const
                && (id->decl_type == DeclType::none
                    || id->decl_type == DeclType::dyn)) {
                const bool numeric = inc->th == TypeHint::i
                                  || inc->th == TypeHint::f;
                /* A PROVEN int/float global/capture -> StoreGlobalV/StoreCaptureV
                 * compound (`x += 1`; the operand is numeric so `+` == inc-dec).
                 * A proven int/float LOCAL was handled by compile_int/float_stmt
                 * (IntBin/FloatBin) before this. */
                if (numeric && (id->sym.kind == SymKind::global
                                || id->sym.kind == SymKind::capture)) {
                    Instr in;
                    in.op = id->sym.kind == SymKind::global
                                ? OpCode::StoreGlobalV : OpCode::StoreCaptureV;
                    in.node_idx = add_ast_node(s);
                    in.target = id->sym.slot;
                    in.set_a(int_lit(1));
                    in.aop = inc->is_inc ? Op::plus : Op::minus;
                    ops.push_back(in);
                    return true;
                }
                /* A DYN/general scalar (local/global/capture) -> the int/float-
                 * CHECKED inc-dec: a compound `+= 1` would CONCAT a string, but
                 * `d++` must THROW (inc-dec is int/float-only). */
                if (!numeric) {
                    int kind;
                    switch (id->sym.kind) {
                    case SymKind::local:   kind = 0; break;
                    case SymKind::capture: kind = 2; break;
                    case SymKind::global:  kind = 1; break;
                    default: return false;      /* builtin / unresolved */
                    }
                    Instr in;
                    in.op = OpCode::IncDecCheckedV;
                    in.node_idx = add_ast_node(s);   /* TypeError caret */
                    in.target = id->sym.slot;
                    in.target2 = kind;
                    in.set_a(int_lit(inc->is_inc ? 1 : 0));
                    ops.push_back(in);
                    return true;
                }
            }
            /* A DYN/general-element subscript `c[k]++` / `c[k]--` (`c` dyn, or a
             * general array<str>/dict-via-dyn - anything NOT a proven flat
             * int/float element, which compile_int/float_stmt already handled
             * via StoreElemInt/Float/DictStore). The CHECKED element inc-dec
             * forms the boxed element LValue and enforces int/float (a flat
             * scalar element has no LValue -> NotLValueEx, exactly as the tree-
             * walker's dyn path). Gating on `sub->th != i/f` is what excludes
             * the flat case (its element is proven numeric, handled earlier). */
            if (const Subscript *sub =
                    dynamic_cast<const Subscript *>(inc->lvalue.get())) {
                if (sub->th == TypeHint::i || sub->th == TypeHint::f)
                    return false;
                int bslot, bkind;
                if (!as_container_base(sub->what.get(), bslot, bkind))
                    return false;
                int kslot;
                if (!compile_boxed_expr(sub->index.get(), kslot, ops))
                    return false;
                Instr in;
                in.op = OpCode::IncDecElemCheckedV;
                /* extract_locs: the BASE's span -> the loc side table (the
                 * undefined-global-base caret - the tree-walker marks the
                 * base identifier, not the whole inc-dec) + inline_ctx. */
                in.node_idx = add_ast_node(sub->what.get());
                in.target = bkind;               /* base kind: 0 loc/1 gbl/2 cap */
                in.target2 = bslot;
                in.set_a(slot_op(kslot));
                in.set_b(int_lit(add_incdec_site(sub, inc)));   /* dual carets */
                in.aop = inc->is_inc ? Op::plus : Op::minus;
                ops.push_back(in);
                return true;
            }
            /* A DYN/general-MEMBER `d.f++` / `d.f--` (`d` dyn/general holding a
             * struct or dict - a PROVEN int/float member is handled by
             * compile_int/float_stmt via StoreMemberV/DictStore). The CHECKED
             * member inc-dec forms the member LValue (struct field / dict value)
             * and enforces int/float; a POD field / missing key throws exactly as
             * the tree-walker. Gated on `inc->th != i/f` (excludes the proven
             * numeric case), a slotted base, and a non-optional member. */
            if (const MemberExpr *m =
                    dynamic_cast<const MemberExpr *>(inc->lvalue.get())) {
                if (inc->th == TypeHint::i || inc->th == TypeHint::f
                        || m->optional)
                    return false;
                int bslot, bkind;
                if (!as_container_base(m->what.get(), bslot, bkind))
                    return false;
                Instr in;
                in.op = OpCode::IncDecMemberCheckedV;
                /* extract_locs: the inc-dec span -> the loc side table (the
                 * undefined-global-base caret) + inline_ctx; then node-free. */
                in.node_idx = add_ast_node(m->what.get());
                in.target = bkind;               /* base kind: 0 loc/1 gbl/2 cap */
                in.target2 = bslot;
                in.set_b(int_lit(add_incdec_site(m, inc, m)));  /* key + carets */
                in.aop = inc->is_inc ? Op::plus : Op::minus;
                ops.push_back(in);
                return true;
            }
            return false;
        }

        const Expr14 *e = dynamic_cast<const Expr14 *>(s);
        if (!e)
            return false;
        const bool is_assign = e->op == Op::assign;
        const Op cbase = compound_base_op(e->op);   /* `+=` -> plus, else inv */
        if (!is_assign && cbase == Op::invalid)
            return false;

        /* Multi-assign `a, b, .. = <rvalue>`: an ARRAY literal distributes its
         * elements to the target slots (destructure, no array); a non-array
         * SCALAR spreads to every target. A non-qualifying shape falls back. */
        if (is_assign) {
            if (const IdList *il =
                    dynamic_cast<const IdList *>(e->lvalue.get())) {
                if (try_multi_literal_store(e, il, ops))
                    return true;
                if (try_multi_scalar_spread(e, il, ops))
                    return true;
                if (try_multi_unpack(e, il, ops))
                    return true;
                return false;
            }
        }

        /* A COMPOUND multi-target `a, b OP= rhs`: each target OP= its element
         * (an array rhs) or the scalar (a non-array rhs) — MultiUnpackV with the
         * base op. */
        if (!is_assign && cbase != Op::invalid) {
            if (const IdList *il =
                    dynamic_cast<const IdList *>(e->lvalue.get()))
                return try_multi_unpack(e, il, ops, cbase);
        }

        /* An assignment whose TARGET is not an lvalue always throws (the tree-
         * walker evaluates the rhs first, then handle_single_expr14 rejects the
         * target): a scalar LITERAL target (`0 = 99`, `true = false`, or a
         * const-inlined `K = 6`) -> NotLValueEx(lvalue loc); a BUILTIN-name
         * target (`print = 5`) -> CannotRebindBuiltinEx(lvalue loc). Compile the
         * rhs for its side effects (+ its own throw), THEN throw. Plain assign
         * only; a compound rhs takes the compound store path. */
        if (is_assign) {
            Chunk::ThrowKind tk = Chunk::ThrowKind::not_lvalue;
            const UniqueId *tname = nullptr;
            Loc tstart = e->lvalue->start, tend = e->lvalue->end;
            bool bad = dynamic_cast<const Literal *>(e->lvalue.get()) != nullptr;
            if (const Identifier *bl =
                    dynamic_cast<const Identifier *>(e->lvalue.get())) {
                if (bl->sym.kind == SymKind::builtin) {
                    tk = Chunk::ThrowKind::rebind_builtin;
                    bad = true;
                } else if (bl->sym.kind == SymKind::unresolved) {
                    /* An assignment to an UNDECLARED name in a FUNCTION (a
                     * top-level one is an implicit-global DECL and never
                     * unresolved): the tree-walker evaluates the rhs FIRST
                     * (its side effects run), then doAssign's UndefinedId
                     * check throws - loc-less, stamped with the Expr14 span
                     * by Construct::eval. Mirror exactly. */
                    tk = Chunk::ThrowKind::undefined_var;
                    tname = bl->uid;
                    tstart = e->start;
                    tend = e->end;
                    bad = true;
                }
            }
            if (bad) {
                const size_t om = ops.size();
                const size_t cm = chunk.consts.size();
                const int st = next_temp;
                int rslot;
                if (compile_boxed_expr(e->rvalue.get(), rslot, ops)) {
                    emit_throw(tk, tstart, tend, tname, ops);
                    return true;
                }
                ops.resize(om);
                chunk.consts.resize(cm);
                next_temp = st;
                return false;
            }
        }

        const Identifier *lv =
            dynamic_cast<const Identifier *>(e->lvalue.get());
        if (!lv || (lv->sym.kind != SymKind::local
                    && lv->sym.kind != SymKind::global
                    && lv->sym.kind != SymKind::capture))
            return false;
        /* A typed INT/FLOAT scalar target needs coerce_to_decl_type at the
         * store on a PLAIN assign (numeric WIDENING - float<-int/bool,
         * int<-bool - or a runtime NARROWING throw for a dyn value whose type
         * doesn't fit): CoerceNumV, below. The live producer is the
         * inferencer's coerces_dyn accumulator stamp (`var s = 0; s = s + d`
         * with a dyn `d` - the proven-scalar case went to
         * compile_int/float_stmt already, and an EXPLICIT `int x = <dyn>` is
         * compile-rejected). A COMPOUND does NOT coerce
         * (handle_single_expr14 coerces op==assign only), so it lowers below
         * as a plain CompoundV / StoreGlobalV compound. Every OTHER declared
         * type (bool/str/array/dict/struct/typed-container) coerces to a
         * NO-OP, so a plain boxed store is byte-identical. */
        const bool coerce_num = is_assign
            && (lv->decl_type == DeclType::i
                || lv->decl_type == DeclType::f);
        /* A CONST DECL (a const arr/dict/func kept as a runtime symbol; const
         * SCALARS are inlined, so never here). `pInConstDecl` on the Expr14
         * distinguishes it from a REASSIGN (which has no such flag and must
         * throw, below): materialize the rvalue + DeclConstV, which binds the
         * slot as a CONST LValue so a later rebind still throws. Local or
         * global; a capture const decl falls back. */
        if (lv->is_const && is_assign && (e->fl & pInConstDecl)
            && lv->sym.kind != SymKind::capture) {
            const size_t om = ops.size();
            const size_t cm = chunk.consts.size();
            const int st = next_temp;
            int rslot;
            if (compile_boxed_expr(e->rvalue.get(), rslot, ops)) {
                Instr in;
                in.op = OpCode::DeclConstV;
                in.target = lv->sym.slot;
                in.target2 = lv->sym.kind == SymKind::global ? 1 : 0;
                in.set_a(slot_op(rslot));
                ops.push_back(in);
                return true;
            }
            ops.resize(om);
            chunk.consts.resize(cm);
            next_temp = st;
        }

        /* A reassignment (plain OR compound) of a CONST runtime symbol (a
         * func/array/dict kept in a slot; a const SCALAR is inlined + its
         * reassign hits the bad-lvalue throw above) must throw
         * CannotRebindConstEx. The tree-walker evaluates the RHS first, THEN
         * throws, so compile the rhs (for its side effects + its own throw) and
         * emit a native ThrowRuntimeV with the lvalue's caret - byte-identical.
         * A `const` DECL (pInConstDecl) was already handled above (DeclConstV);
         * this is only a REBIND. */
        if (lv->is_const) {
            const size_t om = ops.size();
            const size_t cm = chunk.consts.size();
            const int st = next_temp;
            int rslot;
            if (compile_boxed_expr(e->rvalue.get(), rslot, ops)) {
                emit_throw(Chunk::ThrowKind::rebind_const,
                           e->lvalue->start, e->lvalue->end, nullptr, ops);
                return true;
            }
            ops.resize(om);
            chunk.consts.resize(cm);
            next_temp = st;
            return false;
        }

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();

        /* A GLOBAL-table lvalue (a top-level var a function reads) or a closure
         * CAPTURE slot: a PLAIN assign lowers to StoreGlobalV/StoreCaptureV
         * (compile the rvalue into a temp, then write the table/capture slot -
         * byte-identical to the tree-walker), a compound `x OP= v` to the same
         * op with the base op (rhs a boxed operand; a complex rhs falls back).
         * No retarget: the producing op writes a FRAME temp; the target is in
         * gfuncs / the capture vector. */
        if (lv->sym.kind == SymKind::global
            || lv->sym.kind == SymKind::capture) {
            const OpCode store_op = lv->sym.kind == SymKind::global
                                        ? OpCode::StoreGlobalV
                                        : OpCode::StoreCaptureV;
            if (is_assign) {
                int rslot;
                if (!compile_boxed_expr(e->rvalue.get(), rslot, ops)) {
                    ops.resize(omark);
                    chunk.consts.resize(cmark);
                    return false;
                }
                /* A typed i/f target: coerce into a FRESH temp first (never
                 * in place - rslot may be a live local, not a temp), then
                 * store the coerced value. */
                if (coerce_num) {
                    const int ct = alloc_temp();
                    Instr co;
                    co.op = OpCode::CoerceNumV;
                    co.node_idx = add_ast_node(s);   /* the Expr14 caret */
                    co.target = ct;
                    co.target2 = lv->decl_type == DeclType::f ? 1 : 0;
                    co.set_a(slot_op(rslot));
                    ops.push_back(co);
                    rslot = ct;
                }
                Instr in;
                in.op = store_op;
                in.target = lv->sym.slot;   /* global-table / capture slot */
                in.set_a(slot_op(rslot));      /* aop invalid == plain assign */
                ops.push_back(in);
                return true;
            }
            /* Compound `x OP= rhs`: the rhs is a boxed operand (immediate or a
             * slot, like the local CompoundV); a complex rhs falls back. */
            Operand rhs_op;
            if (!boxed_operand(e->rvalue.get(), rhs_op, ops)) {
                ops.resize(omark);
                chunk.consts.resize(cmark);
                return false;
            }
            Instr in;
            in.op = store_op;
            in.node_idx = add_ast_node(s);               /* loc: compound may throw (div/undef) */
            in.target = lv->sym.slot;
            in.set_a(rhs_op);
            in.aop = cbase;
            ops.push_back(in);
            return true;
        }

        /* Compound-assign `lv OP= rhs` -> a boxed read-modify-write; the rhs
         * can be an IMMEDIATE (`s += 3` needs no LoadConstV first). */
        if (!is_assign) {
            Operand rhs_op;
            if (!boxed_operand(e->rvalue.get(), rhs_op, ops)) {
                ops.resize(omark);
                chunk.consts.resize(cmark);
                return false;
            }
            Instr in;
            in.op = OpCode::CompoundV;
            in.node_idx = add_ast_node(s);
            in.target = lv->sym.slot;
            in.set_b(rhs_op);
            in.aop = cbase;
            ops.push_back(in);
            return true;
        }

        /* Plain assign: compile the rvalue, then retarget/move it. */
        int rslot;
        if (!compile_boxed_expr(e->rvalue.get(), rslot, ops)) {
            ops.resize(omark);
            chunk.consts.resize(cmark);
            return false;
        }

        /* A typed i/f LOCAL: CoerceNumV IS the store (dst = the lvalue slot,
         * src = the compiled rvalue) - the retarget/MoveV is subsumed. */
        if (coerce_num) {
            Instr co;
            co.op = OpCode::CoerceNumV;
            co.node_idx = add_ast_node(s);           /* the Expr14 caret */
            co.target = lv->sym.slot;
            co.target2 = lv->decl_type == DeclType::f ? 1 : 0;
            co.set_a(slot_op(rslot));
            ops.push_back(co);
            return true;
        }

        if (rslot != lv->sym.slot) {
            /* Retarget the producing op to write the lvalue directly - but ONLY
             * an op THIS statement emitted (ops grew past omark). A LEAF rvalue
             * (a bare local) emits no op, so ops.back() would be the PREVIOUS
             * statement's op - retargeting it would corrupt it (the `var t = s`
             * bug). A leaf falls to MoveV. And ONLY a TEMP result
             * (rslot >= temp_base, like compile_to_run_slot's guard): an
             * assignment-as-expression (`a = (b = [1,2])`, R3) produces the
             * INNER TARGET's local slot with its store as ops.back() -
             * retargeting that would STEAL b's store (b never assigned; the
             * probe caught it as an intptr aliasing divergence). */
            if (ops.size() > omark && rslot >= temp_base
                && ops.back().target == rslot
                && (ops.back().op == OpCode::BinOpV
                    || ops.back().op == OpCode::LoadConstV
                    || ops.back().op == OpCode::LoadLiteralObjV
                    || ops.back().op == OpCode::MakeArrayV
                    || ops.back().op == OpCode::MakeDictV
                    || ops.back().op == OpCode::MakeClosureV
                    || ops.back().op == OpCode::StructCtorV
                    || ops.back().op == OpCode::MakeStructArrayV
                    || ops.back().op == OpCode::SliceV)) {
                /* These read their operand slots BEFORE writing `target`, and
                 * the local lvalue slot can't overlap the temp run - so writing
                 * the result straight into `a` is sound (even `a=[a,1]` /
                 * `a=a[1:]`, whose operand slots are read first). */
                ops.back().target = lv->sym.slot;
            } else {
                Instr in;
                in.op = OpCode::MoveV;
                in.node_idx = add_ast_node(s);
                in.target = lv->sym.slot;
                in.target2 = rslot;
                ops.push_back(in);
            }
        }
        return true;
    }

    /*
     * Compile int expression `e` into `ops`, leaving the result in `out` (a
     * slot or immediate). A leaf costs no op; an arith/neg TypedScalarExpr emits
     * IntBin(s) into temp slots. Returns false for a non-int expression (a
     * comparison, a call, a subscript, ...) so the enclosing loop falls back.
     */
    /*
     * The ARRAY base of a subscript as a slot holding the array: a bare local
     * slot directly, or a nested `a[i]` loaded into a temp via LoadElemValue (a
     * native general-array element read), so a 2-D read `a[i][j]` lowers with
     * both indices native. READ path only - a 2-D WRITE via a temp would COW
     * the temp and never write back, so store codegen keeps as_array_slot.
     */
    bool compile_array_base(const Construct *e, int &out_slot,
                            std::vector<Instr> &ops)
    {
        if (as_array_slot(e, out_slot))
            return true;
        const Subscript *sub = dynamic_cast<const Subscript *>(e);
        if (!sub || !sub->base_array)
            return false;
        int inner;
        Operand idx;
        if (!compile_array_base(sub->what.get(), inner, ops)
            || !compile_int_expr(sub->index.get(), idx, ops))
            return false;
        const int t = alloc_temp();
        Instr in;
        in.op = OpCode::LoadElemValue;
        in.node_idx = add_ast_node(e);
        in.target = t;
        in.target2 = inner;
        in.set_a(idx);
        ops.push_back(in);
        out_slot = t;
        return true;
    }

    /*
     * Native user-function call `dst = f(args...)` -> CallV: evaluate each arg
     * into a contiguous register run [argbase, argbase+n), then one CallV that
     * gathers those values and calls do_func_call (no node->eval of the call).
     * Only a DirectCallExpr the inferencer proved a user function
     * (vm_direct_func); an arg compile_boxed_expr can't lower (a nested call, a
     * complex expression) declines the native call form. Args are evaluated
     * left-to-right, once each.
     */
    /* Evaluate a call's args into a fresh contiguous register run
     * [argbase, argbase+n) via compile_boxed_expr, left-to-right, once each;
     * rolls back ops + temps and returns false if an arg can't lower. Shared by
     * the native user-call and native-builtin lowerings. */
    /* Compile `e` into the run slot `dst`: fuse the producing op's target
     * straight to `dst` (dropping a redundant temp + MoveV - `load t, "x"; move
     * rarg = t` becomes `load rarg, "x"`), or emit a MoveV; then restore the
     * per-element scratch. Returns false if `e` can't lower (the CALLER rolls
     * back ops/next_temp). Shared by emit_args_range (call arg runs) and the
     * array/dict literal builders. */
    bool compile_to_run_slot(const Construct *e, int dst,
                             std::vector<Instr> &ops)
    {
        const int sub = next_temp;
        int out;
        if (!compile_boxed_expr(e, out, ops))
            return false;
        if (out == dst) {
            /* already in place */
        } else if (out >= temp_base && !ops.empty()
                   && ops.back().target == out
                   && op_writes_pure_target(ops.back().op)) {
            ops.back().target = dst;
        } else {
            Instr mv;
            mv.op = OpCode::MoveV;
            mv.target = dst;
            mv.target2 = out;
            ops.push_back(mv);
        }
        next_temp = sub;   /* free this element's scratch for the next */
        return true;
    }

    bool emit_args_range(const std::vector<unique_ptr<Construct>> &elems,
                         int &argbase, std::vector<Instr> &ops, int start = 0)
    {
        const size_t mark = ops.size();
        const int save_top = next_temp;
        const int n = static_cast<int>(elems.size()) - start;

        argbase = next_temp;
        next_temp += n;
        if (next_temp > max_temp)
            max_temp = next_temp;

        for (int i = 0; i < n; i++) {
            if (!compile_to_run_slot(elems[start + i].get(),
                                     argbase + i, ops)) {
                ops.resize(mark);
                next_temp = save_top;
                return false;
            }
        }
        return true;
    }

    bool try_native_call(const DirectCallExpr *dc, int &out_slot,
                         std::vector<Instr> &ops)
    {
        if (!dc->vm_direct_func || dc->direct_func_slot < 0 || !dc->args)
            return false;

        int argbase;
        if (!emit_args_range(dc->args->elems, argbase, ops))
            return false;

        const int dst = alloc_temp();
        Instr cv;
        /* A CachedCallExpr (a pure tree-recursive callee the unroll dedups)
         * routes through the per-frame pure-call cache; else a plain call. */
        cv.op = dynamic_cast<const CachedCallExpr *>(dc)
                    ? OpCode::CachedCallV : OpCode::CallV;
        cv.node_idx = add_ast_node(dc);
        cv.target = dst;
        cv.target2 = dc->direct_func_slot;
        cv.set_a(int_lit(argbase));
        cv.set_b(int_lit(static_cast<int>(dc->args->elems.size())));
        ops.push_back(cv);
        out_slot = dst;
        return true;
    }

    /* An INDIRECT call of a func VALUE (a closure / lambda / func-valued var):
     * a plain CallExpr (not a Direct{Call,BuiltinCall}Expr) whose callee is
     * Func-typed (vm_direct_func). Evaluate the callee expression into a temp
     * (callee-first, matching the tree-walker), then the args into a register
     * run, and emit CallValueV. The runtime value is a FuncObject (the Func
     * static type proves it), so no dispatch is needed. */
    /* Compile an INDIRECT call's arg0: fill the CallSite's LVALUE DESCRIPTOR
     * (the by-ref encoding the dispatch re-derives a func_lv target from)
     * and, for the elem/member/none forms, the VALUE into run slot `dst` at
     * arg0's position (the ordinary SubscriptV/MemberV/boxed compile, so an
     * OOB/missing-key/undefined-base throw fires in argument order, like the
     * tree-walker's raw arg eval). The slot/undef forms leave run[0] to the
     * dispatch: an undefined global/unresolved name surfaces at the CONSUMER
     * (bind/adapter), exactly where the tree-walker's raw UndefinedId does.
     * An elem's INDEX temp stays live for the dispatch re-derive (all later
     * arg scratch is allocated above it). */
    bool compile_indirect_arg0(const Construct *e, int dst,
                               Chunk::CallSite &cs, std::vector<Instr> &ops)
    {
        if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
            switch (id->sym.kind) {
            case SymKind::local:   cs.a0_kind = 0; break;
            case SymKind::global:  cs.a0_kind = 1; break;
            case SymKind::capture: cs.a0_kind = 2; break;
            case SymKind::builtin: cs.a0_kind = 3; break;
            default:
                /* unresolved (incl. `_`): the raw UndefinedId semantics */
                cs.a0_form = Chunk::CallSite::A0::undef;
                cs.a0_name = id->uid;
                return true;
            }
            cs.a0_form = Chunk::CallSite::A0::slot;
            cs.a0_slot = id->sym.slot;
            return true;
        }
        if (const Subscript *sub = dynamic_cast<const Subscript *>(e)) {
            int bslot, bkind;
            if (as_container_base(sub->what.get(), bslot, bkind)) {
                /* base value first, then the index - Subscript::do_eval's
                 * order; the index temp is re-read by the dispatch. */
                int bval, kslot;
                if (compile_boxed_expr(sub->what.get(), bval, ops)
                    && compile_boxed_expr(sub->index.get(), kslot, ops)) {
                    Instr in;
                    in.op = OpCode::SubscriptV;
                    in.node_idx = add_ast_node(sub);   /* subscript caret */
                    in.target = dst;
                    in.target2 = bval;
                    in.set_a(slot_op(kslot));
                    ops.push_back(in);
                    cs.a0_form = Chunk::CallSite::A0::elem;
                    cs.a0_kind = static_cast<unsigned char>(bkind);
                    cs.a0_slot = bslot;
                    cs.a0_operand = kslot;
                    return true;
                }
            }
            /* an unsupported base/index: the plain value path below */
        }
        if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(e)) {
            int bslot, bkind;
            if (as_container_base(m->what.get(), bslot, bkind)) {
                int bval;
                if (compile_boxed_expr(m->what.get(), bval, ops)) {
                    const int mk = add_member_key(m);
                    Instr in;
                    in.op = OpCode::MemberV;
                    in.target = dst;
                    in.target2 = bval;
                    in.set_a(int_lit(mk));
                    ops.push_back(in);
                    cs.a0_form = Chunk::CallSite::A0::member;
                    cs.a0_kind = static_cast<unsigned char>(bkind);
                    cs.a0_slot = bslot;
                    cs.a0_operand = mk;
                    return true;
                }
            }
        }
        cs.a0_form = Chunk::CallSite::A0::none;
        return compile_to_run_slot(e, dst, ops);
    }

    bool try_native_value_call(const CallExpr *call, int &out_slot,
                               std::vector<Instr> &ops)
    {
        if (dynamic_cast<const DirectCallExpr *>(call)
            || dynamic_cast<const DirectBuiltinCallExpr *>(call))
            return false;
        if ((!call->vm_direct_func && !call->vm_dyn_callee) || !call->args)
            return false;

        const size_t mark = ops.size();
        const int save_top = next_temp;

        int callee_slot;
        if (!compile_boxed_expr(call->what.get(), callee_slot, ops)) {
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }

        /* A DYN callee - AST-FREE (F1 step 2): CheckCallableV (a non-callable
         * throws BEFORE the args evaluate, the tree-walker's order), then the
         * args into a register run - arg0 via its lvalue DESCRIPTOR (the
         * by-ref encoding, so a runtime func_lv callee gets the true LValue*:
         * slice write-back / const / NotLValueEx byte-identical) - then
         * CallValueGenericV dispatching on the runtime callee with the
         * pooled CallSite carets. */
        if (!call->vm_direct_func) {
            const size_t n = call->args->elems.size();
            if (n > 0xfff) {              /* the nargs|site<<12 packing bound */
                ops.resize(mark);
                next_temp = save_top;
                return false;
            }
            Instr chk;
            chk.op = OpCode::CheckCallableV;
            chk.node_idx = add_ast_node(call->what.get());  /* callee caret */
            chk.set_a(slot_op(callee_slot));
            ops.push_back(chk);

            /* Build the site LOCALLY and push it after the args compile: a
             * nested indirect call inside an arg registers its own site,
             * which may reallocate the pool (a reference would dangle). */
            Chunk::CallSite cs;
            cs.start = call->args->start;
            cs.end = call->args->end;
            cs.arr_hint = call->args->arr_hint;
            cs.args.reserve(n);
            for (const auto &el : call->args->elems)
                cs.args.push_back(ArgLoc{el->start, el->end});

            const int argbase = next_temp;
            next_temp += static_cast<int>(n);
            if (next_temp > max_temp)
                max_temp = next_temp;
            bool ok = true;
            for (size_t i = 0; ok && i < n; i++) {
                const Construct *ae = call->args->elems[i].get();
                const int dst = argbase + static_cast<int>(i);
                ok = i == 0 ? compile_indirect_arg0(ae, dst, cs, ops)
                            : compile_to_run_slot(ae, dst, ops);
            }
            if (!ok) {
                ops.resize(mark);
                next_temp = save_top;
                return false;
            }
            const int site = static_cast<int>(chunk.call_sites.size());
            chunk.call_sites.push_back(std::move(cs));

            const int dstg = alloc_temp();
            Instr cvg;
            cvg.op = OpCode::CallValueGenericV;
            cvg.node_idx = add_ast_node(call);   /* call-site loc (extract) */
            cvg.target = dstg;
            cvg.target2 = callee_slot;
            cvg.set_a(int_lit(argbase));
            cvg.set_b(int_lit(static_cast<int_type>(n)
                              | (static_cast<int_type>(site) << 12)));
            ops.push_back(cvg);
            out_slot = dstg;
            return true;
        }

        /* A Func-typed callee (always a FuncObject): pre-evaluate the args into a
         * register run and call it natively (CallValueV). */
        int argbase;
        if (!emit_args_range(call->args->elems, argbase, ops)) {
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }

        const int dst = alloc_temp();
        Instr cv;
        cv.op = OpCode::CallValueV;
        cv.node_idx = add_ast_node(call);
        cv.target = dst;
        cv.target2 = callee_slot;
        cv.set_a(int_lit(argbase));
        cv.set_b(int_lit(static_cast<int>(call->args->elems.size())));
        ops.push_back(cv);
        out_slot = dst;
        return true;
    }

    /* Native builtin call -> CallBuiltinV, but only for a builtin with the
     * VALUE ABI (func_v is set - a migrated, read-only builtin); a mutating /
     * AST builtin declines (its call site falls back whole). */
    /* `defined(g)` where `g` is a GLOBAL-table symbol -> DefinedGlobalV (reads
     * gfuncs->defined[slot], the genuine runtime property). The always-bound
     * cases (param/local/capture/builtin) already folded to `true` at resolve
     * time (try_fold_defined), so the only `defined` call reaching codegen has a
     * global (or an unresolved / non-identifier) arg; a non-global one declines
     * here (the call site falls back whole). AST-free (the slot is known;
     * never throws). */
    bool try_native_defined_global(const DirectBuiltinCallExpr *dc,
                                   int &out_slot, std::vector<Instr> &ops)
    {
        const Identifier *callee = dynamic_cast<Identifier *>(dc->what.get());
        if (!callee || callee->get_str() != "defined")
            return false;
        if (!dc->args || dc->args->elems.size() != 1)
            return false;
        const Identifier *arg =
            dynamic_cast<Identifier *>(dc->args->elems[0].get());
        if (!arg || arg->sym.kind != SymKind::global)
            return false;
        const int dst = alloc_temp();
        Instr in;
        in.op = OpCode::DefinedGlobalV;
        in.target = dst;
        in.target2 = arg->sym.slot;
        ops.push_back(in);
        out_slot = dst;
        return true;
    }

    /* `defined(<non-identifier expr>)`: only a bare Identifier can evaluate
     * to the UndefinedId sentinel (Identifier::do_eval is its SOLE producer -
     * eval.cpp), so for any other arg builtin_defined is exactly "evaluate
     * the arg (its effects/throws included), then true". Lowered as: compile
     * the arg into a scratch temp (result discarded) + LoadConstV true. A
     * WRONG-ARITY call (`defined(a,b)`) throws InvalidNumberOfArgsEx BEFORE
     * evaluating any arg -> a bare ThrowRuntimeV(bad_args) with the args
     * caret. The identifier forms never reach here (try_fold_defined /
     * DefinedGlobalV); an unresolved name INSIDE the arg throws via its own
     * lowering, matching the tree-walker's RValue. AST-free. */
    bool try_native_defined_expr(const DirectBuiltinCallExpr *dc,
                                 int &out_slot, std::vector<Instr> &ops)
    {
        const Identifier *callee = dynamic_cast<Identifier *>(dc->what.get());
        if (!callee || callee->get_str() != "defined" || !dc->args)
            return false;
        if (dc->args->elems.size() != 1) {
            emit_throw(Chunk::ThrowKind::bad_args,
                       dc->args->start, dc->args->end, nullptr, ops);
            out_slot = alloc_temp();   /* dead: the op always throws */
            return true;
        }
        if (dynamic_cast<const Identifier *>(dc->args->elems[0].get()))
            return false;              /* id forms: fold / DefinedGlobalV */
        const size_t mark = ops.size();
        const int save_top = next_temp;
        int t;
        if (!compile_boxed_expr(dc->args->elems[0].get(), t, ops)) {
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }
        const int dst = alloc_temp();
        Instr in;
        in.op = OpCode::LoadConstV;
        in.target = dst;
        in.target2 = add_const(EvalValue(true));
        ops.push_back(in);
        out_slot = dst;
        return true;
    }

    /*
     * F1 (roadmap): a float-proven MATH-builtin call whose arg(s) compile as
     * float expressions lowers to MathFnV - raw operand read (an int operand
     * promotes at runtime, like FloatBin's), direct C call, raw float write.
     * Deletes the whole CallBuiltinV marshal for the sqrt/sin/cos/log class.
     * Gated on: an UNSHADOWED builtin (DirectBuiltinCallExpr guarantees it),
     * the exact arity (a wrong-arity call must throw at runtime -> decline to
     * the generic path), and the call node's th == f (so `abs(int)` - an
     * int result - and `float("3")` - a string parse - stay generic). The op
     * NEVER THROWS for a numeric operand, so it is loc- and node-free.
     */
    bool try_math_fn(const DirectBuiltinCallExpr *dc, int &out_slot,
                     std::vector<Instr> &ops)
    {
        struct Ent { const UniqueId *uid; MathFn fn; int nargs; };
        static const std::vector<Ent> tbl = [] {
            std::vector<Ent> v;
            const auto add = [&v](const char *n, MathFn f, int na) {
                v.push_back({UniqueId::get(n), f, na});
            };
            add("sqrt", MathFn::sqrt_, 1);  add("cbrt", MathFn::cbrt_, 1);
            add("sin", MathFn::sin_, 1);    add("cos", MathFn::cos_, 1);
            add("tan", MathFn::tan_, 1);    add("asin", MathFn::asin_, 1);
            add("acos", MathFn::acos_, 1);  add("atan", MathFn::atan_, 1);
            add("exp", MathFn::exp_, 1);    add("exp2", MathFn::exp2_, 1);
            add("log", MathFn::log_, 1);    add("log2", MathFn::log2_, 1);
            add("log10", MathFn::log10_, 1);
            add("ceil", MathFn::ceil_, 1);  add("floor", MathFn::floor_, 1);
            add("trunc", MathFn::trunc_, 1);
            add("float", MathFn::tofloat_, 1);
            add("abs", MathFn::fabs_, 1);   /* th==f gate -> float abs */
            add("pow", MathFn::pow_, 2);
            return v;
        }();

        if (!dc->args || dc->lvalue_arg0)
            return false;

        const Identifier *bid =
            dynamic_cast<const Identifier *>(dc->what.get());
        if (!bid)
            return false;

        for (const Ent &e : tbl) {
            if (e.uid != bid->uid)
                continue;
            if (static_cast<int>(dc->args->elems.size()) != e.nargs)
                return false;      /* wrong arity throws -> generic path */
            Operand x, y;
            const size_t mark = ops.size();
            if (!compile_float_expr(dc->args->elems[0].get(), x, ops)) {
                ops.resize(mark);
                return false;
            }
            if (e.nargs == 2
                && !compile_float_expr(dc->args->elems[1].get(), y, ops)) {
                ops.resize(mark);
                return false;
            }
            const int dst = alloc_temp();
            Instr in;
            in.op = OpCode::MathFnV;
            in.target = dst;
            in.target2 = static_cast<int>(e.fn);
            in.set_a(x);
            if (e.nargs == 2)
                in.set_b(y);
            ops.push_back(in);
            out_slot = dst;
            return true;
        }
        return false;
    }

    bool try_native_builtin(const DirectBuiltinCallExpr *dc, int &out_slot,
                            std::vector<Instr> &ops)
    {
        /* A mutating builtin's union holds func_lv (which aliases func_v's
         * storage, so the null test below can't distinguish it) - its arg0 is
         * an lvalue, handled by the CallBuiltinLV path, not here. */
        if (dc->lvalue_arg0)
            return try_native_mutating_builtin(dc, out_slot, ops);
        if (!dc->builtin.func_v || !dc->args)
            return false;

        int argbase;
        if (!emit_args_range(dc->args->elems, argbase, ops))
            return false;

        const int dst = alloc_temp();
        Instr cv;
        cv.op = OpCode::CallBuiltinV;
        /* AST-free: the builtin + its arg carets live in the builtin_calls pool
         * (index in target2), so no node. */
        cv.target2 = add_builtin_call(dc);
        cv.target = dst;
        cv.set_a(int_lit(argbase));
        cv.set_b(int_lit(static_cast<int>(dc->args->elems.size())));
        ops.push_back(cv);
        out_slot = dst;
        return true;
    }

    /* map(f, c) / filter(f, c): the validate-first sequence - compile arg0 (the
     * function) into t0, CheckFuncV(t0) (throws BEFORE arg1 is evaluated if it
     * is not a function, the tree-walker's tested order), compile arg1 (the
     * container) into t1, then MapFilterV(dst, t0, t1). Both are read-only, so
     * this is an EXPRESSION-position lowering (compile_boxed_expr). */
    bool try_native_map_filter(const DirectBuiltinCallExpr *dc, int &out_slot,
                               std::vector<Instr> &ops)
    {
        if (dc->map_filter_kind == 0 || !dc->args
            || dc->args->elems.size() != 2)
            return false;

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();
        const int save_top = next_temp;

        int t0;
        if (!compile_boxed_expr(dc->args->elems[0].get(), t0, ops)) {
            ops.resize(omark);
            next_temp = save_top;
            chunk.consts.resize(cmark);
            return false;
        }
        Instr chk;
        chk.op = OpCode::CheckFuncV;
        chk.node_idx = add_ast_node(dc->args->elems[0].get());   /* arg0's caret */
        chk.set_a(slot_op(t0));
        ops.push_back(chk);

        int t1;
        if (!compile_boxed_expr(dc->args->elems[1].get(), t1, ops)) {
            ops.resize(omark);
            next_temp = save_top;
            chunk.consts.resize(cmark);
            return false;
        }

        const int dst = alloc_temp();
        Instr in;
        in.op = OpCode::MapFilterV;
        in.node_idx = add_ast_node(dc->args->elems[1].get());    /* arg1's caret (container) */
        in.target = dst;
        in.target2 = dc->map_filter_kind == 2 ? 1 : 0;   /* is_filter */
        in.set_a(slot_op(t0));
        in.set_b(slot_op(t1));
        ops.push_back(in);
        out_slot = dst;
        return true;
    }

    /* Native mutating-builtin call -> CallBuiltinLV, but ONLY when arg0 is a
     * slotted identifier (local/global/capture) - the common `append(a, x)`
     * form. The value args are NOT compiled here: func_lv self-evaluates them,
     * which is what keeps append's construct-in-place fast path (it needs the
     * arg node). A subscript/member/other arg0 (or an unresolved one)
     * declines - the whole statement falls back. */
    bool try_native_mutating_builtin(const DirectBuiltinCallExpr *dc,
                                     int &out_slot, std::vector<Instr> &ops)
    {
        if (!dc->builtin.func_lv || !dc->args)
            return false;

        /* ZERO args (`intptr()`, `append()`): the builtin's own arity check
         * throws InvalidNumberOfArgsEx at the args caret - the tree-walker's
         * exact runtime behavior, lowered as a pooled throw. */
        if (dc->args->elems.empty()) {
            emit_throw(Chunk::ThrowKind::bad_args,
                       dc->args->start, dc->args->end, nullptr, ops);
            out_slot = alloc_temp();   /* dead: the op always throws */
            return true;
        }

        const Construct *a0 = dc->args->elems[0].get();

        /* 2c: a SUBSCRIPT lvalue target `append/push/pop(a[i], ...)`. Compile
         * [index, value args...] into a CONTIGUOUS run (REST-NATIVE: the
         * element's LValue* is formed at runtime by Type::subscript - the tree-
         * walker's exact COW - then func_lv gets the value via rest, no
         * self-eval): `b` = the run base, run[0] = the index, run[1..] = the
         * values (append/push have 1, pop has 0). Needs a slotted-id base + all
         * args to lower; a nested base / non-lowerable arg declines (the
         * statement falls back whole). */
        if (!dc->lvalue_rest_native && !a0->is_id()) {
            if (auto *sub = dynamic_cast<const Subscript *>(a0)) {
                const Construct *base = sub->what.get();
                int bkind = -1;
                if (base->is_id())
                    switch (static_cast<const Identifier *>(base)->sym.kind) {
                    case SymKind::local:   bkind = 0; break;
                    case SymKind::global:  bkind = 1; break;
                    case SymKind::capture: bkind = 2; break;
                    default: break;
                    }
                if (bkind >= 0) {
                    const int nvals =
                        static_cast<int>(dc->args->elems.size()) - 1;
                    const size_t mark = ops.size();
                    const int save_top = next_temp;
                    const int runbase = next_temp;
                    next_temp += 1 + nvals;
                    if (next_temp > max_temp)
                        max_temp = next_temp;
                    bool ok =
                        compile_to_run_slot(sub->index.get(), runbase, ops);
                    for (int i = 0; ok && i < nvals; i++)
                        ok = compile_to_run_slot(
                            dc->args->elems[1 + i].get(),
                            runbase + 1 + i, ops);
                    if (ok) {
                        const int dst = alloc_temp();
                        Instr cv;
                        cv.op = OpCode::CallBuiltinLVElem;
                        /* AST-free: builtin + carets in the pool (a.slot); a.lit
                         * = the base slot kind, b = the run base. */
                        cv.target = dst;
                        cv.target2 =
                            static_cast<const Identifier *>(base)->sym.slot;
                        cv.set_a_dual(add_builtin_call(dc), bkind);
                        cv.set_b(int_lit(runbase));
                        ops.push_back(cv);
                        out_slot = dst;
                        return true;
                    }
                    ops.resize(mark);
                    next_temp = save_top;
                }
            }
        }

        /* A struct-MEMBER lvalue target `append/push/pop/insert/erase(s.f, ...)`
         * -> CallBuiltinLVMember: the base is a slotted-id STRUCT; the value args
         * compile into a rest run (NO index — unlike the subscript case). The
         * handler forms the boxed field LValue* via vm_member_lvalue. A dict
         * member / POD field / non-slotted base falls through (a POD field has no
         * LValue; the runtime check throws the tree-walker's exact error). */
        if (!a0->is_id()) {
            if (auto *m = dynamic_cast<const MemberExpr *>(a0)) {
                const Construct *base = m->what.get();
                int bkind = -1;
                if (m->base_struct && base->is_id())
                    switch (static_cast<const Identifier *>(base)->sym.kind) {
                    case SymKind::local:   bkind = 0; break;
                    case SymKind::global:  bkind = 1; break;
                    case SymKind::capture: bkind = 2; break;
                    default: break;
                    }
                if (bkind >= 0) {
                    const int nvals =
                        static_cast<int>(dc->args->elems.size()) - 1;
                    const size_t mark = ops.size();
                    const int save_top = next_temp;
                    const int runbase = next_temp;
                    next_temp += nvals;
                    if (next_temp > max_temp)
                        max_temp = next_temp;
                    bool ok = true;
                    for (int i = 0; ok && i < nvals; i++)
                        ok = compile_to_run_slot(dc->args->elems[1 + i].get(),
                                                 runbase + i, ops);
                    if (ok) {
                        const int dst = alloc_temp();
                        Instr cv;
                        cv.op = OpCode::CallBuiltinLVMember;
                        cv.target = dst;
                        cv.target2 =
                            static_cast<const Identifier *>(base)->sym.slot;
                        const int bc = add_builtin_call(dc);
                        cv.set_a_dual(bc, bkind);
                        chunk.builtin_calls[bc].member = m->memUid;
                        cv.set_b(int_lit(runbase));
                        ops.push_back(cv);
                        out_slot = dst;
                        return true;
                    }
                    ops.resize(mark);
                    next_temp = save_top;
                }
            }
        }

        int kind = -1;
        int a0slot = -1;
        if (!a0->is_id()) {
            /* arg0 is not a slotted id. A SUBSCRIPT/MEMBER target was handled
             * above; anything ELSE (a literal, a call/arith result — the
             * only lvalues in MyLang are id / subscript / member) is PROVABLY
             * not an lvalue, so a REQUIRES-lvalue builtin always throws
             * NotLValueEx(arg0 loc) after evaluating its args -> a native
             * ThrowRuntimeV; the sort family (value-arg0-accepting) instead
             * materializes arg0 into a temp below. */
            const Identifier *cid =
                dynamic_cast<const Identifier *>(dc->what.get());
            if (cid && builtin_requires_lvalue_arg0(cid->get_str())
                && !dynamic_cast<const Subscript *>(a0)
                && !dynamic_cast<const MemberExpr *>(a0)) {
                const size_t om = ops.size();
                const size_t cm = chunk.consts.size();
                const int st = next_temp;
                bool ok = true;
                for (const auto &a : dc->args->elems) {   /* side effects */
                    int tmp;
                    if (!compile_boxed_expr(a.get(), tmp, ops)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    emit_throw(Chunk::ThrowKind::not_lvalue,
                               a0->start, a0->end, nullptr, ops);
                    out_slot = alloc_temp();   /* dead: always throws */
                    return true;
                }
                ops.resize(om);
                chunk.consts.resize(cm);
                next_temp = st;
                return false;
            }
            /* sort/rev_sort/reverse ACCEPT a VALUE arg0 (a fresh clone / a
             * call result: the tree-walker sorts that unaliased value in
             * place and returns it). Materialize it into a TEMP slot - the
             * temp IS the "lvalue" (kind 0), invisible to the script, so
             * func_lv sorts the same fresh value the tree-walker does. */
            {
                int vslot;
                if (!compile_boxed_expr(a0, vslot, ops))
                    return false;
                kind = 0;
                a0slot = vslot;
            }
        } else {
            switch (static_cast<const Identifier *>(a0)->sym.kind) {
            case SymKind::local:   kind = 0; break;
            case SymKind::global:  kind = 1; break;
            case SymKind::capture: kind = 2; break;
            default: return false;   /* builtin / unresolved -> fall back */
            }
            a0slot = static_cast<const Identifier *>(a0)->sym.slot;
        }

        /* EMPLACE (Phase 2b): append(struct_arr, Ctor(args)) with a POD struct
         * ctor -> EmplaceStruct: compile the ctor's field arg VALUES into a
         * register run (`b`) and coerce them into the array's bytes, no
         * temporary StructObject. Recognized by a SELF-EVAL lvalue builtin
         * (append/push - not rest-native) with 2 args, arg1 a POD struct
         * construction (vm_struct_ctor_def): pop/intptr take 1 arg and
         * insert/erase are rest-native, so this shape is append/push only. */
        if (!dc->lvalue_rest_native && dc->args->elems.size() == 2) {
            auto *ctor =
                dynamic_cast<const CallExpr *>(dc->args->elems[1].get());
            if (ctor && ctor->vm_struct_ctor_def && ctor->args) {
                int fieldbase;
                if (emit_args_range(ctor->args->elems, fieldbase, ops)) {
                    /* Pool the ctor def + carets (AST-free op): the container
                     * arg's caret, the per-field coerce carets, the callee
                     * name; `a` packs the base kind with the pool index. The
                     * whole-args caret rides the loc side table
                     * (extract_locs). */
                    Chunk::EmplaceSite site;
                    site.def = ctor->vm_struct_ctor_def;
                    site.bname = nullptr;
                    if (const Identifier *cid =
                            dynamic_cast<const Identifier *>(dc->what.get()))
                        site.bname = cid->uid;
                    site.a0_start = a0->start;
                    site.a0_end = a0->end;
                    site.field_locs.reserve(ctor->args->elems.size());
                    for (const auto &fe : ctor->args->elems)
                        site.field_locs.push_back(
                            ArgLoc{fe->start, fe->end});
                    const int sidx =
                        static_cast<int>(chunk.emplace_sites.size());
                    chunk.emplace_sites.push_back(std::move(site));

                    const int dst = alloc_temp();
                    Instr cv;
                    cv.op = OpCode::EmplaceStruct;
                    cv.node_idx = add_ast_node(dc);
                    cv.target = dst;
                    cv.target2 =
                        static_cast<const Identifier *>(a0)->sym.slot;
                    cv.set_a(int_lit(kind | (sidx << 2)));
                    cv.set_b(int_lit(fieldbase));
                    ops.push_back(cv);
                    out_slot = dst;
                    return true;
                }
                /* a field arg didn't lower -> fall through to CallBuiltinLV
                 * (append self-evals the ctor node = construct-in-place). */
            }
        }

        /* PER-OP rest-native decision. Compile the value args (1..n) into a
         * register run so func_lv has ZERO node->eval; `b` carries its base and
         * MARKS this op rest-native (the VM reads `in.b_is_lit()`, not a
         * per-builtin flag). Two ways to become rest-native:
         *   - lvalue_rest_native (insert/erase): ALWAYS - a rest arg that can't
         *     lower fails the whole native call (fall back to the tree-walker).
         *   - lvalue_rest_capable (append/push): the PLAIN case reached here (the
         *     ctor case took EmplaceStruct above, the subscript-target case took
         *     CallBuiltinLVElem above) - pre-evaluate the value PER-OP; if it
         *     can't lower, DON'T fail - leave `b` unset and let func_lv self-eval
         *     arg1 off the node (a self-eval CallBuiltinLV, as before).
         * pop/intptr (no value args) and sort/reverse (cmp self-eval'd) are
         * neither, so they stay self-eval. */
        int restbase = 0;
        bool rest_op = false;
        if (dc->lvalue_rest_native) {
            if (!emit_args_range(dc->args->elems, restbase, ops, 1))
                return false;   /* a rest arg didn't lower -> fall back */
            rest_op = true;
        } else if (dc->lvalue_rest_capable) {
            /* Pre-evaluate the value(s) PER-OP. A rest-capable builtin
             * (append/push/sort/rev_sort) must NEVER become a self-eval
             * CallBuiltinLV - its func_lv is rest-native-only (no node). So two
             * cases fall back to the tree-walker (whole-statement)
             * instead: (a) arg1
             * is a struct ctor whose EmplaceStruct fell through - append_tw does
             * the construct-in-place there; (b) a value/cmp that doesn't lower to
             * a register run. Otherwise it's rest-native. */
            /* (A qualifying POD-ctor arg1 took EmplaceStruct above; a
             * NON-qualifying one - a dyn field arg - compiles as a VALUE in
             * the rest run via the checked ctor op: observably identical to
             * append_tw's construct-in-place, incl. the throw-before-append
             * ordering.) */
            if (!emit_args_range(dc->args->elems, restbase, ops, 1))
                return false;   /* didn't lower -> tree-walker (no self-eval) */
            rest_op = true;
        }

        const int dst = alloc_temp();
        Instr cv;
        cv.op = OpCode::CallBuiltinLV;
        /* D1: the plain `append(a, x)` / `push(a, x)` shape (rest-native,
         * exactly one value arg) gets the dedicated AppendV - same operand
         * layout, the marshaling deleted; every rare/error shape inside it
         * falls back to the pooled full-builtin path. A user `func append`
         * shadow never gets here (only an UNSHADOWED builtin devirtualizes
         * to DirectBuiltinCallExpr). */
        if (rest_op && dc->args->elems.size() == 2) {
            if (const Identifier *bid =
                    dynamic_cast<const Identifier *>(dc->what.get())) {
                static const UniqueId *const uid_append =
                    UniqueId::get("append");
                static const UniqueId *const uid_push = UniqueId::get("push");
                if (bid->uid == uid_append || bid->uid == uid_push)
                    cv.op = OpCode::AppendV;
            }
        }
        /* AST-free: the Builtin + arg carets live in the builtin_calls pool
         * (index in a.slot; a.lit carries the arg0 slot kind). */
        cv.target = dst;
        cv.target2 = a0slot;
        cv.set_a_dual(add_builtin_call(dc), kind);
        if (rest_op)
            cv.set_b(int_lit(restbase));
        ops.push_back(cv);
        out_slot = dst;
        return true;
    }

    /*
     * Native `return <expr>;` -> ReturnV (no node->eval of the return): the
     * value expr compiles via compile_boxed_expr - so a `return f(x)` becomes a
     * CallV, `return a+b` a BinOpV, etc. A bare `return;` loads `none`. An expr
     * compile_boxed_expr can't lower (e.g. a ternary from a recursion unroll)
     * declines (the statement aborts if nothing lowers it).
     */
    /*
     * P8 Inc 2c: at a flow op (return / break / continue) that crosses the
     * innermost `crossed` trys, INLINE each crossed try's handler-pop + finally
     * here, innermost first — ANY nesting depth (each level chains). The
     * interleaved pops give the correct throw-in-finally-during-unwind
     * semantics: while T_i's finally
     * runs, T_i's handler is popped but the OUTER trys' handlers stay live, so
     * a throw in T_i's finally dispatches to the enclosing handler (like the
     * tree-walker). A crossed try's finally is compiled with the exited trys
     * removed from `trys` (truncated to [0, T_i)), so a flow op in a finally
     * routes through the still-active outer trys, not through T_i again.
     * Returns false (caller falls back) only if a finally fails to compile.
     */
    bool inline_crossed_finallys(size_t crossed)
    {
        const std::vector<TryFrame> saved = trys;   /* shallow (to_fin ptrs) */
        const size_t total = saved.size();
        for (size_t j = 0; j < crossed; j++) {
            const size_t ti = total - 1 - j;        /* innermost first */
            const TryFrame &tf = saved[ti];
            /* pop this try's handler, unless the flow op is in the INNERMOST
             * try's catch body (the dispatch already popped it there). */
            if (!(j == 0 && tf.in_catch)) {
                Instr ph;
                ph.op = OpCode::PopHandler;
                chunk.code.push_back(ph);
            }
            if (tf.finally_body) {
                trys.resize(ti);                    /* active trys = [0, T_i) */
                const bool ok =
                    compile_scalar_body(body_stmts(tf.finally_body), false);
                if (!ok) {
                    trys = saved;
                    return false;
                }
            }
        }
        trys = saved;
        return true;
    }

    /*
     * P8 Inc 2c step 3: emit a break/continue. It crosses the trys pushed after
     * its loop (trys[loop.try_depth..]); each is exited by inlining its
     * handler-pop + finally here, then a Jump to the loop's break/continue
     * target. Returns false (caller fails the body) only if a crossed finally
     * fails to compile. Precondition: loops non-empty (checked by the caller).
     */
    bool emit_break_cont(bool is_break)
    {
        LoopFrame &lp = loops.back();
        const size_t crossed = trys.size() - lp.try_depth;
        if (crossed > 0 && !inline_crossed_finallys(crossed))
            return false;
        /* handlers popped + finallys run; jump to the loop target. */
        const size_t j = emit(OpCode::Jump);
        if (is_break)
            lp.breaks.push_back(j);
        else
            lp.conts.push_back(j);
        return true;
    }

    bool try_native_return(const ReturnStmt *ret, std::vector<Instr> &ops)
    {
        const size_t mark = ops.size();
        const int save_top = next_temp;

        int vslot;
        if (ret->elem) {
            if (!compile_boxed_expr(ret->elem.get(), vslot, ops)) {
                ops.resize(mark);
                next_temp = save_top;
                return false;
            }
        } else {
            vslot = alloc_temp();
            Instr ld;
            ld.op = OpCode::LoadConstV;
            ld.target = vslot;
            ld.target2 = add_const(EvalValue());   /* none */
            ops.push_back(ld);
        }

        /* Inside an INLINED-CALL boundary: a `return v` doesn't ReturnV the
         * chunk - it yields v as this expression's value (MoveV into the
         * boundary's rslot) then JUMPs to the body's end. A return that
         * crosses trys INSIDE the boundary (trys.size() > try_base) inlines
         * each crossed try's handler-pop + finally first - bounded at the
         * BOUNDARY, not the function - exactly like a real return's crossed
         * finallys (the value is copied to a protected temp beforehand, since
         * a finally may overwrite it), mirroring InlinedCallExpr::do_eval's
         * FlowState-swap + the try scope guards. */
        if (!inline_returns.empty()) {
            InlineRet &ir = inline_returns.back();
            const size_t crossed = trys.size() - ir.try_base;
            if (crossed > 0) {
                const int vtmp = alloc_temp();
                Instr cp;
                cp.op = OpCode::MoveV;
                cp.target = vtmp;
                cp.target2 = vslot;
                ops.push_back(cp);

                const int saved_base = temp_base;
                temp_base = vtmp + 1;
                if (temp_base > max_temp)
                    max_temp = temp_base;
                next_temp = temp_base;
                const bool ok = inline_crossed_finallys(crossed);
                temp_base = saved_base;
                if (!ok) {
                    ops.resize(mark);
                    next_temp = save_top;
                    return false;
                }
                vslot = vtmp;            /* the boundary reads the copy */
            }
            Instr mv;
            mv.op = OpCode::MoveV;
            mv.target = ir.rslot;
            mv.target2 = vslot;
            ops.push_back(mv);
            ir.jumps.push_back(ops.size());
            Instr jp;
            jp.op = OpCode::Jump;
            ops.push_back(jp);           /* .target backpatched to body end */
            next_temp = save_top;
            return true;
        }

        /* P8 Inc 2c: a return crosses ALL enclosing trys (up to the function).
         * If none has a finally, ReturnV stops the chunk - destroying the whole
         * handler stack, so no explicit handler-pop is needed. Otherwise inline
         * each crossed try's handler-pop + finally, from innermost up to the
         * OUTERMOST finally-try, then ReturnV; ReturnV cleans up any no-finally
         * trys outside that. */
        size_t outermost_fin = trys.size();
        for (size_t i = 0; i < trys.size(); i++)
            if (trys[i].has_finally) {
                outermost_fin = i;
                break;
            }

        if (outermost_fin < trys.size()) {
            const size_t crossed = trys.size() - outermost_fin;
            /* COPY the value into a fresh temp BEFORE the finallys run: `return
             * s; finally { s = 999; }` must return s's OLD value, but vslot may
             * alias the local `s` that the finally overwrites (and even a temp
             * vslot could be reused by a finally's temps). MoveV captures the
             * value now (a scalar copy, or the same COW handle for a container,
             * matching what the tree-walker's flow->value captures). The value
             * is computed before the finallys (correct: `return f()` evaluates
             * f() first), and read by ReturnV after. Then protect the copy by
             * raising the temp base above it. */
            const int vtmp = alloc_temp();
            Instr mv;
            mv.op = OpCode::MoveV;
            mv.target = vtmp;
            mv.target2 = vslot;
            ops.push_back(mv);

            const int saved_base = temp_base;
            temp_base = vtmp + 1;
            if (temp_base > max_temp)
                max_temp = temp_base;
            next_temp = temp_base;
            const bool ok = inline_crossed_finallys(crossed);
            temp_base = saved_base;
            next_temp = save_top;
            if (!ok) {
                ops.resize(mark);
                return false;
            }
            vslot = vtmp;                        /* ReturnV reads the copy */
        }

        Instr rv;
        rv.op = OpCode::ReturnV;
        rv.node_idx = add_ast_node(ret);
        rv.set_a(slot_op(vslot));
        ops.push_back(rv);
        return true;
    }

    /* P8 Inc 1: `throw <expr>` -> compile the value into a temp + a native Throw
     * op (a same-frame catch is a native jump, no C++ throw). Self-cleaning. */
    bool try_native_throw(const ThrowStmt *th, std::vector<Instr> &ops)
    {
        const size_t mark = ops.size();
        const int save_top = next_temp;
        int vslot;
        if (!compile_boxed_expr(th->elem.get(), vslot, ops)) {
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }
        Instr in;
        in.op = OpCode::Throw;
        in.node_idx = add_ast_node(th);                 /* throw-site loc (extract_locs) */
        in.set_a(slot_op(vslot));
        ops.push_back(in);
        return true;
    }

    /* P8 Inc 2a: `rethrow` (only in a catch body) -> a native Rethrow op
     * (re-raise vm_exc with the rethrow-site loc). Always succeeds. */
    void emit_rethrow(const RethrowStmt *rt, std::vector<Instr> &ops)
    {
        Instr in;
        in.op = OpCode::Rethrow;
        in.node_idx = add_ast_node(rt);                 /* rethrow-site loc (extract_locs) */
        ops.push_back(in);
    }

    /* P3: a typed DICT scalar read `d[k]` / `d.k` (value proven int/float) ->
     * DictLoadInt/Float into a fresh temp `tt`. The base must be a local-slot
     * dict; a subscript compiles its key to a boxed temp (in `a`), a member
     * carries its key on the node (memId). Returns false for a non-dict-read /
     * wrong-th / non-local base, so the caller falls through to the array/boxed
     * path. `th` guards the value type (i for DictLoadInt, f for Float). */
    bool try_dict_scalar_load(const Construct *e, int &tt,
                              std::vector<Instr> &ops, OpCode op, TypeHint th)
    {
        if (e->th != th)
            return false;
        if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(e)) {
            int dslot;
            if (!m->base_dict || !as_array_slot(m->what.get(), dslot))
                return false;
            tt = alloc_temp();
            Instr in;
            in.op = op;
            in.node_idx = add_ast_node(m);                 /* extract_locs records + nulls */
            in.target = tt;
            in.target2 = dslot;
            /* the member NAME (a dict key) goes into the CONST POOL; `a` as an
             * immediate = its index, so the handler needs no AST (a.is_lit
             * distinguishes a member key from a subscript's key temp). */
            in.set_a(int_lit(add_const(m->memId)));
            ops.push_back(in);
            return true;
        }
        if (const Subscript *sub = dynamic_cast<const Subscript *>(e)) {
            int dslot, kslot;
            if (!sub->base_dict || !as_array_slot(sub->what.get(), dslot)
                || !compile_boxed_expr(sub->index.get(), kslot, ops))
                return false;
            tt = alloc_temp();
            Instr in;
            in.op = op;
            in.node_idx = add_ast_node(sub);
            in.target = tt;
            in.target2 = dslot;
            in.set_a(slot_op(kslot));
            ops.push_back(in);
            return true;
        }
        return false;
    }

    /* Struct-foreach direct read: if `m` reads the ACTIVE loop var's scalar
     * field (`p.x`), emit LoadStructField{Int,Float} - a direct byte read of
     * sfe_arr_slot[sfe_ctr_slot].field - into a temp (out). The body analysis
     * (struct_fe_body_ok) already proved the base is the loop var + the field a
     * scalar, so this can't misfire. */
    bool try_sfe_field(const MemberExpr *m, Operand &out,
                       std::vector<Instr> &ops, OpCode fieldop)
    {
        if (sfe_loop_slot < 0)
            return false;
        const Identifier *bid =
            dynamic_cast<const Identifier *>(m->what.get());
        if (!bid || bid->sym.kind != SymKind::local
            || bid->sym.slot != sfe_loop_slot)
            return false;
        const FieldDef *f = sfe_def->field_of(m->memUid);
        if (!f || f->offset < 0)
            return false;
        const int fidx = static_cast<int>(f - sfe_def->fields.data());
        const int tt = alloc_temp();
        Instr in;
        in.op = fieldop;
        in.target = tt;
        in.target2 = sfe_arr_slot;
        in.set_a(slot_op(sfe_ctr_slot));
        in.set_b(int_lit(fidx));
        ops.push_back(in);
        out = slot_op(tt);
        return true;
    }

    /*
     * H1: the TYPED standalone struct-member read `p.x` (th==i/f) - a proven
     * non-opt STRUCT base (`MemberExpr::base_struct`) in a resolved LOCAL
     * slot lowers to LoadMemberInt/Float: the POD fast path reads the scalar
     * straight from the instance's bytes, so the consuming arith stays
     * IntBin/FloatBin (bench 64's body was 5 member.v + 6 boxed ops per
     * iteration). A non-local base (global/capture - rare) or an optional
     * `?.` stays on the boxed MemberV path.
     */
    bool try_member_scalar(const MemberExpr *m, Operand &out,
                           std::vector<Instr> &ops, OpCode op)
    {
        if (!m->base_struct || m->optional)
            return false;
        const Identifier *bid =
            dynamic_cast<const Identifier *>(m->what.get());
        if (!bid || bid->sym.kind != SymKind::local)
            return false;
        const int tt = alloc_temp();
        Instr in;
        in.op = op;
        in.target = tt;
        in.target2 = bid->sym.slot;
        in.set_a(int_lit(add_member_key(m)));
        ops.push_back(in);
        out = slot_op(tt);
        return true;
    }

    /*
     * A TYPED ternary VALUE `cond ? a : b` with th==i/f (F-class follow-up
     * from plans/vm-peephole.md): a typed-compare condition emits ONE native
     * JumpUnless{Int,Float}Cmp to the else arm (any other condition boxes to
     * a JumpUnlessTrueV - same shape as the boxed ternary's, arms still
     * typed); each arm compiles through the TYPED compilers and lands in a
     * common dst (MoveV/LoadImm - the E1 peephole then retargets the arm
     * producers into dst and deletes the moves). This is what turns the
     * recursion-unroll's guard ternaries (fib's whole body) from boxed
     * bin.v/cmp.v/jmp.ifnot.v chains into IntBin/JumpUnlessIntCmp.
     */
    bool try_typed_ternary(const Construct *e, Operand &out,
                           std::vector<Instr> &ops, bool flt)
    {
        const TernaryExpr *t = dynamic_cast<const TernaryExpr *>(e);
        if (!t)
            return false;

        const size_t mark = ops.size();
        const int save_top = next_temp;
        const int dst = alloc_temp();       /* reserved BELOW the scratch */
        const int scratch = next_temp;

        /* The branch-to-else: a typed comparison natively, else boxed
         * truthiness (JumpUnlessTrueV, the boxed ternary's own shape). */
        size_t cj = ops.size();
        bool emitted = false;
        if (const TypedScalarExpr *c =
                dynamic_cast<const TypedScalarExpr *>(t->condExpr.get())) {
            if (c->cat == TypedScalarExpr::Cat::cmp) {
                Operand a, b;
                Op cmp;
                if (compile_int_cond(t->condExpr.get(), ops, a, cmp, b)) {
                    Instr in;
                    in.op = OpCode::JumpUnlessIntCmp;
                    in.aop = cmp; in.set_a(a); in.set_b(b);
                    cj = ops.size();
                    ops.push_back(in);
                    emitted = true;
                } else {
                    ops.resize(mark);
                    next_temp = scratch;
                    if (compile_float_cond(t->condExpr.get(), ops,
                                           a, cmp, b)) {
                        Instr in;
                        in.op = OpCode::JumpUnlessFloatCmp;
                        in.aop = cmp; in.set_a(a); in.set_b(b);
                        cj = ops.size();
                        ops.push_back(in);
                        emitted = true;
                    } else {
                        ops.resize(mark);
                        next_temp = scratch;
                    }
                }
            }
        }
        if (!emitted) {
            int cslot;
            if (!compile_boxed_expr(t->condExpr.get(), cslot, ops)) {
                ops.resize(mark);
                next_temp = save_top;
                return false;
            }
            Instr in;
            in.op = OpCode::JumpUnlessTrueV;
            in.target2 = cslot;
            cj = ops.size();
            ops.push_back(in);
        }
        next_temp = scratch;

        /* dst = <operand>: a lit -> LoadImm, a slot -> MoveV (E1 retargets). */
        const auto store_arm = [&](const Operand &o) {
            Instr in;
            if (o.is_lit) {
                in.op = flt ? OpCode::LoadImmFloat : OpCode::LoadImmInt;
                in.target = dst;
                in.set_a(o);
            } else {
                in.op = OpCode::MoveV;
                in.target = dst;
                in.target2 = static_cast<int>(o.slot);
            }
            ops.push_back(in);
        };

        Operand a1;
        if (!(flt ? compile_float_expr(t->thenExpr.get(), a1, ops)
                  : compile_int_expr(t->thenExpr.get(), a1, ops))) {
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }
        store_arm(a1);
        const size_t jmp_i = ops.size();
        {
            Instr j;
            j.op = OpCode::Jump;
            ops.push_back(j);
        }
        next_temp = scratch;

        ops[cj].target = static_cast<int>(ops.size());     /* else arm */

        Operand a2;
        if (!(flt ? compile_float_expr(t->elseExpr.get(), a2, ops)
                  : compile_int_expr(t->elseExpr.get(), a2, ops))) {
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }
        store_arm(a2);
        next_temp = scratch;
        ops[jmp_i].target = static_cast<int>(ops.size());  /* merge */
        out = slot_op(dst);
        return true;
    }

    bool compile_int_expr(const Construct *e, Operand &out,
                          std::vector<Instr> &ops)
    {
        if (as_int_operand(e, out))
            return true;

        if (e->th == TypeHint::i && try_typed_ternary(e, out, ops, false))
            return true;

        if (e->th == TypeHint::i)
            if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(e)) {
                if (try_sfe_field(m, out, ops, OpCode::LoadStructFieldInt))
                    return true;
                if (try_member_scalar(m, out, ops, OpCode::LoadMemberInt))
                    return true;
            }

        int dtt;
        if (try_dict_scalar_load(e, dtt, ops, OpCode::DictLoadInt,
                                 TypeHint::i)) {
            out = slot_op(dtt);
            return true;
        }

        /* Array element read `a[i]` -> LoadElemInt into a temp (arrays only; a
         * dict subscript stays fallback - see Subscript::base_array). A nested
         * base `a[i][k]` loads `a[i]` via compile_array_base first. */
        if (const Subscript *sub = dynamic_cast<const Subscript *>(e)) {
            if (e->th != TypeHint::i || !sub->base_array)
                return false;
            int aslot;
            Operand idx;
            if (!compile_array_base(sub->what.get(), aslot, ops)
                || !compile_int_expr(sub->index.get(), idx, ops))
                return false;
            const int tt = alloc_temp();
            Instr in;
            in.op = OpCode::LoadElemInt;
            in.node_idx = add_ast_node(e);
            in.target = tt;
            in.target2 = aslot;
            in.set_a(idx);
            ops.push_back(in);
            out = slot_op(tt);
            return true;
        }

        /* A scalar-result BUILTIN call -> CallBuiltinV into a temp; the
         * result is then a native int operand. Builtins ONLY: a builtin is
         * cheap, so the loop-nativization it enables outweighs the boxing. A
         * user call whose body is tree-walked (a closure) would only add the
         * boxing on top of the call (see 11_closure_counter); a cheap/inlinable
         * user call (`func f(x)=>x+1`) is already inlined away. */
        if (e->th == TypeHint::i)
            if (const DirectBuiltinCallExpr *bc =
                    dynamic_cast<const DirectBuiltinCallExpr *>(e)) {
                int t;
                if (try_native_defined_global(bc, t, ops)   /* defined(global) */
                        || try_native_defined_expr(bc, t, ops)
                        || try_native_builtin(bc, t, ops)) { /* value ABI */
                    out = slot_op(t);
                    return true;
                }
                /* An unliftable builtin call (a nested lvalue arg0, an arg
                 * that can't lower): decline - the statement-level dispatch
                 * falls back whole (EvalToSlot is gone; every live builtin
                 * is value/lvalue-native, so this residue is exotic). */
                return false;
            }

        /* An int-returning native user-function call -> CallV, its result an
         * int operand (so `s += f(i)` stays the int fast path). */
        if (e->th == TypeHint::i)
            if (const DirectCallExpr *dc =
                    dynamic_cast<const DirectCallExpr *>(e)) {
                int ct;
                if (try_native_call(dc, ct, ops)) {
                    out = slot_op(ct);
                    return true;
                }
            }

        const TypedScalarExpr *t = dynamic_cast<const TypedScalarExpr *>(e);
        if (!t || t->kind != TypeHint::i)
            return false;

        if (t->cat == TypedScalarExpr::Cat::arith) {
            Operand acc;
            if (!compile_int_expr(t->elems[0].second.get(), acc, ops))
                return false;
            for (size_t i = 1; i < t->elems.size(); i++) {
                Operand rhs;
                if (!compile_int_expr(t->elems[i].second.get(), rhs, ops))
                    return false;
                const int tt = alloc_temp();
                Instr in;
                in.op = OpCode::IntBin;
                in.node_idx = add_ast_node(e);
                in.target = tt;
                in.set_a(acc);
                in.set_b(rhs);
                in.aop = t->elems[i].first;
                ops.push_back(in);
                acc = slot_op(tt);
            }
            out = acc;
            return true;
        }

        if (t->cat == TypedScalarExpr::Cat::neg) {
            Operand op;
            if (!compile_int_expr(t->elems[0].second.get(), op, ops))
                return false;
            const int tt = alloc_temp();
            Instr in;                    /* tt = 0 - op */
            in.op = OpCode::IntBin;
            in.node_idx = add_ast_node(e);
            in.target = tt;
            in.set_a(int_lit(0));
            in.set_b(op);
            in.aop = Op::minus;
            ops.push_back(in);
            out = slot_op(tt);
            return true;
        }

        return false;   /* cmp / logical / lnot -> bool, not an int expr */
    }

    /*
     * A GENERAL nested lvalue-chain store `base.s1.s2... = v` / `OP= v` mixing
     * MEMBER and SUBSCRIPT steps (`a[i].f=v`, `q.p.x=v`, `d.a[0].f=v`,
     * `s.f[i]=v`) -> StoreLValueChainV. A single `s.f`/`a[i]` and a pure-
     * subscript chain are handled by the specific paths (StoreMemberV / DictStore
     * / StoreElem*), so this fires ONLY on a ≥2-step chain with ≥1 MEMBER step
     * and a slotted base. Compiles the rhs, then each subscript step's key into a
     * temp (INSIDE-OUT, matching the tree-walker's chained lvalue eval), builds
     * the chain_steps pool entry, and emits the op. Byte-identical: the runtime
     * walk uses the same Type::subscript / member-lvalue / vm_member_store /
     * vm_subscript_store the tree-walker's chained do_eval does.
     */
    bool try_native_chain_store(const Expr14 *e, std::vector<Instr> &ops)
    {
        switch (e->op) {
        case Op::assign: case Op::addeq: case Op::subeq:
        case Op::muleq:  case Op::diveq: case Op::modeq: break;
        default: return false;
        }
        /* Decompose the lvalue OUTSIDE-IN into member/subscript steps down to a
         * base. `chain` is outermost-first; reverse for inside-out. */
        std::vector<const Construct *> chain;   /* outermost-first */
        const Construct *cur = e->lvalue.get();
        for (;;) {
            if (dynamic_cast<const MemberExpr *>(cur)) {
                chain.push_back(cur);
                cur = static_cast<const MemberExpr *>(cur)->what.get();
            } else if (dynamic_cast<const Subscript *>(cur)) {
                chain.push_back(cur);
                cur = static_cast<const Subscript *>(cur)->what.get();
            } else {
                break;
            }
        }
        const int nsteps = static_cast<int>(chain.size());
        int nmember = 0;
        for (const Construct *c : chain)
            if (dynamic_cast<const MemberExpr *>(c))
                nmember++;
        /* A single-SUBSCRIPT step is StoreElemValue's job (the universal
         * store, incl. a dyn base); a single MEMBER step reaches here only
         * when the PROVEN paths (StoreMemberV / DictStore, base_struct /
         * base_dict) declined - i.e. an UNPROVEN (dyn / template) base:
         * `p.x = 5` in a dyn-param body. The chain final-step dispatch
         * mirrors the tree-walker exactly (POD byte store / boxed field /
         * dict vivify / "Expected struct object"). */
        if (nsteps < 2 && nmember == 0)
            return false;
        if (nmember == 0)
            return false;            /* pure-subscript chain -> StoreElem* */
        if (nsteps == 1) {
            /* keep the PROVEN single-member stores on their tuned ops
             * (StoreMemberV / DictStore, tried after this call): only an
             * UNPROVEN base takes the 1-step chain. */
            auto *m1 = static_cast<const MemberExpr *>(chain[0]);
            if (m1->base_struct || m1->base_dict)
                return false;
        }
        /* An OPTIONAL member (`a?.b`) short-circuits a none base - not handled
         * by the chain walk; leave it to the tree-walker. */
        for (const Construct *c : chain)
            if (auto *m = dynamic_cast<const MemberExpr *>(c))
                if (m->optional)
                    return false;
        int bslot, bkind;
        if (!as_container_base(cur, bslot, bkind))
            return false;

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();
        const int st = next_temp;

        int vslot;
        if (!compile_boxed_expr(e->rvalue.get(), vslot, ops)) {
            ops.resize(omark);
            chunk.consts.resize(cmark);
            next_temp = st;
            return false;
        }

        /* Build the steps INSIDE-OUT (base -> final): chain[nsteps-1-i]. A
         * subscript step compiles its key into a temp NOW (matching the
         * tree-walker's key eval order: innermost first); a member step records
         * its member-key pool index. */
        std::vector<Chunk::ChainStep> steps;
        steps.reserve(static_cast<size_t>(nsteps));
        bool ok = true;
        for (int i = 0; i < nsteps && ok; i++) {
            const Construct *c = chain[nsteps - 1 - i];
            if (auto *m = dynamic_cast<const MemberExpr *>(c)) {
                steps.push_back({true, add_member_key(m), c->start, c->end});
            } else {
                const Subscript *sub = static_cast<const Subscript *>(c);
                int kslot;
                if (!compile_boxed_expr(sub->index.get(), kslot, ops)) {
                    ok = false;
                    break;
                }
                steps.push_back({false, kslot, c->start, c->end});
            }
        }
        if (!ok) {
            ops.resize(omark);
            chunk.consts.resize(cmark);
            next_temp = st;
            return false;
        }

        const int steps_idx = static_cast<int>(chunk.chain_steps.size());
        chunk.chain_steps.push_back(std::move(steps));

        Instr in;
        in.op = OpCode::StoreLValueChainV;
        in.node_idx = add_ast_node(e->lvalue.get());   /* outer lvalue loc */
        in.target = vslot;                             /* value temp */
        in.target2 = bslot;                            /* base slot */
        /* DUAL operand: lo = the chain_steps pool idx, hi = the base kind */
        in.set_a_dual(steps_idx, bkind);
        in.aop = e->op;
        ops.push_back(in);
        return true;
    }

    /*
     * The R4 VALUE-form catch-all: an inc-dec used as a value whose lvalue the
     * pure read+mutate path declined (a side-effecting subscript index/base -
     * `y = a[f()]++`, `z = ++d[kf()]`, `y = a[f()][g()]++`, `y = a[f()].x++`,
     * `y = mk()[0]++` - or a shape it doesn't cover). Decomposes the lvalue
     * into a root + member/subscript steps (each subscript KEY compiled into a
     * temp ONCE, in the tree-walker's eval order: root first, then keys
     * inside-out), pools the steps + tier/flags/carets in `incdec_chains`, and
     * emits ONE IncDecChainV. The runtime walk + final-step semantics
     * (vm_incdec_final) mirror IncDecExpr::do_eval's tiers byte-identically -
     * including the AST-shape-dependent flat/POD gates (`allow_flat`/
     * `allow_pod` = no_side_effects(final base), try_flat/try_pod's own gate)
     * and a compiled RVALUE root's rvalue-ness (kind 3 seeds the walk with a
     * VALUE, so `mk()[0]++` still throws NotLValueEx).
     */
    bool try_incdec_chain(const IncDecExpr *inc, int &out_slot,
                          std::vector<Instr> &ops)
    {
        /* Decompose OUTSIDE-IN, like try_native_chain_store. */
        std::vector<const Construct *> chain;   /* outermost-first */
        const Construct *root = inc->lvalue.get();
        for (;;) {
            if (auto *m = dynamic_cast<const MemberExpr *>(root)) {
                if (m->optional)
                    return false;   /* d?.f++ is compile-rejected anyway */
                chain.push_back(root);
                root = m->what.get();
            } else if (dynamic_cast<const Subscript *>(root)) {
                chain.push_back(root);
                root = static_cast<const Subscript *>(root)->what.get();
            } else {
                break;
            }
        }
        if (chain.empty())
            return false;           /* a bare id: the pure path's territory */

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();
        const int st = next_temp;

        /* The ROOT: a container slot (kind 0/1/2), else compile the expression
         * into a temp (kind 3 - an RVALUE root; the walk seeds a VALUE from it,
         * reproducing the tree-walker's non-lvalue base semantics). Compiled
         * FIRST, before any key - the tree-walker's eval order. */
        int bslot, bkind;
        if (!as_container_base(root, bslot, bkind)) {
            if (!compile_boxed_expr(root, bslot, ops)) {
                ops.resize(omark);
                chunk.consts.resize(cmark);
                next_temp = st;
                return false;
            }
            bkind = 3;
        }

        /* Steps INSIDE-OUT; a subscript key compiles into a temp NOW (once). */
        std::vector<Chunk::ChainStep> steps;
        steps.reserve(chain.size());
        const size_t nsteps = chain.size();
        for (size_t i = 0; i < nsteps; i++) {
            const Construct *cn = chain[nsteps - 1 - i];
            if (auto *m = dynamic_cast<const MemberExpr *>(cn)) {
                steps.push_back({true, add_member_key(m), cn->start, cn->end});
            } else {
                const Subscript *sub = static_cast<const Subscript *>(cn);
                int kslot;
                if (!compile_boxed_expr(sub->index.get(), kslot, ops)) {
                    ops.resize(omark);
                    chunk.consts.resize(cmark);
                    next_temp = st;
                    return false;
                }
                steps.push_back({false, kslot, cn->start, cn->end});
            }
        }

        /* The site: tier + the AST-shape flat/POD gates + carets. */
        Chunk::IncDecChain site;
        const Construct *fin = chain[0];        /* outermost == final step */
        site.tier2 = inc->th == TypeHint::i || inc->th == TypeHint::f;
        site.is_prefix = inc->is_prefix;
        if (auto *fsub = dynamic_cast<const Subscript *>(fin)) {
            site.allow_flat = construct_no_side_effects(fsub->what.get());
            site.kstart = fsub->index->start;
            site.kend = fsub->index->end;
        } else {
            auto *fmem = static_cast<const MemberExpr *>(fin);
            site.allow_pod = construct_no_side_effects(fmem->what.get());
        }
        site.id_start = inc->start;
        site.id_end = inc->end;
        site.steps = std::move(steps);

        const int site_idx = static_cast<int>(chunk.incdec_chains.size());
        chunk.incdec_chains.push_back(std::move(site));

        const int dst = alloc_temp();
        Instr in;
        in.op = OpCode::IncDecChainV;
        /* extract_locs: the inc-dec span -> the loc side table (the
         * undefined-global-root caret) + inline_ctx; then node-free. */
        in.node_idx = add_ast_node(inc);
        in.target = dst;
        in.target2 = bslot;
        in.set_a(int_lit(bkind));          /* root kind: 0/1/2, 3 = rvalue temp */
        in.set_b(int_lit(site_idx));       /* incdec_chains pool idx */
        in.aop = inc->is_inc ? Op::plus : Op::minus;
        ops.push_back(in);
        out_slot = dst;
        return true;
    }

    /*
     * Compile statement `s` to native int op(s). Handles `x++`/`x--`, a
     * compound assignment `x OP= <int expr>` (rhs may be nested), and a plain
     * `x = <definitely-int expr>`. Returns false otherwise (a plain assign of a
     * bare/ bool rhs, a decl, a call, ...) so the loop falls back.
     */
    bool compile_int_stmt(const Construct *s, std::vector<Instr> &ops)
    {
        if (const IncDecExpr *inc = dynamic_cast<const IncDecExpr *>(s)) {
            Operand dst;
            if (inc->th == TypeHint::i && as_int_operand(inc->lvalue.get(), dst)
                && !dst.is_lit) {
                Instr in;
                in.op = OpCode::IntBin;
                in.node_idx = add_ast_node(s);
                in.target = dst.slot;
                in.set_a(dst);
                in.set_b(int_lit(1));
                in.aop = inc->is_inc ? Op::plus : Op::minus;
                ops.push_back(in);
                return true;
            }
            /* A flat-INT array element `a[i]++` / `a[i]--` -> StoreElemInt with
             * the base op + a constant 1 (`a[i] += 1`). StoreElemInt's loc is
             * node->start/end (generic - works for an IncDecExpr node). */
            if (const Subscript *sub =
                    dynamic_cast<const Subscript *>(inc->lvalue.get())) {
                /* A NESTED subscript `a[i][j]++`/`--` -> StoreElem2V compound
                 * (== `a[i][j] += 1`), with a synthesized boxed 1. Checked
                 * FIRST: `a[i][j]` is base_array too, but its base `a[i]` is a
                 * Subscript (not a slot), so the flat path below would bail.
                 * The inner base is a Subscript over a slotted array; matches
                 * the Expr14 nested-store path (value first, then k1, then k2). */
                if (const Subscript *inner =
                        dynamic_cast<const Subscript *>(sub->what.get())) {
                    /* int/float-ONLY (like all inc-dec): a dyn element must fall
                     * back (`+= 1` would concat a string; `++` throws). */
                    if (inc->th != TypeHint::i && inc->th != TypeHint::f)
                        return false;
                    int aslot;
                    if (!as_array_slot(inner->what.get(), aslot))
                        return false;
                    const int one = alloc_temp();
                    Instr ld;
                    ld.op = OpCode::LoadConstV;
                    ld.target = one;
                    ld.target2 = add_const(EvalValue((int_type)1));
                    ops.push_back(ld);
                    int k1slot, k2slot;
                    if (!compile_boxed_expr(inner->index.get(), k1slot, ops)
                        || !compile_boxed_expr(sub->index.get(), k2slot, ops))
                        return false;
                    Instr in;
                    in.op = OpCode::StoreElem2V;
                    in.target = one;                   /* the boxed-1 value slot */
                    in.target2 = aslot;
                    /* DUAL: lo = the k1 temp slot, hi = the chain_locs idx
                     * (inner k1 loc, outer k2 loc - the per-step carets for a
                     * byte-identical intermediate throw). */
                    in.set_a_dual(k1slot, add_chain_locs({inner, sub}));
                    in.set_b(slot_op(k2slot));
                    in.aop = inc->is_inc ? Op::addeq : Op::subeq;
                    ops.push_back(in);
                    return true;
                }
                if (sub->th == TypeHint::i && sub->base_array) {
                    int aslot, akind;
                    Operand idx;
                    if (!as_container_base(sub->what.get(), aslot, akind)
                        || !compile_int_expr(sub->index.get(), idx, ops))
                        return false;
                    Instr in;
                    in.op = OpCode::StoreElemInt;
                    in.node_idx = add_ast_node(sub);   /* subscript loc for OOB (matches TW) */
                    in.target = akind;   /* base kind: 0 loc / 1 gbl / 2 cap */
                    in.target2 = aslot;
                    in.set_a(idx);
                    in.set_b(int_lit(1));
                    in.aop = inc->is_inc ? Op::plus : Op::minus;
                    ops.push_back(in);
                    return true;
                }
                /* A DICT element `d[k]++`/`d[k]--` -> DictStore with a boxed
                 * 1 + the compound op (== `d[k] += 1`), now DictStore is
                 * AST-free (node = the subscript, for its loc). Gated on
                 * base_dict ONLY (like the Expr14 `d[k] += v` compound), NOT on
                 * the value th: a default dict `dict(0)` infers a `none` value
                 * type yet `counts[w]++` is valid; the boxed 1 promotes for
                 * int/float, a non-numeric value throws as the TW does. */
                if (sub->base_dict) {
                    int dslot, dkind;
                    if (!as_container_base(sub->what.get(), dslot, dkind))
                        return false;
                    const int vtemp = alloc_temp();   /* the boxed 1 (value) */
                    Instr ld;
                    ld.op = OpCode::LoadConstV;
                    ld.target = vtemp;
                    ld.target2 = add_const(EvalValue((int_type)1));
                    ops.push_back(ld);
                    int kslot;
                    if (!compile_boxed_expr(sub->index.get(), kslot, ops))
                        return false;
                    Instr in;
                    in.op = OpCode::DictStore;
                    in.node_idx = add_ast_node(sub);
                    in.target = dkind;   /* base kind: 0 loc / 1 gbl / 2 cap */
                    in.target2 = dslot;
                    in.set_a(slot_op(kslot));
                    in.set_b(slot_op(vtemp));
                    in.aop = inc->is_inc ? Op::addeq : Op::subeq;
                    ops.push_back(in);
                    return true;
                }
            }

            /* A struct/dict MEMBER `p.x++` / `d.k++` -> the compound member
             * store (== `p.x += 1`), with a synthesized boxed 1. Mirrors the
             * Expr14 member-store path (StoreMemberV / DictStore). Gated on a
             * PROVEN int/float lvalue (`inc->th`): inc-dec is int/float-ONLY, so
             * a dyn/general field must fall back (`b.v++` on a string throws,
             * but `b.v += 1` would concat) - the compound store is equivalent
             * only when the field is numeric. */
            if (inc->th != TypeHint::i && inc->th != TypeHint::f)
                return false;
            if (const MemberExpr *m =
                    dynamic_cast<const MemberExpr *>(inc->lvalue.get())) {
                if (!m->base_struct && !m->base_dict)
                    return false;
                int bslot, bkind;
                if (!as_container_base(m->what.get(), bslot, bkind))
                    return false;
                const int one = alloc_temp();
                Instr ld;
                ld.op = OpCode::LoadConstV;
                ld.target = one;
                ld.target2 = add_const(EvalValue((int_type)1));
                ops.push_back(ld);
                const Op cop = inc->is_inc ? Op::addeq : Op::subeq;
                Instr in;
                if (m->base_dict) {
                    Instr kin;               /* the member name as a string key */
                    kin.op = OpCode::LoadConstV;
                    kin.node_idx = add_ast_node(m);
                    kin.target = alloc_temp();
                    kin.target2 = add_const(m->memId);
                    ops.push_back(kin);
                    in.op = OpCode::DictStore;
                    in.node_idx = add_ast_node(m);
                    in.target = bkind;
                    in.target2 = bslot;
                    in.set_a(slot_op(kin.target));
                    in.set_b(slot_op(one));
                    in.aop = cop;
                } else {
                    in.op = OpCode::StoreMemberV;
                    in.target = bkind;
                    in.target2 = bslot;
                    in.set_a(int_lit(add_member_key(m)));
                    in.set_b(slot_op(one));
                    in.aop = cop;
                }
                ops.push_back(in);
                return true;
            }
            return false;
        }

        const Expr14 *e = dynamic_cast<const Expr14 *>(s);
        if (!e)
            return false;

        /* A GENERAL member/subscript lvalue CHAIN (`a[i].f=v`, `q.p.x=v`,
         * `s.f[i]=v`) - tried first, but gated to fire ONLY on a ≥2-step chain
         * with ≥1 member step (the simple single-step and pure-subscript-chain
         * stores below handle everything else). */
        if (try_native_chain_store(e, ops))
            return true;

        /* Native array-element store `a[i] = v` / `a[i] OP= v` (int element).
         * The value is compiled BEFORE the index (tree-walker: rhs then index).
         * A subscript lvalue can't be a scalar slot, so this always returns. */
        if (const Subscript *sub =
                dynamic_cast<const Subscript *>(e->lvalue.get())) {

            /* NESTED store `a[k0][k1]...[kn] = v` / `OP= v`: the base is another
             * Subscript. Collect the whole chain (outermost-first) down to a
             * slotted base. A depth-2 store over a LOCAL array keeps the tuned
             * StoreElem2V (value, k1, k2); anything deeper (or a global/capture
             * base) -> the GENERIC StoreElemChainV (value, then keys base-to-
             * innermost). Matches the tree-walker's rhs-then-lvalue eval order. */
            if (dynamic_cast<const Subscript *>(sub->what.get())) {
                switch (e->op) {
                case Op::assign: case Op::addeq: case Op::subeq:
                case Op::muleq:  case Op::diveq: case Op::modeq: break;
                default: return false;
                }
                std::vector<const Subscript *> chain;   /* outermost-first */
                const Construct *cur = sub;
                while (auto *s = dynamic_cast<const Subscript *>(cur)) {
                    chain.push_back(s);
                    cur = s->what.get();
                }
                const int nkeys = static_cast<int>(chain.size());

                /* Depth-2 fast path (local array base) -> StoreElem2V. */
                int aslot;
                if (nkeys == 2 && as_array_slot(cur, aslot)) {
                    const Subscript *inner = chain[1];
                    int vslot, k1slot, k2slot;
                    if (!compile_boxed_expr(e->rvalue.get(), vslot, ops)
                        || !compile_boxed_expr(inner->index.get(), k1slot, ops)
                        || !compile_boxed_expr(sub->index.get(), k2slot, ops))
                        return false;
                    Instr in;
                    in.op = OpCode::StoreElem2V;
                    in.target = vslot;
                    in.target2 = aslot;
                    /* DUAL: lo = the k1 temp slot, hi = the chain_locs idx. */
                    in.set_a_dual(k1slot, add_chain_locs({inner, sub}));
                    in.set_b(slot_op(k2slot));
                    in.aop = e->op;
                    ops.push_back(in);
                    return true;
                }

                /* Generic N-level -> StoreElemChainV. */
                int bslot, bkind;
                if (!as_container_base(cur, bslot, bkind))
                    return false;
                const size_t omark = ops.size();
                const size_t cmark = chunk.consts.size();
                const int st = next_temp;
                int vslot;
                if (!compile_boxed_expr(e->rvalue.get(), vslot, ops)) {
                    ops.resize(omark);
                    chunk.consts.resize(cmark);
                    next_temp = st;
                    return false;
                }
                const int keybase = next_temp;
                next_temp += nkeys;
                if (next_temp > max_temp)
                    max_temp = next_temp;
                bool ok = true;
                for (int k = 0; k < nkeys && ok; k++)
                    /* base-to-innermost: chain is outermost-first, so index k
                     * (from the base) is chain[nkeys-1-k]'s index. */
                    ok = compile_to_run_slot(chain[nkeys - 1 - k]->index.get(),
                                             keybase + k, ops);
                if (!ok) {
                    ops.resize(omark);
                    chunk.consts.resize(cmark);
                    next_temp = st;
                    return false;
                }
                /* Per-step carets INSIDE-OUT (matching the keys): key k is
                 * chain[nkeys-1-k]'s subscript node. nkeys is derived from the
                 * pool entry's size, so a.slot holds the pool index. */
                std::vector<const Construct *> locnodes;
                locnodes.reserve(static_cast<size_t>(nkeys));
                for (int k = 0; k < nkeys; k++)
                    locnodes.push_back(chain[nkeys - 1 - k]);
                Instr in;
                in.op = OpCode::StoreElemChainV;
                in.target = vslot;
                in.target2 = bslot;
                /* DUAL: lo = the chain_locs idx (nkeys = entry size), hi =
                 * the base kind. */
                in.set_a_dual(add_chain_locs(locnodes), bkind);
                in.set_b(int_lit(keybase));
                in.aop = e->op;
                ops.push_back(in);
                return true;
            }

            /* P2: a DICT element store `d[k] = v` / `d[k] OP= v` -> DictStore.
             * The base must be a local slot; the KEY + VALUE compile to boxed
             * temps (value first - tree-walker rhs-then-lvalue order; the base
             * `d` is a side-effect-free slot read done in the handler). The
             * runtime Type::subscript(for_write) + slot_rmw do the store. */
            if (sub->base_dict) {
                int dslot, dkind;
                switch (e->op) {
                case Op::assign: case Op::addeq: case Op::subeq:
                case Op::muleq:  case Op::diveq: case Op::modeq: break;
                default: return false;
                }
                if (!as_container_base(sub->what.get(), dslot, dkind))
                    return false;
                int vslot, kslot;
                if (!compile_boxed_expr(e->rvalue.get(), vslot, ops)
                    || !compile_boxed_expr(sub->index.get(), kslot, ops))
                    return false;
                Instr in;
                in.op = OpCode::DictStore;
                in.node_idx = add_ast_node(sub);   /* the subscript, for its loc (extract_locs) */
                in.target = dkind;   /* base kind: 0 local / 1 global / 2 cap */
                in.target2 = dslot;
                in.set_a(slot_op(kslot));
                in.set_b(slot_op(vslot));
                in.aop = e->op;
                ops.push_back(in);
                return true;
            }

            /* FLAT int array element `a[i] = <int>` / `OP= <int>` -> the fast
             * unboxed StoreElemInt (an int-compilable value + index). On any
             * failure - e.g. a dyn index (a closure param) or a non-int rhs -
             * roll the partial ops back and FALL THROUGH to the universal
             * StoreElemValue below (which boxes the operands and dispatches at
             * runtime). A flat FLOAT array is left to compile_float_stmt's
             * StoreElemFloat (excluded from the catch-all). */
            if (sub->th == TypeHint::i && sub->base_array) {
                const size_t mark = ops.size();
                Op aop = Op::invalid;
                bool ok = true;
                switch (e->op) {
                    case Op::assign: aop = Op::invalid; break;
                    case Op::addeq:  aop = Op::plus;    break;
                    case Op::subeq:  aop = Op::minus;   break;
                    case Op::muleq:  aop = Op::times;   break;
                    case Op::diveq:  aop = Op::div;     break;
                    case Op::modeq:  aop = Op::mod;     break;
                    default: ok = false; break;
                }
                int aslot, akind;
                Operand val, idx;
                if (ok && as_container_base(sub->what.get(), aslot, akind)) {
                    /* A bool-literal RHS (`a[i]=true/false`, common in bool
                     * arrays) is a 0/1 int operand: StoreElemInt's bool branch
                     * writes it to bvec, or an int array stores the promoted
                     * 0/1. A bool VAR / comparison RHS compiles via compile_int_
                     * expr (th==i). rhs-before-index order is preserved. */
                    bool vok;
                    if (const LiteralBool *lb =
                            dynamic_cast<const LiteralBool *>(e->rvalue.get())) {
                        val = int_lit(lb->bval() ? 1 : 0);
                        vok = true;
                    } else {
                        vok = compile_int_expr(e->rvalue.get(), val, ops);
                    }
                    if (vok && compile_int_expr(sub->index.get(), idx, ops)) {
                        Instr in;
                        in.op = OpCode::StoreElemInt;
                        /* OOB/type errors carry the SUBSCRIPT loc (matching the
                         * tree-walker's flat_store_core), a COMPOUND div0 the
                         * Expr14 loc (stamped by Construct::eval). A PLAIN assign
                         * can't div0, so the subscript loc is fully correct;
                         * only a compound needs the Expr14 (its OOB then trails
                         * the tree-walker by the `OP= rhs` width - a rare edge). */
                        in.node_idx = add_ast_node(
                            aop == Op::invalid ? static_cast<const Construct *>(
                                                     sub)
                                               : static_cast<const Construct *>(
                                                     s));
                        in.target = akind;   /* 0 local / 1 global / 2 cap */
                        in.target2 = aslot;
                        in.set_a(idx);
                        in.set_b(val);
                        in.aop = aop;
                        ops.push_back(in);
                        return true;
                    }
                }
                ops.resize(mark);   /* fall through to the universal store */
            }

            /* UNIVERSAL store (catch-all): ANY container-slot base -> Store
             * ElemValue, whose vm_subscript_store dispatches at runtime (flat /
             * general / dict, matching the tree-walker's try_flat->general).
             * Covers a proven GENERAL array, a DYN / captured / unproven base,
             * AND a flat int array with a non-int-compilable index (fell through
             * above). EXCLUDES a proven flat FLOAT array (th==f && base_array),
             * left to compile_float_stmt's fast unboxed StoreElemFloat. */
            if (!(sub->th == TypeHint::f && sub->base_array)) {
                int aslot, akind;
                switch (e->op) {
                case Op::assign: case Op::addeq: case Op::subeq:
                case Op::muleq:  case Op::diveq: case Op::modeq: break;
                default: return false;
                }
                if (!as_container_base(sub->what.get(), aslot, akind))
                    return false;
                int vslot, kslot;
                if (!compile_boxed_expr(e->rvalue.get(), vslot, ops)
                    || !compile_boxed_expr(sub->index.get(), kslot, ops))
                    return false;
                Instr in;
                in.op = OpCode::StoreElemValue;
                in.node_idx = add_ast_node(sub);   /* the subscript, for its loc (extract_locs) */
                in.target = akind;   /* base kind: 0 local / 1 global / 2 cap */
                in.target2 = aslot;
                in.set_a(slot_op(kslot));
                in.set_b(slot_op(vslot));
                in.aop = e->op;
                ops.push_back(in);
                return true;
            }

            return false;   /* proven flat float -> compile_float_stmt */
        }

        /* A DICT MEMBER store `d.k = v` / `d.k OP= v` -> DictStore, exactly like
         * `d["k"] = v`: the member NAME is the string key (baked into the const
         * pool, loaded to a temp). base_dict is set by the inferencer for a dict
         * base; a STRUCT member store falls through (a separate op). */
        if (const MemberExpr *m =
                dynamic_cast<const MemberExpr *>(e->lvalue.get())) {
            if (m->base_dict) {
                int dslot, dkind;
                switch (e->op) {
                case Op::assign: case Op::addeq: case Op::subeq:
                case Op::muleq:  case Op::diveq: case Op::modeq: break;
                default: return false;
                }
                if (!as_container_base(m->what.get(), dslot, dkind))
                    return false;
                int vslot;
                if (!compile_boxed_expr(e->rvalue.get(), vslot, ops))
                    return false;
                Instr kin;               /* the member name as a string key */
                kin.op = OpCode::LoadConstV;
                kin.node_idx = add_ast_node(m);
                kin.target = alloc_temp();
                kin.target2 = add_const(m->memId);
                ops.push_back(kin);
                Instr in;
                in.op = OpCode::DictStore;
                in.node_idx = add_ast_node(m);             /* for its loc (extract_locs) */
                in.target = dkind;       /* base kind: 0 local / 1 gbl / 2 cap */
                in.target2 = dslot;
                in.set_a(slot_op(kin.target));
                in.set_b(slot_op(vslot));
                in.aop = e->op;
                ops.push_back(in);
                return true;
            }

            /* A STRUCT field store `s.f = v` / `s.f OP= v` -> StoreMemberV. The
             * base is a slotted local/global/capture; the value is a boxed temp.
             * The member uid + carets ride in the member-key pool (AST-free). */
            if (m->base_struct) {
                int sslot, skind;
                switch (e->op) {
                case Op::assign: case Op::addeq: case Op::subeq:
                case Op::muleq:  case Op::diveq: case Op::modeq: break;
                default: return false;
                }
                if (!as_container_base(m->what.get(), sslot, skind))
                    return false;
                int vslot;
                if (!compile_boxed_expr(e->rvalue.get(), vslot, ops))
                    return false;
                Instr in;
                in.op = OpCode::StoreMemberV;
                in.target = skind;       /* base kind: 0 local / 1 gbl / 2 cap */
                in.target2 = sslot;
                in.set_a(int_lit(add_member_key(m)));   /* AST-free: pool index */
                in.set_b(slot_op(vslot));
                in.aop = e->op;
                ops.push_back(in);
                return true;
            }
        }

        Operand dst;
        if (!as_int_operand(e->lvalue.get(), dst) || dst.is_lit)
            return false;

        if (e->op == Op::assign) {
            if (!definitely_int(e->rvalue.get()))
                return false;
            Operand r;
            if (!compile_int_expr(e->rvalue.get(), r, ops))
                return false;
            /* Peephole: if the last op produced `r` in a temp, retarget it to
             * write `dst` directly; a constant -> a clean LoadImmInt; else a
             * slot-to-slot copy (dst = r + 0). */
            if (!r.is_lit && !ops.empty() && ops.back().op == OpCode::IntBin
                && ops.back().target == r.slot) {
                ops.back().target = dst.slot;
            } else if (r.is_lit) {
                Instr in;
                in.op = OpCode::LoadImmInt;
                in.node_idx = add_ast_node(s);
                in.target = dst.slot;
                in.set_a(r);
                ops.push_back(in);
            } else {
                Instr in;                /* dst = r + 0 (slot copy) */
                in.op = OpCode::IntBin;
                in.node_idx = add_ast_node(s);
                in.target = dst.slot;
                in.set_a(r);
                in.set_b(int_lit(0));
                in.aop = Op::plus;
                ops.push_back(in);
            }
            return true;
        }

        Op arith;
        switch (e->op) {
            case Op::addeq: arith = Op::plus;  break;
            case Op::subeq: arith = Op::minus; break;
            case Op::muleq: arith = Op::times; break;
            case Op::diveq: arith = Op::div;   break;
            case Op::modeq: arith = Op::mod;   break;
            default: return false;
        }

        Operand rhs;                     /* dst = dst <arith> rhs (rhs nested) */
        if (!compile_int_expr(e->rvalue.get(), rhs, ops))
            return false;
        Instr in;
        in.op = OpCode::IntBin;
        in.node_idx = add_ast_node(s);
        in.target = dst.slot;
        in.set_a(dst);
        in.set_b(rhs);
        in.aop = arith;
        ops.push_back(in);
        return true;
    }

    /*
     * Read a while condition as an int comparison into `a <cmp> b`, appending
     * any operand-computation ops (a nested `(x+1) < N`) to `ops`. False for a
     * non-int / non-comparison condition.
     */
    bool compile_int_cond(const Construct *cond, std::vector<Instr> &ops,
                          Operand &a, Op &cmp, Operand &b)
    {
        const TypedScalarExpr *t = dynamic_cast<const TypedScalarExpr *>(cond);
        if (!t || t->cat != TypedScalarExpr::Cat::cmp
            || t->kind != TypeHint::i || t->elems.size() != 2)
            return false;
        cmp = t->elems[1].first;
        return compile_int_expr(t->elems[0].second.get(), a, ops)
            && compile_int_expr(t->elems[1].second.get(), b, ops);
    }

    /* The FLOAT analogues of compile_int_expr/stmt/cond (mirroring the tree-
     * walker's eval_float): operands read as float, arithmetic is `double`,
     * FloatBin writes a float slot. No bool-safety concern - a float
     * destination is never a bool slot. */

    bool compile_float_expr(const Construct *e, Operand &out,
                            std::vector<Instr> &ops)
    {
        if (as_float_operand(e, out))
            return true;

        if (e->th == TypeHint::f && try_typed_ternary(e, out, ops, true))
            return true;

        if (e->th == TypeHint::f)
            if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(e)) {
                if (try_sfe_field(m, out, ops, OpCode::LoadStructFieldFloat))
                    return true;
                if (try_member_scalar(m, out, ops, OpCode::LoadMemberFloat))
                    return true;
            }

        int dtt;
        if (try_dict_scalar_load(e, dtt, ops, OpCode::DictLoadFloat,
                                 TypeHint::f)) {
            out = slot_op(dtt);
            return true;
        }

        /* Array element read `a[i]` (float element) -> LoadElemFloat; the index
         * is still an int expression. */
        if (const Subscript *sub = dynamic_cast<const Subscript *>(e)) {
            if (e->th != TypeHint::f || !sub->base_array)
                return false;
            int aslot;
            Operand idx;
            if (!compile_array_base(sub->what.get(), aslot, ops)
                || !compile_int_expr(sub->index.get(), idx, ops))
                return false;
            const int tt = alloc_temp();
            Instr in;
            in.op = OpCode::LoadElemFloat;
            in.node_idx = add_ast_node(e);
            in.target = tt;
            in.target2 = aslot;
            in.set_a(idx);
            ops.push_back(in);
            out = slot_op(tt);
            return true;
        }

        /* A scalar-result BUILTIN call -> CallBuiltinV (value ABI); the
         * result is a native float operand. A MATH builtin (sqrt/sin/...)
         * gets the marshal-free MathFnV first (F1). An unliftable one
         * declines (EvalToSlot is gone - the statement falls back whole). */
        if (e->th == TypeHint::f)
            if (const DirectBuiltinCallExpr *bc =
                    dynamic_cast<const DirectBuiltinCallExpr *>(e)) {
                int t;
                if (try_math_fn(bc, t, ops)
                        || try_native_builtin(bc, t, ops)) {
                    out = slot_op(t);
                    return true;
                }
                return false;
            }

        /* A float-returning native user-function call -> CallV. */
        if (e->th == TypeHint::f)
            if (const DirectCallExpr *dc =
                    dynamic_cast<const DirectCallExpr *>(e)) {
                int ct;
                if (try_native_call(dc, ct, ops)) {
                    out = slot_op(ct);
                    return true;
                }
            }

        const TypedScalarExpr *t = dynamic_cast<const TypedScalarExpr *>(e);
        if (!t || t->kind != TypeHint::f)
            return false;

        if (t->cat == TypedScalarExpr::Cat::arith) {
            Operand acc;
            if (!compile_float_expr(t->elems[0].second.get(), acc, ops))
                return false;
            for (size_t i = 1; i < t->elems.size(); i++) {
                Operand rhs;
                if (!compile_float_expr(t->elems[i].second.get(), rhs, ops))
                    return false;
                const int tt = alloc_temp();
                Instr in;
                in.op = OpCode::FloatBin;
                in.node_idx = add_ast_node(e);
                in.target = tt;
                in.set_a(acc);
                in.set_b(rhs);
                in.aop = t->elems[i].first;
                ops.push_back(in);
                acc = slot_op(tt);
            }
            out = acc;
            return true;
        }

        if (t->cat == TypedScalarExpr::Cat::neg) {
            Operand op;
            if (!compile_float_expr(t->elems[0].second.get(), op, ops))
                return false;
            const int tt = alloc_temp();
            Instr in;                    /* tt = 0.0 - op */
            in.op = OpCode::FloatBin;
            in.node_idx = add_ast_node(e);
            in.target = tt;
            in.set_a(float_lit(0));
            in.set_b(op);
            in.aop = Op::minus;
            ops.push_back(in);
            out = slot_op(tt);
            return true;
        }

        return false;
    }

    bool compile_float_stmt(const Construct *s, std::vector<Instr> &ops)
    {
        if (const IncDecExpr *inc = dynamic_cast<const IncDecExpr *>(s)) {
            const Identifier *id =
                dynamic_cast<const Identifier *>(inc->lvalue.get());
            if (inc->th == TypeHint::f && id
                && id->sym.kind == SymKind::local && id->th == TypeHint::f) {
                Instr in;
                in.op = OpCode::FloatBin;
                in.node_idx = add_ast_node(s);
                in.target = id->sym.slot;
                in.set_a(slot_op(id->sym.slot));
                in.set_b(float_lit(1));
                in.aop = inc->is_inc ? Op::plus : Op::minus;
                ops.push_back(in);
                return true;
            }
            /* A flat-FLOAT array element `a[i]++` / `a[i]--` -> StoreElemFloat
             * (== `a[i] += 1.0`); loc is node->start/end (generic). */
            if (const Subscript *sub =
                    dynamic_cast<const Subscript *>(inc->lvalue.get())) {
                if (sub->th == TypeHint::f && sub->base_array) {
                    int aslot;
                    Operand idx;
                    if (!as_array_slot(sub->what.get(), aslot)
                        || !compile_int_expr(sub->index.get(), idx, ops))
                        return false;
                    Instr in;
                    in.op = OpCode::StoreElemFloat;
                    in.node_idx = add_ast_node(sub);   /* subscript loc for OOB (matches TW) */
                    in.target2 = aslot;
                    in.set_a(idx);
                    in.set_b(float_lit(1));
                    in.aop = inc->is_inc ? Op::plus : Op::minus;
                    ops.push_back(in);
                    return true;
                }
            }
            return false;
        }

        const Expr14 *e = dynamic_cast<const Expr14 *>(s);
        if (!e)
            return false;

        /* Native array-element store `a[i] = v` / `a[i] OP= v` (float element);
         * value before index, like compile_int_stmt. */
        if (const Subscript *sub =
                dynamic_cast<const Subscript *>(e->lvalue.get())) {
            if (sub->th != TypeHint::f || !sub->base_array)
                return false;
            Op aop;
            switch (e->op) {
                case Op::assign: aop = Op::invalid; break;
                case Op::addeq:  aop = Op::plus;    break;
                case Op::subeq:  aop = Op::minus;   break;
                case Op::muleq:  aop = Op::times;   break;
                case Op::diveq:  aop = Op::div;     break;
                case Op::modeq:  aop = Op::mod;     break;
                default: return false;
            }
            int aslot, akind;
            if (!as_container_base(sub->what.get(), aslot, akind))
                return false;
            Operand val, idx;
            if (!compile_float_expr(e->rvalue.get(), val, ops)
                || !compile_int_expr(sub->index.get(), idx, ops))
                return false;
            Instr in;
            in.op = OpCode::StoreElemFloat;
            /* PLAIN assign -> the SUBSCRIPT loc (OOB/type, matching the tree-
             * walker); a COMPOUND -> the Expr14 loc (for its div0). See the
             * StoreElemInt note. */
            in.node_idx = add_ast_node(
                aop == Op::invalid ? static_cast<const Construct *>(sub)
                                   : static_cast<const Construct *>(s));
            in.target = akind;   /* base kind: 0 local / 1 global / 2 cap */
            in.target2 = aslot;
            in.set_a(idx);
            in.set_b(val);
            in.aop = aop;
            ops.push_back(in);
            return true;
        }

        /* The destination must be a genuine FLOAT slot (int dst -> compile_int_
         * stmt handles it; a float result can't narrow into an int/bool). */
        const Identifier *dst =
            dynamic_cast<const Identifier *>(e->lvalue.get());
        if (!dst || dst->sym.kind != SymKind::local || dst->th != TypeHint::f)
            return false;
        const int dslot = dst->sym.slot;

        if (e->op == Op::assign) {
            Operand r;
            if (!compile_float_expr(e->rvalue.get(), r, ops))
                return false;
            if (!r.is_lit && !ops.empty() && ops.back().op == OpCode::FloatBin
                && ops.back().target == r.slot) {
                ops.back().target = dslot;
            } else if (r.is_lit) {
                Instr in;
                in.op = OpCode::LoadImmFloat;
                in.node_idx = add_ast_node(s);
                in.target = dslot;
                in.set_a(r);
                ops.push_back(in);
            } else {
                Instr in;                /* dst = r + 0.0 (slot copy) */
                in.op = OpCode::FloatBin;
                in.node_idx = add_ast_node(s);
                in.target = dslot;
                in.set_a(r);
                in.set_b(float_lit(0));
                in.aop = Op::plus;
                ops.push_back(in);
            }
            return true;
        }

        Op arith;
        switch (e->op) {
            case Op::addeq: arith = Op::plus;  break;
            case Op::subeq: arith = Op::minus; break;
            case Op::muleq: arith = Op::times; break;
            case Op::diveq: arith = Op::div;   break;
            case Op::modeq: arith = Op::mod;   break;
            default: return false;
        }

        Operand rhs;                     /* dst = dst <arith> rhs */
        if (!compile_float_expr(e->rvalue.get(), rhs, ops))
            return false;
        Instr in;
        in.op = OpCode::FloatBin;
        in.node_idx = add_ast_node(s);
        in.target = dslot;
        in.set_a(slot_op(dslot));
        in.set_b(rhs);
        in.aop = arith;
        ops.push_back(in);
        return true;
    }

    bool compile_float_cond(const Construct *cond, std::vector<Instr> &ops,
                            Operand &a, Op &cmp, Operand &b)
    {
        const TypedScalarExpr *t = dynamic_cast<const TypedScalarExpr *>(cond);
        if (!t || t->cat != TypedScalarExpr::Cat::cmp
            || t->kind != TypeHint::f || t->elems.size() != 2)
            return false;
        cmp = t->elems[1].first;
        return compile_float_expr(t->elems[0].second.get(), a, ops)
            && compile_float_expr(t->elems[1].second.get(), b, ops);
    }

    /*
     * Emit a loop/if condition as native compare-branches that jump to the loop
     * EXIT when the condition is FALSE, recording each branch's index in
     * `exit_jumps` (the caller patches them to the exit label). A single
     * comparison -> one JumpUnless{Int,Float}Cmp. A CONJUNCTION `A && B && ...`
     * (a Cat::logical of all `&&`) -> one compare-branch per conjunct, each to
     * exit: fall-through means all held, and a later conjunct's ops only run if
     * the earlier branches didn't take (native short-circuit, sound since the
     * operands are side-effect-free scalar reads). Returns false for a `||`, a
     * non-comparison, or a boxed operand - those await the boxed general path.
     * Self-truncating per comparison; the caller discards all of chunk on
     * a false return. This is what makes `while (it < MAXIT && zr*zr+zi*zi <=
     * 4.0)` (mandelbrot) go native instead of falling back whole.
     */
    bool emit_cond_jumps(const Construct *cond,
                         std::vector<size_t> &exit_jumps)
    {
        const TypedScalarExpr *t = dynamic_cast<const TypedScalarExpr *>(cond);

        /* A typed int/float comparison -> one native compare-branch to the
         * exit. On failure, fall through to the boxed path (don't bail). */
        if (t && t->cat == TypedScalarExpr::Cat::cmp) {
            Operand a, b;
            Op cmp;
            reset_temps();
            const size_t mark = chunk.code.size();
            if (compile_int_cond(cond, chunk.code, a, cmp, b)) {
                exit_jumps.push_back(
                    emit_cmp(OpCode::JumpUnlessIntCmp, cond, cmp, a, b));
                return true;
            }
            chunk.code.resize(mark);
            reset_temps();
            if (compile_float_cond(cond, chunk.code, a, cmp, b)) {
                exit_jumps.push_back(
                    emit_cmp(OpCode::JumpUnlessFloatCmp, cond, cmp, a, b));
                return true;
            }
            chunk.code.resize(mark);
            /* fall through to boxed */
        }

        /* `A && B && ...` -> one compare-branch per conjunct (each recurses, so
         * a bool-var conjunct lowers via the boxed path below). A `||` chain
         * isn't split - it falls through to the boxed path, which evaluates it
         * as a single truthiness test. Tried into a scratch: on any conjunct
         * failure, undo the partial emission and box the whole condition. */
        if (t && t->cat == TypedScalarExpr::Cat::logical) {
            bool all_and = true;
            for (size_t i = 1; i < t->elems.size(); i++)
                if (t->elems[i].first != Op::land) { all_and = false; break; }
            if (all_and) {
                const size_t mark = chunk.code.size();
                const size_t nexit = exit_jumps.size();
                bool ok = true;
                for (const auto &pr : t->elems)
                    if (!emit_cond_jumps(pr.second.get(), exit_jumps)) {
                        ok = false;
                        break;
                    }
                if (ok)
                    return true;
                chunk.code.resize(mark);
                exit_jumps.resize(nexit);
                /* fall through to boxed */
            }
        }

        /* BOXED condition: a bool VARIABLE (`while (flag)`), a dyn/string
         * comparison, a `||` chain, or a conjunct that didn't lower - compile
         * it to a bool slot, then branch to the exit unless true. This is the
         * SAME path `if` uses, so a loop condition is now just as capable (a
         * bool-var loop cond used to bail the WHOLE loop to an eval.stmt). */
        int cslot;
        reset_temps();
        const size_t mark = chunk.code.size();
        if (compile_boxed_expr(cond, cslot, chunk.code)) {
            Instr in;
            in.op = OpCode::JumpUnlessTrueV;
            in.node_idx = add_ast_node(cond);
            in.target2 = cslot;
            exit_jumps.push_back(chunk.code.size());
            chunk.code.push_back(in);
            return true;
        }
        chunk.code.resize(mark);
        return false;
    }

    /* A loop body's statements (a Block's elems, or a single statement). */
    std::vector<const Construct *> body_stmts(const Construct *body)
    {
        std::vector<const Construct *> stmts;
        if (body) {
            if (const Block *b = dynamic_cast<const Block *>(body))
                for (const auto &e : b->elems)
                    stmts.push_back(e.get());
            else
                stmts.push_back(body);
        }
        return stmts;
    }

    /*
     * Compile a loop/if body's statements DIRECTLY into chunk.code (so any
     * jumps a nested loop / if emits are chunk-absolute, no relocation), each
     * dispatched by its OWN kind: an int/float scalar statement, a NESTED
     * `for`/`while` loop, or an `if`. A FLOW-FREE statement that isn't natively
     * compilable (an array-building decl `var row = array(n,0)`, a general
     * store `c[i] = row`, a void call) lowers via its own native op WITHIN the
     * native loop, so the loop still goes native around it. SELF-TRUNCATING: a
     * flow-AFFECTING unsupported statement (break/continue/return, or a nested
     * loop/if that can't compile) resizes chunk.code back to the body start and
     * returns false, so the caller falls the whole loop back. Also returns
     * false only when a statement cannot compile at all (the
     * tree-walker's tight counter beats a native loop that only dispatches
     * fallbacks).
     */
    /* A named `func f(..){..}` decl STATEMENT -> MakeClosureV (create the
     * FuncObject, snapshotting captures) + StoreGlobalV (write the slot +
     * mark defined) - byte-identical to FuncDeclStmt::do_eval's global-bind
     * `slots[slot] = LValue(func, false); defined = 1`. In a SCRIPT a NAMED
     * func decl ALWAYS has a global slot (top-level hoist or a scoped
     * global - hoist_scoped_decls covers nested decls, incl. inside loop/if
     * bodies): the grammar rejects a capture list on a named func, and
     * pWrapDeclBody closed the brace-less-body masked route - ML_CHECK
     * guards the invariant. Shared by gen_stmt (top-level / function-body
     * statements) and compile_scalar_body (loop/if bodies - a decl there
     * re-binds each iteration, exactly as the tree-walker re-evals it). */
    void emit_func_decl(const FuncDeclStmt *fd, std::vector<Instr> &ops)
    {
        ML_CHECK(fd->id->sym.kind == SymKind::global);
        const int t = alloc_temp();
        Instr mk;
        mk.op = OpCode::MakeClosureV;
        mk.target = t;
        mk.target2 = static_cast<int>(chunk.closure_defs.size());
        chunk.closure_defs.push_back(fd->desc);
        ops.push_back(mk);
        Instr st;                     /* aop invalid == plain assign+defined */
        st.op = OpCode::StoreGlobalV;
        st.target = fd->id->sym.slot;
        st.set_a(slot_op(t));
        ops.push_back(st);
    }

    /* A `struct P {..}` decl STATEMENT: bake the type descriptor (a trivial
     * t_structtype value holding the program-lifetime StructTypeDef*) into
     * the const pool -> LoadConstV + StoreGlobalV. The tree-walker binds it
     * CONST, but that flag is unobservable at runtime (a reassign `P = x` is
     * a compile-time error, `isconst` folds), so a plain StoreGlobalV is
     * differential-identical. Like a func decl, a SCRIPT struct decl is
     * always global (structs never capture). Shared like emit_func_decl. */
    void emit_struct_decl(const StructDeclStmt *sd, std::vector<Instr> &ops)
    {
        ML_CHECK(sd->id->sym.kind == SymKind::global);
        const int t = alloc_temp();
        Instr ld;
        ld.op = OpCode::LoadConstV;
        ld.target = t;
        ld.target2 = add_const(EvalValue(sd->def));
        ops.push_back(ld);
        Instr st;
        st.op = OpCode::StoreGlobalV;
        st.target = sd->id->sym.slot;
        st.set_a(slot_op(t));
        ops.push_back(st);
    }

    bool compile_scalar_body(const std::vector<const Construct *> &stmts,
                             bool is_loop_body = true)
    {
        const size_t start = chunk.code.size();
        bool any_native = false;
        for (const Construct *s : stmts) {
            reset_temps();
            const size_t mark = chunk.code.size();
            if (compile_int_stmt(s, chunk.code)) {
                any_native = true;
                continue;
            }
            chunk.code.resize(mark);
            reset_temps();
            if (compile_float_stmt(s, chunk.code)) {
                any_native = true;
                continue;
            }
            chunk.code.resize(mark);
            reset_temps();
            if (compile_boxed_stmt(s, chunk.code)) {   /* dyn/string assign */
                any_native = true;
                continue;
            }
            chunk.code.resize(mark);

            /* Nested native control flow. Each is itself self-truncating, so on
             * failure chunk.code is already back at `mark`. */
            if (const ForRangeStmt *fr = dynamic_cast<const ForRangeStmt *>(s)) {
                if (try_native_for_range(fr)) {
                    any_native = true;
                    continue;
                }
            } else if (const WhileStmt *w = dynamic_cast<const WhileStmt *>(s)) {
                if (try_native_scalar_while(w)) {
                    any_native = true;
                    continue;
                }
            } else if (const IfStmt *iff = dynamic_cast<const IfStmt *>(s)) {
                if (compile_native_if(iff)) {
                    any_native = true;
                    continue;
                }
            } else if (const ForStmt *fs = dynamic_cast<const ForStmt *>(s)) {
                if (try_native_for(fs)) {
                    any_native = true;
                    continue;
                }
            } else if (const ForeachStmt *fe =
                           dynamic_cast<const ForeachStmt *>(s)) {
                if (try_native_foreach(fe) || try_native_foreach_str(fe)
                    || try_native_foreach_unpack(fe)
                    || try_native_struct_foreach(fe)
                    || try_native_dict_foreach(fe)
                    || try_native_dyn_foreach(fe)) {
                    any_native = true;
                    continue;
                }
            } else if (const TryCatchStmt *tc =
                           dynamic_cast<const TryCatchStmt *>(s)) {
                if (compile_native_try(tc)) {   /* native handler region (P8) */
                    any_native = true;
                    continue;
                }
            } else if (const Block *blk = dynamic_cast<const Block *>(s)) {
                /* A bare nested block `{ ... }` - what a const-folded
                 * `if (true) { ... }` leaves behind (a const condition drops
                 * the if but keeps its braced body), or an explicit block. A
                 * scope_free block runs inline in the same frame, so compile
                 * its statements in place (nested blocks recurse). Without this
                 * the whole enclosing loop failed to lower. */
                if (blk->scope_free && compile_scalar_body(body_stmts(blk))) {
                    any_native = true;
                    continue;
                }
            }

            chunk.code.resize(mark);

            /* break / continue -> a native Jump to the enclosing loop's exit /
             * continue point (backpatched by pop_loop). Needs an enclosing
             * native loop frame (always present - compile_scalar_body only runs
             * inside one). Gap B. A break/continue crossing a single try pops
             * its handler + runs its finally first (Inc 2c step 3); crossing
             * nested trys (emit_break_cont returns false) fails the body so the
             * region tree-walks. */
            if (dynamic_cast<const BreakStmt *>(s)) {
                if (loops.empty() || !emit_break_cont(/*is_break=*/true)) {
                    chunk.code.resize(start);
                    return false;
                }
                any_native = true;
                continue;
            }
            if (dynamic_cast<const ContinueStmt *>(s)) {
                if (loops.empty() || !emit_break_cont(/*is_break=*/false)) {
                    chunk.code.resize(start);
                    return false;
                }
                any_native = true;
                continue;
            }

            /* An assignment/decl (Expr14), a call statement (CallExpr), or a
             * `return` (ReturnStmt) lowers via ReturnV WITHIN the
             * native loop. Expr14/CallExpr don't touch the loop's FlowState;
             * a return sets flow==ret and stops the chunk (abandoning the
             * loop). Anything else that can't compile fails the whole loop
             * (-> the enclosing statement's NotLoweredEx). */
            /* A user-function call statement -> CallV (result discarded). */
            if (const DirectCallExpr *dc =
                    dynamic_cast<const DirectCallExpr *>(s)) {
                int dst;
                if (try_native_call(dc, dst, chunk.code)) {
                    any_native = true;
                    continue;
                }
            }
            /* A builtin call statement -> CallBuiltinV (result discarded). */
            if (const DirectBuiltinCallExpr *bc =
                    dynamic_cast<const DirectBuiltinCallExpr *>(s)) {
                int dst;
                if (try_native_builtin(bc, dst, chunk.code)) {
                    any_native = true;
                    continue;
                }
            }
            /* A func-VALUE call statement -> CallValueV (result discarded);
             * rejects a Direct{Call,BuiltinCall}Expr internally (F-3). */
            if (const CallExpr *call = dynamic_cast<const CallExpr *>(s)) {
                int dst;
                if (try_native_value_call(call, dst, chunk.code)) {
                    any_native = true;
                    continue;
                }
            }
            /* `return <expr>;` -> ReturnV / SetPend-to-finally (Inc 2c). */
            if (const ReturnStmt *ret = dynamic_cast<const ReturnStmt *>(s)) {
                if (try_native_return(ret, chunk.code)) {
                    any_native = true;
                    continue;
                }
                /* Inside an INLINED-CALL boundary a return MUST be redirected
                 * (yield the expr value); try_native_return declined (an
                 * uncompilable return value or crossed finally) - so fail
                 * the body and let the entire InlinedCallExpr tree-walk
                 * (byte-identical). */
                if (!inline_returns.empty()) {
                    chunk.code.resize(start);
                    return false;
                }
                /* try_native_return declined (a nested-try return, or an
                 * uncompilable value). If any enclosing try has a finally, a
                 * partial lowering could SKIP it - fail the whole body and
                 * let the try region tree-walk (runs the finally correctly)
                 * ... which itself fails to lower -> NotLoweredEx: with the
                 * no-fail contract this path must be unreachable (all return
                 * values compile; crossed finallys inline). Kept as the
                 * conservative guard. */
                for (const auto &tf : trys)
                    if (tf.has_finally) {
                        chunk.code.resize(start);
                        return false;
                    }
            }
            /* `throw <expr>;` -> a native Throw op (P8 Inc 1). */
            if (const ThrowStmt *th = dynamic_cast<const ThrowStmt *>(s)) {
                if (try_native_throw(th, chunk.code)) {
                    any_native = true;
                    continue;
                }
            }
            /* `rethrow;` (in a catch body) -> a native Rethrow op (P8 Inc 2a). */
            if (const RethrowStmt *rt = dynamic_cast<const RethrowStmt *>(s)) {
                emit_rethrow(rt, chunk.code);
                any_native = true;
                continue;
            }

            /* A nested named func/struct decl (a scoped global - inside a
             * loop body it re-binds each iteration, as the tree-walker
             * re-evals the decl; a fresh FuncObject per iteration). This was
             * the last REAL-code whole-loop fallback (gen_stmt handled the
             * top-level/function-body decls; the loop/if body compiler did
             * not). */
            if (const FuncDeclStmt *fdd =
                    dynamic_cast<const FuncDeclStmt *>(s)) {
                if (fdd->id) {
                    emit_func_decl(fdd, chunk.code);
                    any_native = true;
                    continue;
                }
            }
            if (const StructDeclStmt *sdd =
                    dynamic_cast<const StructDeclStmt *>(s)) {
                if (sdd->id) {
                    emit_struct_decl(sdd, chunk.code);
                    any_native = true;
                    continue;
                }
            }

            /* The DISCARD tier (mirrors gen_stmt's tail): a bare leaf
             * (Identifier / scalar literal) statement is a deliberate no-op
             * (the tree-walker never RValue-s a discarded statement); any
             * OTHER expression statement compiles into a scratch temp and
             * drops the result - `map(f, a);` in a loop body, a discarded
             * ctor, `s[k];` - with the byte-identical throw carets. This is
             * the terminal tier: what it can't compile fails the whole
             * body (-> the enclosing statement's not-lowered abort). */
            {
                /* Same inert-leaf rule as gen_stmt's tail: always-bound ids /
                 * literals are no-ops; an unresolved or global id compiles
                 * (throw / defined-checked load), result discarded. */
                const Identifier *bid = dynamic_cast<const Identifier *>(s);
                const bool inert =
                    dynamic_cast<const Literal *>(s)
                    || (bid && bid->sym.kind != SymKind::unresolved
                            && bid->sym.kind != SymKind::global);
                if (inert)
                    continue;
                const size_t emark = chunk.code.size();
                const int st2 = next_temp;
                int dst;
                if (compile_boxed_expr(s, dst, chunk.code)) {
                    any_native = true;
                    continue;
                }
                chunk.code.resize(emark);
                next_temp = st2;
            }

            chunk.code.resize(start);
            return false;
        }
        /* (The old "all-fallback loop body isn't worth a native loop" gate is
         * GONE with the fallback ops: every statement lowers, so any_native
         * is bookkeeping only, and an EMPTY body - `while (cond);`,
         * `foreach (...) { }` - compiles to zero body ops, correctly.) */
        (void)any_native;
        (void)is_loop_body;
        (void)start;
        return true;
    }

    /*
     * An `if` inside a native body -> jumps, with native then/else. The
     * condition is a native int/float compare (JumpUnless*Cmp -> Lelse when
     * false) when it is one, else a boxed JumpUnlessTrueV - so `if (flag)` /
     * `if (a[i])` work too. An uncompilable condition fails the whole `if`
     * (no per-cond fallback op). Self-truncating like the loops.
     */
    bool compile_native_if(const IfStmt *f)
    {
        const size_t start = chunk.code.size();

        size_t jf;                    /* the conditional jump, patched to Lelse */
        Operand ca, cb;
        Op cmp;
        reset_temps();
        if (compile_int_cond(f->condExpr.get(), chunk.code, ca, cmp, cb)) {
            jf = emit_cmp(OpCode::JumpUnlessIntCmp, f->condExpr.get(),
                          cmp, ca, cb);
        } else {
            chunk.code.resize(start);
            reset_temps();
            if (compile_float_cond(f->condExpr.get(), chunk.code, ca, cmp, cb)) {
                jf = emit_cmp(OpCode::JumpUnlessFloatCmp, f->condExpr.get(),
                              cmp, ca, cb);
            } else {
                chunk.code.resize(start);
                reset_temps();
                int cslot;
                /* BOXED condition -> compute a bool slot + branch-unless-
                 * true; the last resort before failing the whole `if`. */
                if (compile_boxed_expr(f->condExpr.get(), cslot, chunk.code)) {
                    Instr in;
                    in.op = OpCode::JumpUnlessTrueV;
                    in.node_idx = add_ast_node(f->condExpr.get());
                    in.target2 = cslot;
                    jf = chunk.code.size();
                    chunk.code.push_back(in);
                } else {
                    /* an uncompilable cond: fail the whole `if` (-> the
                     * statement-level NotLoweredEx; must be unreachable
                     * under the no-fail contract). */
                    chunk.code.resize(start);
                    return false;
                }
            }
        }

        if (f->thenBlock
            && !compile_scalar_body(body_stmts(f->thenBlock.get()), false)) {
            chunk.code.resize(start);
            return false;
        }

        if (f->elseBlock) {
            const size_t j = emit(OpCode::Jump);
            chunk.code[jf].target = here();          /* Lelse */
            if (!compile_scalar_body(body_stmts(f->elseBlock.get()), false)) {
                chunk.code.resize(start);
                return false;
            }
            chunk.code[j].target = here();           /* Lend */
        } else {
            chunk.code[jf].target = here();          /* Lend */
        }
        return true;
    }

    /* P8 Inc 0: true if `c` contains a break/continue/return/rethrow that could
     * cross a try boundary (leaking a handler); flow-crossing-try lowering
     * (Inc 2c) handles those natively now. Conservative: flags a
     * break/continue even inside a NESTED loop in the body (safe, rare in a try
     * body). Does NOT descend into a nested function (its flow is local). */
    /*
     * P8 Inc 0: lower a `try {} catch (A) {} catch (B as e) {}` (NO finally, NO
     * rethrow/flow-escape, resolved catch vars) to a native handler region.
     * `throw` + runtime errors inside still C++-throw and are caught by
     * vm_run_chunk's boundary, which routes them to the pushed handler. Layout:
     *   PushHandler catch=Lcatch      ; active try region
     *   <try body> ; PopHandler ; Jump Lend        ; normal exit
     *   Lcatch:  CatchTest A -> Lca   ; boundary lands here (vm_exc set)
     *            CatchTest B -> Lcb ; Reraise       ; no clause matched
     *   Lca: <catch A> ; Jump Lend
     *   Lcb: <catch B> ; Jump Lend
     *   Lend:
     */
    /* Emit SetPend <p> (Inc 2b): the pending action a following finally runs. */
    void emit_setpend(Pend p)
    {
        Instr in;
        in.op = OpCode::SetPend;
        in.target = static_cast<int>(p);
        chunk.code.push_back(in);
    }

    bool compile_native_try(const TryCatchStmt *t)
    {
        const bool has_fin = t->finallyBody != nullptr;

        /* A resolved (frame-slot) catch var only; the REPL's map var falls back. */
        for (const auto &cs : t->catchStmts) {
            const Identifier *asId = cs.first.asId.get();
            if (asId && asId->sym.kind != SymKind::local)
                return false;
        }
        const size_t start = chunk.code.size();
        const size_t ct_start = chunk.catch_types.size();
        const size_t trys_depth = trys.size();

        /* Jumps to the finally block / to Lend, backpatched at the end. With a
         * finally, every exit path sets the pending action + jumps to Lfin
         * (EndFinally resumes it); without, exits jump straight to Lend. */
        std::vector<size_t> to_fin, to_end;
        auto exit_to = [&](Pend p) {
            if (has_fin) {
                emit_setpend(p);
                to_fin.push_back(emit(OpCode::Jump));
            } else if (p == Pend::reraise) {
                emit(OpCode::Reraise);
            } else {
                to_end.push_back(emit(OpCode::Jump));
            }
        };
        auto bail = [&]() {
            chunk.code.resize(start);
            chunk.catch_types.resize(ct_start);
            trys.resize(trys_depth);
            return false;
        };

        const size_t ph = emit(OpCode::PushHandler);   /* target=catch_pc (patch) */
        /* Register this try so a flow op in the body/catch (Inc 2c) can inline
         * this finally + pop this handler. `in_catch` starts false (the body's
         * handler is live); flipped true before the catch bodies. */
        trys.push_back({ has_fin, has_fin ? &to_fin : nullptr, false,
                         t->finallyBody.get() });
        if (!compile_scalar_body(body_stmts(t->tryBody.get()), false))
            return bail();
        emit(OpCode::PopHandler);
        exit_to(Pend::normal);                         /* normal try exit */

        chunk.code[ph].target = static_cast<int>(here());   /* Lcatch */

        std::vector<size_t> tests;
        for (const auto &cs : t->catchStmts) {
            int types_idx = -1;
            if (cs.first.exList) {
                std::vector<std::string> names;
                for (const auto &id : cs.first.exList->elems)
                    names.push_back(std::string(id->get_str()));
                types_idx = static_cast<int>(chunk.catch_types.size());
                chunk.catch_types.push_back(std::move(names));
            }
            const Identifier *asId = cs.first.asId.get();
            Instr in;
            in.op = OpCode::CatchTest;
            in.set_a(int_lit(types_idx));
            in.target2 = asId ? asId->sym.slot : -1;   /* bind slot / -1 */
            tests.push_back(chunk.code.size());         /* patch .target below */
            chunk.code.push_back(in);
        }
        exit_to(Pend::reraise);                        /* no clause matched */

        /* Catch bodies run with THIS try's handler already popped (the
         * dispatch popped it), so a return here must not re-pop it. */
        trys.back().in_catch = true;
        size_t ci = 0;
        for (const auto &cs : t->catchStmts) {
            chunk.code[tests[ci]].target = static_cast<int>(here());  /* body_pc */
            if (!compile_scalar_body(body_stmts(cs.second.get()), false))
                return bail();
            exit_to(Pend::normal);                     /* catch body done */
            ci++;
        }
        trys.pop_back();                               /* body + catches done */

        if (has_fin) {
            /* The SHARED finally block, reached by the NORMAL and RERAISE exits
             * only (flow ops inline their own copy - Inc 2c). */
            const int lfin = static_cast<int>(here());
            for (size_t j : to_fin)
                chunk.code[j].target = lfin;
            if (!compile_scalar_body(body_stmts(t->finallyBody.get()), false))
                return bail();
            emit(OpCode::EndFinally);
        }

        const int lend = static_cast<int>(here());
        for (size_t j : to_end)
            chunk.code[j].target = lend;
        return true;
    }

    /* Push a JumpUnless{Int,Float}Cmp and return its index (for backpatching). */
    size_t emit_cmp(OpCode opc, const Construct *node, Op cmp,
                    const Operand &a, const Operand &b)
    {
        Instr t;
        t.op = opc;
        t.node_idx = add_ast_node(node);
        t.aop = cmp;
        t.set_a(a);
        t.set_b(b);
        const size_t at = chunk.code.size();
        chunk.code.push_back(t);
        return at;
    }

    /*
     * A resolved-local scalar (int OR float) loop -> native register ops emitted
     * DIRECTLY into chunk.code:
     *   Lstart: <cond ops> JumpUnless{Int,Float}Cmp{a,cmp,b -> Lend}
     *           <body ops> Jump Lstart ; Lend:
     * Fires when the condition is an int/float comparison and every body
     * statement compiles (scalar / nested loop / if). Self-truncating: any
     * failure resizes chunk.code back to the start and returns false (-> the
     * enclosing statement's NotLoweredEx, or the enclosing body falls back).
     */
    bool try_native_scalar_while(const WhileStmt *w)
    {
        const size_t start = chunk.code.size();
        const int lstart = here();

        /* The condition -> native compare-branch(es) to the exit; a compound
         * `A && B` becomes one branch per conjunct (see emit_cond_jumps). */
        std::vector<size_t> exit_jumps;
        if (!emit_cond_jumps(w->condExpr.get(), exit_jumps)) {
            chunk.code.resize(start);
            return false;
        }

        push_loop();
        if (!compile_scalar_body(body_stmts(w->body.get()))) {
            loops.pop_back();
            chunk.code.resize(start);
            return false;
        }

        emit(OpCode::Jump, nullptr, lstart);
        const int lend = here();
        for (size_t j : exit_jumps)
            chunk.code[j].target = lend;
        pop_loop(lend, lstart);   /* continue -> the cond re-test (Lstart) */
        return true;
    }

    /*
     * A counted int for-loop (ForRangeStmt) -> native register ops emitted
     * directly into chunk.code. `init` runs ONCE as a fallback (it declares `i`
     * - a frame slot - so running it in place writes the same slot);
     * `bound`/`step` must be simple int operands (a slot or literal, both
     * loop-immutable, so reading them each iteration matches the once-evaluated
     * tree-walker); the body compiles like a while body (scalar / nested / if).
     * The counter uses the FUSED ForLoopStep back-edge (one dispatch = i +=
     * step + test + branch). Self-truncating on failure.
     */
    bool try_native_for_range(const ForRangeStmt *f)
    {
        const size_t start = chunk.code.size();
        const int saved_base = temp_base;

        /* `init` FIRST (`var i = start`, once): ForRangeStmt::do_eval's order
         * is init -> bound -> step, and the init may have SIDE EFFECTS a
         * non-trivial bound reads (`for (var i = drop(x); i < len(x); i++)`
         * with drop popping x) - compiling the bound first evaluated it
         * BEFORE the init, a real wrong-result -vm divergence (pinned by the
         * "for-range evaluates init before the bound" test). */
        if (!emit_init(f->init.get())) {
            chunk.code.resize(start);
            temp_base = saved_base;
            return false;
        }

        /* The bound + step are loop-immutable (the for-range specializer proved
         * it), so evaluate ONCE - after the init. A simple int operand (a slot
         * or immediate) is used directly; a non-trivial bound OR step -
         * `len(s)`/`f()`/an arith chain/a subscript read - compiles into a
         * temp reserved for the whole loop, which ForLoopStep re-reads each
         * iteration (its value is fixed). This is what lets a
         * `for (i; i < len(x); i++)` counted loop (and a non-operand step
         * `i += st[0]`) go native instead of falling back to node->eval. */
        Operand bound;
        if (!as_int_operand(f->bound.get(), bound)) {
            reset_temps();
            int bslot;
            if (!compile_boxed_expr(f->bound.get(), bslot, chunk.code)) {
                chunk.code.resize(start);
                temp_base = saved_base;
                return false;
            }
            bound = slot_op(bslot);
            temp_base = next_temp;   /* reserve the bound temp */
        }
        Operand step;
        if (f->step) {
            if (!as_int_operand(f->step.get(), step)) {
                reset_temps();
                int sslot;
                if (!compile_boxed_expr(f->step.get(), sslot, chunk.code)) {
                    chunk.code.resize(start);
                    temp_base = saved_base;
                    return false;
                }
                step = slot_op(sslot);
                temp_base = next_temp;   /* reserve the step temp too */
            }
        } else {
            step = int_lit(1);
        }

        const Operand ci = slot_op(f->i_slot);

        /* Initial test: skip the loop entirely if !(i <cmp> bound). */
        const size_t jt =
            emit_cmp(OpCode::JumpUnlessIntCmp, f, f->cmp_op, ci, bound);

        const int lbody = here();
        push_loop();
        if (!compile_scalar_body(body_stmts(f->body.get()))) {
            loops.pop_back();
            chunk.code.resize(start);
            temp_base = saved_base;
            return false;
        }

        const int lcont = here();   /* continue -> the fused step (i+=; test) */

        /* Fused back-edge: i += step; if (i <cmp> bound) goto lbody. */
        Instr fstep;
        fstep.op = OpCode::ForLoopStep;
        fstep.node_idx = add_ast_node(f);
        fstep.aop = f->cmp_op;
        fstep.target = lbody;
        fstep.target2 = f->i_slot;
        fstep.set_a(bound);
        fstep.set_b(step);
        chunk.code.push_back(fstep);

        const int lend = here();
        chunk.code[jt].target = lend;
        pop_loop(lend, lcont);
        temp_base = saved_base;
        return true;
    }

    /*
     * A general `for (init; cond; inc) body` that specialize_types did NOT turn
     * into a ForRangeStmt (its cond isn't the counted `i </<= bound` shape -
     * e.g. `f*f <= n`) -> native register ops, lowered to the WHILE form:
     *   <init once>  Lstart: <cond> JmpUnlessCmp -> Lend ; <body> ; <inc> ;
     *   Jump Lstart ; Lend:
     * The cond re-runs each iteration; the inc runs after the body. Fires when
     * the cond compiles (int/float compare, split `&&`, or a boxed truthiness
     * test) OR is ABSENT (`for (;;)` - no exit branch; it leaves via a native
     * break/return/throw, or genuinely runs forever, like the tree-walker),
     * the body compiles (scalar / nested / the discard tier), and the inc
     * is a compilable int/float/boxed statement.
     * Self-truncating. The loop var must be a frame slot (so running init/inc
     * in place is sound, no child scope) - the operand compilers enforce that.
     */
    bool try_native_for(const ForStmt *f)
    {
        const size_t start = chunk.code.size();

        if (f->init && !emit_init(f->init.get())) { /* declare the var, once */
            chunk.code.resize(start);
            return false;
        }

        const int lstart = here();

        /* The condition -> native compare-branch(es) to the exit, sharing the
         * while path's helper: an int/float compare, a split `A && B`, or a
         * boxed truthiness test (a bool var / `||` / dyn). Bails the loop only
         * if even the boxed path can't compile the cond. NO cond (`for (;;)`)
         * = an unconditional loop: no exit branch at all - it leaves via a
         * break / return / throw in the body (or genuinely runs forever),
         * exactly like the tree-walker's missing-cond ForStmt. */
        std::vector<size_t> exit_jumps;
        if (f->cond && !emit_cond_jumps(f->cond.get(), exit_jumps)) {
            chunk.code.resize(start);
            return false;
        }

        push_loop();
        if (!compile_scalar_body(body_stmts(f->body.get()))) {
            loops.pop_back();
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();   /* continue -> the inc, then re-test */

        /* The increment, each iteration after the body (int, float, or a
         * BOXED statement - a dyn `d++`, a string compound `s += "x"`: the
         * same three-tier dispatch every statement gets). */
        if (f->inc) {
            reset_temps();
            const size_t imark = chunk.code.size();
            if (!compile_int_stmt(f->inc.get(), chunk.code)) {
                chunk.code.resize(imark);
                reset_temps();
                if (!compile_float_stmt(f->inc.get(), chunk.code)) {
                    chunk.code.resize(imark);
                    reset_temps();
                    if (!compile_boxed_stmt(f->inc.get(), chunk.code)) {
                        loops.pop_back();
                        chunk.code.resize(start);
                        return false;
                    }
                }
            }
        }

        emit(OpCode::Jump, nullptr, lstart);
        const int lend = here();
        for (size_t j : exit_jumps)
            chunk.code[j].target = lend;
        pop_loop(lend, lcont);
        return true;
    }

    /*
     * Native foreach over a flat int/float array with a single, non-indexed
     * loop var (ForeachStmt::elem_th, set by the inferencer - the only sound
     * case; a dict / string / general / tuple / indexed foreach stays the
     * tree-walker fallback). Lowered as a counted loop: snapshot the container
     * once, n = its length, then each step does `x = c[i]` (a direct flat
     * element load into the loop var) + the native body + i++. break / continue
     * / return use the same FlowState + loop backpatching as try_native_for.
     */
    bool try_native_foreach(const ForeachStmt *fe)
    {
        /* Single, non-indexed loop var over a proven array. elem_th i/f is the
         * flat-scalar fast path (LoadElemInt/Float); any other element type
         * (general/str/array/dict/dyn) goes native via LoadElemValue, which
         * binds the element's EvalValue into the loop var - box-free (no
         * unbox), matching the tree-walker's general `elem = view[i].get()`. */
        if (!fe->container_is_array)
            return false;

        /* For an INDEXED foreach (ids = [index, elem]) the index var IS the
         * loop counter (the body reads it as the index) and the element goes
         * into ids[1]; a single-var foreach uses a temp counter and puts the
         * element in ids[0]. */
        const int elem_id = fe->indexed ? 1 : 0;
        const Identifier *id =
            dynamic_cast<const Identifier *>(fe->ids->elems[elem_id].get());
        if (!id || id->sym.kind != SymKind::local)
            return false;
        const int x_slot = id->sym.slot;
        int idx_slot = -1;
        if (fe->indexed) {
            const Identifier *ix =
                dynamic_cast<const Identifier *>(fe->ids->elems[0].get());
            if (!ix || ix->sym.kind != SymKind::local)
                return false;
            idx_slot = ix->sym.slot;
        }

        const size_t start = chunk.code.size();
        reset_temps();

        /* Snapshot the container into a temp: the tree-walker evals it ONCE
         * before the loop, so a body reassignment of the container var must not
         * change what we iterate. */
        int csrc;
        if (!compile_boxed_expr(fe->container.get(), csrc, chunk.code)) {
            chunk.code.resize(start);
            return false;
        }
        const int c = alloc_temp();
        Instr mv;
        mv.op = OpCode::MoveV;
        mv.target = c;
        mv.target2 = csrc;
        chunk.code.push_back(mv);

        const int n = alloc_temp();
        Instr ln;
        ln.op = OpCode::ArrLen;
        ln.node_idx = add_ast_node(fe->container.get());
        ln.target = n;
        ln.target2 = c;
        chunk.code.push_back(ln);

        /* The counter: for an indexed foreach it IS the index var (ids[0], read
         * by the body); otherwise a fresh temp. Either way it starts at 0 and
         * the ForLoopStep increments it. */
        const int i = fe->indexed ? idx_slot : alloc_temp();
        Instr z;
        z.op = OpCode::LoadImmInt;
        z.target = i;
        z.set_a(int_lit(0));
        chunk.code.push_back(z);

        /* Reserve c/n/i for the whole loop - a body statement's reset_temps()
         * must not reuse their slots. */
        const int saved_base = temp_base;
        temp_base = next_temp;

        /* Initial test: skip the loop entirely for an empty array. */
        const size_t jt = emit_cmp(OpCode::JumpUnlessIntCmp,
                                   fe->container.get(), Op::lt,
                                   slot_op(i), slot_op(n));

        const int lbody = here();

        /* x = c[i] : a direct flat int/float element load into the loop var,
         * re-run at the top of every iteration (the ForLoopStep re-enters
         * here). */
        Instr ld;
        ld.op = fe->elem_th == TypeHint::i ? OpCode::LoadElemInt
              : fe->elem_th == TypeHint::f ? OpCode::LoadElemFloat
              : fe->elem_is_bool           ? OpCode::LoadElemBool
                                           : OpCode::LoadElemValue;
        ld.node_idx = add_ast_node(fe->container.get());
        ld.target = x_slot;
        ld.target2 = c;
        ld.set_a(slot_op(i));
        chunk.code.push_back(ld);

        push_loop();
        if (!compile_scalar_body(body_stmts(fe->body.get()))) {
            loops.pop_back();
            temp_base = saved_base;
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();   /* continue -> the fused step */

        /* Fused back-edge: i += 1; if (i < n) goto lbody (the same
         * superinstruction the native for-range uses - one dispatch per
         * iteration instead of a separate compare + increment + jump). */
        Instr fstep;
        fstep.op = OpCode::ForLoopStep;
        fstep.node_idx = add_ast_node(fe->container.get());
        fstep.aop = Op::lt;
        fstep.target = lbody;
        fstep.target2 = i;
        fstep.set_a(slot_op(n));
        fstep.set_b(int_lit(1));
        chunk.code.push_back(fstep);

        const int lend = here();
        chunk.code[jt].target = lend;
        pop_loop(lend, lcont);
        temp_base = saved_base;
        return true;
    }

    /*
     * Native foreach over a proven STRING (container_is_str): a counted loop
     * over the char count (StrLen bound), each iteration binding char i as a
     * fresh 1-char string (LoadStrChar) into the loop var - the string analogue
     * of the general-array path (LoadElemValue). Single-var puts the char in
     * ids[0]; an indexed 2-var uses ids[0] as the counter/index and ids[1] as
     * the char. Mirrors try_native_foreach exactly bar the two ops. Neither
     * StrLen nor LoadStrChar can throw (i is loop-bounded), so no loc/node.
     */
    bool try_native_foreach_str(const ForeachStmt *fe)
    {
        if (!fe->container_is_str)
            return false;

        const int elem_id = fe->indexed ? 1 : 0;
        const Identifier *id =
            dynamic_cast<const Identifier *>(fe->ids->elems[elem_id].get());
        if (!id || id->sym.kind != SymKind::local)
            return false;
        const int x_slot = id->sym.slot;
        int idx_slot = -1;
        if (fe->indexed) {
            const Identifier *ix =
                dynamic_cast<const Identifier *>(fe->ids->elems[0].get());
            if (!ix || ix->sym.kind != SymKind::local)
                return false;
            idx_slot = ix->sym.slot;
        }

        const size_t start = chunk.code.size();
        reset_temps();

        int csrc;
        if (!compile_boxed_expr(fe->container.get(), csrc, chunk.code)) {
            chunk.code.resize(start);
            return false;
        }
        const int c = alloc_temp();
        Instr mv;
        mv.op = OpCode::MoveV;
        mv.target = c;
        mv.target2 = csrc;
        chunk.code.push_back(mv);

        const int n = alloc_temp();
        Instr ln;
        ln.op = OpCode::StrLen;
        ln.target = n;
        ln.target2 = c;
        chunk.code.push_back(ln);

        const int i = fe->indexed ? idx_slot : alloc_temp();
        Instr z;
        z.op = OpCode::LoadImmInt;
        z.target = i;
        z.set_a(int_lit(0));
        chunk.code.push_back(z);

        const int saved_base = temp_base;
        temp_base = next_temp;

        const size_t jt = emit_cmp(OpCode::JumpUnlessIntCmp,
                                   fe->container.get(), Op::lt,
                                   slot_op(i), slot_op(n));

        const int lbody = here();

        Instr ld;
        ld.op = OpCode::LoadStrChar;
        ld.target = x_slot;
        ld.target2 = c;
        ld.set_a(slot_op(i));
        chunk.code.push_back(ld);

        push_loop();
        if (!compile_scalar_body(body_stmts(fe->body.get()))) {
            loops.pop_back();
            temp_base = saved_base;
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();

        Instr fstep;
        fstep.op = OpCode::ForLoopStep;
        fstep.node_idx = add_ast_node(fe->container.get());
        fstep.aop = Op::lt;
        fstep.target = lbody;
        fstep.target2 = i;
        fstep.set_a(slot_op(n));
        fstep.set_b(int_lit(1));
        chunk.code.push_back(fstep);

        const int lend = here();
        chunk.code[jt].target = lend;
        pop_loop(lend, lcont);
        temp_base = saved_base;
        return true;
    }

    /*
     * Native foreach over a flat array<PodStruct> whose body reads the loop var
     * ONLY as scalar-field reads (struct_fe_body_ok). The loop var is NEVER
     * materialized: the counted loop over the array runs, and each `p.field` in
     * the body compiles to a DIRECT byte read of the array element
     * (LoadStructField*, via the sfe mapping), skipping the per-iteration
     * StructObject + memcpy the tree-walker's reused-object foreach pays. Any
     * whole-`p` use or a `p.field` write makes struct_fe_body_ok fail -> the
     * tree-walker fallback.
     */
    bool try_native_struct_foreach(const ForeachStmt *fe)
    {
        if (!fe->container_struct_def || fe->indexed || !fe->ids
            || fe->ids->elems.size() != 1)
            return false;
        const Identifier *id =
            dynamic_cast<const Identifier *>(fe->ids->elems[0].get());
        if (!id || id->sym.kind != SymKind::local)
            return false;
        const int x_slot = id->sym.slot;
        /* If every loop-var use is a SCALAR-FIELD read (p.x), take the DIRECT
         * read path (p never materialized). Otherwise the body uses `p` as a
         * whole value (print(p), append(o, p), q = p, p.a.x nested), so
         * MATERIALIZE a fresh StructObject per iteration (LoadStructElemV) into
         * `p`'s slot and compile the body normally. */
        const bool whole_p = !struct_fe_body_ok(fe->body.get(), x_slot,
                                                fe->container_struct_def, false);

        const size_t start = chunk.code.size();
        reset_temps();

        int csrc;
        if (!compile_boxed_expr(fe->container.get(), csrc, chunk.code)) {
            chunk.code.resize(start);
            return false;
        }
        const int c = alloc_temp();
        Instr mv;
        mv.op = OpCode::MoveV;
        mv.target = c;
        mv.target2 = csrc;
        chunk.code.push_back(mv);

        const int n = alloc_temp();
        Instr ln;
        ln.op = OpCode::ArrLen;
        ln.node_idx = add_ast_node(fe->container.get());
        ln.target = n;
        ln.target2 = c;
        chunk.code.push_back(ln);

        const int i = alloc_temp();
        Instr z;
        z.op = OpCode::LoadImmInt;
        z.target = i;
        z.set_a(int_lit(0));
        chunk.code.push_back(z);

        const int saved_base = temp_base;
        temp_base = next_temp;

        const size_t jt = emit_cmp(OpCode::JumpUnlessIntCmp,
                                   fe->container.get(), Op::lt,
                                   slot_op(i), slot_op(n));
        const int lbody = here();

        if (whole_p) {
            /* Materialize p = c[i] (a fresh StructObject) at the top of each
             * iteration; the body reads p's slot normally (no sfe mapping). */
            Instr ld;
            ld.op = OpCode::LoadStructElemV;
            ld.target = x_slot;
            ld.target2 = c;
            ld.set_a(slot_op(i));
            chunk.code.push_back(ld);
        } else {
            /* No element load - p is never materialized. Activate the direct-
             * read mapping so a p.field read -> LoadStructField*(c[i].fld). */
            sfe_loop_slot = x_slot;
            sfe_arr_slot = c;
            sfe_ctr_slot = i;
            sfe_def = fe->container_struct_def;
        }

        push_loop();
        const bool body_ok = compile_scalar_body(body_stmts(fe->body.get()));

        sfe_loop_slot = -1;   /* deactivate (success AND failure path) */
        sfe_arr_slot = sfe_ctr_slot = -1;
        sfe_def = nullptr;

        if (!body_ok) {
            loops.pop_back();
            temp_base = saved_base;
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();
        Instr fstep;
        fstep.op = OpCode::ForLoopStep;
        fstep.node_idx = add_ast_node(fe->container.get());
        fstep.aop = Op::lt;
        fstep.target = lbody;
        fstep.target2 = i;
        fstep.set_a(slot_op(n));
        fstep.set_b(int_lit(1));
        chunk.code.push_back(fstep);

        const int lend = here();
        chunk.code[jt].target = lend;
        pop_loop(lend, lcont);
        temp_base = saved_base;
        return true;
    }

    /*
     * Native STRICT foreach-unpack: `foreach (x, y in pairs)` over a proven
     * array<array<int/float>>. Same counted loop over the OUTER array as
     * try_native_foreach, but each element is a flat sub-array destructured
     * BOX-FREE into the consecutive loop-var slots (UnpackElemInt/Float), with
     * the strict size check. A `_`, non-consecutive slots, or a general/opt
     * sub-array (unpack_elem_th none) fall back to the tree-walker's do_iter.
     */
    bool try_native_foreach_unpack(const ForeachStmt *fe)
    {
        const bool flat = fe->unpack_elem_th != TypeHint::none;
        if ((!flat && !fe->unpack_elem_value) || !fe->ids)
            return false;

        /* All loop vars must be real (non-`_`), resolved-local, CONSECUTIVE
         * slots from `base` (guaranteed for `var x, y` - declared in order;
         * verified). For an INDEXED loop, ids[0] is the index var (= the loop
         * counter, read by the body) and ids[1..] are the unpack targets; the
         * unpack width is N - 1 and must be >= 2. Non-indexed: all N are unpack
         * targets, width N, counter is a temp. */
        const int N = static_cast<int>(fe->ids->elems.size());
        const int nunpack = N - (fe->indexed ? 1 : 0);
        if (nunpack < 2)
            return false;
        /* Build the per-unpack-position target slots (-1 for a `_` placeholder).
         * `base` is ids[0]'s slot (the counter for an indexed loop). A `_` or a
         * non-consecutive layout switches to UnpackElemTargets (a targets pool);
         * the all-real consecutive case keeps the tuned UnpackElem run. */
        int base = -1;
        const int first = fe->indexed ? 1 : 0;
        std::vector<int32_t> targets;
        bool consecutive = true;
        for (int k = 0; k < N; k++) {
            const Identifier *id =
                dynamic_cast<const Identifier *>(fe->ids->elems[k].get());
            if (!id)
                return false;
            if (k < first) {                 /* the index var: real local */
                if (id->is_underscore() || id->sym.kind != SymKind::local)
                    return false;
                base = id->sym.slot;
                continue;
            }
            const int pos = static_cast<int>(targets.size());
            if (id->is_underscore()) {
                targets.push_back(-1);
                consecutive = false;
                continue;
            }
            if (id->sym.kind != SymKind::local)
                return false;
            targets.push_back(id->sym.slot);
            if (pos == 0) {
                if (first == 0) base = id->sym.slot;
            } else if (id->sym.slot != targets[0] + pos) {
                consecutive = false;
            }
        }
        const int unpack_base = consecutive ? targets[0] : -1;

        const size_t start = chunk.code.size();
        reset_temps();

        int csrc;
        if (!compile_boxed_expr(fe->container.get(), csrc, chunk.code)) {
            chunk.code.resize(start);
            return false;
        }
        const int c = alloc_temp();
        Instr mv;
        mv.op = OpCode::MoveV;
        mv.target = c;
        mv.target2 = csrc;
        chunk.code.push_back(mv);

        const int n = alloc_temp();
        Instr ln;
        ln.op = OpCode::ArrLen;
        ln.node_idx = add_ast_node(fe->container.get());
        ln.target = n;
        ln.target2 = c;
        chunk.code.push_back(ln);

        /* The counter: for an indexed loop it IS the index var (base, read by
         * the body); otherwise a fresh temp. */
        const int i = fe->indexed ? base : alloc_temp();
        Instr z;
        z.op = OpCode::LoadImmInt;
        z.target = i;
        z.set_a(int_lit(0));
        chunk.code.push_back(z);

        const int saved_base = temp_base;
        temp_base = next_temp;   /* reserve c/n/(i) */

        const size_t jt = emit_cmp(OpCode::JumpUnlessIntCmp,
                                   fe->container.get(), Op::lt,
                                   slot_op(i), slot_op(n));

        const int lbody = here();

        /* Per element: read pairs[i] (a sub-array), strict-check its length ==
         * nunpack, and write its scalars into unpack_base..+nunpack-1 - box-free
         * raw for a flat int/float sub-array (UnpackElemInt/Float), else each
         * element's boxed value (UnpackElemValue, for a general/dyn/str/mixed
         * sub-array like shopping's [str, float]). */
        Instr up;
        if (consecutive) {
            up.op = fe->unpack_elem_th == TypeHint::i ? OpCode::UnpackElemInt
                  : fe->unpack_elem_th == TypeHint::f ? OpCode::UnpackElemFloat
                                                      : OpCode::UnpackElemValue;
            up.target = unpack_base;
        } else {
            /* A `_` (or non-consecutive layout): per-position targets pool,
             * each element bound box-free (vm_arr_elem, flat or general). */
            up.op = OpCode::UnpackElemTargets;
            up.target = static_cast<int>(chunk.unpack_targets.size());
            chunk.unpack_targets.push_back(targets);
        }
        up.node_idx = add_ast_node(fe->container.get());
        up.target2 = c;
        up.set_a(slot_op(i));
        up.set_b(int_lit(nunpack));
        chunk.code.push_back(up);

        push_loop();
        if (!compile_scalar_body(body_stmts(fe->body.get()))) {
            loops.pop_back();
            temp_base = saved_base;
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();
        Instr fstep;
        fstep.op = OpCode::ForLoopStep;
        fstep.node_idx = add_ast_node(fe->container.get());
        fstep.aop = Op::lt;
        fstep.target = lbody;
        fstep.target2 = i;
        fstep.set_a(slot_op(n));
        fstep.set_b(int_lit(1));
        chunk.code.push_back(fstep);

        const int lend = here();
        chunk.code[jt].target = lend;
        pop_loop(lend, lcont);
        temp_base = saved_base;
        return true;
    }

    /*
     * Native dict `foreach` via a LIVE iterator (DictIterInit/DictIterNext) - a
     * dict has no O(1) index, so it's a while-shaped loop, not the counted
     * array one. Non-indexed: 1 var binds the key (keys-only), 2 vars key +
     * value. INDEXED (`foreach (i, k[, v] in indexed d)`): ids[0] is the index
     * counter (a plain int local, init 0, `+= 1` each continue), and the
     * key/value follow it - matching do_iter's `id_start`/count==2 binding. A
     * `_` in a key/value position binds nothing (slot -1). Break/continue/
     * return flow through the same FlowState machinery as the array foreach.
     */
    bool try_native_dict_foreach(const ForeachStmt *fe)
    {
        if (!fe->container_is_dict || !fe->ids)
            return false;
        const int nvars = static_cast<int>(fe->ids->elems.size());
        /* index offset: an indexed loop's ids[0] is the counter, key/value
         * follow. So valid var counts are 1/2 (non-indexed) or 2/3 (indexed). */
        const int off = fe->indexed ? 1 : 0;
        const int nkv = nvars - off;   /* key[+value] target count */
        if (nkv != 1 && nkv != 2)
            return false;

        /* Resolve the key/value target slots. `_` -> -1 (bind nothing); any
         * non-local target -> bail (leave to the tree-walker). */
        auto slot_of = [&](size_t i, int &out) -> bool {
            const Identifier *id =
                dynamic_cast<const Identifier *>(fe->ids->elems[i].get());
            if (!id)
                return false;
            if (id->is_underscore()) { out = -1; return true; }
            if (id->sym.kind != SymKind::local)
                return false;
            out = id->sym.slot;
            return true;
        };
        /* The index counter must be a real (non-`_`) int local. */
        int idx_slot = -1;
        if (off) {
            const Identifier *id0 =
                dynamic_cast<const Identifier *>(fe->ids->elems[0].get());
            if (!id0 || id0->is_underscore()
                || id0->sym.kind != SymKind::local)
                return false;
            idx_slot = id0->sym.slot;
        }
        int k_slot = -1, v_slot = -1;
        if (!slot_of(off, k_slot))
            return false;
        if (nkv == 2 && !slot_of(off + 1, v_slot))
            return false;

        const size_t start = chunk.code.size();
        reset_temps();

        /* Compile the dict container into a slot; DictIterInit's intrusive_ptr
         * copy IS the once-eval snapshot (a body reassign of the container var
         * can't change what we iterate). */
        int dsrc;
        if (!compile_boxed_expr(fe->container.get(), dsrc, chunk.code)) {
            chunk.code.resize(start);
            return false;
        }
        const int saved_base = temp_base;
        temp_base = next_temp;      /* reserve dsrc for DictIterInit */

        /* Indexed: the counter starts at 0 (the first iteration's index),
         * incremented at each continue point (below) - so it holds the
         * iteration number during the body, byte-identical to do_iter. */
        if (off) {
            Instr z;
            z.op = OpCode::LoadImmInt;
            z.target = idx_slot;
            z.set_a(int_lit(0));
            chunk.code.push_back(z);
        }

        const int iter_id = alloc_dict_iter();

        Instr init;
        init.op = OpCode::DictIterInit;
        init.node_idx = add_ast_node(fe->container.get());
        init.target = iter_id;
        init.target2 = dsrc;
        chunk.code.push_back(init);

        const int lnext = here();   /* test + bind + advance */
        Instr nx;
        nx.op = OpCode::DictIterNext;
        nx.target2 = iter_id;
        nx.set_a(slot_op(k_slot));     /* -1 == `_`/keys-only-unused */
        nx.set_b(slot_op(v_slot));
        const size_t nx_i = chunk.code.size();
        chunk.code.push_back(nx);   /* .target (end_pc) backpatched below */

        push_loop();
        if (!compile_scalar_body(body_stmts(fe->body.get()))) {
            loops.pop_back();
            temp_base = saved_base;
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();   /* continue -> increment, back to Next */
        if (off) {
            /* index += 1 (idx_slot = idx_slot + 1), then loop. */
            Instr inc;
            inc.op = OpCode::IntBin;
            inc.aop = Op::plus;
            inc.target = idx_slot;
            inc.set_a(slot_op(idx_slot));
            inc.set_b(int_lit(1));
            chunk.code.push_back(inc);
        }
        Instr jb;
        jb.op = OpCode::Jump;
        jb.target = lnext;
        chunk.code.push_back(jb);

        const int lend = here();
        chunk.code[nx_i].target = lend;
        pop_loop(lend, lcont);
        temp_base = saved_base;
        return true;
    }

    /*
     * Native `foreach (e in <dyn container>)` / `foreach (k, v in <dyn>)` via a
     * runtime-dispatching live iterator (ForeachDynInit/ForeachDynNext): the
     * array-vs-dict choice is made at runtime, then (1-var) the loop var binds
     * the array element or the dict key box-free, or (2-var) an array element
     * is STRICT-unpacked into the two vars and a dict binds key+value - matching
     * do_iter. A `_` binds nothing (slot -1). Indexed / >2-var falls back.
     */
    bool try_native_dyn_foreach(const ForeachStmt *fe)
    {
        /* No container_is_dyn gate: ForeachDynInit/Next RUNTIME-dispatch
         * array|dict and mirror do_iter's bind rules for ANY id list (any
         * var count, `indexed`, `_`, dict none-padding), so this is the
         * UNIVERSAL foreach - tried LAST, after the specific fast forms, it
         * also covers the residual proven shapes (a lone `_`, a 1-var
         * indexed array, a >2-var dict with none-pad, ...). A non-iterable
         * container throws the tree-walker's TypeErrorEx from Init. */
        if (!fe->ids || fe->ids->elems.empty())
            return false;

        /* Collect the per-var frame slots (-1 == `_`) into an unpack_targets
         * pool entry - GENERAL over the id list: any var count, `indexed`
         * (targets[0] is the iteration counter), `_` placeholders. A non-local
         * var (global/capture - rare) falls back. */
        std::vector<int32_t> targets;
        targets.reserve(fe->ids->elems.size());
        for (const auto &el : fe->ids->elems) {
            const Identifier *id =
                dynamic_cast<const Identifier *>(el.get());
            if (!id)
                return false;
            if (id->is_underscore()) {
                targets.push_back(-1);
                continue;
            }
            if (id->sym.kind != SymKind::local)
                return false;
            targets.push_back(id->sym.slot);
        }
        const int nvars = static_cast<int>(targets.size());

        const size_t start = chunk.code.size();
        reset_temps();

        int dsrc;
        if (!compile_boxed_expr(fe->container.get(), dsrc, chunk.code)) {
            chunk.code.resize(start);
            return false;
        }
        const int saved_base = temp_base;
        temp_base = next_temp;      /* reserve dsrc for ForeachDynInit */

        const int iter_id = alloc_dyn_iter();

        const int tgt_idx = static_cast<int>(chunk.unpack_targets.size());
        chunk.unpack_targets.push_back(std::move(targets));

        Instr init;
        init.op = OpCode::ForeachDynInit;
        init.node_idx = add_ast_node(fe->container.get());   /* extract_locs -> the caret */
        init.target = iter_id;
        init.target2 = dsrc;
        init.set_a(int_lit(nvars | (fe->indexed ? 256 : 0)));
        init.set_b(int_lit(tgt_idx));         /* the per-var slots (-1 == `_`) */
        chunk.code.push_back(init);

        const int lnext = here();          /* test + bind + advance */
        Instr nx;
        nx.op = OpCode::ForeachDynNext;
        nx.target2 = iter_id;              /* targets ride the iterator state */
        /* A multi-var array element is strict-unpacked, so Next can throw;
         * record the container caret (do_iter uses container->start/end). */
        if (nvars - (fe->indexed ? 1 : 0) >= 2)
            nx.node_idx = add_ast_node(fe->container.get());
        const size_t nx_i = chunk.code.size();
        chunk.code.push_back(nx);          /* .target (end_pc) backpatched */

        push_loop();
        if (!compile_scalar_body(body_stmts(fe->body.get()))) {
            loops.pop_back();
            temp_base = saved_base;
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();
        Instr jb;
        jb.op = OpCode::Jump;
        jb.target = lnext;
        chunk.code.push_back(jb);

        const int lend = here();
        chunk.code[nx_i].target = lend;
        pop_loop(lend, lcont);
        temp_base = saved_base;
        return true;
    }

    void gen_stmts(const std::vector<unique_ptr<Construct>> &elems)
    {
        for (const auto &e : elems)
            gen_stmt(e.get());
    }

    void gen_stmt(const Construct *s)
    {
        /* Native-first: the register machine applies to top-level AND
         * function-body statements, not only loop bodies. A resolved-local
         * int/float scalar decl/assign/`++`/compound-assign lowers to register
         * ops; an `if` with a native compare condition + native branches lowers
         * to compares/jumps (compile_native_if); a loop tries its native
         * form; a return/call/expression statement its own op or the discard
         * tier. Each attempt is self-truncating (resets to `mark` on
         * failure); what nothing lowers is a NotLoweredEx compile abort.
         * This is what makes a scalar-arithmetic FUNCTION body native, not
         * only a loop body. */
        reset_temps();
        const size_t mark = chunk.code.size();
        if (compile_int_stmt(s, chunk.code))
            return;
        chunk.code.resize(mark);
        reset_temps();
        if (compile_float_stmt(s, chunk.code))
            return;
        chunk.code.resize(mark);
        reset_temps();
        if (compile_boxed_stmt(s, chunk.code))   /* dyn/string scalar assign */
            return;
        chunk.code.resize(mark);

        if (const IfStmt *f = dynamic_cast<const IfStmt *>(s)) {
            if (!compile_native_if(f))
                throw_not_lowered(s);
            return;
        }
        if (const WhileStmt *w = dynamic_cast<const WhileStmt *>(s)) {
            if (!try_native_scalar_while(w))
                throw_not_lowered(s);
            return;
        }
        if (const ForRangeStmt *fr = dynamic_cast<const ForRangeStmt *>(s)) {
            if (!try_native_for_range(fr))
                throw_not_lowered(s);
            return;
        }
        if (const ForStmt *fs = dynamic_cast<const ForStmt *>(s)) {
            if (!try_native_for(fs))           /* general (non-range) for */
                throw_not_lowered(s);
            return;
        }
        if (const ForeachStmt *fe = dynamic_cast<const ForeachStmt *>(s)) {
            if (!try_native_foreach(fe)         /* flat/general array */
                && !try_native_foreach_str(fe)  /* string chars */
                && !try_native_foreach_unpack(fe) /* strict array destructure */
                && !try_native_struct_foreach(fe) /* flat struct fields */
                && !try_native_dict_foreach(fe)   /* live dict iterator */
                && !try_native_dyn_foreach(fe))   /* runtime array|dict */
                throw_not_lowered(s);
            return;
        }
        /* A user-function call statement (result discarded) -> CallV. */
        if (const DirectCallExpr *dc =
                dynamic_cast<const DirectCallExpr *>(s)) {
            int dst;
            if (try_native_call(dc, dst, chunk.code))
                return;
        }
        /* A builtin call statement (result discarded) -> CallBuiltinV. */
        if (const DirectBuiltinCallExpr *bc =
                dynamic_cast<const DirectBuiltinCallExpr *>(s)) {
            int dst;
            if (try_native_builtin(bc, dst, chunk.code))
                return;
        }
        /* A func-VALUE call statement (a call through a Func-typed var/closure,
         * result discarded) -> CallValueV (F-3, phonebook's `cmdfunc(data)`).
         * try_native_value_call rejects a Direct{Call,BuiltinCall}Expr, so this
         * only catches a plain CallExpr the two handlers above didn't. */
        if (const CallExpr *call = dynamic_cast<const CallExpr *>(s)) {
            int dst;
            if (try_native_value_call(call, dst, chunk.code))
                return;
        }
        /* `return <expr>;` -> ReturnV (its expr compiled natively). */
        if (const ReturnStmt *ret = dynamic_cast<const ReturnStmt *>(s)) {
            if (try_native_return(ret, chunk.code))
                return;
        }
        /* A `func f(..) {..}` decl statement bound into a GLOBAL slot (a
         * hoisted top-level / scoped function) -> MakeClosureV (create the
         * FuncObject, snapshotting captures) + StoreGlobalV (write the slot +
         * mark defined) - byte-identical to FuncDeclStmt::do_eval's global-bind
         * `slots[slot] = LValue(func, false); defined = 1`. In a SCRIPT a
         * NAMED func decl ALWAYS has a global slot (top-level hoist or a
         * scoped global): the grammar rejects a capture list on a named func,
         * and the brace-less-body masked route was removed by pWrapDeclBody
         * (parser.cpp) - so a non-global named decl is provably impossible
         * here (the REPL, whose names ARE map-resident, never runs codegen);
         * ML_CHECK guards the invariant. */
        if (const FuncDeclStmt *fd = dynamic_cast<const FuncDeclStmt *>(s)) {
            if (fd->id) {
                emit_func_decl(fd, chunk.code);
                return;
            }
        }
        /* A `struct P {..}` decl bound into a GLOBAL slot (hoisted, like a func
         * name): bake the type descriptor (a trivial t_structtype value holding
         * the program-lifetime StructTypeDef*) into the const pool -> LoadConst
         * + StoreGlobalV. The tree-walker binds it CONST, but that flag is
         * unobservable at runtime (a reassign `P = x` is a compile-time error,
         * `isconst` folds), so a plain StoreGlobalV is differential-identical.
         * Like a func decl, a SCRIPT struct decl is always global (structs
         * never capture; pWrapDeclBody closed the brace-less-body route). */
        const StructDeclStmt *sd = dynamic_cast<const StructDeclStmt *>(s);
        if (sd) {
            if (sd->id) {
                emit_struct_decl(sd, chunk.code);
                return;
            }
        }
        /* A `try/catch` -> a native handler region (P8 Inc 0). */
        if (const TryCatchStmt *tc = dynamic_cast<const TryCatchStmt *>(s)) {
            if (compile_native_try(tc))
                return;
        }
        /* `throw <expr>;` -> a native Throw op (P8 Inc 1). */
        if (const ThrowStmt *th = dynamic_cast<const ThrowStmt *>(s)) {
            if (try_native_throw(th, chunk.code))
                return;
        }
        /* A standalone braced block `{ ... }` statement: a SCOPE-FREE block runs
         * its statements directly in the current context (no child EvalContext -
         * every decl is a frame slot), exactly like an if/loop body, so compile
         * them into this chunk (each still natively or per-statement fallback).
         * A non-scope-free block (a capture / nested func / slot-overflow decl)
         * needs its own context, which vm_run_chunk doesn't build, so it falls
         * back. Matches the tree-walker's Block::do_eval scope-free path. */
        if (const Block *blk = dynamic_cast<const Block *>(s)) {
            if (blk->scope_free) {
                gen_stmts(blk->elems);
                return;
            }
        }

        /* A bare EXPRESSION statement whose value is discarded (`s[3];`,
         * `a[i:j];`, `x + y;`, `flag ? f() : g();`): compile it into a scratch
         * temp and drop the result - evaluating for value then discarding is the
         * same as evaluating for effect, so any error (OOB / missing-key / type)
         * still throws natively with the right caret. SKIP a bare leaf
         * (Identifier / scalar Literal): it has no side effect, and the
         * tree-walker never RValue-s a discarded statement - so an undefined
         * name must stay its harmless no-op (an UndefinedId sentinel), NOT a
         * LoadGlobalV that would throw. A non-liftable expr (an AST/dev builtin,
         * an inc-dec, an unresolved name) compiles or aborts below. */
        {
            /* A discarded INERT leaf - a scalar literal, or an id that is
             * ALWAYS bound (local/param/capture/builtin) - is a no-op, as
             * Block::do_eval (it never RValue-s the result). An UNRESOLVED
             * id must throw UndefinedVariableEx and a GLOBAL id throws iff
             * not yet defined - both exactly what compile_boxed_expr's leaf
             * paths emit (ThrowRuntimeV / LoadGlobalV), result discarded. */
            const Identifier *bid = dynamic_cast<const Identifier *>(s);
            const bool inert =
                dynamic_cast<const Literal *>(s)
                || (bid && bid->sym.kind != SymKind::unresolved
                        && bid->sym.kind != SymKind::global);
            if (inert)
                return;
            const size_t emark = chunk.code.size();
            const int st = next_temp;
            int dst;
            if (compile_boxed_expr(s, dst, chunk.code))
                return;
            chunk.code.resize(emark);
            next_temp = st;
        }

        throw_not_lowered(s);
    }

};

/*
 * Post-codegen pass: move an op's caret Loc from its `Instr::node` AST pointer
 * into the chunk's loc SIDE TABLE, and NULL the node - so the op is AST-free
 * (serializable / JIT-able) and only the throw path pays for the loc (loc_at).
 * Runs on the FINISHED chunk (no interaction with the codegen's rollback), in
 * ascending pc order (so `locs` stays sorted). Applied to the ops whose ONLY
 * use of `node` is the error Loc - the register div/mod ops for now; other ops
 * (fallback node->eval, op-data like a member key) keep their node until those
 * uses are migrated too.
 */
/* FLATTEN an InlineCtx chain into the serializable inline_frames pool, deduping
 * shared chains via `memo`. Interns the PARENT first, so a frame's parent always
 * has a lower index (a clean topological order). Returns the pool index (-1 for
 * a null chain). Off the recursive codegen frame (chains can nest). */
static ML_NOINLINE int32_t
intern_inline_ctx(const InlineCtx *ic, Chunk &chunk,
                  std::unordered_map<const InlineCtx *, int32_t> &memo)
{
    if (!ic)
        return -1;
    auto it = memo.find(ic);
    if (it != memo.end())
        return it->second;
    const int32_t parent = intern_inline_ctx(ic->parent, chunk, memo);
    const int32_t idx = static_cast<int32_t>(chunk.inline_frames.size());
    chunk.inline_frames.push_back(
        {ic->callee_name, ic->params, ic->call_site, parent});
    memo[ic] = idx;
    return idx;
}

static void extract_locs(Chunk &chunk,
                         const std::vector<const Construct *> &ast_nodes)
{
    auto node_at = [&](int32_t idx) -> const Construct * {
        return idx < 0 ? nullptr : ast_nodes[static_cast<size_t>(idx)];
    };
    /* InlineCtx* -> inline_frames index, shared across ops so a chain reused by
     * many spliced ops is flattened once. */
    std::unordered_map<const InlineCtx *, int32_t> inline_memo;
    for (size_t pc = 0; pc < chunk.code.size(); pc++) {
        Instr &in = chunk.code[pc];
        const Construct *node = node_at(in.node_idx);
        if (!node)
            continue;
        /* P8 Inc 4: an op spliced from an INLINED body records that body's
         * inlined-at chain (FLATTENED into inline_frames), so a backtrace
         * crossing it shows the virtual frames. Recorded BEFORE the switch nulls
         * the node; pc-ascending, so inline_ctxs stays sorted. (Rare - only
         * inlined ops have one.) */
        if (node->inline_ctx)
            chunk.inline_ctxs.push_back(
                {static_cast<uint32_t>(pc),
                 intern_inline_ctx(node->inline_ctx, chunk, inline_memo)});
        switch (in.op) {
        case OpCode::IntBin:
        case OpCode::FloatBin:
        case OpCode::DictLoadInt:
        case OpCode::DictLoadFloat:
        case OpCode::SubscriptV:
        case OpCode::BinOpV:
        case OpCode::CompoundV:
        case OpCode::CmpV:
        case OpCode::UnaryV:         /* node = operand expr (`-str`/`~str` caret) */
        case OpCode::LoadGlobalV:
        case OpCode::CallValueV:
        case OpCode::UnpackElemInt:
        case OpCode::UnpackElemFloat:
        case OpCode::UnpackElemValue:
        case OpCode::UnpackElemTargets:
        case OpCode::SliceV:
        case OpCode::StoreGlobalV:   /* compound/inc-dec (plain: node null) */
        case OpCode::StoreCaptureV:
        case OpCode::DictStore:      /* node = the Subscript (its caret) */
        case OpCode::StoreElemValue:
        case OpCode::StoreLValueChainV: /* node = the outer lvalue (its caret) */
        case OpCode::IncDecCheckedV:  /* node = the inc-dec (TypeError caret) */
        case OpCode::StructCtorV:    /* node = ctor (defensive coerce loc) */
        case OpCode::MakeStructArrayV: /* node = a ctor (defensive coerce loc) */
        case OpCode::ForeachDynInit: /* node = container (unsupported caret) */
        case OpCode::ForeachDynNext: /* node set only for a 2-var unpack caret */
        case OpCode::LoadElemInt:    /* node = the a[i] / container (OOB caret) */
        case OpCode::LoadElemFloat:
        case OpCode::LoadElemValue:
        case OpCode::MultiUnpackV:   /* node = the Expr14 (unpack-length caret) */
        case OpCode::StoreElemInt:   /* node = the SUBSCRIPT (plain: OOB/type) or
                                      * the Expr14 (compound: its div0 caret) */
        case OpCode::StoreElemFloat:
        case OpCode::CheckFuncV:     /* node = arg0 (Expected-function caret) */
        case OpCode::MapFilterV:     /* node = arg1 (unsupported-container caret) */
        case OpCode::Throw:          /* node = ThrowStmt (throw-site loc) */
        case OpCode::Rethrow:        /* node = RethrowStmt (rethrow-site loc) */
        case OpCode::CoerceNumV:    /* node = the Expr14 (narrow-throw caret) */
        case OpCode::CheckCallableV: /* node = the callee (NotCallable caret) */
        case OpCode::CallValueGenericV: /* node = the CallExpr: the CALL-SITE
                                     * loc (a FuncObject callee's backtrace via
                                     * do_func_call's loc_at); the op is now
                                     * AST-FREE (pooled ArgLocs + the lvalue-
                                     * preserving arg run) - F1 step 2. */
        /* the checked inc-decs: dual carets in incdec_sites; the side-table
         * loc = the undefined-global-base caret (vm_store_base). */
        case OpCode::IncDecElemCheckedV:
        case OpCode::IncDecMemberCheckedV:
        case OpCode::IncDecChainV:   /* carets in incdec_chains; side-table loc
                                      * = the undefined-global-root caret */
            /* node used ONLY for the caret now (div/mod; the missing-key
             * KeyNotFoundEx; a subscript OOB/key/type error; a boxed
             * arith/compound/compare div-zero or type error; the cold
             * undefined-global error - the operation itself is AST-free):
             * record the loc -> AST-free. */
            chunk.locs.push_back(
                {static_cast<uint32_t>(pc), node->start, node->end});
            in.node_idx = -1;
            break;
        case OpCode::JumpUnlessIntCmp:
        case OpCode::JumpUnlessFloatCmp:
        case OpCode::ForLoopStep:
        case OpCode::LogV:
        case OpCode::MemberV:
        case OpCode::ArrLen:         /* never throws (just reads the size) */
        case OpCode::DictIterInit:   /* pins a proven dict; no node-based throw */
            /* node not needed for a caret: LogV never throws; MemberV's carets
             * (and name/uid/optional) live in the member-key pool; ArrLen /
             * DictIterInit never throw with a node loc. Drop it. */
            in.node_idx = -1;
            break;
        case OpCode::CallV:
        case OpCode::CachedCallV: {
            /* Record the CALLEE-IDENTIFIER loc (matches the tree-walker's
             * undefined-callee caret); the backtrace call-site uses its start
             * (== the call's). NotCallableEx is unreachable for a CallV (a
             * proven global func slot), so its caret is moot. */
            const CallExpr *call = static_cast<const CallExpr *>(node);
            chunk.locs.push_back({static_cast<uint32_t>(pc),
                                  call->what->start, call->what->end});
            in.node_idx = -1;
            break;
        }
        /* The ops that genuinely READ the node at RUNTIME - KEEP node_idx (these
         * populate ast_nodes): the fallbacks (node->eval / vm_eval_cond), the
         * builtin-call ops (the args ExprList for per-arg carets), and the flat
         * int/float element store (node->start for its OOB/div0 caret; the
         * general/dict stores above use the loc side table instead, so they were
         * nulled). Everything NOT listed anywhere here sets a node it never uses
         * at runtime - the default nulls it, so a fully-native chunk's pool is
         * empty (the accurate "not serializable yet" signal). */
        case OpCode::EmplaceStruct: {
            /* The ctor def + container/field carets are in the emplace_sites
             * pool; record the WHOLE-ARGS caret (the handler's catch stamps a
             * loc-less throw with it) -> AST-free. */
            const auto *dc = static_cast<const DirectBuiltinCallExpr *>(node);
            chunk.locs.push_back({static_cast<uint32_t>(pc),
                                  dc->args->start, dc->args->end});
            in.node_idx = -1;
            break;
        }
        default:
            in.node_idx = -1;
            break;
        }
    }
}

/*
 * After extract_locs nulled every loc-only op's node_idx, rebuild ast_nodes to
 * hold ONLY the entries still referenced by a live Instr::node_idx (the
 * runtime-node ops - fallbacks / builtin calls / a store's caret), remapping
 * indices in pc order. Drops the loc-only nodes AND any codegen-rollback
 * orphans, so a fully-native chunk ends with an EMPTY pool - the accurate
 * "not yet serializable" signal. (add_ast_node gives each op a unique index, so
 * no two ops share an entry; a duplicate would just copy the node, still sound.)
 */
/* Flatten every op that STILL needs its AST node at runtime (node_idx >= 0
 * after extract_locs nulled the loc-only ones) into the pc-keyed `node_table`
 * side table, then DROP the indexed `ast_nodes` pool + the live `node_idx`s.
 * After this the runtime looks a node up by pc (node_at_pc), the last AST
 * reference off the per-Instr field. pc-ascending, so node_table stays sorted. */
/* After extract_locs, NO op may still reference an AST node: every native op
 * is pool/loc-based and the fallback ops are deleted. Enforce the invariant -
 * a live node_idx here is a codegen bug (an op emitted with a node that no
 * extract_locs case handles). */
static void verify_ast_free(Chunk &chunk)
{
    for (Instr &in : chunk.code) {
        ML_CHECK(in.node_idx == -1);
        (void)in;
    }
}

}  /* namespace */


/*
 * B1/B2 (plans/vm-performance-roadmap.md): rewrite general IntBin/FloatBin
 * ops into their per-operator, per-shape variants (see the enum comment in
 * bytecode.h). IN PLACE - opcode + (for a lit-first commutative op) an
 * operand swap only, so pcs, the loc side table, and every pool stay
 * untouched. Runs on the FINISHED chunk, after extract_locs.
 */
static void specialize_arith_ops(Chunk &ck)
{
    for (Instr &in : ck.code) {

        if (in.op == OpCode::IntBin) {

            /* Normalize a lit-first COMMUTATIVE op to reg-first (RI). */
            if (in.a_is_lit() && !in.b_is_lit()) {
                switch (in.aop) {
                case Op::plus: case Op::times: case Op::band:
                case Op::bor:  case Op::bxor:
                    in.swap_ab();
                    break;
                default:
                    break;
                }
            }
            if (in.a_is_lit())
                continue;                 /* lit-first non-commutative */
            const bool ri = in.b_is_lit();

            switch (in.aop) {
            case Op::plus:  in.op = ri ? OpCode::IntAddRI
                                       : OpCode::IntAddRR; break;
            case Op::minus: in.op = ri ? OpCode::IntSubRI
                                       : OpCode::IntSubRR; break;
            case Op::times: in.op = ri ? OpCode::IntMulRI
                                       : OpCode::IntMulRR; break;
            case Op::band:  in.op = ri ? OpCode::IntAndRI
                                       : OpCode::IntAndRR; break;
            case Op::bor:   in.op = ri ? OpCode::IntOrRI
                                       : OpCode::IntOrRR; break;
            case Op::bxor:  in.op = ri ? OpCode::IntXorRI
                                       : OpCode::IntXorRR; break;
            case Op::shl:   in.op = ri ? OpCode::IntShlRI
                                       : OpCode::IntShlRR; break;
            case Op::shr:   in.op = ri ? OpCode::IntShrRI
                                       : OpCode::IntShrRR; break;
            case Op::mod:
                /* Only the NONZERO-immediate form (no zero check needed -
                 * the checksum shape); mod-by-reg / mod-by-zero keep
                 * IntBin's checked path + its div0 loc. */
                if (ri && in.b_lit() != 0)
                    in.op = OpCode::IntModRI;
                break;
            default:
                break;                    /* div / ushr: keep IntBin */
            }

        } else if (in.op == OpCode::FloatBin) {

            if (in.a_is_lit() && !in.b_is_lit()) {
                switch (in.aop) {
                case Op::plus: case Op::times:
                    in.swap_ab();
                    break;
                default:
                    break;
                }
            }
            if (in.a_is_lit())
                continue;
            const bool ri = in.b_is_lit();

            switch (in.aop) {
            case Op::plus:  in.op = ri ? OpCode::FloatAddRI
                                       : OpCode::FloatAddRR; break;
            case Op::minus: in.op = ri ? OpCode::FloatSubRI
                                       : OpCode::FloatSubRR; break;
            case Op::times: in.op = ri ? OpCode::FloatMulRI
                                       : OpCode::FloatMulRR; break;
            default:
                break;                    /* div/mod: keep FloatBin */
            }
        }
    }
}

/* ============ The post-codegen PEEPHOLE pass (roadmap E1-E4) ============
 *
 * plans/vm-peephole.md. Runs BEFORE extract_locs, so the loc/inline_ctxs
 * side tables are built from the ALREADY-compacted code and the pass only
 * ever rewrites Instr pc fields (every pool is operand-indexed, never
 * pc-indexed; Instr::node_idx handles ride inside the moved Instr structs).
 * Compile-time only - no new runtime ops, so vm_run_chunk is untouched.
 */

/*
 * THE pc-field table - the single audited enumeration of every Instr field
 * that holds a pc, used by remapping (MANDATORY for correctness), threading,
 * and the CFG successor walk. Audited 2026-07-16 against every `pc = in->`
 * site in vm.cpp. THE TRAP (fuzzer-caught in E-v1): a "target" field is NOT
 * always a pc - ForLoopStep::target2 is the COUNTER SLOT, JumpUnlessTrueV's
 * target2 the value slot, SetPend::target a Pend enum. A new branching op
 * MUST be added here, and ALWAYS run tests/nested_fuzz.py after touching
 * this pass.
 */
template <typename F>
static void visit_pc_fields(Instr &in, F f)
{
    switch (in.op) {
    case OpCode::Jump:
    case OpCode::JumpUnlessIntCmp:
    case OpCode::JumpUnlessFloatCmp:
    case OpCode::JumpUnlessTrueV:      /* target2 = the value SLOT */
    case OpCode::JumpIfNotNoneV:       /* a = the value operand */
    case OpCode::ForLoopStep:          /* target2 = the COUNTER SLOT */
    case OpCode::DictIterNext:
    case OpCode::ForeachDynNext:
    case OpCode::CatchTest:            /* a.lit = catch_types idx */
    case OpCode::PushHandler:          /* -> the runtime handler's catch_pc */
        f(in.target);
        break;
    default:
        break;
    }
}

/* Ops that never continue at pc+1. Throw/Reraise/Rethrow also never fall
 * through, but control can resume at handler pcs - keeping their fall-through
 * edge is CONSERVATIVE (it only keeps more ops alive / extends liveness). */
static bool op_falls_through(OpCode op)
{
    switch (op) {
    case OpCode::Jump:
    case OpCode::Halt:
    case OpCode::ReturnV:
        return false;
    default:
        return true;
    }
}

/* Retarget through a chain of plain Jumps (hop cap for degenerate cycles;
 * a self-Jump - `while(true);` - is left intact). */
static int pp_thread(const Chunk &ck, int t)
{
    int hops = 0;
    while (hops++ < 8
           && t >= 0 && static_cast<size_t>(t) < ck.code.size()
           && ck.code[t].op == OpCode::Jump
           && ck.code[t].target != t)
        t = ck.code[t].target;
    return t;
}

/* `JumpUnlessIntCmp(C)` inversion for branch-over-jump. INT ONLY: float
 * compares don't invert under NaN (`!(a<b)` is not `a>=b`). */
static Op invert_int_cmp(Op o)
{
    switch (o) {
    case Op::lt:    return Op::ge;
    case Op::le:    return Op::gt;
    case Op::gt:    return Op::le;
    case Op::ge:    return Op::lt;
    case Op::eq:    return Op::noteq;
    case Op::noteq: return Op::eq;
    default:        return Op::invalid;
    }
}

/*
 * E1's USE/DEF model over TEMP slots. `visit_use_def` reports each op's frame-
 * slot reads (u) and writes (d) - layouts verified against disasm.cpp/vm.cpp
 * per op. An op NOT in the verified set is a BARRIER (returns false): treated
 * as reading EVERY temp, so liveness stays sound without a complete table
 * (the pool-target ops - MultiUnpackV, UnpackElem*, IncDecChainV, the LV
 * builtin family, stores - reference slots through pools/runs; barrier'ing
 * them costs optimization, never correctness).
 */
template <typename U, typename D>
static bool visit_use_def(const Instr &in, U u, D d)
{
    const auto opnd = [&](const Operand &o) {
        if (!o.is_lit && o.slot >= 0)
            u(static_cast<int>(o.slot));
    };
    const auto run = [&](int base, int cnt) {
        for (int i = 0; i < cnt; i++)
            u(base + i);
    };
    switch (in.op) {
    case OpCode::Jump: case OpCode::Halt: case OpCode::PopHandler:
    case OpCode::PushHandler: case OpCode::SetPend: case OpCode::EndFinally:
    case OpCode::Reraise: case OpCode::Rethrow: case OpCode::ThrowRuntimeV:
        return true;
    case OpCode::IntBin: case OpCode::FloatBin:
    case OpCode::BinOpV: case OpCode::CmpV: case OpCode::LogV:
        opnd(in.a()); opnd(in.b()); d(in.target); return true;
    case OpCode::JumpUnlessIntCmp: case OpCode::JumpUnlessFloatCmp:
        opnd(in.a()); opnd(in.b()); return true;
    case OpCode::JumpUnlessTrueV:
        u(in.target2); return true;
    case OpCode::JumpIfNotNoneV:
        opnd(in.a()); return true;
    case OpCode::ForLoopStep:
        u(in.target2); opnd(in.a()); opnd(in.b()); d(in.target2); return true;
    case OpCode::MoveV:
        u(in.target2); d(in.target); return true;
    case OpCode::LoadImmInt: case OpCode::LoadImmFloat:
    case OpCode::LoadConstV: case OpCode::LoadLiteralObjV:
    case OpCode::LoadGlobalV: case OpCode::LoadCaptureV:
    case OpCode::LoadBuiltinV: case OpCode::DefinedGlobalV:
    case OpCode::MakeClosureV:   /* captures snapshot NAMED locals, not temps */
        d(in.target); return true;
    case OpCode::UnaryV: case OpCode::CoerceNumV:
        opnd(in.a()); d(in.target); return true;
    case OpCode::CompoundV:
        u(in.target); opnd(in.b()); d(in.target); return true;
    case OpCode::MathFnV:
        opnd(in.a()); opnd(in.b()); d(in.target); return true;
    case OpCode::SubscriptV: case OpCode::MemberV:
    case OpCode::DictLoadInt: case OpCode::DictLoadFloat:
    case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
    case OpCode::LoadStrChar:
        u(in.target2); opnd(in.a()); d(in.target); return true;
    case OpCode::ArrLen: case OpCode::StrLen:
        u(in.target2); d(in.target); return true;
    case OpCode::SliceV:
        u(in.target2);
        if (in.a_slot() >= 0) u(static_cast<int>(in.a_slot()));
        if (in.b_slot() >= 0) u(static_cast<int>(in.b_slot()));
        d(in.target); return true;
    case OpCode::ReturnV:
        opnd(in.a()); return true;
    case OpCode::StoreGlobalV: case OpCode::StoreCaptureV:
        /* target = the GLOBAL/CAPTURE index, NOT a frame slot - no def */
        opnd(in.a()); return true;
    case OpCode::CallV: case OpCode::CachedCallV: case OpCode::CallBuiltinV:
        run(static_cast<int>(in.a_lit()), static_cast<int>(in.b_lit()));
        d(in.target); return true;
    case OpCode::CallValueV:
        u(in.target2);
        run(static_cast<int>(in.a_lit()), static_cast<int>(in.b_lit()));
        d(in.target); return true;
    case OpCode::MakeArrayV: case OpCode::StructCtorV:
        run(static_cast<int>(in.a_lit()), static_cast<int>(in.b_lit()));
        d(in.target); return true;
    case OpCode::MakeDictV:
        run(static_cast<int>(in.a_lit()), 2 * static_cast<int>(in.b_lit()));
        d(in.target); return true;
    case OpCode::AppendV:
        if (in.a_lit() == 0)             /* arg0 kind 0 = a frame slot */
            u(in.target2);
        u(static_cast<int>(in.b_lit())); /* the value's SLOT (lit-encoded) */
        d(in.target); return true;
    default:
        return false;                  /* BARRIER - reads everything */
    }
}

/* Producer ops whose ONLY frame-slot write is `target` (verified above) and
 * whose semantics don't otherwise depend on the dst - safe to retarget. */
static bool retargetable_dst(OpCode op)
{
    switch (op) {
    case OpCode::IntBin: case OpCode::FloatBin:
    case OpCode::BinOpV: case OpCode::CmpV: case OpCode::LogV:
    case OpCode::UnaryV: case OpCode::CoerceNumV: case OpCode::MoveV:
    case OpCode::LoadImmInt: case OpCode::LoadImmFloat:
    case OpCode::LoadConstV: case OpCode::LoadLiteralObjV:
    case OpCode::LoadGlobalV: case OpCode::LoadCaptureV:
    case OpCode::LoadBuiltinV: case OpCode::DefinedGlobalV:
    case OpCode::MakeClosureV: case OpCode::MathFnV:
    case OpCode::SubscriptV: case OpCode::MemberV:
    case OpCode::DictLoadInt: case OpCode::DictLoadFloat:
    case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
    case OpCode::LoadStrChar: case OpCode::ArrLen: case OpCode::StrLen:
    case OpCode::SliceV: case OpCode::CallV: case OpCode::CachedCallV:
    case OpCode::CallValueV: case OpCode::CallBuiltinV:
    case OpCode::MakeArrayV: case OpCode::MakeDictV: case OpCode::StructCtorV:
        return true;
    default:
        return false;
    }
}

static void peephole_chunk(Chunk &ck)
{
    std::vector<Instr> &code = ck.code;
    if (code.empty())
        return;

    /* Threading can expose a jump-to-next, whose deletion can expose more -
     * iterate to a (cheap) fixpoint. */
    for (int round = 0; round < 4; round++) {

        bool changed = false;
        const size_t n = code.size();

        /*
         * E1: MoveV elimination. Backward liveness over the TEMP slots
         * (single-word bitset; a chunk with > 64 temps skips E1), then
         * `<producer dst=tX>; MoveV d=tX` - adjacent, tX a temp dead after
         * the move, no branch entering at the move - retargets the producer
         * to d and deletes the move. A barrier op reads every temp; when the
         * chunk has handlers, every op may (via a throw) continue at any
         * handler pc, so each op's live-out absorbs the handlers' live-in.
         */
        if (ck.n_temps > 0 && ck.n_temps <= 64) {

            const int tbase = ck.slot_count;
            const uint64_t all = ck.n_temps == 64
                ? ~uint64_t(0) : ((uint64_t(1) << ck.n_temps) - 1);
            const auto bit = [&](int slot) -> uint64_t {
                return (slot >= tbase && slot < tbase + ck.n_temps)
                    ? (uint64_t(1) << (slot - tbase)) : 0;
            };

            std::vector<char> is_tgt(n + 1, 0);
            std::vector<int> handler_pcs;
            for (Instr &in : code) {
                visit_pc_fields(in, [&](int &t) {
                    if (t >= 0 && static_cast<size_t>(t) <= n)
                        is_tgt[t] = 1;
                });
                if (in.op == OpCode::PushHandler)
                    handler_pcs.push_back(in.target);
            }

            std::vector<uint64_t> live_in(n, 0);
            for (bool liv_changed = true; liv_changed; ) {
                liv_changed = false;
                uint64_t hlive = 0;
                for (int h : handler_pcs)
                    if (h >= 0 && static_cast<size_t>(h) < n)
                        hlive |= live_in[h];
                for (size_t r = 0; r < n; r++) {
                    const size_t p = n - 1 - r;
                    Instr &in = code[p];
                    uint64_t out = hlive;
                    visit_pc_fields(in, [&](int &t) {
                        if (t >= 0 && static_cast<size_t>(t) < n)
                            out |= live_in[t];
                    });
                    if (op_falls_through(in.op) && p + 1 < n)
                        out |= live_in[p + 1];
                    uint64_t use = 0, def = 0;
                    const bool known = visit_use_def(in,
                        [&](int s) { use |= bit(s); },
                        [&](int s) { def |= bit(s); });
                    const uint64_t lin =
                        known ? ((out & ~def) | use) : all;
                    if (lin != live_in[p]) {
                        live_in[p] = lin;
                        liv_changed = true;
                    }
                }
            }

            uint64_t hlive = 0;
            for (int h : handler_pcs)
                if (h >= 0 && static_cast<size_t>(h) < n)
                    hlive |= live_in[h];

            for (size_t q = 0; q < n; q++) {
                Instr &m = code[q];
                if (m.op != OpCode::MoveV || m.target == m.target2)
                    continue;
                const int src = m.target2;
                const uint64_t sb = bit(src);
                if (!sb)
                    continue;             /* src not a temp */
                /* live-out of the move = live-in of its fall-through + the
                 * handler absorption (MoveV cannot throw; stay uniform). */
                const uint64_t mout =
                    (q + 1 < n ? live_in[q + 1] : 0) | hlive;
                if (mout & sb)
                    continue;             /* src still read later */

                /* EVERY predecessor of the move must be a retargetable
                 * producer of src: the fall-through one directly, and each
                 * branch into the move must be a plain Jump (a conditional
                 * entering the join disqualifies) whose own fall-through
                 * predecessor is such a producer (the arm's tail; nothing
                 * else may enter that Jump). Covers both the arm-move shape
                 * (`prod; move; jmp` per arm) and the JOIN-move shape
                 * (`prod; jmp Lq` / `prod` fall-through + one move at Lq). */
                std::vector<size_t> prods;
                bool ok = true;
                for (size_t j = 0; j < n && ok; j++) {
                    if (j == q)
                        continue;
                    bool hits = false;
                    visit_pc_fields(code[j], [&](int &t) {
                        if (t == static_cast<int>(q))
                            hits = true;
                    });
                    if (!hits)
                        continue;
                    if (code[j].op != OpCode::Jump || is_tgt[j] || j == 0) {
                        ok = false;
                        break;
                    }
                    Instr &p = code[j - 1];
                    if (!op_falls_through(p.op) || !retargetable_dst(p.op)
                        || p.target != src) {
                        ok = false;
                        break;
                    }
                    prods.push_back(j - 1);
                }
                if (!ok)
                    continue;
                if (q > 0 && op_falls_through(code[q - 1].op)) {
                    Instr &p = code[q - 1];
                    if (!retargetable_dst(p.op) || p.target != src)
                        continue;
                    prods.push_back(q - 1);
                }
                if (prods.empty())
                    continue;
                for (size_t p : prods)   /* produce straight into d */
                    code[p].target = m.target;
                m.op = OpCode::Jump;     /* neutralize: jump-to-next... */
                m.target = static_cast<int>(q) + 1;  /* ...deleted below */
                changed = true;
            }
        }

        /* E3a: thread every pc field through Jump chains. */
        for (Instr &in : code)
            visit_pc_fields(in, [&](int &t) {
                const int nt = pp_thread(ck, t);
                if (nt != t) {
                    t = nt;
                    changed = true;
                }
            });

        /* Branch-target map (post-threading). */
        std::vector<char> is_tgt(n + 1, 0);
        for (Instr &in : code)
            visit_pc_fields(in, [&](int &t) {
                if (t >= 0 && static_cast<size_t>(t) <= n)
                    is_tgt[t] = 1;
            });

        std::vector<char> del(n, 0);

        /* E3d: INT branch-over-jump inversion. `junless(C) L1; jmp L2; L1:`
         * with nothing else entering the jmp -> `junless(!C) L2` + the jmp
         * marked deleted (the inverted branch's fall-through must land at
         * L1 = i+2, which deleting the jmp produces). */
        for (size_t i = 0; i + 1 < n; i++) {
            Instr &b = code[i];
            const Instr &j = code[i + 1];
            if (b.op != OpCode::JumpUnlessIntCmp || j.op != OpCode::Jump)
                continue;
            if (b.target != static_cast<int>(i) + 2 || is_tgt[i + 1])
                continue;
            const Op inv = invert_int_cmp(b.aop);
            if (inv == Op::invalid)
                continue;
            b.aop = inv;
            b.target = j.target;
            del[i + 1] = 1;
            changed = true;
        }

        /* The del-aware fall-through successor: the next surviving pc
         * (compaction collapses deleted ops away, so this IS the runtime
         * successor). */
        const auto next_live = [&](size_t i) -> size_t {
            size_t k = i + 1;
            while (k < n && del[k])
                k++;
            return k;
        };

        /* E3b: a Jump to its own (del-aware) next op is a no-op. */
        for (size_t i = 0; i < n; i++) {
            Instr &in = code[i];
            if (!del[i] && in.op == OpCode::Jump
                && in.target == static_cast<int>(next_live(i))) {
                del[i] = 1;
                changed = true;
            }
        }

        /* E3c: reachability DFS from pc 0 over the del-aware CFG; anything
         * unvisited is dead (the Jumps threading obsoleted, a merge tail
         * every path returns past, ...). */
        {
            std::vector<char> reach(n, 0);
            std::vector<size_t> stack;
            const auto push = [&](size_t p) {
                while (p < n && del[p])   /* land past deleted ops */
                    p++;
                if (p < n && !reach[p]) {
                    reach[p] = 1;
                    stack.push_back(p);
                }
            };
            push(0);
            while (!stack.empty()) {
                const size_t p = stack.back();
                stack.pop_back();
                Instr &in = code[p];
                visit_pc_fields(in, [&](int &t) {
                    if (t >= 0)
                        push(static_cast<size_t>(t));
                });
                if (op_falls_through(in.op))
                    push(p + 1);
            }
            for (size_t i = 0; i < n; i++)
                if (!reach[i] && !del[i]) {
                    del[i] = 1;
                    changed = true;
                }
        }

        /* Compact: drop deleted ops, remap every pc field via the
         * prefix-sum map (a field pointing AT a deleted op remaps to the
         * first survivor at-or-after it - exactly where execution
         * continues for a deleted jump-to-next). */
        if (std::count(del.begin(), del.end(), 1) > 0) {
            std::vector<int> newpc(n + 1);
            int live = 0;
            for (size_t i = 0; i < n; i++) {
                newpc[i] = live;
                if (!del[i])
                    live++;
            }
            newpc[n] = live;
            std::vector<Instr> out;
            out.reserve(live);
            for (size_t i = 0; i < n; i++)
                if (!del[i])
                    out.push_back(code[i]);
            for (Instr &in : out)
                visit_pc_fields(in, [&](int &t) {
                    if (t >= 0 && static_cast<size_t>(t) <= n)
                        t = newpc[t];
                });
            code = std::move(out);
        }

        if (!changed)
            break;
    }
}

Chunk
codegen_chunk(const Block *block, int slot_count)
{
    Codegen cg;
    cg.temp_base = cg.next_temp = cg.max_temp = slot_count;
    cg.gen_stmts(block->elems);
    /* A body that ends in a ReturnV needs no Halt terminator: ReturnV already
     * stops the chunk (vm_run_chunk `return`s), so a trailing Halt is dead AND
     * unreferenced - the codegen emits no jump to the chunk end past a
     * return (an if whose branches all return leaves no merge Jump; a loop
     * exit targets the following op, not the end). A FALL-THROUGH body (last op
     * not a ReturnV - a void fn, a trailing loop/if) keeps the Halt as
     * its implicit-return-`none` terminator + jump target. Saves one dead instr
     * per always-returning function. */
    if (cg.chunk.code.empty()
        || cg.chunk.code.back().op != OpCode::ReturnV)
        cg.emit(OpCode::Halt);
    cg.chunk.n_temps = cg.max_temp - slot_count;
    cg.chunk.n_dict_iters = cg.max_dict_iters;
    cg.chunk.n_dyn_iters = cg.max_dyn_iters;
    cg.chunk.slot_count = slot_count;
    collect_slot_names(block, cg.chunk.slot_names);   /* -vd debug info */
    peephole_chunk(cg.chunk);     /* E1-E4 - BEFORE extract_locs, so the loc/
                                   * inline_ctxs side tables build from the
                                   * compacted code (no side-table remap) */
    extract_locs(cg.chunk, cg.ast_nodes);   /* carets -> the loc side table */
    specialize_arith_ops(cg.chunk);   /* B1/B2 - AFTER extract_locs: an
                                       * in-place op swap, no pc shifts, and
                                       * a stale IntBin div0 loc entry for a
                                       * specialized (non-throwing) op is
                                       * never queried */
    verify_ast_free(cg.chunk);    /* the finished chunk holds NO Construct* */
    return std::move(cg.chunk);
}

Chunk
codegen_program(const Block *root)
{
    return codegen_chunk(root, root->slot_count);
}

void
collect_funcs(const Construct *c, std::vector<const FuncDeclStmt *> &out)
{
    if (!c)
        return;
    if (const FuncDeclStmt *fn = dynamic_cast<const FuncDeclStmt *>(c)) {
        out.push_back(fn);
        collect_funcs(fn->body.get(), out);   /* nested closures within */
        return;
    }
    /* The COMPLETE child walker (inferencer.h) - a hand-kept dynamic_cast
     * chain here used to miss try/catch bodies and slices, leaving a func
     * declared there out of the AOT precompile (the lazy safety net hid it;
     * the AST teardown cannot tolerate that). */
    for_each_child_of(const_cast<Construct *>(c),
                      [&](Construct *ch) { collect_funcs(ch, out); });
}

bool
codegen_func_body(const FuncDeclStmt *fn, Chunk &out)
{
    /* A base template is a monomorphization source, never called → no chunk
     * (the ONLY compiled-set exclusion; do_func_call ML_CHECKs if one is ever
     * called after the AST teardown). */
    if (fn->desc->is_template_base)
        return false;

    /* Every body is a Block since the `=>` desugar. */
    ML_CHECK(fn->body && fn->body->is_block());

    const Block *body = static_cast<const Block *>(fn->body.get());

    /*
     * vm_run_chunk runs the body's statements directly in the call's args
     * context (no per-block child EvalContext), which is correct only for a
     * SCOPE-FREE body (every decl is a frame slot). The one way a script
     * function is NOT scope-free is the pathological un-slottable >64-param
     * function - under the no-fail codegen that is a loud compile refusal,
     * not a silent tree-walk (post-teardown there is no tree to walk).
     */
    if (!body->scope_free)
        throw_not_lowered(fn);

    /* EVERY callable body keeps its chunk - even an empty/no-op one (a bare
     * Halt returning none): after the AST teardown the chunk is the only way
     * to run the body, so there is no "not worth it" tier anymore. */
    out = codegen_chunk(body, fn->desc->frame_size);
    return true;
}

