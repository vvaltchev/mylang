/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "uniqueid.h"
#include <vector>
#include <utility>
#include <cstring>

/*
 * Custom struct types (see plans/structs.md). A `struct` decl builds one
 * StructTypeDef (AST-owned, program-lifetime); the type descriptor value
 * (t_structtype) holds a raw pointer to it, exactly like a FuncObject holds a
 * FuncDeclStmt*. A struct *instance* (t_struct) is a StructObject.
 *
 * v1: every field has an explicit type (no `var`); `opt` is allowed only on the
 * reference kinds (dyn/array/dict/struct). Storage is `boxed` (a slot array of
 * LValues) for now; the POD C-layout + flat-array path is added in later phases.
 */

/* The static kind of a field, derived from its explicit type annotation. */
enum class FieldKind : unsigned char {
    f_bool, f_int, f_float, f_str, f_array, f_dict, f_dyn, f_struct
};

struct FieldDef {
    const UniqueId *name = nullptr;
    FieldKind kind = FieldKind::f_dyn;
    const UniqueId *struct_ty = nullptr;  /* when kind == f_struct */
    bool is_opt = false;
    int slot = -1;        /* boxed: index into StructObject::fields */
    int offset = -1;      /* POD: byte offset into the bytes buffer; else -1 */
};

/*
 * POD field metrics: our OWN fixed alignment rules so a struct's C-style layout
 * depends only on the word size (sizeof(int_type)), never on host `alignof`.
 * This is the one place an arch assumption lives (see plans/structs.md §6); on
 * the LE x86/arm/riscv targets only the int width (4 vs 8) varies. -1 marks a
 * non-POD-scalar kind (a struct is then boxed).
 */
inline int pod_field_size(FieldKind k)
{
    switch (k) {
        case FieldKind::f_bool:  return 1;
        case FieldKind::f_int:   return static_cast<int>(sizeof(int_type));
        case FieldKind::f_float: return static_cast<int>(sizeof(float_type));
        default:                 return -1;
    }
}

inline int pod_field_align(FieldKind k) { return pod_field_size(k); }

struct StructTypeDef {

    enum class Layout : unsigned char { boxed, pod };

    const UniqueId *name = nullptr;
    std::vector<FieldDef> fields;                      /* declaration order */
    std::vector<std::pair<const UniqueId *, EvalValue>> consts;
    Layout layout = Layout::boxed;
    int size = 0;         /* POD: total struct size in bytes (later phases) */
    int align = 1;        /* POD: struct alignment */

    int slot_of(const UniqueId *n) const {
        for (const auto &f : fields)
            if (f.name == n)
                return f.slot;
        return -1;
    }

    const FieldDef *field_of(const UniqueId *n) const {
        for (const auto &f : fields)
            if (f.name == n)
                return &f;
        return nullptr;
    }

    const EvalValue *const_of(const UniqueId *n) const {
        for (const auto &c : consts)
            if (c.first == n)
                return &c.second;
        return nullptr;
    }

    bool is_pod() const { return layout == Layout::pod; }

    /*
     * Decide the storage and, for POD, assign each field a C-style byte offset.
     * POD iff there is at least one field and every field is a non-opt scalar
     * (bool/int/float). v1 keeps it scalar-only; nested POD-struct fields
     * (recursive inline layout) are a later phase - a struct field makes the
     * struct boxed for now. Called once by the parser after the fields are
     * built.
     */
    void compute_layout() {
        layout = Layout::boxed;
        size = 0;
        align = 1;

        if (fields.empty())
            return;

        for (const auto &f : fields)
            if (f.is_opt || pod_field_size(f.kind) < 0)
                return;                       /* not all non-opt scalars */

        int cursor = 0, maxalign = 1;
        for (auto &f : fields) {
            const int a = pod_field_align(f.kind);
            const int s = pod_field_size(f.kind);
            cursor = (cursor + a - 1) & ~(a - 1);   /* align up */
            f.offset = cursor;
            cursor += s;
            if (a > maxalign)
                maxalign = a;
        }

        size = (cursor + maxalign - 1) & ~(maxalign - 1);   /* tail padding */
        align = maxalign;
        layout = Layout::pod;
    }
};

/*
 * A struct instance. COW value semantics like arrays/dicts (RefCounted; a
 * shared instance is cloned before a mutation; a const instance is deep
 * read-only). v1 storage is `fields` (a boxed LValue slot array); the POD byte
 * buffer is added later.
 */
class StructObject : public RefCounted {

public:

    StructTypeDef *def = nullptr;
    bool readonly = false;
    /*
     * Exactly one is used, per def->layout: `fields` (a boxed LValue slot
     * array) for a boxed struct, `bytes` (a C-laid-out buffer of def->size
     * bytes) for a POD struct. The unused one stays an empty vector (cheap).
     * The default copy ctor copies both, so clone/COW work for either layout.
     */
    std::vector<LValue> fields;     /* boxed: by slot */
    std::vector<char> bytes;        /* POD: def->size bytes */

    StructObject() = default;
    explicit StructObject(StructTypeDef *d) : def(d) {
        if (d->is_pod())
            bytes.resize(static_cast<size_t>(d->size));
    }
    StructObject(const StructObject &) = default;
    StructObject(StructObject &&) = default;

    bool is_pod() const { return def->is_pod(); }
    bool is_readonly() const { return readonly; }
    void set_readonly() { readonly = true; }
    void clear_readonly() { readonly = false; }

    /* Load a POD field as an EvalValue (a typed scalar at its byte offset). */
    EvalValue pod_get(int slot) const {
        const FieldDef &f = def->fields[slot];
        const char *p = bytes.data() + f.offset;
        switch (f.kind) {
            case FieldKind::f_bool:
                return EvalValue(static_cast<unsigned char>(*p) != 0);
            case FieldKind::f_int: {
                int_type v;
                std::memcpy(&v, p, sizeof v);
                return EvalValue(v);
            }
            case FieldKind::f_float: {
                float_type v;
                std::memcpy(&v, p, sizeof v);
                return EvalValue(v);
            }
            default:
                throw InternalErrorEx();
        }
    }

    /* Store a (coerced) scalar value into a POD field's byte slot. */
    void pod_set(int slot, const EvalValue &v) {
        const FieldDef &f = def->fields[slot];
        char *p = bytes.data() + f.offset;
        switch (f.kind) {
            case FieldKind::f_bool:
                *p = v.get<bool>() ? 1 : 0;
                break;
            case FieldKind::f_int: {
                int_type x = v.get<int_type>();
                std::memcpy(p, &x, sizeof x);
                break;
            }
            case FieldKind::f_float: {
                float_type x = v.get<float_type>();
                std::memcpy(p, &x, sizeof x);
                break;
            }
            default:
                throw InternalErrorEx();
        }
    }
};
