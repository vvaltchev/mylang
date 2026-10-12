/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. It's a C++ file in the form of a header,
 * just because it's faster to compile it this way. It is #included once, by
 * types.cpp.
 *
 * #49: THE FIT CHECK - does a run-time value fit a place's type? A `dyn`
 * value (or a container whose element type is `dyn`) entering a parameter,
 * a declared variable, an element, a field or a dict is checked against
 * the place's type, given as a run-time annotation (TypeAnnot): every
 * engine calls value_fits, so they refuse the same values with the same
 * message.
 *
 * THE WALK IS BOUNDED BY THE ANNOTATION, NOT BY THE VALUE. Each level of
 * recursion descends one level of the annotation, which is finite, so a
 * value that contains itself cannot send the walk round a cycle (no cycle
 * guard is needed - cyclewalk.h's rule is about walks the VALUE drives).
 * A `dyn` (or absent) element type stops the walk, a struct is a def
 * identity check with no descent into its fields (its fields were checked
 * when they were stored), and a FLAT array is decided by its storage kind
 * alone. What remains is a general array's or a dict's elements, walked
 * once per (container, annotation) pair: a container met twice at the
 * same annotation (a DAG) is not walked again.
 */

namespace {

struct FitWalk {
    /* (container, annotation) keys already walked - the arrays walked
     * element by element and the dicts, the walks that cost one step per
     * element. An array is keyed by its WINDOW over its storage too, as
     * the cycle guard keys it: two slices of one storage hold different
     * elements, and the second must be walked. */
    struct Key {
        const void *id;
        size_type off, len;
        const TypeAnnot *a;
        bool operator==(const Key &o) const
        {
            return std::tie(id, off, len, a)
                   == std::tie(o.id, o.off, o.len, o.a);
        }
    };
    struct KeyHash {
        size_t operator()(const Key &k) const
        {
            size_t h = std::hash<const void *>()(k.id);
            hash_combine(h, std::hash<const void *>()(k.a));
            hash_combine(h, static_cast<size_t>(k.off));
            hash_combine(h, static_cast<size_t>(k.len));
            return h;
        }
    };
    std::unordered_set<Key, KeyHash> seen;

    bool visit(const void *id, size_type off, size_type len,
               const TypeAnnot *a)
    {
        return seen.insert(Key{id, off, len, a}).second;
    }
};

/* the run-time kind of `v` as a misfit message names it */
std::string fit_kind_text(const EvalValue &v)
{
    if (v.is<NoneVal>())
        return "none";
    if (v.is<intrusive_ptr<StructObject>>()) {
        /* a struct always has a def, and a def a name (the loader
         * refuses a nameless one) */
        const StructObject &o = *v.get_ref<intrusive_ptr<StructObject>>();
        if (o.boxed)
            return "box<" + o.def->name->val + ">";
        return o.def->name->val;
    }
    if (v.is<intrusive_ptr<BoxObj>>()) {
        /* a scalar box holds one of four kinds (box_make) */
        switch (v.get_ref<intrusive_ptr<BoxObj>>()->kind) {
        case 'i': return "box<int>";
        case 'f': return "box<float>";
        case 'b': return "box<bool>";
        default:  return "box<str>";
        }
    }
    return runtime_type_name(v);
}

/* a misfit at the current level: what is there */
bool fit_miss(const EvalValue &v, FitMisfit &why)
{
    why.where.clear();
    why.got = fit_kind_text(v);
    return false;
}

bool fits(const EvalValue &v, const TypeAnnot *a, FitWalk &w,
          FitMisfit &why);

/* does every element of `arr` fit `ea`? */
bool fits_elems(const SharedArrayObj &arr, const TypeAnnot *ea, FitWalk &w,
                FitMisfit &why)
{
    using St = SharedArrayObj::Storage;
    const size_type n = arr.size();
    if (!ea || ea->kind == DeclType::dyn || n == 0)
        return true;
    /* a FLAT array is decided by its storage kind: a misfit names what
     * that storage holds, at element 0 */
    const char *flat = nullptr;
    switch (arr.skind()) {
    case St::ints:
        flat = ea->kind == DeclType::i ? nullptr : "int";
        break;
    case St::floats:
        flat = ea->kind == DeclType::f ? nullptr : "float";
        break;
    case St::bools:
        flat = ea->kind == DeclType::b ? nullptr : "bool";
        break;
    case St::strs:
        flat = ea->kind == DeclType::s ? nullptr : "str";
        break;
    case St::structs: {
        const StructTypeDef *sd = arr.flat_structs().def;
        if (ea->kind != DeclType::strct || ea->strct != sd)
            flat = sd->name->val.c_str();
        break;
    }
    case St::objs:
    default:                            /* general */
        if (!w.visit(arr.storage_id(), arr.offset(), n, ea))
            return true;
        for (size_type i = 0; i < n; i++) {
            if (!fits(arr_elem_at(arr, i), ea, w, why)) {
                why.where = "[" + std::to_string(i) + "]" + why.where;
                return false;
            }
        }
        return true;
    }
    if (!flat)
        return true;
    why.where = "[0]";
    why.got = flat;
    return false;
}

/* "key K [0]" / "default [0]": a key or the default names itself, then
 * what is inside it */
void fit_prefix(FitMisfit &why, std::string head)
{
    if (!why.where.empty()) {
        head += ' ';
        head += why.where;
    }
    why.where = std::move(head);
}

bool fits_dict(const DictObject &d, const TypeAnnot *ka,
               const TypeAnnot *va, FitWalk &w, FitMisfit &why)
{
    /* a dict annotation has both a key and a value annotation, or
     * neither (a plain `dict`) */
    const bool keys = ka && ka->kind != DeclType::dyn;
    const bool vals = va && va->kind != DeclType::dyn;
    if (!keys && !vals)
        return true;
    /* one key per dict annotation: a key node always comes with the
     * same value node (a copied annotation shares both) */
    if (!w.visit(&d, 0, 0, ka))
        return true;
    for (const auto &kv : d.get_ref()) {
        if (keys && !fits(kv.first, ka, w, why)) {
            fit_prefix(why, "key " + kv.first.to_string_repr());
            return false;
        }
        if (vals && !fits(kv.second.get(), va, w, why)) {
            why.where = "[" + kv.first.to_string_repr() + "]" + why.where;
            return false;
        }
    }
    if (vals && d.get_has_default()
        && !fits(d.get_default(), va, w, why)) {
        fit_prefix(why, "default");
        return false;
    }
    return true;
}

/*
 * `a` is never null: a caller passes a whole annotation, and a container
 * annotation's elem / key / val are walked only when present (fits_elems,
 * fits_dict). Each kind tests its value and REPORTS a miss - never
 * `test || fit_miss(..)`: fit_miss returns false, so its true arm would
 * be a branch no input takes.
 */
bool fits(const EvalValue &v, const TypeAnnot *a, FitWalk &w,
          FitMisfit &why)
{
    if (a->kind == DeclType::dyn)
        return true;
    if (v.is<NoneVal>()) {
        if (a->opt)
            return true;
        return fit_miss(v, why);
    }
    bool ok;
    switch (a->kind) {
    case DeclType::i:
        ok = v.is<int_type>();
        break;
    case DeclType::f:
        ok = v.is<float_type>();
        break;
    case DeclType::b:
        ok = v.is<bool>();
        break;
    case DeclType::s:
        ok = v.is<SharedStr>();
        break;
    case DeclType::strct:
        /* the def identity of a struct VALUE (a box<P> has P's def); an
         * image's annotation with no def fits no struct */
        ok = v.is<intrusive_ptr<StructObject>>()
             && !v.get_ref<intrusive_ptr<StructObject>>()->boxed
             && v.get_ref<intrusive_ptr<StructObject>>()->def == a->strct;
        break;
    case DeclType::box:
        ok = box_fits_annot(v, a);
        break;
    case DeclType::arr:
        if (!v.is<SharedArrayObj>())
            return fit_miss(v, why);
        return fits_elems(v.get_ref<SharedArrayObj>(), a->elem.get(), w,
                          why);
    case DeclType::dict:
        if (!v.is<intrusive_ptr<DictObject>>())
            return fit_miss(v, why);
        return fits_dict(*v.get_ref<intrusive_ptr<DictObject>>(),
                         a->key.get(), a->val.get(), w, why);
    default:                /* `none`: an image's annotation, no check */
        return true;
    }
    if (ok)
        return true;
    return fit_miss(v, why);
}

/* "an 'int'" / "a 'str'" / "none" */
std::string fit_got_phrase(const std::string &got)
{
    if (got == "none")
        return "none";
    /* `got` is a kind's name, never empty; a struct's may be capitalized
     * ("an 'Egg'") */
    const bool an = std::strchr("aeiouAEIOU", got[0]) != nullptr;
    return std::string(an ? "an '" : "a '") + got + "'";
}

/* "the argument is an 'int'" / "the argument's [1] is a 'str'" */
std::string fit_subject_text(const char *subject, const FitMisfit &m)
{
    std::string s = subject;
    if (!m.where.empty()) {
        s += "'s ";
        s += m.where;
    }
    s += " is ";
    s += fit_got_phrase(m.got);
    return s;
}

std::string fit_annot_text(const TypeAnnot &a)
{
    return reflect_decl_annot(a.kind, a.strct, &a, a.opt);
}

} // namespace

bool value_fits(const EvalValue &v, const TypeAnnot &a, FitMisfit &why)
{
    FitWalk w;
    return fits(v, &a, w, why);
}

TypeErrorEx param_fit_error(const UniqueId *param, const TypeAnnot &a,
                            const FitMisfit &m, int site_arg)
{
    std::string s = "parameter '";
    s += param->val;
    s += "' is declared '" + fit_annot_text(a) + "' but ";
    s += fit_subject_text("the argument", m);
    TypeErrorEx ex(intern_msg(s));
    ex.bind_arg = site_arg;
    return ex;
}

TypeErrorEx fit_error(const FitSpec &spec, const FitMisfit &m)
{
    /* a var or field spec is named by the compile; an image's may not
     * be (the loader does not require it) */
    const char *nm = spec.name ? spec.name->val.c_str() : "?";
    std::string s;
    switch (spec.place) {
    case FitSpec::Place::var:
        s = "'";
        s += nm;
        s += "'";
        break;
    case FitSpec::Place::field:
        s = "field '";
        s += nm;
        s += "'";
        break;
    case FitSpec::Place::elem:     s = "the element";    break;
    case FitSpec::Place::dict_val: s = "the dict value"; break;
    default:                       s = "the dict key";   break;
    }
    s += " is declared '" + fit_annot_text(*spec.annot) + "' but ";
    s += fit_subject_text(spec.place == FitSpec::Place::dict_key
                              ? "the key" : "the value", m);
    return TypeErrorEx(intern_msg(s));
}

void check_fit(const EvalValue &v, const FitSpec &spec)
{
    FitMisfit m;
    if (!value_fits(v, *spec.annot, m))
        throw fit_error(spec, m);
}

void check_fit_loc(const EvalValue &v, const FitSpec &spec, Loc s, Loc e)
{
    FitMisfit m;
    if (value_fits(v, *spec.annot, m))
        return;
    TypeErrorEx ex = fit_error(spec, m);
    ex.loc_start = s;
    ex.loc_end = e;
    throw ex;
}
