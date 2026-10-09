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
#include "structtype.h"

/*
 * The struct TYPE descriptor (t_structtype): a trivial value holding a raw
 * StructTypeDef*. It is a `const` value the `struct` decl binds in scope; it is
 * callable (construction, handled in CallExpr::do_eval) and supports `.CONST`
 * reads (handled in MemberExpr::do_eval). See plans/archived/structs.md.
 */
class TypeStructType : public Type {

public:

    TypeStructType() : Type(Type::t_structtype) { }

    bool is_true(const EvalValue &a) override { return true; }
    string to_string(const EvalValue &a) override;
    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
};

string TypeStructType::to_string(const EvalValue &a)
{
    return string(a.get<StructTypeDef *>()->name->val);
}

void TypeStructType::eq(EvalValue &a, const EvalValue &b)
{
    a = b.is<StructTypeDef *>() &&
        a.get<StructTypeDef *>() == b.get<StructTypeDef *>();
}

void TypeStructType::noteq(EvalValue &a, const EvalValue &b)
{
    a = !(b.is<StructTypeDef *>() &&
          a.get<StructTypeDef *>() == b.get<StructTypeDef *>());
}

/*
 * A struct INSTANCE (t_struct): a value (copied at the write, struct_own).
 * `==` is structural field-wise between same-type instances (different types
 * -> not equal), and `hash` agrees with it; both, and printing, stop at a
 * boxed instance already on the walk (cyclewalk.h).
 */
class TypeStruct : public TypeImpl<intrusive_ptr<StructObject>> {

public:

    TypeStruct() : TypeImpl<intrusive_ptr<StructObject>>(Type::t_struct) { }

    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    size_t hash(const EvalValue &a) override;
    bool is_true(const EvalValue &a) override { return true; }
    string to_string(const EvalValue &a) override;
    string pretty(const EvalValue &a, int indent, int width) override;
    EvalValue clone(const EvalValue &a) override;
    int_type use_count(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;
};

static bool struct_equal(const StructObject &x, const StructObject &y)
{
    if (x.def != y.def)
        return false;

    /* a CLASS instance - or a box - is equal to itself only: == is
     * identity */
    if (x.is_ref() || y.is_ref())
        return &x == &y;

    /* POD: same def -> same layout, so a raw byte compare is exact. */
    if (x.is_pod())
        return x.bytes == y.bytes;

    /* boxed: field-wise EvalValue equality (recurses for nested structs).
     * A struct VALUE takes no place on the cycle guard's pair stack: a
     * field leading back here passes through an array or a dict, which
     * does - and copy-on-write may share one object between this side
     * and a copy reached on the other, which read as a one-sided back
     * edge and made two equal copies unequal (cyclewalk.h, cyc_key). */
    for (size_t i = 0; i < x.fields.size(); i++)
        if (!(x.fields[i].get() == y.fields[i].get()))
            return false;

    return true;
}

void TypeStruct::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<intrusive_ptr<StructObject>>()) {
        a = false;
        return;
    }

    const bool e = struct_equal(*a.get<intrusive_ptr<StructObject>>().get(),
                                *b.get<intrusive_ptr<StructObject>>().get());
    a = e;
}

void TypeStruct::noteq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<intrusive_ptr<StructObject>>()) {
        a = true;
        return;
    }

    const bool e = struct_equal(*a.get<intrusive_ptr<StructObject>>().get(),
                                *b.get<intrusive_ptr<StructObject>>().get());
    a = !e;
}

/*
 * Deep hash of a struct: combine the field hashes in declaration order (a
 * struct is a fixed sequence of fields), salted with the struct type's NAME
 * so two struct types with equal field values usually hash differently -
 * eq() only compares instances of the SAME def, so a collision between two
 * types is a probe, never a wrong answer. NOT the def's address: a hash is
 * observable (`hash()`, a dict's iteration order), and an address differs
 * between runs, engines and a .myv load (it did until 2026-10-08).
 * Field-wise (via pod_get / fields[i]) keeps it consistent with eq for both
 * POD (a==b => equal field values => equal hash) and boxed instances, and
 * avoids hashing POD padding bytes.
 */
