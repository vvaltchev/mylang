# `.myv` cross-table references: identity BEFORE content

Status: **DESIGNED, NOT STARTED.** The one shape that can hit it today
is REFUSED at write time (commit 2d8793b), so nothing is broken and this
is a design decision, not a fire. Maintainer's call, 2026-08-25: the
obvious fix (swap two sections) is a lateral move and is NOT what we
want — see *Why the swap is not the answer*.

## The bug that exposed it

    pure func sq(x) => x * x;
    struct Ops { const F = sq; }
    var dyn f = runtime(Ops.F);
    print(f(7));

runs fine from source (`49`), and before 2d8793b compiled to an image
that COULD NOT BE LOADED:

    $ mylang -c prog.my -o prog.myv     # exit 0, image written
    $ mylang prog.myv
    MyvError: corrupt .myv (descriptor)

The file was not corrupt. It was exactly what the writer meant to
produce, and the message blamed the user's file for the format's own
defect. Today the writer refuses instead, naming the reason:

    MyvError: unserializable value (a function in a struct's const
    member - the descriptor table is written after the struct table,
    so it cannot be read back)

## The actual root cause

A stored FUNCTION value is a REFERENCE, not a function: value tag 9 is
a `dref` — a `u32` index into the descriptor table (section 11 of
docs/myv-format.txt). The image is a single forward pass:

    HEADER / SOURCE REF / STRING TABLE
    STRUCT TABLE      (section 7)   name, fields, folded `const` members
    DESCRIPTOR TABLE  (section 8)
    CHUNKS            (section 9)
    GLOBALS           (section 10)

**The writer can see forward; the reader cannot.** `myv_write` builds
`struct_ids` AND `desc_ids` before emitting a single byte, so when it
reaches a struct's const member holding `sq` it resolves the descriptor
to index 2 and writes `[09][02]` without noticing anything. The reader
meets that same byte while still inside section 7, where `r.descs` is
still EMPTY — every index is out of range.

Struct consts are the ONLY value-bearing record that precedes the
descriptor table; a const array of functions rides a CHUNK pool
(section 9) and round-trips fine, which is why the hole stayed open.

## Why the swap is not the answer

The section order IS a topological sort of the table-reference graph.
Today that graph is acyclic:

    struct fields  -> earlier structs only
    struct consts  -> descriptors, struct types
    descriptors    -> (nothing)
    chunks         -> structs, descriptors
    globals        -> structs, descriptors

so swapping 7 and 8 would work — `descriptors -> nothing` holds only
because the format stores so little about a function (a `Param` carries
a `u8 decl_type`, never the struct index of a `func f(P p)` annotation;
the reader's descriptor loop never touches `r.structs`). That is an
accident of the current feature set, not a property of the design, and
**a cycle has no traversal order** — the same argument that makes
`build_reachable_reads` a fixpoint rather than a walk.

**CLASSES close the cycle, in more than one place:**

- **method tables** — a class record names its methods
  (class -> descriptor), and a method's descriptor must name its owning
  class for binding / `self` / backtrace naming (descriptor -> class).
  That is the loop on its own;
- **typed params** — `func f(P p)` drops the struct identity today
  because nothing at runtime needs it. Dynamic dispatch or a runtime
  type check makes `Param` carry a real class index: another
  descriptor -> class edge;
- **class-level constants and field defaults** —
  `class C { const F = sq; int n = compute(); }` is this bug's shape
  again, plus defaults that construct OTHER classes;
- **inheritance / mixins** — class -> class, and mutually referring or
  out-of-order classes have no legal section order at all.

Any one of those makes a pair of tables mutually referential. Swapping
now buys one release and re-opens the same hole under a new name.

## The design: identity, then content, then wiring

The format already does the right thing TWICE, at too small a scope —
inside the struct table and inside the descriptor table:

> "The loader constructs all N definitions first, then fills them, so a
> field may reference any of them during the fill."

Generalize that across tables:

1. **IDENTITY.** A table of contents after the string table: the counts
   (ideally the byte offsets too) of every table. Construct EMPTY
   SHELLS for all structs, all descriptors, all classes. Every index in
   the file is now resolvable, because identity exists before any
   content is read.
2. **CONTENT.** Fill the records in whatever order the file has. Any
   cross-reference — struct const -> descriptor, method descriptor ->
   class, class -> base class — resolves against a shell.
3. **WIRING.** Only then, what DERIVES from filled content:
   `compute_layout()`, `compute_bind_flags` (the `fast_bind`
   cross-check), `build_boxed_ops`, the load-time JIT.

Section order stops mattering, which is the actual requirement. It is
what object formats do — a symbol table of identities, references as
indices, patched afterwards — and it survives a feature that has not
been designed yet.

## Constraints to design against

- **Nothing may read a SHELL's fields during phase 2.** There is
  already an instance: `FuncObject`'s constructor reads
  `func->captures.empty()`. A value that materializes an object from a
  not-yet-filled descriptor must store the POINTER only, or that
  construction moves to phase 3.
- **Hostile input (#137).** `Reader::countv` bounds a count by BYTES
  REMAINING, which works because a count sits immediately before its
  own records. A TOC weakens that to a whole-file bound, so each count
  needs a per-record MINIMUM SIZE bound instead
  (`N <= remaining / smallest possible record`), or the shells become
  an unbounded allocation from an attacker-controlled number.
  `vm_verify_program` still bounds every instruction operand
  afterwards, and `tests/myv_fuzz.py` is the net.
- **Derived data stays derived.** `boxed_ops`, `catch_uids`, layouts and
  `bind_req` are rebuilt, never stored (v4's rule). Phase 3 is where
  they belong; do not let the reordering tempt anything into the file.
- **The doc IS the spec.** `docs/myv-format.txt` and
  `MYV_FORMAT_VERSION` (serialize.h, currently **14**) move in the SAME
  commit, and `tests/myv_doc_check.py` is written FROM the doc, so it
  follows too — it must still consume an image to exactly EOF.

## Interim state (what is in the tree now)

- the writer REFUSES a function value inside a struct's const members,
  with the reason in the message (serialize.cpp, `in_struct_consts`);
- `docs/myv-format.txt` says so in both places that matter — the struct
  table's `Const` record and value tag 9 — and points here;
- `tests/driver_checks.sh` pins the refusal, and pins that a function
  value in a CHUNK pool compiles, loads and CALLS correctly.

If a guard is wanted before the restructure lands, `in_struct_consts`
generalizes into a "which tables are readable at this point" mask the
writer asserts against, so the NEXT record that adds a forward
reference fails loudly at write time instead of producing an unreadable
image.

## Acceptance, when it is built

- the program at the top of this file compiles to an image, loads, and
  prints `49`; add that shape to the FAT corpus in `tests/myv_fuzz.py`
  (the `const OPS = [sq]` shape is already there, added when the
  loader's null-context crash was fixed);
- `-vd` of the loaded image is byte-identical to `-vd` of a fresh
  compile (the round-trip oracle);
- `tests/myv_doc_check.py` consumes a fresh image to exactly EOF;
- `myv_fuzz.py` over BOTH a Debug/ASan and an `ASSERTS=OFF` Release
  build: 0 crashes, 0 loader hangs.
