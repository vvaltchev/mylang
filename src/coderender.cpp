/* SPDX-License-Identifier: BSD-2-Clause */

#include "coderender.h"
#include "syntax.h"
#include "lexer.h"          /* OpString */
#include "errors.h"         /* InlineCtx */
#include "eval.h"
#include "evalvalue.h"
#include "structtype.h"
#include "cyclewalk.h"      /* a baked value that contains itself */
#include "reflect.h"        /* reflect_typeof for LiteralObj element types */

#include <cmath>
#include <sstream>
#include <string>
#include <cstdio>
#include <cstdlib>

using std::string;

namespace {

/* Operator precedence (higher binds tighter), matching the parser ladder. */
int op_prec(Op op)
{
    switch (op) {
        case Op::lor:                                   return 2;
        case Op::land:                                  return 3;
        case Op::eq:   case Op::noteq:                  return 4;
        case Op::lt:   case Op::gt: case Op::le: case Op::ge: return 5;
        case Op::plus: case Op::minus:                  return 6;
        case Op::times: case Op::div: case Op::mod:     return 7;
        default:                                        return 8;  /* unary */
    }
}

const int PREC_POSTFIX = 9;    /* call / subscript / member */
const int PREC_PRIMARY = 10;   /* literal / id / parenthesized */

/* A float literal that reads back as the SAME double, and as a float: the
 * fewest significant digits (%.*g, 1 to 17 - 17 always round-trips a
 * double) whose strtod is the value, with ".0" when the text would read as
 * an int. `%g` alone kept six digits: 1234567.0 printed as 1.23457e+06. */
string fmt_float(float_type v)
{
    if (std::isnan(v))
        return "nan";                  /* never C's sign-bit `-nan` */
    if (std::isinf(v))
        return v < 0 ? "-inf" : "inf";
    const double d = static_cast<double>(v);
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d)
            break;
    }
    string s(buf);
    if (s.find_first_of(".e") == string::npos)   /* no '.', no exponent */
        s += ".0";
    return s;
}

string escape_string(const std::string_view &sv)
{
    string out = "\"";
    for (char c : sv) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    out += "\"";
    return out;
}

/* A var/const/param declared-type keyword for a DeclType, or "" if none. */
const char *decl_type_kw(DeclType d)
{
    switch (d) {
        case DeclType::b:    return "bool";
        case DeclType::i:    return "int";
        case DeclType::f:    return "float";
        case DeclType::s:    return "str";
        case DeclType::arr:  return "array";
        case DeclType::dict: return "dict";
        default:             return "";
    }
}

/* The flat-array element type a hint implies (informative; `array<int>` is not
 * valid script syntax but conveys the specialized storage). */
const char *arr_hint_type(ArrHint h)
{
    switch (h) {
        case ArrHint::flat_i: return "array<int>";
        case ArrHint::flat_f: return "array<float>";
        case ArrHint::flat_b: return "array<bool>";
        case ArrHint::flat_s: return "array<struct>";
        case ArrHint::flat_c: return "array<class>";
        default:              return nullptr;
    }
}

struct Renderer {

    std::ostream &o;
    const InlineCtx *cur_inline = nullptr;   /* the inline frame we're inside */
    /* inferred parameter types for the top-level function (REPL :show only);
     * empty falls back to the AST's explicit annotations / dyn modifiers. */
    const std::vector<std::string> *ptypes = nullptr;
    string ret_type;                /* inferred return type prefix, or "" */

    explicit Renderer(std::ostream &os) : o(os) {}

    static string ind(int n) { return string(n * 4, ' '); }

    /* Track entry into a spliced (inlined) subtree. We deliberately do NOT print
     * a per-node "inlined NAME" marker: a guard body is inlined as a real
     * TERNARY (expressible code that needs no annotation), and marking every
     * interior node floods a deeply-nested unroll. The only marker is one
     * emitted by the InlinedCallExpr case (the non-expressible block form). */
    bool enter_inline(const Construct *c, const InlineCtx *&saved)
    {
        saved = cur_inline;
        if (c->inline_ctx && c->inline_ctx != cur_inline) {
            cur_inline = c->inline_ctx;
            return true;
        }
        return false;
    }
    void leave_inline(const InlineCtx *saved) { cur_inline = saved; }

