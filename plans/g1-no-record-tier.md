# G1: the NO-RECORD call tier (lazy frame reconstruction)

**Status: DESIGN ONLY. Nothing here is implemented.** Written 2026-08-09 at
the maintainer's request, after the reach measurement in
docs/jit-optimizations.md ("G1 fork REACH") showed the leaf-only version
misses recursion entirely.

The one-line claim this document argues:

> For a call the JIT emitted, the **hardware return address already
> identifies the call site**, and everything the frame record stores about
> that call site is a COMPILE-TIME CONSTANT of it. So the record does not
> have to be written at call time - it can be REBUILT, from the native
> stack plus a pc-keyed side table, on the rare paths that ask.

---

## 1. The problem, concretely

    func sumto(n) {
        if (n == 0)
            return 0;
        return n + sumto(n - 1);
    }
    var r = sumto(900);

Today every one of those 900 calls writes a **`VmCallRec`** - 136 bytes,
17 fields (vm.cpp) - and the pop tears it down. Measured on the caller
side: **51 of 136 executed instructions are the record fill plus its
mutations**, and this is the shape that pays it per level with nothing to
amortise against. `10_recursion_deep` makes 1,353,000 such calls.

What C++ does for the same function is push a return address.

## 2. What the record actually holds, and who reads it

Grouped by CONSUMER, because that is what decides whether a field can be
rebuilt rather than stored:

**REBUILDABLE - a compile-time constant of the CALL SITE:**
- `ret_chunk`, `ret_pc` - read by the pop (resume), the unwind and the
  backtrace.
- `dst` - the pop's "where the result goes"; the emitted store can bake it.
- `desc` - the backtrace only.
- `call_site_packed` - the backtrace, on a deleted run.
- `run_chunk` - the unwind's handler search; the callee is known at the
  site.

**NOT NEEDED AT ALL** - `handler_base`, `diter_base`, `dyiter_base`,
`pend_base`, all read by the pop to trim watermarks. `plain_frame` already
proves the callee moves none of them, which is exactly why that gate costs
0% reach: every corpus call already has it.

**GENUINE RUN-TIME RESIDUE:**
- `window`, `nslots`, `seg`, `seg_top_before` - the pop releases the
  window. Only `seg_top_before` must survive the call; the rest are
  derivable from it plus the callee's own slot count.
- `caller_captures` - the pop restores `ctx.captures`.

**EXCLUDED FROM THE TIER** - `cache_key`, `caller_cache` (the pure-call
cache; this is the documented `09_fib_recursive` decline). And
`boundary` / `sync_stop` do not arise: a record-less frame is neither.

**The result: of the 17 fields, 11 are compile-time constants of the call
site, 4 are the watermarks `plain_frame` already makes unnecessary, and
only `seg_top_before` + `caller_captures` are real run-time residue.**

## 3. The mechanism

### 3a. The side table (compile time, derived, never serialized)

At JIT emit time, for each emitted call site, record

    native return offset  ->  { ret_chunk, ret_pc, dst, callee desc }

