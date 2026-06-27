/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. It is a C++ file in the form of a header,
 * #included once into types.cpp (it relies on the type system and the other
 * builtins being already pulled in - in particular arr.cpp.h's arr_elem_at).
 *
 * Runtime reflection builtins: introspect the LIVE program state - the global
 * symbols, a value's structural type, a function's declared signature, a
 * struct's in-memory layout, and a function's specialization/template clones.
 * They are runtime (non-const) builtins, so scripts and the -rt suite use them
 * too, not only the REPL. The inherently compile-time views (a symbol's
 * INFERRED static type, the optimizer's reasoning) live in the REPL
 * meta-commands and the trace facility, not here.
 *
 * Honest limits (documented in README): a const SCALAR is folded away at
 * parse/inference time and is not a runtime symbol, so globals() does not list
 * it (the REPL's :globals adds it from the persistent const context); in a
 * non-REPL script, top-level VARS are frame slots rather than map entries, so
 * globals() there reports only the map-resident names (functions, structs,
 * spec/tmpl clones, captured globals).
 */

#pragma once

#include "defs.h"
#include "eval.h"
#include "evaltypes.cpp.h"
#include "syntax.h"
#include "structtype.h"
#include "trace.h"
#include "reflect.h"
#include "coderender.h"

#include <string>
#include <vector>
#include <algorithm>

/* ---- shared rendering helpers (reused by the :help/:globals commands) ---- */

/* A var/const/param annotation keyword, or null for a plain (inferred) decl. */
static const char *reflect_decl_type_kw(DeclType d)
{
    switch (d) {
        case DeclType::b:    return "bool";
        case DeclType::i:    return "int";
        case DeclType::f:    return "float";
        case DeclType::s:    return "str";
        case DeclType::arr:  return "array";
        case DeclType::dict: return "dict";
        default:             return nullptr;
    }
}

/* A struct field's type keyword (the struct name for an f_struct field). */
/* Render a parsed TypeAnnot (array<int>, dict<str, Point>, ...) as a string,
 * matching the `?`-suffix notation of sty_to_string / decltype. */
static std::string reflect_annot_str(const TypeAnnot *ta)
{
    if (!ta)
        return "?";
    std::string s;
    switch (ta->kind) {
        case DeclType::b:   s = "bool";  break;
        case DeclType::i:   s = "int";   break;
        case DeclType::f:   s = "float"; break;
        case DeclType::s:   s = "str";   break;
        case DeclType::dyn: s = "dyn";   break;
        case DeclType::strct:
            s = ta->strct ? std::string(ta->strct->name->val) : "struct";
            break;
        case DeclType::arr:
            s = "array<" + reflect_annot_str(ta->elem.get()) + ">";
            break;
        case DeclType::dict:
            s = "dict<" + reflect_annot_str(ta->key.get()) + "," +
                reflect_annot_str(ta->val.get()) + ">";
            break;
        default: s = "dyn";
    }
    if (ta->opt)
        s += "?";
    return s;
}

static std::string reflect_field_type(const FieldDef &f)
{
    /* a parameterized container (`array<int>`, `dict<str,P>`): the annot
     * carries the full element type (with its own `?` for nullability). */
    if (f.annot && (f.kind == FieldKind::f_array ||
                    f.kind == FieldKind::f_dict)) {
        std::string t = reflect_annot_str(f.annot.get());
        if (f.is_opt && (t.empty() || t.back() != '?'))
            t += "?";
        return t;
    }

    std::string s;
    if (f.is_opt)
        s += "opt ";
    switch (f.kind) {
        case FieldKind::f_bool:   s += "bool";  break;
        case FieldKind::f_int:    s += "int";   break;
        case FieldKind::f_float:  s += "float"; break;
        case FieldKind::f_str:    s += "str";   break;
        case FieldKind::f_array:  s += "array"; break;
        case FieldKind::f_dict:   s += "dict";  break;
        case FieldKind::f_dyn:    s += "dyn";   break;
        case FieldKind::f_struct:
            s += f.struct_ty ? std::string(f.struct_ty->val)
                             : std::string("struct");
            break;
    }
    return s;
}

/* One parameter's "[const] [opt] [TYPE|dyn] name" declared form. */
static std::string reflect_param_str(const Identifier *p)
{
    std::string s;
    if (p->const_param)
        s += "const ";
    if (p->opt_mod)
        s += "opt ";
    if (const char *t = reflect_decl_type_kw(p->decl_type)) {
        s += t;
        s += " ";
    } else if (p->dyn_mod) {
        s += "dyn ";
    }
    s += std::string(p->get_str());
    return s;
}

/* A function's declared signature, e.g. "pure func hypot(float a, float b)". */
std::string reflect_func_sig(const FuncDeclStmt *f)
{
    std::string s;
    if (f->explicit_pure)
        s += "pure ";
    s += "func ";
    s += !f->display_name.empty()
            ? f->display_name
            : (f->id ? std::string(f->id->get_str())
                     : std::string("<lambda>"));
    s += "(";
    if (f->params) {
        const auto &ps = f->params->elems;
        for (size_t i = 0; i < ps.size(); i++) {
            if (i)
                s += ", ";
            s += reflect_param_str(ps[i].get());
        }
    }
    s += ")";
    return s;
}

/* A struct type's constructor signature, e.g. "Point(int x, int y)". */
std::string reflect_struct_ctor(const StructTypeDef *def)
{
    std::string s(def->name ? std::string(def->name->val) : std::string("?"));
    s += "(";
    for (size_t i = 0; i < def->fields.size(); i++) {
        if (i)
            s += ", ";
        s += reflect_field_type(def->fields[i]);
        s += " ";
        s += std::string(def->fields[i].name->val);
    }
    s += ")";
    return s;
}

/* The structural type of an array value, from its storage kind (probing a
 * general array's elements for a homogeneous element type). */
static std::string reflect_array_type(const SharedArrayObj &a)
{
    switch (a.skind()) {
        case SharedArrayObj::Storage::ints:    return "array<int>";
        case SharedArrayObj::Storage::floats:  return "array<float>";
        case SharedArrayObj::Storage::bools:   return "array<bool>";
        case SharedArrayObj::Storage::structs:
            return std::string("array<") +
                   std::string(a.flat_structs().def->name->val) + ">";
        default:
            break;                                    /* general */
    }

    const size_type n = a.size();
    if (n == 0)
        return "array<dyn>";

    const EvalValue e0 = arr_elem_at(a, 0);
    const int t0 = e0.get_type()->t;
    for (size_type i = 1; i < n; i++)
        if (arr_elem_at(a, i).get_type()->t != t0)
            return "array<dyn>";                      /* heterogeneous */
    return std::string("array<") + reflect_typeof(e0) + ">";
}

/* The structural type of a dict value, probing one entry for key/value. */
static std::string reflect_dict_type(const DictObject &d)
{
    const DictObject::inner_type &m = d.get_ref();
    if (m.empty())
        return "dict";
    const auto &kv = *m.begin();
    return std::string("dict<") + reflect_typeof(kv.first) + "," +
           reflect_typeof(kv.second.get()) + ">";
}

std::string reflect_typeof(const EvalValue &e)
{
    switch (e.get_type()->t) {
        case Type::t_none:    return "none";
        case Type::t_int:     return "int";
        case Type::t_float:   return "float";
        case Type::t_bool:    return "bool";
        case Type::t_str:     return "str";
        case Type::t_ex:      return "exception";
        case Type::t_builtin: return "builtin";
        case Type::t_func:
            return reflect_func_sig(e.get<shared_ptr<FuncObject>>()->func);
        case Type::t_structtype:
            return std::string("type ") +
                   std::string(e.get<StructTypeDef *>()->name->val);
        case Type::t_struct:
            return std::string(
                e.get<intrusive_ptr<StructObject>>()->def->name->val);
        case Type::t_arr:
            return reflect_array_type(e.get<SharedArrayObj>());
        case Type::t_dict:
            return reflect_dict_type(*e.get<intrusive_ptr<DictObject>>());
        default:
            /* the remaining TypeE values (t_lval/t_undefid) are internal
             * pseudo-types never seen on an RValue. */
            return "?";
    }
}

/* A struct type's full in-memory layout (POD offsets/sizes or boxed slots). */
std::string reflect_layout(const StructTypeDef *def)
{
    std::string s(def->name ? std::string(def->name->val) : std::string("?"));
    if (def->is_pod()) {
        s += " - POD, size=" + std::to_string(def->size) +
             " bytes, align=" + std::to_string(def->align) + "\n";
        for (const auto &f : def->fields) {
            int sz = 0, al = 0;
            StructTypeDef::pod_field_metrics(f, sz, al);
            s += "  " + std::string(f.name->val) + ": " +
                 reflect_field_type(f) + " @" + std::to_string(f.offset) +
                 " (" + std::to_string(sz) + " bytes)\n";
        }
    } else {
        s += " - boxed, " + std::to_string(def->fields.size()) + " slot(s)\n";
        for (const auto &f : def->fields)
            s += "  " + std::string(f.name->val) + ": " +
                 reflect_field_type(f) + " [slot " +
                 std::to_string(f.slot) + "]\n";
    }
    for (const auto &c : def->consts)
        s += "  const " + std::string(c.first->val) + " = " +
             c.second.to_string() + "\n";
    return s;
}

/* ------------------------------- builtins -------------------------------- */

/*
 * globals(): the names bound in the global scope, sorted, as an array<str>.
 * Excludes builtins (use :help builtins). See the file header for the const
 * scalar / script-slot limits.
 */
EvalValue builtin_globals(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    EvalContext *root = get_root_ctx(ctx);
    std::vector<std::pair<const UniqueId *, const LValue *>> syms;
    root->collect_symbols(syms);

    /* The root context copies every builtin into its symbol map; exclude them
     * (they are the province of :help builtins, not the user's globals). */
    std::vector<std::string> names;
    names.reserve(syms.size());
    for (const auto &kv : syms) {
        if (EvalContext::const_builtins.count(kv.first) ||
            EvalContext::builtins.count(kv.first))
            continue;
        names.push_back(std::string(kv.first->val));
    }

    /* Top-level functions are GLOBAL-table slots in a script (not map entries),
     * so add the defined ones from the table - keeping globals() listing
     * functions as it did before they were slotted. (In the REPL functions
     * stay in the map, so the table is empty and this is a no-op.) */
    if (root->gfuncs) {
        const GlobalFuncTable &gt = *root->gfuncs;
        for (size_t i = 0; i < gt.names.size(); i++)
            if (gt.defined[i] && gt.names[i])
                names.push_back(std::string(gt.names[i]->val));
    }

    std::sort(names.begin(), names.end());

    SharedArrayObj::vec_type vec;
    vec.reserve(names.size());
    for (std::string &n : names)
        vec.emplace_back(SharedStr(move(n)), false);
    return SharedArrayObj(move(vec));
}

/*
 * typeof(x): the runtime structural type of `x` as a string - richer than
 * type(x) (which gives the bare kind "array"): "array<int>", "dict<int,str>",
 * a struct name, a function's signature, etc.
 */
EvalValue builtin_typeof(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    const EvalValue &e = RValue(exprList->elems[0]->eval(ctx));
    return SharedStr(reflect_typeof(e));
}

/*
 * signature(f): the declared signature of a function (or a struct type's
 * constructor), as a string. Accepts a function value, a struct type
 * descriptor, or a struct instance (its type's constructor).
 */
EvalValue builtin_signature(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (e.is<shared_ptr<FuncObject>>())
        return SharedStr(
            reflect_func_sig(e.get<shared_ptr<FuncObject>>()->func));
    if (e.is<StructTypeDef *>())
        return SharedStr(reflect_struct_ctor(e.get<StructTypeDef *>()));
    if (e.is<intrusive_ptr<StructObject>>())
        return SharedStr(
            reflect_struct_ctor(e.get<intrusive_ptr<StructObject>>()->def));

    throw TypeErrorEx("Expected a function or struct type", arg->start,
                      arg->end);
}

/*
 * Build a StructLayout reflection value (a native composite type, see
 * plans/reflection.md) describing `qdef`: name, size, align, pod, and an
 * array<StructField> with each field's name/type/offset/size/align (the last
 * three are -1 for a boxed field, which has no byte layout).
 */
static EvalValue reflect_make_layout(const StructTypeDef *qdef)
{
    StructTypeDef *sf = const_cast<StructTypeDef *>(native_struct_field_def());
    StructTypeDef *sl = const_cast<StructTypeDef *>(native_struct_layout_def());

    SharedArrayObj::vec_type fieldvec;
    for (const FieldDef &f : qdef->fields) {
        int sz = 0, al = 0;
        const bool pod = StructTypeDef::pod_field_metrics(f, sz, al);
        auto fo = make_intrusive<StructObject>(sf);
        fo->fields.emplace_back(
            EvalValue(SharedStr(std::string(f.name->val))), false);
        fo->fields.emplace_back(
            EvalValue(SharedStr(reflect_field_type(f))), false);
        fo->fields.emplace_back(
            EvalValue(static_cast<int_type>(qdef->is_pod() ? f.offset : -1)),
            false);
        fo->fields.emplace_back(
            EvalValue(static_cast<int_type>(pod ? sz : -1)), false);
        fo->fields.emplace_back(
            EvalValue(static_cast<int_type>(pod ? al : -1)), false);
        fieldvec.emplace_back(
            EvalValue(intrusive_ptr<StructObject>(fo)), false);
    }

    auto lo = make_intrusive<StructObject>(sl);
    lo->fields.emplace_back(
        EvalValue(SharedStr(std::string(qdef->name->val))), false);
    lo->fields.emplace_back(
        EvalValue(static_cast<int_type>(qdef->size)), false);
    lo->fields.emplace_back(EvalValue(static_cast<int_type>(qdef->align)),
                            false);
    lo->fields.emplace_back(EvalValue(qdef->is_pod()), false);
    lo->fields.emplace_back(EvalValue(SharedArrayObj(move(fieldvec))), false);
    return EvalValue(intrusive_ptr<StructObject>(lo));
}

/*
 * layout(S): a struct's in-memory layout as a structured `StructLayout` value -
 * `.name`, `.size`, `.align`, `.pod`, and `.fields` (an array<StructField> of
 * {name, type, offset, size, align}). Accepts a struct type descriptor or a
 * struct instance. (The legacy `reflect_layout` string renderer is kept for the
 * REPL `:layout`-style introspection.)
 */
EvalValue builtin_layout(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (e.is<StructTypeDef *>())
        return reflect_make_layout(e.get<StructTypeDef *>());
    if (e.is<intrusive_ptr<StructObject>>())
        return reflect_make_layout(e.get<intrusive_ptr<StructObject>>()->def);

    throw TypeErrorEx("Expected a struct type or instance", arg->start,
                      arg->end);
}

/*
 * specializations(f): the synthetic global names ($specN / $tmplN) of every
 * specialization / template-instantiation clone derived from function `f`, as
 * an array<str> (empty when none). The clones are real globals (a clone is
 * inserted at the root block and binds its synthetic name), so this is purely
 * a runtime scope walk; what each clone specializes on is shown by the trace /
 * :globals.
 */
EvalValue builtin_specializations(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (!e.is<shared_ptr<FuncObject>>())
        throw TypeErrorEx("Expected a function", arg->start, arg->end);

    const FuncDeclStmt *f = e.get<shared_ptr<FuncObject>>()->func;
    const std::string name =
        !f->display_name.empty()
            ? f->display_name
            : (f->id ? std::string(f->id->get_str()) : std::string());

    std::vector<std::pair<const UniqueId *, const LValue *>> syms;
    get_root_ctx(ctx)->collect_symbols(syms);

    std::vector<std::string> out;
    for (const auto &kv : syms) {
        const EvalValue &v = kv.second->get();
        if (!v.is<shared_ptr<FuncObject>>())
            continue;
        const FuncDeclStmt *g = v.get<shared_ptr<FuncObject>>()->func;
        if (g == f || g->display_name != name)
            continue;
        /* a clone has a synthetic id ($specN/$tmplN) + the original name in
         * display_name; the original itself has an empty display_name so it is
         * skipped by the display_name test above. */
        out.push_back(std::string(g->id->get_str()));
    }
    std::sort(out.begin(), out.end());

    SharedArrayObj::vec_type vec;
    vec.reserve(out.size());
    for (std::string &n : out)
        vec.emplace_back(SharedStr(move(n)), false);
    return SharedArrayObj(move(vec));
}

/*
 * show(x): render the FINAL optimized AST of `x` back into synthetic MyLang
 * code (folded consts as literals, inlined call bodies spliced in, dead code
 * gone, flat-array element types shown). If `x` is a function value its whole
 * declaration is rendered; otherwise `x` is treated as an EXPRESSION and its
 * (already-optimized) argument tree is rendered - so show(2 + 3 * 4) -> "14"
 * and show(f(1, 2)) shows how that call optimized. Returns the code as a
 * string. See coderender.{h,cpp} and the richer REPL :show command.
 */
EvalValue builtin_show(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();

    /* A bare identifier bound to a function -> render the function's body.
     * (An Identifier eval is a side-effect-free scope lookup.) */
    if (dynamic_cast<Identifier *>(arg)) {
        const EvalValue lv = arg->eval(ctx);
        if (lv.is<LValue *>()) {
            const EvalValue &v = lv.get<LValue *>()->get();
            if (v.is<shared_ptr<FuncObject>>())
                return SharedStr(
                    render_func_code(v.get<shared_ptr<FuncObject>>()->func));
        }
    }

    /* Otherwise render the (optimized) expression AST itself - no evaluation. */
    return SharedStr(render_construct_code(arg));
}

/* ------------------------ diagnostic tracing ----------------------------- */

/*
 * trace(category, on): enable/disable a diagnostic trace category (see
 * trace.h). category is a string ("infer", "inline", "specialize", "template",
 * "autoconst", "autopure", "arrays", "fold", or "all"); on is truthy/falsy.
 * Throws InvalidValueEx on an unknown category. Returns none.
 */
EvalValue builtin_trace(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 2)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *a0 = exprList->elems[0].get();
    const EvalValue &name = RValue(a0->eval(ctx));
    const EvalValue &on = RValue(exprList->elems[1]->eval(ctx));

    if (!name.is<SharedStr>())
        throw TypeErrorEx("Expected a category string", a0->start, a0->end);

    const string cat(name.get<SharedStr>().get_view());
    if (!trace_set(cat, on.get_type()->is_true(on)))
        throw InvalidValueEx("Unknown trace category", a0->start, a0->end);
    return none;
}

/* traceoff(): disable all trace categories. Returns none. */
EvalValue builtin_traceoff(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    trace_clear_all();
    return none;
}

/* tracing(): the active trace categories as a sorted array<str>. */
EvalValue builtin_tracing(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 0)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    std::vector<std::string> active = trace_active();
    SharedArrayObj::vec_type vec;
    vec.reserve(active.size());
    for (std::string &n : active)
        vec.emplace_back(SharedStr(move(n)), false);
    return SharedArrayObj(move(vec));
}