    /* ------------------------------ expressions ----------------------- */

    /* Render `c` as an expression, parenthesizing if its precedence is below
     * `min_prec`. */
    void expr(const Construct *c, int min_prec)
    {
        const InlineCtx *saved;
        const bool tagged = enter_inline(c, saved);

        const int prec = expr_prec(c);
        const bool paren = prec < min_prec;
        if (paren) o << "(";
        expr_inner(c);
        if (paren) o << ")";

        if (tagged) leave_inline(saved);
    }

    int expr_prec(const Construct *c)
    {
        /* an assignment is the loosest expression of all: as an operand
         * it needs its parentheses (`(u = 3) + 1`) */
        if (ctag(c) == ConstructType::expr14)
            return 0;
        /* a lambda's `=>` body would swallow what follows it, and a call
         * of one must not read as a call of its body */
        if (ctag(c) == ConstructType::func_decl)
            return 1;
        if (auto *e = dynamic_cast<const TypedScalarExpr *>(c)) {
            if (e->cat == TypedScalarExpr::Cat::neg ||
                e->cat == TypedScalarExpr::Cat::lnot)
                return 8;
            return e->elems.size() > 1 ? op_prec(e->elems[1].first) : 8;
        }
        if (auto *m = dynamic_cast<const MultiOpConstruct *>(c)) {
            if (dynamic_cast<const Expr02 *>(c))
                return 8;
            return m->elems.size() > 1 ? op_prec(m->elems[1].first) : 8;
        }
        if (dynamic_cast<const CallExpr *>(c) ||
            dynamic_cast<const Subscript *>(c) ||
            dynamic_cast<const Slice *>(c) ||
            dynamic_cast<const MemberExpr *>(c))
            return PREC_POSTFIX;
        if (auto *e = dynamic_cast<const IncDecExpr *>(c))
            return e->is_prefix ? 8 : PREC_POSTFIX;
        if (ctag(c) == ConstructType::deref)
            return 8;                       /* a prefix operator */
        /* ternary / ?? are the loosest expressions (just above assignment) */
        if (dynamic_cast<const TernaryExpr *>(c) ||
            dynamic_cast<const CoalesceExpr *>(c))
            return 1;
        if (auto *e1 = dynamic_cast<const Expr01 *>(c))
            return e1->elem ? expr_prec(e1->elem.get()) : PREC_PRIMARY;
        return PREC_PRIMARY;
    }

    /* render a flat (Op,operand) chain: operand0 op1 operand1 op2 ... */
    void op_chain(const std::vector<std::pair<Op, unique_ptr<Construct>>> &el,
                  int my_prec)
    {
        if (!el.empty() && el[0].first != Op::invalid) {
            /* unary prefix form (Expr02 / TypedScalar neg/lnot) */
            o << OpString[(int)el[0].first];
            expr(el[0].second.get(), 8);
            return;
        }
        for (size_t i = 0; i < el.size(); i++) {
            if (i) o << " " << OpString[(int)el[i].first] << " ";
            expr(el[i].second.get(), my_prec + 1);
        }
    }

