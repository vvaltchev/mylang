/* SPDX-License-Identifier: BSD-2-Clause */

#include "disasm.h"
#include "codegen.h"      /* codegen_program / codegen_chunk */
#include "coderender.h"   /* render_construct_code - shared AST decompiler */
#include "syntax.h"
#include "lexer.h"        /* OpString */

#include <sstream>
#include <iomanip>
#include <vector>

namespace {

/* A register slot as text: the source VARIABLE NAME for a resolved local
 * (slot < slot_count), else `rN` for a scratch temp (the VM's registers ARE the
 * frame slots; a temp has no name). So a loop reads `s = s + i`, not
 * `r3 = r3 + r2`. */
std::string reg(const Chunk &ch, int slot)
{
    if (slot >= 0 && slot < ch.slot_count
        && static_cast<size_t>(slot) < ch.slot_names.size()
        && !ch.slot_names[slot].empty())
        return ch.slot_names[slot];
    return "r" + std::to_string(slot);
}

/* An operand as "smart assembly": a register (named or `rN`) or an immediate
 * `#N`. The immediate is rendered by its lit_kind (int / float / bool), so a
 * boxed op's `#3` / `#1.5` / `#true` prints correctly. (`is_float` is now
 * subsumed by lit_kind, kept only for call-site compatibility.) */
std::string reg_or_imm(const Chunk &ch, const Operand &o, bool /*is_float*/)
{
    if (!o.is_lit)
        return reg(ch, o.slot);
    std::ostringstream s;
    s << "#";
    switch (o.lit_kind) {
    case Operand::LitKind::f: s << o.flit; break;
    case Operand::LitKind::b: s << (o.lit ? "true" : "false"); break;
    default:                  s << o.lit; break;
    }
    return s.str();
}

/* The callee's source name for a CallExpr node (`len`, `print`, `fib`, ...). */
std::string callee_name(const Construct *node)
{
    if (auto *call = dynamic_cast<const CallExpr *>(node))
        if (auto *id = dynamic_cast<const Identifier *>(call->what.get()))
            return std::string(id->get_str());
    return "?";
}

/* A call's argument registers as `(a, b, c)` from the run [base, base+n). */
std::string arglist(const Chunk &ch, int base, int n)
{
    std::string s = "(";
    for (int i = 0; i < n; i++) {
        if (i)
            s += ", ";
        s += reg(ch, base + i);
    }
    return s + ")";
}

/* The Identifier name behind a load/global/capture/builtin op's node. */
std::string node_name(const Construct *node)
{
    if (auto *id = dynamic_cast<const Identifier *>(node))
        return std::string(id->get_str());
    return "?";
}

std::string opsym(Op op)
{
    return OpString[static_cast<int>(op)];
}

/* A fallback op's still-embedded Construct*, rendered by the SHARED AST
 * decompiler and collapsed to one trimmed, truncated line so the op stays a
 * single disassembly row. */
std::string node1(const Construct *node)
{
    if (!node)
        return "<null>";
    const std::string code = render_construct_code(node);
    std::string out;
    bool space = false;
    for (char c : code) {
        if (c == '\n' || c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !out.empty())
            out += ' ';
        space = false;
        out += c;
    }
    const size_t cap = 66;
    if (out.size() > cap)
        out = out.substr(0, cap - 3) + "...";
    return out;
}

std::string store_op(Op aop)
{
    return aop == Op::invalid ? std::string("=") : opsym(aop) + "=";
}

/* Find every FuncDeclStmt reachable through Blocks / function bodies (top-level
 * and function-nested). A function inside a loop/if body is not walked - rare,
 * and a step-1 limitation of the -vd driver. */
void collect_funcs(const Construct *c, std::vector<const FuncDeclStmt *> &out)
{
    if (!c)
        return;
    if (const FuncDeclStmt *fn = dynamic_cast<const FuncDeclStmt *>(c)) {
        out.push_back(fn);
        collect_funcs(fn->body.get(), out);
        return;
    }
    if (const Block *b = dynamic_cast<const Block *>(c))
        for (const auto &e : b->elems)
            collect_funcs(e.get(), out);
}

}  /* namespace */

