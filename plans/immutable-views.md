# Immutable views — a design TODO (not a plan)

**Status: idea, raised by the maintainer on 2026-09-25. Not designed.**
Filed in `plans/language-deferred.md`; this file only introduces the
subject so it is not lost.

## The idea

A way to hand code a **read-only view** of a container that is not
itself `const` — and to check at **compile time** that the code
receiving it cannot mutate it: not directly (`a[i] = v`, `append(a, x)`,
`erase`, `sort` in place, ...), and not by passing it on to a function
that might.

The motivating case is a builtin callback. `sort(arr, cmp)` hands the
comparator elements of an array it is sorting in place. The comparator
is arbitrary script code, so today the only defence against it
mutating `arr` mid-sort is a RUN-TIME guard after every comparison
(#49: `sort()` raises `InvalidArgumentEx` if the comparator changed
the array's length or storage). That guard costs ~5% on
34_sort_custom_cmp. `map`/`filter` pay a similar per-step length
re-read, and `foreach` a per-iteration shift check (#53) unless
codegen can prove the body inert.

If the callback could only ever see the container through an
immutable view — and the compiler proved it — those run-time checks
would disappear: the mutation would be a compile error, not a
defined run-time exception.

## What already exists to build on

- **Deep read-only values at run time**: `const` containers carry a
  `readonly` flag on the shared object; every write path refuses it.
  That is a run-time property of the VALUE, not a static property of a
  reference, and it is all-or-nothing per object.
- **`func_mutates_input`** (resolver.cpp): the taint analysis that
  decides whether a function mutates a reference parameter — what
  makes `pure` mean "no observable side effects".
- **The #93 parameter escape analysis** (`stamp_noescape_params`): a
  transitive, fail-closed, per-parameter fact over the call graph,
  using the callee-set analysis to name callees.

A compile-time "does not mutate parameter k" fact is close to both of
those, and would likely share their machinery and their fail-closed
rules (an unknown callee or node shape = "may mutate").

## Open questions (for when this is picked up)

- **Surface syntax**, if any: a parameter annotation (`func f(view
  array<int> a)`), a type modifier, or purely inferred with no syntax.
- **What a view forbids**: element writes only, or also growth; is a
  nested container reached through it also read-only (deep vs
  shallow)?
- **Where the guarantee comes from**: the callee's declaration, or a
  whole-program proof at each builtin call site (the comparator is
  usually an inline lambda, so the latter may cover most cases without
  new syntax).
- **Interaction with `dyn`, aliases and captures**: a view stored in a
  captured variable, or into another container, must not become a
  mutable path back to the original.
- **Relation to `const`**: whether a view is just a non-owning
  `const`, and what that means for COW and slices.
- **RULE 1 / RULE 2**: every engine must agree, and a program that
  compiles today must still compile unless it really mutates through
  a view.

## 2026-10-11: `const` parameters, and a deep `const` / `imm`

**Status: the maintainer's idea, being studied - NO decision yet.** He
wrote: "in theory we should have a `const` or `imm` keyword that makes a
parameter treat the input as immutable at a deep level: the whole array
and all of its elements become immutable. That might be hard to
implement." This section records what `const` means today, what the
idea would change, and the design questions it raises.

### What a `const` parameter means today (measured on 3f32e4d8)

`const` on a parameter forbids REASSIGNING it (a compile error, the
resolver). Beyond that it depends on the parameter's type:

| parameter            | what the body tries         | today                  |
|----------------------|-----------------------------|------------------------|
| `const int x`        | `x = 1`                     | compile error          |
| `const S s` (struct) | `s.n = 1`, `s.i = box(9)`   | compile error          |
| `const S s`          | `*s.i = 1` (a box field)    | ALLOWED - writes the   |
|                      | `s.a[0] = 1` (array field)  | caller's box / array / |
|                      | `s.c.v = 1` (class field)   | instance               |
| `const box<int> b`   | `*b = 1`                    | compile error          |
| `const C c` (class)  | `c.v = 1`                   | compile error          |
| `const a`, `const    | `a[0] = 9`, `a[0] += 1`,    | ALLOWED - changes the  |
| array<int> a`        | `a[0]++`                    | caller's array         |
| `const a`            | `append(a, 9)`, `pop(a)`,   | was a RULE 2 split:    |
|                      | `insert`, `erase`, `push`,  | the VM changed the     |
|                      | `sort(a)`, `reverse(a)`     | caller's array, the    |
|                      |                             | tree-walker raised     |
|                      |                             | CannotChangeConstEx    |
|                      |                             | (or sorted a copy)     |
| `const a`            | `a += [9]`                  | compile error (rebind) |

So today's `const` is SHALLOW: it protects the binding, and the fields
of a struct VALUE (which is the callee's own copy anyway), and - by a
separate rule - a box or class instance the parameter IS. It does not
protect an array, a dict, or anything reached through a reference held
in a field. The last row group is a RULE 2 bug, independent of this
design; it is fixed to the shallow meaning the element stores already
had (the binding's const-ness no longer reaches the in-place builtins
in the tree-walker), so that every engine agrees until the design is
decided.

### The idea

A parameter marked `const` (a breaking change of its meaning) or a new
keyword (`imm`, `view`) makes everything reachable from the argument
read-only INSIDE the callee, at every depth:

```C#
func total(imm array<array<int>> m) {
    m[0][0] = 1;          # compile error
    append(m[0], 1);      # compile error
    var row = m[0];
    row[0] = 1;           # compile error: row is a view of m's row
    return sum(map(func(r) => sum(r), m));   # fine: reads only
}
```

The caller's array is NOT frozen: the caller keeps writing it through
its own names. The guarantee is "this function cannot change it through
this parameter", like a C++ `const &`.

### Design questions

1. **Keyword.** Reuse `const` (then `const a` stops allowing `a[0] = 9`,
   a breaking change for existing programs) or add `imm` / `view`, and
   keep `const` = "the name is not rebound". C++ uses one word for
   both, Rust distinguishes `mut` bindings from `&` / `&mut`.
2. **Depth.** Deep (everything reachable: nested containers, struct
   fields, boxes, class instances) or one level. The maintainer's
   statement is deep. Deep must include class instances and boxes, or
   the rows above stay inconsistent.
3. **Where it is enforced.**
   - (A) STATICALLY: a read-only qualifier in the static type (a bit on
     StaticType). A read through a read-only value is read-only; a
     write through one is a compile error; passing one to a
     parameter that is not read-only is a compile error (or the
     function must be inferred not to mutate it - `func_mutates_input`
     already computes that for `pure`). Zero run-time cost.
   - (B) AT RUN TIME: a "view" bit on the reference HANDLE (not on the
     object, which the caller still writes): copied with the handle,
     propagated to a container read through it, checked by every write
     path - including the JIT's inline store tiers. A cost on every
     store, and a bit to find in each handle (the dict handle is a bare
     intrusive_ptr).
   - (C) COPY-IN: the parameter is bound to a deep read-only copy
     (make_const_clone). Simple and sound, but O(size) per call.
   `dyn` loses a static qualifier (`var dyn d = m; d[0][0] = 1;`), so (A)
   alone is not sound: either a dyn copy of a read-only value is
   refused, or (B) backs (A) where the static type is `dyn`.
4. **Escapes.** Returning the parameter, storing it in a global, a
   container or a closure capture: is the stored value read-only too?
   (C++ propagates `const` to the result type.) If not, a mutable alias
   of the caller's array leaks out of a function that promised not to
   mutate it, and a later call can mutate it through that alias.
5. **Callbacks.** A function passed in may still mutate the caller's
   array through the caller's own name while the callee holds a view,
   so a view guarantees nothing about the value staying the same during
   the call - only that the callee does not change it. Whether that is
   enough for the sort/map callback optimization in the first part of
   this file has to be checked case by case.
6. **`pure`.** A `pure` function already may not mutate a reference
   parameter (`func_mutates_input`); it could imply the qualifier on
   every parameter, so the check moves from an analysis to the type
   system.
7. **Slices and copies.** A slice of a view is a view; `clone()` and
   `deepclone()` of a view are ordinary mutable copies.
8. **`.myv`.** A static-only design (A) stores nothing; (B) stores the
   bit wherever a value is stored (a constant pool can hold a view only
   if a constant can be one).

### What it would take

- (A): the qualifier in StaticType and the inferencer's join /
  assignable / check pass; the in-place builtins and every store form
  in the check pass; README and the error messages. Medium; all of it
  in the inferencer.
- (B): a handle bit for arrays, dicts, struct and class instances and
  boxes; every write path in the tree-walker, the VM and the JIT; the
  serializer. Large, and a cost on the hot store paths.
- (C): one bind-time copy in every engine. Small, slow for big inputs.

A reasonable first step, if the maintainer wants the feature: (A) with
the `dyn` hole closed by refusing a read-only value into `dyn`, keeping
(B) for later only if dyn views turn out to be needed.