    /* Render `c` as code. EXHAUSTIVE over ConstructType, with no `default`:
     * -Werror=switch makes a new node kind a build error until it is given
     * a rendering here. (A dynamic_cast ladder ending in a placeholder used
     * to print a comment naming any node it had no arm for - a lambda value
     * as `FuncDeclStmt`, a multi-assignment target as `IdList`.) */
    void expr_inner(const Construct *c)
    {
        switch (ctag(c)) {

        case ConstructType::lit_int:
            o << static_cast<const LiteralInt *>(c)->ival();
            return;

        case ConstructType::lit_float:
            o << fmt_float(static_cast<const LiteralFloat *>(c)->fval());
            return;

        case ConstructType::lit_bool:
            o << (static_cast<const LiteralBool *>(c)->bval()
                      ? "true" : "false");
            return;

        case ConstructType::lit_none:
            o << "none";
            return;

        case ConstructType::lit_str:
            o << escape_string(static_cast<const LiteralStr *>(c)
                                   ->strval().get<SharedStr>().get_view());
            return;

        case ConstructType::lit_obj: {
            /* a baked const array/dict/struct */
            CycStack st;
            value(static_cast<const LiteralObj *>(c)->literal_value(), st);
            return;
        }

        case ConstructType::id:
            o << static_cast<const Identifier *>(c)->get_str();
            return;

        case ConstructType::idlist: {
            /* a multi-assignment's targets: `a, b = ...` */
            const auto *l = static_cast<const IdList *>(c);
            for (size_t i = 0; i < l->elems.size(); i++) {
                if (i) o << ", ";
                o << l->elems[i]->get_str();
            }
            return;
        }

        case ConstructType::expr_list:
            args(static_cast<const ExprList *>(c));
            return;

        case ConstructType::lit_arr: {
            const auto *e = static_cast<const LiteralArray *>(c);
            o << "[";
            for (size_t i = 0; i < e->elems.size(); i++) {
                if (i) o << ", ";
                expr(e->elems[i].get(), 0);
            }
            o << "]";
            return;
        }

        case ConstructType::lit_dict: {
            const auto *e = static_cast<const LiteralDict *>(c);
            o << "{";
            for (size_t i = 0; i < e->elems.size(); i++) {
                if (i) o << ", ";
                expr_inner(e->elems[i].get());
            }
            o << "}";
            return;
        }

        case ConstructType::lit_dict_kv: {
            const auto *kv = static_cast<const LiteralDictKVPair *>(c);
            expr(kv->key.get(), 0);
            o << ": ";
            expr(kv->value.get(), 0);
            return;
        }

        case ConstructType::member: {
            const auto *e = static_cast<const MemberExpr *>(c);
            expr(e->what.get(), PREC_POSTFIX);
            o << (e->optional ? "?." : ".") << e->memId;
            return;
        }

        case ConstructType::subscript: {
            const auto *e = static_cast<const Subscript *>(c);
            expr(e->what.get(), PREC_POSTFIX);
            o << "[";
            expr(e->index.get(), 0);
            o << "]";
            return;
        }

        case ConstructType::slice: {
            const auto *e = static_cast<const Slice *>(c);
            expr(e->what.get(), PREC_POSTFIX);
            o << "[";
            if (e->start_idx) expr(e->start_idx.get(), 0);
            o << ":";
            if (e->end_idx) expr(e->end_idx.get(), 0);
            o << "]";
            return;
        }

        case ConstructType::call: {
            /* the whole CallExpr family (Direct/Cached/DirectBuiltin share
             * the tag and the layout) */
            const auto *e = static_cast<const CallExpr *>(c);
            expr(e->what.get(), PREC_POSTFIX);
            o << "(";
            if (e->args)
                args(e->args.get());
            o << ")";
            return;
        }

        case ConstructType::incdec: {
            const auto *e = static_cast<const IncDecExpr *>(c);
            const char *op = e->is_inc ? "++" : "--";
            if (e->is_prefix) { o << op; expr(e->lvalue.get(), 8); }
            else { expr(e->lvalue.get(), PREC_POSTFIX); o << op; }
            return;
        }

        case ConstructType::deref:
            o << "*";
            expr(static_cast<const DerefExpr *>(c)->elem.get(), 8);
            return;

        case ConstructType::ternary: {
            const auto *e = static_cast<const TernaryExpr *>(c);
            expr(e->condExpr.get(), 2);
            o << " ? ";
            expr(e->thenExpr.get(), 2);
            o << " : ";
            expr(e->elseExpr.get(), 1);     /* right-assoc */
            return;
        }

        case ConstructType::coalesce: {
            const auto *e = static_cast<const CoalesceExpr *>(c);
            expr(e->lhs.get(), 2);
            o << " ?? ";
            expr(e->rhs.get(), 1);          /* right-assoc */
            return;
        }

        case ConstructType::typed_scalar:
            op_chain(static_cast<const TypedScalarExpr *>(c)->elems,
                     expr_prec(c));
            return;

        case ConstructType::expr01: {
            const auto *e = static_cast<const Expr01 *>(c);
            if (e->elem) expr_inner(e->elem.get());
            return;
        }

        case ConstructType::expr02: case ConstructType::expr03:
        case ConstructType::expr04: case ConstructType::expr05:
        case ConstructType::expr06: case ConstructType::expr07:
        case ConstructType::expr08: case ConstructType::expr09:
        case ConstructType::expr10: case ConstructType::expr11:
        case ConstructType::expr12:
            op_chain(static_cast<const MultiOpConstruct *>(c)->elems,
                     expr_prec(c));
            return;

        case ConstructType::expr14:
            assignment(static_cast<const Expr14 *>(c));
            return;

        case ConstructType::func_decl:
            /* a function VALUE (a lambda); a named function is a
             * statement and never an operand */
            func(static_cast<const FuncDeclStmt *>(c), cur_level);
            return;

        case ConstructType::inlined_call:
            inlined_call(static_cast<const InlinedCallExpr *>(c));
            return;

        /* statements: never an operand in a tree the parser and the
         * passes build (an inlined body is a Block the inlined_call arm
         * renders itself) - a statement position goes through stmt() */
        case ConstructType::other:
        case ConstructType::nop:
        case ConstructType::brk:
        case ConstructType::cont:
        case ConstructType::rethrow:
        case ConstructType::throw_stmt:
        case ConstructType::block:
        case ConstructType::if_stmt:
        case ConstructType::ret:
        case ConstructType::while_stmt:
        case ConstructType::struct_decl:
        case ConstructType::try_catch:
        case ConstructType::foreach_stmt:
        case ConstructType::for_stmt:
        case ConstructType::for_range:
            o << "/* " << c->name << " */";
            return;
        }
    }

