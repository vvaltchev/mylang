# Type inference — autonomous decisions & open questions

Decisions made while implementing without the user present. Each is a "proper
solution" choice; flag any you want changed.

## Decisions made (deviations from / refinements of the plan)

### Q1. Pass order: inference runs BEFORE resolve_names (not after).
plans/type-inference.md D9 said "after resolve_names". I changed to **after
parse(+const-fold), before resolve_names**. Reasons: (a) the inliner (part of
resolve_names, on by default) rewrites the tree and remaps slots — a clean
lexical-scope inferencer is far simpler on the un-mangled parse tree; (b) type
soundness is a property of the *source*, best checked on what the user wrote;
(c) the future speed-specialization phase (M8) is the part that needs the
resolved/inlined tree, and it stays after resolve_names. Inference also runs
under `-nr` (validate-only), since type-checking *is* validation.

### Q2. No `static_type` field on Construct; types live in inferencer side-tables.
The inferencer owns an `STyArena` whose nodes die when the pass returns, so a
pointer stored on an AST node would dangle afterwards. Instead the inferencer
keeps `unordered_map<const Construct*, STyRef>` (expr types) + its own symbol
table. syntax.h is untouched. When M8 (speed) needs persistent per-node types,
add the field then and keep the arena alive — not needed for inference+checking.

### Q3. "Unknown → dyn" for genuinely un-typeable expressions (NOT conflicts).
D2 (conflict = hard error) is honored: a symbol constrained to two incompatible
concretes (int & str) errors. Separately, an expression whose type cannot be
determined — a value from a builtin given a `dyn` signature, an operation with a
`dyn` operand, an unresolved free identifier (see Q4) — is typed `dyn` and
propagates. dyn means "I don't know, defer to runtime", which is sound; it is
NOT silent widening of a conflict. This is what lets dynamic-leaning existing
programs keep working while real violations are still caught.

### Q4. Unresolved free identifiers -> dyn (not a compile error), except builtins.
A name that resolves to no local/global/builtin is typed `dyn` rather than a
compile-time UndefinedVariableEx. Why: `defined(x)` legitimately takes a
possibly-undefined identifier, and MyLang's model treats undefined-as-value as a
*runtime* error tied to use. Erroring at compile time would break `defined()`
and dynamic patterns. The runtime still throws UndefinedVariableEx as before.
(`defined`/`undef` args are exempt from typing entirely.)

### Q5. Globals are fully hoisted for function-body resolution.
Probes confirm a function body sees top-level vars/consts/funcs regardless of
source order (they run at call time). So all top-level decls are collected first
and visible inside every function body. Consequence/limitation: a *main-level*
use-before-declaration (`print(x); var x=5;`) is NOT flagged at compile time
(still a runtime error). `var x = <expr using x>` types the RHS before binding x
(no self-hoist), matching runtime "reads the outer x".

### Q6. Const scalars are already gone by inference time.
Parse-time const folding inlines const scalars as literals and drops their decls
before inference runs, so the inferencer never sees them as symbols (their uses
are literals with obvious types). Const arrays/dicts/funcs remain as decls and
are typed like vars (const-ness doesn't change a value's type; immutability is
enforced elsewhere).

### Q7. Nullability: params declared (`opt`), locals/globals/returns inferred.
Per D3. A non-`opt` param rejects none/opt/None args (NullabilityEx at the call
site). A local/global/return becomes `opt T` if any none flows in (incl. bare
`var x;`). Using an `opt` value where non-opt is required (arithmetic operand,
subscript base, non-opt arg, condition-as-value? no — conditions accept
anything) -> NullabilityEx. No flow-narrowing in v1.

### Q8. Builtin signatures: precise where useful, `dyn` otherwise.
A signature table types common builtins (len->int, str->str, sqrt->float, ...).
Builtins that are genuinely polymorphic/variadic (print, clone, type, ...) are
given dyn-returning signatures, so code using them stays permissive. Listed in
inferencer.cpp.

### Q9. `dyn` and `opt` keywords as param/var modifiers.
Per D5. `func f(opt x, dyn y)`, `var dyn z = ...;`, `var opt w;`. Implemented as
parser modifiers alongside `const` (Identifier flags + a var-decl flag). `const`
+ `opt`/`dyn` combine. (`const` already exists.)

## Open questions for the user (proceeding with the noted choice)

1. **Strictness of the rollout.** I default inference ON and migrate the
   existing suite. Where an existing test triggered a *runtime* TypeErrorEx that
   inference now catches at *compile time*, I change its expected exception to
   the compile-time one. If you'd rather keep those as runtime errors, we'd need
   inference to not fold statically-known type errors — I think compile-time is
   the point, so I proceed that way.
2. **`opt`/`dyn` spelling** — went with the obvious keywords. Say if you prefer
   `dynamic`/`maybe`/`?`-suffix/etc.
3. **Equality/comparison across types** — `==`/`!=` allowed between any
   comparable pair (numeric cross, same-kind), result int; ordering `< <= > >=`
   numeric-or-string only; otherwise TypeMismatchEx. Confirm.
4. **Calling a non-function** detected at compile time -> TypeMismatchEx
   (message "not callable"). Reusing TypeMismatchEx rather than a 4th class.
