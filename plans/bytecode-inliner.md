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

**The remapper, with the layouts already verified** (from each op's emit
site + `visit_use_def`, so the next session does not re-derive them):

    IntBin FloatBin + every specialized RR/RI + IntModRI + IntAddModRI
    CmpIntV CmpFloatV        a, b, target        (IntAddModRI's target2
                                                  is the IMM - leave it)
    JumpUnlessIntCmp/Float   a, b                (target is a pc)
    Jump                     nothing             (target is a pc)
    LoadImmInt/Float         target              (a is the literal)
    MoveV                    target, target2
    ReturnV                  a
    CallV                    target, a_lit(+base = the ARG RUN base)
                             -- target2 is a GLOBAL slot: do NOT remap

`ForLoopStep` and `IntAddStep` were dropped from the whitelist rather
than guessed at: their `target` is a pc and their `target2` a counter
slot, but the operand layout wants checking against their emit sites
before they carry a splice. Dropping them costs reach, nothing else.

**And the net that makes this safe without a second audited table:**
after remapping an instruction, run `visit_use_def` on it and ML_CHECK
that every slot it reports is `>= base`. The callee's own slots are all
`< callee_frame`, so any field the remapper MISSED is still below `base`
and the check fires. That uses the existing audited table as a CHECKER
instead of duplicating it - duplication being exactly how the other
tables drifted.

## STATUS: the splice is WRITTEN and OPT-IN (`-bi` / MYLANG_BCINLINE=1)

It produces correct bytecode. On `func sumto(n) { if (n==0) return 0;
return n + sumto(n-1); }` it emits the textbook form (`-vd`, one level):

       3  i.bin        r1 = r0 - 1
       4  move         r4 = r1          <- the arg bind
       5  i.jmp.ifnot  r4 == 0, L9      <- the body, slots +4
       6  load         r5, 0
       7  move         r2 = r5          <- ReturnV -> move to the call's dst
       8  jmp          L13              <- ... and jump to the join
       9  i.bin        r5 = r4 - 1
      10  call.v       r6 = g0(r5)      <- the recursion, now 2 levels down
      11  i.bin        r7 = r4 + r6
      12  move         r2 = r7          <- the TAIL return just falls through
      13  i.bin        r3 = r0 + r2     <- the join
      14  return.v     r3

and it runs correctly with the JIT off. **All three defects below are now
FIXED (2026-08-02), and NONE of them was the splice's own bug** - one was
a pair of interpreted-path backtrace bugs, one a JIT remap silently
disabled 39 commits earlier, one this pass's own reproducibility. With
the splice FORCED ON the whole battery is green: `-rt` 1680/1680 +
1486/1486, the fuzzer 200/200 with 0 diverged, and every sample
byte-identical to the splice-off run (bar `rand_sort`, which uses
`rand()`). It remains DEFAULT OFF pending a flip decision, which is a
value question rather than a correctness one - see the reach note.

The three, as found:

