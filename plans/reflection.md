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

## The type-query family (compile-time; replaces `typeof`)

All are **compile-time type queries** with an *unevaluated* operand (like C++
`decltype`/`sizeof` - the argument's value is never computed), folded to a
literal from the argument's STATIC type. No runtime work when the type is known
(always, since even `dyn` is statically `dyn`). MyLang leans compile-time: never
do at runtime what can be done at compile time; this is groundwork for an
eventual compiled mode.

- **`typestr(x)`** -> the full structural string: `"array<dict<str,int>>"`,
  `"int?"`, `"Point"`. (Replaces the old `typeof`.)
- **`kindstr(x)`** -> just the kind: `"array"`, `"int"`, `"struct"`. (The
  shortcut for the many tests that only check the kind.)
- **`type(x)`** / **`decltype(v)`** -> a `Type` object (item 5 below).
- **`typeof` is REMOVED.**

All are `const` and **const-fold** (the inferencer rewrites the call to its
literal result, via the generalized `fold_decltype`). Under `-nti` they fall
back to the value's runtime type. `decltype` takes an identifier; the rest take
any expression.

## Item 5 - Type objects (`type` / `decltype`)

`type(value)` and `decltype(variable)` return a native **`Type`** object:

```
struct Type {
    str   kind;       # "int" / "array" / "dict" / "struct" / "func" / ...
    str   name;       # full structural string: "array<int>", "Point"
    bool  nullable;   # opt?
    Type? elem;       # array element (recursive, opt)
    Type? key;        # dict key
    Type? val;        # dict value
}
```

So `type(a).kind == "array"`, `type(a).elem.kind == "int"`, `type(a).name`.

**Pre-generated, never on-the-fly.** A `Type` is built at COMPILE TIME by the
inferencer for each `type()`/`decltype()` call site (from the arg's static
`STy`) and baked as a `LiteralObj` - we do NOT generate `Type`s at runtime
(there is no programmatic type creation, so none is needed, and unused
reflection costs no memory). `typestr`/`kindstr` give the string forms cheaply.
`Type` is another native composite type (item 4's machinery), recursive via the
`opt Type` self-reference.

## Status

- DONE: the string family (`typestr`/`kindstr`; `typeof` removed).
- NEXT: `Type` objects (`type`/`decltype`) - converts `type(x)==K` ->
  `kindstr(x)==K` and `decltype(x)==S` -> `decltype(x).name`.
