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

**Backtrace** - the same walk. A frame shows (its function, the loc where
it was called). STEP 3b established exactly how each is recovered, and it
is NOT "the site has them directly" as first written - the site has the
CALLER, not the callee:
  - call-site loc = `site.site_loc` (directly, proven in step 2);
  - the frame's FUNCTION = `site(the frame ABOVE it).caller_desc` - the
    site records which function's chunk it was emitted in, so the frame a
    call creates is named by the site one closer to the top. `desc(N) ==
    site(N+1).caller_desc`; main's null descriptor closes the bottom.
Both `site_loc` and `caller_desc` are compile-time constants stored in
every build. The shadow walk (steps 3a/3b) verifies both against the
still-present records before step 4 removes them.

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

**WHAT STEP 3a MEASURED (2026-08-10), and it refines point 2.** The rbp
chain does NOT span the whole record stack - it covers only the TOPMOST
CONTIGUOUS NATIVE SEGMENT. Its floor is the first C++ return address: a
fragment entered from the interpreter (jit_enter), or a deeper call that
declined to the C++ tier (the sync depth cap runs interpreted-flat -
ackermann). Below that floor the record stack continues with EARLIER
native segments the current rbp chain cannot reach. So the mixed walk is
not "one rbp chain vs one record stack" - it is a SEQUENCE of native
segments (each an rbp chain) glued by C++/interpreter frames, and the
record stack is their concatenation. Point 2's native-SP field is how the
step-4 walk finds where one segment's rbp chain ends and the next begins;
the shadow walk already proves each segment matches its record slice
frame-for-frame (jit_norec_push_verify), so step 4 adds the gluing, not
the per-segment check.

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

## 8. DECIDED (maintainer, 2026-08-09)

