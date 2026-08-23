# Plan: `?.` optional chaining, `??` null-coalescing, and the ternary `?:`

Three operators that all involve `?`. They are designed together so the four
`?` roles (the existing nullable suffix `int?`, plus `?.`, `??`, `?:`) coexist
with no ambiguity. **Status: ALL DONE — ternary `?:` + `??` (phase 1+2) and
`?.` optional member access (phase 3).**

Phase 3 notes (as built): `Op::qmdot` (`?.`) - a `bool MemberExpr::optional`
flag (NOT the planned `OptChainExpr` node; the per-link flag is far less code
and
covers the all-optional chains that MyLang's member chains actually are). Each
`?.` guards ONLY its own base (`a?.b?.c` short-circuits; a plain `.c` after `?.`
is unguarded - a deliberate simplification vs JS's whole-chain guard, documented
in README). do_eval returns none on a none base; type_of is `opt(member)`; the
check pass skips the non-opt-base requirement for it. Scoped to member access -
optional subscript `?.[` and optional call `?.()` are not implemented. This
makes
the reflection chains clean: `type(a)?.elem?.kind` (no if-narrowing).

Phase 1+2 notes (as built): `Op::coalesce` (`??`) is the only new token (ternary
reuses `?`/`:`). `pExprCoalesce` + `pExpr13` sit between `||` and `=`; `pExpr14`
enters at `pExpr13`. New nodes `TernaryExpr`/`CoalesceExpr` (syntax.h) - both
short-circuit in eval, fold in the parser AND in AutoConst's `fold_reads` (which
needed the new cases - it has no generic fallthrough). The decl-vs-ternary
ambiguity is resolved by `is_decl_terminator` gating in `pAcceptDeclPrefix` (a
`T ? name` is a decl only when a decl terminator follows `name`; else it is a
ternary, bailed *before* `lookup_struct_type` can wrongly reject the condition
name). Inference: ternary = `join(branches)`, `a ?? b` = a's non-none part
joined with b (non-opt when b is). Covered by the `ternary:` / `coalesce:` tests
+ a cross-input `repl:` test.

## 0. Why one plan

`?` is already the nullable **type suffix** (`int?`, `array<int?>`, param `y?`).
Adding three more `?`-bearing constructs needs a single coherent token +
precedence design so they never collide. The good news (confirmed in source):

- `Op::questionmark` (35) and `Op::colon` (33) **already exist**, so the
  **ternary adds no lexer token** — it reuses `?` and `:`.
- Only `?.` and `??` are new tokens — two 2-char operators, recognized by the
  existing maximal-munch path (`lexer.cpp` ~262, which already does `>>>`/`>>`).
  Both have `?` as a prefix, which the 1-char bootstrap requires.
- The precedence ladder has a real gap at `pExpr13` (CLAUDE.md: levels
  `02..12,14`, 13 unused) — exactly where the conditional operator goes in C.

## 1. The four `?` roles, kept separate

| Spelling        | Role                     | Where recognized                |
|-----------------|--------------------------|---------------------------------|
| `int?` `y?`     | nullable type suffix     | decl/type position (existing)   |
| `a ?. b`        | optional chaining        | postfix chain (`pExpr01`)       |
| `a ?? b`        | null-coalescing          | binary, new level below `\|\|`  |
| `c ? a : b`     | ternary conditional      | new `pExpr13`                   |

**Lexer.** Add `Op::qmdot` (`?.`) and `Op::coalesce` (`??`) to `operators.h`
(before `op_count`) and the index-aligned `OpString` (`lexer.h`), bump
`op_count`. Maximal munch tries 2-char `?.`/`??` before the 1-char `?`, so:
`a?.b`→`?.`, `a??b`→`??`, `a ? b`→`?` (space breaks the munch), `int?`→`?`,
`array<int?>`→`?` then `>` (no `?>` token). No change to how `?`/`:` lex today.

## 2. Precedence & associativity

Insert two levels between `||` (`pExpr12`) and assignment (`pExpr14`), matching
C# (`|| > ?? > ?: > =`):

```
pExpr12   ||                              (existing, unchanged)
  └ pExprCoalesce   ??     right-assoc    NEW   (a ?? b ?? c == a ?? (b ?? c))
      └ pExpr13     ?:     right-assoc    NEW   (a?b:c?d:e == a?b:(c?d:e))
          └ pExpr14 = += …  (existing)
```

Wiring: `pExpr14`'s `lside = pExpr12(...)` (parser.cpp:1571) becomes
`lside = pExpr13(...)`. Then:

- **`pExpr13` (ternary):** `cond = pExprCoalesce(...)`; if `pAcceptOp(?)`:
  `then = pExpr14(...)` (full expr, bounded by `:`), `pExpectOp(:)`,
  `els = pExpr13(...)` (right-assoc); build a `TernaryExpr`. Else return `cond`.
