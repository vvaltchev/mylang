/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. It's a C++ file in the form of a header,
 * just because it's faster to compile it this way. It is #included once, by
 * types.cpp, after types/fit.cpp.h (whose message helpers it uses).
 *
 * THE CAST (plans/cast.md): `cast<T>(x)` converts a value to EXACTLY the
 * type T - every engine calls cast_value, so they convert, and refuse, the
 * same values with the same messages.
 *
 *  - a scalar by the table (bool / int / float / str, both ways - int(),
 *    float() and str()'s own rules, and the words a string may say for a
 *    bool);
 *  - `T?` lets none through; `dyn?` keeps the value as it is, `dyn` too
 *    once it is not none;
 *  - a container is a NEW container of type T, in T's storage, each
 *    element (key, value, default) converted the same way; a nested
 *    container that already has its target type is shared, as a shallow
 *    copy shares it;
 *  - a struct, a class instance, a box must already be that type, and
 *    the top one is copied (a class instance: a new instance).
 *
 * THE WALK IS BOUNDED BY THE TYPE, NOT BY THE VALUE (fit.cpp.h's rule):
 * each level descends one level of T, a `dyn` element stops it, so a value
 * that contains itself cannot send it round a cycle. A container met again
 * at the same type (a DAG) is converted once and shared in the result.
 *
 * The result is complete before anyone sees it: a refusal drops what was
 * built, and the source is never written.
 *
 * T is INTERNED and COMPLETE (an array or a box names its element, a dict
 * its key and value, at every level): the parser refuses a bare `array` /
 * `dict` and interns the type, and the .myv loader refuses an incomplete
 * one - so a component is never null here, and an interned node's
 * components are interned too.
 */

namespace {

struct CastWalk {

    /* one step of the position a refusal names */
    struct Step {
        char kind;                  /* 'i' [i], 'v' [key], 'k' key, 'd' */
        size_type i;
        const EvalValue *key;
    };

    /* a container (cyc_copy_key / cyc_key_obj) and the type it becomes */
    typedef std::pair<CycKey, const TypeAnnot *> MemoKey;

    struct MemoHash {
        size_t operator()(const MemoKey &m) const
        {
            size_t h = CycKeyHash()(m.first);
            hash_combine(h, std::hash<const void *>()(m.second));
            return h;
        }
    };

    const TypeAnnot *top;           /* the cast's type, for the message */
    bool at_compile;                /* a constant evaluation (float(s)) */
    std::vector<Step> path;
    std::unordered_map<MemoKey, EvalValue, MemoHash> memo;
};

/* "cast<T>: [1] key "a" " - what every refusal starts with */
std::string cast_prefix(const CastWalk &w)
{
    std::string s = "cast<" + annot_type_text(w.top) + ">: ";
    for (const CastWalk::Step &st : w.path) {
        switch (st.kind) {
        case 'i':
            s += "[" + std::to_string(st.i) + "]";
            break;
        case 'v':
            s += "[" + st.key->to_string_repr() + "]";
            break;
        case 'k':
            s += "key " + st.key->to_string_repr();
            break;
        default:
            s += "default";
            break;
        }
    }
    if (!w.path.empty())
        s += ' ';
    return s;
}

/* how a refusal names the value: a scalar or a string by its value (a
 * long string cut), anything else by its kind */
std::string cast_subject(const EvalValue &v)
{
    if (v.is<int_type>() || v.is<float_type>() || v.is<bool>()
            || v.is<NoneVal>())
        return v.to_string_repr();
    if (v.is<SharedStr>()) {
        std::string r = v.to_string_repr();
        if (r.size() > 40)
            r = r.substr(0, 36) + "...\"";
        return r;
    }
    return fit_got_phrase(fit_kind_text(v));
}

/* the type a value must become, as a refusal names it: "an 'int'" */
std::string cast_target(const TypeAnnot *t)
{
    return fit_got_phrase(annot_type_text(t));
}

[[noreturn]] void cast_value_error(const CastWalk &w, const std::string &m)
{
    throw InvalidValueEx(intern_msg(cast_prefix(w) + m));
}

/* a value of the wrong KIND for `t` */
[[noreturn]] void cast_kind_error(const CastWalk &w, const EvalValue &v,
                                  const TypeAnnot *t)
{
    throw TypeErrorEx(intern_msg(cast_prefix(w) + cast_subject(v)
                                 + " is not " + cast_target(t)));
}

/* the words a string may say for a bool, case ignored: 1 true, 0
 * false, -1 none of them */
int cast_bool_word(std::string_view s)
{
    static const char *const yes[] = { "true", "yes", "on", "enabled" };
    static const char *const no[] = { "false", "no", "off", "disabled" };
    const auto eq = [&](const char *w) {
        const size_t n = std::strlen(w);
        if (s.size() != n)
            return false;
        for (size_t i = 0; i < n; i++)
            if (std::tolower(static_cast<unsigned char>(s[i])) != w[i])
                return false;
        return true;
    };
    for (const char *w : yes)
        if (eq(w))
            return 1;
    for (const char *w : no)
        if (eq(w))
            return 0;
    return -1;
}

EvalValue cast_to_bool(const EvalValue &v, const TypeAnnot *t,
                       CastWalk &w)
{
    if (v.is<bool>())
        return v;
    if (v.is<int_type>()) {
        const int_type i = v.get<int_type>();
        if (i != 0 && i != 1)
            cast_value_error(w, cast_subject(v)
                             + " is not a bool (only 0 and 1 are)");
        return EvalValue(i == 1);
    }
    if (v.is<float_type>()) {
        const float_type f = v.get<float_type>();
        if (f != 0.0 && f != 1.0)
            cast_value_error(w, cast_subject(v)
                             + " is not a bool (only 0.0 and 1.0 are)");
        return EvalValue(f == 1.0);
    }
    if (v.is<SharedStr>()) {
        const int b = cast_bool_word(v.get_ref<SharedStr>().get_view());
        if (b < 0)
            cast_value_error(w, cast_subject(v) + " is not a bool (true, "
                             "yes, on, enabled, false, no, off and "
                             "disabled are)");
        return EvalValue(b == 1);
    }
    cast_kind_error(w, v, t);
}

EvalValue cast_to_int(const EvalValue &v, const TypeAnnot *t, CastWalk &w)
{
    if (v.is<int_type>())
        return v;
    if (v.is<bool>())
        return EvalValue(static_cast<int_type>(v.get<bool>() ? 1 : 0));
    if (v.is<float_type>()) {
        /* int()'s rule: [-2^63, 2^63) is exactly the doubles whose
         * truncation fits; a NaN fails both comparisons */
        const float_type f = v.get<float_type>();
        const float_type lim = static_cast<float_type>(
            -static_cast<float_type>(std::numeric_limits<int_type>::min()));
        if (!(f >= -lim && f < lim))
            cast_value_error(w, cast_subject(v) + " has no int value (NaN, "
                             "infinite or out of range)");
        return EvalValue(static_cast<int_type>(f));
    }
    if (v.is<SharedStr>()) {
        int_type r = 0;
        const NumText o =
            numtext_int(v.get_ref<SharedStr>().get_view(), r);
        if (o == NumText::overflow)
            cast_value_error(w, cast_subject(v) + " does not fit an int");
        if (o != NumText::ok)
            cast_value_error(w, cast_subject(v) + " is not an int");
        return EvalValue(r);
    }
    cast_kind_error(w, v, t);
}

EvalValue cast_to_float(const EvalValue &v, const TypeAnnot *t,
                        CastWalk &w)
{
    if (v.is<float_type>())
        return v;
    if (v.is<int_type>())
        return EvalValue(static_cast<float_type>(v.get<int_type>()));
    if (v.is<bool>())
        return EvalValue(static_cast<float_type>(v.get<bool>() ? 1 : 0));
    if (v.is<SharedStr>()) {
        /* float()'s rule: out of the double range is refused at compile
         * time and rounds (to an infinity or a signed zero) at run time */
        float_type r = 0;
        const NumText o =
            numtext_float(v.get_ref<SharedStr>().get_view(), r);
        if (o == NumText::bad)
            cast_value_error(w, cast_subject(v) + " is not a float");
        if (o != NumText::ok && w.at_compile)
            cast_value_error(w, cast_subject(v)
                             + " is outside the float range");
        return EvalValue(r);
    }
    cast_kind_error(w, v, t);
}

EvalValue cast_to_str(const EvalValue &v, const TypeAnnot *t, CastWalk &w)
{
    if (v.is<SharedStr>())
        return v;
    if (v.is<int_type>() || v.is<float_type>() || v.is<bool>())
        return EvalValue(SharedStr(v.to_string()));
    cast_kind_error(w, v, t);
}

EvalValue cast_rec(const EvalValue &v, const TypeAnnot *t, CastWalk &w,
                   bool top);

/* an element, a key, a value, a default: below the top */
EvalValue cast_elem(const EvalValue &v, const TypeAnnot *t, CastWalk &w)
{
    return cast_rec(v, t, w, false);
}

/* The storage an array of element type `e` is built in (rule 4 of
 * strict-container-types.md): flat for a non-opt int / float / bool and a
 * POD struct, the class references for a class, else general. */
ArrHint cast_arr_hint(const TypeAnnot *e, const StructTypeDef *&sd)
{
    sd = nullptr;
    if (e->kind == DeclType::strct && e->strct->is_class)
        return ArrHint::flat_c;
    if (e->opt)
        return ArrHint::general;
    switch (e->kind) {
    case DeclType::i: return ArrHint::flat_i;
    case DeclType::f: return ArrHint::flat_f;
    case DeclType::b: return ArrHint::flat_b;
    case DeclType::strct:
        if (e->strct->is_pod()) {
            sd = e->strct;
            return ArrHint::flat_s;
        }
        return ArrHint::general;
    default:
        return ArrHint::general;
    }
}

EvalValue cast_array(const SharedArrayObj &arr, const TypeAnnot *t,
                     CastWalk &w)
{
    const TypeAnnot *e = t->elem.get();
    const size_type n = arr.size();
    std::vector<EvalValue> out;
    out.reserve(n);
    for (size_type i = 0; i < n; i++) {
        w.path.push_back(CastWalk::Step{'i', i, nullptr});
        out.push_back(cast_elem(arr_elem_at(arr, i), e, w));
        w.path.pop_back();
    }
    const StructTypeDef *sd = nullptr;
    const ArrHint h = cast_arr_hint(e, sd);
    EvalValue r = build_array_from_values(out.data(), n, h, sd, false);
    const_cast<SharedArrayObj &>(r.get_ref<SharedArrayObj>())
        .set_ctype(annot_nonopt(t));
    return r;
}

EvalValue cast_dict(const DictObject &d, const TypeAnnot *t, CastWalk &w)
{
    const TypeAnnot *kt = t->key.get();
    const TypeAnnot *vt = t->val.get();
    auto nd = make_intrusive<DictObject>();
    nd->set_ctype(annot_nonopt(t));
    /* (converted key, the key it came from), for a collision's message */
    std::vector<std::pair<EvalValue, const EvalValue *>> from;
    for (const auto &kv : d.get_ref()) {
        w.path.push_back(CastWalk::Step{'k', 0, &kv.first});
        EvalValue ck = make_const_clone(cast_elem(kv.first, kt, w), true);
        w.path.back().kind = 'v';
        EvalValue cv = cast_elem(kv.second.get(), vt, w);
        w.path.pop_back();
        if (nd->get_ref().find(ck) != nd->get_ref().end()) {
            const EvalValue *first = nullptr;
            for (const auto &p : from)
                if (p.first == ck)
                    first = p.second;
            /* the dict held the key `first` became, so it is recorded */
            ML_CHECK(first);
            cast_value_error(w, "keys " + first->to_string_repr() + " and "
                             + kv.first.to_string_repr() + " both become "
                             + ck.to_string_repr());
        }
        from.emplace_back(ck, &kv.first);
        nd->build_emplace(std::move(ck), LValue(std::move(cv), false));
    }
    if (d.get_has_default()) {
        w.path.push_back(CastWalk::Step{'d', 0, nullptr});
        nd->set_default(cast_elem(d.get_default(), vt, w));
        w.path.pop_back();
    }
    return EvalValue(intrusive_ptr<DictObject>(nd));
}

/*
 * `v` as exactly `t`. `top`: the cast's own value, which is always a new
 * container or a copy; below it a container or an instance that already
 * has its target type is shared.
 */
EvalValue cast_rec(const EvalValue &v, const TypeAnnot *t, CastWalk &w,
                   bool top)
{
    if (t->kind == DeclType::dyn) {
        if (!t->opt && v.is<NoneVal>())
            cast_kind_error(w, v, t);
        return v;
    }
    if (v.is<NoneVal>()) {
        if (t->opt)
            return v;
        cast_kind_error(w, v, t);
    }
    switch (t->kind) {
    case DeclType::i:
        return cast_to_int(v, t, w);
    case DeclType::f:
        return cast_to_float(v, t, w);
    case DeclType::s:
        return cast_to_str(v, t, w);
    case DeclType::strct: {
        /* a struct value of that struct, or an instance of that class (a
         * box<P> has P's def, and is not a P) */
        if (!v.is<intrusive_ptr<StructObject>>())
            cast_kind_error(w, v, t);
        const StructObject &o = *v.get_ref<intrusive_ptr<StructObject>>();
        if (o.boxed || o.def != t->strct)
            cast_kind_error(w, v, t);
        return top ? v.clone() : v;
    }
    case DeclType::box:
        if (!box_fits_annot(v, t))
            cast_kind_error(w, v, t);
        return top ? v.clone() : v;
    case DeclType::arr: {
        if (!v.is<SharedArrayObj>())
            cast_kind_error(w, v, t);
        const SharedArrayObj &arr = v.get_ref<SharedArrayObj>();
        const TypeAnnot *ct = annot_nonopt(t);
        if (!top && arr.ctype() == ct)
            return v;
        CastWalk::MemoKey mk{CycKey(), ct};
        const bool keyed = !top && cyc_copy_key(arr, mk.first);
        if (keyed) {
            auto it = w.memo.find(mk);
            if (it != w.memo.end())
                return it->second;
        }
        EvalValue r = cast_array(arr, t, w);
        if (keyed)
            w.memo.emplace(mk, r);
        return r;
    }
    case DeclType::dict: {
        if (!v.is<intrusive_ptr<DictObject>>())
            cast_kind_error(w, v, t);
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        const TypeAnnot *ct = annot_nonopt(t);
        if (!top && d.ctype() == ct)
            return v;
        CastWalk::MemoKey mk{cyc_key_obj(&d), ct};
        if (!top) {
            auto it = w.memo.find(mk);
            if (it != w.memo.end())
                return it->second;
        }
        EvalValue r = cast_dict(d, t, w);
        if (!top)
            w.memo.emplace(mk, r);
        return r;
    }
    default:            /* bool (dyn returned above; none is no type) */
        return cast_to_bool(v, t, w);
    }
}

} // namespace

EvalValue cast_value(const EvalValue &v, const TypeAnnot &t,
                     bool at_compile)
{
    CastWalk w;
    w.top = &t;
    w.at_compile = at_compile;
    return cast_rec(v, &t, w, true);
}

EvalValue cast_value_loc(const EvalValue &v, const TypeAnnot &t,
                         bool at_compile, Loc s, Loc e)
{
    try {
        return cast_value(v, t, at_compile);
    } catch (RuntimeException &ex) {  /* INT-COV-EXEMPT: its type test */
        ex.loc_start = s;
        ex.loc_end = e;
        throw;
    }
}
