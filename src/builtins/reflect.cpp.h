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
static std::string reflect_field_type(const FieldDef &f)
{
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

    std::vector<std::pair<const UniqueId *, const LValue *>> syms;
    get_root_ctx(ctx)->collect_symbols(syms);

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
 * layout(S): a struct's in-memory layout (POD field offsets/sizes + total
 * size/alignment, or the boxed slot list), as a multi-line string. Accepts a
 * struct type descriptor or a struct instance.
 */
EvalValue builtin_layout(EvalContext *ctx, ExprList *exprList)
{
    if (exprList->elems.size() != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);

    Construct *arg = exprList->elems[0].get();
    const EvalValue &e = RValue(arg->eval(ctx));

    if (e.is<StructTypeDef *>())
        return SharedStr(reflect_layout(e.get<StructTypeDef *>()));
    if (e.is<intrusive_ptr<StructObject>>())
        return SharedStr(
            reflect_layout(e.get<intrusive_ptr<StructObject>>()->def));

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