    /* A baked constant's VALUE as code: its print form - which is code
     * already (`[1, 2]`, `{"k": 1}`, `P(x: 1, y: 2)`, `box(5)`) - except
     * that a float is written to read back as the same double (fmt_float);
     * the print form keeps six decimals (`[0.333333]`). A value that
     * contains itself renders its back edge as the print form does,
     * `[...]` / `{...}` / `Name(...)` (cyclewalk.h). */
    void value(const EvalValue &v, CycStack &st)
    {
        switch (v.get_type()->t) {

        case Type::t_float:
            o << fmt_float(v.get<float_type>());
            return;

        case Type::t_arr: {
            const SharedArrayObj &a = v.get_ref<SharedArrayObj>();
            CycKey k;
            const bool keyed = cyc_key(v, k);
            if (keyed && st.contains(k)) {
                o << "[...]";
                return;
            }
            CycGuard g(st, k, keyed);
            o << "[";
            for (size_type i = 0; i < a.size(); i++) {
                if (i) o << ", ";
                value(vm_arr_elem(a, i), st);
            }
            o << "]";
            return;
        }

        case Type::t_dict: {
            const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
            const CycKey k = cyc_key_obj(&d);
            if (st.contains(k)) {
                o << "{...}";
                return;
            }
            CycGuard g(st, k);
            o << "{";
            bool first = true;
            for (const auto &kv : d.get_ref()) {
                if (!first) o << ", ";
                first = false;
                value(kv.first, st);
                o << ": ";
                value(kv.second.get(), st);
            }
            o << "}";
            return;
        }

        case Type::t_struct: {
            const StructObject &s = *v.get_ref<intrusive_ptr<StructObject>>();
            const StructTypeDef &def = *s.def;
            if (s.boxed) o << "box(";
            o << def.name->val;
            CycKey k;
            const bool keyed = cyc_key(v, k);
            if (keyed && st.contains(k)) {
                o << (s.boxed ? "(...))" : "(...)");
                return;
            }
            CycGuard g(st, k, keyed);
            o << "(";
            for (size_t i = 0; i < def.fields.size(); i++) {
                if (i) o << ", ";
                o << def.fields[i].name->val << ": ";
                value(s.is_pod() ? s.pod_get(static_cast<int>(i))
                                 : s.fields[i].get(), st);
            }
            o << (s.boxed ? "))" : ")");
            return;
        }

        case Type::t_box:
            o << "box(";
            value(v.get_ref<intrusive_ptr<BoxObj>>()->v, st);
            o << ")";
            return;

        default:
            o << v.to_string_repr();
            return;
        }
    }

