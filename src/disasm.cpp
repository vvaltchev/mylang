/* SPDX-License-Identifier: BSD-2-Clause */

#include "disasm.h"
#include "codegen.h"
#include "eval.h"      /* builtin_slot / builtin_slot_name */
#include "jit.h"       /* jit_type_singletons (-vdj) */      /* codegen_program / codegen_chunk */
#include "coderender.h"   /* render_construct_code - shared AST decompiler */
#include "syntax.h"
#include "structtype.h"   /* StructTypeDef / FieldDef (the custom-type dump) */
#include "errors.h"       /* InlineCtx (the inline_ctxs pool dump) */
#include "lexer.h"        /* OpString */

#include <sstream>
#include <iomanip>
#include <vector>
#include <functional>
#include <unordered_set>

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
/* A float literal that reads AS a float: an integral value keeps a
 * trailing ".0" (so `2.0` doesn't render as the int-looking `2`). */
std::string flit_str(float_type f)
{
    std::ostringstream s;
    s << f;
    const std::string t = s.str();
    if (t.find('.') == std::string::npos && t.find('e') == std::string::npos
            && t.find("inf") == std::string::npos
            && t.find("nan") == std::string::npos)
        return t + ".0";
    return t;
}

std::string reg_or_imm(const Chunk &ch, const Operand &o, bool /*is_float*/)
{
    if (!o.is_lit)
        return reg(ch, o.slot);
    std::ostringstream s;
    switch (o.lit_kind) {
    case Operand::LitKind::f: s << flit_str(o.flit); break;
    case Operand::LitKind::b: s << (o.lit ? "true" : "false"); break;
    default:                  s << o.lit; break;   /* decimal; no marker
                                                    * (slots are rN/names) */
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

std::string opsym(Op op)
{
    return OpString[static_cast<int>(op)];
}

/* The baked operator of a B1/B2 specialized arith op (bytecode.h). */
const char *spec_arith_sym(OpCode op)
{
    switch (op) {
    case OpCode::IntAddRR: case OpCode::IntAddRI:
    case OpCode::FloatAddRR: case OpCode::FloatAddRI: return "+";
    case OpCode::IntSubRR: case OpCode::IntSubRI:
    case OpCode::FloatSubRR: case OpCode::FloatSubRI: return "-";
    case OpCode::IntMulRR: case OpCode::IntMulRI:
    case OpCode::FloatMulRR: case OpCode::FloatMulRI: return "*";
    case OpCode::IntAndRR: case OpCode::IntAndRI: return "&";
    case OpCode::IntOrRR:  case OpCode::IntOrRI:  return "|";
    case OpCode::IntXorRR: case OpCode::IntXorRI: return "^";
    case OpCode::IntShlRR: case OpCode::IntShlRI: return "<<";
    case OpCode::IntShrRR: case OpCode::IntShrRI: return ">>";
    case OpCode::IntModRI: return "%";
    default: return "?";
    }
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
    /* One blank line + a header: these are the SERIALIZABLE pools/side
     * tables the ops reference by index/pc - the audit surface for the
     * `.myv` stored-bytecode file, NOT needed to read the code above
     * (names/values are already inlined at the use sites). */
    s << "\n; ===== serializable pools (what a .myv file stores) =====\n";
    if (!ch.consts.empty()) {
        s << "; -- consts (" << ch.consts.size() << ") --\n";
        for (size_t i = 0; i < ch.consts.size(); i++)
            s << ";   [" << i << "]" << "  "
              << ch.consts[i].get_type()->to_string_repr(ch.consts[i]) << "\n";
    }
    if (!ch.member_keys.empty()) {
        s << "; -- member_keys (" << ch.member_keys.size() << ") --\n";
        for (size_t i = 0; i < ch.member_keys.size(); i++) {
            const auto &mk = ch.member_keys[i];
            s << ";   [" << i << "]" << "  " << (mk.optional ? "?." : ".")
              << mk.memId.get_type()->to_string(mk.memId) << "\n";
        }
    }
    if (!ch.catch_types.empty()) {
        s << "; -- catch_types (" << ch.catch_types.size() << ") --\n";
        for (size_t i = 0; i < ch.catch_types.size(); i++) {
            s << ";   [" << i << "]" << "  ";
            for (size_t j = 0; j < ch.catch_types[i].size(); j++)
                s << (j ? ", " : "") << ch.catch_types[i][j];
            s << "\n";
        }
    }
    if (!ch.literal_objs.empty()) {
        s << "; -- literal_objs (" << ch.literal_objs.size() << ") --\n";
        for (size_t i = 0; i < ch.literal_objs.size(); i++) {
            const auto &lo = ch.literal_objs[i];
            s << ";   [" << i << "]" << "  "
              << lo.value.get_type()->to_string_repr(lo.value)
              << (lo.immutable ? "  (immutable)" : "") << "\n";
        }
    }
    if (!ch.closure_defs.empty()) {
        s << "; -- closure_defs (" << ch.closure_defs.size() << ") --\n";
        for (size_t i = 0; i < ch.closure_defs.size(); i++) {
            const FuncDescriptor *fd = ch.closure_defs[i];
            s << ";   [" << i << "]" << "  "
              << (fd->name ? std::string(fd->name->val) : "<lambda>") << "\n";
        }
    }
    if (!ch.struct_defs.empty()) {
        s << "; -- struct_defs (" << ch.struct_defs.size() << ") --\n";
        for (size_t i = 0; i < ch.struct_defs.size(); i++)
            s << ";   [" << i << "]" << "  " << ch.struct_defs[i]->name->val
              << "\n";
    }
    if (!ch.boxed_ctors.empty()) {
        s << "; -- boxed_ctors (" << ch.boxed_ctors.size() << ") --\n";
        for (size_t i = 0; i < ch.boxed_ctors.size(); i++)
            s << ";   [" << i << "]" << "  " << ch.boxed_ctors[i].def->name->val
              << "  (" << ch.boxed_ctors[i].arg_locs.size() << " args)\n";
    }
    if (!ch.throws.empty()) {
        s << "; -- throws (" << ch.throws.size() << ") --\n";
        for (size_t i = 0; i < ch.throws.size(); i++) {
            const auto &t = ch.throws[i];
            s << ";   [" << i << "]" << "  " << throw_kind_name(t.kind)
              << (t.name ? " " + std::string(t.name->val) : "") << "\n";
        }
    }
    if (!ch.builtin_calls.empty()) {
        /* one entry per builtin call SITE: the name + the per-arg source
         * carets a runtime type/arity error points at (the carets aren't
         * shown here; the name is already inline at the `call.blt` op). */
        s << "; -- builtin_calls: per-call-site name + arg carets ("
          << ch.builtin_calls.size() << ") --\n";
        for (size_t i = 0; i < ch.builtin_calls.size(); i++) {
            const auto &bc = ch.builtin_calls[i];
            s << ";   [" << i << "]" << "  " << (bc.name ? bc.name->val : "?")
              << "  (" << bc.args.size() << " args)\n";
        }
    }
    if (!ch.emplace_sites.empty()) {
        s << "; -- emplace_sites (" << ch.emplace_sites.size() << ") --\n";
        for (size_t i = 0; i < ch.emplace_sites.size(); i++) {
            const auto &es = ch.emplace_sites[i];
            s << ";   [" << i << "]" << "  "
              << (es.bname ? es.bname->val : "append") << " <- "
              << es.def->name->val
              << "  (" << es.field_locs.size() << " fields)\n";
        }
    }
    if (!ch.call_sites.empty()) {
        s << "; -- call_sites (" << ch.call_sites.size() << ") --\n";
        static const char *fm[] = {"none", "slot", "elem", "member", "undef"};
        for (size_t i = 0; i < ch.call_sites.size(); i++) {
            const auto &cs = ch.call_sites[i];
            s << ";   [" << i << "]" << "  " << cs.args.size() << " args, a0="
              << fm[static_cast<int>(cs.a0_form)];
            if (cs.a0_form == Chunk::CallSite::A0::slot
                || cs.a0_form == Chunk::CallSite::A0::elem
                || cs.a0_form == Chunk::CallSite::A0::member)
                s << " k" << static_cast<int>(cs.a0_kind)
                  << "[" << cs.a0_slot << "]";
            s << "\n";
        }
    }
    if (!ch.incdec_sites.empty()) {
        s << "; -- incdec_sites (" << ch.incdec_sites.size() << ") --\n";
        for (size_t i = 0; i < ch.incdec_sites.size(); i++) {
            const auto &is = ch.incdec_sites[i];
            s << ";   [" << i << "]" << "  lval@" << is.lstart.line << ":"
              << is.lstart.col << "  incdec@" << is.istart.line << ":"
              << is.istart.col;
            if (is.memUid)
                s << "  ." << is.memUid->val;
            s << "\n";
        }
    }
    if (!ch.incdec_chains.empty()) {
        s << "; -- incdec_chains (" << ch.incdec_chains.size() << ") --\n";
        for (size_t i = 0; i < ch.incdec_chains.size(); i++) {
            const auto &ic = ch.incdec_chains[i];
            s << ";   [" << i << "]" << "  steps=";
            for (const Chunk::ChainStep &st : ic.steps)
                s << (st.is_member ? ".<m#" : "[r")
                  << st.operand << (st.is_member ? ">" : "]");
            s << (ic.tier2 ? "  tier2" : "  dyn")
              << (ic.is_prefix ? " pre" : " post");
            if (ic.allow_flat)
                s << " flat-ok";
            if (ic.allow_pod)
                s << " pod-ok";
            s << "  incdec@" << ic.id_start.line << ":" << ic.id_start.col
              << "\n";
        }
    }
    if (!ch.locs.empty()) {
        /* a throwing op's SOURCE caret, looked up by pc only on the error
         * path: `pcN -> line:col` = op N's error points at that source
         * line:col. */
        s << "; -- locs: throwing-op pc -> source line:col ("
          << ch.locs.size() << ") --\n";
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

/* ============ Native-AOT fragment disassembler (-vdj) ============
 *
 * Decodes exactly the x86-64 forms jit.cpp emits (a small, fixed set),
 * interleaving each VM op's `; pc N` boundary from NativeCode::Frag::marks.
 * An unrecognized byte prints `.byte` and advances 1; the next op mark
 * resyncs, so a decode slip is bounded to one op. The slot-window layout
 * (stride 48, payload +0, type +24) is stable and mirrored here for the
 * `slot N` / `slot N.type` labels (cosmetic - the JIT itself bakes the
 * runtime-probed offsets). */
namespace {

const char *gp64(int r)
{
    static const char *n[16] = { "rax","rcx","rdx","rbx","rsp","rbp",
        "rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
    return (r >= 0 && r < 16) ? n[r] : "r?";
}

std::string hex2(uint8_t v)
{
    static const char *h = "0123456789abcdef";
    return std::string(1, h[v >> 4]) + std::string(1, h[v & 15]);
}

/* [rdi+disp] -> the slot's NAME (via `nm`, the chunk's slot-namer),
 * else [base+0xNN]. The frame window base is rdi. */
using SlotNamer = std::function<std::string(int)>;
std::string mem_disp(int base_reg, int32_t disp, const SlotNamer &nm)
{
    if (base_reg == 7 /*rdi*/) {
        const int stride = 48, poff = 0, toff = 24;
        if (disp >= 0 && disp % stride == poff)
            return nm(disp / stride);
        if (disp >= 0 && disp % stride == toff)
            return nm(disp / stride) + ".type";
    }
    std::ostringstream o;
    o << "[" << gp64(base_reg) << (disp < 0 ? "-0x" : "+0x") << std::hex
      << (disp < 0 ? -disp : disp) << "]";
    return o.str();
}

/* Decode ONE instruction at code[p]; append its mnemonic to `out` and
 * advance p. Covers jit.cpp's emitted forms. */
void decode_one(const uint8_t *c, uint32_t n, uint32_t &p, std::string &out,
                std::string &cmt, const SlotNamer &nm,
                const void *ti, const void *tf, const void *ta)
{
    const uint32_t start = p;
    std::ostringstream o;
    cmt.clear();
    bool pf_f2 = false, pf_66 = false;
    while (p < n && (c[p] == 0xF2 || c[p] == 0xF3 || c[p] == 0x66)) {
        if (c[p] == 0xF2) pf_f2 = true;
        if (c[p] == 0x66) pf_66 = true;
        p++;
    }
    uint8_t rex = 0;
    if (p < n && (c[p] & 0xF0) == 0x40) rex = c[p++];
    const bool W = rex & 8, R = rex & 4, X = rex & 2, B = rex & 1;
    if (p >= n) { p = start + 1; out = ".byte 0x" + hex2(c[start]); return; }
    const uint8_t op = c[p++];
    auto rd32 = [&]() -> int32_t {
        int32_t v = 0;
        for (int i = 0; i < 4 && p < n; i++) v |= int32_t(c[p++]) << (i*8);
        return v;
    };
    auto rd64 = [&]() -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8 && p < n; i++) v |= uint64_t(c[p++]) << (i*8);
        return v;
    };
    /* modrm: returns reg field (+R) and formats the r/m as a string,
     * consuming disp32/sib as needed. Only the mod=10 (disp32) and
     * mod=11 (register) + one SIB form jit.cpp uses are handled. */
    auto modrm = [&](int &regf, std::string &rm) {
        const uint8_t m = c[p++];
        const int mod = m >> 6, reg = ((m >> 3) & 7) + (R ? 8 : 0),
                  rmf = (m & 7) + (B ? 8 : 0);
        regf = reg;
        if (mod == 3) { rm = gp64(rmf); return; }
        if ((m & 7) == 4) {           /* SIB: [rcx + r9*8] (our only form) */
            const uint8_t sib = c[p++];
            const int idx = ((sib >> 3) & 7) + (X ? 8 : 0);
            const int base = (sib & 7) + (B ? 8 : 0);
            rm = std::string("[") + gp64(base) + "+" + gp64(idx) + "*8]";
            return;
        }
        const int32_t d = (mod == 2) ? rd32()
                        : (mod == 1) ? int8_t(c[p++]) : 0;
        rm = mem_disp((m & 7), d, nm);
    };
    int regf; std::string rm;

    switch (op) {
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        const int rr = (op - 0xB8) + (B ? 8 : 0);
        if (W) {
            const uint64_t imm = rd64();
            const void *pv = reinterpret_cast<const void *>(imm);
            const char *tag = pv == ti ? "<int-tag>" : pv == tf ? "<float-tag>"
                            : pv == ta ? "<array-tag>" : nullptr;
            if (tag) { o << "movabs " << gp64(rr) << ", " << tag;
                       cmt = "the Type-tag constant"; }
            else if (imm >= 1000)                  /* big values (addresses,
                                                    * fn pointers) in HEX */
                o << "movabs " << gp64(rr) << ", 0x" << std::hex << imm
                  << std::dec;
            else   o << "movabs " << gp64(rr) << ", " << int64_t(imm);
        } else {
            const uint32_t imm = rd32();            /* mov eNN, imm32 = the
                                                     * resume VM pc (exit) */
            o << "mov e" << (gp64(rr) + 1) << ", " << imm;
            cmt = "return: resume at vm pc " + std::to_string(imm);
        }
        break; }
    case 0xC3: o << "ret"; break;
    case 0x50: case 0x51: case 0x52: case 0x53:      /* push r64 */
    case 0x54: case 0x55: case 0x56: case 0x57:
        o << "push " << gp64((op - 0x50) + (B ? 8 : 0)); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:      /* pop r64 */
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        o << "pop " << gp64((op - 0x58) + (B ? 8 : 0)); break;
    case 0x8B: modrm(regf, rm); o << "mov " << gp64(regf) << ", " << rm;
        break;
    case 0x89: modrm(regf, rm); o << "mov " << rm << ", " << gp64(regf);
        break;
    case 0x01: modrm(regf, rm); o << "add " << rm << ", " << gp64(regf);
        break;
    case 0x29: modrm(regf, rm); o << "sub " << rm << ", " << gp64(regf);
        break;
    case 0x21: modrm(regf, rm); o << "and " << rm << ", " << gp64(regf);
        break;
    case 0x09: modrm(regf, rm); o << "or "  << rm << ", " << gp64(regf);
        break;
    case 0x31: modrm(regf, rm); o << "xor " << rm << ", " << gp64(regf);
        break;
    case 0x39: modrm(regf, rm); o << "cmp " << rm << ", " << gp64(regf);
        break;
    case 0x99: o << (W ? "cqo" : "cdq"); break;
    case 0xF7: { modrm(regf, rm);
        o << ((regf & 7) == 7 ? "idiv " : "f7/? ") << rm; break; }
    case 0xC1: { modrm(regf, rm); const uint8_t imm = c[p++];
        o << ((regf & 7) == 4 ? "shl " : "sar ") << rm << ", " << int(imm);
        break; }
    case 0xD3: { modrm(regf, rm);
        o << ((regf & 7) == 4 ? "shl " : "sar ") << rm << ", cl"; break; }
    case 0xFF: { modrm(regf, rm);   /* group 5: /0 inc /1 dec /2 call
                                     * /4 jmp /6 push (the reg field is the
                                     * opcode extension, NOT a register) */
        const int sub = regf & 7;
        o << (sub == 0 ? "inc " : sub == 1 ? "dec " : sub == 2 ? "call "
            : sub == 4 ? "jmp " : sub == 6 ? "push " : "ff/? ") << rm;
        break; }
    case 0x80: { modrm(regf, rm); const uint8_t imm = c[p++];
        o << "cmp byte " << rm << ", " << int(imm); break; }
    case 0x90: o << "nop"; break;
    case 0xE8: { const int32_t d = rd32();   /* call rel32 (N6a direct
                                              * libm call); target is
                                              * external, show the signed
                                              * displacement in hex */
        o << "call " << (d < 0 ? "-0x" : "+0x") << std::hex
          << (d < 0 ? -int64_t(d) : int64_t(d)) << std::dec;
        cmt = "direct rel32 call (libm)"; break; }
    case 0xE9: { const int32_t d = rd32();
        o << "jmp +" << std::dec << (int32_t(p) + d); break; }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0xEB: {
        const int8_t d = int8_t(c[p++]);
        static const char *js[16] = {"jo","jno","jb","jae","je","jne",
            "jbe","ja","js","jns","jp","jnp","jl","jge","jle","jg"};
        const char *m = op == 0xEB ? "jmp" : js[op - 0x70];
        o << m << " +" << std::dec << (int32_t(p) + d); break; }
    case 0x0F: {
        const uint8_t o2 = c[p++];
        if (o2 == 0xAF) { modrm(regf, rm);
            o << "imul " << gp64(regf) << ", " << rm; }
        else if (o2 >= 0x80 && o2 <= 0x8F) { const int32_t d = rd32();
            static const char *j[16] = {"jo","jno","jb","jae","je","jne",
                "jbe","ja","js","jns","jp","jnp","jl","jge","jle","jg"};
            o << j[o2 - 0x80] << " +" << std::dec << (int32_t(p) + d); }
        else if (o2 == 0x10) { modrm(regf, rm);
            o << "movsd xmm" << (regf) << ", " << rm; }
        else if (o2 == 0x11) { modrm(regf, rm);
            o << "movsd " << rm << ", xmm" << regf; }
        else if (o2 == 0x51) { modrm(regf, rm);   /* sqrtsd (N6a) */
            o << "sqrtsd xmm" << regf << ", " << rm; }
        else if (o2 == 0x58 || o2 == 0x59 || o2 == 0x5C || o2 == 0x5E) {
            modrm(regf, rm);
            const char *m = o2==0x58?"addsd":o2==0x59?"mulsd":
                            o2==0x5C?"subsd":"divsd";
            o << m << " xmm" << regf << ", " << rm; }
        else if (o2 == 0x2A) { modrm(regf, rm);
            o << "cvtsi2sd xmm" << regf << ", " << rm; }
        else if (o2 == 0x6E) { modrm(regf, rm);
            o << "movq xmm" << regf << ", " << rm; }
        else if (o2 == 0x2E) { modrm(regf, rm);
            o << "ucomisd xmm" << regf << ", " << rm; }
        else { o << ".0f 0x" << hex2(o2); }
        break; }
    default:
        p = start + 1;
        out = ".byte 0x" + hex2(c[start]);
        return;
    }
    (void)pf_f2; (void)pf_66;
    out = o.str();
}

void disasm_native_frag(std::ostream &s, const uint8_t *code,
                        const NativeCode::Frag &frag, const SlotNamer &nm,
                        const std::function<std::string(size_t)> &render_row)
{
    const void *ti = nullptr, *tf = nullptr, *ta = nullptr;
    jit_type_singletons(ti, tf, ta);
    s << "       . ---- native x86-64 (rdi=frame slots, rsi=int-tag,"
      << " r8=float-tag; xmm=SSE) ----\n";
    uint32_t p = 0, mi = 0;
    bool first = true;
    while (p < frag.len) {
        /* each op boundary: a blank line, then the SOURCE VM op these
         * instructions implement (blank line groups the fragment by op). */
        while (mi < frag.marks.size() && frag.marks[mi].off == p) {
            const uint32_t vpc = frag.marks[mi].vm_pc;
            if (!first)
                s << "       .\n";
            first = false;
            s << "       . ; vm pc " << vpc << ": " << render_row(vpc)
              << "\n";
            mi++;
        }
        const uint32_t st = p;
        std::string mn, cmt;
        decode_one(code, frag.len, p, mn, cmt, nm, ti, tf, ta);
        std::ostringstream line;
        line << "       .   +" << std::setw(3) << std::setfill(' ')
             << std::dec << st << ": " << mn;
        if (!cmt.empty())
            line << std::string(mn.size() < 26 ? 26 - mn.size() : 1, ' ')
                 << "; " << cmt;
        s << line.str() << "\n";
    }
    /* close the native block so it doesn't run into the next VM op */
    s << "       . ---- end native ----\n";
}

}  // namespace

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

    /* The frame's slot map: named locals (slot < slot_count) then the
     * scratch-temp range. A `subscript.v r7 = r4[r5]` reads clearly once
     * you can look r4/r5/r7 up here (temps have no source name). */
    {
        int named = 0;
        for (int sl = 0; sl < chunk.slot_count; sl++)
            if (static_cast<size_t>(sl) < chunk.slot_names.size()
                    && !chunk.slot_names[sl].empty())
                named++;
        s << "; frame: " << chunk.slot_count << " locals + "
          << chunk.n_temps << " temps = "
          << (chunk.slot_count + chunk.n_temps) << " slots";
        if (chunk.n_temps > 0)
            s << "  (r" << chunk.slot_count << "..r"
              << (chunk.slot_count + chunk.n_temps - 1) << " are temps)";
        s << "\n";
        if (named) {
            for (int sl = 0; sl < chunk.slot_count; sl++)
                if (static_cast<size_t>(sl) < chunk.slot_names.size()
                        && !chunk.slot_names[sl].empty())
                    s << ";   r" << sl << " -> " << chunk.slot_names[sl]
                      << "\n";
        }
    }
    s << "\n";   /* separate the header block from the code */

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
    /* A builtin-table entry as `name` when resolvable, else `builtin[N]`.
     * Robust: a value-ABI adapter whose fn-pointer doesn't match the
     * registry, or a non-Builtin slot, falls back to the index. */
    auto BLT = [&](int_type idx) -> std::string {
        return std::string(builtin_slot_name(static_cast<int>(idx)));
    };

    /* Render ONE VM op's mnemonic (shared by the main listing and the
     * -vdj native fragment's `; vm pc N: <op>` markers). */
    auto render_row = [&](size_t pc) -> std::string {
        const Instr &in = chunk.code[pc];
        std::ostringstream row;

        switch (in.op) {
        case OpCode::OpCount_:      /* sentinel - never emitted; the case
                                     * keeps this switch's -Wswitch
                                     * exhaustiveness (a new op must add its
                                     * render here) */
            row << "??";
            break;
        case OpCode::Jump:
            row << "jmp          L" << in.target;
            break;
        case OpCode::Throw:
            row << "throw        " << D(in.a_slot());
            break;
        case OpCode::PushHandler:
            row << "try.push     catch=L" << in.target;
            break;
        case OpCode::PopHandler:
            row << "try.pop";
            break;
        case OpCode::CatchTest: {
            row << "catch.test   ";
            if (in.a_lit() < 0)
                row << "(any)";
            else {
                const auto &names = chunk.catch_types[in.a_lit()];
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

        case OpCode::IntBin:
            row << "i.bin        " << D(in.target) << " = "
                << RI(in.a(), false) << " " << opsym(in.aop) << " "
                << RI(in.b(), false);
            break;
        /* B1/B2 specialized arithmetic: same 3-address render, the operator
         * baked in the opcode (the shape is visible from the operands). */
        case OpCode::IntAddRR: case OpCode::IntAddRI:
        case OpCode::IntSubRR: case OpCode::IntSubRI:
        case OpCode::IntMulRR: case OpCode::IntMulRI:
        case OpCode::IntAndRR: case OpCode::IntAndRI:
        case OpCode::IntOrRR:  case OpCode::IntOrRI:
        case OpCode::IntXorRR: case OpCode::IntXorRI:
        case OpCode::IntShlRR: case OpCode::IntShlRI:
        case OpCode::IntShrRR: case OpCode::IntShrRI:
        case OpCode::IntModRI:
            row << "i.bin        " << D(in.target) << " = "
                << RI(in.a(), false) << " " << spec_arith_sym(in.op) << " "
                << RI(in.b(), false);
            break;
        case OpCode::FloatAddRR: case OpCode::FloatAddRI:
        case OpCode::FloatSubRR: case OpCode::FloatSubRI:
        case OpCode::FloatMulRR: case OpCode::FloatMulRI:
            row << "f.bin        " << D(in.target) << " = "
                << RI(in.a(), true) << " " << spec_arith_sym(in.op) << " "
                << RI(in.b(), true);
            break;
        case OpCode::JumpUnlessIntCmp:
            row << "i.jmp.ifnot  " << RI(in.a(), false) << " "
                << opsym(in.aop) << " " << RI(in.b(), false)
                << ", L" << in.target;
            break;
        case OpCode::FloatBin:
            row << "f.bin        " << D(in.target) << " = "
                << RI(in.a(), true) << " " << opsym(in.aop) << " "
                << RI(in.b(), true);
            break;
        case OpCode::JumpUnlessFloatCmp:
            row << "f.jmp.ifnot  " << RI(in.a(), true) << " "
                << opsym(in.aop) << " " << RI(in.b(), true)
                << ", L" << in.target;
            break;
        case OpCode::ForLoopStep:
            row << "for.step     " << D(in.target2) << " "
                << (in.aop == Op::lt || in.aop == Op::le ? "+=" : "-=") << " "
                << RI(in.b(), false) << ", if " << D(in.target2) << " "
                << opsym(in.aop) << " " << RI(in.a(), false)
                << " -> L" << in.target;
            break;
        case OpCode::IntAddModRI:
            /* E4 fusion */
            row << "i.addmod     " << D(in.target) << " = ("
                << RI(in.a(), false) << " + " << RI(in.b(), false)
                << ") % " << in.target2;
            break;
        case OpCode::JumpUnlessElemInt:
            /* E4 fusion */
            row << "jmp.ifnotel  " << D(in.target2) << "["
                << RI(in.a(), false) << "], L" << in.target;
            break;
        case OpCode::IntAddStep:
            /* #9 fusion: a_dual = (add dst, bound) */
            row << "i.addstep    " << D(in.a_dual_lo()) << " += "
                << RI(in.b(), false) << "; " << D(in.target2)
                << (in.aop == Op::lt || in.aop == Op::le ? "++" : "--")
                << ", if " << D(in.target2) << " " << opsym(in.aop) << " "
                << (in.a_is_lit() ? "" : "r")
                << in.a_dual_hi() << " -> L" << in.target;
            break;
        case OpCode::ForStepElemInt:
            /* #9 fusion: b_dual = (array, elem dst) */
            row << "for.step.el  " << D(in.target2)
                << (in.aop == Op::lt || in.aop == Op::le ? "++" : "--")
                << ", if " << D(in.target2) << " " << opsym(in.aop) << " "
                << RI(in.a(), false) << ": " << D(in.b_dual_hi()) << " = "
                << D(in.b_dual_lo()) << "[" << D(in.target2)
                << "] -> L" << in.target;
            break;
        case OpCode::EnterNative:
            /* native-AOT: the fragment entry (jit.cpp; bytes auditable
             * via `objdump -D -b binary -m i386:x86-64` on the dump) */
            row << "enter.nat    frag@+" << in.a_lit();
            break;
        case OpCode::StructFieldAddInt:
            /* #9 fusion: b_dual = (field idx, other slot) */
            row << "sf.add       " << D(in.target) << " = "
                << D(in.b_dual_hi()) << " + " << D(in.target2) << "["
                << RI(in.a(), false) << "].f" << in.b_dual_lo();
            break;
        case OpCode::LoadElemInt:
            row << "load.elem.i  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a(), false) << "]";
            break;
        case OpCode::LoadElemFloat:
            row << "load.elem.f  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a(), false) << "]";
            break;
        case OpCode::LoadElemBool:
            row << "load.elem.b  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a(), false) << "]   ; bool";
            break;
        case OpCode::LoadElemValue:
            row << "load.elem.v  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a(), false) << "]";
            break;
        case OpCode::LoadStructFieldInt:
        case OpCode::LoadStructFieldFloat:
            row << "load.sfield" << (in.op == OpCode::LoadStructFieldInt
                                        ? "i " : "f ")
                << D(in.target) << " = " << D(in.target2) << "["
                << RI(in.a(), false) << "].fld" << in.b_lit();
            break;
        case OpCode::LoadStructElemV:
            row << "load.selem   " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a(), false) << "]   ; struct";
            break;
        case OpCode::DictIterInit:
            row << "dict.iter.i  I" << in.target << " <- " << D(in.target2);
            break;
        case OpCode::DictIterNext:
            row << "dict.iter.n  I" << in.target2 << " k=" << in.a_slot()
                << " v=" << in.b_slot() << " -> L" << in.target;
            break;
        case OpCode::ForeachDynInit: {
            const std::vector<int32_t> &tg = chunk.unpack_targets[in.b_lit()];
            row << "fe.dyn.init  I" << in.target << " <- " << D(in.target2)
                << "  ; array|dict runtime dispatch, "
                << (in.a_lit() & 0xff) << "-var"
                << ((in.a_lit() >> 8) ? ", indexed" : "") << ", [";
            for (size_t i = 0; i < tg.size(); i++) {
                if (i) row << ", ";
                if (tg[i] < 0) row << "_"; else row << "r" << tg[i];
            }
            row << "]";
            break;
        }
        case OpCode::ForeachDynNext:
            row << "fe.dyn.next  I" << in.target2 << " -> L" << in.target;
            break;
        case OpCode::UnpackElemInt:
        case OpCode::UnpackElemFloat:
        case OpCode::UnpackElemValue:
            row << (in.op == OpCode::UnpackElemInt   ? "unpack.elem.i"
                    : in.op == OpCode::UnpackElemFloat ? "unpack.elem.f"
                                                       : "unpack.elem.v")
                << " r" << in.target << ".." << (in.target + in.b_lit() - 1)
                << " = " << D(in.target2) << "[" << RI(in.a(), false)
                << "] (" << in.b_lit() << ")";
            break;
        case OpCode::UnpackElemTargets: {
            row << "unpack.elem.t targets[" << in.target << "]" << " = "
                << D(in.target2) << "[" << RI(in.a(), false) << "] ("
                << in.b_lit() << ") [";
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
            if (in.a_is_lit())
                row << chunk.consts[in.a_lit()].get_type()->to_string(
                           chunk.consts[in.a_lit()]);
            else
                row << RI(in.a(), false);
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
                << D(in.target2) << "[" << RI(in.a(), false) << "]";
            break;
        case OpCode::StoreElemInt:
            row << "store.elem.i " << bref(in.target, in.target2) << "["
                << RI(in.a(), false) << "] " << store_op(in.aop) << " "
                << RI(in.b(), false);
            break;
        case OpCode::StoreElemFloat:
            row << "store.elem.f " << bref(in.target, in.target2) << "["
                << RI(in.a(), false) << "] " << store_op(in.aop) << " "
                << RI(in.b(), true);
            break;
        case OpCode::DictStore: {
            /* aop is the Expr14 op (assign/addeq/...), not the arith form. */
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "dict.store   " << bref(in.target, in.target2) << "["
                << RI(in.a(), false) << "] " << o << " " << RI(in.b(), false);
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
                << chunk.member_keys[in.a_lit()].memId.get_type()
                       ->to_string(chunk.member_keys[in.a_lit()].memId)
                << " " << o << " " << RI(in.b(), false);
            break;
        }
        case OpCode::StoreElemValue: {
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "store.elem.v " << bref(in.target, in.target2) << "["
                << RI(in.a(), false) << "] " << o << " " << RI(in.b(), false);
            break;
        }
        case OpCode::StoreElem2V: {
            const char *o = in.aop == Op::assign ? "=" :
                            in.aop == Op::addeq  ? "+=" :
                            in.aop == Op::subeq  ? "-=" :
                            in.aop == Op::muleq  ? "*=" :
                            in.aop == Op::diveq  ? "/=" : "%=";
            row << "store.elem2 " << D(in.target2) << "[" << RI(in.a(), false)
                << "][" << RI(in.b(), false) << "] " << o << " " << D(in.target);
            break;
        }
        case OpCode::StoreElemChainV:
            row << "store.chain " << D(in.target2) << "[..x"
                << chunk.chain_locs[in.a_dual_lo()].size()
                << " @" << D(in.b_lit()) << "] " << store_op(in.aop) << " "
                << D(in.target);
            break;
        case OpCode::StoreLValueChainV: {
            static const char *bk[] = {"loc", "gbl", "cap"};
            row << "store.lvchain " << bk[in.a_dual_hi() & 3] << "[" << in.target2
                << "]";
            const std::vector<Chunk::ChainStep> &st =
                chunk.chain_steps[in.a_dual_lo()];
            for (const Chunk::ChainStep &s : st)
                row << (s.is_member ? ".<m#" : "[r")
                    << s.operand << (s.is_member ? ">" : "]");
            row << " " << store_op(in.aop) << " " << D(in.target);
            break;
        }
        case OpCode::IncDecCheckedV:
            row << "incdec.chk   " << D(in.target)
                << (in.a_lit() ? " ++" : " --") << "   ; dyn, int/float-checked";
            break;
        case OpCode::IncDecElemCheckedV: {
            static const char *bk[] = {"loc", "gbl", "cap"};
            row << "incdec.elem  " << bk[in.target & 3] << "[" << in.target2
                << "][" << RI(in.a(), false) << "]"
                << (in.aop == Op::plus ? " ++" : " --")
                << "   ; dyn elem, int/float-checked";
            break;
        }
        case OpCode::IncDecMemberCheckedV: {
            static const char *bk[] = {"loc", "gbl", "cap"};
            const Chunk::IncDecSite &is = chunk.incdec_sites[in.b_lit()];
            row << "incdec.membr " << bk[in.target & 3] << "[" << in.target2
                << "]." << (is.memUid ? is.memUid->val : "<member>")
                << (in.aop == Op::plus ? " ++" : " --")
                << "   ; dyn member, int/float-checked";
            break;
        }
        case OpCode::IncDecChainV: {
            static const char *bk[] = {"loc", "gbl", "cap", "val"};
            const Chunk::IncDecChain &ic = chunk.incdec_chains[in.b_lit()];
            row << "incdec.chain " << D(in.target) << " = "
                << bk[in.a_lit() & 3] << "[" << in.target2 << "]";
            for (const Chunk::ChainStep &st : ic.steps)
                row << (st.is_member ? ".<m#" : "[r")
                    << st.operand << (st.is_member ? ">" : "]");
            row << (in.aop == Op::plus ? " ++" : " --")
                << (ic.is_prefix ? "   ; prefix" : "   ; postfix")
                << (ic.tier2 ? ", typed" : ", dyn-checked");
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
            row << " = " << RI(in.a(), false);
            break;
        }
        case OpCode::CallBuiltinV:
            row << "call.blt.v   " << D(in.target) << " = "
                << builtin_call_name(chunk, in.target2)
                << arglist(chunk, in.a_lit(), in.b_lit());
            break;
        case OpCode::AppendV:
            /* D1: same shape as call.blt.lv, the append fast op; a -1 dst =
             * a DISCARDED result (the peephole's dead-dst rule) */
            row << "append.v     "
                << (in.target < 0 ? std::string("_") : D(in.target)) << " = "
                << builtin_call_name(chunk, in.a_dual_lo())
                << "(" << lval_ref(in.a_dual_hi(), in.target2) << ", "
                << D(static_cast<int_type>(in.b_lit())) << ")";
            break;
        case OpCode::MathFnV: {
            /* F1: the typed math-builtin call (selector in target2) */
            static const char *const mf_names[] = {
                "sqrt", "cbrt", "sin", "cos", "tan", "asin", "acos",
                "atan", "exp", "exp2", "log", "log2", "log10", "ceil",
                "floor", "trunc", "float", "abs", "pow",
            };
            const size_t fi = static_cast<size_t>(in.target2);
            row << "math.v       " << D(in.target) << " = "
                << (fi < sizeof(mf_names) / sizeof(mf_names[0])
                        ? mf_names[fi] : "?")
                << "(" << RI(in.a(), false);
            if (static_cast<MathFn>(in.target2) == MathFn::pow_)
                row << ", " << RI(in.b(), false);
            row << ")";
            break;
        }
        case OpCode::CallBuiltinLV: {
            /* AST-free: name + arg count from the builtin_calls pool (a.slot).
             * A valid `b` (is_lit) is a rest-run base (rest-native) - show its
             * values; `b` unset = no value args (pop/intptr). */
            const int bcidx = in.a_dual_lo();
            row << "call.blt.lv  " << D(in.target) << " = "
                << builtin_call_name(chunk, bcidx)
                << "(" << lval_ref(in.a_dual_hi(), in.target2);
            if (in.b_is_lit()) {
                const int nrest = static_cast<int>(
                    chunk.builtin_calls[bcidx].args.size()) - 1;
                for (int i = 0; i < nrest; i++)
                    row << ", " << reg(chunk, in.b_lit() + i);
            }
            row << ")";
            break;
        }
        case OpCode::EmplaceStruct: {
            /* AST-free: the def / name / field count come from the
             * emplace_sites pool (`a` packs kind | idx << 2). */
            const Chunk::EmplaceSite &es = chunk.emplace_sites[in.a_lit() >> 2];
            const int nf = static_cast<int>(es.field_locs.size());
            row << "emplace      " << D(in.target) << " = "
                << (es.bname ? es.bname->val : "append") << "("
                << lval_ref(in.a_lit() & 3, in.target2) << " <- "
                << std::string(es.def->name->val)
                << arglist(chunk, in.b_lit(), nf) << ")";
            break;
        }
        case OpCode::CallBuiltinLVElem: {
            /* AST-free: name/arg count from the pool (a.slot). `b` = the run
             * base: run[0] = the index, run[1..] = the value args (append 1,
             * pop 0). */
            const int bcidx = in.a_dual_lo();
            const int nvals =
                static_cast<int>(chunk.builtin_calls[bcidx].args.size()) - 1;
            row << "call.blt.lve " << D(in.target) << " = "
                << builtin_call_name(chunk, bcidx)
                << "(" << lval_ref(in.a_dual_hi(), in.target2) << "["
                << reg(chunk, in.b_lit()) << "]";
            for (int i = 0; i < nvals; i++)
                row << ", " << reg(chunk, in.b_lit() + 1 + i);
            row << ")";
            break;
        }
        case OpCode::CallBuiltinLVMember: {
            const int bcidx = in.a_dual_lo();
            const Chunk::BuiltinCall &bc = chunk.builtin_calls[bcidx];
            const int nvals = static_cast<int>(bc.args.size()) - 1;
            row << "call.blt.lvm " << D(in.target) << " = "
                << builtin_call_name(chunk, bcidx) << "("
                << lval_ref(in.a_dual_hi(), in.target2) << "."
                << (bc.member ? bc.member->val : "?");
            for (int i = 0; i < nvals; i++)
                row << ", " << reg(chunk, in.b_lit() + i);
            row << ")";
            break;
        }
        case OpCode::CallV:
            /* AST-free: the callee is a global slot (its name lives in gfuncs,
             * not the chunk), so show g<n>. */
            row << "call.v       "
                << (in.target < 0 ? std::string("_") : D(in.target))
                << " = g" << in.target2
                << arglist(chunk, in.a_lit(), in.b_lit());
            break;
        case OpCode::CachedCallV:
            row << "call.cached  " << D(in.target) << " = g" << in.target2
                << arglist(chunk, in.a_lit(), in.b_lit());
            break;
        case OpCode::CallValueV:
            /* the callee is a func VALUE in a temp (target2), not a slot. */
            row << "call.val     "
                << (in.target < 0 ? std::string("_") : D(in.target))
                << " = " << D(in.target2)
                << arglist(chunk, in.a_lit(), in.b_lit());
            break;
        case OpCode::CallValueGenericV:
            /* generic dyn-callee dispatch, AST-free: args pre-evaluated in
             * the run (arg0 lvalue-preserving), carets pooled. */
            row << "call.val.dyn " << D(in.target) << " = " << D(in.target2)
                << arglist(chunk, in.a_lit(),
                           static_cast<int>(in.b_lit() & 0xfff))
                << "   ; runtime dispatch, site [" << (in.b_lit() >> 12) << "]";
            break;
        case OpCode::CheckCallableV:
            row << "check.call   " << RI(in.a(), false)
                << "  ; throw if not callable";
            break;
        case OpCode::CheckFuncV:
            row << "check.func   " << RI(in.a(), false)
                << "  ; throw if not a function";
            break;
        case OpCode::MapFilterV:
            row << (in.target2 ? "filter       " : "map          ")
                << D(in.target) << " = " << RI(in.a(), false) << "("
                << RI(in.b(), false) << ")";
            break;
        case OpCode::ReturnV:
            row << "return.v     " << RI(in.a(), false);
            break;
        case OpCode::MakeArrayV:
            row << "make.arr     " << D(in.target) << " = "
                << arglist(chunk, in.a_lit(), in.b_lit())
                << "  ; array literal, hint " << in.target2;
            break;
        case OpCode::MakeDictV:
            row << "make.dict    " << D(in.target) << " = "
                << arglist(chunk, in.a_lit(), 2 * in.b_lit())
                << "  ; dict literal (" << in.b_lit() << " pairs)";
            break;
        case OpCode::MakeClosureV:
            row << "make.closure " << D(in.target) << " = closure_defs["
                << in.target2 << "]";
            break;
        case OpCode::StructCtorV:
            row << "struct.ctor  " << D(in.target) << " = struct_defs["
                << in.target2 << "]("
                << arglist(chunk, in.a_lit(), in.b_lit()) << ")";
            break;
        case OpCode::StructCtorBoxedV: {
            const Chunk::BoxedCtor &bc = chunk.boxed_ctors[in.target2];
            row << "struct.ctor.b " << D(in.target) << " = "
                << std::string(bc.def->name->val) << "("
                << arglist(chunk, in.a_lit(),
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
                << in.target2 << "][" << in.b_lit() << "]("
                << arglist(chunk, in.a_lit(),
                           in.b_lit() * static_cast<int_type>(nf)) << ")";
            break;
        }
        case OpCode::LoadImmInt:
            row << "load         " << D(in.target) << ", " << in.a_lit();
            break;
        case OpCode::LoadImmFloat:
            row << "load         " << D(in.target) << ", " << flit_str(in.a_flit());
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
                << (in.target2 ? "float(" : "int(") << RI(in.a(), false)
                << ")   ; typed-store widen/check";
            break;
        case OpCode::BinOpV:
            row << "bin.v        " << D(in.target) << " = "
                << RI(in.a(), false) << " " << opsym(in.aop) << " "
                << RI(in.b(), false) << "   ; boxed";
            break;
        case OpCode::CompoundV:
            row << "compound.v   " << D(in.target) << " " << opsym(in.aop)
                << "= " << RI(in.b(), false) << "   ; boxed";
            break;
        case OpCode::CmpV:
            row << "cmp.v        " << D(in.target) << " = "
                << RI(in.a(), false) << " " << opsym(in.aop) << " "
                << RI(in.b(), false) << "   ; boxed";
            break;
        case OpCode::LogV:
            row << "log.v        " << D(in.target) << " = "
                << RI(in.a(), false) << " " << opsym(in.aop) << " "
                << RI(in.b(), false) << "   ; boxed";
            break;
        case OpCode::UnaryV:
            row << "unary.v      " << D(in.target) << " = "
                << opsym(in.aop) << RI(in.a(), false) << "   ; boxed";
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
                << RI(in.a(), false);
            break;
        case OpCode::DeclConstV:
            row << "decl.const   " << (in.target2 ? "g" : "r") << in.target
                << " = " << RI(in.a(), false) << "  ; const";
            break;
        case OpCode::StoreCaptureV:
            row << "store.cap    " << CAP(in.target)
                << (in.aop == Op::invalid ? " = " : " OP= ")
                << RI(in.a(), false);
            break;
        case OpCode::LoadCaptureV:
            row << "load.capture " << D(in.target) << ", cap[" << in.target2
                << "]";
            break;
        case OpCode::LoadBuiltinV:
            row << "load.builtin " << D(in.target) << ", " << BLT(in.target2)
                << "   ; = builtin[" << in.target2 << "]";
            break;
        case OpCode::SubscriptV:
            row << "subscript.v  " << D(in.target) << " = " << D(in.target2)
                << "[" << RI(in.a(), false) << "]";
            break;
        case OpCode::MemberV:
            /* AST-free: the member name comes from the member-key pool. */
            row << "member.v     " << D(in.target) << " = " << D(in.target2)
                << "." << chunk.member_keys[in.a_lit()].memId.get_type()
                              ->to_string(chunk.member_keys[in.a_lit()].memId);
            break;
        case OpCode::LoadMemberInt:
        case OpCode::LoadMemberFloat:
            /* H1: typed standalone struct-member read */
            row << (in.op == OpCode::LoadMemberInt ? "load.mem.i   "
                                                   : "load.mem.f   ")
                << D(in.target) << " = " << D(in.target2) << "."
                << chunk.member_keys[in.a_lit()].memId.get_type()
                       ->to_string(chunk.member_keys[in.a_lit()].memId);
            break;
        case OpCode::SliceV:
            row << "slice.v      " << D(in.target) << " = " << D(in.target2)
                << "[";
            if (in.a_slot() >= 0) row << D(in.a_slot());
            row << ":";
            if (in.b_slot() >= 0) row << D(in.b_slot());
            row << "]";
            break;
        case OpCode::JumpUnlessTrueV:
            row << "jmp.ifnot.v  " << D(in.target2) << ", L" << in.target;
            break;
        case OpCode::JumpIfNotNoneV:
            row << "jmp.notnone  " << RI(in.a(), false) << ", L" << in.target
                << "   ; ?? short-circuit";
            break;
        case OpCode::Halt:
            row << "halt";
            break;
        }
        return row.str();
    };

    for (size_t pc = 0; pc < chunk.code.size(); pc++) {
        const Instr &in = chunk.code[pc];
        s << std::setw(4) << pc << "  " << render_row(pc) << "\n";

        /* -vdj: after an EnterNative line, disassemble its fragment. */
        if (in.op == OpCode::EnterNative && chunk.native.base
                && !chunk.native.frags.empty()) {
            const uint32_t off = static_cast<uint32_t>(in.a_lit());
            for (const NativeCode::Frag &fr : chunk.native.frags)
                if (fr.start == off) {
                    disasm_native_frag(
                        s, static_cast<const uint8_t *>(chunk.native.base)
                               + fr.start, fr,
                        [&chunk](int sl) { return reg(chunk, sl); },
                        render_row);
                    break;
                }
        }
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
            dump_struct_type(sd->def, s);
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

/* An operand identifier that is a REGISTER (a machine reg OR a VM slot
 * `rN`/`gN`/`cN`/`xmmN`) - colored muted, so a source VARIABLE name (any
 * other identifier) stands out. `byte`/`type` are plumbing keywords. */
bool is_reg_token(const std::string &id)
{
    static const std::unordered_set<std::string> x86 = {
        "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
        "eax","ebx","ecx","edx","esi","edi","ebp","esp",
        "al","bl","cl","dl" };
    if (x86.count(id))
        return true;
    const auto tail_digits = [&](size_t from) {
        if (id.size() <= from)
            return false;
        for (size_t x = from; x < id.size(); x++)
            if (!isdigit((unsigned char)id[x]))
                return false;
        return true;
    };
    if ((id[0] == 'r' || id[0] == 'g' || id[0] == 'c') && tail_digits(1))
        return true;                             /* rN/gN/cN + r8..r15 */
    if (id.compare(0, 3, "xmm") == 0 && tail_digits(3))
        return true;
    return false;
}

bool is_kw_token(const std::string &id)
{
    return id == "byte" || id == "type" || id == "ptr" || id == "word"
        || id == "qword" || id == "dword";
}

/* Color the operand run (from index `i` to EOL): immediates, `L<n>`
 * labels, registers (gray), source VARIABLES (cyan), keywords, comments,
 * punctuation. Shared by the VM ops and the native disasm. */
void scan_operands(std::ostringstream &o, const std::string &line, size_t i)
{
    while (i < line.size()) {
        const char c = line[i];
        if (c == ';') {                              /* inline comment -> EOL */
            o << "\033[38;5;245m" << line.substr(i) << RST;
            return;
        }
        if (c == ' ') { o << c; i++; continue; }
        if (isdigit((unsigned char)c)
            || (c == '-' && i + 1 < line.size()
                && isdigit((unsigned char)line[i + 1]))) {
            size_t k = i + 1;                        /* immediate / number */
            while (k < line.size()
                   && (isdigit((unsigned char)line[k]) || line[k] == 'x'
                       || (line[k] >= 'a' && line[k] <= 'f')
                       || line[k] == '.'))
                k++;
            o << "\033[38;5;150m" << line.substr(i, k - i) << RST;
            i = k;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t k = i;
            while (k < line.size()
                   && (isalnum((unsigned char)line[k]) || line[k] == '_'))
                k++;
            const std::string id = line.substr(i, k - i);
            bool label = id.size() > 1 && id[0] == 'L';
            for (size_t x = 1; label && x < id.size(); x++)
                if (!isdigit((unsigned char)id[x]))
                    label = false;
            const bool field = i > 0 && line[i - 1] == '.';   /* `.type` */
            const char *col =
                label                       ? "\033[38;5;213m"   /* pink   */
              : field || is_kw_token(id)    ? "\033[38;5;244m"   /* gray   */
              : is_reg_token(id)            ? "\033[38;5;244m"   /* gray   */
                                            : "\033[38;5;81m";   /* VAR cyan */
            o << col << id << RST;
            i = k;
            continue;
        }
        o << "\033[38;5;243m" << c << RST;           /* operator / punct */
        i++;
    }
}

std::string hl_line(const std::string &line)
{
    size_t i = 0;
    while (i < line.size() && line[i] == ' ')
        i++;

    /* A native-fragment line (-vdj): `. ---- ...`, `. ; vm pc N: <op>`,
     * `.   +off: mnemonic operands ; cmt`, or a bare `.`. */
    if (i < line.size() && line[i] == '.') {
        std::ostringstream o;
        o << line.substr(0, i) << "\033[38;5;238m.\033[0m";   /* the marker */
        size_t k = i + 1;
        while (k < line.size() && line[k] == ' ') o << line[k++];
        if (k >= line.size())
            return o.str();                          /* bare `.` */
        if (line[k] == '-' || line[k] == ';') {
            /* a `---- ... ----` rule or a `; vm pc N: <op>` marker: dim
             * the rule/marker prefix, then colorize the trailing VM op. */
            const size_t colon = line.find(": ", k);
            if (line[k] == ';' && colon != std::string::npos) {
                /* This marker is a COMMENT: the VM op was REPLACED by the
                 * native code below it, so it must NOT look like a live
                 * instruction. A dark-gray gradient - the `; vm pc N:`
                 * label darker than the (still legible) rendered op. */
                o << "\033[38;5;240m" << line.substr(k, colon + 2 - k)
                  << "\033[38;5;245m" << line.substr(colon + 2) << RST;
            } else {
                o << "\033[38;5;238m" << line.substr(k) << RST;
            }
            return o.str();
        }
        /* an instruction line: `+off:` (dim), mnemonic (category), operands */
        if (line[k] == '+') {
            size_t e = line.find(':', k);
            if (e != std::string::npos) e++;
            else e = k;
            o << "\033[38;5;240m" << line.substr(k, e - k) << RST;
            k = e;
            while (k < line.size() && line[k] == ' ') o << line[k++];
            size_t m = k;
            while (m < line.size()
                   && (islower((unsigned char)line[m]) || isdigit(
                           (unsigned char)line[m])))
                m++;
            if (m > k) {
                const std::string mn = line.substr(k, m - k);
                o << mnemonic_color(mn) << mn << RST;
                k = m;
            }
            scan_operands(o, line, k);
            return o.str();
        }
        scan_operands(o, line, k);
        return o.str();
    }

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

    scan_operands(o, line, i);
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
