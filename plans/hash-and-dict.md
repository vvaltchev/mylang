# Universal hashing, any-type dict keys, and the dict/ordered_dict question

Status: **planning**. This documents the investigation, an important discrepancy
with the task's premise, the decisions taken, and the implementation phases.

## TL;DR of what I found (read this first)

The task was framed as: "mylang's dict uses `<` comparison and is O(log N);
rename it to `ordered_dict`, and make `dict`/`{}` a real hashmap (O(1)) using
`hash()`." **That premise is factually incorrect for the current code:**

- `DictObject` (`shareddict.h`) is **already** a
  `std::unordered_map<EvalValue, LValue>` — a hashmap, **O(1)** lookups, keyed
  by
  `std::hash<EvalValue>` (→ `EvalValue::hash()` → `Type::hash`) and
  `EvalValue::operator==`. It does **not** use `operator<` at all.
- Verified empirically: filling+reading a dict of N int keys took 0.070s at
  N=200k and 0.640s at N=2M — ~linear in N, i.e. **flat per-operation (O(1))**,
  exactly the hashmap profile (a `std::map` would show a log-N per-op growth).
- `hash(x)` **already exists** as a const builtin (`builtin_hash`,
  `builtins/generic.cpp.h`) and works for the **scalars** `int`/`float`/`str`/
  `bool` (and folds at compile time). `EvalValue::hash()` dispatches to
  `Type::hash`.

So "make the dict a hashmap" is **already done**, and there is **no sorted/tree
dict to rename**. What is genuinely missing — and what this plan delivers — is:

1. **`hash()` for the non-scalar types.** Today `hash([1,2])`, `hash({..})`,
   `hash(Struct(..))`, `hash(none)` all throw `TypeErrorEx` (the base
   `Type::hash` throws). Arrays/dicts/structs therefore **cannot be dict keys**
   (verified: `d[[1,2]] = 5` → "does NOT support hash()"). This is the real,
   high-value gap.
2. **String hashes are recomputed on every lookup.** `TypeStr::hash` does
   `std::hash<string_view>()(view)` every time — O(len) per dict probe. Strings
   are immutable, so this should be cached. Real perf win for string-keyed
   dicts.
3. **Any-type dict keys** — a direct consequence of (1), plus a mutability story
   (below). `EvalValue::operator==` already does structural equality for
   arrays/structs (`Type::eq`), so only `hash` is missing for keys to work.

## The dict / ordered_dict / rename question — needs the user's call

Because the premise is wrong, I am **not** doing the
`dict` → `ordered_dict` rename autonomously: the current `dict` is a hashmap, so
relabeling it "ordered" would be actively misleading, and "making `dict` a
hashmap" is a no-op. Renaming would also be a gratuitous breaking change.

What is *actually* true about ordering: mylang's dict iterates in
**unordered** (hash-bucket) order, whereas **Python's dict is insertion-
ordered**. So the legitimate, useful thing in the neighborhood of the user's
request is a **new, separate `ordered_dict` type that preserves insertion
order** (Python-`dict`/`OrderedDict`-compatible), *added alongside* the existing
hashmap `dict` — not a rename. That needs an insertion-ordered container
(`vector<pair>` + `unordered_map<key,index>`, or a linked hashmap), which is a
sizable feature in its own right.

Recommendation for review:
- **Keep `dict` / `{}` as the hashmap** (it already is; no change, no breakage).
- If insertion-ordered iteration is wanted (for Python parity / reproducible
  output), **add** `ordered_dict()` as a new type in a follow-up — I'll scaffold
  it on request. Its bench would fairly pair against Python's `dict`/
  `OrderedDict`.
- The naming the user settled on (`dict` = hashmap, `ordered_dict` = ordered)
  then holds **without any rename** — `dict` is already the hashmap, and
  `ordered_dict` becomes the new addition.

Everything below (phases 1–4) is independent of this question and is pure upside
(faster string-keyed dicts, any-type keys), so I implement it now.

## Current state — files & mechanics

- `EvalValue::hash()` (`evalvalue.h`) → `type->hash(*this)`;
  `std::hash<EvalValue>`
  is specialized there to call it. `Type::hash` (base, `type.h`) throws.
  Overrides: `TypeInt`/`TypeBool` (bool hashes as the equal int 0/1),
  `TypeFloat` (an integer-valued float hashes as the equal int, so `1`/`1.0`
  share a key), `TypeStr` (recomputed `string_view` hash).