    /* a call's arguments, with a named argument's label: the REPL's
     * `:show EXPR` renders a tree the inferencer has not desugared, where
     * dropping `y:` from `P(y: 5, x: 6)` would change which field gets
     * which value */
    void args(const ExprList *l)
    {
        for (size_t i = 0; i < l->elems.size(); i++) {
            if (i) o << ", ";
            if (i < l->arg_names.size() && l->arg_names[i])
                o << l->arg_names[i]->val << ": ";
            expr(l->elems[i].get(), 0);
        }
    }

    static size_t chain_len(const InlineCtx *c)
    {
        size_t n = 0;
        for (; c; c = c->parent)
            n++;
        return n;
    }

    /* The frame of the callee an inlined block body came from. The inliner
     * tags the BODY's statements with the callee's frame, whose parent is
     * the call site's chain; the InlinedCallExpr, its Block and the
     * argument-temp stores keep the call site's own chain (resolver.cpp,
     * try_inline_block). So the callee is the element of a statement's
     * chain just inside the call site's - found by LENGTH, not by pointer:
     * when the enclosing function is itself inlined, every node's chain is
     * re-rooted into a fresh copy (tag_inline / rebase), and the copies do
     * not share their parents. */
    static const InlineCtx *callee_frame(const InlinedCallExpr *e)
    {
        auto *b = dynamic_cast<const Block *>(e->elem.get());
        if (!b)
            return nullptr;
        const size_t n = chain_len(e->inline_ctx);
        for (const auto &s : b->elems) {
            const size_t m = chain_len(s->inline_ctx);
            if (m <= n)
                continue;           /* an argument temp: the call site's */
            const InlineCtx *c = s->inline_ctx;
            for (size_t i = 0; i + n + 1 < m; i++)
                c = c->parent;
            return c;
        }
        return nullptr;
    }

    /* An inlined block body in expression position (the non-expressible
     * form, used when the body is not a temp-free guard chain - a guard body
     * is inlined as a real ternary instead). Label it once, then render the
     * single-return body as its value, else the block. */
    void inlined_call(const InlinedCallExpr *e)
    {
        const InlineCtx *fr = callee_frame(e);
        const char *nm = fr ? fr->callee_name.c_str() : "?";
        auto *b = dynamic_cast<const Block *>(e->elem.get());
        const ReturnStmt *r = (b && b->elems.size() == 1)
            ? dynamic_cast<const ReturnStmt *>(b->elems[0].get()) : nullptr;
        if (r) {
            o << "/* inlined " << nm << " */ (";
            if (r->elem) expr(r->elem.get(), 0); else o << "none";
            o << ")";
        } else if (e->elem) {
            /* a multi-statement inlined body: render it as a brace block so
             * the expansion is visible. Best-effort - a block in expression
             * position is not valid MyLang, but :show is a decompiler. */
            o << "/* inlined " << nm << " */ ";
            const int lvl = cur_level;
            inline_block(e->elem.get(), lvl);
            cur_level = lvl;
        }
    }

    /* `a = v`, `a += v`, `a, b = v`, and a declaration `var a = v` */
    void assignment(const Expr14 *e14)
    {
        if (e14->fl & pFlags::pInDecl) {
            const bool is_const = e14->fl & pFlags::pInConstDecl;
            if (auto *lv = dynamic_cast<const Identifier *>(e14->lvalue.get()))
                o << decl_kw(lv, e14->rvalue.get(), is_const) << " "
                  << lv->get_str();
            else if (auto *l = dynamic_cast<const IdList *>(
                         e14->lvalue.get())) {
                /* `var a, b = ...`: the keyword of the first target (the
                 * annotation and the modifiers apply to all of them) */
                o << (l->elems.empty()
                          ? string(is_const ? "const" : "var")
                          : decl_kw(l->elems[0].get(), nullptr, is_const))
                  << " ";
                expr_inner(l);
            } else
                expr(e14->lvalue.get(), 0);
            o << " = ";
            expr(e14->rvalue.get(), 0);
            return;
        }
        expr(e14->lvalue.get(), 0);
        o << " " << OpString[(int)e14->op] << " ";
        expr(e14->rvalue.get(), 0);
    }

