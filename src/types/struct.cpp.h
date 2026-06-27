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
 * reads (handled in MemberExpr::do_eval). See plans/structs.md.
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
 * A struct INSTANCE (t_struct). COW value semantics like arrays/dicts. `==` is
 * structural field-wise between same-type instances (different types -> not
 * equal); not hashable (v1).
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

    /* POD: same def -> same layout, so a raw byte compare is exact. */
    if (x.is_pod())
        return x.bytes == y.bytes;

    /* boxed: field-wise EvalValue equality (recurses for nested structs) */
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
 * struct is a fixed sequence of fields), salted with the struct's def identity
 * so two different struct types with equal field values hash differently -
 * matching eq(), which only compares instances of the SAME def. Field-wise
 * (via pod_get / fields[i]) keeps it consistent with eq for both POD (a==b =>
 * equal field values => equal hash) and boxed instances, and avoids hashing
 * POD padding bytes.
 */
size_t TypeStruct::hash(const EvalValue &a)
{
    const StructObject &o = *a.get<intrusive_ptr<StructObject>>().get();
    const StructTypeDef &def = *o.def;

    size_t seed = hash_salt_struct;
    hash_combine(seed, std::hash<const void *>()(o.def));

    for (size_t i = 0; i < def.fields.size(); i++)
        hash_combine(seed, (o.is_pod() ? o.pod_get(static_cast<int>(i))
                                       : o.fields[i].get()).hash());

    return seed;
}

string TypeStruct::to_string(const EvalValue &a)
{
    const StructObject &o = *a.get<intrusive_ptr<StructObject>>().get();
    const StructTypeDef &def = *o.def;

    string res = string(def.name->val);
    res += "(";

    for (size_t i = 0; i < def.fields.size(); i++) {

        res += string(def.fields[i].name->val);
        res += ": ";
        res += (o.is_pod() ? o.pod_get(static_cast<int>(i))
                           : o.fields[i].get()).to_string_repr();

        if (i != def.fields.size() - 1)
            res += ", ";
    }

    res += ")";
    return res;
}

string TypeStruct::pretty(const EvalValue &a, int indent, int width)
{
    const string flat = to_string_repr(a);
    const StructObject &o = *a.get<intrusive_ptr<StructObject>>().get();
    const StructTypeDef &def = *o.def;

    if (def.fields.empty() || indent + static_cast<int>(flat.size()) <= width)
        return flat;

    string res = string(def.name->val);
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
    res += ")";
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
