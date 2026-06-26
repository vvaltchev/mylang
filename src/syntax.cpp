/* SPDX-License-Identifier: BSD-2-Clause */

#include "errors.h"
#include "syntax.h"

using std::string;
using std::string_view;

/* Monotonic node-identity counter (see Construct::node_id in syntax.h). Starts
 * at 1 so 0 can stay a "no node" sentinel. Single-threaded interpreter, so a
 * plain increment is fine. */
uint64_t Construct::next_node_id = 1;

#ifdef RECYCLE_ALLOC

/*
 * The adversarial recycling allocator (RECYCLE=1). It NEVER returns memory to
 * the system: a freed block goes onto a size-keyed LIFO bin and the very next
 * same-size `new` reuses it - so a fresh node deterministically lands on a
 * just-freed node's address. That is exactly the condition that exposes a
 * "raw AST pointer used as a stable map key" bug (a stale entry getting matched
 * by the recycled address). Leaks by design; only for the short-lived `-rt`
 * test process. See CLAUDE.md "Invariants & hazards".
 *
 * Layout: a small header (kept max_align-wide so the returned pointer stays
 * suitably aligned) stores the block size so delete can find the right bin.
 * Under ASan we poison the user region while it sits on the free-list, so a
 * dangling access in the reuse window is still caught (forced reuse AND UAF
 * detection at once).
 */
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <new>

/* Detect ASan portably. __SANITIZE_ADDRESS__ is GCC's (and clang's) macro;
 * __has_feature is clang-only - and must be tested in a SEPARATE, nested #if,
 * because GCC's preprocessor still parses `__has_feature(...)` in a combined
 * `defined(__has_feature) && __has_feature(...)` expression and errors on it. */
#if defined(__SANITIZE_ADDRESS__)
#  define RECYCLE_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define RECYCLE_ASAN 1
#  endif
#endif

#ifdef RECYCLE_ASAN
#  include <sanitizer/asan_interface.h>
#  define RECYCLE_POISON(p, n)   __asan_poison_memory_region((p), (n))
#  define RECYCLE_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#  define RECYCLE_POISON(p, n)   ((void) 0)
#  define RECYCLE_UNPOISON(p, n) ((void) 0)
#endif

/*
 * The recycler never frees, so LeakSanitizer would report every recycled block
 * as a leak. Those leaks are intentional and bounded (one short -rt process),
 * so turn leak detection off for a RECYCLE build. (A weak hook ASan reads at
 * start; harmless if ASan isn't linked.)
 */
extern "C" const char *__asan_default_options()
{
    return "detect_leaks=0";
}

namespace {

constexpr std::size_t RECYCLE_HDR = alignof(std::max_align_t);

std::unordered_map<std::size_t, std::vector<void *>> &recycle_bins()
{
    static std::unordered_map<std::size_t, std::vector<void *>> bins;
    return bins;
}

}   /* anonymous namespace */

void *Construct::operator new(std::size_t n)
{
    auto &bin = recycle_bins()[n];
    void *base;

    if (!bin.empty()) {         /* reuse the most-recently freed block */
        base = bin.back();
        bin.pop_back();
    } else {
        base = std::malloc(n + RECYCLE_HDR);
        if (!base)
            throw std::bad_alloc();
    }

    *static_cast<std::size_t *>(base) = n;      /* remember size for delete */
    void *user = static_cast<char *>(base) + RECYCLE_HDR;
    RECYCLE_UNPOISON(user, n);
    return user;
}

void Construct::operator delete(void *p) noexcept
{
    if (!p)
        return;

    void *base = static_cast<char *>(p) - RECYCLE_HDR;
    std::size_t n = *static_cast<std::size_t *>(base);
    RECYCLE_POISON(p, n);               /* trap a dangling access till reuse */
    recycle_bins()[n].push_back(base);  /* never freed; reused by next new(n) */
}

#endif   /* RECYCLE_ALLOC */

string
escape_str(const string_view &v)
{
    string s;
    s.reserve(v.size() * 2);

    for (char c : v) {

        switch (c) {

            case '\"':
                s += "\\\"";
                break;
            case '\\':
                s += "\\\\";
                break;
            case '\r':
                s += "\\r";
                break;
            case '\n':
                s += "\\n";
                break;
            case '\t':
                s += "\\t";
                break;
            case '\v':
                s += "\\v";
                break;
            case '\a':
                s += "\\a";
                break;
            case '\b':
                s += "\\b";
                break;

            default:
                s += c;
        }
    }

    return s;
}

ostream &operator<<(ostream &s, const EvalValue &c)
{
    return s << c.to_string();
}

void ChildlessConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');
    s << indent;
    s << name;
}

static void
generic_single_child_serialize(const char *name,
                               const unique_ptr<Construct> &elem,
                               ostream &s,
                               int level)
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(";

    if (elem) {

        if (!elem->is_const)
            s << endl;

        elem->serialize(s, elem->is_const ? 0 : level + 1);

        if (!elem->is_const) {
            s << endl;
            s << indent;
        }

    } else {

        s << "<NoElem>";
    }

    s << ")";
}

