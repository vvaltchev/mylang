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

/* A ThrowRuntimeV site's kind, for the op render + the throws pool dump. */
static const char *throw_kind_name(Chunk::ThrowKind k)
{
    switch (k) {
    case Chunk::ThrowKind::undefined_var:  return "undefined_var";
    case Chunk::ThrowKind::not_lvalue:     return "not_lvalue";
    case Chunk::ThrowKind::rebind_builtin: return "rebind_builtin";
    case Chunk::ThrowKind::rebind_const:   return "rebind_const";
    case Chunk::ThrowKind::bad_args:       return "bad_args";
    }
    return "?";
}

/* A CallBuiltinV's callee name from the AST-free builtin_calls pool. */
std::string builtin_call_name(const Chunk &ch, int idx)
{
    const UniqueId *n = ch.builtin_calls[static_cast<size_t>(idx)].name;
    return n ? std::string(n->val) : "?";
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

/* collect_funcs moved to codegen.cpp (shared with the VM's AOT precompile);
 * declared in codegen.h. */

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
    if (!ch.boxed_ctors.empty()) {
        s << "; -- boxed_ctors (" << ch.boxed_ctors.size() << ") --\n";
        for (size_t i = 0; i < ch.boxed_ctors.size(); i++)
            s << ";   #" << i << "  " << ch.boxed_ctors[i].def->name->val
              << "  (" << ch.boxed_ctors[i].arg_locs.size() << " args)\n";
    }
    if (!ch.throws.empty()) {
        s << "; -- throws (" << ch.throws.size() << ") --\n";
        for (size_t i = 0; i < ch.throws.size(); i++) {
            const auto &t = ch.throws[i];
            s << ";   #" << i << "  " << throw_kind_name(t.kind)
              << (t.name ? " " + std::string(t.name->val) : "") << "\n";
        }
    }
    if (!ch.builtin_calls.empty()) {
        s << "; -- builtin_calls (" << ch.builtin_calls.size() << ") --\n";
        for (size_t i = 0; i < ch.builtin_calls.size(); i++) {
            const auto &bc = ch.builtin_calls[i];
            s << ";   #" << i << "  " << (bc.name ? bc.name->val : "?")
              << "  (" << bc.args.size() << " args)\n";
        }
    }
    if (!ch.emplace_sites.empty()) {
        s << "; -- emplace_sites (" << ch.emplace_sites.size() << ") --\n";
        for (size_t i = 0; i < ch.emplace_sites.size(); i++) {
            const auto &es = ch.emplace_sites[i];
            s << ";   #" << i << "  "
              << (es.bname ? es.bname->val : "append") << " <- "
              << es.def->name->val
              << "  (" << es.field_locs.size() << " fields)\n";
        }
    }
    if (!ch.incdec_sites.empty()) {
        s << "; -- incdec_sites (" << ch.incdec_sites.size() << ") --\n";
        for (size_t i = 0; i < ch.incdec_sites.size(); i++) {
            const auto &is = ch.incdec_sites[i];
            s << ";   #" << i << "  lval@" << is.lstart.line << ":"
              << is.lstart.col << "  incdec@" << is.istart.line << ":"
              << is.istart.col;
            if (is.memUid)
                s << "  ." << is.memUid->val;
            s << "\n";
        }
    }
    if (!ch.node_table.empty()) {
        /* The ONE non-serializable side table: the AST nodes ops still need at
         * runtime (fallbacks / builtin calls / a store's caret), pc-keyed. A
         * fully-native chunk has NONE - so this section appearing IS the ".myv
         * can't drop the AST yet" signal. Rendered by the shared decompiler. */
        s << "; -- node_table (" << ch.node_table.size()
          << ", NOT serializable) --\n";
        for (const auto &ne : ch.node_table)
            s << ";   pc" << ne.pc << "  " << node1(ne.node) << "\n";
    }
    if (!ch.locs.empty()) {
        s << "; -- locs (" << ch.locs.size() << ") --\n";
        for (const auto &l : ch.locs)
            s << ";   pc" << l.pc << " -> " << l.start.line << ":"
              << l.start.col << "\n";
    }
    if (!ch.inline_ctxs.empty()) {
        s << "; -- inline_ctxs (" << ch.inline_ctxs.size() << ", "
          << ch.inline_frames.size() << " frames) --\n";
        for (const auto &ie : ch.inline_ctxs) {
            s << ";   pc" << ie.pc << " -> ";
            for (int32_t i = ie.frame; i >= 0;
                 i = ch.inline_frames[i].parent) {
                const auto &f = ch.inline_frames[i];
                s << f.callee_name << "@" << f.call_site.line
                  << (f.parent >= 0 ? " < " : "");
            }
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
    /* int_type, not int: a slot sometimes rides an Operand's int_type `lit`
     * (e.g. StoreElemChainV's run base, CallValueGenericV's callee temp) - a
     * widening call is warning-free on all three compilers (MSVC C4244). */
    auto D  = [&](int_type slot)            { return reg(chunk, slot); };
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
            row << "eval.stmt    " << node1(chunk.node_at_pc(pc));
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
            row << "jmp.ifnot    (" << node1(chunk.node_at_pc(pc)) << "), L" << in.target;
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
        case OpCode::LoadElemBool:
            row << "load.elem.b  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a, false) << "]   ; bool";
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
        case OpCode::LoadStructElemV:
            row << "load.selem   " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a, false) << "]   ; struct";
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
                << "  ; array|dict runtime dispatch, " << in.a.lit << "-var";
            break;
        case OpCode::ForeachDynNext:
            row << "fe.dyn.next  I" << in.target2 << " e=" << in.a.slot;
            if (in.b.slot >= 0)
                row << " v=" << in.b.slot;
            row << " -> L" << in.target;
            break;
        case OpCode::UnpackElemInt:
        case OpCode::UnpackElemFloat:
        case OpCode::UnpackElemValue:
            row << (in.op == OpCode::UnpackElemInt   ? "unpack.elem.i"
                    : in.op == OpCode::UnpackElemFloat ? "unpack.elem.f"
                                                       : "unpack.elem.v")
                << " r" << in.target << ".." << (in.target + in.b.lit - 1)
                << " = " << D(in.target2) << "[" << RI(in.a, false)
                << "] (" << in.b.lit << ")";
            break;
        case OpCode::UnpackElemTargets: {
            row << "unpack.elem.t targets#" << in.target << " = "
                << D(in.target2) << "[" << RI(in.a, false) << "] ("
                << in.b.lit << ") [";
            const std::vector<int32_t> &tg = chunk.unpack_targets[in.target];
            for (size_t k = 0; k < tg.size(); k++)
                row << (k ? " " : "") << (tg[k] < 0 ? "_" : ("r" +
                    std::to_string(tg[k])));
            row << "]";
            break;
        }
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
        case OpCode::StrLen:
            row << "str.len      " << D(in.target) << " = len("
                << D(in.target2) << ")   ; str chars";
            break;
        case OpCode::LoadStrChar:
            row << "load.strchar " << D(in.target) << " = "
                << D(in.target2) << "[" << RI(in.a, false) << "]";
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
        case OpCode::StoreElemChainV:
            row << "store.chain " << D(in.target2) << "[..x"
                << chunk.chain_locs[in.a.slot].size()
                << " @" << D(in.b.lit) << "] " << store_op(in.aop) << " "
                << D(in.target);
            break;
        case OpCode::StoreLValueChainV: {
            static const char *bk[] = {"loc", "gbl", "cap"};
            row << "store.lvchain " << bk[in.a.lit & 3] << "[" << in.target2
                << "]";
            const std::vector<Chunk::ChainStep> &st =
                chunk.chain_steps[in.a.slot];
            for (const Chunk::ChainStep &s : st)
                row << (s.is_member ? ".<m#" : "[r")
                    << s.operand << (s.is_member ? ">" : "]");
            row << " " << store_op(in.aop) << " " << D(in.target);
            break;
        }
        case OpCode::IncDecCheckedV:
            row << "incdec.chk   " << D(in.target)
                << (in.a.lit ? " ++" : " --") << "   ; dyn, int/float-checked";
            break;
        case OpCode::IncDecElemCheckedV: {
            static const char *bk[] = {"loc", "gbl", "cap"};
            row << "incdec.elem  " << bk[in.target & 3] << "[" << in.target2
                << "][" << RI(in.a, false) << "]"
                << (in.aop == Op::plus ? " ++" : " --")
                << "   ; dyn elem, int/float-checked";
            break;
        }
        case OpCode::IncDecMemberCheckedV: {
            static const char *bk[] = {"loc", "gbl", "cap"};
            const Chunk::IncDecSite &is = chunk.incdec_sites[in.b.lit];
            row << "incdec.membr " << bk[in.target & 3] << "[" << in.target2
                << "]." << (is.memUid ? is.memUid->val : "<member>")
                << (in.aop == Op::plus ? " ++" : " --")
                << "   ; dyn member, int/float-checked";
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
            row << "eval.slot    " << D(in.target) << " = " << node1(chunk.node_at_pc(pc));
            break;
        case OpCode::CallBuiltinV:
            row << "call.blt.v   " << D(in.target) << " = "
                << builtin_call_name(chunk, in.target2)
                << arglist(chunk, in.a.lit, in.b.lit);
            break;
        case OpCode::CallBuiltinLV: {
            /* AST-free: name + arg count from the builtin_calls pool (a.slot).
             * A valid `b` (is_lit) is a rest-run base (rest-native) - show its
             * values; `b` unset = no value args (pop/intptr). */
            const int bcidx = in.a.slot;
            row << "call.blt.lv  " << D(in.target) << " = "
                << builtin_call_name(chunk, bcidx)
                << "(" << lval_ref(in.a.lit, in.target2);
            if (in.b.is_lit) {
                const int nrest = static_cast<int>(
                    chunk.builtin_calls[bcidx].args.size()) - 1;
                for (int i = 0; i < nrest; i++)
                    row << ", " << reg(chunk, in.b.lit + i);
            }
            row << ")";
            break;
        }
        case OpCode::EmplaceStruct: {
            /* AST-free: the def / name / field count come from the
             * emplace_sites pool (`a` packs kind | idx << 2). */
            const Chunk::EmplaceSite &es = chunk.emplace_sites[in.a.lit >> 2];
            const int nf = static_cast<int>(es.field_locs.size());
            row << "emplace      " << D(in.target) << " = "
                << (es.bname ? es.bname->val : "append") << "("
                << lval_ref(in.a.lit & 3, in.target2) << " <- "
                << std::string(es.def->name->val)
                << arglist(chunk, in.b.lit, nf) << ")";
            break;
        }
        case OpCode::CallBuiltinLVElem: {
            /* AST-free: name/arg count from the pool (a.slot). `b` = the run
             * base: run[0] = the index, run[1..] = the value args (append 1,
             * pop 0). */
            const int bcidx = in.a.slot;
            const int nvals =
                static_cast<int>(chunk.builtin_calls[bcidx].args.size()) - 1;
            row << "call.blt.lve " << D(in.target) << " = "
                << builtin_call_name(chunk, bcidx)
                << "(" << lval_ref(in.a.lit, in.target2) << "["
                << reg(chunk, in.b.lit) << "]";
            for (int i = 0; i < nvals; i++)
                row << ", " << reg(chunk, in.b.lit + 1 + i);
            row << ")";
            break;
        }
        case OpCode::CallBuiltinLVMember: {
            const int bcidx = in.a.slot;
            const Chunk::BuiltinCall &bc = chunk.builtin_calls[bcidx];
            const int nvals = static_cast<int>(bc.args.size()) - 1;
            row << "call.blt.lvm " << D(in.target) << " = "
                << builtin_call_name(chunk, bcidx) << "("
                << lval_ref(in.a.lit, in.target2) << "."
                << (bc.member ? bc.member->val : "?");
            for (int i = 0; i < nvals; i++)
                row << ", " << reg(chunk, in.b.lit + i);
            row << ")";
            break;
        }
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
        case OpCode::CallValueGenericV:
            /* generic dyn-callee dispatch; args bound from the node. */
            row << "call.val.dyn " << D(in.target) << " = " << D(in.a.lit)
                << "(...)   ; dyn dispatch";
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
        case OpCode::StructCtorBoxedV: {
            const Chunk::BoxedCtor &bc = chunk.boxed_ctors[in.target2];
            row << "struct.ctor.b " << D(in.target) << " = "
                << std::string(bc.def->name->val) << "("
                << arglist(chunk, in.a.lit,
                           static_cast<int_type>(bc.arg_locs.size())) << ")";
            break;
        }
        case OpCode::ThrowRuntimeV: {
            const Chunk::ThrowSite &t = chunk.throws[in.target];
            row << "throw.rt      " << throw_kind_name(t.kind);
            if (t.name)
                row << " '" << std::string(t.name->val) << "'";
            break;
        }
        case OpCode::MakeStructArrayV: {
            const size_t nf =
                chunk.struct_defs[in.target2]->fields.size();
            row << "make.structarr " << D(in.target) << " = struct_defs["
                << in.target2 << "][" << in.b.lit << "]("
                << arglist(chunk, in.a.lit,
                           in.b.lit * static_cast<int_type>(nf)) << ")";
            break;
        }
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
        case OpCode::CoerceNumV:
            row << "coerce.num   " << D(in.target) << " = "
                << (in.target2 ? "float(" : "int(") << RI(in.a, false)
                << ")   ; typed-store widen/check";
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
        case OpCode::UnaryV:
            row << "unary.v      " << D(in.target) << " = "
                << opsym(in.aop) << RI(in.a, false) << "   ; boxed";
            break;
        case OpCode::LoadGlobalV:
            /* AST-free: the global name lives in gfuncs, not the chunk, so a
             * disassembly shows the global slot (g<n>). */
            row << "load.global  " << D(in.target) << ", g" << in.target2;
            break;
        case OpCode::DefinedGlobalV:
            row << "defined.g    " << D(in.target) << " = defined(g"
                << in.target2 << ")";
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
                << node_name(chunk.node_at_pc(pc));
            break;
        case OpCode::LoadBuiltinV:
            row << "load.builtin " << D(in.target) << ", "
                << node_name(chunk.node_at_pc(pc));
            break;
        case OpCode::SubscriptV:
            row << "subscript.v  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a, false) << "]";
            cmt(row, chunk.node_at_pc(pc));
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

    /* Drive off the COMPILED CHUNK SET, not the raw AST walk: codegen_func_body
     * compiles a function ONLY if it has bytecode (skips a base template - a
     * monomorphization source that is never called/compiled - a non-scope-free
     * closure, and an all-fallback body). So a base template shows NO chunk
     * because it HAS none (it does not exist as a chunk), not because we filter
     * it - a faithful bytecode image. (When every function is native, this set
     * equals all non-template functions.) */
    int anon = 0;
    for (const FuncDeclStmt *fn : funcs) {
        Chunk ck;
        if (!codegen_func_body(fn, ck))
            continue;

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

        s << "\n" << disassemble(ck, title, cap_names);
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
