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
  copies - except along a cycle, whose back edge the cycle guard
  (plans/reference-cycles.md, part A, landed 2026-10-08) links to the copy
  in progress, so a ring copies to a ring. Whether it should keep sharing
  everywhere (Python's `deepcopy` does) is still open.

Two pre-existing bugs found and fixed on the way, each its own commit: the
loop transforms decided invariance by name while arrays and dicts are
references, and a struct's hash was salted with its def's address. A third
was fixed in this step: a struct's `const` member could not hold a struct
construction (its initializer was not parsed as a constant's).

## Step 2 design - `array<C>` as a flat vector of references (DONE)

A new `SharedObject::Storage::objs`: a vector of `StructObject *`, each
an owned reference (sharedarray.h cannot see `StructObject` complete, so
the retain and the release are out-of-line functions), 8 bytes per
element against a general array's 48-byte `LValue`, a null pointer for
`none` (`array<opt C>`). It follows the
`strs` model (top-10 #7), not the scalar one:

- CREATION is type-driven: an `ArrHint::flat_c` (with `arr_hint_struct`
  naming the class) on an array whose destination is `array<C>` /
  `array<opt C>`, stamped by `set_array_repr_hint` exactly where `flat_s`
  is for a POD struct. A literal, an empty `[]`, `array(n)` and
  `make_array` honour it; anything else stays general.
- HOT PATHS read and write the handles directly: element read (boxing a
  handle is a retain), element store of an instance of that class or
  `none` (anything else PROMOTES - `dyn` laundering), append, pop, len,
  foreach, slices (offset/len, as every flat kind), ==, hash, printing,
  clone (a handle copy), const freezing (each instance frozen in place),
  the `.myv` array record (a new storage kind, its elements `cls`
  records).
- EVERY OTHER op promotes IN PLACE through `get_vec()` (insert, erase,
  sort, reverse, map, filter, ...), as a strs or structs array does - so
  a missing fast path costs speed, never an answer.
- A STORE THROUGH AN ELEMENT (`a[i].v = 5`) needs no location: the
  element is a reference, so the walk reads the handle and stores into
  the instance (a temporary holder for `struct_own`, which hands a class
  instance back as it is). The tree-walker's store walk and the VM's
  `vm_chain_walk` each get that one step; the JIT's element tiers decline
  a non-general storage to their helpers today, which stays correct.
- `array_storage()` reports `"class"`.

Nets to build with it: the functional test's arrays of classes under
every engine and lever, `array_storage` checks that the flat kind is
really chosen (else the test is vacuous), promotion by every cold op,
a `dyn` alias storing a non-instance, and a `.myv` round trip of a
constant `array<C>`.

## Step 2 as built (2026-10-08)

As designed above, with these decisions:

- **Any class instance fits, of any class.** The storage holds references,
  so it needs no element def; an element reads back as the instance with
  its own def. Only a value that is not a class instance or `none` (through
  a `dyn` alias) promotes.
- **A literal of class instances is flat by its VALUES too** (mode 6 of
  `build_array_from_values`), as a literal of POD structs is: a constant's
  array literal is built before inference, so only its values can make it
  flat, and that is what puts the `objs` record into a `.myv` image.
- **A store through an element needs no LValue**: a class instance read as
  a value is a location (`store_rooted`, `vm_member_lvalue_ref`, the chain
  store's final member step), refused with `CannotChangeConstEx` for a
  constant's - so `a[i].v = 5` works for any array, flat or not.
- **A class (or struct) may name itself in a field's annotation**:
  `class Tree { array<Tree> kids; }`. Without it the canonical tree did not
  parse ("'Tree' is not a type").
- **The hint at a constructor or call argument: built after step 2**, as
  part of the #46 literal rule (a literal takes the storage of the
  declared type it lands in, at every level): `Tree(v, [], p)` builds its
  `[]` flat class storage, and `f([])` for an `array<int>` parameter flat
  ints.
- `-nti` has no types, so a type-driven `array<C>` is general there (a
  literal of instances is still flat): the functional test prints those
  storages instead of asserting them, the `-rt` `class:` cases assert.

Watched failing (a sabotage harness, each case rebuilt and checked, the
source restored by a plain copy and rebuilt, then a control run that must
pass): the class `struct_own` arm, the chain store's class holder, the
member-store class arm, `ovec_type`'s destructor, `promote_objs_to_general`,
value-driven mode 6, the `flat_c` literal arm, the self-named field type,
`arr_push_value`'s objs case, `flat_store_core`'s objs arm, the `.myv`
writer's objs records, the `flat_c` hint, and the const-clone /
mutable-clone objs arms - each fails the functional test, `-rt`'s
`class:` cases or the image round trip, and each control passes. One is
unobservable by construction: `ovec_type::set` retains the new element
before releasing the old (a self-store `a[i] = a[i]` would otherwise free
it), and its one caller (`flat_store_core`) holds the value it stores, so
the order never matters today; it stays, as the type's own invariant.

Bugs found and fixed on the way, each its own commit: `+=` on a flat array
built a fresh array whenever the left side was not general (a flat string
or struct array lost its aliases; a flat scalar array through `dyn` took a
value it cannot hold); `sum()` of a flat struct array was an
InternalErrorEx. Fixed in this step's own commit: the `.myv` writer's
general-array arm applied a slice's offset twice (latent - no slice
reaches the writer).

## Step 3 design - `box()` and unary `*`

**Static types.** `box<T>` is a new static kind (`StaticTypeKind::Box`,
its `elem` the boxed type), written as an annotation like `array<T>`
(`box<int> b;`, `opt box<P> q;`, a field `box<int> n;`). It is invariant
(`box<int>` is not a `box<float>`), and a box is never `none` unless
`opt`. `typestr` / `kindstr`: `box<int>` / `box`.

**`box(v)`** is a run-time builtin - never const-folded, so a compile-time
pass can neither duplicate nor merge a box's identity, and no box reaches
a `.myv` constant pool. By the argument's STATIC type:
- an array, a dict, a function, a class instance or a box: `v` itself
  (the static type is unchanged);
- a struct `P`: a `box<P>`;
- `int` / `float` / `bool` / `str`: a `box<int>` ... `box<str>`;
- `dyn`: decided by the run-time value the same way, typed `dyn`;
- a possibly-`none` argument is a compile error (box what it holds).

**Representation.** Two, each reusing what exists:
- `box<P>` is a `StructObject` flagged `boxed` - a copy of the struct in
  the one heap object kind classes use. Every reference rule a class has
  is asked of the OBJECT (`is_ref()`: the def is a class, or it is
  boxed): `struct_own` writes it in place, `==` and `hash` are identity
  (an identity number, as for a class), copying a `box<P>` copies the
  reference, and member access needs nothing new (`b.x`, `b.x = 5`,
  `b.inner.y++`, `append(b.a, 1)` already work on a `StructObject`).
  `*b` reads a COPY - an unboxed clone; `*b = q` overwrites the box's
  fields in place, keeping its identity.
- `box<int>` / `box<float>` / `box<bool>` / `box<str>` is a new value
  kind `t_box` (`BoxObj`: a cell, the element kind it was made with, the
  read-only bit, an identity number). A store through a `dyn` alias is
  checked against the element kind at run time (an int into a
  `box<float>` widens, a string into a `box<int>` is a TypeErrorEx), so
  no typed reader can find a misfit.
- `print(b)`: `box(5)`, `box(P(x: 1))`. `clone(b)` / `deepclone(b)`: a new
  box (a new identity) holding a copy.

**Unary `*`** (`DerefExpr`, a location): `*b` reads the boxed value,
`*b = v` / `*b OP= v` / `(*b)++` / `--*b` write it. The ASSIGNABLE-SHAPE
rule gains it as a fifth location form. `*` on a non-box static type is a
compile error; on a `dyn` a run-time check. A box used as its value
(`b + 1`, `f(b)` into an `int` parameter) is a compile error naming the
fix ("b is a box<int>; read it with *b"). Precedence: a prefix operator
(pExpr02), so `*b++` is `*(b++)` - refused on a box with a message naming
`(*b)++`; `a/*b` opens a block comment exactly as in C.

**Engines.** The tree-walker evaluates `DerefExpr`; the VM has
`LoadBoxV` (dst = *box) and `StoreBoxV` (*box = value, the value widened
by the store's `rv_coerce`), and lowers a compound or inc-dec to a load,
the op and a store with the box held in one temp. The JIT runs the two
ops as helper calls (inline tiers are step 4). `.myv`: the two opcodes
are appended (version bump); no value record changes, since no box is
ever a constant.

**Analyses that must learn it** (each a place a box's sharing is
visible): the loop transforms (a store through a box, or into a
`box<P>`'s field, is shared storage - `th_val` false, `alias_content`
set); the escape analysis and the inliner's write-through rule (`*p = v`
writes THROUGH `p`); the cycle walks (a `t_box` has one child); the
constant-identity rules (`box()` is not const).

## Corrections to plans/struct-value-semantics.md

Its phase-2 paragraph said a class is "never flat in an array" - decided
otherwise above (flat pointer storage). Its open questions (`==`, hashing,
`const`) are answered above.
