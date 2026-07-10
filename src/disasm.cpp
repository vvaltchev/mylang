/* SPDX-License-Identifier: BSD-2-Clause */

#include "disasm.h"
#include "codegen.h"      /* codegen_program / codegen_chunk */
#include "coderender.h"   /* render_construct_code - shared AST decompiler */
#include "syntax.h"
#include "structtype.h"   /* StructTypeDef / FieldDef (the custom-type dump) */
#include "errors.h"       /* InlineCtx (the inline_ctxs pool dump) */
#include "lexer.h"        /* OpString */

#include <sstream>
#include <iomanip>
#include <vector>

namespace {

/* A register slot as text: the source VARIABLE NAME for a resolved local
 * (slot < slot_count), else `rN` for a scratch temp (the VM's registers ARE the
 * frame slots; a temp has no name). So a loop reads `s = s + i`, not
 * `r3 = r3 + r2`. */
std::string reg(const Chunk &ch, int_type slot)
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
std::string arglist(const Chunk &ch, int_type base, int_type n)
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

/* Find every FuncDeclStmt reachable from `c` - a COMPLETE walk, so a lambda in
 * ANY expression position (a `return func[..]{..}`, a `var f = func..`, a call
 * arg, a ternary branch, ...) is disassembled too, not only top-level / body-
 * statement functions. Each FuncDeclStmt's own body is recursed for further
 * nested closures. */
void collect_funcs(const Construct *c, std::vector<const FuncDeclStmt *> &out)
{
    if (!c)
        return;
    if (const FuncDeclStmt *fn = dynamic_cast<const FuncDeclStmt *>(c)) {
        out.push_back(fn);
        collect_funcs(fn->body.get(), out);   /* nested closures within */
        return;
    }
    auto rec = [&](const Construct *ch) { collect_funcs(ch, out); };
    if (const Block *b = dynamic_cast<const Block *>(c)) {
        for (const auto &e : b->elems) rec(e.get());
    } else if (auto *sc = dynamic_cast<const SingleChildConstruct *>(c)) {
        rec(sc->elem.get());
    } else if (auto *mo = dynamic_cast<const MultiOpConstruct *>(c)) {
        for (auto &p : mo->elems) rec(p.second.get());
    } else if (auto *ts = dynamic_cast<const TypedScalarExpr *>(c)) {
        for (auto &p : ts->elems) rec(p.second.get());
    } else if (auto *me = dynamic_cast<const MultiElemConstruct<> *>(c)) {
        for (auto &e : me->elems) rec(e.get());
    } else if (auto *e = dynamic_cast<const Expr14 *>(c)) {
        rec(e->lvalue.get()); rec(e->rvalue.get());
    } else if (auto *ce = dynamic_cast<const CallExpr *>(c)) {
        rec(ce->what.get()); rec(ce->args.get());
    } else if (auto *sub = dynamic_cast<const Subscript *>(c)) {
        rec(sub->what.get()); rec(sub->index.get());
    } else if (auto *m = dynamic_cast<const MemberExpr *>(c)) {
        rec(m->what.get());
    } else if (auto *ret = dynamic_cast<const ReturnStmt *>(c)) {
        rec(ret->elem.get());
    } else if (auto *iff = dynamic_cast<const IfStmt *>(c)) {
        rec(iff->condExpr.get()); rec(iff->thenBlock.get());
        rec(iff->elseBlock.get());
    } else if (auto *w = dynamic_cast<const WhileStmt *>(c)) {
        rec(w->condExpr.get()); rec(w->body.get());
    } else if (auto *f = dynamic_cast<const ForStmt *>(c)) {
        rec(f->init.get()); rec(f->cond.get()); rec(f->inc.get());
        rec(f->body.get());
    } else if (auto *fr = dynamic_cast<const ForRangeStmt *>(c)) {
        rec(fr->init.get()); rec(fr->bound.get()); rec(fr->step.get());
        rec(fr->body.get());
    } else if (auto *fe = dynamic_cast<const ForeachStmt *>(c)) {
        rec(fe->container.get()); rec(fe->body.get());
    } else if (auto *te = dynamic_cast<const TernaryExpr *>(c)) {
        rec(te->condExpr.get()); rec(te->thenExpr.get());
        rec(te->elseExpr.get());
    } else if (auto *co = dynamic_cast<const CoalesceExpr *>(c)) {
        rec(co->lhs.get()); rec(co->rhs.get());
    }
}

/* Every StructDeclStmt reachable from `c` (a custom type definition). Structs
 * appear only as STATEMENTS, so only the statement-containers are walked. */
void collect_structs(const Construct *c,
                     std::vector<const StructDeclStmt *> &out)
{
    if (!c)
        return;
    if (auto *sd = dynamic_cast<const StructDeclStmt *>(c)) {
        out.push_back(sd);
        return;
    }
    auto rec = [&](const Construct *ch) { collect_structs(ch, out); };
    if (auto *b = dynamic_cast<const Block *>(c)) {
        for (const auto &e : b->elems) rec(e.get());
    } else if (auto *fn = dynamic_cast<const FuncDeclStmt *>(c)) {
        rec(fn->body.get());
    } else if (auto *iff = dynamic_cast<const IfStmt *>(c)) {
        rec(iff->thenBlock.get()); rec(iff->elseBlock.get());
    } else if (auto *w = dynamic_cast<const WhileStmt *>(c)) {
        rec(w->body.get());
    } else if (auto *f = dynamic_cast<const ForStmt *>(c)) {
        rec(f->body.get());
    } else if (auto *fr = dynamic_cast<const ForRangeStmt *>(c)) {
        rec(fr->body.get());
    } else if (auto *fe = dynamic_cast<const ForeachStmt *>(c)) {
        rec(fe->body.get());
    } else if (auto *t = dynamic_cast<const TryCatchStmt *>(c)) {
        rec(t->tryBody.get()); rec(t->finallyBody.get());
        for (const auto &cs : t->catchStmts) rec(cs.second.get());
    }
}

const char *field_kind_str(FieldKind k)
{
    switch (k) {
    case FieldKind::f_bool:   return "bool";
    case FieldKind::f_int:    return "int";
    case FieldKind::f_float:  return "float";
    case FieldKind::f_str:    return "str";
    case FieldKind::f_array:  return "array";
    case FieldKind::f_dict:   return "dict";
    case FieldKind::f_dyn:    return "dyn";
    case FieldKind::f_struct: return "struct";
    }
    return "?";
}

/* One custom TYPE definition - name, layout (POD byte offsets / boxed slots),
 * fields, and folded consts. This is what a `.myv` file must store per struct
 * type so an instance can be laid out + type-checked on load. */
void dump_struct_type(const StructTypeDef *def, std::ostringstream &s)
{
    const bool pod = def->layout == StructTypeDef::Layout::pod;
    s << "; struct " << def->name->val << "  [" << (pod ? "pod" : "boxed");
    if (pod)
        s << " size=" << def->size << " align=" << def->align;
    s << "]\n";
    for (const auto &f : def->fields) {
        s << ";     ";
        if (f.is_opt)
            s << "opt ";
        if (f.kind == FieldKind::f_struct && f.struct_ty)
            s << f.struct_ty->val;
        else
            s << field_kind_str(f.kind);
        s << " " << f.name->val;
        if (f.offset >= 0)
            s << " @" << f.offset;                 /* POD byte offset */
        else if (f.slot >= 0)
            s << " (slot " << f.slot << ")";       /* boxed slot */
        s << "\n";
    }
    for (const auto &c : def->consts)
        s << ";     const " << c.first->val << " = "
          << c.second.get_type()->to_string_repr(c.second) << "\n";
}

/* The chunk's serializable POOLS + side tables, printed after its code (a
 * `.myv` file stores exactly these, referenced by index/pc from the ops).
 * Only non-empty sections print, to keep a plain function's dump terse. */
void dump_chunk_pools(const Chunk &ch, std::ostringstream &s)
{
    if (!ch.consts.empty()) {
        s << "; -- consts (" << ch.consts.size() << ") --\n";
        for (size_t i = 0; i < ch.consts.size(); i++)
            s << ";   #" << i << "  "
              << ch.consts[i].get_type()->to_string_repr(ch.consts[i]) << "\n";
    }
    if (!ch.member_keys.empty()) {
        s << "; -- member_keys (" << ch.member_keys.size() << ") --\n";
        for (size_t i = 0; i < ch.member_keys.size(); i++) {
            const auto &mk = ch.member_keys[i];
            s << ";   #" << i << "  " << (mk.optional ? "?." : ".")
              << mk.memId.get_type()->to_string(mk.memId) << "\n";
        }
    }
    if (!ch.catch_types.empty()) {
        s << "; -- catch_types (" << ch.catch_types.size() << ") --\n";
        for (size_t i = 0; i < ch.catch_types.size(); i++) {
            s << ";   #" << i << "  ";
            for (size_t j = 0; j < ch.catch_types[i].size(); j++)
                s << (j ? ", " : "") << ch.catch_types[i][j];
            s << "\n";
        }
    }
    if (!ch.literal_objs.empty()) {
        s << "; -- literal_objs (" << ch.literal_objs.size() << ") --\n";
        for (size_t i = 0; i < ch.literal_objs.size(); i++) {
            const auto &lo = ch.literal_objs[i];
            s << ";   #" << i << "  "
              << lo.value.get_type()->to_string_repr(lo.value)
              << (lo.immutable ? "  (immutable)" : "") << "\n";
        }
    }
    if (!ch.closure_defs.empty()) {
        s << "; -- closure_defs (" << ch.closure_defs.size() << ") --\n";
        for (size_t i = 0; i < ch.closure_defs.size(); i++) {
            const FuncDeclStmt *fd = ch.closure_defs[i];
            s << ";   #" << i << "  "
              << (fd->id ? std::string(fd->id->get_str()) : "<lambda>") << "\n";
        }
    }
    if (!ch.struct_defs.empty()) {
        s << "; -- struct_defs (" << ch.struct_defs.size() << ") --\n";
        for (size_t i = 0; i < ch.struct_defs.size(); i++)
            s << ";   #" << i << "  " << ch.struct_defs[i]->name->val
              << "\n";
    }
    if (!ch.locs.empty()) {
        s << "; -- locs (" << ch.locs.size() << ") --\n";
        for (const auto &l : ch.locs)
            s << ";   pc" << l.pc << " -> " << l.start.line << ":"
              << l.start.col << "\n";
    }
    if (!ch.inline_ctxs.empty()) {
        s << "; -- inline_ctxs (" << ch.inline_ctxs.size() << ") --\n";
        for (const auto &ie : ch.inline_ctxs) {
            s << ";   pc" << ie.pc << " -> ";
            for (const InlineCtx *ic = ie.ic; ic; ic = ic->parent)
                s << ic->callee_name << "@" << ic->call_site.line
                  << (ic->parent ? " < " : "");
            s << "\n";
        }
    }
}

}  /* namespace */