- **FRAME POINTERS**, as a hard opinion: the implementation is much
  simpler and the walk can be made 100% reliable. **MyLang itself does
  NOT need `-fno-omit-frame-pointer`**: a record-less frame can only
  exist between two JIT-EMITTED call sites (every path through C++ - the
  interpreter tier, vm_exec_block, a builtin re-entry - pushes a record),
  so the chain the walk traverses consists entirely of prologues our own
  emitter wrote. The C++ compiler's frames are never walked. The walk's
  ANCHOR is captured on the COLD path by emitted code (the raise /
  reconstruct entry passes the fragment's own rbp as an argument) - zero
  hot cost, no dependence on how the C++ was compiled.
  AUDIT ITEM before step 1: confirm the emitter does not currently use
  rbp as a pin/scratch register (the register file uses rbx, r12-r15,
  xmm4-7; rbp's status is unverified).
- **The two-representation cost is acceptable** if the improvement is
  meaningful. No single arc is expected to reach the final goal; the
  composition of many is. Measure at the end; a significant improvement
  must be observable.
- **The pure-cached shape is NOT rescued - and NOT removed.** The
  per-frame pure-call cache keeps working exactly as today: the gate
  routes a cache-interacting call down the EXISTING record-ful path
  (same records, same results, carets, backtraces, same speed as now).
  "Not rescued" means only that those calls do not get the new saving -
  a routing decision, never a feature removal. fib already has its 14x
  from the cache itself, and its residue is 0.15% of corpus calls.

---

# THE TESTING PLAN (agreed with the maintainer, 2026-08-09)

Modeled on SQLite's discipline ("How SQLite Is Tested"): independent
oracles checking each other, EXHAUSTIVE and DETERMINISTIC anomaly
injection (their OOM net fails the 1st malloc, then the 2nd, then the
3rd... until a run needs no injection), 100% branch coverage of the core,
and tests that are themselves verified by planted defects. Nothing
probabilistic gates anything. The maintainer's standing rule for this
work: **when in doubt, MORE testing, never less** - a slow test's
frequency is his judgment call later; removal is not on the table.
These nets run locally first and are wired into CI once they exist (the
driver_checks.sh / myv_doc_check.py pattern).

Terms, by example:

    func c() { throw E(7); }                    # RECORD-LESS candidate:
    func a() { var r = c(); return r; }         #   plain_frame, emitted call
    func b() { try { var r = a(); return r; }   # RECORD-FUL: n_trys > 0 -
               catch (E as e) { return e.x; } } #   keeps its VmCallRec
    print(b());

## Net 1 - the SHADOW ORACLE (always-on during development, kept forever)

`MYLANG_NOREC_SHADOW=1` (TESTS builds): the tier's machinery runs AND the
record is still written; at every point a reconstruction would be USED -
a pop, one unwind step, one backtrace frame - the rebuilt values are
compared field-for-field against the live record:

    SHADOW MISMATCH call #12841: rebuilt {ret_pc=17, dst=3, desc=sumto}
                                 record  {ret_pc=18, dst=3, desc=sumto}

Two properties make it the primary net rather than scaffolding:
- **One-way mirror**: the reconstruction code cannot see the records, so
  shadow mode exercises the identical code a real record-less run would.
- **Every suite becomes a reconstruction test**: -rt, corpus_diff,
  nested_fuzz, repl_fuzz, 69_exc_crossframe's 340k unwinds - re-run under
  shadow, every call in each verifies the side table. Millions of
  deterministic checks before any behaviour changes.

## Net 1b - the FULL-STACK AUDIT (hardening lane; agreed 2026-08-09)

Compare-at-use proves "every frame we USED was right". It cannot see a
TRANSIENT corruption: clobber the saved-rbp link of the frame at depth
400 in a 501-deep all-record-less recursion that never throws, and every
pop still verifies only the TOP frame (one step from the anchor) - all
pops pass, the broken link is never read, the bug ships. The audit
proves the stronger statement: at EVERY call event, walk the ENTIRE
chain and compare every frame to its shadow record - i.e. "an exception
thrown at any moment would have unwound correctly", even in runs where
nothing throws:

    SHADOW WALK FAIL at call #101: frame 400/501 unreachable
      (link 0x7ffd... not in [anchor, top-record SP])

Cost is O(current depth) per call - negligible on flat call patterns,
quadratic on deep recursion (10_recursion_deep: ~600M comparison steps,
order seconds-to-a-minute in a debug build). Scoped to the
VM_HARDENING/TESTS lane, which is never benchmarked. Both modes ship:
at-use always in shadow, the audit in the hardening lane.

## Net 2 - the DETERMINISTIC EVENT SWEEP (SQLite's fail-the-Nth-malloc)

Reconstruction must be correct at EVERY point it could ever be demanded,
not just where exceptions happen to fall. `MYLANG_RECON_AT=N` forces the
PRODUCTION reconstruct-and-continue path (not the audit - the real
single-shot flow) at the Nth call event, verified against shadow:

    N=1
    while :; do
      MYLANG_NOREC_SHADOW=1 MYLANG_RECON_AT=$N ./mylang prog.my || exit 1
      N=$((N+1))            # stop when N exceeds the run's event count
    done

Every N is a separate deterministic run. Applied to the Net 3 corpus and
tests/functional/. Complementary to Net 1b: the sweep exercises the real
code path one point at a time; the audit checks chain integrity
continuously.

## Net 3 - EXHAUSTIVE SMALL-SCOPE ENUMERATION (not a fuzzer)

A stdlib-only generator that emits ALL programs in a bounded shape space:
- per level (depth <= 4): frame kind in {record-less scalar, record-ful
  `try`, record-ful `try/finally`, record-ful dict-iter, cached-call}
- terminal in {return int, return float, throw, a builtin that captures
  a backtrace WITHOUT throwing}
- if throw: caught at level j for every j <= d, or uncaught
(The `finally` and backtrace-capture axes were added per the expand-by-
default rule; the axis list is OPEN for additions and closed to
removals.) Order ~10^3-10^4 tiny self-asserting programs. The uncaught
ones compare FULL STDERR BYTE-FOR-BYTE across tw / vm-nojit / jit /
jit+shadow - RULE 2 is the spec, and the backtrace is exactly the hard
consumer.

Why bounded depth suffices (stated so it can be attacked): the walk is
inductive - each step processes one frame given only its parent's
anchor, so any ordering/off-by-one bug is expressible in an interleaving
of length <= 3-4; more depth adds repetition, not states. The cases that
do NOT fit that argument are enumerated EXPLICITLY instead: a call
exactly at a SEG_SLOTS segment boundary, recursion deep enough to grow
the native stack, a reconstruction spanning both.

## Net 4 - the COVERAGE GATE (SQLite's 100% MC/DC, scoped)

The GCOV lane exists (-DGCOV=ON). A gate script requires 100% line +
branch coverage of the NEW walk/reconstruction code only; an uncovered
branch is either covered or listed in the script with a written reason
("unreachable: plain_frame excludes X") - never silently exempt.

## Net 5 - the SABOTAGE MATRIX, written before the code

Each entry one planted defect + the net that must fail; run mechanically,
each watched failing before its mechanism lands:

    defect                                   caught by
    ret_pc off by one in the side table      Net 1, first -rt run
    interleave ignores the SP bound          Net 3, (less,FUL,less) throw
    side table stale after a re-JIT          Net 1 under REPL/-rt recompile
    caller_captures pop skipped              Net 3, closure levels
    wrong seg_top_before restore             Net 2 + explicit segment cases
    transient deep-link clobber              Net 1b (and ONLY Net 1b)
    walk does not terminate at the anchor    the ML_VM_CHECK below

Plus one INVARIANT, not a test: each walk step's SP strictly increases
and stays within [anchor, top record's SP] - an ML_VM_CHECK, so a
corrupted chain is a loud located abort in every CI lane, never a wild
read.

## Net 6 - the levers, from day one

`MYLANG_JIT_OFF=norec` and `MYLANG_JIT_FORCE=norec` land WITH step 1, so
corpus_diff --levers picks the tier up automatically and every A/B is
same-binary.

## The honest limit

ASan cannot see emitted frames, so the walk's memory safety is NOT
covered by the sanitizer lanes. The proof there is deterministic but
different: the SP-bounds ML_VM_CHECK (termination + range) plus shadow
equality (content). Stated here so the ASan-green lanes are never read
as covering it.

## Sequencing against the build order (§7)

Nets 1, 1b, 5, 6 land WITH step 1 (the unused side table) - by the time
anything depends on the table it has survived millions of shadow
comparisons. Nets 2 + 3 land before step 3 (the mixed walk). Net 4 gates
step 4 (stop writing records). Steps 1-3 stay zero-behaviour; nothing
gets faster until the nets exist. Days spent purely on these test arcs
are budgeted and expected - the point is writing the hard code afterward
without fear.

# STEP 4 DESIGN FACTS (established 2026-08-10, before any emit)

The full-protocol read that preceded step 4 settled every open mechanism
question and found one NEW gate plus one NEW hazard the plan above did
not know. Recorded here so step 4 is built against verified facts, not
the §3b sketch.

## 3c. The WINDOW CHAIN - discrimination without a release field

§5 point 2 planned a native-SP field on record-ful records to order the
mixed walk. NOT NEEDED. `frag_entry` pushes `rbx` (the frame-base
register = the frame's WINDOW) FIRST after `push rbp`, unconditionally -
so `[fp-8]` of every native frame is the WINDOW OF THE FRAME BELOW (the
caller's rbx, saved by this frame's prologue). Two consequences:

- **"Does this frame have a record?"** = `top_rec.window == the frame's
  own window`. Windows are unique among live frames, C++-pushed records
  carry real windows too, and a frame's record - when it has one - is
  the top record at its own return/raise. Zero new record fields, in
  any build.
- **The walk learns each next frame's window as it descends** (read
  `[fp-8]` before stepping `fp = [fp]`), so the record-ful/record-less
  interleave is decided per frame with one compare.

Verified as the SHADOW WINDOW CHAIN (`g_jit_norec_win_chain`, beside the
desc chain in `jit_norec_push_verify`): `records[idx-2].window ==
*(fp-8)` at every native-to-native level, across the whole suite and the
protocol benches (lockstep with `norec_desc` - 53,328 on fib's audit
run, 19,999 on 69_exc_crossframe). Sabotage: a checker reading [fp-16]
aborts instantly ("frame-below window != [fp-8]"). The rbx-first
prologue order is now LOAD-BEARING and commented so in `frag_entry`; an
emitter-side reorder sabotage is structurally unfireable TODAY because
no sync-call-containing fragment pins cache regs (`saved` is empty in
every walkable frame - verified via -vdj on a loop+recursion shape), but
the standing shadow walk catches it the day a pinning fragment appears.

## 4b. The gate gains FULLY-DELETED, and the measured reach

**The record is the interpreter's ONLY resume vehicle.** A frame that
exits its fragment mid-body - an interpreted island, a bail - is resumed
by `jit_sync_postexit` driving `vm_dispatch` through the frame's record
(sync_stop + the baked resume stub). A record-less frame therefore may
NEVER exit to the interpreter mid-body, and the only chunk shape that
guarantees that is **fully deleted: every op is EnterNative** (the head
plus the #64 post-call entry stubs). The gate becomes:

    not cached  +  plain_frame  +  FULLY DELETED  +  ref_slots small
                                                     (RET_REF_GUARD_MAX,
                                                      the C4c return arm)

Classified per M5b inline push (in push_verify, zero extra emit),
MYLANG_JITSTATS rows `norec_gate_*`. Measured at scale 1:

    bench                 pushes     gate_ok   declined by
    45_gcd               149,998       100%    -
    76_funcval_dispatch  999,999       100%    -
    78_typed_param_call  2,000,001     100%    -
    09_fib_recursive      10,687         0%    cached (as designed)
    10_recursion_deep      5,998         50%   body: sumto$0 keeps an
                                               interpreted CallV island
    69_exc_crossframe     39,998         50%   body: same shape

**The 50% rows correct §4's "up to 99.8%" prediction**: the plain_frame
measurement could not see the fully-deleted requirement. Where a
recursion partner keeps ONE interpreted op, its frames keep records -
correct, and the record-less frames interleave with them (which the
window chain handles). Raising those benches to 100% is a separate,
later task: nativize the partner's residual op.

## 4c. The record-less protocol, concretely

**The call site** (in place of the ~51-instruction record fill):

    push  <&caller_frame[dst]>    ; lea from rbx - baked; 0 for a
                                  ; discarded result
    push  <ctx.captures>          ; the caller's own captures value
    call  <callee entry>          ; both paths: parity kept (2 pushes)
    pop   -> ctx.captures         ; restore
    add   rsp, 8                  ; drop dst_addr
    ; restore act.vframe.slots/size from rbx + the CALLER's own baked
    ; nslots (the callee cannot know them; the caller knows both at
    ; emit time)

The residue must be pushed AFTER the M5a stack switch (and popped before
switch_post) or it lands on the wrong stack. §3b's `push seg_top_before`
is NOT needed: an inline push never changes the segment (segment
overflow declines to the slow C++ path), deeper frames restore their own
tops by induction, so the CALLEE restores `seg->top -= my_total` and
`act.used -= my_total` from its own compile-time totals.

**The callee return** discriminates by `top_rec.window == rbx`:
record-ful -> today's C4c inline pop, byte-identical; record-less -> a
NEW arm that reads `[rbp+16]` (captures - unused here, the caller
restores) and `[rbp+24]` (dst_addr), writes the result through dst_addr
(old-dst-trivial + ref-result guards decline to a helper taking BOTH
addresses), runs its own ref_slots release scan, restores seg-top/used
from baked totals, and rets the sentinel. It must NOT touch the top
record - that record belongs to an ancestor.

**The unwind walk** (vm_raise + vm_unwind_walk) gains the raising
fragment's rbp (a new argument from jit_throw / jit_rethrow /
jit_end_finally's emit). Current frame record-less (window compare) ->
reconstruct: backtrace frame from (chunk's desc via the descent,
site.site_loc), release `seg->top` via the chunk's own totals, restore
captures from the residue at [fp+16], step to the caller via [fp+8]
(site) / [fp] (chain) / [fp-8] (next window). A record-ful frame runs
today's body. The walk stays SEGMENT-LOCAL exactly as today: sync_stop
conversion still bounces the status up the native stack, and each
record-less post-call site's exception arm must CLEAN ITS OWN FRAME
(capture + release + captures restore, a helper taking rbp + site)
before propagating - an exception never crosses a record-less frame as
a bare status.

## 4d. The -3/SWITCH hazard - the one genuinely new mechanism

The interpreted-flat protocol (JIT_RET_SWITCH, the sync depth cap -
ackermann's path) resumes each native caller through ITS RECORD's baked
resume stub after the flat callee completes. A record-less frame in that
chain has no record and its native frame is unwound by the -3
propagation - it would be UNRESUMABLE. The fix is the lazy-
reconstruction thesis applied once more: **materialize the records at
the moment the slow helper decides to go flat**. The helper (a C++ cold
path) receives the deepest fragment's rbp, walks the chain
innermost-to-outermost, discriminates by window (as the unwind walk
does), and INSERTS a full record for each record-less frame at its
chain position - below the already-pushed SWITCH record, ordered by the
walk itself. Everything a record holds is reconstructible: the site
gives dst / caller chunk / caller desc; the site must additionally
carry the POST-CALL ENTRY-STUB pc (`NorecSite::resume_pc`, set from the
same expression the slow path already bakes); captures come from the
residue; totals from the chunk. Cold, rare (cap-exceeded only), and
entirely in C++.

## 4e. Build order within step 4

  4-i.   NorecSite::resume_pc + jit_chunk_norec_ok() (the real gate
         function, unifying the push_verify mirror) - mechanical.
  4-ii.  The raise-helper rbp argument + the mixed unwind walk, WITH
         records still written (the walk prefers reconstruction for a
         qualifying frame and cross-checks the record it still has -
         the §7-step-3 sabotage, now with the real walk).
  4-iii. The callee return's record-less arm + the call-site residue,
         behind MYLANG_JIT_FORCE=norec (records still written but
         IGNORED by the return - the A/B lever).
  4-iv.  The -3 materializer.
  4-v.   Stop writing the record for gate-passing calls; flip the lever
         default; Net 4 coverage gate; measure per shape + suite.