**(1) [NOT THE SPLICE'S BUG - root-caused and mostly FIXED 2026-08-02 in
`9fe3783` + `c1310ed`; the JIT residual moved to
plans/jit-backtrace-frames.md.]** The
first reading was that a spliced frame's virtual backtrace frame was
dropped by `vm_unwind_walk`'s once-guarded flush. The "splice" half was
wrong; the "guard" half turned out to be RIGHT, but for the interpreted
path rather than the one being looked at:

  - **The splice's backtrace is CORRECT.** With the JIT off, `-bi` and
    `-nbi` and `-tw` all render a throwing `sumto(4)` byte-identically
    (6 frames). The 5-vs-6 reading came from comparing two JIT-ON runs.
  - **The divergence is JIT-only and PRE-EXISTS the splice entirely.**
    It reproduces on the DEFAULT build with the splice off, using only
    the AST inliner's recursion unroll:

        func f(n) {
            if (n < 2) return 1 / (n - n);
            return f(n - 1) + f(n - 2);
        }
        print(f(5));

    tree-walker and `-nj` both render 6 frames; the JIT renders 4, and
    its last frame reads `main() at line 4` where it should read `line
    6`. So a frame is not merely missing - one is MISATTRIBUTED.
  - **BUT the guard was real on the interpreted path.** Comparing `-tw`
    against `-nj` (not two JIT runs) at depths 2..7 showed the VM
    diverging at every depth, for TWO independent reasons - a loc-less
    throw from a folder-synthesized divisor, and the once-guarded flush
    dropping every call site's frames after the first. The first is
    fixed at its origin in `9fe3783` (#76), the second in `c1310ed`;
    one two-depth parity test pins both.
  - **The JIT residual survives**, and its cause is not the guard: once a
    run's originals are deleted every op collapses onto the head
    `EnterNative` (54 loc entries at `pc0` in this program), so every
    pc-keyed side-table lookup on the JIT's exception path is degenerate.
    The designed answer is baking, which today covers only the raise site
    and only when the op has a chain. Design + acceptance test:
    **plans/jit-backtrace-frames.md**.
  - Attempting a pc lookup at `EnterNative` was tried and REJECTED - it
    recovered a frame on one shape by luck and broke another.

The missing-test gap is closed: `inlined_recursion_backtrace_parity`
(tests.cpp) pins this shape on both engines. It forces the JIT off for
now; removing that line is the acceptance test for the residual.

**(2) [ROOT-CAUSED AND FIXED 2026-08-02 - NOT THE SPLICE'S BUG.]** A
chunk with two splices dispatched a garbage opcode under the JIT (UBSan:
`index 190 out of bounds for type 'void *[128]'`).

The JIT's rebuild inserts an `EnterNative` head per run, so every
surviving pc moves and a surviving branch op must have its target
remapped through `entry_remap`. Eleven branch opcodes shared ONE remap
body BY FALL-THROUGH, and #78 step D (`f0391fb`) deleted the
`case OpCode::CatchTest:` label that body was attached to - silently
turning all eleven into no-ops. A stale target then points into the
pre-insertion pc space.

WHY NOTHING CAUGHT IT FOR 39 COMMITS: instrumenting the restored remap
showed the default corpus DOES hit it - 39 stale targets across `-rt`,
zero across bench/ - but every one was off by only ONE OR TWO ops and
none landed out of range, so no test observed a difference. The splice is
what makes it fatal: it produces a chunk that KEEPS interpreted branches
(the CallV islands stop the run being deletable) while shortening the
code enough that a stale target lands past the end.

The differential's failure here is a COVERAGE gap, not a structural one,
and the distinction matters: the tree-walker has NO JIT path (the only
pipeline is AST -> bytecode -> native), so tw-vs-VM is already a JIT-free
oracle against a JIT-on engine and CAN catch a JIT miscompile. It missed
this one because in the default config nothing was observable - an
in-range stale target lands on a preceding op or an inserted EnterNative,
re-enters interpreted and computes the same answer - and because the
configuration that goes out of range needs `-bi`, which is default-off.
Running `-rt` with the splice forced on is what closes it.

Fixed at the origin (`f0391fb` amended - exp-work only, 39 back, the
hunk untouched since). Two nets on top: an ASSERTS-only invariant that
every surviving branch target is in range, asserted right after the
rebuild, and `bc_inline_two_splices_jit`, which runs ackermann as
tw / vm / vm+splice and requires all three to agree. Both were verified
failing with the fix reverted (the invariant aborts at the site; with
`ASSERTS=0` the test reaches the original garbage dispatch).

**(3) [FIXED 2026-08-02.] The pass order was NON-DETERMINISTIC** - it
iterates `g_func_chunks`, an unordered_map, and read each callee's body
LIVE, so whether a callee was seen before or after its own splice varied.
Never a correctness hazard (a spliced callee gains `inline_ctxs`, which
the gate rejects, so the worst case was a MISSED inline) but a
reproducibility one, and CLAUDE.md pins "compiling twice is
byte-identical".

Fixed with the first of the two options - **snapshot every body up front**
(`BcInlineSnapshot`, taken for every chunk before the first splice, and
carrying the gate's verdict on the PRISTINE body). Chosen over sorted
iteration because it makes the pass order-INDEPENDENT rather than merely
order-stable, and because it makes the "ONE level, from a snapshot"
contract in the header true rather than accidental. It also converges on
the BETTER of the two orders: every caller now inlines the pristine
callee, where the unlucky order used to decline.

PROVEN, since re-running a binary could not show it - the map order is
stable on this machine, so both `samples/gcd` and 40 repeat runs agreed
either way. A temporary reverse-order switch in both splice loops made it
visible, and the shape matters: **a two-level chain cannot show it** (the
callee is a leaf; splicing it changes nothing), so `gcd` "passed" on the
broken build. It takes a three-level chain `a -> b -> c` where the middle
function is both caller and callee. Pinned by
`bc_inline_order_independent` (tests.cpp), which splices one program in
both orders and compares all resulting bytecode; it asserts the splice
actually FIRED (a pass that inlines nothing agrees trivially) and reports
the bug's fingerprint - the two orders splicing a DIFFERENT NUMBER of
sites. Verified failing with the fix reverted.

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

## Coverage (2026-08-02) - what is exercised, what is not

Method: `cmake -DTESTS=1 -DGCOV=ON -DASAN=OFF`, then `-rt` (all three
differential modes), `-rt` again with `MYLANG_BCINLINE=1`, and every
sample with the splice on. `codegen.cpp` overall: **88.2% of 5032 lines**.

The splice's own gates were the interesting part - several had NEVER been
taken, i.e. written from the spec and never executed, which on this
codebase is the same risk class as an unclassified opcode.

NOW TESTED (`bc_inline_decline_gates`, which also asserts a CONTROL shape
DOES splice so it cannot pass vacuously):
  - a call omitting a trailing `opt` param (the bind loop only moves what
    was passed);
  - a callee whose frame would push the caller past
    `BC_INLINE_MAX_FRAME`.

UNREACHABLE BY DESIGN - deliberately NOT tested, and annotated in the
source so the next reader does not "fix" the coverage:
  - `bc_remap_slots`'s `case OpCode::ReturnV`: the emit loop rewrites a
    ReturnV and `continue`s before it can arrive. Kept because the
    `default:` ABORTS - this is a whitelist, so an entry that costs
    nothing is cheaper than an abort.
  - the `ReturnV yields a LITERAL` gate: codegen always emits
    `LoadImmInt` + `ReturnV <slot>`, so `return 7` produces a slot. A
    test would have to fake a chunk to reach it.

NOW TESTED (second pass, same day):
  - **the caller-frame path** - a splice into a caller that ALREADY
    carries `inline_ctxs` entries, i.e. the AST inliner and the bytecode
    splice meeting in one chunk. The fuzzer never generates it; it needs a
    caller holding both an AST-inlined call and a spliceable one.
    `bc_inline_caller_with_frames` checks the VALUE in every JIT/splice
    combination, checks a throw from inside the spliced callee renders a
    backtrace byte-identical to the tree-walker's, and asserts
    `g_bc_inline_caller_frames` moved so it cannot pass unexercised.
  - **the corpus audit** (`bc_inline_audit_reports`), asserting all three
    verdicts it can print - OK, `runtime-callee`, and `op:<name>` - by
    building a program that elicits each. A measurement tool that silently
    reports the wrong thing is worse than one that does not run, since its
    histogram is what this feature's scope was decided from. Verified by
    sabotaging the reason string and watching the test fail.
  - **the frame budget**, properly this time. The first version used a fat
    CALLEE, which `BC_INLINE_MAX_OPS` rejects long before the frame check -
    so it passed while measuring the wrong gate. It now uses a fat CALLER
    and a 2-op callee, and the audit confirms that callee is reported `OK`
    (i.e. the decline really is the budget).

THE TAIL-TARGET REWRITE IS GONE, not merely uncovered. It mapped a callee
branch pointing past the end onto the join, which LEFT THE CALL'S DST
UNWRITTEN - the caller then read a stale value where the callee returns
`none`. That was a real miscompile, reachable until the trailing-Halt fix
(see CLAUDE.md). The requirement is now stated DIRECTLY as a gate
(`branch-past-end`, verified to reject on its own with the Halt fix
temporarily reverted) and the rewrite ASSERTS instead of "handling" it -
the handling was the bug, so a revived shape should abort loudly rather
than silently miscompile.

REMAINING UNCOVERED, all deliberate: the `empty` and `branch-past-end`
gates and `bc_remap_slots`'s `ReturnV`/`default` cases (unreachable by
construction - a whitelist's belt-and-braces), the literal-return gate
(codegen always emits a slot), and the audit's `unresolved-callee`.

## The counted-loop fusions (lifted 2026-08-02)

`ForLoopStep` and `IntAddStep` were excluded from the whitelist with the
note that "the operand layout wants checking against their emit sites
before either carries a splice". That audit is done and both are in. The
layouts, read off the VM cases and cross-checked against `visit_use_def`
and the pc-field table:

    ForLoopStep   target  = a PC (visit_pc_fields owns it - NOT a slot)
                  target2 = the COUNTER slot
                  a       = the bound operand, b = the step operand

    IntAddStep    target  = a PC
                  target2 = the COUNTER slot
                  b       = the added value operand
                  a       = a DUAL, and NOT uniform:
                              lo = accumulate dst - ALWAYS a slot
                              hi = the bound - a LITERAL VALUE when
                                   a_is_lit(), else a SLOT

**THE TRAP, and it is a nasty one:** `set_a_dual` CLEARS the is-literal
bits as a side effect. Writing the halves back without restoring the flag
turns a literal bound into a slot INDEX, and the loop reads its bound out
of whatever slot that number names.

Removing the restore was verified to corrupt the bytecode - `if r6 < 4`
became `if r6 < r4` - while the program STILL PRINTED THE RIGHT ANSWER,
because the slot it wrongly named happened to hold a value giving the same
result. The whole 1689-test suite passed with the defect in. That is why
`bc_inline_fusion_operands` asserts the FLAG structurally instead of
comparing values: a value oracle cannot see this class of corruption.

**REACH: no measured gain on the corpus, and that is the honest result.**
The capability is real (the shape matrix's `for` rows flipped from
must-decline to must-splice), but the blocker histogram over bench/ +
samples is unchanged at 3 OK sites - because nothing there was blocked by
a counted loop in the first place. What blocks it now:

    10  runtime-callee   - an indirect call; needs a GUARD (inline cache),
                           a separate and much larger step
     5  no-tail-return   - the callee falls off the end
     3  OK
     1  too-big

So the dominant blocker by far is the runtime callee, which is exactly
what plans/call-protocol-arc.md predicted and what 76/11/63 are made of.
Lifting the counted-loop exclusion was worth doing - it removes a whole
class of decline and the audit trail now says so - but it is NOT what
widens this feature; an inline cache is.

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
