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

/* the intern table's key: a node's own fields, its children by their
 * (interned) pointers */
struct AnnotKey {
    DeclType kind;
    bool opt;
    const StructTypeDef *strct;
    const TypeAnnot *elem, *key, *val;
    bool operator==(const AnnotKey &o) const
    {
        return std::tie(kind, opt, strct, elem, key, val)
               == std::tie(o.kind, o.opt, o.strct, o.elem, o.key, o.val);
    }
};

struct AnnotKeyHash {
    size_t operator()(const AnnotKey &k) const
    {
        size_t h = std::hash<int>()(static_cast<int>(k.kind) * 2 + k.opt);
        hash_combine(h, std::hash<const void *>()(k.strct));
        hash_combine(h, std::hash<const void *>()(k.elem));
        hash_combine(h, std::hash<const void *>()(k.key));
        hash_combine(h, std::hash<const void *>()(k.val));
        return h;
    }
};

/* program-lifetime, built on first use: a plain pointer, set on the
 * first call, needs no initialization guard (MyLang is single-threaded),
 * and a guard's branches are ones no test can take */
std::unordered_map<AnnotKey, std::unique_ptr<TypeAnnot>, AnnotKeyHash> &
annot_table()
{
    static std::unordered_map<AnnotKey, std::unique_ptr<TypeAnnot>,
                              AnnotKeyHash> *t = nullptr;
    if (!t)
        t = new std::unordered_map<AnnotKey, std::unique_ptr<TypeAnnot>,
                                   AnnotKeyHash>();
    return *t;
}

/* a non-owning handle on an interned node */
std::shared_ptr<TypeAnnot> annot_handle(const TypeAnnot *a)
{
    return std::shared_ptr<TypeAnnot>(std::shared_ptr<TypeAnnot>(),
                                      const_cast<TypeAnnot *>(a));
}

const TypeAnnot *annot_intern_key(const AnnotKey &k)
{
    auto &t = annot_table();
    auto it = t.find(k);
    if (it != t.end())
        return it->second.get();
    auto n = std::make_unique<TypeAnnot>();
    n->kind = k.kind;
    n->opt = k.opt;
    n->strct = k.strct;
    if (k.elem)
        n->elem = annot_handle(k.elem);
    if (k.key)
        n->key = annot_handle(k.key);
    if (k.val)
        n->val = annot_handle(k.val);
    const TypeAnnot *r = n.get();
    t.emplace(k, std::move(n));
    return r;
}

} // namespace

const TypeAnnot *annot_intern(const TypeAnnot &a)
{
    AnnotKey k{a.kind, a.opt, a.strct,
               a.elem ? annot_intern(*a.elem) : nullptr,
               a.key ? annot_intern(*a.key) : nullptr,
               a.val ? annot_intern(*a.val) : nullptr};
    return annot_intern_key(k);
}

const TypeAnnot *annot_nonopt(const TypeAnnot *a)
{
    if (!a->opt)
        return a;
    return annot_intern_key(AnnotKey{a->kind, false, a->strct,
                                     a->elem.get(), a->key.get(),
                                     a->val.get()});
}

/* (annot_table's note: a plain pointer, no initialization guard)
 * The DYNAMIC types - what a container made where `dyn` is declared
 * carries: they refuse nothing, so their components are `dyn?` (rule 3:
 * a `dyn` component is never none, and array<dyn> is a fixed type, rule
 * 2) */
const TypeAnnot *annot_dyn_array()
{
    static const TypeAnnot *a = nullptr;
    if (!a) {
        const TypeAnnot *d =
            annot_intern_key(AnnotKey{DeclType::dyn, true, nullptr,
                                      nullptr, nullptr, nullptr});
        a = annot_intern_key(AnnotKey{DeclType::arr, false, nullptr,
                                      d, nullptr, nullptr});
    }
    return a;
}

const TypeAnnot *annot_dyn_dict()
{
    static const TypeAnnot *a = nullptr;
    if (!a) {
        const TypeAnnot *d =
            annot_intern_key(AnnotKey{DeclType::dyn, true, nullptr,
                                      nullptr, nullptr, nullptr});
        a = annot_intern_key(AnnotKey{DeclType::dict, false, nullptr,
                                      nullptr, d, d});
    }
    return a;
}

