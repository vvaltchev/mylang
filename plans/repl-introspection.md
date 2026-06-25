# REPL introspection: documentation, reflection, and the "thinking" tracer

> Status: **in progress.** Goal — turn the REPL into a tool that documents the
> language, reflects on live program state, and narrates the optimizer's
> reasoning. Three independent pillars; each lands and is testable on its own.

## Why

MyLang has grown a deep, non-trivial compiler: whole-program type inference,
inlining, specialization, template monomorphization, auto-const, auto-pure,
typed-scalar (M8) specialization, flat arrays, CSE, COW. None of that is
*observable* from the prompt beyond `-s` / `:analyze`, and the only docs are the
README (a spec, not a teaching/reference tool that the REPL can query). The
author can no longer fully anticipate what a given snippet will do — so the REPL
needs to *explain itself*: full documentation, runtime reflection, and a
per-category trace of the compiler's reasoning.

Three pillars:

1. **Reflection builtins** — runtime introspection, implemented as *builtins*
   (so scripts and `-rt` tests use them too, not just the REPL).
2. **`:help` documentation system** — the language + builtins reference, queryable.
3. **Diagnostic tracing** — a per-category, toggleable narration of inference /
   inlining / specialization / templates / auto-const / auto-pure / array
   storage / folding, shown before the code runs.

Design constraint (from the maintainer): runtime-derivable facts are builtins;
the inherently compile-time views (inferred static types, the reasoning trace)
are REPL meta-commands **and** a script-toggleable trace, so nothing that *can*
be a builtin is hidden behind a `:`-command.

---

## Pillar 1 — Reflection builtins

New file `src/builtins/reflect.cpp.h` (`#include`d into `types.cpp`), all
registered as **runtime** builtins (they inspect live state; never const-fold).
Documented in README "Builtins", tested in `tests.cpp` (`reflect:` tests).

- `globals()` → `array<str>`: the names bound in the global scope (vars, funcs,
  structs, const containers, and any `$specN`/`$tmplN` clones that became
  globals), sorted, excluding builtins. Walks `get_root_ctx(ctx)` and
  `collect_symbols`. **Honest limit:** a const *scalar* is folded away and is
  not a runtime symbol, so it is absent here — `:globals` (Pillar 2/4) adds it
  from the persistent const context. Documented.
- `typeof(x)` → `str`: a rich *runtime structural* type string, distinct from
  the existing `type(x)` (which returns the bare kind, `"array"`). Examples:
  `"int"`, `"opt str"` is N/A at runtime (a none is `"none"`), `"array<int>"`
  (from storage kind), `"array<Point>"`, `"array<dyn>"` (general),
  `"dict<int,str>"` (probe one pair; empty → `"dict"`), `"Point"` (instance),
  `"type Point"` (descriptor), `"func(...)->?"` (from the decl), `"exception"`.
  Implemented from the value alone, so it works in any script.
- `signature(f)` → `str`: render a function's declared signature from its
  `FuncDeclStmt` — `[pure] func name(p1, opt p2, dyn p3, const p4)` with
  annotations / `?` / `~` reflected, and `display_name` when set. A struct type
  → its constructor form `Point(int x, int y)`. A builtin value → a short note.