size_t TypeStruct::hash(const EvalValue &a)
{
    const StructObject &o = *a.get_ref<intrusive_ptr<StructObject>>().get();
    const StructTypeDef &def = *o.def;

    size_t seed = hash_salt_struct;
    hash_combine(seed, std::hash<std::string_view>()(def.name->val));

    /* a CLASS instance hashes by IDENTITY, consistent with its == - its
     * number, never its address (see g_class_ident) - so its hash does not
     * change when its fields do, and it can be a dict key unfrozen. Its
     * fields are never walked, so no cycle can pass through it here. */
    if (o.is_ref()) {
        hash_combine(seed, std::hash<uint64_t>()(o.ident));
        return seed;
    }

    /* a struct VALUE is never a back edge (cyclewalk.h, cyc_key): a cycle
     * through it is answered by the array or dict holding it */
    for (size_t i = 0; i < def.fields.size(); i++)
        hash_combine(seed, (o.is_pod() ? o.pod_get(static_cast<int>(i))
                                       : o.fields[i].get()).hash());

    return seed;
}

string TypeStruct::to_string(const EvalValue &a)
{
    const StructObject &o = *a.get_ref<intrusive_ptr<StructObject>>().get();
    const StructTypeDef &def = *o.def;

    /* a box<P> prints as `box(P(...))` */
    string res = o.boxed ? "box(" + string(def.name->val)
                         : string(def.name->val);

    /* a class instance or box<P> already being printed further up:
     * `Name(...)`, or `box(Name(...))` */
    CycKey k;
    const bool keyed = cyc_key(a, k);
    if (keyed && cyc_render().contains(k))
        return res + (o.boxed ? "(...))" : "(...)");
    CycGuard g(cyc_render(), k, keyed);

    res += "(";

    for (size_t i = 0; i < def.fields.size(); i++) {

        res += string(def.fields[i].name->val);
        res += ": ";
        res += (o.is_pod() ? o.pod_get(static_cast<int>(i))
                           : o.fields[i].get()).to_string_repr();

        if (i != def.fields.size() - 1)
            res += ", ";
    }

    res += o.boxed ? "))" : ")";
    return res;
}

string TypeStruct::pretty(const EvalValue &a, int indent, int width)
{
    const StructObject &o = *a.get_ref<intrusive_ptr<StructObject>>().get();
    const StructTypeDef &def = *o.def;

    CycKey k;
    const bool keyed = cyc_key(a, k);
    if (keyed && cyc_render().contains(k))
        return o.boxed ? "box(" + string(def.name->val) + "(...))"
                       : string(def.name->val) + "(...)";

    const string flat = to_string_repr(a);

    if (def.fields.empty() || indent + static_cast<int>(flat.size()) <= width)
        return flat;

    CycGuard g(cyc_render(), k, keyed);   /* a field leading back: Name(...) */
    string res = o.boxed ? "box(" + string(def.name->val)
                         : string(def.name->val);
    res += "(\n";
    const string pad(indent + 2, ' ');
    for (size_t i = 0; i < def.fields.size(); i++) {
        const string fname = string(def.fields[i].name->val);
        res += pad;
        res += fname;
        res += ": ";
        const EvalValue fv = o.is_pod() ? o.pod_get(static_cast<int>(i))
                                        : o.fields[i].get();
        const int val_col = indent + 2 + static_cast<int>(fname.size()) + 2;
        res += fv.pretty(val_col, width);
        if (i != def.fields.size() - 1)
            res += ",";
        res += "\n";
    }
    res += string(indent, ' ');
    res += o.boxed ? "))" : ")";
    return res;
}

EvalValue TypeStruct::clone(const EvalValue &a)
{
    const StructObject &o = *a.get<intrusive_ptr<StructObject>>().get();

    /* Shallow clone: a fresh, mutable top whose boxed slots are copied (a
     * non-trivial sub-object like an array is shared via its own COW). */
    auto copy = make_intrusive<StructObject>(o);
    copy->clear_readonly();
    return intrusive_ptr<StructObject>(copy);
}

int_type TypeStruct::use_count(const EvalValue &a)
{
    return a.get<intrusive_ptr<StructObject>>().use_count();
}

EvalValue TypeStruct::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(
        a.get<intrusive_ptr<StructObject>>().get());
}
