/* SPDX-License-Identifier: BSD-2-Clause */

#include "coderender.h"
#include "syntax.h"
#include "lexer.h"          /* OpString */
#include "errors.h"         /* InlineCtx */
#include "eval.h"
#include "evalvalue.h"
#include "structtype.h"
#include "reflect.h"        /* reflect_typeof for LiteralObj element types */

#include <sstream>
#include <string>
#include <cstdio>

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

/* A readable float literal: %g, with ".0" when it would otherwise look int. */
string fmt_float(float_type v)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    string s(buf);
    if (s.find_first_of(".eEni") == string::npos)   /* no '.', exp, inf, nan */
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

    /* Note a transition into a spliced (inlined) subtree once, so we don't
     * repeat the annotation on every interior node. */
    bool enter_inline(const Construct *c, const InlineCtx *&saved)
    {
        saved = cur_inline;
        if (c->inline_ctx && c->inline_ctx != cur_inline) {
            o << "/* inlined " << c->inline_ctx->callee_name << " */ ";
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

    void expr_inner(const Construct *c)
    {
        if (auto *e = dynamic_cast<const LiteralInt *>(c)) {
            o << e->ival(); return;
        }
        if (auto *e = dynamic_cast<const LiteralFloat *>(c)) {
            o << fmt_float(e->fval()); return;
        }
        if (auto *e = dynamic_cast<const LiteralBool *>(c)) {
            o << (e->bval() ? "true" : "false"); return;
        }
        if (dynamic_cast<const LiteralNone *>(c)) {
            o << "none"; return;
        }
        if (auto *e = dynamic_cast<const LiteralStr *>(c)) {
            o << escape_string(e->strval().get<SharedStr>().get_view());
            return;
        }
        if (auto *e = dynamic_cast<const LiteralObj *>(c)) {
            /* a baked const array/dict/struct - its to_string is code-like */
            const EvalValue &v = e->literal_value();
            o << v.get_type()->to_string(v); return;
        }
        if (auto *e = dynamic_cast<const Identifier *>(c)) {
            o << e->get_str(); return;
        }
        if (auto *e = dynamic_cast<const LiteralArray *>(c)) {
            o << "[";
            for (size_t i = 0; i < e->elems.size(); i++) {
                if (i) o << ", ";
                expr(e->elems[i].get(), 0);
            }
            o << "]"; return;
        }
        if (auto *e = dynamic_cast<const LiteralDict *>(c)) {
            o << "{";
            for (size_t i = 0; i < e->elems.size(); i++) {
                if (i) o << ", ";
                expr(e->elems[i]->key.get(), 0);
                o << ": ";
                expr(e->elems[i]->value.get(), 0);
            }
            o << "}"; return;
        }
        if (auto *e = dynamic_cast<const MemberExpr *>(c)) {
            expr(e->what.get(), PREC_POSTFIX);
            o << (e->optional ? "?." : ".") << e->memId; return;
        }
        if (auto *e = dynamic_cast<const Subscript *>(c)) {
            expr(e->what.get(), PREC_POSTFIX);
            o << "[";
            expr(e->index.get(), 0);
            o << "]"; return;
        }
        if (auto *e = dynamic_cast<const Slice *>(c)) {
            expr(e->what.get(), PREC_POSTFIX);
            o << "[";
            if (e->start_idx) expr(e->start_idx.get(), 0);
            o << ":";
            if (e->end_idx) expr(e->end_idx.get(), 0);
            o << "]"; return;
        }
        if (auto *e = dynamic_cast<const CallExpr *>(c)) {
            expr(e->what.get(), PREC_POSTFIX);
            o << "(";
            if (e->args)
                for (size_t i = 0; i < e->args->elems.size(); i++) {
                    if (i) o << ", ";
                    expr(e->args->elems[i].get(), 0);
                }
            o << ")"; return;
        }
        if (auto *e = dynamic_cast<const IncDecExpr *>(c)) {
            const char *op = e->is_inc ? "++" : "--";
            if (e->is_prefix) { o << op; expr(e->lvalue.get(), 8); }
            else { expr(e->lvalue.get(), PREC_POSTFIX); o << op; }
            return;
        }
        if (auto *e = dynamic_cast<const TernaryExpr *>(c)) {
            expr(e->condExpr.get(), 2);
            o << " ? ";
            expr(e->thenExpr.get(), 2);
            o << " : ";
            expr(e->elseExpr.get(), 1);     /* right-assoc */
            return;
        }
        if (auto *e = dynamic_cast<const CoalesceExpr *>(c)) {
            expr(e->lhs.get(), 2);
            o << " ?? ";
            expr(e->rhs.get(), 1);          /* right-assoc */
            return;
        }
        if (auto *e = dynamic_cast<const TypedScalarExpr *>(c)) {
            op_chain(e->elems, expr_prec(c)); return;
        }
        if (auto *e = dynamic_cast<const Expr01 *>(c)) {
            if (e->elem) expr_inner(e->elem.get());
            return;
        }
        if (auto *e = dynamic_cast<const MultiOpConstruct *>(c)) {
            op_chain(e->elems, expr_prec(c)); return;
        }
        if (auto *e = dynamic_cast<const Expr14 *>(c)) {
            expr(e->lvalue.get(), 0);
            o << " " << OpString[(int)e->op] << " ";
            expr(e->rvalue.get(), 0); return;
        }
        /* fallback for an unhandled expression node */
        o << "/* " << c->name << " */";
    }

    /* ------------------------------ statements ------------------------ */

    /* The declared type for a `var`/`const` decl: an explicit annotation,
     * else best-effort from the rvalue's hints (typed-scalar / flat-array /
     * literal), else "var". */
    string decl_kw(const Identifier *lv, const Construct *rv, bool is_const)
    {
        const string prefix = is_const ? "const " : "";
        if (lv->decl_type != DeclType::none)        /* explicit annotation */
            return prefix + decl_type_kw(lv->decl_type);
        if (rv) {                                   /* best-effort inference */
            if (const char *at = arr_hint_type(rv->arr_hint))
                return prefix + at;
            if (auto *lo = dynamic_cast<const LiteralObj *>(rv))
                return prefix + reflect_typeof(lo->literal_value());
            if (rv->th == TypeHint::i) return prefix + "int";
            if (rv->th == TypeHint::f) return prefix + "float";
            if (dynamic_cast<const LiteralInt *>(rv))   return prefix + "int";
            if (dynamic_cast<const LiteralFloat *>(rv)) return prefix + "float";
            if (dynamic_cast<const LiteralBool *>(rv))  return prefix + "bool";
            if (dynamic_cast<const LiteralStr *>(rv))   return prefix + "str";
        }
        return is_const ? "const" : "var";
    }

    void stmt(const Construct *c, int level)
    {
        if (!c || dynamic_cast<const NopConstruct *>(c))
            return;

        const InlineCtx *saved;
        const bool tagged = enter_inline(c, saved);

        if (auto *b = dynamic_cast<const Block *>(c)) {
            block(b, level);
            if (tagged) leave_inline(saved);
            return;
        }

        o << ind(level);

        if (auto *f = dynamic_cast<const FuncDeclStmt *>(c)) {
            func(f, level);
            o << "\n";
        } else if (auto *s = dynamic_cast<const StructDeclStmt *>(c)) {
            o << "struct " << s->def->name->val << " { ... }\n";
        } else if (auto *i = dynamic_cast<const IfStmt *>(c)) {
            o << "if (";
            expr(i->condExpr.get(), 0);
            o << ") ";
            inline_block(i->thenBlock.get(), level);
            if (i->elseBlock) {
                o << " else ";
                inline_block(i->elseBlock.get(), level);
            }
            o << "\n";
        } else if (auto *w = dynamic_cast<const WhileStmt *>(c)) {
            o << "while (";
            expr(w->condExpr.get(), 0);
            o << ") ";
            inline_block(w->body.get(), level);
            o << "\n";
        } else if (auto *f = dynamic_cast<const ForStmt *>(c)) {
            o << "for (";
            if (f->init) stmt_oneline(f->init.get());
            o << "; ";
            if (f->cond) expr(f->cond.get(), 0);
            o << "; ";
            if (f->inc) stmt_oneline(f->inc.get());
            o << ") ";
            inline_block(f->body.get(), level);
            o << "\n";
        } else if (auto *fr = dynamic_cast<const ForRangeStmt *>(c)) {
            /* the specialized counted loop - shown as the equivalent for() with
             * a "counted" marker so the optimization is visible. */
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
        } else if (auto *fe = dynamic_cast<const ForeachStmt *>(c)) {
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
        } else if (auto *r = dynamic_cast<const ReturnStmt *>(c)) {
            o << "return";
            if (r->elem) { o << " "; expr(r->elem.get(), 0); }
            o << ";\n";
        } else if (auto *t = dynamic_cast<const TryCatchStmt *>(c)) {
            o << "try ";
            inline_block(t->tryBody.get(), level);
            for (const auto &cs : t->catchStmts) {
                o << " catch (";
                if (cs.first.exList)
                    for (size_t i = 0; i < cs.first.exList->elems.size(); i++) {
                        if (i) o << ", ";
                        o << cs.first.exList->elems[i]->get_str();
                    }
                if (cs.first.asId) o << " as " << cs.first.asId->get_str();
                o << ") ";
                inline_block(cs.second.get(), level);
            }
            if (t->finallyBody) {
                o << " finally ";
                inline_block(t->finallyBody.get(), level);
            }
            o << "\n";
        } else {
            /* an expression statement (call, assignment, ...) */
            stmt_oneline(c);
            o << ";\n";
        }

        if (tagged) leave_inline(saved);
    }

    /* a one-line statement body without trailing ';' or newline (for/decl) */
    void stmt_oneline(const Construct *c)
    {
        if (auto *e14 = dynamic_cast<const Expr14 *>(c)) {
            if (e14->fl & pFlags::pInDecl) {
                auto *lv = dynamic_cast<const Identifier *>(e14->lvalue.get());
                const bool is_const = e14->fl & pFlags::pInConstDecl;
                if (lv)
                    o << decl_kw(lv, e14->rvalue.get(), is_const) << " "
                      << lv->get_str();
                else
                    expr(e14->lvalue.get(), 0);
                o << " = ";
                expr(e14->rvalue.get(), 0);
                return;
            }
            expr(e14->lvalue.get(), 0);
            o << " " << OpString[(int)e14->op] << " ";
            expr(e14->rvalue.get(), 0);
            return;
        }
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
     * clone shows `func f$0(...)`), then its body. */
    void func(const FuncDeclStmt *f, int level)
    {
        if (f->explicit_pure || f->effective_pure)
            o << (f->explicit_pure ? "pure " : "/* pure */ ");
        if (!ret_type.empty())                       /* inferred return type */
            o << ret_type << " ";
        o << "func ";
        o << (f->id ? string(f->id->get_str()) : string("<lambda>"));
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

        if (auto *b = dynamic_cast<const Block *>(f->body.get())) {
            o << "{\n";
            for (const auto &e : b->elems)
                stmt(e.get(), level + 1);
            o << ind(level) << "}";
        } else if (f->body) {
            o << "=> ";
            expr(f->body.get(), 0);
            o << ";";
        } else {
            o << "{ }";
        }
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
    if (dynamic_cast<const Block *>(c) ||
        dynamic_cast<const IfStmt *>(c) ||
        dynamic_cast<const WhileStmt *>(c) ||
        dynamic_cast<const ForStmt *>(c) ||
        dynamic_cast<const ForRangeStmt *>(c) ||
        dynamic_cast<const ForeachStmt *>(c) ||
        dynamic_cast<const ReturnStmt *>(c) ||
        dynamic_cast<const FuncDeclStmt *>(c) ||
        dynamic_cast<const TryCatchStmt *>(c))
        r.stmt(c, 0);
    else
        r.expr(c, 0);
    return o.str();
}
