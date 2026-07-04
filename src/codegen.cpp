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
     * Compile int expression `e` into `ops`, leaving the result in `out` (a
     * slot or immediate). A leaf costs no op; an arith/neg TypedScalarExpr emits
     * IntBin(s) into temp slots. Returns false for a non-int expression (a
     * comparison, a call, a subscript, ...) so the enclosing loop falls back.
     */
    bool compile_int_expr(const Construct *e, Operand &out,
                          std::vector<Instr> &ops)
    {
        if (as_int_operand(e, out))
            return true;

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
             * write `dst` directly; else emit a move (dst = r + 0). */
            if (!r.is_lit && !ops.empty() && ops.back().op == OpCode::IntBin
                && ops.back().target == r.slot) {
                ops.back().target = dst.slot;
            } else {
                Instr in;
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
            } else {
                Instr in;                /* dst = r + 0.0 */
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

    /* Gather a while body's statements (a Block's elems, or a single stmt). */
    std::vector<const Construct *> body_stmts(const WhileStmt *w)
    {
        std::vector<const Construct *> stmts;
        if (const Construct *body = w->body.get()) {
            if (const Block *b = dynamic_cast<const Block *>(body))
                for (const auto &e : b->elems)
                    stmts.push_back(e.get());
            else
                stmts.push_back(body);
        }
        return stmts;
    }

    /* Emit a compiled native loop: Lstart <cond ops> JumpUnless{cmp_op} <body
     * ops> Jump Lstart ; Lend. */
    void emit_native_while(const WhileStmt *w, OpCode cmp_op, Op cmp,
                           Operand ca, Operand cb,
                           const std::vector<Instr> &cond_ops,
                           const std::vector<Instr> &body_ops)
    {
        const int lstart = here();
        for (const Instr &in : cond_ops)
            chunk.code.push_back(in);

        Instr test;
        test.op = cmp_op;
        test.node = w->condExpr.get();
        test.aop = cmp;
        test.a = ca;
        test.b = cb;
        const size_t jt = chunk.code.size();
        chunk.code.push_back(test);      /* target = Lend, patched below */

        for (const Instr &in : body_ops)
            chunk.code.push_back(in);

        emit(OpCode::Jump, nullptr, lstart);
        chunk.code[jt].target = here();  /* Lend */
    }

    /*
     * A resolved-local scalar (int OR float) loop -> native register ops, no
     * fallback:
     *   Lstart: <cond ops> JumpUnless{Int,Float}Cmp{a,cmp,b -> Lend}
     *           <body ops> Jump Lstart ; Lend:
     * Fires when the condition is an int/float comparison and every body
     * statement is a compilable int/float assignment/inc-dec (so no decl to
     * scope, no break/continue/return). The condition and each body statement
     * are dispatched by their OWN kind, so a MIXED int/float loop compiles too;
     * any unsupported statement emits nothing and returns false (-> Phase 1).
     */
    bool try_native_scalar_while(const WhileStmt *w)
    {
        std::vector<Instr> cond_ops, body_ops;
        Operand ca, cb;
        Op cmp;
        OpCode cmp_opcode;

        /* Condition: int or float compare (per-condition dispatch). A failed
         * int attempt may have appended partial operand ops - clear before
         * retrying float. */
        reset_temps();
        if (compile_int_cond(w->condExpr.get(), cond_ops, ca, cmp, cb)) {
            cmp_opcode = OpCode::JumpUnlessIntCmp;
        } else {
            cond_ops.clear();
            reset_temps();
            if (!compile_float_cond(w->condExpr.get(), cond_ops, ca, cmp, cb))
                return false;
            cmp_opcode = OpCode::JumpUnlessFloatCmp;
        }

        /* Body: each statement dispatched by its OWN kind (int OR float), so a
         * MIXED loop - `while (i < n) { f += 0.5; i++; }` - compiles. A failed
         * int attempt may leave partial ops; truncate before trying float. */
        for (const Construct *s : body_stmts(w)) {
            reset_temps();
            const size_t mark = body_ops.size();
            if (compile_int_stmt(s, body_ops))
                continue;
            body_ops.resize(mark);
            reset_temps();
            if (!compile_float_stmt(s, body_ops))
                return false;
        }

        emit_native_while(w, cmp_opcode, cmp, ca, cb, cond_ops, body_ops);
        return true;
    }

    void gen_stmts(const std::vector<unique_ptr<Construct>> &elems)
    {
        for (const auto &e : elems)
            gen_stmt(e.get());
    }

    void gen_stmt(const Construct *s)
    {
        if (const IfStmt *f = dynamic_cast<const IfStmt *>(s)) {
            gen_if(f);
            return;
        }
        if (const WhileStmt *w = dynamic_cast<const WhileStmt *>(s)) {
            gen_while(w);
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
codegen_program(const Block *root)
{
    Codegen cg;
    cg.temp_base = cg.next_temp = cg.max_temp = root->slot_count;
    cg.gen_stmts(root->elems);
    cg.emit(OpCode::Halt);
    cg.chunk.n_temps = cg.max_temp - root->slot_count;
    return std::move(cg.chunk);
}
