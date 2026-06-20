/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "uniqueid.h"
#include <vector>
#include <utility>

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
    int offset = -1;      /* POD: byte offset (later phases); else -1 */
};

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
    std::vector<LValue> fields;     /* by slot; boxed storage */

    StructObject() = default;
    explicit StructObject(StructTypeDef *d) : def(d) { }
    StructObject(const StructObject &) = default;
    StructObject(StructObject &&) = default;

    bool is_readonly() const { return readonly; }
    void set_readonly() { readonly = true; }
    void clear_readonly() { readonly = false; }
};