const TypeAnnot *annot_array_of_dyn()
{
    static const TypeAnnot *a = nullptr;
    if (!a) {
        const TypeAnnot *d =
            annot_intern_key(AnnotKey{DeclType::dyn, false, nullptr,
                                      nullptr, nullptr, nullptr});
        a = annot_intern_key(AnnotKey{DeclType::arr, false, nullptr,
                                      d, nullptr, nullptr});
    }
    return a;
}

const TypeAnnot *annot_str_array()
{
    const TypeAnnot *e = annot_intern_key(AnnotKey{DeclType::s, false,
                                                   nullptr, nullptr,
                                                   nullptr, nullptr});
    return annot_intern_key(AnnotKey{DeclType::arr, false, nullptr, e,
                                     nullptr, nullptr});
}

const TypeAnnot *annot_array_of_struct(const StructTypeDef *d)
{
    const TypeAnnot *e = annot_intern_key(AnnotKey{DeclType::strct, false, d,
                                                   nullptr, nullptr,
                                                   nullptr});
    return annot_intern_key(AnnotKey{DeclType::arr, false, nullptr, e,
                                     nullptr, nullptr});
}

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

std::string fit_annot_text(const TypeAnnot &a);

/* a misfit at the current level: what is there */
bool fit_miss(const EvalValue &v, FitMisfit &why)
{
    why.where.clear();
    why.got = fit_kind_text(v);
    return false;
}

bool fits(const EvalValue &v, const TypeAnnot *a, FitWalk &w,
          FitMisfit &why);

/* a `dyn` component and an absent one (a generic `array`, a box of any
 * element) say the same: no type */
const TypeAnnot *ct_typed(const TypeAnnot *a)
{
    return a && a->kind != DeclType::dyn ? a : nullptr;
}

/* are two component types the same type? (a container's own type is
 * interned, a place's annotation need not be) */
bool ct_same(const TypeAnnot *c, const TypeAnnot *p)
{
    c = ct_typed(c);
    p = ct_typed(p);
    if (c == p)
        return true;
    if (!c || !p)
        return false;
    return c->kind == p->kind && c->opt == p->opt && c->strct == p->strct
           && ct_same(c->elem.get(), p->elem.get())
           && ct_same(c->key.get(), p->key.get())
           && ct_same(c->val.get(), p->val.get());
}

/*
 * Q2 increment 3 (plans/typed-containers.md, section 3, decision A): may
 * a container whose own type is `ct` enter a place annotated `a` (of the
 * same kind)? A component the place types `dyn` takes anything - a store
 * into it is checked against the container's own type (increment 2) - and
 * every other component must be the container's exactly, since a store
 * into it is checked against nothing: an array<int> holds no int? because
 * a place called it array<int?>, and an array<array<int>> does not enter
 * an array<array<dyn>>, whose elements take any array unchecked.
 */
bool ct_enters(const TypeAnnot *ct, const TypeAnnot *a)
{
    /* (rule 3: a `dyn` component is never none, so it takes no
     * component that may be - an array<dyn?> does not enter an
     * array<dyn>) */
    const auto comp = [](const TypeAnnot *c, const TypeAnnot *pl) {
        if (pl && pl->kind == DeclType::dyn && !pl->opt && c && c->opt)
            return false;
        return !ct_typed(pl) || ct_same(c, pl);
    };
    if (a->kind == DeclType::arr)
        return comp(ct->elem.get(), a->elem.get());
    return comp(ct->key.get(), a->key.get())
           && comp(ct->val.get(), a->val.get());
}

/* a typed container refused at a place: what it is, by its type */
bool ct_miss(const TypeAnnot *ct, FitMisfit &why)
{
    why.where.clear();
    why.got = fit_annot_text(*ct);
    return false;
}

