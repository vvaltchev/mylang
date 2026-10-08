# Struct value semantics (maintainer, 2026-10-08)

## The decision

A `struct` is a VALUE type, as in C#: every copy of a struct is an
independent value. A copy is made wherever a struct moves to a new home -
assignment and declaration (`var f = b[0]`), an argument, a return, a
`foreach` variable, a capture, a container element, a field. The copy is
SHALLOW in the C# sense: the plain fields and the nested structs are
copied; an array or a dict inside it is a reference type and stays
shared. `b[0].x = 99` still writes the element in place - `b[0]` is a
location.

A `class` (phase 2) is the reference type, conceptually a
`shared_ptr<struct>`: copying one copies the reference. The TYPE decides
the semantics.

Until now every boxed struct aliased (`var q = p; q.x = 9` changed `p`),
while a flat struct array's element read was a copy - the same source
gave different answers depending on the storage the optimizer picked, a
RULE 2 violation the README's "layout is transparent" claim hid.

## The mechanism: copy-on-write, decided at the HOLDER

Copying eagerly at every move would need a copy op wherever a value
travels (MoveV, every bind tier of every call protocol, the JIT's raw
copies, returns, captures, containers) and an allocation per copy. A
struct is never observably shared except through a WRITE, so the copy is
deferred to the write: a struct object may be shared by any number of
holders, and a write first makes it its holder's OWN.

    StructObject &struct_own(LValue *&holder);   /* eval.cpp */

returns the object the holder may write in place: the one it holds, if
no other holder can see it, else a shallow MUTABLE clone put into the
holder first. "Another holder can see it" is

- `use_count() > 1` - another slot, field, element, capture or
  exception holds it;
- the holder is BORROWED (#94: a parameter bound with no retain, so the
  count does not include it - the caller's slot holds the counted
  reference). The clone replaces it through `frame_release`, never a
  plain overwrite, which would release a count the slot never took;
- the object is READ-ONLY: it is a constant's (deep read-only) value or
  part of one, which a copy may change and the constant may not. The
  clone clears the flag; its reference-typed fields keep theirs.