- **`pExprCoalesce` (`??`):** `lhs = pExpr12(...)`; if `pAcceptOp(??)`:
  `rhs = pExprCoalesce(...)` (right-assoc); build a `CoalesceExpr`. Else `lhs`.

`?.` is **postfix** (tightest), handled in the `pExpr01` call/subscript/member
loop — see §5.

## 3. The decl-vs-ternary ambiguity (the only real parser wrinkle)

`pAcceptDeclPrefix` (parser.cpp:292) recognizes `TYPE [?] NAME` as a typed decl
by shape. For `int ? a : b` it currently matches `int`(type) `?`(opt) `a`(name)
and commits to a decl — then chokes on the `:`. This is the **only** place a
ternary is ambiguous, and **only at statement start / for-init** (the only
positions that run the decl scanner). In every value position — `= rhs`, call
args, `return`, array/dict elements — the parser is already in expression mode,
so `cond ? a : b` is unambiguous with **zero** parens.

**Resolution (recommended): a one-token lookahead.** After the prefix scan,
when `starter` is set, peek the token *after* the declared name; if it is `:`
(or, more generally, not a decl-continuation `;`/`=`/`,`), the prefix is **not**
a decl —
reject it and let `pStmt` parse an expression statement (the ternary). A decl
name is never followed by `:`, so this is sound and invisible. ~3 lines in an
already-lookahead-based scanner; **keeps idiomatic bare `cond ? a : b`
everywhere.**

**Alternative considered — require `(cond) ? a : b`** (the parenthesized form
the maintainer suggested). It *does* fully sidestep the wrinkle: a ternary
statement
would then start with `(`, which `pAcceptDeclPrefix` never treats as a decl
(none of its branches match `(`), so **no scanner change at all**. The cost: a
permanent `(...)` tax on *every* ternary, including unambiguous ones like
`var z = x > 0 ? a : b` — no mainstream C-family language requires this, and it
would make MyLang's ternary feel off. **Recommendation: the lookahead, not
mandatory parens** — the lexer is not at risk at all (the ternary adds no
token), and the parser change is small, localized, and deterministic. (Decision
point: the maintainer can choose the parens form for maximal grammar
simplicity.)

## 4. The other `:` consumers (slice / dict / named-arg)

`:` is also slice (`a[lo:hi]`), dict (`{k: v}`), and named-arg (`f(x: 1)`). The
ternary **eats its own `:`** because each of those parses a full expression for
the part *before* checking for `:`:

