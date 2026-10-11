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

### Unary `*` - reading and writing a box (decided 2026-10-08)

Explicit `*`, as C and Rust, and NO implicit conversions. Monomorphization
already gives a template called with a `box<int>` its own instance, so a
box is always a proven static type; the reason for `*` is that three
contexts would otherwise have two meanings each (`b = 5`: store or
rebind? `b == c`: identity or value? `f(b)` into `f(int n)`: copy or
reference?).

- `*b` is an lvalue of type `T`: `*b = 6`, `*b += 1`, `(*b)++`,
  `print(*b)`, `f(*b)`. `b = box(7)` rebinds. `b == c` is identity,
  `*b == *c` compares values.
- A member access dereferences by itself (`b.x` for a `box<P>`, as for a
  class) - the one form with no second meaning.
- A missing `*` is a compile error naming the fix (`b + 1`: "b is a
  box<int>; read it with *b"). Implicit read-unboxing may be added later
  as sugar; taking it away later would break programs.
- Syntax: `a/*b` begins a block comment, exactly as in C (the lexer rule
  stays; CLAUDE.md's "there is no unary `*`" justification changes) -
  write `a / *b`. Unary `*` binds like the other prefix operators, so
  `*b++` is `*(b++)`, a compile error on a box whose message suggests
  `(*b)++`. `*` on a non-box static type is a compile error; on a `dyn`
  it is a run-time check. `box<T>` is a type annotation like `array<T>`;
  `opt box<int>` is a nullable box, and `*` on one needs a narrowing, as
  a member read on an `opt` struct does.

### `box(str)` is a real box (decided 2026-10-08)

A MyLang string is a VALUE (the window model, README): a pass-through
`box(s)` would give no sharing at all. So `box(s)` is a `box<str>`, boxed
like a scalar. The pass-through set is arrays, dicts, class instances,
functions and boxes.

### A field of the class's own type is `opt` (decided 2026-10-08)

A non-`opt` `Node next` field can never be constructed - the first `Node`
would need an existing one. Such a field must be `opt` (`opt Node next`,
`Node? next`); the recursive-struct check keeps refusing the non-`opt`
form, with a message naming `opt`.

### Reference cycles - plans/reference-cycles.md

Refcounting cannot free a cycle, and classes make cycles the ordinary
way to write a parent pointer or a doubly linked list. Classes can ship
before the answer (cycles already exist through arrays and dicts), but
the language needs one before classes are finished. The problem, the
measured facts and the options are in plans/reference-cycles.md; it is
deliberately not designed yet.

## Order of work

1. The shared heap object + `class` (declaration, construction, member
   read/write through the reference, identity `==` / hash, const), every
   engine, `.myv`. **DONE 2026-10-08** - see *Step 1 as built* below.
2. `array<C>` flat pointer storage, `opt C` as null.
3. `box()` over structs (reuses 1), then scalar boxes with `*`.
4. The JIT's inline tiers for member access through a reference.

## Step 1 as built (2026-10-08)

The representation is the decided one: a class is a `StructTypeDef` with
`is_class`, its instances `StructObject`s with the `t_struct` tag, every
difference decided by the def (CLAUDE.md, *A CLASS IS A STRUCT WITH
REFERENCE SEMANTICS*). Decisions taken on the way, for review:

- **A write through an alias of a constant instance raises
  `CannotChangeConstEx`** - for a field (`d.v = 5`) and for a mutating
  builtin on an array it holds (`append(d.xs, 1)`) alike. (An element
  store into a constant ARRAY through an alias raises `NotLValueEx`; one
  exception for the whole object read better than two.)
- **The hash of an instance is an identity NUMBER**, not its address:
  `g_class_ident`, restarted where a program's constants are parsed and
  where a run starts, so a dict keyed by instances iterates in the same
  order in every engine, every run and a `.myv` load. The `.myv` stores
  each constant instance's number.
- **A pure function cannot construct an instance** (a compile error): a
  construction is a new identity, so a folded, cached or de-duplicated call
  would merge objects the program can tell apart. A plain function that
  constructs one is never inferred pure. An instance is made at compile
  time only by a `const` declaration's initializer.
- **`opt` on a class-typed field** is allowed (`opt Node next`); on a
  struct-typed field it stays refused.
- `kindstr(c)` is `"class"`, `:globals` says `class type`, `-vd` prints
  `; class NAME`.
- `deepclone` copies every class instance it reaches once per REFERENCE,
  like everything else it copies - two references to one instance become two
  copies. Whether it should keep sharing (Python's `deepcopy` does) is
  open, and waits on the cycle-safe walk (plans/reference-cycles.md, A).

Two pre-existing bugs found and fixed on the way, each its own commit: the
loop transforms decided invariance by name while arrays and dicts are
references, and a struct's hash was salted with its def's address. A third
was fixed in this step: a struct's `const` member could not hold a struct
construction (its initializer was not parsed as a constant's).

## Corrections to plans/struct-value-semantics.md

Its phase-2 paragraph said a class is "never flat in an array" - decided
otherwise above (flat pointer storage). Its open questions (`==`, hashing,
`const`) are answered above.