- `layout(S)` → `str`: a struct's in-memory layout. POD → `POD, size=N,
  align=A` then one line per field `name: type @offset (size)`; boxed → `boxed,
  N slots` then `name: type [slot]`. Accepts a struct *type* or *instance*.
- `specializations(f)` → `array<str>`: every `$specN` / `$tmplN` clone whose
  `display_name` equals `f`'s name, each rendered via the signature logic (so
  you see the concrete monomorphized signatures). Runtime-reachable because the
  clones bind as REPL globals.

Shared rendering helpers (signature/type/layout) live in `reflect.cpp.h`; the
`:help`/`:globals` meta-commands reuse them where practical.

## Pillar 2 — `:help` documentation system

New module `src/replhelp.{h,cpp}` — a static documentation database + a query
front end. No core changes; pure content + lookup. Colorized on a TTY (reuse
the highlight ANSI palette).

Data:
- **Builtin docs**: `name → {category, signature, summary, long?}` for every
  builtin (and the math constants). Categories: conversion/introspection,
  arrays, dict, string, math, float-classification, constants, random, I/O,
  filesystem, control, exceptions, reflection.
- **Language docs**: `category → [feature]`; `feature → {syntax, summary,
  long}`. Categories include *surface* features (values & types, variables &
  const, operators, control flow, functions, closures, arrays, dicts, strings,
  structs, exceptions, named args, nullability & `opt`, `dyn`, type annotations,
  templates) and *optimization* features (const-folding, auto-const, auto-pure,
  inlining, specialization, template instantiation, typed-scalar/M8, flat
  arrays, CSE, copy-on-write). The optimization entries explain the *why* and
  point at the trace category that shows it live.

Dispatch (`:help [topic]`):
- `:help` — overview: what the help system covers + the index commands.
- `:help builtins` — all builtins grouped by category (name + one-liner).
- `:help builtins <category>` — that category expanded with signatures.
- `:help language` — the feature categories with a brief description each.
- `:help <category>` — features in a language category.
- `:help <name>` — resolve `<name>` as builtin → feature → category, print its
  full entry; on a miss, suggest near matches.

## Pillar 3 — Diagnostic tracing ("MyLang's mind")

New module `src/trace.{h,cpp}`: a `TraceCat` bitmask enum, a global
`g_trace_mask`, `trace_enabled(cat)` (the hot guard), and a `TraceWriter` that
emits colored, indented lines to a sink (default `std::cerr`; swappable so
`-rt` can capture). **When a category is off the cost is one mask test**, so the
instrumentation can sit on hot paths without harming a normal run.

Categories: `infer`, `inline`, `specialize`, `template`, `autoconst`,
`autopure`, `arrays`, `fold` (+ `all`). Decision points get a one-line guarded
emit that reads like reasoning, e.g.:
- `infer:  x  :=  join(int, none)  =  opt int`
- `infer:  f  param a  <-  int  (call @3:5)`
- `inline: f(y)  body 7 nodes ≤ 24  →  splice`
- `spec:   g(3)  folded 12→4 nodes  →  $spec0`
- `tmpl:   id(int)  →  $tmpl0  (instance 1)`
- `autoc:  k  write-once scalar 5  →  fold uses`
- `autop:  hypot  reads only consts/params  →  effective pure`
- `array:  a  dest array<int>  →  flat (ints)`

Control surface (both, per the builtins-first rule):
- builtin `trace(category: str, on: bool)` → none, and `traceoff()` → none, so
  a script/test can flip categories; plus `tracing()` → `array<str>` (active
  categories).
- meta-command `:trace` (show state), `:trace <cat...> on|off`, `:trace off`.

The trace fires inside the REPL's real per-input pipeline (`check_input` →
`resolve_names` → `specialize_types`), so `:trace infer on` then typing code (or
`:source file`) narrates the decisions just before the value is produced.

---

## Phasing (commit per step)

0. **Plan** (this file). ✅
1. **Pillar 1** reflection builtins + `reflect:` tests + README. ✅
2. **Pillar 2a** `replhelp` builtins reference (`:help builtins[/cat]`, `:help
   <builtin>`) + tests. ✅
3. **Pillar 2b** `replhelp` language reference (`:help language`, categories,
   features incl. optimizations) + tests. ✅
4. **Pillar 3a** `trace` module + control surface (builtin + `:trace`) wired
   with the `infer` category + tests. ✅
5. **Pillar 3b** remaining trace categories (inline, specialize, template,
   autoconst, autopure, arrays, fold) + the `--trace` CLI flag + tests. ✅
6. **`:globals` / `:type <expr>`** rich REPL views backed by
   `ReplInfer::global_type` (committed globals' inferred static types, const
   scalars merged from the const ctx) + tests. ✅
7. **Docs sync**: README (builtins + `:help`/`:trace`/`:globals`/`:type` +
   `--trace`), CLAUDE.md (the `trace`/`replhelp` TUs, `reflect.{h,cpp.h}`, the
   trace mechanism, the reflection builtins, the meta-commands). ✅

**Status: phases 0-7 landed.** Follow-up work in progress:

8. **Inspectable compiler clones + `:show` (the optimized-AST view).**
   - **Rename** synthetic clones from `$tmplN`/`$specN` to `<name>$N` /
     `<name>$sN` (the original name as a prefix, monotonic per name so a
     re-`undef`'d template can't collide), and make `$` a valid identifier
     char so `typeof(f$0)` lexes. `specializations()` and `:trace template`
     then show the readable names.
   - **`:show <name>` + `show(f)` builtin** — render the FINAL optimized AST of
     a function (and, for `:show <name>`, its `<name>$N` clones) as synthetic
     MyLang-like code: dead code already gone, folded consts as literals,
     inlined call bodies spliced in (annotated), flat-array element types as
     `array<int>` (not valid script syntax, but informative), explicit
     annotations / typed-scalar hints surfaced. So `func f(x,y)=>x+y;
     func g(){print(f(1,2));}` shows `g` as `func g() { print(3); }`, and
     `func g(x,y){print(f(x,y));}` as `func g(x, y) { print(x + y); }`. A new
     `coderender.{h,cpp}` unparser; the optimized tree is the REPL's retained,
     post-`resolve_names` AST.

Possible later follow-ups (not blocking): a `:type` non-committing inference
*probe* for an arbitrary expression; Tab-completion of `:help` topics
(`repl_help_topics` exists but isn't wired into the completer); full inferred
LOCAL types in `:show` (today best-effort from AST hints).

## Open questions (decide as we go)

- Return shapes: builtins return `str` / `array<str>` (typed, simple) rather
  than heterogeneous dicts. The pretty tabular views are the `:`-commands.
- Trace verbosity levels (terse vs. step-by-step) — start terse, add a `:trace
  verbose` toggle only if needed.
- Whether `typeof` should consult the inferencer in the REPL for a *static*
  answer; for now it is purely value-driven (runtime), and `:type <expr>` is the
  static one. Keeps `typeof` script-portable.