An array ELEMENT is detached from its array first
(`LValue::write_target`: a slice made standalone, an alias's live slices
cloned), and may then live in NEW storage - so `holder` is a reference
the function updates, and the caller uses it, never the slot it passed.
A const binding is decided at compile time (a constant's field: the
parser; a `const` parameter's: the inferencer - see below). A holder
inside a read-only container never reaches here: the container's element
read is an rvalue, and a field of a VALUE is not a location
(`member_store_step` / `vm_member_lvalue_ref` hand out a field only for a
slot's struct).

THE ORDERING RULE: own the holder BEFORE deriving any pointer into the
object. A transient copy of the struct (the tree-walker's `RValue`)
raises the count, so owning after taking one clones needlessly and -
worse - leaves the write going into the transient's (old) object. Every
write path walks holder by holder: own the root holder, take the field's
LValue from the owned object, own that (a nested struct), and so on.

## Where a struct is written in place (the inventory)

C++, both engines:

- tree-walker: `store_walk` (the base of every store - member_store,
  subscript_store's base, the dyn inc-dec, a mutating builtin's first
  argument through `store_base_value`) owns each struct a member step
  enters; `member_store` owns the final holder;
- VM: `vm_member_store`, `vm_member_lvalue`, `vm_member_lvalue_ref`
  (now taking the walk's cursor, so it can own the slot), `vm_chain_walk`
  (StoreLValueChainV / IncDecChainV), IncDecMemberCheckedV;
- the POD places (`pod_place_rooted_field` enters a rooted struct's
  bytes; a flat array's element bytes belong to the array, a reference
  type, and need nothing).

Already value-safe: the struct constructors' in-place reuse (planned
ctor, C4e) and the flat foreach's `vm_struct_elem_into` (#110) - each
requires `use_count() == 1` and a non-read-only object, and their dst is
a frame local, never a borrowed parameter.

Emitted code: the inline field-store tier of StoreMemberV (#97 inc 4) is
the only in-place struct write. It gained two guards - the object's
refcount is 1 (`memberv_shared`), the base slot's `borrowed` byte is 0
(`memberv_borrowed`) - and declines to its helper otherwise (the readonly
guard was already there). Its const-slot guard is gone: a const binding
no longer stops a field write at run time (see `const` parameters).

## Passes that alias two names (the half the engines cannot fix)

COW decides per holder, so any pass that makes two NAMES one slot - or
substitutes one variable for another - breaks it: the write then lands
in the other variable's holder. Each of these was sound while structs
aliased:

- the AST inliner substitutes a caller variable for a parameter the body
  WRITES THROUGH (`writes_through`: "only a NAME may fill a write
  position"). DONE: a parameter written INTO through a member step
  (`p.x = v`, `p.f[i] += v`, `p.x++` - `param_member_written`, the
  shape `member_write_root` recognizes) takes no argument directly, not
  even a name, in the expression, block and tail engines; it is
  temp-bound, and the temp is the call's copy. The shape cannot tell a
  struct field from a dict key, so a dict member write temp-binds too,
  which is harmless (the temp holds the same dict);
- `collapse_locals` copy-propagates a write-once local (`var q = p;
  q.x = 5`). DONE: `count_slot_writes` counts a write THROUGH a local
  (any chain rooted at it, an inc-dec operand, a mutating builtin's
  first argument) as a write of it, so such a local is neither
  write-once nor a value-stable rvalue source. That also closes the
  same hole for a SLICE local, whose element store detaches it;
- the bytecode inliner renames a parameter onto its argument's slot when
  the body "never writes" it (`bc_site_param_renames`), and a field
  store has no def row in `visit_use_def` (#25, on purpose). NO CHANGE
  NEEDED: `bc_inline_op_ok` admits no store-through op at all, so a
  body that writes a field is never inlined at bytecode level. Admitting
  one later requires marking its base as written.

The `Identifier::may_hold_struct` stamp the first draft planned is not
needed: the member-first SHAPE is a sufficient, type-free test.

## `const` parameters (decided during the work)

The VM never enforced a `const` parameter at run time (its bind
ignores `ParamDesc::cnst`, the borrow clears the flag, the inliner's
temp is not const); only the tree-walker refused a POD field write
through one - a pre-existing RULE 2 divergence. Settled as C#'s `in`:
writing a field of the struct a `const` parameter holds, through struct
fields only (`c.x`, `c.inner.x`, `c.x++`), is a compile error when the
type is known (`Inferencer::check_const_param_store`); a write into an
array or a dict it holds (`c.a[0] = v`, `append(c.a, v)`, `c.d.k = v`)
is allowed. At run time no engine enforces const on a parameter, so a
`dyn` const parameter is a writable copy everywhere.

## Found on the way (pre-existing, fixed separately)

- a store into a slice's element freed the element mid-store when the
  slice was its storage's only owner (UAF, every engine), and the VM's
  compound int store into a general slice skipped the detach - fixed
  in `LValue::write_target` / `slot_rmw`;
- the VM's in-place builtins (`append`, `sort`, ...) on a deep chain or
  a dyn member (`append(o.i.a, x)`, `append(dynv.a, x)`) work on a held
  copy, so a slice there detaches the copy and the write is lost (the
  tree-walker writes the field) - task #32.

## Phase 2: `class`

`class Name { ... }`: the declaration grammar of a struct; reference
semantics (never owned/cloned - a write goes into the shared object);
never flat in an array and never embedded inline (always boxed); a
field may name its own class without `dyn?` (a reference cannot recurse
infinitely). Open for the maintainer: `==` (identity, as C#, or
structural, as a struct), hashing and a class as a dict key (keys are
frozen by a deep read-only clone today), `const` of a class value.
