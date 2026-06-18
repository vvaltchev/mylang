# Flat typed arrays (`array<int>` / `array<float>` unboxed storage)

Status: **M1 (flat int arrays) DONE.** A large, value-model-level optimization
enabled by the type inferencer. Read this in full before continuing.

**Chosen design: approach B (tagged storage in `SharedObject`), NOT approach A
(separate `t_int_arr`/`t_float_arr` types).** The original draft below the
"Data model (approach A — rejected)" heading is kept for the rationale; the live
design is "Data model (approach B — chosen)". Approach B is far less invasive:
one array type (`t_arr`), one value handle (`SharedArrayObj`), no new `TypeE`
entries, no `ValueU` members, no second COW/slice implementation. The storage
*kind* is an internal detail of `SharedObject`; every operation that doesn't have
a flat fast path transparently `promote_to_general()`s and reuses the existing
`vector<LValue>` code. That capped the M1 change to ~a dozen call sites.

## Verdict

**Possible, and type inference is exactly what makes it sound.** Today an array
is a `vector<LValue>` with **48-byte** element slots (`EvalValue` 32 + the
`container`/`container_idx`/`is_const` COW back-pointer fields). Bulk operations
(`reverse`, `sort`, `sum`, `foreach`, subscript loops) are memory-bandwidth
bound on that slot size — the root cause of `21_array_reverse` etc. being ~6×
Python (see the perf analysis: Python moves 8-byte pointers, we move 48-byte
slots).

If an array is **provably homogeneous int or float**, we can store it as a flat
`vector<int_type>` / `vector<float_type>` — **8-byte** slots, contiguous, no
boxing, no per-element COW back-pointers. Then `reverse`/`sort`/`sum`/`foreach`
move 6× less memory and need no per-element `EvalValue` machinery. For `sum` and
arithmetic loops we'd likely **beat CPython** (raw `int64` vs CPython's boxed
`PyObject*` + small-int deref). This is the payoff M8 set up for scalars,
extended to arrays.

**Why it needs type inference:** the flat representation is usable only when we
*know* every element is (and stays) int/float. Before, there was no such
proof; now `infer_types` proves `array<int>` / `array<float>` whole-program, so
we can pick the representation safely and reject the operations that would break
it at compile time.

## Data model (approach B — chosen)

No new types. `SharedArrayObj`'s pointee `SharedObject` gains a `Storage kind`
discriminator and an **anonymous union** of the three backing vectors
(`sharedarray.h`):

```cpp
enum class Storage : unsigned char { general, ints, floats };
struct SharedObject final : RefCounted {
    Storage kind;
    union {
        vec_type  vec;     // kind == general : vector<LValue>  (48-byte slots)
        ivec_type ivec;    // kind == ints    : vector<int_type>   (8-byte)
        fvec_type fvec;    // kind == floats  : vector<float_type> (8-byte)
    };
    std::unordered_set<SharedArrayObjTempl *> slices;
    bool readonly = false;
    // manual placement-new ctors per kind + a kind-switching dtor (a union
    // can't run its members' non-trivial ctors/dtors).
};
```