std::string disassemble(const Chunk &chunk, const std::string &title)
{
    std::ostringstream s;
    s << "; ===== " << title << "  (" << chunk.code.size() << " instr, "
      << chunk.n_temps << " temps) =====\n"
      << "; registers: a source var reads by NAME; `rN` is a scratch temp\n";

    /* Terse operand helpers bound to this chunk (names + immediates), and the
     * `; source` comment column an op carries its AST snippet in. */
    auto D  = [&](int slot)                 { return reg(chunk, slot); };
    auto RI = [&](const Operand &o, bool f) { return reg_or_imm(chunk, o, f); };
    auto cmt = [&](std::ostream &r, const Construct *n) {
        if (n)
            r << "   ; " << node1(n);
    };
    /* arg0's lvalue target for a CallBuiltinLV, by slot kind (a.lit). */
    auto lval_ref = [&](int kind, int slot) -> std::string {
        if (kind == 0) return "&" + reg(chunk, slot);        /* local */
        if (kind == 1) return "&g" + std::to_string(slot);   /* global */
        return "&c" + std::to_string(slot);                  /* capture */
    };

    for (size_t pc = 0; pc < chunk.code.size(); pc++) {
        const Instr &in = chunk.code[pc];
        std::ostringstream row;

        switch (in.op) {
        case OpCode::EvalStmt:
            row << "eval.stmt    " << node1(in.node);
            break;
        case OpCode::Jump:
            row << "jmp          L" << in.target;
            break;
        case OpCode::JumpIfFalse:
            row << "jmp.ifnot    (" << node1(in.node) << "), L" << in.target;
            break;
        case OpCode::LoopBackEdge:
            row << "loop.back    cont=L" << in.target << " brk=L" << in.target2;
            break;
        case OpCode::IntBin:
            row << "i.bin        " << D(in.target) << " = "
                << RI(in.a, false) << " " << opsym(in.aop) << " "
                << RI(in.b, false);
            break;
        case OpCode::JumpUnlessIntCmp:
            row << "i.jmp.ifnot  " << RI(in.a, false) << " "
                << opsym(in.aop) << " " << RI(in.b, false)
                << ", L" << in.target;
            break;
        case OpCode::FloatBin:
            row << "f.bin        " << D(in.target) << " = "
                << RI(in.a, true) << " " << opsym(in.aop) << " "
                << RI(in.b, true);
            break;
        case OpCode::JumpUnlessFloatCmp:
            row << "f.jmp.ifnot  " << RI(in.a, true) << " "
                << opsym(in.aop) << " " << RI(in.b, true)
                << ", L" << in.target;
            break;
        case OpCode::ForLoopStep:
            row << "for.step     " << D(in.target2) << " "
                << (in.aop == Op::lt || in.aop == Op::le ? "+=" : "-=") << " "
                << RI(in.b, false) << ", if " << D(in.target2) << " "
                << opsym(in.aop) << " " << RI(in.a, false)
                << " -> L" << in.target;
            break;
        case OpCode::LoadElemInt:
            row << "load.elem.i  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a, false) << "]";
            break;
        case OpCode::LoadElemFloat:
            row << "load.elem.f  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a, false) << "]";
            break;
        case OpCode::LoadElemValue:
            row << "load.elem.v  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a, false) << "]";
            break;
        case OpCode::DictLoadInt:
        case OpCode::DictLoadFloat: {
            const char *mn = in.op == OpCode::DictLoadInt
                ? "dict.load.i  " : "dict.load.f  ";
            /* AST-free: a member's key is const-pool[a.lit] (an immediate), a
             * subscript's key is a temp slot. `node` is null (loc table). */
            row << mn << D(in.target) << " = " << D(in.target2) << "[";
            if (in.a.is_lit)
                row << chunk.consts[in.a.lit].get_type()->to_string(
                           chunk.consts[in.a.lit]);
            else
                row << RI(in.a, false);
            row << "]";
            break;
        }
        case OpCode::ArrLen:
            row << "arr.len      " << D(in.target) << " = len("
                << D(in.target2) << ")";
            break;
        case OpCode::StoreElemInt:
            row << "store.elem.i " << D(in.target2) << "["
                << RI(in.a, false) << "] " << store_op(in.aop) << " "
                << RI(in.b, false);
            break;
        case OpCode::StoreElemFloat:
            row << "store.elem.f " << D(in.target2) << "["
                << RI(in.a, false) << "] " << store_op(in.aop) << " "
                << RI(in.b, true);
            break;
        case OpCode::DictStore: {
            /* aop is the Expr14 op (assign/addeq/...), not the arith form. */
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "dict.store   " << D(in.target2) << "["
                << RI(in.a, false) << "] " << o << " " << RI(in.b, false);
            break;
        }
        case OpCode::StoreElemValue: {
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "store.elem.v " << D(in.target2) << "["
                << RI(in.a, false) << "] " << o << " " << RI(in.b, false);
            break;
        }
        case OpCode::EvalToSlot:
            row << "eval.slot    " << D(in.target) << " = " << node1(in.node);
            break;
        case OpCode::CallBuiltinV:
            row << "call.blt.v   " << D(in.target) << " = "
                << callee_name(in.node)
                << arglist(chunk, in.a.lit, in.b.lit);
            cmt(row, in.node);
            break;
        case OpCode::CallBuiltinLV:
            row << "call.blt.lv  " << D(in.target) << " = "
                << callee_name(in.node)
                << "(" << lval_ref(in.a.lit, in.target2) << ", ...)";
            cmt(row, in.node);
            break;
        case OpCode::EmplaceStruct: {
            const auto *dc =
                static_cast<const DirectBuiltinCallExpr *>(in.node);
            const auto *ctor =
                static_cast<const CallExpr *>(dc->args->elems[1].get());
            const int nf = static_cast<int>(ctor->args->elems.size());
            row << "emplace      " << D(in.target) << " = "
                << callee_name(in.node) << "("
                << lval_ref(in.a.lit, in.target2) << " <- "
                << std::string(ctor->vm_struct_ctor_def->name->val)
                << arglist(chunk, in.b.lit, nf) << ")";
            cmt(row, in.node);
            break;
        }
        case OpCode::CallBuiltinLVElem:
            row << "call.blt.lve " << D(in.target) << " = "
                << callee_name(in.node) << "("
                << lval_ref(in.a.lit, in.target2) << "[" << RI(in.b, false)
                << "], ...)";
            cmt(row, in.node);
            break;
        case OpCode::CallV:
            row << "call.v       " << D(in.target) << " = "
                << callee_name(in.node)
                << arglist(chunk, in.a.lit, in.b.lit);
            cmt(row, in.node);
            break;
        case OpCode::CachedCallV:
            row << "call.cached  " << D(in.target) << " = "
                << callee_name(in.node)
                << arglist(chunk, in.a.lit, in.b.lit);
            cmt(row, in.node);
            break;
        case OpCode::ReturnV:
            row << "return.v     " << RI(in.a, false);
            break;
        case OpCode::LoadImmInt:
            row << "load         " << D(in.target) << ", #" << in.a.lit;
            break;
        case OpCode::LoadImmFloat:
            row << "load         " << D(in.target) << ", #" << in.a.flit;
            break;
        case OpCode::LoadConstV: {
            const EvalValue &c = chunk.consts[in.target2];
            row << "load         " << D(in.target) << ", "
                << c.get_type()->to_string_repr(c);
            break;
        }
        case OpCode::MoveV:
            row << "move         " << D(in.target) << " = " << D(in.target2);
            break;
        case OpCode::BinOpV:
            row << "bin.v        " << D(in.target) << " = "
                << RI(in.a, false) << " " << opsym(in.aop) << " "
                << RI(in.b, false) << "   ; boxed";
            break;
        case OpCode::CompoundV:
            row << "compound.v   " << D(in.target) << " " << opsym(in.aop)
                << "= " << RI(in.b, false) << "   ; boxed";
            break;
        case OpCode::CmpV:
            row << "cmp.v        " << D(in.target) << " = "
                << RI(in.a, false) << " " << opsym(in.aop) << " "
                << RI(in.b, false) << "   ; boxed";
            break;
        case OpCode::LogV:
            row << "log.v        " << D(in.target) << " = "
                << RI(in.a, false) << " " << opsym(in.aop) << " "
                << RI(in.b, false) << "   ; boxed";
            break;
        case OpCode::LoadGlobalV:
            /* AST-free: the global name lives in gfuncs, not the chunk, so a
             * disassembly shows the global slot (g<n>). */
            row << "load.global  " << D(in.target) << ", g" << in.target2;
            break;
        case OpCode::LoadCaptureV:
            row << "load.capture " << D(in.target) << ", "
                << node_name(in.node);
            break;
        case OpCode::LoadBuiltinV:
            row << "load.builtin " << D(in.target) << ", "
                << node_name(in.node);
            break;
        case OpCode::SubscriptV:
            row << "subscript.v  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a, false) << "]";
            cmt(row, in.node);
            break;
        case OpCode::MemberV:
            row << "member.v     " << D(in.target) << " = " << D(in.target2)
                << ".<member>";
            cmt(row, in.node);
            break;
        case OpCode::JumpUnlessTrueV:
            row << "jmp.ifnot.v  " << D(in.target2) << ", L" << in.target;
            break;
        case OpCode::Halt:
            row << "halt";
            break;
        }

        s << std::setw(4) << pc << "  " << row.str() << "\n";
    }
    return s.str();
}

std::string disassemble_program(const Block *root)
{
    std::ostringstream s;
    s << disassemble(codegen_program(root), "main");

    std::vector<const FuncDeclStmt *> funcs;
    for (const auto &e : root->elems)
        collect_funcs(e.get(), funcs);

    for (const FuncDeclStmt *fn : funcs) {
        if (!fn->body || !fn->body->is_block())
            continue;
        const Block *body = static_cast<const Block *>(fn->body.get());
        std::string name = fn->id ? std::string(fn->id->get_str())
                                  : "<anon>";
        s << "\n" << disassemble(codegen_chunk(body, fn->frame_size),
                                 "func " + name);
    }
    return s.str();
}
