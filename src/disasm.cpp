/* SPDX-License-Identifier: BSD-2-Clause */

#include "disasm.h"
#include "codegen.h"
#include "vm.h"
#include "eval.h"      /* builtin_slot / builtin_slot_name */
#include "env.h"      /* env_get / file_open - MSVC deprecates
                       * getenv and fopen (C4996) */
#include "jit.h"       /* jit_type_singletons (-vdj); JitCtx (native calls) */
#include "funcdesc.h"  /* FuncDescriptor::vm_chunk (faithful native-call dump) */
#include "coderender.h"   /* render_construct_code - shared AST decompiler */
#include "syntax.h"
#include "structtype.h"   /* StructTypeDef / FieldDef (the custom-type dump) */
#include "errors.h"       /* InlineCtx (the inline_ctxs pool dump) */
#include "lexer.h"        /* OpString */

#include <sstream>
#include <iomanip>
#include <vector>
#include <functional>
#include <unordered_map>
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

/* The bare enum NAME of an opcode (for the M1 container-plan blocker list -
 * the full mnemonic lives in render_row's per-op switch; here the enum spelling
 * is the clearest "what op is blocking nativization"). */
const char *opcode_name(OpCode op)
{
#define ML_OPCODE_NAME(N) case OpCode::N: return #N;
    switch (op) {
    ML_FOR_EACH_OPCODE(ML_OPCODE_NAME)
    default: return "?";
    }
#undef ML_OPCODE_NAME
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

/* The Expr14-op spelling (`=` / `+=` / `<<=` / ...) for the stores whose
 * `aop` carries the COMPOUND op, not the base arith form (DictStore,
 * StoreMemberV, StoreElemValue, StoreElem2V). OpString already spells a
 * compound op with its `=`, so this is a direct lookup - a new compound
 * operator renders here with no edit. */
static std::string expr14_op(Op aop)
{
    return aop == Op::assign ? std::string("=") : opsym(aop);
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
              << mk.memId.get_type()->to_string(mk.memId);
            if (mk.bake_def && mk.bake_slot >= 0)
                s << "  (baked " << mk.bake_def->name->val
                  << " slot " << mk.bake_slot << ")";
            s << "\n";
        }
    }
    if (!ch.ctor_plans.empty()) {
        /* the planned POD ctor's per-field {offset, act} (StructCtorV's
         * b_dual_hi indexes this) - act 0 int, 1 float, 2 bool */
        s << "; -- ctor_plans (" << ch.ctor_plans.size() << ") --\n";
        for (size_t i = 0; i < ch.ctor_plans.size(); i++) {
            s << ";   [" << i << "] ";
            for (const auto &pf : ch.ctor_plans[i].f)
                s << " {+" << pf.off << " "
                  << (pf.act == 0 ? "int" : pf.act == 1 ? "float" : "bool")
                  << " <- slot" << pf.src << "}";
            s << "\n";
        }
    }
    if (!ch.boxed_ops.empty()) {
        /* model-flip nativize-ops: the JIT-bakeable operand data for
         * BinOpV/CmpV/CompoundV (the op stores this index in its target2).
         * DERIVED from the code (build_boxed_ops) - a .myv load rebuilds it. */
        s << "; -- boxed_ops (" << ch.boxed_ops.size()
          << ", derived) --\n";
        for (size_t i = 0; i < ch.boxed_ops.size(); i++) {
            const auto &bo = ch.boxed_ops[i];
            s << ";   [" << i << "]  slot" << bo.target << " "
              << opsym(bo.aop) << "\n";
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
    if (!ch.handler_sites.empty()) {
        /* #78: the per-try-region catch table - what the raise path matches
         * against (the CatchTest/Reraise chain in the code above mirrors it
         * until step D removes those ops). */
        s << "; -- handler_sites: try region -> clauses ("
          << ch.handler_sites.size() << ") --\n";
        for (size_t i = 0; i < ch.handler_sites.size(); i++) {
            const auto &hs = ch.handler_sites[i];
            s << ";   [" << i << "]  ";
            if (hs.clauses.empty() && hs.fin_pc < 0) {
                s << "(none)\n";
                continue;
            }
            for (size_t j = 0; j < hs.clauses.size(); j++) {
                const auto &cl = hs.clauses[j];
                s << (j ? " | " : "");
                if (cl.types_idx < 0)
                    s << "(any)";
                else {
                    const auto &nm = ch.catch_types[cl.types_idx];
                    for (size_t k = 0; k < nm.size(); k++)
                        s << (k ? "|" : "") << nm[k];
                }
                if (cl.bind_slot >= 0)
                    s << " as " << reg(ch, cl.bind_slot);
                s << " -> L" << cl.body_pc;
            }
            if (hs.fin_pc >= 0)
                s << (hs.clauses.empty() ? "" : "  ")
                  << "finally L" << hs.fin_pc;
            if (hs.has_rethrow)
                s << "  [rethrow]";
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
    if (!ch.base_locs.empty()) {
        /* #127: a container store's SECOND caret - the BASE identifier's own
         * span, used when that base is an unbound global (`locs` holds the
         * whole `g[0]` lvalue, for the OOB/type errors). */
        s << "; -- base_locs: store pc -> base line:col ("
          << ch.base_locs.size() << ") --\n";
        for (const auto &l : ch.base_locs)
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

/*
 * A boolean MYLANG_* env knob: SET, non-empty and not "0". The same
 * three-part test lowmem.h applies to MYLANG_NO_LOWMEM - stated once
 * here because a presence-only check makes `=0` mean ON, which is the
 * opposite of what anyone spelling it that way intends.
 */
bool env_flag_on(const char *name)
{
    /* env_get, not getenv: MSVC deprecates getenv and WERROR is on.
     * jit.cpp may call getenv freely because every one of its calls is
     * inside `#if ML_JIT_SUPPORTED`, which MSVC never compiles - this
     * file is compiled on every platform. */
    const std::optional<std::string> v = env_get(name);
    return v && !v->empty() && *v != "0";
}

/* MYLANG_VDJ_ADDRS=1 - print baked ADDRESSES numerically instead of as
 * `<addr>`. Off by default so the dump is REPRODUCIBLE (see tag_name);
 * on when you are chasing one specific baked pointer and the digits are
 * the thing you need. */
bool vdj_show_addrs()
{
    /* ⛔ A VALUE CHECK, NOT A PRESENCE CHECK - `=0` MUST MEAN OFF.
     * With a bare has_value() this knob turned the masking OFF for
     * `MYLANG_VDJ_ADDRS=0`, so a reader who spelled the disable that way
     * saw raw pointers and concluded the dump is NOT reproducible - the
     * exact wrong conclusion the masking exists to prevent (it happened,
     * 2026-08-29, while comparing a .myv image's -vdj against its
     * source's). The idiom is lowmem.h's: set, non-empty, not "0". */
    static const bool on = env_flag_on("MYLANG_VDJ_ADDRS");
    return on;
}

const char *gp64(int r)
{
    static const char *n[16] = { "rax","rcx","rdx","rbx","rsp","rbp",
        "rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
    return (r >= 0 && r < 16) ? n[r] : "r?";
}

/* The 8-bit register names. WITHOUT a REX prefix, encodings 4-7 are the
 * legacy HIGH bytes ah/ch/dh/bh; WITH one they are the uniform low bytes
 * spl/bpl/sil/dil. The JIT's byte store (`mov [rcx+r9], dil`) is the
 * REX form, and naming it `bh` would be actively misleading. */
const char *gp8(int r, bool rex)
{
    static const char *lo[16] = { "al","cl","dl","bl","spl","bpl","sil","dil",
        "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b" };
    static const char *legacy[8] = { "al","cl","dl","bl","ah","ch","dh","bh" };
    if (!rex && r >= 0 && r < 8) return legacy[r];
    return (r >= 0 && r < 16) ? lo[r] : "r?b";
}

std::string hex2(uint8_t v)
{
    static const char *h = "0123456789abcdef";
    return std::string(1, h[v >> 4]) + std::string(1, h[v & 15]);
}

/*
 * ⛔ A BAKED ADDRESS MUST BE PRINTED SYMBOLICALLY, NOT NUMERICALLY.
 *
 * The JIT bakes absolute addresses (the Type singletons, helper
 * entry points, pool buffers). Their numeric values move with every
 * process under ASLR, so printing the number makes `-vdj` output
 * NON-DETERMINISTIC - the same binary disassembling the same program
 * twice produced different text. That is not cosmetic: `-vdj` is the
 * evidence a JIT change is a pure restructuring, and a dump that
 * differs from itself cannot be that evidence. It cost exactly that:
 * scripts/vdjcmp.sh reported 77 of 108 programs as differing from
 * THEMSELVES, and worked around it with regex masking that then went
 * stale the moment tags became imm32.
 *
 * So symbolise here, at the source, and let every consumer benefit.
 * `tag_name` covers the three Type singletons in both the movabs
 * (imm64) and imm32 spellings - the low-address arena means the SAME
 * pointer appears as a full 8-byte immediate in one instruction and a
 * sign-extended 4-byte one in the next.
 */
const char *tag_name(uint64_t imm, const void *ti, const void *tf,
                     const void *ta)
{
    const void *pv = reinterpret_cast<const void *>(imm);
    if (ti && pv == ti) return "<int-tag>";
    if (tf && pv == tf) return "<float-tag>";
    if (ta && pv == ta) return "<array-tag>";
    return nullptr;
}

/*
 * ⛔ EVERY IMMEDIATE GOES THROUGH HERE. THERE ARE TEN PRINTERS AND
 * FIXING THREE OF THEM IS NOT FIXING THE BUG (2026-08-18).
 *
 * A baked pointer can appear as ANY immediate the emitter happens to
 * use: `movabs` (imm64), `mov r/m, imm32`, `cmp rax, imm32`, and - the
 * one that actually got through - the group-1 `cmp r/m, imm32` that
 * guards a STRUCT INSTANCE against its `StructTypeDef *`. The first
 * round of this fix patched the three printers I had seen misbehave;
 * CI then failed on a struct program, because the fourth printer was
 * never considered. That is the audit-table trap in its purest form:
 * the defect is in the CLASS, so the fix belongs at the CLASS's one
 * choke point.
 *
 * The rule, in order:
 *   - a known Type singleton prints as its name;
 *   - a power of two AT OR ABOVE 2^32 prints numerically. Real
 *     emitted constants are bit patterns (0x8000000000000000 for the
 *     INT_MIN division check), deterministic - masking them would
 *     cost readability for no reproducibility gain. BELOW 2^32 the
 *     exemption is a REPRODUCIBILITY HOLE, found the day it bit: the
 *     MAP_32BIT arena sometimes lands at exactly 0x40000000, so a
 *     baked singleton pointer printed as digits in the runs where
 *     mmap returned a 1GiB-aligned base and as `<addr>` everywhere
 *     else - the dump differed from ITSELF (vdjcmp's flake detector
 *     caught it on 69_exc_crossframe, 2026-08-22);
 *   - anything else >= 0x1000 is assumed to be an ADDRESS and prints as
 *     `<addr>`, because its digits vary per process and would make the
 *     dump differ from itself;
 *   - everything else prints numerically.
 * MYLANG_VDJ_ADDRS=1 disables the masking entirely.
 */
std::string imm_str(int64_t v, const void *ti, const void *tf,
                    const void *ta)
{
    if (const char *t = tag_name(static_cast<uint64_t>(v), ti, tf, ta))
        return t;
    const uint64_t u = static_cast<uint64_t>(v < 0 ? -v : v);
    const bool pow2 = u && (u & (u - 1)) == 0 && u >= (1ull << 32);
    if (!vdj_show_addrs() && u >= 0x1000 && !pow2)
        return "<addr>";
    std::ostringstream o;
    if (u >= 0x1000) o << "0x" << std::hex << u << std::dec;
    else             o << v;
    return o.str();
}

/* [rbx+disp] -> the slot's NAME (via `nm`, the chunk's slot-namer),
 * else [base+0xNN]. The frame window base is rbx (callee-saved; it was
 * rdi before the JIT moved the pins off the caller-saved registers). */
using SlotNamer = std::function<std::string(int)>;
std::string mem_disp(int base_reg, int32_t disp, const SlotNamer &nm)
{
    if (base_reg == 3 /*rbx*/) {
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
    /*
     * modrm: returns the reg field (+REX.R) and formats the r/m operand,
     * consuming the SIB byte and the displacement.
     *
     * ⛔ THE SIB ARM USED TO BE WRONG IN THREE WAYS, AND THE FIRST ONE
     * DESYNCHRONISED THE WHOLE STREAM (fixed 2026-08-17):
     *
     *   1. it returned WITHOUT consuming the displacement, although a
     *      SIB byte does not replace disp8/disp32 - mod still selects
     *      one. `mov rdi, [rsp+8]` (48 8B 7C 24 08) therefore decoded
     *      as `mov rdi, [rsp+rsp*8]` and left `08` behind as a stray
     *      `.byte`, and every later instruction in that fragment was
     *      decoded at the wrong offset until the next `; vm pc` mark
     *      resynced. 330 of the corpus's undecoded bytes were this one
     *      bug, and the garbage it printed FOR the instruction was
     *      worse than the missing byte after it;
     *   2. the SCALE field was ignored and hardcoded `*8`;
     *   3. index == 4 means NO INDEX (the canonical `[rsp+disp]` form),
     *      but it printed `rsp*8`. Note REX.X still applies, so r12 as
     *      an index is index==4 WITH X set - the check must be on the
     *      3-bit field before the extension, which is why `idx3` exists.
     *
     * mod==0 with base==5 (RIP-relative / no-base disp32) is likewise
     * a real encoding and is handled; jit.cpp does not emit it today,
     * but a decoder that silently mis-consumes it is exactly how this
     * class of bug survives.
     */
    auto modrm = [&](int &regf, std::string &rm) {
        const uint8_t m = c[p++];
        const int mod = m >> 6, reg = ((m >> 3) & 7) + (R ? 8 : 0),
                  rmf = (m & 7) + (B ? 8 : 0);
        regf = reg;
        if (mod == 3) { rm = gp64(rmf); return; }
        if ((m & 7) == 4) {                       /* SIB byte follows */
            const uint8_t sib = c[p++];
            const int scale = 1 << (sib >> 6);
            const int idx3 = (sib >> 3) & 7;      /* pre-REX.X */
            const int idx = idx3 + (X ? 8 : 0);
            const int base3 = sib & 7;
            const int base = base3 + (B ? 8 : 0);
            const bool no_base = (base3 == 5 && mod == 0);
            const int32_t d = no_base ? rd32()
                            : (mod == 2) ? rd32()
                            : (mod == 1) ? int8_t(c[p++]) : 0;
            std::ostringstream so;
            so << "[";
            if (!no_base) so << gp64(base);
            if (idx3 != 4) {                      /* 4 == no index */
                if (!no_base) so << "+";
                so << gp64(idx) << "*" << scale;
            }
            if (no_base && idx3 == 4) {
                /*
                 * ⛔ THE NO-BASE disp32 IS AN ABSOLUTE ADDRESS, AND IT
                 * MUST BE MASKED LIKE EVERY OTHER BAKED POINTER.
                 *
                 * This is the low arena's operand (#97 step 1): a
                 * process-lifetime global reached in ONE instruction,
                 * with the pointer sitting in the displacement. Every
                 * other baked address in this dump prints as
                 * `<addr>` / `<int-tag>` / `<helper>` precisely so two
                 * runs and two separately-linked binaries produce
                 * IDENTICAL text - that is what `scripts/vdjcmp.sh`
                 * IS - and this form was added later without learning
                 * the rule.
                 *
                 * ⛔ IT BROKE THE ORACLE AND NOTHING SAID SO. From the
                 * day the arena landed, `vdjcmp.sh` failed its own
                 * SELF-TEST ("the same binary gave two different dumps")
                 * on every invocation, so every "verified byte-identical
                 * emitted code" claim since was unverifiable. The
                 * self-test did its job; nobody ran it. Same shape as
                 * the 2026-08-17 mask-rot, one arena later.
                 */
                so << imm_str(static_cast<int64_t>(
                                  static_cast<uint32_t>(d)),
                              ti, tf, ta);
            } else if (d) {
                so << (d < 0 ? "-0x" : "+0x") << std::hex
                   << (d < 0 ? -int64_t(d) : int64_t(d)) << std::dec;
            }
            so << "]";
            rm = so.str();
            return;
        }
        if (mod == 0 && (m & 7) == 5) {           /* RIP-relative disp32 */
            const int32_t d = rd32();
            std::ostringstream so;
            so << "[rip" << (d < 0 ? "-0x" : "+0x") << std::hex
               << (d < 0 ? -int64_t(d) : int64_t(d)) << std::dec << "]";
            rm = so.str();
            return;
        }
        const int32_t d = (mod == 2) ? rd32()
                        : (mod == 1) ? int8_t(c[p++]) : 0;
        rm = mem_disp(rmf, d, nm);   /* REX.B-extended base (r9 chains) */
    };
    int regf; std::string rm;

    switch (op) {
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        const int rr = (op - 0xB8) + (B ? 8 : 0);
        if (W) {
            const uint64_t imm = rd64();
            if (tag_name(imm, ti, tf, ta))
                cmt = "the Type-tag constant";
            o << "movabs " << gp64(rr) << ", "
              << imm_str(static_cast<int64_t>(imm), ti, tf, ta);
        } else {
            /* mov eNN, imm32 (zero-extending). Until #101 the only
             * emitter of this form was the exit's resume-pc load, and
             * the comment asserted that; movabs now auto-shortens
             * every imm32-range literal to it, so the assertion would
             * lie on ordinary staged constants - the tag comment (the
             * REX.W arm's rule) is the one claim that stays true. */
            const uint32_t imm = rd32();
            if (tag_name(static_cast<uint64_t>(imm), ti, tf, ta))
                cmt = "the Type-tag constant";
            o << "mov e" << (gp64(rr) + 1) << ", "
              << imm_str(static_cast<int64_t>(imm), ti, tf, ta);
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
    case 0x8D: modrm(regf, rm); o << "lea " << gp64(regf) << ", " << rm;
        break;                                /* lea (a helper's &slot arg) */
    case 0x85: modrm(regf, rm); o << "test " << rm << ", " << gp64(regf);
        break;                                /* test (a helper's status) */
    case 0x01: modrm(regf, rm); o << "add " << rm << ", " << gp64(regf);
        break;
    case 0x29: modrm(regf, rm); o << "sub " << rm << ", " << gp64(regf);
        break;
    /* The r64 <- r/m64 DIRECTION of the group-1 arithmetic (opcode
     * bit 1 set). The `rm <- reg` forms above were here from the
     * start; these were not, so `add rdx, [rcx+0xd8]` and
     * `sub rcx, [rax+0x28]` - the M5b record-push address arithmetic -
     * decoded as nothing at all. */
    case 0x03: modrm(regf, rm); o << "add " << gp64(regf) << ", " << rm;
        break;
    case 0x2B: modrm(regf, rm); o << "sub " << gp64(regf) << ", " << rm;
        break;
    case 0x0B: modrm(regf, rm); o << "or "  << gp64(regf) << ", " << rm;
        break;
    case 0x23: modrm(regf, rm); o << "and " << gp64(regf) << ", " << rm;
        break;
    case 0x33: modrm(regf, rm); o << "xor " << gp64(regf) << ", " << rm;
        break;
    /* movsxd r64, r/m32 - how a 32-bit frame_size / count field is
     * widened before it is compared or added. */
    case 0x63: modrm(regf, rm); o << "movsxd " << gp64(regf) << ", " << rm;
        break;
    /* mov r/m8, r8 - the BYTE element store (`mov [rcx+r9], dil`, a
     * flat array<bool> write). With REX present the source is the
     * uniform low-byte set (dil/sil/spl/bpl), which is why this prints
     * the low-byte name rather than gp64. */
    case 0x88: { modrm(regf, rm);
        o << "mov byte " << rm << ", " << gp8(regf, rex != 0); break; }
    case 0x8A: { modrm(regf, rm);
        o << "mov " << gp8(regf, rex != 0) << ", byte " << rm; break; }
    case 0x21: modrm(regf, rm); o << "and " << rm << ", " << gp64(regf);
        break;
    case 0x09: modrm(regf, rm); o << "or "  << rm << ", " << gp64(regf);
        break;
    case 0x31: modrm(regf, rm); o << "xor " << rm << ", " << gp64(regf);
        break;
    case 0x39: modrm(regf, rm); o << "cmp " << rm << ", " << gp64(regf);
        break;
    /* ⛔ cmp rax, imm32 - the ACCUMULATOR short form (REX.W 3D id), with
     * no modrm byte. This is what `cmp_reg_tag` emits for rax, so #96
     * step 3 made it common, and it was undecoded: 168 corpus sites,
     * each one also desynchronising the four immediate bytes after it.
     * A tag-valued immediate is symbolised like the movabs form. */
    case 0x3D: { const int32_t imm = rd32();
        if (tag_name(uint64_t(uint32_t(imm)), ti, tf, ta))
            cmt = "the Type-tag constant";
        o << "cmp rax, " << imm_str(imm, ti, tf, ta);
        break; }
    case 0x99: o << (W ? "cqo" : "cdq"); break;
    /*
     * ⛔ THE F7 GROUP - /3 neg AND /5 imul WERE MISSING, AND THEIR
     * ABSENCE WAS SILENT (2026-08-19). Only /7 (idiv) was decoded;
     * everything else printed `f7/? rdx`, which is neither a mnemonic
     * nor a `.byte` - so the UNRELIABLE banner, which counts `.byte`
     * lines, stayed quiet while the dump showed a placeholder. 284
     * corpus sites: the div-magic sequence's `neg rdx` and `imul rdx`.
     * Found by cross-checking against objdump (scripts/disasmcheck.py),
     * because no self-check can see this - the decoder is the subject.
     */
    case 0xF7: { modrm(regf, rm); const uint8_t sub = regf & 7;
        if (sub == 0) {                        /* test r/m64, imm32 */
            const int32_t imm = rd32();
            o << "test " << rm << ", " << imm_str(imm, ti, tf, ta);
        } else {
            static const char *const f7[8] = {
                "", "", "not ", "neg ", "mul ", "imul ", "div ", "idiv "
            };
            if (!*f7[sub])
                goto undecoded;
            o << f7[sub] << rm;
        }
        break; }
    case 0xC1: { modrm(regf, rm); const uint8_t imm = c[p++];
        o << ((regf & 7) == 4 ? "shl " : (regf & 7) == 5 ? "shr " : "sar ")
          << rm << ", " << int(imm);
        break; }
    case 0x3B: { modrm(regf, rm);   /* cmp r64, r/m64 (the PushHandler
                                     * capacity check) */
        o << "cmp " << gp64(regf) << ", " << rm; break; }
    case 0x6B: { modrm(regf, rm);   /* imul r64, r/m64, imm8 - the SAME
                                     * record-stride multiply when the
                                     * stride fits a byte (0x30), which
                                     * is the case the assembler
                                     * actually picks */
        const int8_t imm = int8_t(c[p++]);
        o << "imul " << gp64(regf) << ", " << rm << ", " << int(imm);
        break; }
    case 0x69: { modrm(regf, rm);   /* imul r64, r/m64, imm32 (the
                                     * SetPend record-stride multiply) */
        const int32_t imm = rd32();
        o << "imul " << gp64(regf) << ", " << rm << ", "
          << imm_str(imm, ti, tf, ta); break; }
    case 0xC6: { modrm(regf, rm);   /* /0: mov BYTE [rm], imm8 (the
                                     * defined[gslot]=1 store) */
        o << "mov byte " << rm << ", " << int(c[p++]); break; }
    case 0xC7: { modrm(regf, rm);   /* /0: mov r/m, imm32 (emit_raise's
                                     * kind store `mov dword [rax], kind`,
                                     * and since #96 step 3 the TYPE-TAG
                                     * store, whose imm32 is a low-arena
                                     * pointer - symbolise it, or the dump
                                     * carries an ASLR-varying number) */
        uint32_t imm = 0;
        for (int i = 0; i < 4; i++) imm |= uint32_t(c[p++]) << (8 * i);
        if (tag_name(uint64_t(imm), ti, tf, ta))
            cmt = "the Type-tag constant";
        o << "mov " << rm << ", "
          << imm_str(static_cast<int64_t>(static_cast<int32_t>(imm)),
                     ti, tf, ta);
        break; }
    case 0xD1: { modrm(regf, rm);   /* group-2 shift by 1 */
        o << ((regf & 7) == 4 ? "shl " : (regf & 7) == 5 ? "shr " : "sar ")
          << rm << ", 1"; break; }
    case 0xD3: { modrm(regf, rm);
        o << ((regf & 7) == 4 ? "shl " : (regf & 7) == 5 ? "shr " : "sar ")
          << rm << ", cl"; break; }
    case 0xFF: { modrm(regf, rm);   /* group 5: /0 inc /1 dec /2 call
                                     * /4 jmp /6 push (the reg field is the
                                     * opcode extension, NOT a register) */
        const int sub = regf & 7;
        o << (sub == 0 ? "inc " : sub == 1 ? "dec " : sub == 2 ? "call "
            : sub == 4 ? "jmp " : sub == 6 ? "push " : "") << rm;
        if (sub != 0 && sub != 1 && sub != 2 && sub != 4 && sub != 6)
            goto undecoded;
        break; }
    case 0x80: { modrm(regf, rm); const uint8_t imm = c[p++];
        o << "cmp byte " << rm << ", " << int(imm); break; }
    /* group 1 (add/or/adc/sbb/and/sub/xor/cmp by the reg field) with an
     * imm32 (0x81) or a sign-ext imm8 (0x83): the ref-check `cmp ecx, t_str`
     * and the call prologue's `sub/add rsp, 8` alignment pad. */
    case 0x81: case 0x83: { modrm(regf, rm);
        static const char *g1[8] = {"add","or","adc","sbb",
                                    "and","sub","xor","cmp"};
        const int32_t imm = op == 0x81 ? rd32() : int8_t(c[p++]);
        if (tag_name(uint64_t(uint32_t(imm)), ti, tf, ta))
            cmt = "the Type-tag constant";
        o << g1[regf & 7] << " " << rm << ", "
          << imm_str(imm, ti, tf, ta); break; }
    case 0x90: o << "nop"; break;
    case 0xE8: { const int32_t d = rd32();
        /* call rel32 to a C++ helper / libm. The DISPLACEMENT is the
         * distance from this code page to the callee, so BOTH ends move
         * under ASLR and the number is noise - and worse, its WIDTH
         * varies (5 or 6 hex digits for the same libm target), which is
         * what made a width-based mask in vdjcmp.sh race with a
         * call-based one and report a file as differing from itself.
         * Print the shape, not the digits. */
        if (vdj_show_addrs())
            o << "call " << (d < 0 ? "-0x" : "+0x") << std::hex
              << (d < 0 ? -int64_t(d) : int64_t(d)) << std::dec;
        else
            o << "call <helper>";
        cmt = "rel32 call (C++ helper / libm)"; break; }
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
        else if (o2 == 0x28) {         /* movaps (reg-reg fmov) */
            const bool reg_form = (c[p] & 0xC0) == 0xC0;
            const int rm_xmm = (c[p] & 7) + (B ? 8 : 0);
            modrm(regf, rm);
            o << "movaps xmm" << regf << ", ";
            if (reg_form) o << "xmm" << rm_xmm; else o << rm; }
        else if (o2 == 0x57) {         /* xorps (#101: the cvtsi2sd
                                        * merge-dependency break; always
                                        * reg-reg, dst == src) */
            const bool reg_form = (c[p] & 0xC0) == 0xC0;
            const int rm_xmm = (c[p] & 7) + (B ? 8 : 0);
            modrm(regf, rm);
            o << "xorps xmm" << regf << ", ";
            if (reg_form) o << "xmm" << rm_xmm; else o << rm; }
        else if (o2 == 0x10 || o2 == 0x11 || o2 == 0x51) {
            /* reg-reg form: the rm REGISTER is an XMM, not a GP (the
             * generic modrm would print `rsi` for xmm6 - found live on
             * 89_regs_float_08's xmm-pinned fj; the 0x58 family below
             * had the fix, these three arms did not) */
            const bool reg_form = (c[p] & 0xC0) == 0xC0;
            const int rm_xmm = (c[p] & 7) + (B ? 8 : 0);
            modrm(regf, rm);
            const char *m = o2 == 0x51 ? "sqrtsd" : "movsd";
            if (o2 == 0x11) {           /* store direction: rm first */
                o << m << " ";
                if (reg_form) o << "xmm" << rm_xmm; else o << rm;
                o << ", xmm" << regf;
            } else {
                o << m << " xmm" << regf << ", ";
                if (reg_form) o << "xmm" << rm_xmm; else o << rm;
            } }
        else if (o2 == 0x58 || o2 == 0x59 || o2 == 0x5C || o2 == 0x5E) {
            /* reg-reg form: the rm REGISTER is an XMM, not a GP (the generic
             * modrm would print `rcx` for xmm1) */
            const bool reg_form = (c[p] & 0xC0) == 0xC0;
            const int rm_xmm = (c[p] & 7) + (B ? 8 : 0);
            modrm(regf, rm);
            const char *m = o2==0x58?"addsd":o2==0x59?"mulsd":
                            o2==0x5C?"subsd":"divsd";
            o << m << " xmm" << regf << ", ";
            if (reg_form) o << "xmm" << rm_xmm; else o << rm; }
        else if (o2 == 0x2A) { modrm(regf, rm);
            o << "cvtsi2sd xmm" << regf << ", " << rm; }
        else if (o2 == 0x6E) { modrm(regf, rm);
            o << "movq xmm" << regf << ", " << rm; }
        else if (o2 == 0x7E) { modrm(regf, rm);   /* 66 REX.W 0F 7E:
                                                   * movq r/m64, xmm */
            o << "movq " << rm << ", xmm" << regf; }
        else if (o2 == 0x2E) {
            const bool reg_form = (c[p] & 0xC0) == 0xC0;
            const int rm_xmm = (c[p] & 7) + (B ? 8 : 0);
            modrm(regf, rm);
            o << "ucomisd xmm" << regf << ", ";
            if (reg_form) o << "xmm" << rm_xmm; else o << rm; }
        else if (o2 >= 0x90 && o2 <= 0x9F) {   /* setcc r/m8 (CmpIntV) */
            modrm(regf, rm);
            static const char *sc[16] = {"seto","setno","setb","setae",
                "sete","setne","setbe","seta","sets","setns","setp","setnp",
                "setl","setge","setle","setg"};
            o << sc[o2 - 0x90] << " " << rm; }
        else if (o2 == 0xB6) { modrm(regf, rm);   /* movzx r32, r/m8 */
            o << "movzx " << gp64(regf) << ", " << rm; }
        else { goto undecoded; }
        break; }
    default:
        goto undecoded;
    }
    (void)pf_f2; (void)pf_66;
    out = o.str();
    return;
    /*
     * ⛔ ONE EXIT FOR "I DO NOT KNOW", AND IT MUST BE `.byte`.
     *
     * There used to be three ways to not know an instruction, and only
     * one of them said so. `default:` emitted `.byte`, which
     * disasm_native_frag COUNTS and reports in the `DUMP IS UNRELIABLE`
     * banner - but an unhandled sub-opcode printed `f7/? rdx` or
     * `ff/? rax`, and an unhandled two-byte opcode printed `.0f 0x38`.
     * Those look like output. They are not counted, they do not trip
     * the banner, and they claim a LENGTH the decoder has not earned -
     * so the next instruction is read at the wrong offset.
     *
     * Every path now lands here: p rewinds to start + 1 and the byte is
     * reported, so an unknown form is loud, has no length claim, and
     * shows up in the banner exactly like every other one.
     */
undecoded:
    p = start + 1;
    out = ".byte 0x" + hex2(c[start]);
}

/*
 * MYLANG_VDJ_HEX=1 - print each instruction's RAW BYTES beside it.
 *
 * ⛔ THIS EXISTS SO AN INDEPENDENT DISASSEMBLER CAN CHECK OURS. The
 * `DUMP IS UNRELIABLE` banner only catches bytes we FAILED to decode;
 * it says nothing about a byte sequence we decode CONFIDENTLY AND
 * WRONGLY, which is the failure that actually cost weeks (the SIB arm
 * that never consumed its displacement printed `mov rdi,[rsp+rsp*8]`
 * and desynchronised the whole fragment - well-formed, plausible, and
 * wrong). No self-check can find that: the decoder is the thing under
 * test. Only a SECOND decoder can.
 *
 * With the bytes in the dump, `scripts/disasmcheck.py` feeds them to
 * objdump and compares both the instruction BOUNDARIES and the
 * MNEMONICS. objdump is a development-time cross-check invoked by a
 * script, exactly like python3 in the other scripts - it is not a
 * build or test dependency of the interpreter.
 */
static bool vdj_hex_on()
{
    /* a VALUE check like its ADDRS sibling: `=0` means off */
    static const bool on = env_flag_on("MYLANG_VDJ_HEX");
    return on;
}

void disasm_native_frag(std::ostream &s, const uint8_t *code,
                        const NativeCode::Frag &frag, const SlotNamer &nm,
                        const std::function<std::string(size_t)> &render_row)
{
    const void *ti = nullptr, *tf = nullptr, *ta = nullptr;
    jit_type_singletons(ti, tf, ta);
    s << "       . ---- native x86-64 (rbx=frame slots, rsi=int-tag,"
      << " r8=float-tag; xmm=SSE) ----\n";
    uint32_t p = 0, mi = 0;
    unsigned drifted = 0, undecoded = 0;
    bool first = true;
    while (p < frag.len) {
        /*
         * ⛔ A SKIPPED MARK IS PROOF THE DECODER DRIFTED, and it is the
         * only self-check this disassembler can have.
         *
         * Every `marks[i].off` is an offset the JIT recorded at a real
         * instruction boundary. So if the decode ever steps PAST one -
         * `off < p` - some instruction's length was wrong, and every
         * mnemonic since then has been read at the wrong offset. That
         * is far worse than an undecoded byte: it prints confident,
         * well-formed, WRONG instructions.
         *
         * Both counts are reported at the end of the fragment rather
         * than asserted, because `-vdj` is a debugging aid that must
         * still produce its best effort on a fragment it cannot fully
         * decode - but it must never do so SILENTLY. (Before 2026-08-17
         * it did: 5543 undecoded bytes across the corpus, with nothing
         * in the output saying the dump was untrustworthy.)
         */
        while (mi < frag.marks.size() && frag.marks[mi].off < p) {
            drifted++;
            mi++;
        }
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
        if (mn.compare(0, 6, ".byte ") == 0)
            undecoded++;
        std::ostringstream line;
        line << "       .   +" << std::setw(3) << std::setfill(' ')
             << std::dec << st << ": ";
        if (vdj_hex_on()) {
            std::ostringstream hx;
            for (uint32_t q = st; q < p; q++)
                hx << hex2(code[q]);
            line << "{" << hx.str() << "} ";
        }
        line << mn;
        if (!cmt.empty())
            line << std::string(mn.size() < 26 ? 26 - mn.size() : 1, ' ')
                 << "; " << cmt;
        s << line.str() << "\n";
    }
    /* close the native block so it doesn't run into the next VM op */
    if (undecoded || drifted) {
        s << "       . ---- ⛔ DUMP IS UNRELIABLE: " << undecoded
          << " undecoded byte(s), " << drifted << " skipped op mark(s)"
          << " - decode_one is missing an opcode this fragment uses;"
          << " see disasm.cpp ----\n";
    }
    s << "       . ---- end native ----\n";
}

}  // namespace

/*
 * THE JIT PROFILE MAP - see the contract in disasm.h. One header line
 * per fragment plus one line per decoded instruction, at the RUNTIME
 * address, appended to the file named by MYLANG_JIT_MAP.
 *
 * It reuses `decode_one`, the SAME decoder `-vdj` uses and that
 * `scripts/disasmcheck.py` cross-checks against objdump - so a length
 * this writes is a length objdump agrees with, which is what makes the
 * address join sound. An undecodable byte would desynchronise the rest
 * of the fragment, so the writer reports it in the header rather than
 * emitting addresses it cannot stand behind.
 */
void jit_write_map(const Chunk &chunk, const std::string &name)
{
    const std::optional<std::string> path = env_get("MYLANG_JIT_MAP");
    if (!path || path->empty() || !chunk.native.base)
        return;
    FILE *f = file_open(path->c_str(), "a");
    if (!f)
        return;
    const uint8_t *base = static_cast<const uint8_t *>(chunk.native.base);
    const SlotNamer nm = [](int i) {
        return "s" + std::to_string(i);
    };
    for (size_t r = 0; r < chunk.native.frags.size(); r++) {
        const NativeCode::Frag &fr = chunk.native.frags[r];
        const uint8_t *code = base + fr.start;
        uint32_t p = 0, undecoded = 0;
        std::string body;
        while (p < fr.len) {
            const uint32_t st = p;
            std::string mn, cmt;
            /* a REAL namer: decode_one CALLS it for every slot operand,
             * and an empty std::function throws bad_function_call (which
             * a `noexcept`-free path turns into terminate - watched). */
            decode_one(code, fr.len, p, mn, cmt, nm, nullptr,
                       nullptr, nullptr);
            if (mn.compare(0, 6, ".byte ") == 0)
                undecoded++;
            char buf[64];
            snprintf(buf, sizeof(buf), "i %p %u ",
                     static_cast<const void *>(code + st), p - st);
            body += buf;
            body += mn;
            body += "\n";
        }
        fprintf(f, "frag %p %u %s#%zu undecoded=%u\n",
                static_cast<const void *>(code), fr.len, name.c_str(), r,
                undecoded);
        fputs(body.c_str(), f);
    }
    fclose(f);
}

std::string disassemble(const Chunk &chunk, const std::string &title,
                        const std::vector<std::string> &cap_names,
                        const JitCtx *jc)
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
    if (chunk.native_leaf)
        s << "; native_leaf: whole body -> one fragment @+"
          << chunk.native_entry_off << "  (call-able; #55)\n";
    /* plans/archived/model-flip.md M1: the container plan - how this body partitions
     * into NATIVE / ISLAND segments, and whether it could be ONE native
     * container. For a mixed body, list each island's pc span + the distinct
     * un-nativizable opcodes blocking it (the "what to nativize next" surface).
     * DUMP-ONLY; no emission consumes it yet. */
    {
        const ContainerPlan plan = jit_container_plan(chunk, jc);
        if (!plan.segs.empty()) {
            /* A native_leaf is container-ready by definition; its own header
             * line already says so, so only note READY for the interesting
             * not-a-leaf-yet-ready case (below MIN_RUN, multi-run, or no
             * trailing ReturnV - all things the flip WOULD nativize). */
            if (plan.container_ready && !chunk.native_leaf)
                s << "; container plan: READY - whole body native ("
                  << plan.native_op_count << " ops, "
                  << plan.segs.size() << " native run"
                  << (plan.segs.size() == 1 ? "" : "s") << ")\n";
            else if (!plan.container_ready) {
                s << "; container plan: NOT ready - " << plan.island_count
                  << " island" << (plan.island_count == 1 ? "" : "s") << " ("
                  << plan.island_op_count << " island ops, "
                  << plan.native_op_count << " native ops)\n";
                for (const ContainerSeg &sg : plan.segs) {
                    if (sg.native)
                        continue;
                    s << ";   island [pc " << sg.begin << ".." << sg.end
                      << "):";
                    /* distinct blocker opcodes, in first-seen order */
                    std::vector<OpCode> seen;
                    for (size_t p = sg.begin; p < sg.end; p++) {
                        const OpCode op = chunk.code[p].op;
                        bool have = false;
                        for (OpCode q : seen)
                            if (q == op) { have = true; break; }
                        if (!have) {
                            seen.push_back(op);
                            s << " " << opcode_name(op);
                        }
                    }
                    s << "\n";
                }
            }
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
            /* #78 step D: no catch pc - the region id names the
             * handler_sites entry the raise path dispatches from. */
            row << "try.push     region=" << in.a_lit();
            break;
        case OpCode::PopHandler:
            row << "try.pop";
            break;
        case OpCode::Rethrow:
            row << "rethrow      region=" << in.a_lit();
            break;
        case OpCode::SetPend:
            /* Only the shared finally's exits: normal or reraise (a flow op
             * inlines its own finally, so it never sets a pending action). */
            row << "set.pend     "
                << (in.target == 0 ? "normal" : "reraise")
                << "  region=" << in.a_lit();
            break;
        case OpCode::EndFinally:
            row << "end.finally  region=" << in.a_lit();
            break;

        case OpCode::IntBin:
            row << "i.bin        " << D(in.target) << " = "
                << RI(in.a(), false) << " " << opsym(in.aop) << " "
                << RI(in.b(), false);
            break;
        case OpCode::CmpIntV:
        case OpCode::CmpFloatV:
            row << (in.op == OpCode::CmpIntV ? "i.cmp.v      "
                                             : "f.cmp.v      ")
                << D(in.target) << " = " << RI(in.a(), false) << " "
                << opsym(in.aop) << " " << RI(in.b(), false) << "   ; bool";
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
        case OpCode::ExitBlock:
            /* model-flip M2: an island's fall-through exit; a_lit = the pc the
             * container resumes at. Not emitted by codegen yet (M3). */
            row << "exit.block   -> L" << in.a_lit();
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
        case OpCode::LoadElem2Int:
        case OpCode::LoadElem2Float:
            /* the fused nested read - the row is BORROWED, never boxed */
            row << (in.op == OpCode::LoadElem2Int ? "load.elem2.i "
                                                  : "load.elem2.f ")
                << D(in.target) << " = " << D(in.target2)
                << "[" << D(in.a_dual_lo()) << "]["
                << RI(in.b(), false) << "]   ; locs[" << in.a_dual_hi()
                << "]";
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
        case OpCode::OrdCharV:
            row << "ord.char     " << D(in.target) << " = ord("
                << D(in.target2) << "[" << RI(in.a(), false) << "])";
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
            const std::string o = expr14_op(in.aop);
            row << "dict.store   " << bref(in.target, in.target2) << "["
                << RI(in.a(), false) << "] " << o << " " << RI(in.b(), false);
            break;
        }
        case OpCode::StoreMemberV: {
            /* aop is the Expr14 op; the member name comes from the pool. */
            const std::string o = expr14_op(in.aop);
            row << "member.store " << bref(in.target, in.target2) << "."
                << chunk.member_keys[in.a_lit()].memId.get_type()
                       ->to_string(chunk.member_keys[in.a_lit()].memId)
                << " " << o << " " << RI(in.b(), false);
            break;
        }
        case OpCode::StoreElemValue: {
            const std::string o = expr14_op(in.aop);
            row << "store.elem.v " << bref(in.target, in.target2) << "["
                << RI(in.a(), false) << "] " << o << " " << RI(in.b(), false);
            break;
        }
        case OpCode::StoreElem2V: {
            const std::string o = expr14_op(in.aop);
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
                << in.target2 << "](";
            if (in.b_dual_hi() >= 0) {
                /* planned: print each field's SRC slot (a direct local or
                 * a computed-run temp; a is the computed mini-run dual) */
                const Chunk::CtorPlan &cp =
                    chunk.ctor_plans[in.b_dual_hi()];
                for (size_t k = 0; k < cp.f.size(); k++)
                    row << (k ? ", " : "") << D(cp.f[k].src);
                row << ") plan[" << in.b_dual_hi() << "]";
            } else {
                row << arglist(chunk, in.a_lit(), in.b_dual_lo()) << ")";
            }
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
            if (in.b_dual_lo() >= 0)
                row << " @+" << in.b_dual_lo();     /* baked byte offset */
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

/*
 * The LOADED-IMAGE dump (.myv): the same sections disassemble_program emits,
 * but read from a VmProgram instead of re-compiling an AST - no AST exists
 * after a load. Titles come from the descriptors (the same `func <name>` /
 * `closure#N` / `lambda#N` scheme), so a round-trip dump is byte-identical
 * to the source-side one.
 */
std::string disassemble_image(const VmProgram &prog)
{
    std::ostringstream s;

    if (!prog.structs.empty()) {
        s << "; ===== types (" << prog.structs.size() << ") =====\n";
        for (const auto &sd : prog.structs)
            dump_struct_type(sd.get(), s);
        s << "\n";
    }

    s << disassemble(prog.root, "main");

    int anon = 0;
    for (const auto &d : prog.funcs) {
        if (!d->vm_chunk)
            continue;
        std::vector<std::string> cap_names;
        for (const auto &c : d->captures)
            cap_names.push_back(std::string(c.name->val));
        std::string title;
        if (d->name)
            title = "func " + std::string(d->name->val);
        else
            title = (cap_names.empty() ? "lambda#" : "closure#")
                    + std::to_string(anon++);
        s << "\n" << disassemble(*static_cast<const Chunk *>(d->vm_chunk),
                                 title, cap_names);
    }
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

    std::vector<const FuncDeclStmt *> funcs;
    for (const auto &e : root->elems)
        collect_funcs(e.get(), funcs);

    /* #55 STEP 2: dump EXACTLY what execution runs, native calls included. The
     * VM's precompile (vm_precompile_all) codegens ALL bodies THEN jits ALL
     * with a JitCtx (the slot->descriptor map) so a caller's native-call gate
     * sees every callee's native_leaf flag; a per-function codegen+jit here
     * (with no JitCtx) would show interpreted CallVs instead. Replicate that
     * two-pass on THROWAWAY chunks, pointing each descriptor's `vm_chunk` at its
     * local chunk for the gate's native_leaf check, and RESTORE it after (so a
     * later execution / a test in the same process is unaffected). */
    std::unordered_map<const FuncDescriptor *, Chunk> chunks;
    std::vector<std::pair<const FuncDescriptor *, const void *>> saved;

    /* Pass A: codegen every compiled body (jit=false) - the native_leaf flag is
     * set by codegen_chunk. Point each desc->vm_chunk at its local chunk. */
    for (const FuncDeclStmt *fn : funcs) {
        Chunk ck;
        /* codegen_func_body compiles ONLY a function with bytecode (skips a base
         * template - a never-called monomorphization source - and a
         * non-scope-free closure). A skipped function shows NO chunk because it
         * HAS none, a faithful image. */
        if (!codegen_func_body(fn, ck, /*jit=*/false))
            continue;
        auto it = chunks.emplace(fn->desc, std::move(ck)).first;
        saved.push_back({ fn->desc, fn->desc->vm_chunk });
        fn->desc->vm_chunk = &it->second;
    }
    Chunk main_ck = codegen_program(root, /*jit=*/false);

    /* The global slot -> callee descriptor map (as vm_precompile_all builds). */
    std::vector<const FuncDescriptor *> slot_desc(
        root->global_func_names.size(), nullptr);
    for (const FuncDeclStmt *fn : funcs)
        if (fn->id && fn->id->sym.kind == SymKind::global) {
            const int slot = fn->id->sym.slot;
            if (slot >= 0 && static_cast<size_t>(slot) < slot_desc.size())
                slot_desc[slot] = fn->desc;
        }

    /* `fast_bind` is computed by vm_precompile_all's pass A and the SPLICE
     * gates on it (a typed param needs the coercing bind, which a MoveV is
     * not) - so it must be set here too, or a -vd dump would decline an
     * inline that execution performs. */
    for (const FuncDeclStmt *fn : funcs)
        compute_bind_flags(fn->desc);

    /* THE SPLICE, before the jit pass, exactly as vm_precompile_all does -
     * otherwise -vd would dump the un-inlined bytecode and lie about what
     * runs (the dump is the audit surface for the .myv image). */
    /*
     * ⛔ ITERATE `funcs` (SOURCE ORDER), NOT `chunks` - IT IS AN
     * unordered_map KEYED BY A POINTER (2026-08-18).
     *
     * These three passes used to walk `chunks` directly. Its key is a
     * `const FuncDescriptor *`, so the iteration order is a hash of
     * HEAP ADDRESSES - which differ between two parses of the same
     * source in the same process, and between two processes. Splice
     * and JIT order is observable in the dump (the code arena is filled
     * in that order), so `-vdj` was NON-DETERMINISTIC on any program
     * with more than one compiled function.
     *
     * It surfaced as a red Coverage lane: the new `-vdj decodes every
     * emitted form, reproducibly` check compared two dumps of one
     * struct program and they differed - on CI, on the first try, after
     * eight clean local runs. The disassembler was innocent; the DUMP
     * DRIVER was not. Same family as the builtin-slot hazard in
     * serialize.cpp, where a `std::map` keyed by an interned pointer
     * gave a different slot order per process (CLAUDE.md).
     *
     * `funcs` is built by a source-order walk, so it is stable. Any
     * future pass added here must use it for the same reason.
     */
    BcInlineSnapshots bc_snaps;
    for (const FuncDeclStmt *fn : funcs) {
        auto it = chunks.find(fn->desc);
        if (it != chunks.end())
            bc_inline_snapshot(it->second, bc_snaps);
    }
    for (const FuncDeclStmt *fn : funcs) {
        auto it = chunks.find(fn->desc);
        if (it != chunks.end())
            bc_inline_chunk(it->second, slot_desc, bc_snaps);
    }

    /* Pass B: jit each body with its JitCtx (caller_desc = its own descriptor).
     * Main gets no JitCtx - a call from main is never native (as at runtime). */
    for (const FuncDeclStmt *fn : funcs) {
        auto it = chunks.find(fn->desc);
        if (it == chunks.end())
            continue;
        JitCtx jc;
        jc.slot_desc = &slot_desc;
        jc.slot_reassigned = &root->global_slot_reassigned;
        jc.caller_desc = it->first;
        jit_compile_chunk(it->second, &jc);
    }
    jit_compile_chunk(main_ck);

    s << disassemble(main_ck, "main");

    int anon = 0;
    for (const FuncDeclStmt *fn : funcs) {
        auto it = chunks.find(fn->desc);
        if (it == chunks.end())
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

        /* Same JitCtx Pass B used, so the M1 container plan counts a native
         * CallV as native (a null jc would show it as an island). */
        JitCtx djc;
        djc.slot_desc = &slot_desc;
        djc.slot_reassigned = &root->global_slot_reassigned;
        djc.caller_desc = fn->desc;
        s << "\n" << disassemble(it->second, title, cap_names, &djc);
    }

    for (const auto &sv : saved)   /* leave the descriptors as we found them */
        sv.first->vm_chunk = sv.second;

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
