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

/* An operand as "smart assembly": a register slot `rN` (unbounded - the VM's
 * registers ARE the frame slots) or an immediate `#N`. */
std::string reg_or_imm(const Operand &o, bool is_float)
{
    if (!o.is_lit)
        return "r" + std::to_string(o.slot);
    std::ostringstream s;
    s << "#";
    if (is_float)
        s << o.flit;
    else
        s << o.lit;
    return s.str();
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
      << chunk.n_temps << " temps) =====\n";

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
            row << "i.bin        r" << in.target << " = "
                << reg_or_imm(in.a, false) << " " << opsym(in.aop) << " "
                << reg_or_imm(in.b, false);
            break;
        case OpCode::JumpUnlessIntCmp:
            row << "i.jmp.ifnot  " << reg_or_imm(in.a, false) << " "
                << opsym(in.aop) << " " << reg_or_imm(in.b, false)
                << ", L" << in.target;
            break;
        case OpCode::FloatBin:
            row << "f.bin        r" << in.target << " = "
                << reg_or_imm(in.a, true) << " " << opsym(in.aop) << " "
                << reg_or_imm(in.b, true);
            break;
        case OpCode::JumpUnlessFloatCmp:
            row << "f.jmp.ifnot  " << reg_or_imm(in.a, true) << " "
                << opsym(in.aop) << " " << reg_or_imm(in.b, true)
                << ", L" << in.target;
            break;
        case OpCode::ForLoopStep:
            row << "for.step     r" << in.target2 << " "
                << (in.aop == Op::lt || in.aop == Op::le ? "+=" : "-=") << " "
                << reg_or_imm(in.b, false) << ", if r" << in.target2 << " "
                << opsym(in.aop) << " " << reg_or_imm(in.a, false)
                << " -> L" << in.target;
            break;
        case OpCode::LoadElemInt:
            row << "load.elem.i  r" << in.target << " = r" << in.target2
                << "[" << reg_or_imm(in.a, false) << "]";
            break;
        case OpCode::LoadElemFloat:
            row << "load.elem.f  r" << in.target << " = r" << in.target2
                << "[" << reg_or_imm(in.a, false) << "]";
            break;
        case OpCode::LoadElemValue:
            row << "load.elem.v  r" << in.target << " = r" << in.target2
                << "[" << reg_or_imm(in.a, false) << "]  (array elem)";
            break;
        case OpCode::ArrLen:
            row << "arr.len      r" << in.target << " = len(r"
                << in.target2 << ")";
            break;
        case OpCode::StoreElemInt:
            row << "store.elem.i r" << in.target2 << "["
                << reg_or_imm(in.a, false) << "] " << store_op(in.aop) << " "
                << reg_or_imm(in.b, false);
            break;
        case OpCode::StoreElemFloat:
            row << "store.elem.f r" << in.target2 << "["
                << reg_or_imm(in.a, false) << "] " << store_op(in.aop) << " "
                << reg_or_imm(in.b, true);
            break;
        case OpCode::EvalToSlot:
            row << "eval.slot    r" << in.target << " = " << node1(in.node);
            break;
        case OpCode::CallBuiltinV:
            row << "call.blt.v   r" << in.target << " = " << node1(in.node);
            break;
        case OpCode::CallBuiltinLV:
            row << "call.blt.lv  r" << in.target << " = " << node1(in.node)
                << "  (&r" << in.target2 << ")";
            break;
        case OpCode::CallV:
            row << "call.v       r" << in.target << " = g" << in.target2
                << "(r" << in.a.lit << "..+" << in.b.lit << ")";
            break;
        case OpCode::CachedCallV:
            row << "call.cached  r" << in.target << " = g" << in.target2
                << "(r" << in.a.lit << "..+" << in.b.lit << ")";
            break;
        case OpCode::ReturnV:
            row << "return.v     " << reg_or_imm(in.a, false);
            break;
        case OpCode::LoadImmInt:
            row << "load         r" << in.target << ", #" << in.a.lit;
            break;
        case OpCode::LoadImmFloat:
            row << "load         r" << in.target << ", #" << in.a.flit;
            break;
        case OpCode::LoadConstV: {
            const EvalValue &c = chunk.consts[in.target2];
            row << "load.v       r" << in.target << ", "
                << c.get_type()->to_string_repr(c);
            break;
        }
        case OpCode::MoveV:
            row << "move.v       r" << in.target << " = r" << in.target2;
            break;
        case OpCode::BinOpV:
            row << "bin.v        r" << in.target << " = "
                << reg_or_imm(in.a, false) << " " << opsym(in.aop) << " "
                << reg_or_imm(in.b, false);
            break;
        case OpCode::CompoundV:
            row << "compound.v   r" << in.target << " " << opsym(in.aop)
                << "= " << reg_or_imm(in.b, false);
            break;
        case OpCode::CmpV:
            row << "cmp.v        r" << in.target << " = "
                << reg_or_imm(in.a, false) << " " << opsym(in.aop) << " "
                << reg_or_imm(in.b, false);
            break;
        case OpCode::LogV:
            row << "log.v        r" << in.target << " = "
                << reg_or_imm(in.a, false) << " " << opsym(in.aop) << " "
                << reg_or_imm(in.b, false);
            break;
        case OpCode::LoadGlobalV:
            row << "load.global  r" << in.target << ", g" << in.target2;
            break;
        case OpCode::LoadCaptureV:
            row << "load.capture r" << in.target << ", c" << in.target2;
            break;
        case OpCode::LoadBuiltinV:
            row << "load.builtin r" << in.target << ", b" << in.target2;
            break;
        case OpCode::SubscriptV:
            row << "subscript.v  r" << in.target << " = r" << in.target2
                << "[" << reg_or_imm(in.a, false) << "]";
            break;
        case OpCode::MemberV:
            row << "member.v     r" << in.target << " = r" << in.target2
                << ".<member>";
            break;
        case OpCode::JumpUnlessTrueV:
            row << "jmp.ifnot.v  r" << in.target2 << ", L" << in.target;
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
