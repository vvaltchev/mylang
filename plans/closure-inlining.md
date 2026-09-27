# Closure inlining - a GUARDED splice at value-call sites (#97)

Status: **INCREMENT 1 DONE (2026-09-27)** - record and measurements in
docs/jit-optimizations.md, *#97 CLOSURE INLINING, INCREMENT 1*: 11
-28.6% cycles (57 -> 40 instr/iter, vs the ~15 predicted: the staging
and result MoveVs remain), 78 -8.5%, 63 -10.5%. Increments 2-5 not
started. Designed from measurement (2026-09-27). Maintainer's
call: this replaces the call-protocol store trimming (R5/R6 both measured
flat and are parked on `r5-offpath-framefree` / `r6-return-forward`).

## Why the protocol arc has stopped paying

R1-R4 took the four worst call benches down 8-15% in cycles each; R5 and
R6 then measured flat. What is left per call is the PROTOCOL ITSELF -
the pushed dst word and captures, the window, the frameless entry, the
return arm, the release scan - and each remaining store is worth a
percent or two. The body the protocol wraps is 3-4 ops. The next factor
comes from not making the call at all.

## Reach, measured first

Every hot call in the cluster is a `call.val` in MAIN whose callee set
the callee-set analysis (#116) has already NAMED - `Chunk::value_callees`:

    bench  site                     callees  body (ops)
    11     c()          pc18        1        load.capture / i.bin /
                                             store.cap / return.v
    78     add(i)       pc22        1        load.capture / i.bin / return.v
                                             (int k <- int: exact)
    78     scale_it(i)  pc25        1        same, float  (float x <- int:
                                             a WIDENING bind)
    63     c() x2       pc22, pc23  1        the counter body (as 11)
    63     add(i)       pc29        1        the adder body
    63     make_counter/make_adder  -        `call.v` to a FACTORY:
                                             move / make.closure / return.v
    76     fn(st, i)    pc27        2        load.elem.i / i.bin /
                                             store.elem.i / return.v

Cost today vs a hand-inlined twin (pinned P-core, `-npc`,
`OPT=1 ASSERTS=0`, scale-3 minus scale-1 delta per iteration):

    bench  instr/iter  cycles/iter   hand-inlined twin
    11       57          7.8           9 instr   0.8 cycles
    78      130         17.3          30 instr   3.8 cycles
    76      185         28.6          (not built)
    63      535         92.5          (not built: two allocations/iter)

The twins keep the captured value in a REGISTER, which a faithful inline
cannot: a closure's capture lives in its FuncObject and a mutable one
(11's `start++`) persists across calls. So the realistic ceiling for 11
is a load / add / store chain through memory (store-forward latency,
~4-5 cycles), not 0.8; for 78, whose captures are only READ, it is close
to the twin plus two loads per call.

**The existing splice reaches NONE of these sites**, for four separate
reasons, each of which the plan below removes:

 1. it rewrites `CallV` only - a callee in a GLOBAL slot;
 2. MAIN is never a splice caller (`vm_precompile_all` runs it over
    `g_func_chunks`, and main is built after), and every site is in main;
 3. `LoadCaptureV` / `StoreCaptureV` are not whitelisted: they read
    `ctx->captures`, which in the caller's frame is the CALLER's set;
 4. a typed parameter declines (`!fast_bind`).

(The `MYLANG_INLAUDIT` audit is blind to all of it for reason 2 - it
walks function chunks only.)

## The design: an inline cache, in BYTECODE

At a `CallValueV` site whose `value_callees` names exactly one
descriptor D with a splice-eligible body:

        GuardCalleeV   fn_slot, closure_defs[D] -> else Lcall
        <D's body, slots re-based into the caller's frame,
         capture ops rewritten to read THROUGH fn_slot,
         each ReturnV -> MoveV dst = result; Jump Ljoin>
        Jump           Ljoin
    Lcall:
        CallValueV     (the original op, unchanged)
    Ljoin:

Why bytecode and not the JIT: everything downstream then sees the
inlined form for free - the JIT's pins, lever A forwarding, LSRA, C3/C5
and the element tiers apply to the body as they do to any caller code;
`-nj` is faster too; a `.myv` image stores it; and the tree-walker stays
an INDEPENDENT oracle, because this transform runs below the AST (unlike
an AST transform, the five-mode differential is not blind to it).

**The guard is not optional, and it is not a candidate for elision.**
The analysis says D is the only callee, so the guard is unreachable in
a program we compiled - but it is what makes the splice sound on a
loaded image, whose `value_callees` is input (the #137 trust model), and
it is one perfectly predicted compare (the guard-elision ledger in
CLAUDE.md says what removing it would buy: nothing). Its miss arm is
the ORIGINAL call op, so every error, caret and backtrace on that path
is the un-inlined one by construction.

### The new pieces

- **`GuardCalleeV`** - `a` = the callee slot, `target2` = a
  `closure_defs` index, `target` = the else-pc. Hit iff the slot holds a
  `t_func` whose `FuncObject::func == closure_defs[target2]`. Joins
  `visit_pc_fields`, `visit_use_def` (reads `a`, writes nothing),
  `verify_chunk` (pool + slot + pc bounds), the #98 census (all six
  columns), the disassembler, the splice whitelist, and the JIT (`cmp`
  the type word, load `->func`, `cmp` the baked descriptor - the E3
  two-way site already emits exactly this compare).
- **`LoadCaptureOfV` / `StoreCaptureOfV`** - the capture ops with an
  EXPLICIT closure: capture `i` of the FuncObject in slot `fn_slot`
  (its `CaptureSlots::data()` is the first member, the same layout
  `emit_ctx_chain_r9` relies on). The spliced body's `cap[i]` ops are
  rewritten to these; the originals stay what a real call runs.
  `cap_scalar` (0x40) carries over, so the store's reference arm is
  emitted only where it can be taken. Same six tables as above.
- **Main becomes a splice CALLER** - for value sites first. Admitting
  main's `CallV` sites too is a separate, measured step: it changes the
  bytecode of every program with a top-level call.
- **A widening bind** - `CoerceNumV` (exists) as the bind for a
  `float` parameter receiving an int, exactly what `bind_param` does;
  an exact bind is the plain `MoveV` the splice already emits. Anything
  else a typed parameter would do still declines.

### What stays declined

Everything the splice declines today (`plans/bytecode-inliner.md`: try
regions, frame iterators, a `CachedCallV`, the frame budget, anything
off the whitelist), plus:
 - a site with `value_callees` ⊤ or ⊥ (no named callee);
 - a TWO-way site in increment 1 (76 is increment 2);
 - a body that CREATES a closure (`MakeClosureV`: its `closure_defs`
   index would need re-basing - increment 3);
 - a site inside a spliced body (one level, as today).

## Increments

1. **Mono value sites, capture ops, main as a caller.** Reaches 11,
   78 (both sites, via the widening bind) and all three of 63's closure
   calls. Expected: 11 57 -> ~15 instr/iter, 78 130 -> ~40.
2. **Two-way sites** (a second guard falling through to the second
   body, then the call) **+ the element ops** `load.elem.i` /
   `store.elem.i` on the whitelist, with their `base_locs`. Reaches 76.
3. **`MakeClosureV` in a spliced body** (`closure_defs` re-basing -
   the splice's own planned increment 5). Splices 63's two factories
   into main, so main itself holds `c = make.closure D`.
4. **Guard elision by a LOCAL proof, not by the analysis** - when the
   callee slot's only reaching definition in this chunk is a
   `make.closure D` (increment 3's output), the guard is statically
   true. A local dataflow fact, so it is also sound on an image.
5. **Scalar replacement of a non-escaping closure** - 63 allocates two
   FuncObjects per iteration only to call them and drop them. A closure
   that never escapes the frame needs no object: its captures become
   frame slots. Its own design, to be measured (allocation share of 63)
   before it is written. It is also what lets 11's capture live in a
   register.

## Testing

 - **Five-mode `-rt` entries per shape** (mono, widening bind, mutable
   capture persisting across two calls to one closure AND across two
   closures from one factory, a reference capture, an error inside the
   inlined body - backtrace byte-identical to `-nbi`) plus a
   `tests/functional/` program; `corpus_diff` covers `-nbi` as a layer.
 - **An emitted-code counter** for spliced value sites and one for guard
   HITS (bumped by emitted code), so a test cannot pass on the call.
 - **The miss arm needs a FORCE lever** - it is unreachable in our own
   compilation, so a TESTS knob that makes every guard compare against
   a wrong descriptor, run over the corpus, is the only way to execute
   it (the `MYLANG_JIT_FORCE=bakecallee` precedent).
 - **An image test**: a `.myv` whose `value_callees` is tampered to name
   the wrong closure must run correctly through the miss arm.
 - **The inline audit learns main and value sites**, reporting each
   decline by reason, so reach is a measured number before and after.
 - **Sabotage, watched failing**: a missed capture rewrite (the body
   reads the caller's `ctx->captures`), a wrong slot re-base, a guard
   that always hits.

## Sibling cases, stated so they are not rediscovered

 - float-returning bodies ride the same path (78's scaler);
 - a body with a `dyn` capture or parameter: the boxed ops (`bin.v`) are
   not on the whitelist - declined, and counted by the audit;
 - `CachedCallV` sites stay excluded (the pure cache keys on the call);
 - a callback through a HIGHER-ORDER BUILTIN is not a call site at all -
   not reachable by this design (the callback protocol, CB1-CB8, is
   that path's floor).
