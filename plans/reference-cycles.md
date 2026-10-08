# Reference cycles - TODO (maintainer, 2026-10-08)

**Status: OPEN, NOT DESIGNED.** This file collects the problem, the facts
measured so far and the options to evaluate. It is not a plan yet: the
maintainer wants this thought through carefully before anything is built,
because reclaiming cycles is not a local change. It must be decided before
`class` is finished (plans/class-and-box.md, O4).

## The problem

MyLang frees memory by reference counting: every heap value is an
`intrusive_ptr` to a `RefCounted` object, freed when its count reaches
zero. A group of objects that reference each other in a ring keeps every
count above zero after the program drops its last outside reference, so
it is never freed.

**Cycles exist today, without classes.** Arrays and dicts are reference
types, so a program can already build one:

    var dyn a = [1];
    append(a, a);               # a holds itself
    var dyn d = {"k": 1};
    d["self"] = d;              # so does d

and so can a closure that captures a container and is then stored into
it (`var fs = []; var f = func [fs] () { ... }; append(fs, f);` - a
capture is a copy, but the copy of an array is the same array).
LeakSanitizer reports both on a debug build (probe, 2026-10-08). Nothing
in the corpus builds one, which is why no net has ever reported it - and
`tests/int`'s object census would report one as a `census LEAK` today.

`class` and `box<struct>` make a cycle the ORDINARY way to write a parent
pointer, a doubly linked list, a graph, an observer list. So the question
moves from "a corner nobody writes" to "every second data structure".

## Two problems, not one

**A. TRAVERSAL - a walk over a value graph must terminate.** Printing,
`==`, `hash`, `deepclone`, freezing a constant or a dict key, typing a
constant's value, writing a `.myv`: each recursed without a guard, so a
cyclic value crashed the interpreter with a stack overflow (a SIGSEGV in
a release build). That is a RULE 1 bug, not a design question, and it is
being fixed now - see *Part A* below.

**B. RECLAMATION - an unreachable cycle must be freed.** This is the open
design problem, and the rest of this file is about it.

## What any answer must preserve

- **RULE 1 and RULE 2.** A collector may change how much memory a
  program has, never what it prints. That is easy only while MyLang has
  NO FINALIZERS: nothing runs when an object dies, so the moment of its
  death is unobservable. Keep it that way - a finalizer (`__del__`, a
  destructor method) would make the collector's schedule part of the
  language and every engine would have to run it identically.