- `DictObject` (`shareddict.h`): `unordered_map<EvalValue, LValue> data` +
  `readonly` + `has_default`/`default_val`. Handle is
  `intrusive_ptr<DictObject>`.
- `TypeDict` (`types/dict.cpp.h`): `subscript` does `data.find(key)`;
  missing-key
  is throw / default / auto-vivify per the for_write rules.
- `SharedStr`/`StrObj` (`sharedstr.h`): `StrObj : RefCounted { std::string s;
  }`,
  immutable; `SharedStr` is a handle + optional `off`/`len` slice view. **No
  cached hash field today** — room to add one on `StrObj`.
- Struct: `TypeStruct::hash` throws ("not hashable, v1", per CLAUDE.md);
  `TypeStruct::eq` is structural (memcmp for POD, field-wise for boxed).

## Design

### Phase 1 — `hash()` for all types, with a smart combiner

Add `Type::hash` overrides; all return `size_t`. The combiner must be **better
than naive xor** (xor cancels duplicates and ignores order). Two combiners in a
new `hashing.h`:

- `hash_combine(seed, h)` — **order-DEPENDENT**: fold `h` into the running
  sequence hash and avalanche the result with the SplitMix64 finalizer
  (`seed = mix(seed + 0x9e3779b97f4a7c15 + h)`; the golden-ratio constant
  decorrelates successive inputs). Used for **sequences**: arrays and struct
  fields (declaration order). Seeded with a per-kind salt so `[1,2]` ≠
  `struct{1,2}` ≠ the int `combine(1,2)`.