    /* ------------------------------ statements ------------------------ */

    /* The keywords of a `var`/`const` decl, `[const] [opt] TYPE`: TYPE is an
     * explicit annotation, else best-effort from the rvalue's hints
     * (typed-scalar / flat-array / literal), else "var". A `dyn` or `opt`
     * modifier is part of the declaration: `var dyn t = d ?? 7` without
     * its `dyn` does not compile. */
    string decl_kw(const Identifier *lv, const Construct *rv, bool is_const)
    {
        if (lv->dyn_mod)            /* var dyn / var opt dyn / const dyn */
            return string(is_const ? "const " : "var ") +
                   (lv->opt_mod ? "opt " : "") + "dyn";
        const string prefix = string(is_const ? "const " : "") +
                              (lv->opt_mod ? "opt " : "");
        if (lv->decl_type != DeclType::none)        /* explicit annotation */
            return prefix + decl_type_kw(lv->decl_type);
        if (rv) {                                   /* best-effort inference */
            if (const char *at = arr_hint_type(rv->arr_hint))
                return prefix + at;
            if (auto *lo = dynamic_cast<const LiteralObj *>(rv))
                return prefix + reflect_typeof(lo->literal_value());
            /* th == i is ALSO a bool's hint (Construct::th_bool) */
            if (rv->th == TypeHint::i)
                return prefix + (rv->th_bool ? "bool" : "int");
            if (rv->th == TypeHint::f) return prefix + "float";
            if (dynamic_cast<const LiteralInt *>(rv))   return prefix + "int";
            if (dynamic_cast<const LiteralFloat *>(rv)) return prefix + "float";
            if (dynamic_cast<const LiteralBool *>(rv))  return prefix + "bool";
            if (dynamic_cast<const LiteralStr *>(rv))   return prefix + "str";
        }
        return string(is_const ? "const" : "var") +
               (lv->opt_mod ? " opt" : "");
    }

    int cur_level = 0;   /* the enclosing statement's indent, for an inlined
                          * block body rendered inside an expression */

