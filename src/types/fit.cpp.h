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

/*
 * #80: an UNFILLED part of a container's own type - a part of its static
 * type nothing filled (question 9: `dict(0)`'s key), kind `none`. The
 * rest of the type is checked as any container's is; this part takes
 * whatever a store through `dyn` puts there, and is filled when the
 * container first enters a typed place (fits: what it holds is checked;
 * fit_adopt: it takes the place's type there, annot_merge). A container
 * whose own components are ALL unfilled carries no type (ctype_names).
 */
bool annot_unfilled(const TypeAnnot *t) noexcept
{
    return t && t->kind == DeclType::none;
}

bool annot_partial(const TypeAnnot *t) noexcept
{
    return t && (t->kind == DeclType::none || annot_partial(t->elem.get())
                 || annot_partial(t->key.get())
                 || annot_partial(t->val.get()));
}

/* `c` (a container's type) with each unfilled part taken from `p` (the
 * place it enters), and `p`'s unfilled parts left `c`'s: the place
 * passed ct_enters, so every part both fill is the same */
const TypeAnnot *annot_merge(const TypeAnnot *c, const TypeAnnot *p)
{
    if (!c || annot_unfilled(c))
        return p;
    if (!p || annot_unfilled(p))
        return c;
    return annot_intern_key(AnnotKey{
        c->kind, c->opt, c->strct, annot_merge(c->elem.get(), p->elem.get()),
        annot_merge(c->key.get(), p->key.get()),
        annot_merge(c->val.get(), p->val.get())});
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

/* rule 1 (plans/strict-container-types.md): are two component types
 * EXACTLY the same type - `dyn` a type of its own, the `?` included. An
 * absent component below the top (the elements of a generic `array`
 * written as a component: `array<array>`) is a `dyn`, as its static type
 * says (array<array<dyn>>); only a place's TOP-LEVEL generic `array` is a
 * hole (ct_enters) */
bool ct_exact(const TypeAnnot *c, const TypeAnnot *p)
{
    const auto is_dyn = [](const TypeAnnot *t) {
        return !t || (t->kind == DeclType::dyn && !t->opt);
    };
    if (c == p || (is_dyn(c) && is_dyn(p)))
        return true;
    /* #80: an unfilled part - of the container's type (what it holds is
     * checked instead: fits), or of a store's place inside such a type
     * (it takes what is put there) */
    if (annot_unfilled(c) || annot_unfilled(p))
        return true;
    /* (a ctype the compiler makes names every component - a cast needs
     * a complete type - so only an image's can lack one) */
    if (!c)                             /* INT-COV-EXEMPT: an image */
        return false;
    if (!p)
        return false;
    return c->kind == p->kind && c->opt == p->opt && c->strct == p->strct
           && ct_exact(c->elem.get(), p->elem.get())
           && ct_exact(c->key.get(), p->key.get())
           && ct_exact(c->val.get(), p->val.get());
}

/*
 * Q2 increment 3 (plans/typed-containers.md, section 3, decision A): may
 * a container whose own type is `ct` enter a place annotated `a` (of the
 * same kind)? Every component must be the container's exactly (rule 1,
 * plans/strict-container-types.md - a `dyn` component took anything
 * until 2026-11-20): an array<int> holds no int? because a place called
 * it array<int?>, nor a string because a place called it array<dyn>.
 */
bool ct_enters(const TypeAnnot *ct, const TypeAnnot *a)
{
    /* (a place's generic `array` / `dict` takes any: a hole, rule 5) */
    if (a->kind == DeclType::arr)
        return !a->elem || ct_exact(ct->elem.get(), a->elem.get());
    return (!a->key || ct_exact(ct->key.get(), a->key.get()))
           && (!a->val || ct_exact(ct->val.get(), a->val.get()));
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
    /* (an unfilled element, #80, takes anything) */
    if (!ea || n == 0 || annot_unfilled(ea))
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
    /* a `dyn` takes anything but none, unless it is a `dyn?` (rule 3);
     * an unfilled part (#80) takes anything */
    if (v.is<NoneVal>()) {
        if (a->opt || annot_unfilled(a))
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
            if (!ct_enters(ct, a))
                return ct_miss(ct, why);
            /* #80: a type with an unfilled part enters by the parts it
             * fills, and the rest by what it holds */
            if (!annot_partial(ct))
                return true;
        }
        return fits_elems(arr, a->elem.get(), w, why);
    }
    case DeclType::dict: {
        if (!v.is<intrusive_ptr<DictObject>>())
            return fit_miss(v, why);
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        const TypeAnnot *ct = d.ctype();
        if (ct && !d.is_readonly()) {
            if (!ct_enters(ct, a))
                return ct_miss(ct, why);
            if (!annot_partial(ct))     /* (#80, as an array's) */
                return true;
        }
        return fits_dict(d, a->key.get(), a->val.get(), w, why);
    }
    default:        /* `none`: unfilled (#80), or an image's: no check */
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
 * it is the same; one whose type has an unfilled part, #80, takes the
 * place's there - annot_merge). From then on a store through `dyn` is checked
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
/* #80: may a container whose type is `ct` still take one - it has none,
 * or a part of it is unfilled? */
static bool ct_open(const TypeAnnot *ct)
{
    return !ct || annot_partial(ct);
}

void adopt_detach(const EvalValue &v, const TypeAnnot *t)
{
    if (!ctype_names(t))
        return;
    if (v.is<SharedArrayObj>()) {
        auto &a = const_cast<SharedArrayObj &>(v.get_ref<SharedArrayObj>());
        if (!ct_open(a.ctype()) || a.is_readonly())
            return;
        a.materialize_slice();
        a.clone_all_slices();
        if (a.skind() != SharedArrayObj::Storage::general)
            return;
        /* rule 4: the storage follows the type it takes - an array of
         * non-none ints, floats or bools is flat (flatten_general) */
        const TypeAnnot *et = t->elem.get();
        if (et && !et->opt) {
            switch (et->kind) {
            case DeclType::i:
                a.flatten_general(SharedArrayObj::Storage::ints);
                return;
            case DeclType::f:
                a.flatten_general(SharedArrayObj::Storage::floats);
                return;
            case DeclType::b:
                a.flatten_general(SharedArrayObj::Storage::bools);
                return;
            default:
                break;
            }
        }
        const ArrayConstView &view = a.get_view();
        for (size_type i = 0; i < view.size(); i++)
            adopt_detach(view[i].get(), t->elem.get());
        return;
    }
    if (v.is<intrusive_ptr<DictObject>>()) {
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        if (!ct_open(d.ctype()) || d.is_readonly())
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
        if (!ct_open(arr.ctype()) || arr.is_readonly())
            return;
    } else if (v.is<intrusive_ptr<DictObject>>()) {
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        if (!ct_open(d.ctype()) || d.is_readonly())
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

/* The storage a parsed `array<...>` annotation gives an array - the
 * inferencer's array_repr_hint, read off the annotation (a typed const is
 * bound, and a pure function may run, before inference): dflt keeps a
 * value's own (array<str>, a generic `array`); `sd` the POD struct a
 * flat_s array needs */
ArrHint annot_arr_hint(const TypeAnnot *a, const StructTypeDef *&sd)
{
    sd = nullptr;
    const TypeAnnot *e = a->elem.get();   /* (every caller has one) */
    const StructTypeDef *esd =
        e && e->kind == DeclType::strct ? e->strct : nullptr;
    if (!e || (!e->opt && e->kind == DeclType::s))
        return ArrHint::dflt;
    if (!e->opt && e->kind == DeclType::i)
        return ArrHint::flat_i;
    if (!e->opt && e->kind == DeclType::f)
        return ArrHint::flat_f;
    if (!e->opt && e->kind == DeclType::b)
        return ArrHint::flat_b;
    if (!e->opt && esd && esd->is_pod()) {
        sd = esd;
        return ArrHint::flat_s;
    }
    if (esd && esd->is_class)
        return ArrHint::flat_c;
    return ArrHint::general;
}

/*
 * Choice 6 (plans/strict-container-types.md): the ZERO VALUE of the type
 * `a` - what `T x;` gives (the parser's zero_value_literal and
 * build_zero_struct_init, which build it as code) - into `out`, a FRESH
 * value (a container, a struct, a class instance, a box is new each
 * time). False when the type has none: a non-opt `dyn` (rule 3), a struct
 * one of whose fields has none, or a struct field whose struct type is
 * not known here (a forward reference). `out` null: only the answer.
 */
static bool struct_zero_value(const StructTypeDef *d, EvalValue *out,
                              int depth);

/* a scalar zero value into `out` (when the caller wants one) */
static bool zero_into(EvalValue *out, EvalValue v)
{
    if (out)
        *out = std::move(v);
    return true;
}

bool annot_zero_value(const TypeAnnot *a, EvalValue *out, int depth)
{
    /* (a non-opt cycle is refused at parse time; the bound is a guard) */
    if (depth > 64)                     /* INT-COV-EXEMPT: parse refuses it */
        return false;
    /* (every caller passes a component a type names) */
    ML_CHECK(a && a->kind != DeclType::none);
    if (a->opt)
        return zero_into(out, EvalValue());
    switch (a->kind) {
    case DeclType::b:
        return zero_into(out, EvalValue(false));
    case DeclType::i:
        return zero_into(out, EvalValue(static_cast<int_type>(0)));
    case DeclType::f:
        return zero_into(out, EvalValue(static_cast<float_type>(0.0)));
    case DeclType::s:
        return zero_into(out, EvalValue(SharedStr(std::string())));
    case DeclType::arr: {
        if (!out)
            return true;
        const StructTypeDef *sd = nullptr;
        const ArrHint h = annot_arr_hint(a, sd);
        EvalValue v = build_array_from_values(nullptr, 0, h, sd, false);
        /* (a type whose components are all unfilled names none, #80) */
        if (ctype_names(a))
            const_cast<SharedArrayObj &>(v.get_ref<SharedArrayObj>())
                .set_ctype(annot_nonopt(annot_intern(*a)));
        *out = std::move(v);
        return true;
    }
    case DeclType::dict: {
        if (!out)
            return true;
        auto d = make_intrusive<DictObject>();
        /* (a dict annotation has both a key and a value, or neither) */
        if (ctype_names(a))
            d->set_ctype(annot_nonopt(annot_intern(*a)));
        *out = EvalValue(std::move(d));
        return true;
    }
    case DeclType::strct:
        return a->strct && struct_zero_value(a->strct, out, depth + 1);
    case DeclType::box: {
        EvalValue inner;
        if (!annot_zero_value(a->elem.get(), out ? &inner : nullptr,
                              depth + 1))
            return false;
        if (out)
            *out = box_make(inner);
        return true;
    }
    default:                            /* a `dyn` has none */
        return false;
    }
}

static bool struct_zero_value(const StructTypeDef *d, EvalValue *out,
                              int depth)
{
    StructTypeDef *def = const_cast<StructTypeDef *>(d);
    intrusive_ptr<StructObject> obj;
    if (out)
        obj = make_intrusive<StructObject>(def);  /* POD: zeroed bytes */
    if (def->is_pod()) {
        if (out)
            *out = EvalValue(std::move(obj));
        return true;
    }
    if (out)
        obj->fields.reserve(def->fields.size());
    for (const FieldDef &fd : def->fields) {
        EvalValue v;
        bool ok = true;
        if (!fd.is_opt) {
            TypeAnnot k;
            switch (fd.kind) {
            case FieldKind::f_bool:  k.kind = DeclType::b; break;
            case FieldKind::f_int:   k.kind = DeclType::i; break;
            case FieldKind::f_float: k.kind = DeclType::f; break;
            case FieldKind::f_str:   k.kind = DeclType::s; break;
            case FieldKind::f_array: k.kind = DeclType::arr; break;
            case FieldKind::f_dict:  k.kind = DeclType::dict; break;
            case FieldKind::f_dyn:   k.kind = DeclType::dyn; break;
            /* (a box field always has its type in annot, below) */
            case FieldKind::f_box:   k.kind = DeclType::box; break;
            case FieldKind::f_struct:
                k.kind = DeclType::strct;
                k.strct = fd.struct_def;
                break;
            }
            /* a parameterized container or a box has its type in annot
             * (the parser sets one for those alone) */
            const TypeAnnot *fa = fd.annot ? fd.annot.get() : &k;
            ok = annot_zero_value(fa, out ? &v : nullptr, depth + 1);
        }
        if (!ok)
            return false;
        if (out)
            obj->fields.emplace_back(std::move(v), false);
    }
    if (out)
        *out = EvalValue(std::move(obj));
    return true;
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
