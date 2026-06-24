# Interactive REPL — implementation plan

> **STATUS: designed, not started.** This plan targets an IRB-3.4-quality REPL
> (live syntax highlighting, true multi-line block editing, auto-indent, pretty
> `=>` output, history) — *not* the dumb readline loop of the classic Python
> REPL. **v1 = Phases 0–3, Unix-only** (Windows + autocompletion are deferred,
> see §8). The two hard problems are (1) running the *real, full* pipeline per
> input over an **expandable global scope** so the REPL is a faithful inspection
> tool, not a stripped-down interpreter, and (2) hand-rolling the
> terminal/line-editor under the project's hard "no third-party deps" rule.

## 1. Goal & the bar ("inspired by Ruby, not Python")

A `mylang` invoked with no FILE/`-e` on a TTY drops into a REPL. What "Ruby
3.4 / IRB+reline quality" means, concretely, and what we commit to:

- **Live syntax highlighting as you type** (keywords, strings, numbers,
  comments, operators, identifiers) — re-lexed every keystroke.
- **True multi-line block editing**: an incomplete construct (`func f() {` …)
  keeps the block open with a `..>` continuation prompt, and Up/Down move
  *within the block* to edit earlier lines, re-rendered as a unit — not a
  one-shot line buffer.
- **Auto-indentation** of continuation lines by brace/paren depth.
- **Autocompletion (Tab)** of keywords, builtins, REPL-defined names, and
  `struct` field/const names after `.`, eventually with an IRB-style
  navigable dropdown.
- **Pretty result echo**: `=> <value>` after each input, colored; multi-line
  pretty-print for arrays/dicts/structs.
- **History** (in-memory + a persisted file), Up/Down + reverse search.
- **Robust line editing**: Left/Right, Home/End, word moves, kill keys,
  Ctrl-C cancels the line (not the process), Ctrl-D exits on an empty line,
  Ctrl-L clears, terminal-resize aware, terminal restored on any exit.

Non-goals (v1): Unicode/wide-char editing (mylang is deliberately 8-bit /
no-Unicode — the editor is byte = column, matching the language); a full
debugger; image/HTML output.

## 2. Hard constraints (shape every decision)

- **No third-party dependencies, ever** (the project's defining rule — incl.
  the test harness). So **no readline / ncurses / reline**. We hand-roll the
  line editor on raw OS/stdlib headers only: `termios.h`/`unistd.h`/`ioctl`
  (Unix), the Win32 Console API (Windows). ANSI escape sequences drive all
  rendering (on Win10+ via `ENABLE_VIRTUAL_TERMINAL_PROCESSING`), so only the
  *raw-input* layer is platform-specific.
- **8-bit, no Unicode** (deliberate): the editor treats one byte as one
  column. Documented limitation, consistent with the lexer.
- **`-rt` in-binary tests, no framework**: every non-TTY-touching piece (the
  editor state machine, highlighter, completer, multi-line detector, the
  persistent-eval engine) must be a **pure, headless-testable core** fed
  synthetic input — see §7.
- **80-col source, SPDX header, C++17, `-fwrapv`**; new `src/*.cpp` are picked
  up by the Makefile glob automatically.

## 3. Architecture

Two independent subsystems behind a thin TTY shell:

```
  ┌─────────────── tty_io (platform) ───────────────┐   raw bytes in / ANSI out
  │  termios raw-mode | Win32 console; read()/write │
  └───────────────────────┬─────────────────────────┘
                          │ keystrokes / render bytes
  ┌───────────────────────▼─────────────────────────┐
  │  LineEditor (pure core)                          │  buffer+cursor, keymap,
  │   highlight() · complete() · multiline_state()   │  history, redraw planner
  └───────────────────────┬─────────────────────────┘
                          │ a completed source chunk
  ┌───────────────────────▼─────────────────────────┐
  │  ReplEngine (pure-ish core)                       │ persistent interpreter
  │   eval_input(src) -> { output, =>value, error }   │ state; the §3.1 problem
  └──────────────────────────────────────────────────┘
```