- **Slice** (`pAcceptSubscript`:1052): parses the bound with `pExprTop` *then*
  looks for `:`. Since the ternary sits below `pExprTop` in the ladder,
  `pExprTop` consumes the ternary's `:`, so **`a[c ? x : y]` is a subscript by a
  ternary**.
  A slice whose bound is a ternary needs parens: `a[(c?x:y):hi]`. (Identical to
  Python's `a[(x if c else y):]` rule — document it.)
- **Dict** (`pDictKVPair`:1129): the key is `pExpr14` (full expr → eats the
  ternary `:`), then `pExpectOp(:)` separates key/value. `{c ? a : b : v}` works
  but parens are clearer: `{(c?a:b): v}`.
- **Named arg** (pArgList:896): a label is `IDENT :` by one-token lookahead.
  `c ? a : b` starts `IDENT ?`, not `IDENT :`, so it's a positional ternary — no
  collision.

No grammar conflict; the only user-visible rule is "parenthesize a ternary used
as a slice bound."

## 5. `?.` optional chaining (the most involved piece)

`a?.b` ≡ `(a == none) ? none : a.b`, and it **short-circuits the whole rest of
the chain**: `a?.b.c` is `none` when `a` is `none` (`.c` not evaluated), i.e.
`(a==none) ? none : a.b.c`. Also `a?.[i]` (optional subscript) and optionally
`a?.()` (optional call — can defer).

**Recommended implementation — a dedicated `OptChainExpr` node.** When the
`pExpr01` postfix loop encounters the first `?.`/`?[`, collect the entire
remaining chain into one node: a `base` plus a vector of links
`{kind: member|subscript|call, optional: bool, operand}`. `do_eval`: `v = base`;
for each link, `if (link.optional && v is none) return none;` else apply the
link. This puts **all** short-circuit logic in one place and one eval loop —
the existing `MemberExpr`/`Subscript`/`CallExpr` nodes are untouched, and code
with no `?.` pays nothing (the normal left-nested chain is still built). Result
type is `opt(type of the last link)`.

(Rejected: per-node `optional` flag + a propagating "nullish" sentinel — it
would force every postfix node's `do_eval` to recognize and pass the sentinel
through,
touching more code than the collect-and-loop node.)

Because `?.` needs nullability inference + interplay with narrowing, it is the
natural **Phase 3** (after the two pure-expression operators).

## 6. Semantics & inference

- **Ternary `c ? a : b`.** `c` is condition (`is_true`, like `if`/`while` — any
  type, not required `bool`). Eval: evaluate `c`, then **only the taken branch**
  (short-circuit; a new `Construct` node, not exception-based control flow).
  Inference: result type = `join(type a, type b)` (so `c ? 1 : 2.0` → float,
  `c ? x : none` → `opt T`); both branches must type-check. M8: a ternary over
  two typed int/float branches with a known-int condition *could* specialize to
  an unboxed select later — **defer** (correctness first).
- **`a ?? b`.** If `a` is non-`none`, `a`; else `b` (short-circuit `b`).
  Inference: `a:opt T ?? b:U` → `join(T, U)`, dropping `opt` when `b` is
  non-opt (so `optInt ?? 0` is non-null `int`). On a **non-opt** `a` the `??`
  is dead (result is always `a`) — fold to `a` and emit an `autoconst`/`infer`
  trace note (don't error; mirrors how a const-true `if` is handled).
- **`a?.b`.** Result `opt(type a.b)`; see §5. Plays with the existing null
  narrowing (`a?.b ?? d`, `if (a?.b != none) ...`).

All three are **compile-time foldable** when their operands are const (the
"optimizations must generalize" bar): `true ? a : b` → `a`, `none ?? b` → `b`,
`x ?? b` (x const non-none) → `x`, and a fully-const ternary/coalesce → its
literal. Wire into AutoConst + the parser const path; **test cross-input in the
REPL**, not just single-compilation (CLAUDE.md rule).

## 7. AST / files touched (per the recipes)

- `operators.h` / `lexer.h` — `Op::qmdot`, `Op::coalesce`, `OpString`, count.
- `syntax.h` / `syntax.cpp` — `TernaryExpr`, `CoalesceExpr`, `OptChainExpr`
  nodes: each needs `do_eval` decl, `serialize()`, and **`clone()`** (pure
  virtual — omitting it won't compile).
- `parser.cpp` — `pExpr13`, `pExprCoalesce`; rewire `pExpr14`; the
  `pAcceptDeclPrefix` lookahead (§3); the `?.` collection in the `pExpr01` loop.
- `eval.cpp` — the three `do_eval` bodies (short-circuit).
- `inferencer.cpp` / `stype`-via-`StaticType` — type rules (§6), check pass,
  nullability; M8 deferred.
- `resolver.cpp` (AutoConst) + parser const path — const folding (§6).
- `coderender.cpp` — render the three (the `:show`/`-a` decompiler).
- `highlight.cpp` — already colors operators; verify `?.`/`??` render fine.
- `replhelp.cpp` — `:help` entries for the operators / the language section.
- `README.md` (language spec — new operators, precedence, the slice-paren rule)
  and `CLAUDE.md` (the `pExpr` ladder description, eval notes) **in the same
  commits**.

## 8. Phasing

1. **Ternary `?:`** — pure expression; the decl lookahead; const-fold; tests.
2. **`??`** — adjacent precedence level; const-fold; tests. (1+2 can be one
   change — shared ladder rewiring.)
3. **`?.`** (and `?[`) — `OptChainExpr` + nullability inference + narrowing
   interplay. Separate change.

## 9. Test matrix (the bar is "generalizes", not "one example")

- Lexer: `?.`/`??`/`?` disambiguation incl. `array<int?>`, `a?.b`, `a??b`,
  `a ? b : c`, `a ?. b` (spaces).
- Precedence/assoc: `a || b ? c : d`, `a ?? b ? c : d`, `a ? b : c ? d : e`,
  `a ?? b ?? c`, mixing with arithmetic/comparison.
- Decl-vs-ternary edge: `int ? a : b;` as a statement (must be a ternary, not a
  decl error); `int? x;` still a decl; `for (int? x = ...; ...)`.
- Slice/dict interplay: `a[c?x:y]` (subscript), `a[(c?x:y):h]` (slice),
  `{(c?a:b): v}`, `f(c ? a : b)` (positional).
- Short-circuit eval: untaken ternary branch / `??` rhs / `?.` tail **not
  evaluated** (use a side-effecting fn + a flag, like the typestr fold test).
- Nullability: `opt ?? default` → non-opt; `a?.b` → opt; narrowing after.
- Const-fold + **cross-input REPL** (define a helper in one input, fold a
  ternary/`??`/pure-`?.` in a later input).
- `:show` / `-a` render the operators; `:help` lists them.
- Full matrix green: debug+ASan/UBSan, release, RECYCLE.