keyed by the offset of the instruction AFTER `call`. That offset is exactly
what the hardware pushes. The table is a pure function of the emitted code,
so it is **derived**: recomputed on every (re-)compile and NOT stored in a
`.myv`, precisely like `Chunk::native_leaf` and `boxed_ops` already are.
(docs/myv-format.txt's "derived pools are not stored" rule applies as-is.)

### 3b. The call, emitted

    ; today: ~51 instructions of record fill, then call
    ; proposed:
    push  <seg_top_before>        ; the 2-field residue, on the native stack
    push  <ctx.captures>
    call  <callee entry>
    pop   <ctx.captures>
    pop   <seg_top_before>        ; restores the window watermark
    mov   [frame + DST], rax      ; DST is a baked constant

The residue that could not be rebuilt lives in the caller's own native
frame - two pushes instead of a 136-byte structure.

### 3c. Reconstruction, on the paths that ask

Walking the native stack from the current frame gives an ordered list of
return addresses; each maps through the side table to a full logical frame.
That is precisely what a `VmCallRec` would have said, recovered after the
fact. Three consumers:

**Normal return** - asks nothing. The `ret` lands at the call site, which
stores into its baked `dst`. This is the 99.99% path and it is why the
design pays.

**Exception unwind** (`vm_unwind_walk`, vm.cpp) - the hard part, see §5.

**Backtrace** - the same walk, but it only needs `desc` + the call-site
loc, both of which the side table has directly.

## 4. Which calls qualify - the gate

    plain_frame                 the callee moves no handler/iterator
                                watermark  (100% of corpus calls)
  + fixed arity                 no skipped trailing `opt` param
                                (free - excludes nothing)
  + no pure-cache interaction   not a CachedCallV, no live PureCache
                                (this is fib's documented decline)
  + JIT-emitted call site       the C++ interpreter tier keeps records

**NOT required: leaf.** That is the whole point of this version, and the
reason to build it rather than the easy one:

    measured reach          leaf-only    lazy-reconstruction
    share of corpus calls      58.2%           up to 99.8%
    10_recursion_deep           0.0%           100.0%
    09_fib_recursive            0.0%             0.0%  (pure-cache)
    69_exc_crossframe           0.0%           100.0%  (but see below)

`plain_frame` is 100% of the corpus, so the ceiling is 100% minus the
pure-cached calls (10,696 of 6,953,702 = 0.15%).

**The 69_exc_crossframe row is a gate answer, not a safety claim.** Its
callees are `plain_frame` (the `try` lives in a different function), so
they qualify - which means it is the bench that would stress §5's
reconstruction hardest, on 340,000 unwinds. Read it as "this is where the
bugs will be", not as 340k free calls.

**The ceiling is corroborated, and it did not have to be.** The gate is
measured with the JIT OFF, so by itself it cannot say whether a qualifying
call site is one the JIT actually emits - and a call the JIT never emitted
keeps its record regardless. Combining the two measurements answers it:

    bench                JIT-off calls   JIT-on inline pushes   emitted
    10_recursion_deep        1,353,000            1,352,549      99.97%
    45_gcd                     149,999              149,998     100.00%
    76_funcval_dispatch      1,000,000              999,999     100.00%
    78_typed_param_call      2,000,002            2,000,001     100.00%
    09_fib_recursive            10,696               10,687      99.92%

So on these shapes essentially every call is JIT-emitted, and the gate's
number is the real number. That is a corroboration of the ceiling, not a
proof for arbitrary programs.

## 5. What is hard, stated plainly

**The unwind walk becomes a MIXED traversal, and that is the whole risk.**
Today it is a loop over `act.records[]`:

    for (;;) {
        VmCallRec &cur = act.back_rec();
        if (vm_dispatch_exc(...)) return true;   /* a handler here */
        if (cur.boundary) { ...; return false; }
        vm_capture_rec_frame(*ex, cur);          /* backtrace */
        chunk = cur.ret_chunk; pc = cur.ret_pc - 1;
        act.pop_window();
    }

With record-less frames interleaved, the walk must visit BOTH stacks in
the correct interleaved order. Two things make this tractable:

1. **A record-less frame can never catch.** It is `plain_frame`, so
   `n_trys == 0`, so `vm_dispatch_exc` could never dispatch into it. The
   walk only has to *account* for such a frame (backtrace + window
   release), never search it.
2. **The order is recoverable** if each record-ful frame stores the native
   stack pointer at which it was pushed: frames whose native SP lies
   between two records belong between them. That is ONE extra field on
   the record - paid by the record-ful minority, not the hot path.

Still genuinely hard, and the honest risks:

- **Native stack walking is the least portable thing in the codebase.**
  It needs either frame pointers (`-fno-omit-frame-pointer` in the emitted
  prologue - a real cost on every qualifying call) or an unwind table.
  The emitter controls its own prologues, so this is a decision, not a
  constraint - but it must be made explicitly.
- **The 5-mode differential cannot see a wrong backtrace unless a test
  asks for one.** Every existing caret/backtrace test must be re-run
  against a program whose frames are record-less, which today none are.
  Expect this to be where the bugs are.
- **`ML_VM_CHECK`'s per-frame invariants** currently read the record. They
  need a record-less analogue or they silently stop checking - the audit
  table trap in its usual form.
- **Two frame representations coexist** (the C++ interpreter tier keeps
  records). That is a permanent maintenance cost, and it is the strongest
  argument AGAINST the design.

## 6. What it should buy, sized honestly

Caller side today: 136 executed instructions - guards 63, record fill +
mutations 51, stack switch + call 15, status dispatch 5, epilogue 2.

This removes the 51 outright and replaces it with ~4 (two push/pop pairs).
It does not touch the 63 guards, which are the callee-cache's territory.
So the per-call figure goes to roughly **~90**, from 136.

**That is not enough for <=5x on its own, and the arc should say so.** On
`76_funcval_dispatch` the protocol is ~300 Ir per iteration against a ~260
budget at 5x; this takes it to maybe ~200. It gets the goal within reach
of the guard work rather than achieving it alone.

## 7. Build order, if approved

Each step is independently measurable and independently revertable:

1. **The side table, unused.** Emit it, assert it round-trips against the
   existing records (every record-ful frame's `ret_chunk`/`ret_pc`/`dst`
   must equal what the table says). Pure verification, zero behaviour -
   and it proves the central claim before anything depends on it.
2. **Backtrace from the table.** Render frames from the table instead of
   the record, still with records present. Any divergence is a rendering
   bug, caught while both sources exist.
3. **The mixed unwind walk**, still with records present (the native SP
   field, the interleave). Sabotage: force the walk to prefer the
   reconstructed frame and require byte-identical backtraces.
4. **Stop writing the record** for qualifying calls. Only now does
   anything get faster.
5. Measure, per shape and on the suite.

Steps 1-3 are the whole risk and buy nothing; step 4 is small and buys
everything. That ordering is deliberate - it front-loads the falsification.

## 8. What I would want decided before starting

- **Frame pointers or an unwind table?** (§5) This is the portability
  decision and it shapes step 1.
- **Is the two-representation maintenance cost acceptable** for a change
  that does not by itself reach the goal? (§6)
- **Does the pure-cached shape need to be rescued?** Today it is 0.15% of
  calls, so the answer is probably no - but it is also the ONLY shape
  excluded, so saying so explicitly closes the matrix.