### 3.1 The persistent-state problem (the crux, mylang-specific)

A script is parsed as ONE `pBlock`, then `infer_types` → `resolve_names` →
`specialize_types` → `eval(nullptr)`, where the root block builds a *per-call*
"main" `Frame` and slots top-level vars into it. Nothing survives between
inputs, and three layers resist persistence: the parser's `const_ctx` chain;
the whole-program inferencer (an `STyArena` + side tables, discarded each run,
which also *enforces* the mandatory-`dyn`/`opt` rules); and the slot-based
runtime (per-call frame, 64-slot cap, `AlreadyDefinedEx` on re-declare).

**Design (per the maintainer's direction): a FAITHFUL REPL — run the real,
full pipeline on every input; the only difference from a script is that the
global scope is *expandable*.** The mechanism is three persistent stores plus
one rule:

- **Retained input ASTs.** Every committed input's parsed `Construct` tree is
  kept alive for the session (`vector<unique_ptr<Construct>>`). This is what
  lets a `pure func` / `struct` / kept const-array node from input N still be
  *inlined, called, and folded* in input N+1 — pure funcs "remain in the AST",
  so cross-input inlining and pure-call folding Just Work. Never freed = no
  dangling `FuncObject`/`StructTypeDef`/`LiteralObj` pointers.
- **Persistent const context.** One const `EvalContext` carried across inputs
  (the REPL's top-level const scope is *this*, never pushed/popped away). The
  resolution of my earlier worry: const **scalar** decls are dropped from the
  *runtime* tree, but their **value** lives in this const context — which is
  exactly what `MakeConstructFromConstVal` folds against — so previously
  declared consts fold in later inputs with nothing extra. Const arrays/dicts/
  pure-funcs/structs are kept as real symbols (their decl nodes are in the
  retained ASTs and bound here). This is your "const AST that stays unfolded":
  realized as retained decls + a persistent const scope, so no per-input fork
  is needed.
- **Persistent global runtime `EvalContext`** = the expandable global scope.
  REPL top-level decls live here (var/func/struct/kept-const). New inputs add
  to it; re-declaration overwrites. `func`s capture this context, so they see
  globals defined in later inputs (forward refs / mutual recursion across
  inputs). REPL top-level vars resolve as **globals (map)**, not slots — the
  resolver already keeps function-visible globals in the map ("escaped"); the
  REPL marks *all* its top-level decls escaped, so they persist. Crucially
  **nested function/loop locals still slot and specialize normally** — so
  flat arrays, M8 typed scalars, inlining, specialization all run and are
  inspectable. Only the top-level frame is replaced by the persistent map.

**Incremental inference (the one genuinely new pass-level work).** The
inferencer must (a) seed each input's top-level scope with the *persistent*
`TypeSym`s of prior globals (a persistent `STyArena` + a global-symbol table),
and (b) commit the input's globals back. It only looks *backward* (no forward
refs across inputs — fine, an incremental compiler). The mandatory-`dyn`/`opt`
rules run **within** an input; a global still **unconstrained** at the input's
end is not an error but *deferred/unknown* (your `opt var x;` case: `opt`, type
unknown, reads `none` until a use pins it).

**Type commitment (a REPL-only corner case, but NOT a special code path).**
The mental model: **each input is a whole script augmented with a pre-context
of the existing globals.** When an input commits, each global's inferred type
is **pinned like an explicit annotation** for all later inputs — seeded as a
pinned `TypeSym`, so the inferencer's *existing* annotation-conformance check
does all the work. A committed `var x = 3` (→ `int`) then behaves, for the next
input, exactly like `int x = 3`: a later `x = "hello"` fires the **same**
`TypeMismatchEx` as `int x=3; x="hello"` would in a script, and the REPL
**rejects just that input** (keeping `x = 3`), never killing the session. It
differs from the *script* `var x=3; x="hello"` only because that script joins
both up front to `dyn`, whereas the REPL already committed `int`. Distinctions:
- **No special-casing.** The error is not a REPL invention — in a pure script
  the same path is simply *unreachable* for an *inferred* var (inference joins
  to `dyn`); it surfaces only via an explicit annotation or the REPL's commit.
  Reaching it on an inferred-pinned var would actually flag an inference bug.
  The one message tweak: say "**inferred** type T" vs "**declared** type T" by
  whether the pin came from inference or an annotation.
- **Granularity is the input** (confirmed). Within one input, inference is
  whole-input and faithful, so `var x=3; x="hello";` *typed as a single input*
  infers `dyn` (no error), identical to the script; the pin bites only across
  inputs.
- **Assignment vs. re-declaration.** `x = "hello"` (assignment) must conform to
  the pinned type → error. `var x = "hello"` (re-*declaration*) is allowed and
  rebinds `x` to a new pinned type — the REPL permits redefining a global (a
  script rejects the duplicate decl), so this is how you intentionally change a
  global's type.
So the REPL is strict where it can be and patient where it must be — never
"dynamic-mode."

**The pipeline per input, then, is the real one:** parse (folding vs the
persistent const ctx) → infer (seeded with global types) → resolve (top-level =
persistent globals, nested = slots) → inline + specialize (prior pure funcs
available) → eval (in the persistent global ctx) → `=>` echo.

Result echo: evaluate the input's top-level statements in order, keep the last
value, print `=> ` + `value.get_type()->to_string(value)` (the call
mylang.cpp already uses for exception data). Errors are caught per input,
rendered with the **existing caret machinery** (`dumpLocInError`, factored out
of mylang.cpp into a shared `errfmt` so REPL and file driver share it), and the
loop continues. A persistent `lines` buffer + line counter keeps carets
pointing at the right input line, exactly like the file path today.

**Open-world caveat (a real soundness point, not a detail).** A script is
closed-world; the REPL is open — another input may write a global later. So
optimizations that assume they have seen *all* writes/uses of a symbol are
unsound **for globals** and must be scoped to input-*local* declarations:
- **auto-const** must NOT promote a top-level `var` (input 1 `var x = 5` is
  write-once *within* that input; promoting+dropping its decl would make
  input 2's `x = 6` an undefined variable). Top-level vars are excluded from
  auto-const exactly as they are excluded from slotting (both follow from
  "REPL top-level = escaped global").
- **dead-code / value-based folding keyed on a mutable global's value** is
  likewise off. A `const` global is immutable → folding it is still sound;
  a `pure func` / `struct` never mutates → inlining/calling across inputs is
  sound (a later redefinition is just a new symbol for future inputs).
Input-local slots, flat arrays, M8 typed scalars, and inlining of prior pure
funcs are all closed over the input, so they run at full strength. Net: the
REPL is the real optimizer, minus only the optimizations that are *unsound*
in an open world — which is correctness, not a downgrade.

**Inspection is a first-class goal** (this is *why* it must be faithful): the
REPL exposes the existing tools as per-input meta-commands — `:tree` (`-s`),
`:analyze` (`-a` colors), `:type <expr>`, `:tokens` (`-t`) — so you can watch
const-folding, auto-const, inlining, specialization, and flat-array decisions
on a live snippet. These fall out for free once the real passes run per input.

**`:source FILE`** reads a file and feeds it into the REPL **exactly as if it
had been typed at the prompt** — *not* a separate script run. So it augments
the persistent globals and obeys all the same incremental semantics: the file
is split into complete top-level units (the §6 completeness detector, the same
one that decides when an interactive input is done) and each unit is replayed
as one input, in order — so cross-input type commitment applies between
successive units (a sourced `var x=3` on one line and `x="hello"` on the next
errors, just as if typed), and a failing unit is rejected while the earlier
ones keep their effects. It shares the `eval_input` path verbatim; the `=>`
echo is suppressed for sourced units (errors still print). This makes a saved
session, or a prelude of helper funcs/consts, loadable into a live REPL.

### 3.2 The line editor (the terminal problem, under no-deps)

`tty_io` (platform shell): enter raw mode (`tcsetattr` / `SetConsoleMode`),
restore on *every* exit path (RAII + `atexit` + SIGINT/SIGTERM handlers — a
terminal left in raw mode is the classic footgun); query width
(`TIOCGWINSZ` / `GetConsoleScreenBufferInfo`); handle SIGWINCH; read bytes,
decode escape sequences (arrows, Home/End, Delete, PgUp/Dn, bracketed paste).

`LineEditor` (pure core, no syscalls): an edit buffer + cursor, a keymap, and
a **redraw planner** that, given (buffer, cursor, terminal width), emits the
minimal ANSI to repaint. It calls three pure helpers — `highlight()`,
`complete()`, `multiline_state()` — and owns history. Because it never touches
the fd, it is fully unit-testable: feed a byte script, assert buffer/cursor/
emitted ANSI.

## 4. Phasing (v1 = Phases 0–3, each phase ships a better REPL)

- **Phase 0 — The faithful engine + a cooked-mode REPL.** Build `ReplEngine`
  (§3.1): the persistent const/global/type stores, retained ASTs, the
  incremental-inference seeding, the REPL resolver mode (top-level = globals),
  and the per-input full pipeline. Drive it with plain `std::getline` (no raw
  editing yet); multi-line via a lexer depth counter + the "unexpected EOF"
  signal (`SyntaxErrorEx` with `tok == &invalid_tok`), `..>` prompt; `=>` echo;
  error-catch-and-continue; the `:tree`/`:type`/`:analyze`/`:source`/`:quit`
  meta-commands. **This is the meatiest, most important phase** — it proves the
  faithful-persistence architecture, decoupled from terminal code, and is
  fully headless-testable (§7). The §3.1 inferencer/resolver work lives here.
- **Phase 1 — Hand-rolled raw-mode line editor.** `tty_io` (termios) +
  `LineEditor` core: cursor moves, Backspace/Delete, Home/End, word/kill keys,
  Ctrl-C/D/L, ANSI redraw, history (in-memory + `~/.mylang_history`), Up/Down.
  Replaces getline. Unit-test the core with keystroke scripts.
- **Phase 2 — Live syntax highlighting.** Re-lex the buffer each redraw →
  colored tokens; color the `=>` result and error carets. A shared
  `highlight.{h,cpp}` (source → ANSI) that `-a`/`--analyze` can reuse.
- **Phase 3 — Multi-line block editing + auto-indent.** Promote the editor to
  a true multi-line widget: keep an incomplete block open, Up/Down move between
  its lines, the whole block re-renders, auto-indent by depth, submit on a
  complete parse. The headline IRB differentiator and the v1 finish line.

**Deferred past v1** (designed here, built later): **Phase 4 — autocompletion**
(`complete()` from KwString + the `builtins`/`const_builtins` maps + persistent
symbols + struct fields after `.`; eventually an IRB-style dropdown); **Phase 5
— polish** (Ctrl-R reverse search, bracketed paste, pretty multi-line value
printing, more meta-commands, themes/`NO_COLOR`, and the **Windows** raw-input
backend — v1 is Unix-only).

## 5. File layout & touched passes

- `src/repl.{h,cpp}` — `ReplEngine`: the persistent const/global/type stores,
  retained ASTs, `eval_input`, `=>` printing, meta-commands.
- `src/lineedit.{h,cpp}` — `LineEditor` pure core + the `tty_io` platform shell
  (Unix termios in v1; an `#ifdef`'d Windows backend later).
- `src/highlight.{h,cpp}` — source → ANSI tokens (shared with `-a`).
- `src/mylang.cpp` — launch the REPL when no FILE/`-e` and `isatty(0)`
  (Ruby-like); piped stdin runs as a script. Factor `dumpLocInError` into a
  shared `errfmt.{h,cpp}` so the REPL reuses caret rendering.
- **`src/inferencer.{h,cpp}`** — add an *incremental* entry point: infer one
  input seeded with a persistent global `TypeSym` table + `STyArena`, commit
  new global types, and defer (not error) a still-unconstrained global. The
  bulk of the engine work.
- **`src/resolver.{h,cpp}`** — a REPL mode that treats the top-level block's
  decls as persistent globals (map / "escaped") rather than "main"-frame slots,
  while still slotting nested locals.
- Tests: `repl:` / `lineedit:` / `highlight:` sections in `src/tests.cpp`.

## 6. Multi-line "is the input complete?" detector

A pure function `multiline_state(src) -> {complete, incomplete, error}`:
- track `(){}[]` depth and open string literals via the lexer;
- depth > 0, or a line ending in a binary operator / `=>` / `,`, or a `func`/
  `if`/`while`/`for`/`foreach`/`try`/`struct` header without its body →
  *incomplete*;
- otherwise attempt a parse: a `SyntaxErrorEx` at `invalid_tok` (EOF) →
  *incomplete*; any other parse error → *error* (show it); clean → *complete*.
Reused by both the cooked-mode loop (Phase 0) and the editor (Phase 3).

## 7. Testing (no TTY required)

The whole point of the "pure core behind a thin shell" split:
- **Editor**: feed a `vector<byte>` keystroke script + a width; assert final
  buffer, cursor, and (optionally) the emitted ANSI. Covers cursor math, kill
  keys, history nav, completion acceptance, multi-line motion.
- **ReplEngine**: feed a sequence of input strings, assert the concatenated
  output / `=>` lines / errors — e.g. `["var x = 5", "x + 1"]` ⇒ `=> 6`;
  `["func f(a)=>a*2", "f(21)"]` ⇒ `=> 42`; redefining `x`; a runtime error
  then continuing. This is the persistence regression net.
- **highlight()/complete()/multiline_state()**: pure in→out assertions.
Only `tty_io` (a few dozen lines of syscalls) is not unit-tested — it is kept
deliberately trivial.

## 8. Decisions (confirmed with the maintainer)

1. **REPL typing → faithful, not dynamic.** Run the real, full pipeline per
   input over an expandable global scope (§3.1); strict checks apply within an
   input; an unconstrained global is *deferred-unknown* across inputs, not an
   error. The REPL is an inspection tool for the true interpreter.
2. **Platforms → Unix-only for v1.** Asymmetric is fine; Windows deferred.
3. **Scope → through Phase 3** (persistent faithful engine + raw editor + live
   highlighting + multi-line block editing). Autocomplete/polish deferred.
4. **Launch → no-args + TTY** starts the REPL; piped stdin runs as a script.

## 9. Risks / gotchas

- Restore the terminal on *every* exit (RAII + signal handlers); a crashed
  raw-mode process wrecks the user's shell.
- SIGINT must cancel the current line, not the process; SIGWINCH must trigger a
  reflow.
- Const-eval runs pure code at *parse* time — a `const X = pure_call()` in the
  REPL executes during parsing; a failure must abort just that input, not the
  loop (catch around the parse, too).
- REPL `func` capture must point at the persistent global context (forward-ref
  / mutual-recursion across inputs).
- Keep the highlighter allocation-light: it runs on every keystroke; re-lexing
  one logical line is cheap, but avoid per-char re-highlighting of a huge
  paste (bracketed paste → one batch).
- **Retained ASTs grow unbounded** over a session — acceptable (a REPL is
  short-lived), but `:reset` should drop them; a redefined global shadows the
  old in scope, the old node only staying alive to keep existing
  closures/structs valid.
- **Pinned types vs. redefinition**: a committed global's type is *pinned* for
  later inputs (an `x = ...` assignment must conform — the §3.1 corner case);
  only a *re-declaration* (`var x = ...`) rebinds it. Already-evaluated inputs
  are never retro-typed — only future inputs see the new binding (incremental
  compiler).
- **The input's own transient frame** still slots its locals (64-cap like any
  call); only the *global* scope escapes to the map, so the cap never bites
  accumulated globals.