/* does every element of `arr` fit `ea`? */
bool fits_elems(const SharedArrayObj &arr, const TypeAnnot *ea, FitWalk &w,
                FitMisfit &why)
{
    using St = SharedArrayObj::Storage;
    const size_type n = arr.size();
    if (!ea || n == 0)
        return true;
    /* a `dyn` element takes anything but none (rule 3): only a general
     * or a class array can hold one */
    if (ea->kind == DeclType::dyn
            && (ea->opt || (arr.skind() != St::general
                            && arr.skind() != St::objs)))
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
    /* a `dyn` key or value takes anything but none (rule 3) */
    const bool keys = ka && !(ka->kind == DeclType::dyn && ka->opt);
    const bool vals = va && !(va->kind == DeclType::dyn && va->opt);
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
    /* a `dyn` takes anything but none, unless it is a `dyn?` (rule 3) */
    if (v.is<NoneVal>()) {
        if (a->opt)
            return true;
        return fit_miss(v, why);
    }
    if (a->kind == DeclType::dyn)
        return true;
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
    case DeclType::arr: {
        if (!v.is<SharedArrayObj>())
            return fit_miss(v, why);
        /* a container that carries its own type enters by that type; an
         * untyped one (reached through `dyn` only, or a constant's) by
         * what it holds */
        const SharedArrayObj &arr = v.get_ref<SharedArrayObj>();
        const TypeAnnot *ct = arr.ctype();
        if (ct && !arr.is_readonly()) {
            if (ct_enters(ct, a))
                return true;
            return ct_miss(ct, why);
        }
        return fits_elems(arr, a->elem.get(), w, why);
    }
    case DeclType::dict: {
        if (!v.is<intrusive_ptr<DictObject>>())
            return fit_miss(v, why);
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        const TypeAnnot *ct = d.ctype();
        if (ct && !d.is_readonly()) {
            if (ct_enters(ct, a))
                return true;
            return ct_miss(ct, why);
        }
        return fits_dict(d, a->key.get(), a->val.get(), w, why);
    }
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

std::string annot_text(const TypeAnnot *a)
{
    if (!a)
        return "untyped";
    return fit_annot_text(*a);
}

std::string annot_type_text(const TypeAnnot *a)
{
    std::string s = fit_annot_text(*annot_nonopt(a));
    if (a->opt)
        s += '?';
    return s;
}

bool value_fits(const EvalValue &v, const TypeAnnot &a, FitMisfit &why)
{
    FitWalk w;
    return fits(v, &a, w, why);
}

/*
 * Q2 increment 3: an UNTYPED mutable container that fits a place takes
 * the place's type, along the annotation (stamp_ctype: a container under
 * a `dyn` component stays untyped, a typed one keeps its own - it fit, so
 * it is the same). From then on a store through a `dyn` name is checked
 * against it, so the place's type stays true however the container is
 * reached. A read-only one (a constant's) takes none: no store changes it.
 * Under -nti nothing takes a type (g_ctype_adopt).
 */
bool g_ctype_adopt = true;

/*
 * Before an untyped container takes a type: along the type, as the stamp
 * walks, a SLICE becomes an ordinary array (its whole group, as a write
 * through it would do) and an array other slices share gives them copies
 * of their windows - a slice is an array of its own (question 3), and the
 * type is kept on the storage a slice shares, so it would type them all,
 * or (a slice) none. Invisible to a program, like any materialization.
 * The value fits `t` (fit_adopt runs after the fit), so its kind is the
 * type's.
 */
void adopt_detach(const EvalValue &v, const TypeAnnot *t)
{
    if (!ctype_names(t))
        return;
    if (v.is<SharedArrayObj>()) {
        auto &a = const_cast<SharedArrayObj &>(v.get_ref<SharedArrayObj>());
        if (a.ctype() || a.is_readonly())
            return;
        a.materialize_slice();
        a.clone_all_slices();
        if (a.skind() != SharedArrayObj::Storage::general)
            return;
        const ArrayConstView &view = a.get_view();
        for (size_type i = 0; i < view.size(); i++)
            adopt_detach(view[i].get(), t->elem.get());
        return;
    }
    if (v.is<intrusive_ptr<DictObject>>()) {
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        if (d.ctype() || d.is_readonly())
            return;
        for (const auto &kv : d.get_ref())
            adopt_detach(kv.second.get(), t->val.get());
        if (d.get_has_default())
            adopt_detach(d.get_default(), t->val.get());
    }
}

void fit_adopt(const EvalValue &v, const TypeAnnot &a)
{
    if (!g_ctype_adopt || !ctype_names(&a))
        return;
    if (v.is<SharedArrayObj>()) {
        const SharedArrayObj &arr = v.get_ref<SharedArrayObj>();
        if (arr.ctype() || arr.is_readonly())
            return;
    } else if (v.is<intrusive_ptr<DictObject>>()) {
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        if (d.ctype() || d.is_readonly())
            return;
    } else {
        return;                 /* none, into an `opt` place */
    }
    const TypeAnnot *t = annot_nonopt(annot_intern(a));
    adopt_detach(v, t);
    stamp_ctype(v, t);
}

bool value_enters(const EvalValue &v, const TypeAnnot &a, FitMisfit &why)
{
    if (!value_fits(v, a, why))
        return false;
    fit_adopt(v, a);
    return true;
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
    if (!value_enters(v, *spec.annot, m))
        throw fit_error(spec, m);
}

void check_fit_loc(const EvalValue &v, const FitSpec &spec, Loc s, Loc e)
{
    FitMisfit m;
    if (value_enters(v, *spec.annot, m))
        return;
    TypeErrorEx ex = fit_error(spec, m);
    ex.loc_start = s;
    ex.loc_end = e;
    throw ex;
}

/*
 * Q2 increment 2 (plans/typed-containers.md): a container carries its
 * own type, so a store the compiler could not check against it - one
 * through `dyn` (Construct::ct_store, Instr::ct_checked,
 * ArgLocs::ct_store) - checks the value here, at the write, in every
 * engine. A new dict KEY is checked wherever it is inserted, however
 * the dict was reached (ct_fit_key).
 */
const TypeAnnot *ct_elem_annot(const EvalValue &c)
{
    const TypeAnnot *ct = nullptr;
    if (c.is<SharedArrayObj>()) {
        ct = c.get_ref<SharedArrayObj>().ctype();
        return ct ? ct->elem.get() : nullptr;
    }
    if (c.is<intrusive_ptr<DictObject>>()) {
        ct = c.get_ref<intrusive_ptr<DictObject>>()->ctype();
        return ct ? ct->val.get() : nullptr;
    }
    return nullptr;
}

namespace {

/* an int or a bool into a float place is a float, a bool into an int
 * place an int - the numeric widening a typed store makes
 * (coerce_to_decl_type, flat_store_core) */
EvalValue ct_widen(EvalValue v, const TypeAnnot *a)
{
    if (a->kind == DeclType::f) {
        if (v.is<int_type>())
            return EvalValue(static_cast<float_type>(v.get<int_type>()));
        if (v.is<bool>())
            return EvalValue(static_cast<float_type>(v.get<bool>() ? 1 : 0));
    } else if (a->kind == DeclType::i && v.is<bool>()) {
        return EvalValue(static_cast<int_type>(v.get<bool>() ? 1 : 0));
    }
    return v;
}

TypeErrorEx ct_misfit(const char *what, const char *subject,
                      const TypeAnnot &a, const FitMisfit &m)
{
    /* an element type is written as a type, `int?` (typestr's form),
     * not as a declaration's `opt int` */
    std::string s = what;
    s += " '" + fit_annot_text(*annot_nonopt(&a)) + (a.opt ? "?" : "")
         + "' but ";
    s += fit_subject_text(subject, m);
    return TypeErrorEx(intern_msg(s));
}

} // namespace

EvalValue ct_fit(const TypeAnnot *a, EvalValue v, bool dict, Loc s, Loc e)
{
    /* a `dyn` element takes anything but none (rule 3) */
    if (!a || (a->kind == DeclType::dyn && (a->opt || !v.is<NoneVal>())))
        return v;
    v = ct_widen(std::move(v), a);
    FitMisfit m;
    if (value_enters(v, *a, m))
        return v;
    TypeErrorEx ex = ct_misfit(dict ? "the dict's values are"
                                    : "the array's elements are",
                               "the value", *a, m);
    ex.loc_start = s;
    ex.loc_end = e;
    throw ex;
}

EvalValue ct_fit_value(const EvalValue &c, EvalValue v, Loc s, Loc e)
{
    return ct_fit(ct_elem_annot(c), std::move(v),
                  c.is<intrusive_ptr<DictObject>>(), s, e);
}

EvalValue ct_fit_key(const DictObject &d, EvalValue key)
{
    const TypeAnnot *ct = d.ctype();
    const TypeAnnot *a = ct ? ct->key.get() : nullptr;
    if (!a || (a->kind == DeclType::dyn && (a->opt || !key.is<NoneVal>())))
        return key;
    key = ct_widen(std::move(key), a);
    FitMisfit m;
    if (value_enters(key, *a, m))
        return key;
    throw ct_misfit("the dict's keys are", "the key", *a, m);
}
