# Type-driven specialization + mandatory `dyn`

Status: **planning.** Large, multi-phase. The end goal is to make array (and
later, scalar) representation choices driven by the *static type the inferencer
proved*, not by the runtime value. That is only sound once the inferencer is
**provably correct in both directions**, so the bulk of the work is hardening
and auditing type inference — gated behind a hard rule that forces the issue.

This supersedes the "deferred type-driven representation" note in
`plans/typed-arrays.md` and the `[[typed-arrays-track]]` memo.

## The end goal (why)

Today array storage is **value-driven** (`plans/typed-arrays.md`, approach B):
`range()`/`array(N,v)`/`make_array` pick flat vs general from the runtime value,
and a flat array that turns out polymorphic just *promotes* (an O(N) cost).
The waste: we specialize arrays we never proved monomorphic, then pay to undo
it.

The fix is **type-driven** representation: emit flat storage **iff** the
inferencer proved `array<int>`/`array<float>` for that value's destination; emit
general for `array<dyn>`. A statically-`array<int>` value can never receive a
non-int element (that's a compile error), so it never promotes — specialization
becomes a pure win, promotion becomes a `dyn`-only escape hatch.

This is only correct if inference is **sound** (never claims `int` where the
real type is `dyn`) **and** we trust it enough to act on it. Hence the rule.

## The forcing rule: `dyn` becomes mandatory

A variable declared with plain `var` (and `const`) **must infer to a fully
concrete static type**. If its inferred type is `dyn` (or contains `dyn` —
pending the array decision below), that is a **compile error**: the user must
declare it `dyn` / `var dyn` to opt into dynamic typing explicitly. There is no
implicit `dyn` anymore.

- Sound concrete types acceptable under plain `var`: `int`, `float`, `str`,
  `opt T` (nullable is a known type), `array<T>`, `dict<K,V>`, function types,
  `Exception`, and (pending decision) `array<dyn>`/`dict<...dyn...>`.
- `dyn` (the top type) anywhere that the rule covers → error → requires `dyn`.
- The error is a compile-time `Exception` (not a `RuntimeException`), like the
  other inferencer diagnostics (`errors.h`), so `try/catch` can't catch it.
- Gated by `-nti` (no inference → no enforcement), on by default otherwise.

This rule makes case (1) below *enforced by construction*: if inference ever
spuriously concludes `dyn`, the build breaks loudly instead of silently
de-optimizing.

## Two correctness properties the inferencer must satisfy

1. **Soundness (no false fix):** never assign a concrete type when a use case
   would force `dyn`. The forcing rule *enforces* this — a wrong `int` that
   should be `dyn` would mis-accept code that must be rejected, surfacing as a
   real type error somewhere. (Already largely true; the audit confirms it.)

2. **Completeness (no false `dyn`):** never infer `dyn` (or `array<dyn>`) when a
   concrete type was provable. This is *not* enforced by anything today — a
   spuriously-`dyn` identifier just silently misses optimization (and, under the
   forcing rule, forces the user to write a needless `dyn`). We need an audit
   tool to find these.

## `--debug-ti`: the audit tool

A new CLI option that runs inference and dumps every identifier's inferred
type + every use site, machine-readable, so we can find and triage `dyn`s.

Proposed format (one record per declared identifier; tab- or `|`-separated, or
JSONL — final format TBD during implementation):

```
name | kind(const|var|param|func) | decl_line | decl_col | isconst | type | uses
```
where `type` renders the full static type (`int`, `opt float`, `array<int>`,
`array<dyn>`, `dict<str,dyn>`, `func(int)->int`, `dyn`, ...) and `uses` is a
list of `line:col` read/write sites (invert the inferencer's `id_sym`
Identifier→TypeSym map; each `Identifier` carries its `start` loc).

Implementation: hang it off `infer_types` (the inferencer already builds scopes,
one `TypeSym` per decl, `id_sym`, and `FuncInfo` per function). A flag threads
in and, after the fixpoint, walks the symbol tables and prints. `-nr`-style:
dump, don't run.

## The audit workflow (the long part)

1. Run `--debug-ti` over **every** script: `src/tests.cpp` entries (extract or a
   harness), `bench/my/*.my`, `samples/*`.
2. Collect every identifier whose `type` is `dyn` or contains `dyn`
   (`array<dyn>`, `dict<_,dyn>`, ...).
3. For each, read its use sites and decide: **justified** (genuinely
   polymorphic — e.g. holds an int then a str, fed by `runtime()`, a real
   heterogeneous array) or a **false `dyn`** (an inference gap).
   - Justified → annotate the script with `dyn` (samples/tests/bench may need
     this; the user expects it).
   - False `dyn` → debug and **fix the inferencer**, add a regression test.
4. Re-run until every remaining `dyn` is justified, then turn the forcing rule
   on and make the whole corpus pass.

## Phasing (split allowed)

- **Phase A — scalars + function return types.** Make inference 100% correct for
  non-array identifiers and function returns; *tolerate* `array<dyn>` (don't
  enforce the rule for array element types yet). Land `--debug-ti`, the forcing
  rule for scalar `dyn`, and the audit of all non-array `dyn`s.
- **Phase B — arrays.** Extend the rule + audit to array element types; fix the
  array inference gaps (the known one: a mixed `append`/store widens
  `array<int>`→`array<dyn>` via `contribute_container`; element-type checking
  vs. widening). Then flip array representation to type-driven (below).

## Type-driven representation mechanics (the payoff, Phase B end)

Once `array<int>`/`array<float>` is trustworthy, route representation off it:
- The specializer (`specialize_types`, post-`resolve_names`) stamps array-
  producing nodes (`range()`, `array(N[,v])`, `make_array`, array literals,
  folded const arrays) with a representation hint derived from the *inferred
  type at the destination* (extend `TypeHint` with array kinds, or a small new
  hint field), and the creator emits flat only when the hint says int/float.
- A value flowing into a `dyn`/`array<dyn>` slot is created general from the
  start — no specialize-then-promote. `range()` (intrinsically `array<int>`)
  stays flat unless its destination is `dyn`.
- Promotion remains only as the `dyn`-escape safety net.

## Resolved decisions (confirmed with the user)

1. **`array<dyn>` under plain `var` → STRICT.** Any `dyn` *anywhere* in the
   inferred type (`array<dyn>`, `dict<_,dyn>`, nested, or a bare `dyn`) makes a
   plain `var`/`const` an error; the user must declare it `dyn`/`var dyn`. So
   every plain-`var` array is guaranteed monomorphic `array<T>` (flat-eligible).
2. **Enforcement = HARD compile error, on by default**, disabled by `-nti`.
   `opt T`/`array<T>`/`dict<K,V>`/etc. (any fully-concrete type) are fine; only a
   `dyn` somewhere in the type triggers it. The error is an uncatchable
   compile-time `Exception` (like the other inferencer diagnostics). The corpus
   (tests/bench/samples) gets `dyn` added where genuinely needed.
3. **Function returns:** no return-type annotation syntax. A function whose
   return is genuinely `dyn` is handled at the *caller's* `var` (which then needs
   `dyn`); functions themselves get no new annotation.

## Implementation sequencing

Build the rule behind the inference pass but **flip the default on only after**
`--debug-ti` exists and the corpus is audited+annotated, so development isn't
blocked by a corpus-wide break. End state: on by default.