    void stmt(const Construct *c, int level)
    {
        if (!c || ctag(c) == ConstructType::nop)
            return;

        cur_level = level;
        const InlineCtx *saved;
        const bool tagged = enter_inline(c, saved);

        if (ctag(c) == ConstructType::block) {
            block(static_cast<const Block *>(c), level);
            if (tagged) leave_inline(saved);
            return;
        }

        o << ind(level);

        switch (ctag(c)) {

        case ConstructType::func_decl: {
            auto *f = static_cast<const FuncDeclStmt *>(c);
            func(f, level);
            /* a lambda standing as a statement is an expression statement */
            o << (f->id ? "\n" : ";\n");
            break;
        }

        case ConstructType::struct_decl: {
            auto *s = static_cast<const StructDeclStmt *>(c);
            o << (s->def->is_class ? "class " : "struct ")
              << s->def->name->val << " { ... }\n";
            break;
        }

        case ConstructType::if_stmt: {
            auto *i = static_cast<const IfStmt *>(c);
            o << "if (";
            expr(i->condExpr.get(), 0);
            o << ") ";
            inline_block(i->thenBlock.get(), level);
            if (i->elseBlock) {
                o << " else ";
                inline_block(i->elseBlock.get(), level);
            }
            o << "\n";
            break;
        }

        case ConstructType::while_stmt: {
            auto *w = static_cast<const WhileStmt *>(c);
            o << "while (";
            expr(w->condExpr.get(), 0);
            o << ") ";
            inline_block(w->body.get(), level);
            o << "\n";
            break;
        }

        case ConstructType::for_stmt: {
            auto *f = static_cast<const ForStmt *>(c);
            o << "for (";
            if (f->init) stmt_oneline(f->init.get());
            o << "; ";
            if (f->cond) expr(f->cond.get(), 0);
            o << "; ";
            if (f->inc) stmt_oneline(f->inc.get());
            o << ") ";
            inline_block(f->body.get(), level);
            o << "\n";
            break;
        }

        case ConstructType::for_range: {
            /* the specialized counted loop - shown as the equivalent for()
             * with a "counted" marker so the optimization is visible. */
            auto *fr = static_cast<const ForRangeStmt *>(c);
            const Construct *ivar = nullptr;
            if (auto *e14 = dynamic_cast<const Expr14 *>(fr->init.get()))
                ivar = e14->lvalue.get();
            const bool asc = (fr->cmp_op == Op::lt || fr->cmp_op == Op::le);
            o << "for (";
            if (fr->init) stmt_oneline(fr->init.get());
            o << "; ";
            if (ivar) expr(ivar, 0);
            o << " " << OpString[(int)fr->cmp_op] << " ";
            expr(fr->bound.get(), 0);
            o << "; ";
            if (ivar) expr(ivar, 0);
            if (fr->step) {
                o << (asc ? " += " : " -= ");
                expr(fr->step.get(), 0);
            } else {
                o << (asc ? "++" : "--");
            }
            o << ") /* counted */ ";
            inline_block(fr->body.get(), level);
            o << "\n";
            break;
        }

        case ConstructType::foreach_stmt: {
            auto *fe = static_cast<const ForeachStmt *>(c);
            o << "foreach (";
            if (fe->idsVarDecl) o << "var ";
            for (size_t i = 0; i < fe->ids->elems.size(); i++) {
                if (i) o << ", ";
                o << fe->ids->elems[i]->get_str();
            }
            o << " in ";
            if (fe->indexed) o << "indexed ";
            expr(fe->container.get(), 0);
            o << ") ";
            inline_block(fe->body.get(), level);
            o << "\n";
            break;
        }

        case ConstructType::ret: {
            auto *r = static_cast<const ReturnStmt *>(c);
            o << "return";
            if (r->elem) { o << " "; expr(r->elem.get(), 0); }
            o << ";\n";
            break;
        }

        case ConstructType::try_catch: {
            auto *t = static_cast<const TryCatchStmt *>(c);
            o << "try ";
            inline_block(t->tryBody.get(), level);
            for (const auto &cs : t->catchStmts) {
                o << " catch ";
                /* a catch-anything has no list - and no parentheses:
                 * `catch () {` does not parse */
                if (cs.first.exList) {
                    o << "(";
                    expr_inner(cs.first.exList.get());
                    if (cs.first.asId)
                        o << " as " << cs.first.asId->get_str();
                    o << ") ";
                }
                inline_block(cs.second.get(), level);
            }
            if (t->finallyBody) {
                o << " finally ";
                inline_block(t->finallyBody.get(), level);
            }
            o << "\n";
            break;
        }

        case ConstructType::brk:
            o << "break;\n";
            break;

        case ConstructType::cont:
            o << "continue;\n";
            break;

        case ConstructType::rethrow:
            o << "rethrow;\n";
            break;

        case ConstructType::throw_stmt:
            o << "throw ";
            expr(static_cast<const ThrowStmt *>(c)->elem.get(), 0);
            o << ";\n";
            break;

        default:
            /* an expression statement (call, assignment, declaration...):
             * every other kind is an expression, which expr_inner's
             * exhaustive switch renders */
            stmt_oneline(c);
            o << ";\n";
            break;
        }

        if (tagged) leave_inline(saved);
    }

    /* a one-line statement body without trailing ';' or newline (for/decl) */
    void stmt_oneline(const Construct *c)
    {
        expr(c, 0);
    }

    /* render a block as `{ ... }` after a `keyword (...)`; a non-block body is
     * still wrapped so the output is unambiguous. */
    void inline_block(const Construct *c, int level)
    {
        if (auto *b = dynamic_cast<const Block *>(c)) {
            o << "{\n";
            for (const auto &e : b->elems)
                stmt(e.get(), level + 1);
            o << ind(level) << "}";
            return;
        }
        o << "{\n";
        stmt(c, level + 1);
        o << ind(level) << "}";
    }

    void block(const Block *b, int level)
    {
        o << ind(level) << "{\n";
        for (const auto &e : b->elems)
            stmt(e.get(), level + 1);
        o << ind(level) << "}\n";
    }