The `off`/`len`/`slice` slice-view fields and the whole COW/slice machinery are
**unchanged** — a slice shares the parent `shobj` regardless of kind, so slicing
a flat array is still a cheap view that stays flat. `size()` is kind-aware (reads
the live vector's `size()`); `skind()`, `flat_ints()`, `flat_floats()` expose the
unboxed vectors to the hot paths.

**The promotion hook.** `get_vec()` (the general `vector<LValue>&` accessor) calls
`promote_to_general()` on first use: it converts the flat vector to
`vector<LValue>` in place (destroy the flat union member, placement-new the
general one), value-preserving, so it is sound even when the object is shared or
sliced. Every operation that lacks a flat fast path calls `get_vec()` and so
**just works** (paying a one-time O(N) conversion). The const overload promotes
too — promotion preserves the value, only the representation changes.

### Data model (approach A — rejected; kept for rationale)

The original plan added two **new** runtime types alongside `t_arr`, with a
separate `FlatArrayObj<T>` value handle:

- `t_int_arr` — value handle `FlatArrayObj<int_type>`.
- `t_float_arr` — value handle `FlatArrayObj<float_type>`.

Both **non-trivial** (`>= t_str`), appended after `t_dict` in `TypeE`.

**Union placement (important — the `FlatVal` wrapper).** A `union` member cannot
have non-trivial ctors/dtors, so every non-trivial `ValueU` member is wrapped in
`FlatVal<T>` (an `alignas(T) char[sizeof(T)]` buffer; see `flatval.h`), and the
lifecycle (ctor/dtor/copy/move) is driven by type-erased ops
(`TypeErasureOps`/`TypeImpl<T>`) via reinterpret-cast, not the union. So the
new members mirror the existing `FlatVal<SharedArrayObj> arr;` /
`FlatVal<intrusive_ptr<DictObject>> dict;` exactly:

```cpp
// in ValueU (evalvalue.h), alongside the other non-trivial members:
FlatVal<FlatArrayObj<int_type>>   iarr;
FlatVal<FlatArrayObj<float_type>> farr;
```

Wiring (same recipe as any value type): `TypeE` enum + `TypeNames` + `AllTypes`
(`types.cpp`); `TypeToEnum<FlatArrayObj<int_type>>` /
`TypeToEnum<FlatArrayObj<float_type>>` + the `FlatVal` `ValueU` members above
(`evalvalue.h`); a `TypeIntArr : TypeImpl<FlatArrayObj<int_type>>` /
`TypeFloatArr : TypeImpl<FlatArrayObj<float_type>>` class (`src/types/`) which
inherits the type-erased lifecycle ops from `TypeImpl<T>` (so `FlatArrayObj`
just needs the usual value-type ctors/dtor/copy/move, like `SharedArrayObj`).
The handle is ~24 bytes (`intrusive_ptr` + `off`/`len`/`slice`), so `EvalValue`
stays 32 bytes and `LValue` stays 48 — no size regression.

### `FlatArrayObj<T>` — a new container (NOT the existing template)

`SharedArrayObjTempl<LValue>` cannot be reused with `T = int_type`: its
COW/slice machinery (`clone_aliased_slices`, the per-element `container`
back-pointers, `get_value_for_put`) assumes elements are `LValue`s. A flat array
needs none of that per element — COW is at the *array* level only. So a new,
simpler class:

```cpp
template <class T>            // T = int_type | float_type
class FlatArrayObj {
    struct Obj : RefCounted {            // intrusive_ptr pointee
        std::vector<T> vec;
        std::unordered_set<FlatArrayObj *> slices;   // live slice views
        bool readonly = false;                       // backs `const`
    };
    intrusive_ptr<Obj> o;
    size_type off, len; bool slice;      // slice view, like SharedArrayObj
    // value-level COW: clone the vec on a write when shared / sliced.
};
```

This reuses the *array-level* COW + slice design of `SharedArrayObj` (cheap to
copy, lazy slices) but with raw `T` storage and no per-element bookkeeping.

### `LValue` element references into a flat array

`a[i]` as an lvalue must point at `vec[i]`. For a `vector<LValue>` it returns an
`LValue*`; for a flat array, `vec[i]` is a raw `T`, not an `LValue`. Per the
proposal, generalize the element back-reference: instead of `LValue *container`,
the element lvalue records *(which array, which index, which backing kind)*. The
kind tag rides in the padding by `is_const` (free bits). The two write paths
then diverge in `LValue::get_value_for_put`/`put` (`eval.cpp`):

- general (`vector<LValue>`): today's path (COW the `SharedObject`, write the
  `EvalValue` slot).
- flat: COW the `FlatArrayObj` if shared/sliced, then `vec[idx] = v.get<T>()`
  (extract the scalar, store raw). A read yields `EvalValue(vec[idx])`.

`a[i]` as an **rvalue** is already fast via the M8 `Subscript::eval_int` path;
that just gains a flat-array branch that reads `vec[off+idx]` directly (it
already special-cases `SharedArrayObj`).

