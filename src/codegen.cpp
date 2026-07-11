/* SPDX-License-Identifier: BSD-2-Clause */

#include "codegen.h"
#include "syntax.h"

#include <vector>

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
    case OpCode::EvalToSlot:  case OpCode::ArrLen:
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

    /* Pool an AST node an op needs at RUNTIME (fallback / builtin / a store's
     * caret) and return its Chunk::ast_nodes index; -1 for a null node (no
     * entry). A COMPACTION pass after extract_locs drops the loc-only nodes it
     * nulled and any codegen-rollback orphans, so the surviving pool is minimal
     * (empty for a fully-native chunk). See Chunk::ast_nodes. */
    int add_ast_node(const Construct *n)
    {
        if (!n)
            return -1;
        chunk.ast_nodes.push_back(n);
        return static_cast<int>(chunk.ast_nodes.size()) - 1;
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
     * entry: it runs as an EvalStmt whose flow==ret stops the chunk - see
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
     * store) when it is a resolved-local scalar decl, else a fallback EvalStmt.
     * Native-izing the once-run init doesn't speed the loop, but it keeps the
     * `i = start` off the tree-walker and reads cleanly in the disassembly. */
    void emit_init(const Construct *init)
    {
        reset_temps();
        const size_t mark = chunk.code.size();
        if (compile_int_stmt(init, chunk.code))
            return;
        chunk.code.resize(mark);
        reset_temps();
        if (compile_float_stmt(init, chunk.code))
            return;
        chunk.code.resize(mark);
        reset_temps();
        /* Boxed decl-init: `var k = i` (an int/bool leaf, or a dyn/string
         * value) - the int path rejects a bare identifier rhs (it can't prove
         * int-not-bool for a raw int store), but a boxed move preserves the
         * rhs's real type. Runs ONCE per loop entry; keeps a var-initialized
         * for-loop off the EvalStmt fallback. */
        if (compile_boxed_stmt(init, chunk.code))
            return;
        chunk.code.resize(mark);
        emit(OpCode::EvalStmt, init);
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
        in.a = int_lit(base);
        in.b = int_lit(n);
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
                ld.a = top;
                ops.push_back(ld);
                out_slot = t;
                return true;
            }
            ops.resize(omark);
            chunk.consts.resize(cmark);
            next_temp = save_top;
        }

        if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
            if (id->sym.kind == SymKind::local) {
                out_slot = id->sym.slot;
                return true;   /* the slot IS the operand - no op */
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
            chunk.closure_defs.push_back(fd);
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
                /* Every arg must be a scalar the coerce can't throw on
                 * (is_typed_scalar_arg): a typed int/float OR a scalar literal.
                 * A nested-struct-field arg (th none, not a scalar literal) or a
                 * dyn arg fails and falls back. */
                bool all_typed = true;
                for (const auto &a : ce->args->elems)
                    if (!is_typed_scalar_arg(a.get())) {
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
                    in.a = int_lit(base);
                    in.b = int_lit(
                        static_cast<int>(ce->args->elems.size()));
                    in.target2 = static_cast<int>(chunk.struct_defs.size());
                    chunk.struct_defs.push_back(sdef);
                    ops.push_back(in);
                    out_slot = dst;
                    return true;
                }
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
            in.a = int_lit(base);
            in.b = int_lit(static_cast<int>(la->elems.size()));
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
            in.a = int_lit(base);
            in.b = int_lit(npairs);
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
            in.a = slot_op(idx_slot);
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
            in.a = int_lit(add_member_key(m));   /* AST-free: pool index */
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
            in.a = slot_op(start_slot);  /* -1 == absent */
            in.b = slot_op(end_slot);
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

        /* An arith (`a+b`) / comparison (`a<b`) / logical (`a&&b`) chain, as a
         * raw ExprNN OR a TypedScalarExpr - `A && B` of comparisons specializes
         * to a logical even when the comparisons are boxed over a dyn operand,
         * so both forms reach here. emit_boxed_chain handles both. */
        if (const TypedScalarExpr *t =
                dynamic_cast<const TypedScalarExpr *>(e)) {
            const char k = t->cat == TypedScalarExpr::Cat::arith   ? 'a'
                         : t->cat == TypedScalarExpr::Cat::cmp     ? 'c'
                         : t->cat == TypedScalarExpr::Cat::logical ? 'l'
                                                                   : 0;
            return k && emit_boxed_chain(t->elems, k, e, out_slot, ops);
        }
        char k = 0;
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
        if (k == 'c' && elems.size() != 2)   /* only a 2-operand comparison */
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
            in.a = acc_op;
            in.b = rhs_op;
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
     * statement stays an EvalStmt, i.e. the tree-walker's strict
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
                          std::vector<Instr> &ops)
    {
        const size_t n = il->elems.size();
        if (n == 0)
            return false;
        for (const auto &t : il->elems) {
            if (t->is_underscore())
                continue;               /* `_` is a skipped slot */
            if (t->sym.kind != SymKind::local || t->is_const
                || (t->decl_type != DeclType::none
                    && t->decl_type != DeclType::dyn))
                return false;
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
        in.a = slot_op(rslot);
        in.target = static_cast<int>(chunk.unpack_targets.size());
        chunk.unpack_targets.push_back(std::move(targets));
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
                    || id->decl_type == DeclType::dyn)
                && (id->sym.kind == SymKind::global
                    || id->sym.kind == SymKind::capture)) {
                Instr in;
                in.op = id->sym.kind == SymKind::global
                            ? OpCode::StoreGlobalV : OpCode::StoreCaptureV;
                in.node_idx = add_ast_node(s);
                in.target = id->sym.slot;
                in.a = int_lit(1);
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
        const Identifier *lv =
            dynamic_cast<const Identifier *>(e->lvalue.get());
        if (!lv || (lv->sym.kind != SymKind::local
                    && lv->sym.kind != SymKind::global
                    && lv->sym.kind != SymKind::capture))
            return false;
        if (lv->decl_type != DeclType::none && lv->decl_type != DeclType::dyn)
            return false;
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
                in.a = slot_op(rslot);
                ops.push_back(in);
                return true;
            }
            ops.resize(om);
            chunk.consts.resize(cm);
            next_temp = st;
        }

        /* A reassignment of a CONST (a runtime const - a func/array kept in a
         * slot) must throw CannotRebindConstEx; the boxed store would skip that
         * runtime check. Leave a const lvalue to the tree-walker (throws with
         * the lvalue's exact loc). */
        if (lv->is_const)
            return false;

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
                Instr in;
                in.op = store_op;
                in.target = lv->sym.slot;   /* global-table / capture slot */
                in.a = slot_op(rslot);      /* aop invalid == plain assign */
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
            in.a = rhs_op;
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
            in.b = rhs_op;
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

        if (rslot != lv->sym.slot) {
            /* Retarget the producing op to write the lvalue directly - but ONLY
             * an op THIS statement emitted (ops grew past omark). A LEAF rvalue
             * (a bare local) emits no op, so ops.back() would be the PREVIOUS
             * statement's op - retargeting it would corrupt it (the `var t = s`
             * bug). A leaf falls to MoveV. */
            if (ops.size() > omark && ops.back().target == rslot
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
        in.a = idx;
        ops.push_back(in);
        out_slot = t;
        return true;
    }

    /* Emit EvalToSlot{temp = node->eval}, returning the temp as an operand, so
     * a scalar-result call becomes a native int/float operand. */
    Operand eval_to_temp(const Construct *e, std::vector<Instr> &ops)
    {
        const int t = alloc_temp();
        Instr in;
        in.op = OpCode::EvalToSlot;
        in.node_idx = add_ast_node(e);
        in.target = t;
        ops.push_back(in);
        return slot_op(t);
    }

    /*
     * Native user-function call `dst = f(args...)` -> CallV: evaluate each arg
     * into a contiguous register run [argbase, argbase+n), then one CallV that
     * gathers those values and calls do_func_call (no node->eval of the call).
     * Only a DirectCallExpr the inferencer proved a user function
     * (vm_direct_func); an arg compile_boxed_expr can't lower (a nested call, a
     * complex expression) rolls the whole call back to the EvalStmt/EvalToSlot
     * fallback. Args are evaluated left-to-right, once each.
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
        cv.a = int_lit(argbase);
        cv.b = int_lit(static_cast<int>(dc->args->elems.size()));
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
    bool try_native_value_call(const CallExpr *call, int &out_slot,
                               std::vector<Instr> &ops)
    {
        if (dynamic_cast<const DirectCallExpr *>(call)
            || dynamic_cast<const DirectBuiltinCallExpr *>(call))
            return false;
        if (!call->vm_direct_func || !call->args)
            return false;

        const size_t mark = ops.size();
        const int save_top = next_temp;

        int callee_slot;
        if (!compile_boxed_expr(call->what.get(), callee_slot, ops)) {
            ops.resize(mark);
            next_temp = save_top;
            return false;
        }

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
        cv.a = int_lit(argbase);
        cv.b = int_lit(static_cast<int>(call->args->elems.size()));
        ops.push_back(cv);
        out_slot = dst;
        return true;
    }

    /* Native builtin call -> CallBuiltinV, but only for a builtin with the
     * VALUE ABI (func_v is set - a migrated, read-only builtin); a mutating /
     * AST / un-migrated builtin stays the EvalToSlot fallback. */
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
        cv.a = int_lit(argbase);
        cv.b = int_lit(static_cast<int>(dc->args->elems.size()));
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
        chk.a = slot_op(t0);
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
        in.a = slot_op(t0);
        in.b = slot_op(t1);
        ops.push_back(in);
        out_slot = dst;
        return true;
    }

    /* Native mutating-builtin call -> CallBuiltinLV, but ONLY when arg0 is a
     * slotted identifier (local/global/capture) - the common `append(a, x)`
     * form. The value args are NOT compiled here: func_lv self-evaluates them,
     * which is what keeps append's construct-in-place fast path (it needs the
     * arg node). A subscript/member/other arg0 (or an unresolved one) falls
     * back to EvalToSlot - Phase 2 will nativize those lvalue targets. */
    bool try_native_mutating_builtin(const DirectBuiltinCallExpr *dc,
                                     int &out_slot, std::vector<Instr> &ops)
    {
        if (!dc->builtin.func_lv || !dc->args || dc->args->elems.empty())
            return false;

        const Construct *a0 = dc->args->elems[0].get();

        /* 2c: a SUBSCRIPT lvalue target `append/push/pop(a[i], ...)`. Compile
         * [index, value args...] into a CONTIGUOUS run (REST-NATIVE: the
         * element's LValue* is formed at runtime by Type::subscript - the tree-
         * walker's exact COW - then func_lv gets the value via rest, no
         * self-eval): `b` = the run base, run[0] = the index, run[1..] = the
         * values (append/push have 1, pop has 0). Needs a slotted-id base + all
         * args to lower; a nested base / non-lowerable arg falls through to
         * EvalToSlot. */
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
                        cv.node_idx = add_ast_node(dc);
                        cv.target = dst;
                        cv.target2 =
                            static_cast<const Identifier *>(base)->sym.slot;
                        cv.a = int_lit(bkind);
                        cv.b = int_lit(runbase);
                        ops.push_back(cv);
                        out_slot = dst;
                        return true;
                    }
                    ops.resize(mark);
                    next_temp = save_top;
                }
            }
        }

        if (!a0->is_id())
            return false;

        int kind;
        switch (static_cast<const Identifier *>(a0)->sym.kind) {
        case SymKind::local:   kind = 0; break;
        case SymKind::global:  kind = 1; break;
        case SymKind::capture: kind = 2; break;
        default: return false;   /* builtin / unresolved -> fall back */
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
                    const int dst = alloc_temp();
                    Instr cv;
                    cv.op = OpCode::EmplaceStruct;
                    cv.node_idx = add_ast_node(dc);
                    cv.target = dst;
                    cv.target2 =
                        static_cast<const Identifier *>(a0)->sym.slot;
                    cv.a = int_lit(kind);
                    cv.b = int_lit(fieldbase);
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
         * MARKS this op rest-native (the VM reads `in.b.is_lit`, not a
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
             * cases fall back to the tree-walker (EvalToSlot) instead: (a) arg1
             * is a struct ctor whose EmplaceStruct fell through - append_tw does
             * the construct-in-place there; (b) a value/cmp that doesn't lower to
             * a register run. Otherwise it's rest-native. */
            auto *ctor = dc->args->elems.size() == 2
                ? dynamic_cast<const CallExpr *>(dc->args->elems[1].get())
                : nullptr;
            if (ctor && ctor->vm_struct_ctor_def)
                return false;   /* ctor-fallthrough -> EvalToSlot (append_tw) */
            if (!emit_args_range(dc->args->elems, restbase, ops, 1))
                return false;   /* didn't lower -> EvalToSlot (no self-eval) */
            rest_op = true;
        }

        const int dst = alloc_temp();
        Instr cv;
        cv.op = OpCode::CallBuiltinLV;
        cv.node_idx = add_ast_node(dc);
        cv.target = dst;
        cv.target2 = static_cast<const Identifier *>(a0)->sym.slot;
        cv.a = int_lit(kind);
        if (rest_op)
            cv.b = int_lit(restbase);
        ops.push_back(cv);
        out_slot = dst;
        return true;
    }

    /*
     * Native `return <expr>;` -> ReturnV (no node->eval of the return): the
     * value expr compiles via compile_boxed_expr - so a `return f(x)` becomes a
     * CallV, `return a+b` a BinOpV, etc. A bare `return;` loads `none`. An expr
     * compile_boxed_expr can't lower (e.g. a ternary from a recursion unroll)
     * rolls back to the EvalStmt fallback.
     */
    /*
     * P8 Inc 2c step 3: emit a break/continue, crossing at most ONE enclosing
     * try (popping its handler + routing through its finally). Returns false if
     * it crosses MORE than one try (handler-pop + finally chaining not yet
     * supported) - the caller then fails the whole body so the region
     * tree-walks. Precondition: loops non-empty (checked by the caller).
     */
    /*
     * P8 Inc 2c: at a flow op (return / break / continue) that crosses the
     * innermost `crossed` trys, INLINE each crossed try's handler-pop + finally
     * here, innermost first. Interleaving the pops with the finallys gives the
     * correct throw-in-finally-during-unwind semantics: while T_i's finally
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
        rv.a = slot_op(vslot);
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
        in.a = slot_op(vslot);
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
            in.a = int_lit(add_const(m->memId));
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
            in.a = slot_op(kslot);
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
        in.a = slot_op(sfe_ctr_slot);
        in.b = int_lit(fidx);
        ops.push_back(in);
        out = slot_op(tt);
        return true;
    }

    bool compile_int_expr(const Construct *e, Operand &out,
                          std::vector<Instr> &ops)
    {
        if (as_int_operand(e, out))
            return true;

        if (e->th == TypeHint::i)
            if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(e))
                if (try_sfe_field(m, out, ops, OpCode::LoadStructFieldInt))
                    return true;

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
            in.a = idx;
            ops.push_back(in);
            out = slot_op(tt);
            return true;
        }

        /* A scalar-result BUILTIN call -> eval into a temp; the result is then
         * a native int operand. Builtins ONLY: a builtin is cheap, so the
         * loop-nativization it enables outweighs the EvalToSlot box/unbox. A
         * user call whose body is tree-walked (a closure) would only add the
         * boxing on top of the call (see 11_closure_counter); a cheap/inlinable
         * user call (`func f(x)=>x+1`) is already inlined away. */
        if (e->th == TypeHint::i)
            if (const DirectBuiltinCallExpr *bc =
                    dynamic_cast<const DirectBuiltinCallExpr *>(e)) {
                int t;
                if (try_native_builtin(bc, t, ops))   /* value ABI */
                    out = slot_op(t);
                else
                    out = eval_to_temp(e, ops);        /* old ABI fallback */
                return true;
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
                in.a = acc;
                in.b = rhs;
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
            in.a = int_lit(0);
            in.b = op;
            in.aop = Op::minus;
            ops.push_back(in);
            out = slot_op(tt);
            return true;
        }

        return false;   /* cmp / logical / lnot -> bool, not an int expr */
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
                in.a = dst;
                in.b = int_lit(1);
                in.aop = inc->is_inc ? Op::plus : Op::minus;
                ops.push_back(in);
                return true;
            }
            /* A flat-INT array element `a[i]++` / `a[i]--` -> StoreElemInt with
             * the base op + a constant 1 (`a[i] += 1`). StoreElemInt's loc is
             * node->start/end (generic - works for an IncDecExpr node). */
            if (const Subscript *sub =
                    dynamic_cast<const Subscript *>(inc->lvalue.get())) {
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
                    in.a = idx;
                    in.b = int_lit(1);
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
                    in.a = slot_op(kslot);
                    in.b = slot_op(vtemp);
                    in.aop = inc->is_inc ? Op::addeq : Op::subeq;
                    ops.push_back(in);
                    return true;
                }
            }
            return false;
        }

        const Expr14 *e = dynamic_cast<const Expr14 *>(s);
        if (!e)
            return false;

        /* Native array-element store `a[i] = v` / `a[i] OP= v` (int element).
         * The value is compiled BEFORE the index (tree-walker: rhs then index).
         * A subscript lvalue can't be a scalar slot, so this always returns. */
        if (const Subscript *sub =
                dynamic_cast<const Subscript *>(e->lvalue.get())) {

            /* NESTED store `a[i][j] = v` / `a[i][j] OP= v` -> StoreElem2V: the
             * base is another Subscript over a SLOTTED base. Reads `a[i]` as a
             * reference then stores `[j]` into it (flat OR general inner);
             * vm_nested_subscript_store matches the tree-walker's two-level
             * lvalue chain. Value first (rhs), then key1 (inner i), key2 (j) -
             * the tree-walker's rhs-then-lvalue, a(slot)-then-i-then-j order. */
            if (const Subscript *inner =
                    dynamic_cast<const Subscript *>(sub->what.get())) {
                int aslot;
                switch (e->op) {
                case Op::assign: case Op::addeq: case Op::subeq:
                case Op::muleq:  case Op::diveq: case Op::modeq: break;
                default: return false;
                }
                if (!as_array_slot(inner->what.get(), aslot))
                    return false;
                int vslot, k1slot, k2slot;
                if (!compile_boxed_expr(e->rvalue.get(), vslot, ops)
                    || !compile_boxed_expr(inner->index.get(), k1slot, ops)
                    || !compile_boxed_expr(sub->index.get(), k2slot, ops))
                    return false;
                Instr in;
                in.op = OpCode::StoreElem2V;
                in.node_idx = add_ast_node(sub);   /* outer subscript, for its loc (extract_locs) */
                in.target = vslot;
                in.target2 = aslot;
                in.a = slot_op(k1slot);
                in.b = slot_op(k2slot);
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
                in.a = slot_op(kslot);
                in.b = slot_op(vslot);
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
                        in.node_idx = add_ast_node(s);
                        in.target = akind;   /* 0 local / 1 global / 2 cap */
                        in.target2 = aslot;
                        in.a = idx;
                        in.b = val;
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
                in.a = slot_op(kslot);
                in.b = slot_op(vslot);
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
                in.a = slot_op(kin.target);
                in.b = slot_op(vslot);
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
                in.a = int_lit(add_member_key(m));   /* AST-free: pool index */
                in.b = slot_op(vslot);
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
                in.a = r;
                ops.push_back(in);
            } else {
                Instr in;                /* dst = r + 0 (slot copy) */
                in.op = OpCode::IntBin;
                in.node_idx = add_ast_node(s);
                in.target = dst.slot;
                in.a = r;
                in.b = int_lit(0);
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
        in.a = dst;
        in.b = rhs;
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

        if (e->th == TypeHint::f)
            if (const MemberExpr *m = dynamic_cast<const MemberExpr *>(e))
                if (try_sfe_field(m, out, ops, OpCode::LoadStructFieldFloat))
                    return true;

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
            in.a = idx;
            ops.push_back(in);
            out = slot_op(tt);
            return true;
        }

        /* A scalar-result BUILTIN call -> CallBuiltinV (value ABI) or eval into
         * a temp (old ABI); the result is a native float operand. */
        if (e->th == TypeHint::f)
            if (const DirectBuiltinCallExpr *bc =
                    dynamic_cast<const DirectBuiltinCallExpr *>(e)) {
                int t;
                if (try_native_builtin(bc, t, ops))
                    out = slot_op(t);
                else
                    out = eval_to_temp(e, ops);
                return true;
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
                in.a = acc;
                in.b = rhs;
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
            in.a = float_lit(0);
            in.b = op;
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
                in.a = slot_op(id->sym.slot);
                in.b = float_lit(1);
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
                    in.a = idx;
                    in.b = float_lit(1);
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
            in.node_idx = add_ast_node(s);
            in.target = akind;   /* base kind: 0 local / 1 global / 2 cap */
            in.target2 = aslot;
            in.a = idx;
            in.b = val;
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
                in.a = r;
                ops.push_back(in);
            } else {
                Instr in;                /* dst = r + 0.0 (slot copy) */
                in.op = OpCode::FloatBin;
                in.node_idx = add_ast_node(s);
                in.target = dslot;
                in.a = r;
                in.b = float_lit(0);
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
        in.a = slot_op(dslot);
        in.b = rhs;
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
     * store `c[i] = row`, a void call) runs as a fallback EvalStmt WITHIN the
     * native loop, so the loop still goes native around it. SELF-TRUNCATING: a
     * flow-AFFECTING unsupported statement (break/continue/return, or a nested
     * loop/if that can't compile) resizes chunk.code back to the body start and
     * returns false, so the caller falls the whole loop back. Also returns
     * false when NO statement compiled natively (an all-EvalStmt body: the
     * tree-walker's tight counter beats a native loop that only dispatches
     * fallbacks).
     */
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
                if (try_native_foreach(fe) || try_native_foreach_unpack(fe)
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
                 * the whole enclosing loop fell back to one EvalStmt. */
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
             * `return` (ReturnStmt) runs as a fallback EvalStmt WITHIN the
             * native loop. Expr14/CallExpr don't touch the loop's FlowState; a
             * return sets flow==ret, which the EvalStmt handler acts on by
             * stopping the chunk (a return abandons the loop). Anything
             * else (a nested loop/if that couldn't compile, a block) falls the
             * whole loop back. */
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
                /* try_native_return declined (a nested-try return, or an
                 * uncompilable value): it falls back to a tree-walked EvalStmt
                 * that STOPS the chunk on flow==ret. That's fine only if NO
                 * finally is crossed; if any enclosing try has a finally, the
                 * fallback would SKIP it, so fail the whole body and let the
                 * try region tree-walk (which runs the finally correctly). */
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

            if (dynamic_cast<const Expr14 *>(s)
                || dynamic_cast<const CallExpr *>(s)
                || dynamic_cast<const ReturnStmt *>(s)
                || dynamic_cast<const ThrowStmt *>(s)) {
                /* A `throw` runs as a fallback EvalStmt (P8 Inc 0: still a C++
                 * throw) - safe inside a native loop/region: the vm_run_chunk
                 * boundary routes it to an active handler, or propagates it. */
                emit(OpCode::EvalStmt, s);
                continue;
            }

            chunk.code.resize(start);
            return false;
        }
        /* An all-fallback LOOP body: not worth a native loop (the tree-walker's
         * tight counter beats one that only dispatches EvalStmts). This gate
         * does NOT apply to an `if` then/else block (is_loop_body=false): the
         * `if` provides its own native branch, so an all-fallback branch - e.g.
         * `if (n%f==0) return false;` - must still compile so the enclosing
         * loop can go native around it. */
        if (is_loop_body && !any_native) {
            chunk.code.resize(start);
            return false;
        }
        return true;
    }

    /*
     * An `if` inside a native body -> jumps, with native then/else. The
     * condition is a native int/float compare (JumpUnless*Cmp -> Lelse when
     * false) when it is one, else a fallback JumpIfFalse{cond} (eval_cond) - so
     * `if (flag)` / `if (a[i])` work too. Self-truncating like the loops.
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
                /* BOXED condition -> compute a bool slot + branch-unless-true;
                 * else the tree-walker cond (JumpIfFalse). */
                if (compile_boxed_expr(f->condExpr.get(), cslot, chunk.code)) {
                    Instr in;
                    in.op = OpCode::JumpUnlessTrueV;
                    in.node_idx = add_ast_node(f->condExpr.get());
                    in.target2 = cslot;
                    jf = chunk.code.size();
                    chunk.code.push_back(in);
                } else {
                    chunk.code.resize(start);
                    jf = emit(OpCode::JumpIfFalse, f->condExpr.get());
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
     * cross a try boundary (leaking a handler). Inc 0 falls such a try back to
     * EvalStmt (flow-crossing-try is a later increment). Conservative: flags a
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
            in.a = int_lit(types_idx);
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
        t.a = a;
        t.b = b;
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
     * Phase 1 fallback in gen_while, or the enclosing body falls back).
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

        /* The bound + step are loop-immutable (the for-range specializer proved
         * it), so evaluate ONCE. A simple int operand (a slot or immediate) is
         * used directly; a non-trivial bound - `len(s)`/`f()`/an arith chain -
         * compiles into a temp reserved for the whole loop, which ForLoopStep
         * re-reads each iteration (its value is fixed). This is what lets a
         * `for (i; i < len(x); i++)` counted loop go native instead of falling
         * back to node->eval. */
        Operand bound;
        if (!as_int_operand(f->bound.get(), bound)) {
            reset_temps();
            int bslot;
            if (!compile_boxed_expr(f->bound.get(), bslot, chunk.code)) {
                chunk.code.resize(start);
                return false;
            }
            bound = slot_op(bslot);
            temp_base = next_temp;   /* reserve the bound temp */
        }
        Operand step;
        if (f->step) {
            if (!as_int_operand(f->step.get(), step)) {
                chunk.code.resize(start);
                temp_base = saved_base;
                return false;
            }
        } else {
            step = int_lit(1);
        }

        emit_init(f->init.get());                   /* `var i = start`, once */

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
        fstep.a = bound;
        fstep.b = step;
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
     * the cond is an int/float comparison, the body compiles (scalar / nested /
     * flow-free EvalStmt - the any_native gate applies), and the inc is a
     * compilable scalar statement. Self-truncating. The loop var must be a
     * frame slot (so running init/inc in place is sound, no child scope) - the
     * operand compilers enforce that. A break/continue/return in the body isn't
     * flow-free, so the whole loop falls back (correct).
     */
    bool try_native_for(const ForStmt *f)
    {
        if (!f->cond)
            return false;   /* no cond (infinite loop) -> fall back */

        const size_t start = chunk.code.size();

        if (f->init)
            emit_init(f->init.get());                /* declare the var, once */

        const int lstart = here();

        /* The condition -> native compare-branch(es) to the exit, sharing the
         * while path's helper: an int/float compare, a split `A && B`, or a
         * boxed truthiness test (a bool var / `||` / dyn). Bails the loop only
         * if even the boxed path can't compile the cond. */
        std::vector<size_t> exit_jumps;
        if (!emit_cond_jumps(f->cond.get(), exit_jumps)) {
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

        /* The increment, each iteration after the body (int or float). */
        if (f->inc) {
            reset_temps();
            const size_t imark = chunk.code.size();
            if (!compile_int_stmt(f->inc.get(), chunk.code)) {
                chunk.code.resize(imark);
                reset_temps();
                if (!compile_float_stmt(f->inc.get(), chunk.code)) {
                    loops.pop_back();
                    chunk.code.resize(start);
                    return false;
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
        z.a = int_lit(0);
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
                                           : OpCode::LoadElemValue;
        ld.node_idx = add_ast_node(fe->container.get());
        ld.target = x_slot;
        ld.target2 = c;
        ld.a = slot_op(i);
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
        fstep.a = slot_op(n);
        fstep.b = int_lit(1);
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
        if (!struct_fe_body_ok(fe->body.get(), x_slot,
                               fe->container_struct_def, false))
            return false;

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
        z.a = int_lit(0);
        chunk.code.push_back(z);

        const int saved_base = temp_base;
        temp_base = next_temp;

        const size_t jt = emit_cmp(OpCode::JumpUnlessIntCmp,
                                   fe->container.get(), Op::lt,
                                   slot_op(i), slot_op(n));
        const int lbody = here();

        /* No element load - p is never materialized. Activate the direct-read
         * mapping so a p.field read -> LoadStructField*(c[i].fld). */
        sfe_loop_slot = x_slot;
        sfe_arr_slot = c;
        sfe_ctr_slot = i;
        sfe_def = fe->container_struct_def;

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
        fstep.a = slot_op(n);
        fstep.b = int_lit(1);
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
        int base = -1;
        for (int k = 0; k < N; k++) {
            const Identifier *id =
                dynamic_cast<const Identifier *>(fe->ids->elems[k].get());
            if (!id || id->is_underscore() || id->sym.kind != SymKind::local)
                return false;
            if (k == 0)
                base = id->sym.slot;
            else if (id->sym.slot != base + k)
                return false;
        }
        const int unpack_base = base + (fe->indexed ? 1 : 0);

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
        z.a = int_lit(0);
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
        up.op = fe->unpack_elem_th == TypeHint::i ? OpCode::UnpackElemInt
              : fe->unpack_elem_th == TypeHint::f ? OpCode::UnpackElemFloat
                                                  : OpCode::UnpackElemValue;
        up.node_idx = add_ast_node(fe->container.get());
        up.target = unpack_base;
        up.target2 = c;
        up.a = slot_op(i);
        up.b = int_lit(nunpack);
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
        fstep.a = slot_op(n);
        fstep.b = int_lit(1);
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
     * array one. 1 var binds the key (keys-only), 2 vars key + value; a `_`
     * in either position binds nothing (slot -1). Break/continue/return flow
     * through the same FlowState machinery as the array foreach.
     */
    bool try_native_dict_foreach(const ForeachStmt *fe)
    {
        if (!fe->container_is_dict || !fe->ids
            || (fe->ids->elems.size() != 1 && fe->ids->elems.size() != 2))
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
        int k_slot = -1, v_slot = -1;
        if (!slot_of(0, k_slot))
            return false;
        if (fe->ids->elems.size() == 2 && !slot_of(1, v_slot))
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
        nx.a = slot_op(k_slot);     /* -1 == `_`/keys-only-unused */
        nx.b = slot_op(v_slot);
        const size_t nx_i = chunk.code.size();
        chunk.code.push_back(nx);   /* .target (end_pc) backpatched below */

        push_loop();
        if (!compile_scalar_body(body_stmts(fe->body.get()))) {
            loops.pop_back();
            temp_base = saved_base;
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();   /* continue -> back to DictIterNext */
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
        if (!fe->container_is_dyn || fe->indexed || !fe->ids
            || (fe->ids->elems.size() != 1 && fe->ids->elems.size() != 2))
            return false;

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
        const int nvars = static_cast<int>(fe->ids->elems.size());
        int e_slot = -1, v_slot = -1;
        if (!slot_of(0, e_slot))
            return false;
        if (nvars == 2 && !slot_of(1, v_slot))
            return false;

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

        Instr init;
        init.op = OpCode::ForeachDynInit;
        init.node_idx = add_ast_node(fe->container.get());   /* extract_locs -> the caret */
        init.target = iter_id;
        init.target2 = dsrc;
        init.a = int_lit(nvars);           /* 1 or 2 loop vars */
        chunk.code.push_back(init);

        const int lnext = here();          /* test + bind + advance */
        Instr nx;
        nx.op = OpCode::ForeachDynNext;
        nx.target2 = iter_id;
        nx.a = slot_op(e_slot);            /* -1 == `_` */
        nx.b = slot_op(v_slot);            /* 2-var value/2nd slot (-1 if 1-var) */
        /* A 2-var array element is strict-unpacked, so Next can throw; record
         * the container caret (do_iter uses container->start/end). */
        if (nvars == 2)
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
         * to compares/jumps (compile_native_if, else fallback gen_if); a loop
         * tries its native form. Anything else - a return, a call, a complex
         * expression - stays a fallback EvalStmt (tree-walked). Each attempt is
         * self-truncating (resets to `mark` on failure). This is what makes a
         * scalar-arithmetic FUNCTION body native, not only a loop body. */
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
            if (compile_native_if(f))
                return;
            chunk.code.resize(mark);
            gen_if(f);
            return;
        }
        if (const WhileStmt *w = dynamic_cast<const WhileStmt *>(s)) {
            gen_while(w);
            return;
        }
        if (const ForRangeStmt *fr = dynamic_cast<const ForRangeStmt *>(s)) {
            if (!try_native_for_range(fr))     /* else the counted loop falls */
                emit(OpCode::EvalStmt, s);      /* back to the tree-walker */
            return;
        }
        if (const ForStmt *fs = dynamic_cast<const ForStmt *>(s)) {
            if (!try_native_for(fs))           /* general (non-range) for */
                emit(OpCode::EvalStmt, s);
            return;
        }
        if (const ForeachStmt *fe = dynamic_cast<const ForeachStmt *>(s)) {
            if (!try_native_foreach(fe)         /* flat/general array */
                && !try_native_foreach_unpack(fe) /* strict array destructure */
                && !try_native_struct_foreach(fe) /* flat struct fields */
                && !try_native_dict_foreach(fe)   /* live dict iterator */
                && !try_native_dyn_foreach(fe))   /* runtime array|dict */
                emit(OpCode::EvalStmt, s);
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
         * `slots[slot] = LValue(func, false); defined = 1`. A capturing named
         * func (local slot) or the REPL (map) falls back to EvalStmt. */
        if (const FuncDeclStmt *fd = dynamic_cast<const FuncDeclStmt *>(s)) {
            if (fd->id && fd->id->sym.kind == SymKind::global) {
                const int t = alloc_temp();
                Instr mk;
                mk.op = OpCode::MakeClosureV;
                mk.target = t;
                mk.target2 = static_cast<int>(chunk.closure_defs.size());
                chunk.closure_defs.push_back(fd);
                chunk.code.push_back(mk);
                Instr st;             /* aop invalid == plain assign+defined */
                st.op = OpCode::StoreGlobalV;
                st.target = fd->id->sym.slot;
                st.a = slot_op(t);
                chunk.code.push_back(st);
                return;
            }
        }
        /* A `struct P {..}` decl bound into a GLOBAL slot (hoisted, like a func
         * name): bake the type descriptor (a trivial t_structtype value holding
         * the program-lifetime StructTypeDef*) into the const pool -> LoadConst
         * + StoreGlobalV. The tree-walker binds it CONST, but that flag is
         * unobservable at runtime (a reassign `P = x` is a compile-time error,
         * `isconst` folds), so a plain StoreGlobalV is differential-identical
         * (the REPL, map-resident structs, falls back). */
        const StructDeclStmt *sd = dynamic_cast<const StructDeclStmt *>(s);
        if (sd) {
            if (sd->id && sd->id->sym.kind == SymKind::global) {
                const int t = alloc_temp();
                Instr ld;
                ld.op = OpCode::LoadConstV;
                ld.target = t;
                ld.target2 = add_const(EvalValue(sd->def.get()));
                chunk.code.push_back(ld);
                Instr st;
                st.op = OpCode::StoreGlobalV;
                st.target = sd->id->sym.slot;
                st.a = slot_op(t);
                chunk.code.push_back(st);
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
        emit(OpCode::EvalStmt, s);
    }

    void gen_if(const IfStmt *f)
    {
        /* JumpIfFalse cond -> Lelse ; then ; Jump Lend ; Lelse: else ; Lend: */
        const size_t jf = emit(OpCode::JumpIfFalse, f->condExpr.get());

        if (f->thenBlock)
            emit(OpCode::EvalStmt, f->thenBlock.get());

        if (f->elseBlock) {
            const size_t j = emit(OpCode::Jump);
            chunk.code[jf].target = here();          /* Lelse */
            emit(OpCode::EvalStmt, f->elseBlock.get());
            chunk.code[j].target = here();           /* Lend */
        } else {
            chunk.code[jf].target = here();          /* Lend */
        }
    }

    void gen_while(const WhileStmt *w)
    {
        if (try_native_scalar_while(w))    /* Phase 2 register fast path */
            return;

        /* Phase 1 fallback: Lstart JumpIfFalse cond->Lend body LoopBackEdge */
        const int lstart = here();
        const size_t jf = emit(OpCode::JumpIfFalse, w->condExpr.get());

        if (w->body)
            emit(OpCode::EvalStmt, w->body.get());

        const size_t be = emit(OpCode::LoopBackEdge, nullptr, lstart);

        const int lend = here();
        chunk.code[jf].target = lend;
        chunk.code[be].target2 = lend;
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
static void extract_locs(Chunk &chunk)
{
    for (size_t pc = 0; pc < chunk.code.size(); pc++) {
        Instr &in = chunk.code[pc];
        const Construct *node = chunk.node_at(in.node_idx);
        if (!node)
            continue;
        /* P8 Inc 4: an op spliced from an INLINED body records that body's
         * inlined-at chain, so a backtrace crossing it shows the virtual
         * frames. Recorded BEFORE the switch nulls the node; pc-ascending, so
         * inline_ctxs stays sorted. (Rare - only inlined ops have one.) */
        if (node->inline_ctx)
            chunk.inline_ctxs.push_back(
                {static_cast<uint32_t>(pc), node->inline_ctx});
        switch (in.op) {
        case OpCode::IntBin:
        case OpCode::FloatBin:
        case OpCode::DictLoadInt:
        case OpCode::DictLoadFloat:
        case OpCode::SubscriptV:
        case OpCode::BinOpV:
        case OpCode::CompoundV:
        case OpCode::CmpV:
        case OpCode::LoadGlobalV:
        case OpCode::CallValueV:
        case OpCode::UnpackElemInt:
        case OpCode::UnpackElemFloat:
        case OpCode::UnpackElemValue:
        case OpCode::SliceV:
        case OpCode::StoreGlobalV:   /* compound/inc-dec (plain: node null) */
        case OpCode::StoreCaptureV:
        case OpCode::DictStore:      /* node = the Subscript (its caret) */
        case OpCode::StoreElemValue:
        case OpCode::StoreElem2V:    /* node = the outer Subscript (its caret) */
        case OpCode::StructCtorV:    /* node = ctor (defensive coerce loc) */
        case OpCode::MakeStructArrayV: /* node = a ctor (defensive coerce loc) */
        case OpCode::ForeachDynInit: /* node = container (unsupported caret) */
        case OpCode::ForeachDynNext: /* node set only for a 2-var unpack caret */
        case OpCode::LoadElemInt:    /* node = the a[i] / container (OOB caret) */
        case OpCode::LoadElemFloat:
        case OpCode::LoadElemValue:
        case OpCode::MultiUnpackV:   /* node = the Expr14 (unpack-length caret) */
        case OpCode::StoreElemInt:   /* node = the Expr14 (OOB/div0 + store caret) */
        case OpCode::StoreElemFloat:
        case OpCode::CheckFuncV:     /* node = arg0 (Expected-function caret) */
        case OpCode::MapFilterV:     /* node = arg1 (unsupported-container caret) */
        case OpCode::Throw:          /* node = ThrowStmt (throw-site loc) */
        case OpCode::Rethrow:        /* node = RethrowStmt (rethrow-site loc) */
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
        case OpCode::EvalStmt:
        case OpCode::EvalToSlot:
        case OpCode::JumpIfFalse:
        case OpCode::CallBuiltinLV:
        case OpCode::CallBuiltinLVElem:
        case OpCode::EmplaceStruct:
            break;
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
static void compact_ast_nodes(Chunk &chunk)
{
    if (chunk.ast_nodes.empty())
        return;
    std::vector<const Construct *> live;
    for (Instr &in : chunk.code) {
        if (in.node_idx < 0)
            continue;
        const int ni = static_cast<int>(live.size());
        live.push_back(chunk.ast_nodes[static_cast<size_t>(in.node_idx)]);
        in.node_idx = ni;
    }
    chunk.ast_nodes = std::move(live);
}

}  /* namespace */

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
    extract_locs(cg.chunk);   /* move div/mod carets to the loc side table */
    compact_ast_nodes(cg.chunk);   /* minimize the AST-node pool (see above) */
    return std::move(cg.chunk);
}

Chunk
codegen_program(const Block *root)
{
    return codegen_chunk(root, root->slot_count);
}
