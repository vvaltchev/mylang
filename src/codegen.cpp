/* SPDX-License-Identifier: BSD-2-Clause */

#include "codegen.h"
#include "syntax.h"

#include <vector>

namespace {

Operand int_lit(int_type v)
{
    Operand o;
    o.is_lit = true;
    o.lit = v;
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
        int acc;
        if (!compile_boxed_expr(elems[0].second.get(), acc, ops))
            return false;
        for (size_t i = 1; i < elems.size(); i++) {
            int rhs;
            if (!compile_boxed_expr(elems[i].second.get(), rhs, ops))
                return false;
            const int t = alloc_temp();
            Instr in;
            in.op = k == 'a' ? OpCode::BinOpV
                  : k == 'c' ? OpCode::CmpV
                             : OpCode::LogV;
            in.node = k == 'l' ? node : elems[i].second.get();
            in.target = t;
            in.a = slot_op(acc);
            in.b = slot_op(rhs);
            in.aop = elems[i].first;
            ops.push_back(in);
            acc = t;
        }
        out_slot = acc;
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
        int rslot;
        if (!compile_boxed_expr(e->rvalue.get(), rslot, ops)) {
            ops.resize(omark);
            chunk.consts.resize(cmark);
            return false;
        }

        /* Compound-assign `lv OP= rhs` -> a boxed read-modify-write. */
        if (!is_assign) {
            Instr in;
            in.op = OpCode::CompoundV;
            in.node = s;
            in.target = lv->sym.slot;
            in.b = slot_op(rslot);
            in.aop = cbase;
            ops.push_back(in);
            return true;
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
        if (e->th == TypeHint::i
            && dynamic_cast<const DirectBuiltinCallExpr *>(e)) {
            out = eval_to_temp(e, ops);
            return true;
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

        /* A scalar-result BUILTIN call -> eval into a temp; the result is then
         * a native float operand (builtins only - see compile_int_expr). */
        if (e->th == TypeHint::f
            && dynamic_cast<const DirectBuiltinCallExpr *>(e)) {
            out = eval_to_temp(e, ops);
            return true;
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
    return std::move(cg.chunk);
}

Chunk
codegen_program(const Block *root)
{
    return codegen_chunk(root, root->slot_count);
}