## The four hard problems (and how to solve them)

### 1. Representation routing: static type -> runtime value

A value's representation (flat vs general) is dictated by its **static type**,
known at compile time, but a value is *created* at runtime by a literal,
`range()`, `array()`, a function return, etc. The creator must produce the flat
form when the inferred type is `array<int>`/`array<float>`.

Solution: extend the M8 machinery. `TypeHint` already tags nodes int/float; add
`ai`/`af` (array of int/float). The **specializer** rewrites array-producing
nodes in a typed-array context to a flat-producing form:
- `[1,2,3]` (LiteralArray, all-int) -> build a `FlatArrayObj<int_type>`.
- `range(N)` -> flat int directly (it already produces 0..N-1).
- `array(N)` in an `array<int>` context -> flat int (see §3 for the value).
- a folded const int array (`LiteralObj`) -> materialize flat.
Representation is consistent because the type is: a var/param/return has one
inferred type, so every value that flows into it has the same representation
(the type system rejects mixing int and dyn into the same slot, or makes it
`array<dyn>` = general).

### 2. The `dyn` boundary + promote-to-general fallback (the surface-area tamer)

A flat array can flow into a `dyn` context (`dyn` var, `print`, `dyn` param),
where static type is lost. Two rules keep this sound *without* reimplementing
every array operation for flat types:

- **Reads work on flat directly**: `to_string`/`print`, subscript-get, `len`,
  `foreach`, `==`, `sum`, `sort`/`reverse` (produce flat), `+` (flat+flat ->
  flat) — implement these natively on the flat types (the hot set).
- **Anything else PROMOTES**: any operation not in the native set, and any
  element-type-violating mutation only reachable via `dyn` (e.g. `append(x,"s")`
  on a `dyn`-typed flat array, or `a[i]=str`), first calls `promote()` —
  convert the `FlatArrayObj<T>` to a general `SharedArrayObj` (vector<LValue>) —
  then dispatches the normal general-array code. Promotion is O(N) but only on
  cold/escape paths (the type system forbids type-violating ops in non-`dyn`
  code, so promotion is rare).

This **bounds the work**: we only hand-write the *hot* operations for flat
arrays; correctness for the long tail comes from promote-then-reuse. It also
makes the feature incrementally shippable (add native ops one at a time; until
then they promote).

### 3. `array(N)` semantics: `none` vs `0`

`var a = array(100000)` makes N `none`s today; reading an unfilled slot yields
`none`. A flat int array cannot hold `none`. The inferencer already types
`array(N)`-then-fill as `array<int>` (its `join_elem` rule treats the initial
`none` as "uninitialized -> will be int"), so in a typed-int context the flat
`array(N)` must default elements to **`0`** (resp. `0.0`). This is an observable
change (`var a=array(3); print(a[1])` would print `0`, not `none`) — but it is
*more* sound: the type system already claims `a[1] : int`, and `0` honors that
where `none` contradicts it. Decision to confirm with the user: accept the
`0`-default for flat typed arrays (only when the inferred element type is
int/float; `dyn`/object arrays keep `none`).

### 4. Everything that touches arrays must learn the new types

The type-erased lifecycle ops (`TypeErasureOps`: copy/move/dtor via
intrusive_ptr) — trivial, same as `t_dict`. The operation surface: `TypeIntArr`/
`TypeFloatArr` virtuals (`subscript`, `slice`, `len`, `add`, `eq`/`lt`/…,
`clone`, `hash`, `to_string`, `is_true`, `use_count`, `intptr`, `is_slice`) and
the array builtins (`append`/`push`/`pop`/`insert`/`erase`/`sum`/`sort`/
`rev_sort`/`reverse`/`map`/`filter`/`find`/`top`). With the promote fallback,
only the hot subset needs a native flat implementation up front.

## Expected win (where it pays)

