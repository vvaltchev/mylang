/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "eval.h"
#include "evaltypes.cpp.h"

/* #53 part B: the dict's map is reachable for MUTATION only through
 * DictObject's methods, each structural one calling will_restructure()
 * itself - so a new site cannot forget the cursor hook. The whole
 * guarantee is that no mutable map reference can be obtained. */
static_assert(std::is_same<
                  decltype(std::declval<DictObject &>().get_ref()),
                  const DictObject::inner_type &>::value,
              "DictObject::get_ref() must never hand out a mutable map");

class TypeDict : public TypeImpl<intrusive_ptr<DictObject>> {

public:

    TypeDict() : TypeImpl<intrusive_ptr<DictObject>>(Type::t_dict) { }

    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    size_t hash(const EvalValue &a) override;
    EvalValue subscript(const EvalValue &w, const EvalValue &i,
                        bool for_write = false) override;

    int_type len(const EvalValue &a) override;
    int_type use_count(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    string pretty(const EvalValue &a, int indent, int width) override;
    bool is_true(const EvalValue &a) override;
};

int_type TypeDict::len(const EvalValue &a)
{
    return a.get_ref<intrusive_ptr<DictObject>>().get()->get_ref().size();
}

/*
 * Two dicts are equal when they hold the same keys, each mapped to equal
 * values - what unordered_map's operator== computed, spelled out so the
 * walk runs on the cycle guard's pair stack (cyclewalk.h): a dict may hold
 * itself (`d["self"] = d`). A key is looked up by ITS OWN equality, with
 * fresh stacks (CycFreshScope) - the map's key_equal does not depend on
 * where the dict sits; a value is compared inside this walk, `b`'s value
 * on the left as the std operator== had it (CycFlip keeps the stack's
 * sides straight) - a builtin or an exception object has no `==`, so the
 * operand order is observable.
 */
static bool dict_equal(const EvalValue &a, const EvalValue &b)
{
    const DictObject::inner_type &dataA
        = a.get_ref<intrusive_ptr<DictObject>>()->get_ref();
    const DictObject::inner_type &dataB
        = b.get_ref<intrusive_ptr<DictObject>>()->get_ref();

    if (dataA.size() != dataB.size())
        return false;

    CycKey kl, kr;
    cyc_key(a, kl);
    cyc_key(b, kr);
    CycPairStack &st = cyc_eq();

    const int be = st.back_edge(kl, kr);
    if (be >= 0)
        return be == 1;

    CycPairGuard g(st, kl, kr);

    for (const auto &kv : dataA) {

        CycKey kk;
        DictObject::inner_type::const_iterator it;

        if (cyc_key(kv.first, kk)) {
            CycFreshScope fresh;
            it = dataB.find(kv.first);
        } else {
            it = dataB.find(kv.first);
        }

        if (it == dataB.end())
            return false;

        bool same;
        {
            CycFlip flip(st);
            same = it->second.get() == kv.second.get();
        }
        if (!same)
            return false;
    }

    return true;
}

void TypeDict::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<intrusive_ptr<DictObject>>()) {
        a = false;
        return;
    }

    const bool e = dict_equal(a, b);
    a = e;
}

void TypeDict::noteq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<intrusive_ptr<DictObject>>()) {
        a = true;
        return;
    }

    const bool e = dict_equal(a, b);
    a = !e;
}

/*
 * Deep, ORDER-INDEPENDENT hash of a dict. A dict is unordered, so two equal
 * dicts (same pairs, any insertion order) MUST hash equal. Each (k, v) pair is
 * hashed with the order-dependent combine (k before v), then the pair hashes
 * are accumulated commutatively (hash_unordered) so the dict's iteration order
 * does not matter.
 */
size_t TypeDict::hash(const EvalValue &a)
{
    const DictObject::inner_type &data
        = a.get_ref<intrusive_ptr<DictObject>>()->get_ref();
    size_t acc = hash_salt_dict;

    /* A dict may hold itself: met again inside its own hash, it hashes as a
     * back edge at its depth (cyclewalk.h). A KEY hashes by itself, with
     * fresh stacks, as dict equality matches it (dict_equal). */
    CycKey dk;
    cyc_key(a, dk);
    const int d = cyc_hash().depth(dk);
    if (d >= 0)
        return cyc_backedge_hash(d);
    CycGuard g(cyc_hash(), dk);

    for (const auto &[k, v] : data) {
        size_t pair = hash_salt_dict;
        CycKey kk;
        size_t kh;
        if (cyc_key(k, kk)) {
            CycFreshScope fresh;
            kh = k.hash();
        } else {
            kh = k.hash();
        }
        hash_combine(pair, kh);
        hash_combine(pair, v.get().hash());
        hash_unordered(acc, pair);
    }

    return acc;
}

