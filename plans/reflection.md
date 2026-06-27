# Reflection: native composite types, structured layout(), Type objects

A small reflection subsystem. The keystone is **native composite types** -
`StructTypeDef`s the interpreter creates in C++ (not from a user `struct` decl)
that builtins construct and return, and that the inferencer knows about so field
access type-checks.

## Item 4 - DONE: structured `layout()` via native structs

`layout(S)` returned a string; it now returns a **`StructLayout`** value:

```
struct StructLayout {
    str   name;
    int   size;     # POD total size in bytes (0 for boxed)
    int   align;
    bool  pod;
    array<StructField> fields;
}
struct StructField {
    str  name;
    str  type;      # the field's type string (e.g. "int", "array<int>", "Point")
    int  offset;    # POD byte offset, else -1
    int  size;      # POD field size in bytes, else -1
    int  align;     # POD field alignment, else -1
}
```

So `layout(S).fields[0].name`, `.offset`, etc. work programmatically.

**Mechanism (native composite types):**
- `native_struct_field_def()` / `native_struct_layout_def()` (eval.cpp, declared
  in structtype.h) build the two `StructTypeDef`s once (lazy `static`), boxed
  (they have str/array fields), slots assigned in order. The `fields` field of
  StructLayout carries a `TypeAnnot` of `array<StructField>`.
- `builtin_layout` (reflect.cpp.h) constructs the nested boxed `StructObject`s.
- The inferencer registers both defs in `struct_by_name` (in `setup()`), and
  `builtin_result("layout")` returns `struct_ty(StructLayout)` - so the result
  and its field accesses type-check (no `dyn` needed).

## Item 5 - FUTURE: Type objects (the `type`/`typeof` unifier)

Make `type(value)` and `decltype(variable)` return a native **`Type`** object
instead of a string, so reflection is programmatic and the bare-kind/structural
split (the current `type` vs `typeof` redundancy) collapses:

```
struct Type {
    str   kind;       # "int" / "array" / "dict" / "struct" / "func" / ...
    str   name;       # full structural string: "array<int>", "Point"
    bool  nullable;   # opt?
    Type? elem;       # array element (recursive, opt)
    Type? key;        # dict key
    Type? val;        # dict value
    # later: fields (for structs) -> array<StructField>, params/ret (func)
}
```

When this lands, `typeof()` is **absorbed**: its structural string becomes
`str(type(v))` / `type(v).name`, and the bare kind is `type(v).kind`. This is
the agreed end-state (option B): keep `type`/`typeof`/`decltype` as strings
until Type objects exist, then unify here.

**Why it builds on item 4:** `Type` is just another native composite type (and
recursive via the `opt Type` self-reference, allowed by the struct-recursion
rule). The construction + inferencer-typing machinery is the same as item 4.

**Cost / breaking change:** `type(x)` / `decltype(x)` stop returning strings, so
every `type(x) == "int"` / `decltype(x) == "array<int>"` becomes
`type(x).name == ...` (or `str(type(x)) == ...`) - hundreds of call sites/tests.
That churn is why it is staged after item 4 and gated on an explicit go-ahead.