- `hash_unordered(acc, h)` — **order-INDEPENDENT** (commutative) with strong
  per-element mixing so it is not "naive xor": `acc += mix(h)` where `mix` is a
  finalizer avalanche (e.g. SplitMix64's `z=(z^(z>>30))*0xbf58…; z=(z^(z>>27))*
  0x94d0…; z^(z>>31)`). Used for **dicts**, whose iteration order is unspecified
  — two equal dicts built in different orders MUST hash equal, so the combine
  must be commutative. (`+` of avalanched hashes, not xor.)

Per type:
- **`TypeArr::hash`** — fold `hash_combine` over element hashes in order (works
  on the flat int/float/bool/struct storage too — read scalars directly, no
  boxing). Salt with a "array" tag.
- **`TypeDict::hash`** — for each `(k,v)`: `pair = hash_combine(seed_pair,
  hash(k)); pair = hash_combine(pair, hash(v))`; accumulate with
  `hash_unordered`. Salt with a "dict" tag. Order-independent ⇒ equal dicts hash
  equal regardless of build order.
- **`TypeStruct::hash`** — fold `hash_combine` over the field hashes in
  declaration order (POD: read each scalar/inline-struct field from the bytes;
  boxed: hash each `LValue`). Salt with the struct def identity (so two distinct
  struct types with identical field values hash differently — matches `eq`,
  which is same-`def`-only).
- **`TypeNone::hash`** — a fixed constant (so `none` is hashable; keep `hash`
  total). *Note:* this changes today's behavior where `hash(none)` throws; the
  test `"hash() of none is unsupported"` will be updated to assert a stable
  value. (A reasonable, deliberate spec change: `none` is a perfectly good key.)

`builtin_hash` already returns `int_type(e.hash())` and is a const builtin, so
it
folds for const args automatically once the overrides exist — no builtin change.

Recursion/cycles: mylang containers are acyclic in practice (no way to build a
cycle without `dyn` ref tricks; arrays/dicts are values/COW). A deliberately
constructed cycle (`var a=[]; append(a,a)` via `dyn`) could infinite-loop the
deep hash. **Decision:** match `to_string`/`==`, which have the same exposure
and are not cycle-guarded today; document the limitation rather than pay a
visited-set cost on every hash. (Revisit if we ever add real ref cycles.)

### Phase 2 — cache the string hash (compute once)

Strings are immutable, so their hash is stable. Add to `StrObj`:
`mutable size_t hash = 0; mutable bool hash_set = false;`. `TypeStr::hash`:
- **full string** (non-slice `SharedStr`): compute lazily on first call, cache
  on
  the `StrObj`, return the cache thereafter. Lazy (not eager-at-creation) so we
  only pay for strings actually hashed (keys); a string never used as a key /
  `hash()` arg costs nothing.
- **slice** (`off`/`len` view): the hash is of the sub-view, not the whole
  `StrObj`, so the `StrObj` cache doesn't apply — compute on demand. Slices as
  keys are uncommon; revisit only if measured.

`mutable` + the immutability of the string makes the lazy cache safe even though
`hash()` is logically const.

### Phase 3 — any-type dict keys (with a sound mutability story)

Once `hash` is total, `d[[1,2]] = v`, struct keys, etc. just work (equality is
already structural). The hazard: a **mutable** key mutated after insert would
change its hash and corrupt the map. How mylang avoids it:

- **COW already protects the stored key.** The dict stores the key's `EvalValue`
  (holding the COW handle). After `d[arr]=v`, the dict holds a reference, so
  `use_count > 1`; a later `arr[0]=x` triggers copy-on-write — it **clones**
  `arr`, leaving the dict's stored key (and its hash) unchanged. So mutating the
  original after insertion is already safe.
- **Freeze the stored key to be extra-safe and self-documenting.** On insert,
  store the key as a **deep read-only** snapshot (`make_const_clone` of the key)
  so the stored key can never be mutated through *any* alias, guaranteeing its
  hash is permanently valid. Scalars/strings are returned as-is (already
  immutable); only containers pay a one-time freeze clone. This is the
  Python-"keys must be immutable" guarantee, achieved by freezing rather than
  rejecting.

Decision: **freeze container keys on insert** (sound, ergonomic, no new error
surface). Lookups hash/`==` the probe against the frozen stored keys normally.
`get`/`get!`/`erase`/`d.k` member access all route through the same key path.

### Phase 4 — benchmarks

- Add dict benches with **string** keys and **struct/array** keys (new
  capability), paired with Python, to show the string-hash-cache win and the
  any-type-key parity. Keep the existing int-key dict benches.
- Reconfirm no regression on the int-key path (the hot one).

### Deferred / proposed (need the user)

- **`ordered_dict` (insertion-ordered) as a NEW type** — the legitimate version
  of the "ordered" request; a follow-up, not a rename. Would pair fairly against
  Python's `dict`/`OrderedDict` in benches.
- **Incremental O(1) container hash** (the user's "nice to have"): maintain a
  running hash updated on each `append`/`insert`/`erase` so `hash(bigarray)` is
  O(1). Feasible **only for a commutative combine** (add on insert, subtract on
  remove — works for the dict's `hash_unordered`/`+` and would work for arrays
  *if* we used a commutative array combine). But arrays are **ordered**, and an
  order-dependent `hash_combine` is **not** invertible/updatable on a middle
  insert/erase (the positions after it all shift). So: an O(1) incremental hash
  is possible for **dicts** (commutative) and for **append-only** arrays, but
  not
  for general array insert/erase. Given arrays are the common case and
  `hash(array)` is O(n) anyway (same as `==`/`to_string`), **defer** — note it
  as a dict-only optimization if it ever shows up in a profile.

## Decisions & trade-offs (summary)

- **No rename.** The dict is already a hashmap; relabeling it "ordered" is
  wrong.
  Keep `dict`/`{}`. Surface the premise error; propose `ordered_dict` as a new
  type if insertion order is wanted.
- **Smarter-than-xor combiners:** a SplitMix64-avalanche fold `hash_combine`
  (ordered: arrays, structs) and a SplitMix64-avalanche commutative sum
  (unordered: dicts). Per-kind salts so different shapes with the same leaves
  don't collide.
- **`none` becomes hashable** (a deliberate, documented spec change; it's a fine
  key). Updates one existing "unsupported" test.
- **String hash cached lazily on `StrObj`** (immutable ⇒ safe); slices compute
  on
  demand.
- **Container keys are frozen (deep read-only) on insert** — Python's "immutable
  keys" guarantee via freezing, not rejection; COW already prevents most
  corruption, freezing closes the rest. One-time clone cost per container key.
- **No cycle guard** in deep hash (matches `==`/`to_string`); documented.
- **Incremental O(1) hash deferred** (only sound for commutative combines;
  arrays
  are ordered).

## Phase order for implementation

1. `hashing.h` (combiners) + `Type::hash` for array/dict/struct/none + tests.
2. String hash cache on `StrObj` + tests.
3. Any-type keys: freeze-on-insert in `TypeDict` + tests (int/str/array/struct/
   dict/none keys; mutate-after-insert safety;
   equal-keys-different-build-order).
4. Benches (string-key, struct-key) + verify_semantics parity + matrix.
5. README + CLAUDE.md kept in sync each phase.