EvalValue TypeDict::subscript(const EvalValue &what_lval, const EvalValue &key,
                              bool for_write)
{
    const EvalValue &what = RValue(what_lval);
    /* Borrow (get_ref), not copy: `what` is const, so get<>() would COPY the
     * intrusive_ptr (retain+release the DictObject) on EVERY dict index. A dict
     * doesn't COW - the mutations below (auto-vivify / freeze-insert) go through
     * the POINTEE, and operator->/get() on a const intrusive_ptr yield a
     * MUTABLE DictObject*, so the borrow is sound for read AND write. */
    const intrusive_ptr<DictObject> &flatObj =
        what.get_ref<intrusive_ptr<DictObject>>();
    DictObject &obj = *flatObj.get();
    const DictObject::mut_iterator it = obj.find_mut(key);
    const bool present = it != obj.mut_end();

    /*
     * A read-only (const) dict never hands out an assignable lvalue: present ->
     * the value, missing -> the default (default dict) or KeyNotFoundEx. On a
     * write target a const dict returns an rvalue so the write fails NotLValue.
     */
    if (flatObj->is_readonly()) {
        if (present)
            return it->second.get();
        if (obj.get_has_default())
            return obj.get_default();
        if (for_write)
            return none;
        throw KeyNotFoundEx();
    }

    /* Present key: hand out the lvalue (a read RValue()s it; a compound assign
     * reads-and-writes it). */
    if (present)
        return &it->second;

    /*
     * Missing key. A default dict inserts+returns its default (so `d[k]` is
     * non-opt and `d[k] += 1` works). A plain dict auto-vivifies only on a
     * plain-assignment target (`d[k] = v`); a read/compound throws (so `d[k]` /
     * `d.k` never yields none - it is a value or an exception). `get()` is the
     * explicit nullable lookup.
     */
    /*
     * Insert. The key is FROZEN (deep read-only via make_const_clone) before
     * being stored, so a later mutation of the original value cannot change the
     * stored key's value - which would change its hash and corrupt the map
     * (the entry would sit in the wrong bucket). A scalar/string key is
     * immutable already, so make_const_clone returns it as-is (cheap).
     */
    if (obj.get_has_default())            /* a new key: see Cursor */
        return obj.insert_new(make_const_clone(key, true),
                              LValue(obj.get_default(), false));

    if (for_write)
        return obj.insert_new(make_const_clone(key, true), LValue(none, false));

    throw KeyNotFoundEx();
}

string TypeDict::to_string(const EvalValue &a)
{
    const DictObject &obj = *a.get_ref<intrusive_ptr<DictObject>>().get();
    const DictObject::inner_type &data = obj.get_ref();

    /* already being printed further up this walk: `{...}` (cyclewalk.h) */
    const CycKey k = cyc_key_obj(&obj);
    if (cyc_render().contains(k))
        return "{...}";
    CycGuard g(cyc_render(), k);

    string res;
    size_type i = 0;

    res.reserve(48 * data.size());
    res += "{";

    for (const auto &p: data) {

        res += p.first.to_string_repr();
        res += ": ";
        res += p.second.get().to_string_repr();

        if (i != data.size() - 1)
            res += ", ";

        i++;
    }

    res += "}";
    return res;
}

string TypeDict::pretty(const EvalValue &a, int indent, int width)
{
    const DictObject &obj = *a.get_ref<intrusive_ptr<DictObject>>().get();
    const CycKey k = cyc_key_obj(&obj);
    if (cyc_render().contains(k))
        return "{...}";

    const string flat = to_string_repr(a);
    const DictObject::inner_type &data = obj.get_ref();

    if (data.empty() || indent + static_cast<int>(flat.size()) <= width)
        return flat;

    CycGuard g(cyc_render(), k);   /* a value leading back here: {...} */
    string res = "{\n";
    const string pad(indent + 2, ' ');
    size_type i = 0;
    for (const auto &p : data) {
        const string key = p.first.to_string_repr();   /* key: single line */
        res += pad;
        res += key;
        res += ": ";
        /* the value starts after `<pad><key>: `; pass that column so its own
         * fit check / expansion line up under the value, not the key. */
        const int val_col = indent + 2 + static_cast<int>(key.size()) + 2;
        res += p.second.get().pretty(val_col, width);
        if (i != data.size() - 1)
            res += ",";
        res += "\n";
        i++;
    }
    res += string(indent, ' ');
    res += "}";
    return res;
}

bool TypeDict::is_true(const EvalValue &a)
{
    return a.get_ref<intrusive_ptr<DictObject>>()->get_ref().size() > 0;
}

int_type TypeDict::use_count(const EvalValue &a)
{
    return a.get_ref<intrusive_ptr<DictObject>>().use_count();
}

EvalValue TypeDict::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(
        &a.get_ref<intrusive_ptr<DictObject>>().get()->get_ref()
    );
}

EvalValue TypeDict::clone(const EvalValue &a)
{
    const auto &wrapper = a.get_ref<intrusive_ptr<DictObject>>();
    const DictObject &dict = *wrapper.get();
    auto copy = make_intrusive<DictObject>(dict);

    /* A clone is an independent, mutable copy even of a read-only dict. */
    copy->clear_readonly();
    return intrusive_ptr<DictObject>(copy);
}
