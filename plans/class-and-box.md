# `class` and `box()` - reference types (maintainer, 2026-10-08)

Phase 2 of plans/struct-value-semantics.md. A `struct` is a value; a
`class` is the same declaration with REFERENCE semantics - conceptually a
`shared_ptr<struct>`. `box(v)` turns any value into a reference to a heap
copy of it. In both, the STATIC TYPE decides the semantics; nothing ever
asks at run time whether a value is boxed (`dyn` excepted - it is the one
runtime-typed value, as everywhere else).

## Decided

### `class`

- `class Name { ... }` has a struct's declaration grammar (fields, `const`
  members, named / positional construction, zero-initialised `Name n;`).
- Copying a class value copies the REFERENCE (a refcount bump). A write
  through any holder - `c.x = 5`, `c.a[0] = 1`, `c.inner.y++` - goes into
  the one shared object; it never triggers struct_own's copy.
- `==` / `!=` compare IDENTITY. `hash` is identity, so a class instance is
  a dict key by identity (a struct key stays structural, frozen by the
  deep read-only clone).
- `const c = C(...)` makes the WHOLE object read-only (every field,
  through every alias reached from `c`) - the same deep-const the struct
  and the containers have.
- A struct holding a class field copies the reference with the struct
  (shallow): `var s2 = s1;` shares `s1.n`'s object; `s2.n.v = 7` is seen
  through `s1`; `s2.n = Node(0)` rebinds `s2`'s field only. A struct with a
  class field is boxed (not POD), like one with an array field.
- A struct field inside a class lives inside the class object (the class
  object is where the bytes are; `c.inner.x = 1` writes there).
- `array<C>` is stored FLAT: a vector of 8-byte smart pointers, the way an
  `array<str>` holds `SharedStr` handles - smaller than a general array's
  48-byte `LValue`s. `a[i].x = 5` writes the shared object. `array<opt C>`
  stores a null pointer for `none`.

### `box()`

- `box(v)` where `v`'s static type is already a reference type - an
  array, a dict, a class instance, a function, a box - returns `v` itself.
- `box(v)` over a VALUE type creates a heap cell holding a COPY of `v`,
  of type `box<T>`: `box(5)` is a `box<int>`, `box(p)` with `p : P` (a
  struct) is a `box<P>`.
- `box<P>` (P a struct) behaves like a class instance: passed by
  reference, `b.x = 5` writes the shared cell, `==` is identity.
- Run-time representation: ONE heap object kind serves class instances and
  `box<struct>` (the struct's storage, never copied on write - the TYPE
  tag says "reference" where `t_struct` says "value"); `box<int>` /
  `box<float>` / `box<bool>` are a scalar cell.

## Proposed - open for the maintainer

### O1. How a scalar box is read and written

Auto-unboxing CAN be decided entirely at compile time: a function template
called with a `box<int>` already gets its own instance (monomorphization
keys on the argument types), so inside it the box is a proven static type
and no runtime test is needed - no template duplication beyond what
already happens. The hard part is not the implementation, it is that three
contexts have two meanings each once a box reads as its value:

- `b = 5` - store into the cell, or rebind `b`? (If assignment writes
  through, a box variable can never be rebound, and `a[0] = box(9)` writes
  into the box `a[0]` holds instead of replacing it.)
- `b == c` with two boxes - identity (class semantics) or value? With
  unboxing on reads, `b == 5` is a value test and `b == c` an identity
  test, side by side.
- `f(b)` where `f(int n)` - a silent copy of the value, while `g(b)` where
  `g` is a template passes the reference.

**Recommendation: explicit `*`, as C and Rust, and no implicit
conversions.** `*b` is an lvalue of type `T`: `*b = 6`, `*b += 1`,
`(*b)++`, `print(*b)`, `f(*b)`. `b = box(7)` rebinds. `b == c` is
identity, `*b == *c` compares values. A member access dereferences by
itself (`b.x` for a `box<P>`, as for a class) - that is the one form with
no second meaning. A missing `*` is a compile error naming the fix
(`b + 1`: "b is a box<int>; read it with *b"). Implicit read-unboxing can
be added later as sugar if it proves worth its ambiguity; taking it away
later would break programs.

### O2. `box(str)`

A MyLang string is a VALUE: `var b = a; a += "!"` leaves `b` unchanged (the
window model, README). A pass-through `box(s)` would therefore give no
sharing at all - `*b += "x"` through one holder would not be seen by
another. **Recommendation: a string is boxed like a scalar (`box<str>`).**

### O3. Syntax notes for unary `*`

- `a/*b` begins a block comment, exactly as in C (the lexer rule stays;
  CLAUDE.md's "there is no unary `*`" justification changes). Write
  `a / *b`.
- Unary `*` binds like the other prefix operators, so `*b++` is
  `*(b++)` - a compile error on a box, whose message suggests `(*b)++`.
- `*` on a non-box static type is a compile error; on a `dyn` it is a run
  time check (the one runtime-typed value).
- `box<T>` is a type annotation like `array<T>` (`box<int> b = box(5);`),
  `opt box<int>` a nullable box; `*` on an `opt` box needs a narrowing, as
  a member read on an `opt` struct does.

### O4. Reference cycles

Refcounting cannot free a cycle, and classes make cycles the ordinary way
to write a parent pointer or a doubly linked list. Phase 2 can ship
without an answer (the INT object census reports such a leak), but the
language needs one before classes are finished: a `weak` field modifier,
or a cycle collector over class objects. Not proposed yet.

### O5. A field of the class's own type

The struct plan said a class field may name its own class "without
`dyn?`, since a reference cannot recurse infinitely". The STORAGE cannot,
but a non-`opt` `Node next` field can never be constructed: the first
`Node` would need an existing one. **Recommendation:** such a field must
be `opt` (`opt Node next`), and `Node? next` / `Node next = none` read as
the linked-list spelling; the recursive-struct check keeps refusing the
non-`opt` form, with a message naming `opt`.

### O6. Order

1. The shared heap object + `class` (declaration, construction, member
   read/write through the reference, identity `==` / hash, const), every
   engine, `.myv`.
2. `array<C>` flat pointer storage, `opt C` as null.
3. `box()` over structs (reuses 1), then scalar boxes with `*`.
4. The JIT's inline tiers for member access through a reference.

## Corrections to plans/struct-value-semantics.md

Its phase-2 paragraph said a class is "never flat in an array" - decided
otherwise above (flat pointer storage). Its open questions (`==`, hashing,
`const`) are answered above.
