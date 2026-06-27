# Typed array / dict syntax (custom element types)

> **STATUS: design note / deferred.** No code yet. Captures the decision so we
> can implement it later.

## The question

We can declare explicitly-typed variables (`int x = 5;`, and now
`Point p = Point(1,2);` / `Point p;`). We do **not** yet have a way to write the
ELEMENT type of a container: an array of a custom type, or a dict with explicit
key/value types. Today `array a` / `dict d` are *generic* (only the kind is
constrained; the element type is inferred from the initializer).

Candidate syntaxes considered:

1. `A[] arr;`   — postfix `[]` on the type (C# / Java / TypeScript).
2. `A arr[];`   — postfix `[]` on the name (C).
3. `array<A> arr;` / `dict<str, int> d;` — angle-bracket generics.

The crux is **the dict**: there is no natural `[]` form for a dict, so `A[]`
next to `dict<str, int>` reads as two unrelated features. The array drags the
dict along — whatever we pick should make arrays and dicts look like the *same*
construct, because they are (parameterized containers).

## Decision: angle-bracket generics — `array<A>` and `dict<K, V>`

Reasons, in priority order:

1. **One notation, no asymmetry.** `array<Point>` / `dict<str, int>` are
   obviously the same construct. `A[]` forces the dict into a different shape.
2. **It is already the language's own notation.** Inferred types *print* as
   `array<int>`, `dict<K,V>`, `array<dyn>` in `:type`, error messages, and
   `--debug-ti`. Using the same surface syntax means "what you write is what the
   tools show you" — self-consistent. `A[]` would be a second vocabulary.
3. **It composes**: `dict<str, array<Point>>`, `array<array<int>>` fall out;
   the `[]` form has no composing story that includes dicts.
4. **Familiar** (C++/Rust/Java generics) and **backward-compatible**: bare
   `array a` / `dict d` keep meaning "generic, element inferred"; `array<A>`
   merely *adds* element pinning (the `var` vs `int` relationship).

### Rejected / deferred alternatives

- **`A arr[]`** — fights MyLang's "type before name" grammar (the lone C-ism)
  and still has no dict analog. Drop.
- **`A[]`** — terse and nice for arrays alone, but cannot carry dicts. Could be
  added LATER as pure sugar for `array<A>` (TypeScript-style `A[]` == `Array<A>`),
  which would obey the "sugar adds spelling, never subtracts capability" rule —
  but it is a second way to say one thing, so not for v1.

## Implementation notes / costs (when we build it)

- **`>>` token collision**: `array<array<int>>` lexes `>>` as the right-shift
  operator. The type parser must split a trailing `>>` into two `>` in type
  position (exactly what C++11 did). Localized, solved, but real.
- **`array`/`dict` are not keywords** (they are the `array()`/`dict()`
  builtins). Confine the generic parse to declaration / parameter position,
  where we already disambiguate `array name` as a type; `array<...>` there is
  unambiguous (`array < x` as a comparison is meaningless — `array` is a builtin
  value). Elsewhere `<`/`>` stay comparison.

## Why it is worth doing (it is more than cosmetic)

Inference already gives `array<Point>` from an initializer (`var a =
[Point(1,2)]`), like struct vars infer from the RHS. So the syntax mostly serves:

- **Empty / uninitialized** typed containers: `array<Point> pts;` — a
  typed-but-empty array, so later `append`s are checked AND the compiler can pick
  **flat POD-struct storage** for an array it has not seen elements of yet
  (a real capability the bare `array pts;` cannot express).
- **Documentation / explicit checking** at the declaration site.

See also: `plans/type-driven-specialization.md` (flat storage), the "Explicit
types" section of `README.md`, and the struct-annotation work in `CLAUDE.md`.