    /* a function header `[pure] func NAME(params)` using its REAL id (so a
     * clone shows `func f$0(...)`), then its body. A function VALUE with no
     * name (a lambda) is the expression `func [captures] (params) ...`,
     * whose `=> expr` body takes no `;` (the statement holding it does). */
    void func(const FuncDeclStmt *f, int level)
    {
        if (f->desc->explicit_pure || f->desc->effective_pure)
            o << (f->desc->explicit_pure ? "pure " : "/* pure */ ");
        if (!ret_type.empty())                       /* inferred return type */
            o << ret_type << " ";
        o << "func ";
        if (f->id) {
            o << f->id->get_str();
        } else {
            if (f->captures && !f->captures->elems.empty()) {
                o << "[";
                expr_inner(f->captures.get());
                o << "] ";
            }
        }
        o << "(";
        if (f->params)
            for (size_t i = 0; i < f->params->elems.size(); i++) {
                if (i) o << ", ";
                const Identifier *p = f->params->elems[i].get();
                if (ptypes && i < ptypes->size() && !(*ptypes)[i].empty())
                    o << param_str_typed(p, (*ptypes)[i]);   /* inferred type */
                else
                    o << param_str(p);                  /* AST annotation */
            }
        o << ") ";

        /* the signature consumed the inferred types; clear them so a nested
         * function in the body doesn't inherit this function's types. */
        ptypes = nullptr;
        ret_type.clear();

        /* A single-return body - the `=> expr` sugar's parsed form (or its
         * hand-written twin) - renders back as `=> expr;`, the idiomatic
         * spelling. The body's statements move cur_level; a lambda is
         * part of an enclosing statement, which goes on at its own. */
        const int outer_level = cur_level;
        if (const Construct *eb = func_expr_body(f)) {
            o << "=> ";
            expr(eb, 0);
            if (f->id)
                o << ";";
        } else if (auto *b = dynamic_cast<const Block *>(f->body.get())) {
            o << "{\n";
            for (const auto &e : b->elems)
                stmt(e.get(), level + 1);
            o << ind(level) << "}";
        } else {
            o << "{ }";
        }
        cur_level = outer_level;
    }

    /* `[const ]<inferred-type> name` - the inferred-type string already carries
     * `opt` where applicable, so only `const` is taken from the AST. */
    string param_str_typed(const Identifier *p, const string &ty)
    {
        string s;
        if (p->const_param) s += "const ";
        s += ty + " ";
        s += string(p->get_str());
        return s;
    }

    string param_str(const Identifier *p)
    {
        string s;
        if (p->const_param) s += "const ";
        if (p->opt_mod) s += "opt ";
        if (const char *t = decl_type_kw(p->decl_type)) {
            if (*t) { s += t; s += " "; }
            else if (p->dyn_mod) s += "dyn ";
        } else if (p->dyn_mod) {
            s += "dyn ";
        }
        s += string(p->get_str());
        return s;
    }
};

}  /* namespace */

string render_func_code(const FuncDeclStmt *fn,
                        const std::vector<std::string> &param_types,
                        const std::string &ret_type)
{
    if (!fn)
        return "";
    std::ostringstream o;
    Renderer r(o);
    if (!param_types.empty())
        r.ptypes = &param_types;
    r.ret_type = ret_type;
    r.func(fn, 0);
    o << "\n";
    return o.str();
}

string render_construct_code(const Construct *c)
{
    if (!c)
        return "";
    std::ostringstream o;
    Renderer r(o);
    /* a lambda is an expression; a NAMED function is a declaration */
    const auto *fd = dynamic_cast<const FuncDeclStmt *>(c);
    if (dynamic_cast<const Block *>(c) ||
        dynamic_cast<const IfStmt *>(c) ||
        dynamic_cast<const WhileStmt *>(c) ||
        dynamic_cast<const ForStmt *>(c) ||
        dynamic_cast<const ForRangeStmt *>(c) ||
        dynamic_cast<const ForeachStmt *>(c) ||
        dynamic_cast<const ReturnStmt *>(c) ||
        (fd && fd->id) ||
        dynamic_cast<const TryCatchStmt *>(c))
        r.stmt(c, 0);
    else
        r.expr(c, 0);
    return o.str();
}