- **The fast paths.** The JIT retains and releases inline; the VM's frame
  release scan, the borrow bind (#94), the frameless windows and the
  pooled allocator are all built on "a count change is a plain `++`/`--`".
  A scheme that needs a write barrier on every store is a different
  value model and a large regression.
- **Exactness of the leak detectors.** LeakSanitizer cannot see pooled
  memory; the INT object census can, and it must keep telling a real
  leak from a not-yet-collected cycle (or a collection must run before
  the census is read).

## Options to evaluate

1. **A `weak` reference modifier** (Swift `weak`, Rust `Weak`, C++
   `weak_ptr`). The program breaks the cycle itself: a `weak opt Node
   parent` field does not count, and reads `none` once its object died.
   + deterministic, no collector, nothing to tune;
   - a language surface every program with a back pointer must learn,
     and it does not help the array/dict cycles above at all;
   - an object needs a weak count and a "dead but not freed" state, so
     `intrusive_ptr`/`RefCounted` grow (today: one 32-bit count).
2. **A cycle collector by TRIAL DELETION** (Bacon & Rajan 2001, the
   scheme CPython's `gc` module uses). Track the objects that can hold
   references (general arrays, dicts, struct/class objects, closures
   with captures); for a candidate set, subtract every count contributed
   by an edge INSIDE the set; an object left at zero has no outside
   holder, and neither has anything reachable only from it.
   + invisible to programs (with no finalizers), covers every kind;
   + it needs NO ROOT SCAN: a reference held by a frame slot, a JIT
     register or a C++ local shows up as a count no internal edge
     explains. That is what makes it plausible next to a JIT;
   - every holder that holds a reference WITHOUT counting it looks like
     no holder at all, and a collection that ran while one was live
     would free a reachable object. The known uncounted holders must be
     enumerated and proven covered (see below);
   - every object kind needs a "visit my references" function, and a
     kind that forgets one is a silent use-after-free - an audit table
     in the CLAUDE.md sense, so it needs the opcode-census treatment (a
     test derived from the TYPE enum, not from the table);
   - when to run it: an allocation threshold (CPython's generations), a
     `gc()` builtin, at exit only for the census? Each is a RULE 2
     question only if something observable depends on it - which, with
     no finalizers, nothing does.
3. **A tracing collector as a backup** (mark from roots). Needs every
   root, including the JIT's pinned registers, at every collection
   point - the hardest option next to a register allocator, listed only
   to be rejected with a reason.
4. **Forbid cycles** (a cycle-creating store raises). Checking a store
   for a path back to the container is a graph walk per store, and it
   would refuse ordinary data structures. Listed for completeness.

## Uncounted holders a collector would have to account for

Each of these holds a reference with no count of its own, safe today
because something else holds the count for its whole lifetime:

- the BORROW BIND (#94, `LValue::borrowed`) - the caller's slot holds the
  count for the call;
- the frameless window's W5 inline borrows and the W3 uninitialised
  slots (poisoned in a TESTS build);
- `vm_elem2_borrow_row` (a row borrowed for one instruction);
- the foreach struct reuse (`vm_struct_elem_into` overwrites in place
  when `use_count() == 1`);
- a `SharedObject::slices` set (raw pointers to slice HANDLES, not
  references - but a collector walking an array must not follow it);
- anything the JIT copies raw between a load and a store inside one op.

The question for each is whether a collection can run while it is live.
If collections run only at well-defined points (an allocation in a C++
helper, never in emitted code; between statements), most are out of
reach by construction - which is the argument to make, and to pin with a
stress mode (`MYLANG_GC_EVERY=N`, the RECYCLE=1 idea for the collector)
that collects at EVERY such point.

## Where to start, when this is picked up

Measure first: build the corpus of cyclic programs (the three shapes
above, a doubly linked list of classes, a graph with parent pointers, a
closure stored into the array it captures), with an INT-census check
that each frees everything. That corpus is the oracle for whichever
option is chosen. Recommendation, not a decision: option 2, invisible to
programs, with option 1 left open as a later expressiveness feature.

## Part A - the traversal fix (2026-10-08) - DONE

**Status: DONE (2026-10-08).** Every walk below goes through
`src/cyclewalk.h`; CLAUDE.md makes that a rule for new walks. The
deviations from the text below, and why, follow it.

Every recursive walk over a value graph terminates on a cycle. The
semantics (README, *Values that contain themselves*):

- **printing** (`print`, `str`, the REPL echo) renders a container that
  is already being printed as `[...]`, `{...}` or `Name(...)`, as Python
  does;
- **`==`** compares two cyclic values by SHAPE: when the walk meets a
  container it is already comparing, the other side must be that
  container's partner in the same comparison, else the values differ.
  `a == a` is true; two independently built rings of the same shape are
  equal; a ring and its one-step unrolling are not;
- **`hash`** is consistent with that `==` (a back edge hashes as its
  depth on the walk), so a cyclic value can be a dict key;
- **`deepclone`**, freezing a `const` or a dict key, and every other
  deep copy reproduce the cycle in the copy instead of following it
  forever. A value shared at two places that is NOT on a cycle is still
  copied twice, as before;
- the compiler's walks over a constant's value (its static type, the
  callee-set analysis, the dead-template closure) stop at a container
  they are already inside;
- a `.myv` image cannot store a constant that holds a cycle: `-c`
  refuses it with a compile error naming the constant (the format has no
  back reference; adding one is part of B's design, not A's).

### As built - and where it differs

- **`==`'s rule, precisely.** A pair stack of the (lhs, rhs) containers
  being compared. A pair is a back edge when its lhs is on the stack as an
  lhs (depth i) or its rhs as an rhs (depth j); the two are equal iff
  i == j (a side not on the stack counts as -1). Consequences, all pinned:
  each node of a ring of two equals the other; rings of one, two and three
  nodes all differ; `clone(a) != a` for a ring `a`.
- **The identity fast path is restricted.** `x == x` without entering `x`
  stays for the top level (every pair on the stack an identity pair) and
  for a container that reaches no cycle; anywhere else the shortcut would
  make the answer depend on where the walk started - `[a] == [clone(a)]`
  could then be true while `a == clone(a)` is false - and `==` would not
  be transitive, and no hash could agree with it. Every acyclic comparison
  keeps the shortcut exactly - NaN included: with `n = [nan, "x"]`,
  `n == n`, `[n] == [n]` and `[n, 1] == [n, 1]` are still true and
  `n == clone(n)` still false, old and new binaries alike - at the price
  of one memoized reachability search per container per comparison once a
  non-identity pair is on the stack.
- **A dict compares values in the old operand order** (`CycFlip`): libstdc++'s
  `unordered_map::operator==` compared the OTHER dict's value on the left,
  and a builtin exception value has no `eq`, so the order is observable.
- **Ordering does not apply**: arrays, dicts and structs have no `<`
  (`TypeErrorEx`), so nothing recursive needed a rule.
- **Frozen cycles are freed at exit (an extension A needed).** A cyclic
  constant, a cyclic dict key, and a cyclic value the compiler bakes or
  drops while folding can be broken by no program, so LeakSanitizer and
  the INT census would report every program that builds one. They are
  kept by `cyc_keep_until_exit` and emptied by an atexit drain that runs
  before both detectors read the heap. A cycle the PROGRAM built is never
  kept: it is reported, and tests break theirs. Part B replaces this.
- **The mutable bake of a folded value is a deep copy now.**
  `MakeConstructFromConstVal` baked a `var`-bound folded container with a
  SHALLOW `clone()`, which turned a ring into its one-step unrolling (a
  fresh top in front of the original): a folded `var r = mk();` and the
  same call at run time then compared and printed differently (RULE 2).
  It is `make_mutable_clone`, which reproduces the cycle.
- **The `.myv` refusal names where the constant is, not its name**: "a
  constant at the top level", "a constant in function 'f'", "a constant in
  a lambda", "the constant 'S.K'". A chunk's constant pool has no names; a
  struct's const members do. The format is unchanged (no back-reference
  record); docs/myv-format.txt section 11 says a value is a tree.
- **The compiler's walks** answer a back edge with their TOP:
  `static_type_from_value` / `reflect_typeof` -> `dyn`, `cs_eval_value` ->
  the allocation site's abstract object (whose contents it already
  models), `shape_of` -> `NumShape::other`; `keep_in_value` and
  `collect_value_descs` simply stop. `is_readonly_value`, `value_repr`,
  `cse_key`, `make_general_array_clone` and `join` were checked and are
  not recursive over values.
- **A cyclic constant through a pure function compiles and runs in every
  engine** (`-tw`, `-nj`, the JIT, `-nbi`, `-nc`, `--no-opt all`, `-nti`).
  What it needs is an element store: `append` is a run-time builtin and is
  undefined inside a pure function at compile time.