void SingleChildConstruct::serialize(ostream &s, int level) const
{
    generic_single_child_serialize(name, elem, s, level);
}

void
MultiOpConstruct::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    for (const auto &[op, e] : elems) {

        if (op != Op::invalid) {
            s << string((level + 1) * 2, ' ');
            s << "Op '" << OpString[(int)op] << "'";
            s << endl;
        }

        e->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}

void
TypedScalarExpr::serialize(ostream &s, int level) const
{
    static const char *cat_name[] = {
        "arith", "cmp", "logical", "neg", "lnot"
    };

    string indent(level * 2, ' ');

    s << indent;
    s << "TypedScalarExpr<" << cat_name[(int)cat]
      << "," << (kind == TypeHint::f ? 'f' : 'i') << ">(\n";

    for (const auto &[op, e] : elems) {

        if (op != Op::invalid) {
            s << string((level + 1) * 2, ' ');
            s << "Op '" << OpString[(int)op] << "'";
            s << endl;
        }

        e->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}

void LiteralInt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Int(");
    s << std::to_string(value);
    s << ")";
}

void LiteralFloat::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Float(");
    s << std::to_string(value);
    s << ")";
}

void LiteralNone::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("None");
}

void LiteralStr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "\"";
    s << escape_str(value.get<SharedStr>().get_view());
    s << "\"";
}

void LiteralObj::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "Obj(";
    s << value.get_type()->to_string(value);
    s << ")";
}

void LiteralDictKVPair::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "KVPair(\n";

    key->serialize(s, level + 1);
    s << endl;

    value->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

void IncDecExpr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "IncDecExpr(" << (is_prefix ? "prefix " : "postfix ")
      << (is_inc ? "++" : "--") << "\n";

    lvalue->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

void MemberExpr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << "MemberExpr(\n";

    what->serialize(s, level + 1);
    s << endl;

    s << string((level + 1) * 2, ' ');
    s << "Id(\"" << memId << "\")";
    s << endl;

    s << indent;
    s << ")";
}

void Identifier::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << string("Id(\"");
    s << get_str();
    s << "\")";
}

void CallExpr::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    what->serialize(s, level + 1);
    s << endl;
    args->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

/*
 * Like the MultiElemConstruct<> template serialize, but annotates a named
 * argument with its label (a `name:` line above the value). Only the parser's
 * pre-lowering tree (e.g. what `-s` dumps) ever carries labels; after
 * lower_named_args the list is positional and prints like any ExprList.
 */
void ExprList::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    for (size_t i = 0; i < elems.size(); i++) {
        if (i < arg_names.size() && arg_names[i])
            s << string((level + 1) * 2, ' ') << arg_names[i]->val << ":\n";
        elems[i]->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}

/*
 * Named-argument desugaring (see the declaration in syntax.h). One shared
 * implementation for the parser's const-fold path and the inferencer's
 * lower_named_args, so a named call is rewritten - and therefore optimized -
 * identically wherever it is lowered.
 */
void desugar_named_call(CallExpr *call, const std::vector<ParamSpec> &params)
{
    ExprList *args = call->args.get();
    const size_t nparams = params.size();
    const size_t nargs = args->elems.size();

    std::vector<unique_ptr<Construct>> bound(nparams);
    int last = -1;                            /* highest param slot filled */

    for (size_t i = 0; i < nargs; i++) {

        Construct *anode = args->elems[i].get();
        size_t slot;

        if (!args->arg_names[i]) {
            /* positional arg: fills the next param slot, in order */
            slot = i;
            if (slot >= nparams)
                throw WrongArgCountEx(
                    intern_msg("function expects at most " +
                               std::to_string(nparams) + " argument(s)"),
                    call->start, call->end);
        } else {
            /* named arg: match a parameter by its interned name */
            slot = nparams;
            for (size_t k = 0; k < nparams; k++)
                if (params[k].name == args->arg_names[i]) { slot = k; break; }
            if (slot == nparams)
                throw TypeMismatchEx(
                    intern_msg("no parameter named '" +
                               std::string(args->arg_names[i]->val) + "'"),
                    anode->start, anode->end);
        }

        if (static_cast<int>(slot) <= last)
            throw TypeMismatchEx(
                intern_msg("named argument '" +
                           std::string(params[slot].name->val) +
                           "' is out of order or duplicates an earlier argument"
                           " (named arguments must follow parameter order)"),
                anode->start, anode->end);

        bound[slot] = move(args->elems[i]);
        last = static_cast<int>(slot);
    }

    /* Rebuild a contiguous positional list 0..last; an interior gap must be an
     * opt param and is filled with `none`. */
    auto positional = make_unique<ExprList>();
    positional->start = args->start;
    positional->end = args->end;
    bool all_const = true;

    for (int slot = 0; slot <= last; slot++) {
        if (bound[slot]) {
            all_const = all_const && bound[slot]->is_const;
            positional->elems.push_back(move(bound[slot]));
        } else {
            if (!params[slot].opt)
                throw WrongArgCountEx(
                    intern_msg("missing required argument '" +
                               std::string(params[slot].name->val) + "'"),
                    call->start, call->end);
            positional->elems.push_back(make_unique<LiteralNone>());
        }
    }

    positional->is_const = all_const;
    call->args = move(positional);
}

void StructDeclStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');
    s << indent << "StructDecl(\"" << def->name->val << "\"";

    for (const auto &f : def->fields)
        s << ", " << f.name->val;
    for (const auto &c : def->consts)
        s << ", const " << c.first->val;

    s << ")";
}

unique_ptr<Construct> StructDeclStmt::clone() const
{
    auto c = make_unique<StructDeclStmt>();
    copy_base_fields(*c);
    c->id = clone_as(id);
    c->def = make_unique<StructTypeDef>(*def);
    return c;
}

void Expr14::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;

    if (fl & pFlags::pInDecl) {

        if (fl & pFlags::pInConstDecl)
            s << "ConstDecl";
        else
            s << "VarDecl";

    } else {
        s << name;
    }

    s << "(\n";
    lvalue->serialize(s, level + 1);
    s << endl;

    s << string((level + 1) * 2, ' ');
    s << "Op '" << OpString[(int)op] << "'";
    s << endl;

    rvalue->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

void IfStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    condExpr->serialize(s, level+1);
    s << endl;

    if (thenBlock) {

        thenBlock->serialize(s, level+1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoThenBlock>";
    }

    s << endl;

    if (elseBlock) {

        elseBlock->serialize(s, level+1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoElseBlock>";
    }

    s << endl;
    s << indent;
    s << ")";
}

void WhileStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";
    condExpr->serialize(s, level+1);
    s << endl;

    if (body) {

        body->serialize(s, level+1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoBody>";
    }

    s << endl;
    s << indent;
    s << ")";
}

void FuncDeclStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    if (id) {

        id->serialize(s, level + 1);
        s << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoName>" << endl;
    }

    if (captures) {

        captures->serialize(s, level + 1);
        s << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoCaptures>" << endl;
    }

    if (params) {

        params->serialize(s, level + 1);
        s << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoParams>" << endl;
    }

    body->serialize(s, level + 1);
    s << endl;

    s << indent;
    s << ")";
}

void ReturnStmt::serialize(ostream &s, int level) const
{
    generic_single_child_serialize(name, elem, s, level);
}

void Subscript::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";
    what->serialize(s, level+1);
    s << endl;
    index->serialize(s, level+1);
    s << endl;
    s << indent;
    s << ")";
}

void Slice::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";
    what->serialize(s, level+1);
    s << endl;

    if (start_idx) {

        start_idx->serialize(s, level+1);
        s << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoStartIndex>" << endl;
    }

    if (end_idx) {

        end_idx->serialize(s, level+1);
        s << endl;

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoEndIndex>" << endl;
    }

    s << indent;
    s << ")";
}

void TryCatchStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    tryBody->serialize(s, level+1);
    s << endl;

    for (const auto &p : catchStmts) {

        IdList *exList = p.first.exList.get();
        Identifier *asId = p.first.asId.get();
        Construct *body = p.second.get();

        s << string((level + 1) * 2, ' ');
        s << "Catch( ";

        if (exList) {

            for (const unique_ptr<Identifier> &e : exList->elems) {
                s << e->get_str() << " ";
            }

            if (asId) {
                s << "as " << asId->get_str() << " ";
            }

        } else {
            s << "<anything>";
        }

        s << ") (\n";
        body->serialize(s, level+2);
        s << endl;
        s << string((level + 1) * 2, ' ');
        s << ")\n";
    }

    if (finallyBody) {
        s << string((level + 1) * 2, ' ');
        s << "Finally( ";

        finallyBody->serialize(s, level+2);
        s << endl;
        s << ")\n";
    }

    s << indent;
    s << ")";
}

void ForeachStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    ids->serialize(s, level + 1);
    s << endl;

    container->serialize(s, level + 1);
    s << endl;

    if (body) {

        body->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoBody>";
    }

    s << endl;
    s << indent;
    s << ")";
}

void ForStmt::serialize(ostream &s, int level) const
{
    string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    if (init) {

        init->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoInit>";
    }

    s << endl;

    if (cond) {

        cond->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoCond>";
    }

    s << endl;

    if (inc) {

        inc->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoInc>";
    }

    s << endl;

    if (body) {

        body->serialize(s, level + 1);

    } else {

        s << string((level + 1) * 2, ' ');
        s << "<NoBody>";
    }

    s << endl;
    s << indent;
    s << ")";
}
