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
    case OpCode::LoadImmFloat: case OpCode::LoadGlobalV:
    case OpCode::LoadCaptureV: case OpCode::LoadBuiltinV:
    case OpCode::MoveV:       case OpCode::BinOpV:
    case OpCode::CmpV:        case OpCode::LogV:
    case OpCode::IntBin:      case OpCode::FloatBin:
    case OpCode::SubscriptV:  case OpCode::MemberV:
    case OpCode::CallV:       case OpCode::CachedCallV:
    case OpCode::CallBuiltinV: case OpCode::CallBuiltinLV:
    case OpCode::EvalToSlot:  case OpCode::ArrLen:
    case OpCode::LoadElemInt: case OpCode::LoadElemFloat:
    case OpCode::LoadElemValue:
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

    int here() const { return static_cast<int>(chunk.code.size()); }

    size_t emit(OpCode op, const Construct *node = nullptr,
                int target = -1, int target2 = -1)
    {
        Instr in;
        in.op = op;
        in.node = node;
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
    };
    std::vector<LoopFrame> loops;

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
        emit(OpCode::EvalStmt, init);
    }

    int add_const(const EvalValue &v)
    {
        chunk.consts.push_back(v);
        return static_cast<int>(chunk.consts.size()) - 1;
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
            in.node = id;              /* for the undefined-global loc/name */
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
            in.node = e;
            in.target = t;
            in.target2 = add_const(lit);
            ops.push_back(in);
            out_slot = t;
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
            in.node = sub;
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
            in.node = m;
            in.target = t;
            in.target2 = base_slot;
            ops.push_back(in);
            out_slot = t;
            return true;
        }

        /* A native user-function call `f(args...)` -> CallV. */
        if (const DirectCallExpr *dc = dynamic_cast<const DirectCallExpr *>(e))
            if (try_native_call(dc, out_slot, ops))
                return true;

        /* A native builtin call (value ABI) -> CallBuiltinV. */
        if (const DirectBuiltinCallExpr *bc =
                dynamic_cast<const DirectBuiltinCallExpr *>(e))
            if (try_native_builtin(bc, out_slot, ops))
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
            in.node = k == 'l' ? node : elems[i].second.get();
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
    bool compile_boxed_stmt(const Construct *s, std::vector<Instr> &ops)
    {
        const Expr14 *e = dynamic_cast<const Expr14 *>(s);
        if (!e)
            return false;
        const bool is_assign = e->op == Op::assign;
        const Op cbase = compound_base_op(e->op);   /* `+=` -> plus, else inv */
        if (!is_assign && cbase == Op::invalid)
            return false;
        const Identifier *lv =
            dynamic_cast<const Identifier *>(e->lvalue.get());
        if (!lv || lv->sym.kind != SymKind::local)
            return false;
        if (lv->decl_type != DeclType::none && lv->decl_type != DeclType::dyn)
            return false;
        /* A reassignment of a CONST (a runtime const - a func/array kept in a
         * slot) must throw CannotRebindConstEx; the boxed store would skip that
         * runtime check. Leave a const lvalue to the tree-walker (throws with
         * the lvalue's exact loc). */
        if (lv->is_const)
            return false;

        const size_t omark = ops.size();
        const size_t cmark = chunk.consts.size();

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
            in.node = s;
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
                    || ops.back().op == OpCode::LoadConstV)) {
                ops.back().target = lv->sym.slot;
            } else {
                Instr in;
                in.op = OpCode::MoveV;
                in.node = s;
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
        in.node = e;
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
        in.node = e;
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
            const int sub = next_temp;
            int out;
            if (!compile_boxed_expr(elems[start + i].get(), out, ops)) {
                ops.resize(mark);
                next_temp = save_top;
                return false;
            }
            const int dst = argbase + i;
            if (out == dst) {
                /* already in place */
            } else if (out >= temp_base && !ops.empty()
                       && ops.back().target == out
                       && op_writes_pure_target(ops.back().op)) {
                /* Fuse: the arg's value was just produced into a FRESH temp by
                 * an op that purely writes .target - retarget it straight to
                 * the arg slot, dropping the redundant temp + MoveV. So
                 * `load t, "x"; move rarg = t` becomes `load rarg, "x"`. */
                ops.back().target = dst;
            } else {
                Instr mv;
                mv.op = OpCode::MoveV;
                mv.target = dst;
                mv.target2 = out;
                ops.push_back(mv);
            }
            next_temp = sub;   /* free this arg's scratch for the next */
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
        cv.node = dc;
        cv.target = dst;
        cv.target2 = dc->direct_func_slot;
        cv.a = int_lit(argbase);
        cv.b = int_lit(static_cast<int>(dc->args->elems.size()));
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
        cv.node = dc;
        cv.target = dst;
        cv.a = int_lit(argbase);
        cv.b = int_lit(static_cast<int>(dc->args->elems.size()));
        ops.push_back(cv);
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
                    cv.node = dc;
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

        /* REST-NATIVE (insert/erase): compile the value args (1..n) into a
         * register run so func_lv has zero node->eval; `b` carries its base.
         * A self-eval builtin (append/push/pop/intptr) leaves `b` unused and
         * reads its args off the node. */
        int restbase = 0;
        if (dc->lvalue_rest_native) {
            if (!emit_args_range(dc->args->elems, restbase, ops, 1))
                return false;   /* a rest arg didn't lower -> fall back */
        }

        const int dst = alloc_temp();
        Instr cv;
        cv.op = OpCode::CallBuiltinLV;
        cv.node = dc;
        cv.target = dst;
        cv.target2 = static_cast<const Identifier *>(a0)->sym.slot;
        cv.a = int_lit(kind);
        if (dc->lvalue_rest_native)
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

        Instr rv;
        rv.op = OpCode::ReturnV;
        rv.node = ret;
        rv.a = slot_op(vslot);
        ops.push_back(rv);
        return true;
    }

    bool compile_int_expr(const Construct *e, Operand &out,
                          std::vector<Instr> &ops)
    {
        if (as_int_operand(e, out))
            return true;

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
            in.node = e;
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
                in.node = e;
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
            in.node = e;
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
                in.node = s;
                in.target = dst.slot;
                in.a = dst;
                in.b = int_lit(1);
                in.aop = inc->is_inc ? Op::plus : Op::minus;
                ops.push_back(in);
                return true;
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
            if (sub->th != TypeHint::i || !sub->base_array)
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
            int aslot;
            if (!as_array_slot(sub->what.get(), aslot))
                return false;
            Operand val, idx;
            if (!compile_int_expr(e->rvalue.get(), val, ops)
                || !compile_int_expr(sub->index.get(), idx, ops))
                return false;
            Instr in;
            in.op = OpCode::StoreElemInt;
            in.node = s;
            in.target2 = aslot;
            in.a = idx;
            in.b = val;
            in.aop = aop;
            ops.push_back(in);
            return true;
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
                in.node = s;
                in.target = dst.slot;
                in.a = r;
                ops.push_back(in);
            } else {
                Instr in;                /* dst = r + 0 (slot copy) */
                in.op = OpCode::IntBin;
                in.node = s;
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
        in.node = s;
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
            in.node = e;
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
                in.node = e;
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
            in.node = e;
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
                in.node = s;
                in.target = id->sym.slot;
                in.a = slot_op(id->sym.slot);
                in.b = float_lit(1);
                in.aop = inc->is_inc ? Op::plus : Op::minus;
                ops.push_back(in);
                return true;
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
            int aslot;
            if (!as_array_slot(sub->what.get(), aslot))
                return false;
            Operand val, idx;
            if (!compile_float_expr(e->rvalue.get(), val, ops)
                || !compile_int_expr(sub->index.get(), idx, ops))
                return false;
            Instr in;
            in.op = OpCode::StoreElemFloat;
            in.node = s;
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
                in.node = s;
                in.target = dslot;
                in.a = r;
                ops.push_back(in);
            } else {
                Instr in;                /* dst = r + 0.0 (slot copy) */
                in.op = OpCode::FloatBin;
                in.node = s;
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
        in.node = s;
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
        if (!t)
            return false;

        if (t->cat == TypedScalarExpr::Cat::cmp) {
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
            return false;
        }

        if (t->cat == TypedScalarExpr::Cat::logical) {
            for (size_t i = 1; i < t->elems.size(); i++)
                if (t->elems[i].first != Op::land)
                    return false;   /* `||` is not native yet */
            for (const auto &pr : t->elems)
                if (!emit_cond_jumps(pr.second.get(), exit_jumps))
                    return false;
            return true;
        }

        /* BOXED condition (a dyn/string comparison, or a dyn truthy value):
         * compile it to a bool slot, then branch to the exit unless true. */
        int cslot;
        reset_temps();
        const size_t mark = chunk.code.size();
        if (compile_boxed_expr(cond, cslot, chunk.code)) {
            Instr in;
            in.op = OpCode::JumpUnlessTrueV;
            in.node = cond;
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
                if (try_native_foreach(fe)) {
                    any_native = true;
                    continue;
                }
            }

            chunk.code.resize(mark);

            /* break / continue -> a native Jump to the enclosing loop's exit /
             * continue point (backpatched by pop_loop). Needs an enclosing
             * native loop frame (always present - compile_scalar_body only runs
             * inside one). Gap B. */
            if (dynamic_cast<const BreakStmt *>(s)) {
                if (loops.empty()) {
                    chunk.code.resize(start);
                    return false;
                }
                loops.back().breaks.push_back(emit(OpCode::Jump));
                any_native = true;
                continue;
            }
            if (dynamic_cast<const ContinueStmt *>(s)) {
                if (loops.empty()) {
                    chunk.code.resize(start);
                    return false;
                }
                loops.back().conts.push_back(emit(OpCode::Jump));
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
            /* `return <expr>;` -> ReturnV. */
            if (const ReturnStmt *ret = dynamic_cast<const ReturnStmt *>(s)) {
                if (try_native_return(ret, chunk.code)) {
                    any_native = true;
                    continue;
                }
            }

            if (dynamic_cast<const Expr14 *>(s)
                || dynamic_cast<const CallExpr *>(s)
                || dynamic_cast<const ReturnStmt *>(s)) {
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
                    in.node = f->condExpr.get();
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

    /* Push a JumpUnless{Int,Float}Cmp and return its index (for backpatching). */
    size_t emit_cmp(OpCode opc, const Construct *node, Op cmp,
                    const Operand &a, const Operand &b)
    {
        Instr t;
        t.op = opc;
        t.node = node;
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

        loops.push_back({});
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
        Operand bound, step;
        if (!as_int_operand(f->bound.get(), bound))
            return false;
        if (f->step) {
            if (!as_int_operand(f->step.get(), step))
                return false;
        } else {
            step = int_lit(1);
        }

        const size_t start = chunk.code.size();

        emit_init(f->init.get());                   /* `var i = start`, once */

        const Operand ci = slot_op(f->i_slot);

        /* Initial test: skip the loop entirely if !(i <cmp> bound). */
        const size_t jt =
            emit_cmp(OpCode::JumpUnlessIntCmp, f, f->cmp_op, ci, bound);

        const int lbody = here();
        loops.push_back({});
        if (!compile_scalar_body(body_stmts(f->body.get()))) {
            loops.pop_back();
            chunk.code.resize(start);
            return false;
        }

        const int lcont = here();   /* continue -> the fused step (i+=; test) */

        /* Fused back-edge: i += step; if (i <cmp> bound) goto lbody. */
        Instr fstep;
        fstep.op = OpCode::ForLoopStep;
        fstep.node = f;
        fstep.aop = f->cmp_op;
        fstep.target = lbody;
        fstep.target2 = f->i_slot;
        fstep.a = bound;
        fstep.b = step;
        chunk.code.push_back(fstep);

        const int lend = here();
        chunk.code[jt].target = lend;
        pop_loop(lend, lcont);
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

        Operand ca, cb;
        Op cmp;
        OpCode cmp_opcode;
        const size_t cmark = chunk.code.size();
        reset_temps();
        if (compile_int_cond(f->cond.get(), chunk.code, ca, cmp, cb)) {
            cmp_opcode = OpCode::JumpUnlessIntCmp;
        } else {
            chunk.code.resize(cmark);
            reset_temps();
            if (!compile_float_cond(f->cond.get(), chunk.code, ca, cmp, cb)) {
                chunk.code.resize(start);
                return false;
            }
            cmp_opcode = OpCode::JumpUnlessFloatCmp;
        }

        const size_t jt = emit_cmp(cmp_opcode, f->cond.get(), cmp, ca, cb);

        loops.push_back({});
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
        chunk.code[jt].target = lend;
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
        if (fe->elem_th != TypeHint::i && fe->elem_th != TypeHint::f)
            return false;

        const Identifier *id =
            dynamic_cast<const Identifier *>(fe->ids->elems[0].get());
        if (!id || id->sym.kind != SymKind::local)
            return false;
        const int x_slot = id->sym.slot;

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
        ln.node = fe->container.get();
        ln.target = n;
        ln.target2 = c;
        chunk.code.push_back(ln);

        const int i = alloc_temp();
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
                                           : OpCode::LoadElemFloat;
        ld.node = fe->container.get();
        ld.target = x_slot;
        ld.target2 = c;
        ld.a = slot_op(i);
        chunk.code.push_back(ld);

        loops.push_back({});
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
        fstep.node = fe->container.get();
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
            if (!try_native_foreach(fe))       /* flat int/float array only */
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
        /* `return <expr>;` -> ReturnV (its expr compiled natively). */
        if (const ReturnStmt *ret = dynamic_cast<const ReturnStmt *>(s)) {
            if (try_native_return(ret, chunk.code))
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

}  /* namespace */

Chunk
codegen_chunk(const Block *block, int slot_count)
{
    Codegen cg;
    cg.temp_base = cg.next_temp = cg.max_temp = slot_count;
    cg.gen_stmts(block->elems);
    cg.emit(OpCode::Halt);
    cg.chunk.n_temps = cg.max_temp - slot_count;
    cg.chunk.slot_count = slot_count;
    collect_slot_names(block, cg.chunk.slot_names);   /* -vd debug info */
    return std::move(cg.chunk);
}

Chunk
codegen_program(const Block *root)
{
    return codegen_chunk(root, root->slot_count);
}