Big on **homogeneous int/float arrays** doing bulk work: `reverse`/`sort` ~6×
less memory traffic; `sum`/`foreach`/arithmetic loops become raw-`int64` (likely
faster than CPython); subscript-heavy numeric loops (sieve, matrix_mult) get
contiguous 8-byte access + the M8 typed read. **No change** for heterogeneous /
object / `dyn` arrays (they stay general). So it targets exactly the numeric
workloads, complementing M8.

## Milestones (phased; measure after each)

- **M1 — int arrays. DONE.** Approach B: `SharedObject` gains `Storage kind` + the
  anonymous union + `promote_to_general()` (M1a); `range()` produces flat int
  storage (M1b); the hot ops read the unboxed vector directly (M1c): `sum`,
  `reverse`, `sort` (no-comparator), `foreach`, `Subscript::eval_int`/`eval_float`,
  `TypeArr::add` (flat+flat concat), and `clone_internal_vec` (clone/COW keep
  flat). **Crucially, the const path keeps flat:** `make_const_clone` /
  `clone_to_mutable` bake/copy flat int/float arrays without promoting, and the
  inferencer's `sty_from_value` reads the element type from `skind()` instead of
  `get_view()` — *that get_view() was silently promoting every const array during
  type inference and defeating the whole feature.* Everything else promotes.
  **Measured:** `17_array_concat` 2.7x→0.21x, `21_array_reverse` 1.1x→0.37x,
  `18_foreach_array` 1.0x→0.51x, `36_sum_builtin` 0.25x; geomean 0.69x→0.63x.
- **M2 — float arrays.** The storage, union, accessors, promotion, and every flat
  branch already handle `Storage::floats` — but *nothing creates a flat float
  array yet* (range is int-only; `array(N)` is general). Needs a flat-float
  producer (e.g. an all-float `[1.0, 2.0]` literal, or `array(N)` in a float
  context) to exercise it. No current benchmark builds a float array, so this is
  unmeasured until M3.
- **M3 — `array(N)` typed creation** (`0`/`0.0`-default, §3) so `array<int>`/
  `array<float>` allocations are flat, **+ the flat subscript-*store* path**
  (`a[i] = int` writes `flat_ints()[i]` directly; a non-matching element type
  promotes) + mutating builtins on flat (`append`/`insert`/`pop`/`erase`) +
  `map`/`filter`/`find`/`min`/`max`. This is what unlocks the `array(N)`-based
  benchmarks (`14_array_subscript`, `33_sort_ints`, `38_min_max`) — they stay
  general today because `array(N)` is general.
- **M4 — promotion polish + dyn-boundary audit**: confirm every general-array
  site is either flat-aware or reached only after `promote_to_general()`; fuzz the
  dyn-escape paths (a flat array flowing into `dyn`/`print`/`append("s")`).
- **M5 — docs + the full `-rt` suite green + bench**. README (only if behavior
  changes — M3's `array(N)` `0`-default does), CLAUDE.md (the storage kind, the
  routing, promotion). *M1 already updated CLAUDE.md's COW section and this plan.*

## Risks / why this is big

- **Surface area**: arrays are touched by ~15 builtins + a dozen Type virtuals +
  the COW core; the promote fallback caps the *native* surface but every
  general-array site must be reachable-after-promote or flat-aware.
- **The value model is the most delicate, perf-critical, most-tested code.** A
  third array representation is a permanent maintenance cost and a regression
  risk for the existing fast paths.
- **The `dyn` boundary is a soundness surface** (a flat array must not silently
  accept a non-`T` element). Mitigated by compile-time checks (non-`dyn`) +
  promote-on-violation (`dyn`).
- **`array(N)` `0`-default** is an observable behavior change (bounded to typed
  int/float arrays).
- **`intptr()`/COW tests** assert exact sharing; the flat path must preserve the
  same alias/slice semantics (its own tests).

## Alternative considered (and rejected)

Keeping `vector<LValue>` but adding a runtime "all-int" flag and a faster
`reverse`/`sort` for it does **not** help: the elements are still 48-byte slots,
so the memory traffic — the actual cost — is unchanged. Only changing the
*storage* (flat `vector<T>`) moves the needle, which is the user's proposal.
