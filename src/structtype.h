/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "uniqueid.h"
#include <vector>
#include <utility>
#include <cstring>
#include <memory>

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

struct StructTypeDef;

/* DeclType is defined in syntax.h (which includes this header); a forward
 * declaration with the underlying type is enough to use it as a member. */
enum class DeclType : unsigned char;

/*
 * A parsed, recursive TYPE annotation - what backs the parameterized container
 * syntax `array<A>` / `dict<K, V>` (and composes: `dict<str, array<Point>>`).
 * `kind` is the DeclType (bool/int/float/str/dyn/strct, or arr/dict for the
 * containers); `opt` is the `?` nullable suffix; `strct` is set when kind is a
 * user struct; `elem` (array) and `key`/`val` (dict) are the parameters (null
 * for the *generic* `array`/`dict` form, which leaves the element type to
 * inference). Built by `pTypeAnnot` (parser.cpp), converted to an `STy` by the
 * inferencer (`annot_to_sty`). Immutable after parse, so it is freely shared
 * (a clone shares the same node).
 */
struct TypeAnnot {
    DeclType kind;
    bool opt = false;
    const StructTypeDef *strct = nullptr;       /* kind == strct */
    std::shared_ptr<TypeAnnot> elem;            /* kind == arr */
    std::shared_ptr<TypeAnnot> key, val;        /* kind == dict */
};

struct FieldDef {
    const UniqueId *name = nullptr;
    FieldKind kind = FieldKind::f_dyn;
    const UniqueId *struct_ty = nullptr;  /* when kind == f_struct */
    /* For an f_struct field: the resolved nested type (if it was declared
     * before this struct). Non-null AND POD => the field is embedded inline
     * (recursive POD); otherwise the field is a boxed pointer. */
    const StructTypeDef *struct_def = nullptr;
    bool is_opt = false;
    /* For a parameterized container field (`array<int> xs;`): the recursive
     * element/key/value type. Null for a generic `array`/`dict` field. */
    std::shared_ptr<TypeAnnot> annot;
    int slot = -1;        /* boxed: index into StructObject::fields */
    int offset = -1;      /* POD: byte offset into the bytes buffer; else -1 */
};

/*
 * POD field metrics: our OWN fixed alignment rules so a struct's C-style layout
 * depends only on the word size (sizeof(int_type)), never on host `alignof`.
 * This is the one place an arch assumption lives (see plans/structs.md s6); on
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

class StructObject;

/*
 * Store a (already-coerced) value into a POD field's byte slot at `base +
 * f.offset`. Shared by StructObject::pod_set and the construct-in-place append
 * fast path (builtin_append) so both write bytes identically. Defined out of
 * line in eval.cpp (needs StructObject complete for the nested-struct case).
 */
void pod_store_field(const FieldDef &f, char *base, const EvalValue &v);

class EvalContext;
class Construct;

/*
 * The construct-in-place fast path behind `append(flat_struct_arr, S(...))`:
 * build the new POD element straight into the array's byte buffer, no temporary
 * StructObject. Returns false (fall back to the normal value-append) unless the
 * arg is a struct-constructor call for the array's exact POD type. Defined in
 * eval.cpp; called from builtin_append (a different TU).
 */
bool try_construct_into_struct_array(EvalContext *ctx, SharedArrayObj &arr,
                                     Construct *arg);

/*
 * Native (interpreter-defined) composite types for reflection - StructTypeDefs
 * built in C++ rather than from a user `struct` decl. `StructLayout` (returned
 * by layout()) holds general info + an `array<StructField>`; each `StructField`
 * has name/type/offset/size/align. Built once, program-lifetime. The inferencer
 * registers them in struct_by_name so field access on the result type-checks.
 * Defined in eval.cpp. See plans/reflection.md.
 */
const StructTypeDef *native_struct_field_def();
const StructTypeDef *native_struct_layout_def();

/*
 * The native `Type` reflection object returned by type()/decltype():
 *   struct Type { str kind; str name; bool nullable;
 *                 Type? elem; Type? key; Type? val; }
 * Recursive via the `opt Type` self-reference (elem/key/val), allowed by the
 * struct-recursion rule. Built once; program-lifetime.
 */
const StructTypeDef *native_struct_type_def();

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
    /* A field's POD size/align: a scalar's own metrics, or - for an f_struct
     * field whose nested type is itself POD - that struct's size/align embedded
     * inline (recursive POD). Returns false if the field can't be POD-inlined
     * (a ref kind, an opt field, or a boxed/forward-ref struct field). */
    static bool pod_field_metrics(const FieldDef &f, int &sz, int &al) {
        if (f.is_opt)
            return false;
        const int s = pod_field_size(f.kind);
        if (s >= 0) {
            sz = s;
            al = pod_field_align(f.kind);
            return true;
        }
        if (f.kind == FieldKind::f_struct && f.struct_def &&
            f.struct_def->is_pod()) {
            sz = f.struct_def->size;
            al = f.struct_def->align;
            return true;
        }
        return false;
    }

    void compute_layout() {
        layout = Layout::boxed;
        size = 0;
        align = 1;

        if (fields.empty())
            return;

        int sz, al;
        for (const auto &f : fields)
            if (!pod_field_metrics(f, sz, al))
                return;                       /* not all POD-inline-able */

        int cursor = 0, maxalign = 1;
        for (auto &f : fields) {
            pod_field_metrics(f, sz, al);
            cursor = (cursor + al - 1) & ~(al - 1);   /* align up */
            f.offset = cursor;
            cursor += sz;
            if (al > maxalign)
                maxalign = al;
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
        ML_CHECK(slot >= 0 && slot < static_cast<int>(def->fields.size()));
        const FieldDef &f = def->fields[slot];
        /* offset == -1 marks a non-POD (boxed) field: pod_get on it would read
         * at bytes.data()-1. A POD instance must only pod_get POD fields. */
        ML_CHECK(f.offset >= 0 && f.offset < static_cast<int>(bytes.size()));
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
            case FieldKind::f_struct: {
                /* an inline nested POD struct: copy its bytes into a fresh one */
                StructTypeDef *nd = const_cast<StructTypeDef *>(f.struct_def);
                auto obj = make_intrusive<StructObject>(nd);
                std::memcpy(obj->bytes.data(), p, nd->size);
                return intrusive_ptr<StructObject>(obj);
            }
            default:
                throw InternalErrorEx();
        }
    }

    /* Store a (coerced) scalar value into a POD field's byte slot. */
    void pod_set(int slot, const EvalValue &v) {
        ML_CHECK(slot >= 0 && slot < static_cast<int>(def->fields.size()));
        ML_CHECK(def->fields[slot].offset >= 0);
        pod_store_field(def->fields[slot], bytes.data(), v);
    }
};