std::string disassemble(const Chunk &chunk, const std::string &title,
                        const std::vector<std::string> &cap_names)
{
    std::ostringstream s;
    s << "; ===== " << title << "  (" << chunk.code.size() << " instr, "
      << chunk.n_temps << " temps) =====\n";
    /* A closure's captures ARE an anonymous struct: show its fields, and the
     * capture ops below name them (a `cN` capture slot reads as its field). */
    if (!cap_names.empty()) {
        s << "; captures (anon struct): {";
        for (size_t i = 0; i < cap_names.size(); i++)
            s << (i ? ", " : " ") << cap_names[i];
        s << " }\n";
    }
    s << "; registers: a source var reads by NAME; `rN` is a scratch temp\n";

    /* A capture slot as its field name (the anon capture-struct field), else
     * `cN`. */
    auto CAP = [&](int slot) -> std::string {
        if (slot >= 0 && static_cast<size_t>(slot) < cap_names.size())
            return cap_names[slot];
        return "c" + std::to_string(slot);
    };

    /* Terse operand helpers bound to this chunk (names + immediates), and the
     * `; source` comment column an op carries its AST snippet in. */
    auto D  = [&](int slot)                 { return reg(chunk, slot); };
    auto RI = [&](const Operand &o, bool f) { return reg_or_imm(chunk, o, f); };
    auto cmt = [&](std::ostream &r, const Construct *n) {
        if (n)
            r << "   ; " << node1(n);
    };
    /* arg0's lvalue target for a CallBuiltinLV, by slot kind (a.lit). */
    auto lval_ref = [&](int_type kind, int_type slot) -> std::string {
        if (kind == 0) return "&" + reg(chunk, slot);        /* local */
        if (kind == 1) return "&g" + std::to_string(slot);   /* global */
        return "&c" + std::to_string(slot);                  /* capture */
    };
    /* a container-store op's BASE by slot kind (in.target): local reg / gN / cN.
     * (-1, the unset default, is local too.) */
    auto bref = [&](int kind, int slot) -> std::string {
        if (kind == 1) return "g" + std::to_string(slot);    /* global */
        if (kind == 2) return "c" + std::to_string(slot);    /* capture */
        return reg(chunk, slot);                             /* local (or -1) */
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
        case OpCode::Throw:
            row << "throw        " << D(in.a.slot);
            break;
        case OpCode::PushHandler:
            row << "try.push     catch=L" << in.target;
            break;
        case OpCode::PopHandler:
            row << "try.pop";
            break;
        case OpCode::CatchTest: {
            row << "catch.test   ";
            if (in.a.lit < 0)
                row << "(any)";
            else {
                const auto &names = chunk.catch_types[in.a.lit];
                for (size_t i = 0; i < names.size(); i++)
                    row << (i ? "|" : "") << names[i];
            }
            if (in.target2 >= 0)
                row << " as " << reg(chunk, in.target2);
            row << " -> L" << in.target;
            break;
        }
        case OpCode::Reraise:
            row << "reraise";
            break;
        case OpCode::Rethrow:
            row << "rethrow";
            break;
        case OpCode::SetPend:
            /* Only the shared finally's exits: normal or reraise (a flow op
             * inlines its own finally, so it never sets a pending action). */
            row << "set.pend     "
                << (in.target == 0 ? "normal" : "reraise");
            break;
        case OpCode::EndFinally:
            row << "end.finally";
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
        case OpCode::LoadStructFieldInt:
        case OpCode::LoadStructFieldFloat:
            row << "load.sfield" << (in.op == OpCode::LoadStructFieldInt
                                        ? "i " : "f ")
                << D(in.target) << " = " << D(in.target2) << "["
                << RI(in.a, false) << "].fld" << in.b.lit;
            break;
        case OpCode::DictIterInit:
            row << "dict.iter.i  I" << in.target << " <- " << D(in.target2);
            break;
        case OpCode::DictIterNext:
            row << "dict.iter.n  I" << in.target2 << " k=" << in.a.slot
                << " v=" << in.b.slot << " -> L" << in.target;
            break;
        case OpCode::ForeachDynInit:
            row << "fe.dyn.init  I" << in.target << " <- " << D(in.target2)
                << "  ; array|dict runtime dispatch";
            break;
        case OpCode::ForeachDynNext:
            row << "fe.dyn.next  I" << in.target2 << " e=" << in.a.slot
                << " -> L" << in.target;
            break;
        case OpCode::UnpackElemInt:
        case OpCode::UnpackElemFloat:
            row << (in.op == OpCode::UnpackElemInt ? "unpack.elem.i"
                                                   : "unpack.elem.f")
                << " r" << in.target << ".." << (in.target + in.b.lit - 1)
                << " = " << D(in.target2) << "[" << RI(in.a, false)
                << "] (" << in.b.lit << ")";
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
            row << "store.elem.i " << bref(in.target, in.target2) << "["
                << RI(in.a, false) << "] " << store_op(in.aop) << " "
                << RI(in.b, false);
            break;
        case OpCode::StoreElemFloat:
            row << "store.elem.f " << bref(in.target, in.target2) << "["
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
            row << "dict.store   " << bref(in.target, in.target2) << "["
                << RI(in.a, false) << "] " << o << " " << RI(in.b, false);
            break;
        }
        case OpCode::StoreMemberV: {
            /* aop is the Expr14 op; the member name comes from the pool. */
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "member.store " << bref(in.target, in.target2) << "."
                << chunk.member_keys[in.a.lit].memId.get_type()
                       ->to_string(chunk.member_keys[in.a.lit].memId)
                << " " << o << " " << RI(in.b, false);
            break;
        }
        case OpCode::StoreElemValue: {
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "store.elem.v " << bref(in.target, in.target2) << "["
                << RI(in.a, false) << "] " << o << " " << RI(in.b, false);
            break;
        }
        case OpCode::StoreElem2V: {
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "store.elem2 " << D(in.target2) << "[" << RI(in.a, false)
                << "][" << RI(in.b, false) << "] " << o << " " << D(in.target);
            break;
        }
        case OpCode::MultiUnpackV: {
            row << "multi.unpack ";
            const std::vector<int32_t> &tg = chunk.unpack_targets[in.target];
            for (size_t i = 0; i < tg.size(); i++) {
                if (i)
                    row << ", ";
                if (tg[i] < 0)
                    row << "_";
                else
                    row << "r" << tg[i];
            }
            row << " = " << RI(in.a, false);
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
            /* AST-free: the callee is a global slot (its name lives in gfuncs,
             * not the chunk), so show g<n>. */
            row << "call.v       " << D(in.target) << " = g" << in.target2
                << arglist(chunk, in.a.lit, in.b.lit);
            break;
        case OpCode::CachedCallV:
            row << "call.cached  " << D(in.target) << " = g" << in.target2
                << arglist(chunk, in.a.lit, in.b.lit);
            break;
        case OpCode::CallValueV:
            /* the callee is a func VALUE in a temp (target2), not a slot. */
            row << "call.val     " << D(in.target) << " = " << D(in.target2)
                << arglist(chunk, in.a.lit, in.b.lit);
            break;
        case OpCode::CheckFuncV:
            row << "check.func   " << RI(in.a, false)
                << "  ; throw if not a function";
            break;
        case OpCode::MapFilterV:
            row << (in.target2 ? "filter       " : "map          ")
                << D(in.target) << " = " << RI(in.a, false) << "("
                << RI(in.b, false) << ")";
            break;
        case OpCode::ReturnV:
            row << "return.v     " << RI(in.a, false);
            break;
        case OpCode::MakeArrayV:
            row << "make.arr     " << D(in.target) << " = "
                << arglist(chunk, in.a.lit, in.b.lit)
                << "  ; array literal, hint " << in.target2;
            break;
        case OpCode::MakeDictV:
            row << "make.dict    " << D(in.target) << " = "
                << arglist(chunk, in.a.lit, 2 * in.b.lit)
                << "  ; dict literal (" << in.b.lit << " pairs)";
            break;
        case OpCode::MakeClosureV:
            row << "make.closure " << D(in.target) << " = closure_defs["
                << in.target2 << "]";
            break;
        case OpCode::StructCtorV:
            row << "struct.ctor  " << D(in.target) << " = struct_defs["
                << in.target2 << "]("
                << arglist(chunk, in.a.lit, in.b.lit) << ")";
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
        case OpCode::LoadLiteralObjV: {
            const EvalValue &c = chunk.literal_objs[in.target2].value;
            row << "load.obj     " << D(in.target) << ", "
                << c.get_type()->to_string_repr(c)
                << (chunk.literal_objs[in.target2].immutable ? " (const)" : "");
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
        case OpCode::StoreGlobalV:
            row << "store.global g" << in.target
                << (in.aop == Op::invalid ? " = " : " OP= ")
                << RI(in.a, false);
            break;
        case OpCode::DeclConstV:
            row << "decl.const   " << (in.target2 ? "g" : "r") << in.target
                << " = " << RI(in.a, false) << "  ; const";
            break;
        case OpCode::StoreCaptureV:
            row << "store.cap    " << CAP(in.target)
                << (in.aop == Op::invalid ? " = " : " OP= ")
                << RI(in.a, false);
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
            /* AST-free: the member name comes from the member-key pool. */
            row << "member.v     " << D(in.target) << " = " << D(in.target2)
                << "." << chunk.member_keys[in.a.lit].memId.get_type()
                              ->to_string(chunk.member_keys[in.a.lit].memId);
            break;
        case OpCode::SliceV:
            row << "slice.v      " << D(in.target) << " = " << D(in.target2)
                << "[";
            if (in.a.slot >= 0) row << D(in.a.slot);
            row << ":";
            if (in.b.slot >= 0) row << D(in.b.slot);
            row << "]";
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

    /* The serializable POOLS + side tables this chunk carries (a `.myv` file
     * stores exactly these) - printed after the code, non-empty ones only. */
    dump_chunk_pools(chunk, s);

    return s.str();
}

std::string disassemble_program(const Block *root)
{
    std::ostringstream s;

    /* The program's CUSTOM TYPES first (struct type definitions) - program-
     * level data a `.myv` stores once, that instances/ops reference by name. */
    std::vector<const StructDeclStmt *> structs;
    for (const auto &e : root->elems)
        collect_structs(e.get(), structs);
    if (!structs.empty()) {
        s << "; ===== types (" << structs.size() << ") =====\n";
        for (const StructDeclStmt *sd : structs)
            dump_struct_type(sd->def.get(), s);
        s << "\n";
    }

    s << disassemble(codegen_program(root), "main");

    std::vector<const FuncDeclStmt *> funcs;
    for (const auto &e : root->elems)
        collect_funcs(e.get(), funcs);

    int anon = 0;
    for (const FuncDeclStmt *fn : funcs) {
        if (!fn->body || !fn->body->is_block())
            continue;
        const Block *body = static_cast<const Block *>(fn->body.get());

        /* The capture list IS the closure's anonymous struct: its field names,
         * in declaration order (== the cN capture-slot order). */
        std::vector<std::string> cap_names;
        if (fn->captures)
            for (const auto &cap : fn->captures->elems)
                if (auto *id = dynamic_cast<const Identifier *>(cap.get()))
                    cap_names.push_back(std::string(id->get_str()));

        /* Label: a named func is `func <name>`; an anonymous lambda gets a
         * synthetic id, `closure` iff it captures (else `lambda`). */
        std::string title;
        if (fn->id)
            title = "func " + std::string(fn->id->get_str());
        else
            title = (cap_names.empty() ? "lambda#" : "closure#")
                    + std::to_string(anon++);

        s << "\n" << disassemble(codegen_chunk(body, fn->frame_size), title,
                                 cap_names);
    }
    return s.str();
}

/* ------------------------------------------------------------------------
 * -vd syntax highlighting (256-color, TTY only)
 *
 * A post-pass over the finished plain disassembly: it tokenizes each line by
 * the disassembler's own regular shape - `<pc>  <mnemonic>  <operands> ; cmt`
 * - and colors each piece. No opcode rendering is touched. The token classes
 * are unambiguous by prefix: `#N` immediates, `L<N>` labels, `rN`/`gN`/source
 * names registers, `; ...` comments, `; ===== ... =====` section headers.
 * ------------------------------------------------------------------------ */
namespace {

const char *const RST = "\033[0m";

/* The mnemonic's color by op CATEGORY - so the class reads at a glance. */
const char *mnemonic_color(const std::string &m)
{
    if (m.compare(0, 5, "eval.") == 0)
        return "\033[38;5;203m";                    /* fallback  - red     */
    if (m == "jmp" || m == "halt" || m == "for.step"
        || m.compare(0, 4, "loop") == 0 || m.compare(0, 3, "ret") == 0
        || m.compare(0, 3, "jmp") == 0
        || m.find(".jmp.") != std::string::npos)
        return "\033[38;5;214m";                    /* control   - orange  */
    if (m.compare(0, 4, "call") == 0)
        return "\033[38;5;177m";                    /* call      - purple  */
    if (m == "i.bin" || m == "f.bin" || m == "bin.v" || m == "cmp.v"
        || m == "log.v" || m == "compound.v")
        return "\033[38;5;69m";                     /* arithmetic - blue   */
    return "\033[38;5;78m";                         /* mem/other - green   */
}

std::string hl_line(const std::string &line)
{
    size_t i = 0;
    while (i < line.size() && line[i] == ' ')
        i++;

    /* whole-line comment / section header */
    if (i < line.size() && line[i] == ';') {
        const char *c = line.find("=====") != std::string::npos
                            ? "\033[1;38;5;222m"     /* header  - bold gold */
                            : "\033[38;5;245m";      /* comment - gray      */
        return c + line + RST;
    }
    if (i >= line.size())
        return line;                                 /* blank */

    std::ostringstream o;
    o << line.substr(0, i);                          /* leading indent */

    /* pc (the leading number) */
    size_t j = i;
    while (j < line.size() && isdigit((unsigned char)line[j]))
        j++;
    if (j > i) {
        o << "\033[38;5;240m" << line.substr(i, j - i) << RST;
        i = j;
    }
    while (i < line.size() && line[i] == ' ')
        o << line[i++];

    /* mnemonic (the leading lowercase/./digit run) */
    size_t m = i;
    while (m < line.size()
           && (islower((unsigned char)line[m]) || line[m] == '.'
               || isdigit((unsigned char)line[m])))
        m++;
    if (m > i) {
        const std::string mn = line.substr(i, m - i);
        o << mnemonic_color(mn) << mn << RST;
        i = m;
    }

    /* operands - a token scan */
    while (i < line.size()) {
        const char c = line[i];
        if (c == ';') {                              /* inline comment -> EOL */
            o << "\033[38;5;245m" << line.substr(i) << RST;
            break;
        }
        if (c == ' ') {
            o << c;
            i++;
            continue;
        }
        if (c == '#' || isdigit((unsigned char)c)) {  /* immediate / number */
            size_t k = i + (c == '#' ? 1 : 0);
            while (k < line.size() && isdigit((unsigned char)line[k]))
                k++;
            o << "\033[38;5;150m" << line.substr(i, k - i) << RST;
            i = k;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {  /* reg / name / label */
            size_t k = i;
            while (k < line.size()
                   && (isalnum((unsigned char)line[k]) || line[k] == '_'))
                k++;
            const std::string id = line.substr(i, k - i);
            bool label = id.size() > 1 && id[0] == 'L';
            for (size_t x = 1; label && x < id.size(); x++)
                if (!isdigit((unsigned char)id[x]))
                    label = false;
            o << (label ? "\033[38;5;213m" : "\033[38;5;80m") << id << RST;
            i = k;
            continue;
        }
        o << "\033[38;5;243m" << c << RST;           /* operator / punct */
        i++;
    }
    return o.str();
}

} // namespace

std::string highlight_disasm(const std::string &plain)
{
    std::ostringstream out;
    size_t start = 0;
    for (;;) {
        const size_t nl = plain.find('\n', start);
        out << hl_line(plain.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos)
            break;
        out << '\n';
        start = nl + 1;
    }
    return out.str();
}
