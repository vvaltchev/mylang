/* SPDX-License-Identifier: BSD-2-Clause */

#include "codegen.h"
#include "syntax.h"

#include <vector>

namespace {

/* An int immediate operand. */
Operand int_lit(int_type v)
{
    Operand o;
    o.is_lit = true;
    o.lit = v;
    return o;
}

/*
 * Read `e` as a LEAF int operand: a resolved-local int frame slot, or an int
 * literal. Returns false for anything else (a global/capture slot, a float, a
 * nested expression, a call, ...) - the enclosing loop then falls back. Only
 * `SymKind::local` slots are registers here (main-frame / current call); a
 * bool slot also carries `th == i` and is read defensively at runtime.
 */
bool as_int_operand(const Construct *e, Operand &out)
{
    if (const LiteralInt *li = dynamic_cast<const LiteralInt *>(e)) {
        out = int_lit(li->ival());
        return true;
    }
    if (const Identifier *id = dynamic_cast<const Identifier *>(e)) {
        if (id->sym.kind == SymKind::local && id->th == TypeHint::i) {
            out.is_lit = false;
            out.slot = id->sym.slot;
            return true;
        }
    }
    return false;
}

/*
 * Compile statement `s` to native int op(s) appended to `out`. Handles a
 * compound assignment (`x += y` etc.) and `x++`/`x--` where the target is a
 * resolved-local int slot and the rhs is a leaf int operand. Returns false
 * otherwise (plain assign, nested rhs, float, a call, ...) so the loop falls
 * back. Postfix vs prefix on `++`/`--` is irrelevant as a statement.
 */
bool gen_int_stmt(const Construct *s, std::vector<Instr> &out)
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
            out.push_back(in);
            return true;
        }
        return false;
    }

    if (const Expr14 *e = dynamic_cast<const Expr14 *>(s)) {
        Operand dst, rhs;
        if (!as_int_operand(e->lvalue.get(), dst) || dst.is_lit)
            return false;

        Op arith;
        switch (e->op) {
            case Op::addeq: arith = Op::plus;  break;
            case Op::subeq: arith = Op::minus; break;
            case Op::muleq: arith = Op::times; break;
            case Op::diveq: arith = Op::div;   break;
            case Op::modeq: arith = Op::mod;   break;
            /* plain assign + others: fall back (compiled in a later step). */
            default: return false;
        }

        if (!as_int_operand(e->rvalue.get(), rhs))
            return false;

        Instr in;                       /* dst = dst <arith> rhs */
        in.op = OpCode::IntBin;
        in.node = s;
        in.target = dst.slot;
        in.a = dst;
        in.b = rhs;
        in.aop = arith;
        out.push_back(in);
        return true;
    }

    return false;
}

/*
 * Read a while condition as a fused int comparison of two leaf operands. After
 * specialize_types, `i < N` is a TypedScalarExpr(cat=cmp, kind=i) with elems
 * [(invalid, a), (cmpOp, b)]. Returns false for any other shape.
 */
bool gen_int_cond(const Construct *cond, Operand &a, Op &cmp, Operand &b)
{
    const TypedScalarExpr *t = dynamic_cast<const TypedScalarExpr *>(cond);
    if (!t || t->cat != TypedScalarExpr::Cat::cmp || t->kind != TypeHint::i)
        return false;
    if (t->elems.size() != 2)
        return false;
    cmp = t->elems[1].first;
    return as_int_operand(t->elems[0].second.get(), a)
        && as_int_operand(t->elems[1].second.get(), b);
}

/*
 * Lowers a statement list to a Chunk. `if` and `while` become native jumps
 * (Phase 1); a `while` whose condition + body are all resolved-local int ops
 * (Phase 2) compiles with no tree-walker fallback. `for` is NOT flattened -
 * ForStmt::do_eval wraps its body in a child EvalContext (loop-variable scope)
 * a naive flatten would drop. See plans/bytecode-vm.md.
 */
struct Codegen {

    Chunk chunk;

    int here() const { return static_cast<int>(chunk.code.size()); }

    size_t emit(OpCode op, const Construct *node = nullptr,
                int target = -1, int target2 = -1)
    {
        chunk.code.push_back({ op, node, target, target2 });
        return chunk.code.size() - 1;
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
        if (try_native_int_while(w))       /* Phase 2 register fast path */
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

    /*
     * A resolved-local int scalar loop -> native register ops (no fallback):
     *   Lstart: JumpUnlessIntCmp{a,cmp,b -> Lend} ; <body IntBin ops> ;
     *           Jump Lstart ; Lend:
     * Fires only when the condition is a leaf int compare AND every body
     * statement is a compilable int assignment/inc-dec (so there is no decl to
     * scope and no break/continue/return - hence no LoopBackEdge needed).
     * Returns false (emitting nothing) otherwise.
     */
    bool try_native_int_while(const WhileStmt *w)
    {
        Operand ca, cb;
        Op cmp;
        if (!gen_int_cond(w->condExpr.get(), ca, cmp, cb))
            return false;

        /* Gather body statements (a Block's elems, or a single statement). */
        std::vector<const Construct *> stmts;
        if (const Construct *body = w->body.get()) {
            if (const Block *b = dynamic_cast<const Block *>(body)) {
                for (const auto &e : b->elems)
                    stmts.push_back(e.get());
            } else {
                stmts.push_back(body);
            }
        }

        /* Compile every body statement first; bail (no partial emit) on any. */
        std::vector<Instr> body_ops;
        for (const Construct *s : stmts)
            if (!gen_int_stmt(s, body_ops))
                return false;

        const int lstart = here();

        Instr test;
        test.op = OpCode::JumpUnlessIntCmp;
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
        return true;
    }
};

}  /* namespace */

Chunk
codegen_program(const Block *root)
{
    Codegen cg;
    cg.gen_stmts(root->elems);
    cg.emit(OpCode::Halt);
    return std::move(cg.chunk);
}
