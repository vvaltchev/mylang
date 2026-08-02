# The bytecode-level inliner (call-protocol lever 3)

Status: **SCOPED from measurement, implementation starting.** Written
2026-08-01, after the nested-read fusion landed.

## The reach, measured FIRST - and it is not what the lever assumed

plans/call-protocol-arc.md proposed this to fix the four worst benches.
Dumping their bytecode says that is **wrong for three of the four**,
because the calls they execute do not have a statically-known callee:

    76_funcval_dispatch  1 hot call, `call.val`  - callee from an array,
                         genuinely BIMORPHIC (alternates every iteration)
    11_closure_counter   1 hot call, `call.val`  - a closure in a local;
                         loop-INVARIANT, so monomorphic
    63_closures          5 hot calls: 2 `call.v` (the factories), 3
                         `call.val` (the closures)
    10_recursion_deep    `call.v`, self-recursive, depth 900
    08_func_call         no calls left - the AST inliner already took them

A splice-a-known-callee inliner reaches the `call.v` sites only. On the
value calls it is worth exactly nothing without a GUARD (an inline
cache: test the callee's descriptor, run the body, else call) - and 76
would need a bimorphic one. **That is a second, larger step; do not let
the lever's framing imply this one delivers those benches.**

## What it does reach, and why that is still worth doing

**`10_recursion_deep` - and it is the best target.** `sumto$0` is SEVEN
instructions and references NO POOL:

       0  i.jmp.ifnot  r0 == 0, L3
       1  load         r1, 0
       2  return.v     r1
       3  i.bin        r1 = r0 - 1
       4  call.v       r2 = g0(r1)
       5  i.bin        r3 = r0 + r2
       6  return.v     r3

Inlining it into itself k times divides the dynamic call count by k on a
bench that is 19.1x C++ and whose profile is call protocol. The AST
inliner cannot do this: its recursion unroll is for CACHEABLE TREE
recursion (fib-class, where the per-frame PureCache dedups the frontier),
and a linear accumulate is neither.

**Closure factories** (`63`'s two `call.v`) are the other case, and they
show why the gate below matters: `make_counter$0` is three instructions -
`move`, `make.closure closure_defs[0]`, `return.v` - so it is small but
NOT pool-free. It needs `closure_defs` re-basing before it can be spliced.

**The general gap over the AST inliner** is bodies the AST inliner refuses
for AST-SHAPE reasons - a nested function decl (`contains_func`, which is
exactly the closure-factory case) - and bodies that only become small
after specialisation. By bytecode time a nested function is just a
`closure_defs` entry, so the shape objection is gone.

## The corpus audit says the same thing (MYLANG_INLAUDIT=1, landed)

The gate + audit shipped first, DELAUDIT-style. Over bench/ + samples/,
**19 non-main call sites**:

    10  runtime-callee     CallValueV / CachedCallV - needs a GUARD
     5  no-tail-return
     3  OK                 splice-able today
     1  too-big

and the three are exactly what the hand analysis predicted:

    sumto$0  pc4  -> sumto$0  (7 ops)     the self-recursion
    sumto$s0 pc1  -> sumto$0  (7 ops)     the specialized entry
    lcm$0    pc4  -> gcd$0    (6 ops)     samples/gcd

**One thing the audit does NOT see: MAIN.** `vm_precompile_all` runs it
over `g_func_chunks`, and main's chunk is built afterwards by
`vm_compile`. That matters, because 63's two factory calls are in main -
so the transform will need main covered too, and the same v1 limitation
already applies to the #55 native call ("main has no stable descriptor
for the record's ret_chunk"). Count it as a known gap, not a surprise.

Read together with the reach section: the first increment's whole
measurable target is `10_recursion_deep`. That is a deliberate, narrow
landing, not an accident - and it is worth doing because that bench is
19.1x C++ on call protocol alone.

## The design, and the one hazard that shapes all of it

Splicing chunk B into chunk A means rewriting, in B's instructions:

  1. every **SLOT** field  -> + A's frame base (A's frame grows by B's)
  2. every **PC** field    -> + the splice offset
  3. every **POOL INDEX**  -> + A's base for that pool

(2) is solved: `visit_pc_fields` is THE audited pc enumeration and
already exists. (1) and (3) are the hazard, and it is the one this
codebase has been bitten by repeatedly - `visit_use_def`,
`op_writes_scalar` and `visit_pc_fields` are all recorded in CLAUDE.md as
having gone stale when an op was added. A missed SLOT field here is
silent frame corruption; a missed POOL index is a silently wrong
constant, member name or caret. There are ~20 pools and ~40 ops carrying
an index into one, in op-specific fields, with no audited table.

**So the gate is a WHITELIST, not a blacklist.** An op is splice-eligible
only if it is explicitly listed as "every slot and pool field of this op
is enumerated here". Anything else DECLINES the inline. A forgotten entry
then costs a missed optimisation instead of corruption - the failure
direction this has to have.

### Increment 1 - pool-free bodies only

Whitelist the ops that carry NO pool index and whose slot fields
`visit_use_def` already enumerates: the int/float arithmetic family,
`LoadImmInt`/`LoadImmFloat`, `MoveV`, the typed compares and branches,
`ForLoopStep`, `ReturnV`, and `CallV`. That covers `sumto$0` exactly.

`visit_use_def` is read-only (it takes `u`/`d` callbacks), so it cannot
remap. Rather than duplicate the table - which would then drift from it,
the exact failure mode above - generalise it to a MUTATING visitor and
have the liveness pass keep using the read-only form. ONE table, two
uses.

### Increment 2 - the return boundary

B's `ReturnV` becomes `MoveV dst = result` + `Jump` past the splice, the
bytecode twin of what `InlinedCallExpr` does in the AST. A body with
several returns gets several jumps to one join.

### Increment 3 - the backtrace

The virtual-frame machinery exists (`Chunk::inline_frames` + the pc-keyed
`inline_ctxs`), and #56 proved a raise can BAKE its chain rather than
resolve one from a pc. A spliced body's pcs get an `inline_frames` entry
naming B, so a throw inside it still renders B's frame. This must be
pinned byte-identically against the un-inlined form, per #75.

### Increment 4 - self-recursion

Bounded unroll depth, chosen like the AST inliner's (a body-weight
budget), with the recursive `CallV` inside the spliced copy left as a
call. This is what buys `10_recursion_deep`.

### Increment 5 - pools, starting with `closure_defs`

One pool at a time, each with its own whitelist entry naming the field
that holds its index. `closure_defs` first (it unlocks the factories),
`consts` second.

## What must NOT be inlined

  - a callee with try REGIONS (`n_trys != 0`): region ids are chunk-static
    and the handler table is indexed by them; merging two is its own step
  - a callee with dict/dyn ITERATORS (`n_dict_iters`/`n_dyn_iters`) - the
    per-frame pools are watermarked per call record
  - a `CachedCallV` site: the per-frame `PureCache` keys on the callee,
    and splicing the body away changes what is memoised
  - a callee whose frame would push the caller over the slot budget
  - anything the whitelist does not cover, by construction

## Testing

An optimisation the engine differential is BLIND to in the usual way? No
- this one is BELOW the AST, so the tree-walker really is an independent
oracle here, unlike an AST transform. But add all of:

  - a kill switch, so the same binary can run inlined vs not (the `-nj`
    pattern one layer up), and wire it into `opt_layer_equivalence`
  - `-vd` before/after on the corpus: the un-inlined dump must be
    unchanged for every program the gate declines
  - backtrace parity through a spliced body, byte-identical
  - `nested_fuzz.py`, which is what caught the N5 temp-caching bug
  - and, per the standing rule, REINTRODUCE a wrong slot remap and
    confirm a test fails
