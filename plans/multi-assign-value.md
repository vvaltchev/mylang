# The value of a multi-assignment - a design TODO

**Status: deferred by the maintainer on 2026-10-06. Not designed.**
Filed in `plans/language-deferred.md`. Until this is built a
multi-assignment evaluates to `none` in every engine (README, the
assignment bullet under the language overview).

## Today

`a, b = [5, 6]` is an `Expr14` whose target is an `IdList`. Its value is
reachable in two positions the grammar accepts:

    w = a, b = [5, 6];        # parsed as w = (a, b = [5, 6])
    return a, b = [5, 6];

(`(a, b = [5, 6])` in parentheses does not parse - `Expected ')'` - and
`f(a, b = x)` is a call with two arguments, so there is no other.)

The value is `none`: `Expr14::do_eval` returns it, `type_of` types it
`none` (so `array<int> w; w = a, b = [5, 6];` is a compile-time
NullabilityEx) and codegen compiles the statement and then loads none.
Until 2026-10-06 the tree-walker gave none, the VM refused the form
(NotLoweredEx) and the inferencer typed it as the right-hand side, so a
typed `w` compiled and held none (RULE 1).

## What the maintainer asked for

"A temporary list of elements which have their lvalue pointing to the
local variables": the value is a list with one element per target, and
each element IS the target - reading it reads the variable, writing it
writes the variable.

## Why it is not a small change

MyLang has no value that refers to a variable. `t_lval` (an `LValue *`
in an `EvalValue`) exists only inside the tree-walker's evaluation, is
collapsed by `RValue()` before any value is stored, and is invisible to
scripts. A list of such elements would be the first one that escapes:

- **Lifetime.** `return a, b = [5, 6];` with `a` and `b` locals hands
  the caller references into a frame that is gone - a dangling
  reference, i.e. undefined behavior (RULE 1). So either the list must
  not outlive its targets (a rule the compiler enforces), or it must
  keep them alive (closure-style boxing of the variables, which changes
  how every captured/referenced local is stored).
- **The VM and the JIT.** A local's slot is not always where its value
  is: the JIT keeps hot locals in registers (the N5/C2a pins, the linear
  scan), a frameless callee's window lives on the native stack, and a
  bytecode-inlined callee's locals are the caller's slots. A reference
  to a slot would need every one of those tiers to write the register
  back before the reference is taken and to reload after a write
  through it - the same problem a debugger's "write a variable" has.
- **Globals and captures** are separate storage (the global table, a
  closure's capture slots), so an element would need to say which.
- **Types.** The list's element type is the targets' types; with mixed
  targets it is `array<dyn>`, and a typed read through it would need
  the per-element static type to keep every unboxed tier sound.
- **The stored image.** A new value kind needs a `.myv` encoding, or a
  rule that it never reaches a constant pool.

## Questions to settle first

1. **Is the aliasing observable after the expression?** `w = a, b =
   [5, 6]; w[0] = 9;` - does that set `a`? If the list is a TEMPORARY in
   C++'s sense (it exists until the end of the full expression, and
   storing it copies the values out), then with today's grammar the
   aliasing is NEVER observable: both positions above store or return
   the value. The feature would then be exactly "a fresh array of the
   stored values" - cheap, and the references would matter only if a
   form like `(a, b = x)[0] = 9` becomes parseable.
   If instead the list keeps referring to the variables after it is
   stored, it is a reference type, with the lifetime problem above.
2. **What a compound multi-assignment yields** (`a, b += [1, 2]`): the
   targets after the store, like a single compound's value?
3. **A spread** (`a, b = 7`) and **`_`** (`a, _ = [1, 2]`): does the
   list have an element for `_`, and what is it?
4. **Typing**: `array<T>` of the joined target types, or `array<dyn>`?

## Pointers

- tree-walker: `Expr14::do_eval` (eval.cpp), the IdList branch.
- inferencer: `type_of`, the expr14 arm (`is_idlist()` -> none).
- codegen: `compile_boxed_expr_impl`'s Expr14 arm (the value form) and
  `try_multi_literal_store` / `try_multi_scalar_spread` /
  `try_multi_unpack` (the statement form, all of which require LOCAL
  targets - a global or capture target is still a NotLoweredEx, open).
- test: tests/functional/73_multi_assign_value.my.
