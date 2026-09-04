# THE FRAMELESS CALLEE — the plan (#97, opened 2026-08-29)

Every number here was measured on this box, on the SHIPPING lane
(`OPT=1 ASSERTS=0`), with `-npc`, before any code was written. Where a
number came from a TESTS lane it says so and it is not used.

---

## 0. WHERE WE ACTUALLY ARE

### 0.1 The gate exists and reaches nobody

`jit_chunk_frameless_ok` (#97 step 6) was built as a REACH PROBE and
decides nothing. `MYLANG_JITSTATS` on the shipping build, scale 1:

    bench                 frameless_chunks   frameless_calls   total calls
    09_fib_recursive             0                  0             555,823
    76_funcval_dispatch          4                  0             999,999
    63_closures                  6                  2             999,999
    11_closure_counter           3                  1           1,000,000
    78_typed_param_call          4                  0           2,000,001

**3 sites out of 4.5 million calls.** `frameless_chunks` counts BODIES
that qualify; `frameless_calls` counts CALL SITES that could use one.
The gap between those two columns is the whole problem: the bodies are
fine, the SITES cannot name their callee.

### 0.2 Why each bench declines — `MYLANG_FRAMELESS_WHY=1`, verbatim

    09_fib_recursive
      [frameless] not a leaf          slots=3+8 ops=40 refs=[0 3 4 5] last=91
      [frameless] not a leaf          slots=1+9 ops=50 refs=[]        last=52

    78_typed_param_call
      [frameless] not a leaf          slots=7+4 ops=33 refs=[0 2 3 7 8 10]
      [frameless] OK                  slots=1+1 ops=2  refs=[1]
      [frameless] OK                  slots=1+2 ops=3  refs=[]
      [frameless] OK                  slots=1+1 ops=2  refs=[1]
      [frameless] OK                  slots=1+2 ops=3  refs=[]

    76_funcval_dispatch
      [frameless] ref_slots too many  slots=6+4 ops=33 refs=[0 2 3 5 6 7 8]
      [frameless] OK                  slots=2+2 ops=4  refs=[0 1 2 3]   x4

### 0.3 What a call costs today — `scripts/jitprofile.py`

09_fib_recursive, scale 1, 555,823 calls:

    Ir in JIT code   122,514,518   72.4%     220 Ir per call
    elsewhere         46,694,700   27.6%
    run total        169,209,218

and the only hot C++ helper left is

    jit_cached_probe  13,339,968    7.9%      24 Ir per call

everything else outside the fragment is compile-time (dynamic_cast,
dl_lookup, printf). The call protocol has already moved into emitted
code; what remains is what that emitted code DOES.

### 0.4 The per-call bookkeeping, as emitted (fib$0#0, `--top 34`)

    +0    push rbp                       555,832   <- every call
    +1    mov rbp, rsp
    +4    push rbx
    +5    sub rsp, 8                               <- alignment pad
    +9    mov rbx, rdi                             <- the window
    +12   mov rax, s0
    +19   cmp rax, 2                               <- THE FIRST REAL WORK
    +23   jge +724
    +29   mov r8, [<addr>]               278,905   <- the activation
    +37   mov r10, [r8+0x40]                       <- act.back_rec()
    +44   cmp rbx, [r10+0x0]                       <- rec identity
    +51   jne +513
    +57   cmp byte [r10+0x68], 0                   <- boundary?
    +65   jne +401
    +71   cmp [r10+0x58], 0                        <- cache_key?
    +79   jne +707
    +85   cmp [r10+0x70], 0
    +93   jne +707
    +99   cmp [r8+0x270], 0
    +107  jne +707
    +113  mov rdx, [r10+0x30]                      <- the parent's dst
    +120  test rdx, rdx
    +123  js +222
    +129  imul rax, rdx, 48                        <- dst slot address
    +136  mov rcx, [r10+0x88]
    +143  add rcx, rax
    +146  mov rax, [rcx+0x18]                      <- the OLD value's type
    +153  cmp [rax+0x8], 8                         <- trivial?
    +160  jge +707
    +166..+194  the four-word result copy

That is **~21 instructions of RECORD INTERROGATION** before a 32-byte
store, and the head of it —

    mov r8,[addr] -> mov r10,[r8+0x40] -> cmp rbx,[r10+0x0] -> jne

— is a THREE-DEEP DEPENDENT LOAD CHAIN FEEDING A COMPARE THAT GATES THE
NEXT INSTRUCTION. That exact shape is what #111 removed for a 0.86x wall
clock while twenty instructions of #97 steps 1-6 moved the suite 1.002x.
**The dependency chain is the thing that costs time**, and this tier's
argument rests on removing one, not on the instruction count.

### 0.5 What the RECORD is already worth — the norec A/B

`MYLANG_JIT_OFF=norec` turns the record-LESS tier off, so the delta is
what NOT writing a record already buys. It is a lower bound on frameless,
which additionally removes the record READ above.

    bench                 norec on       norec off     the record costs
    11_closure_counter    132,279,738   176,261,165        +33.25%
    78_typed_param_call   358,136,934   446,101,906        +24.56%
    76_funcval_dispatch   334,676,075   375,648,512        +12.24%
    09_fib_recursive      152,679,514   145,422,118         -4.75%

⛔ **09_fib IS NEGATIVE AND THAT IS NOT NOISE** (callgrind Ir,
deterministic). On the one bench this tier exists for, the record-less
tier is a net LOSS of 4.75%. Nothing in the record says why. **Increment
0 is to find out**, because the answer decides whether frameless is a
20% win there or a 0% one, and it is one profile away.

#### ✅ ANSWERED — increment 0, 2026-09-02 (commit `50af0f4`)

**The tier never fired on fib.** 555,823 call gates, ZERO record-less
pushes; 78 makes 2,000,001. Every gate was refused by
`norec_had_cached` — fib is a pure tree-recursion, so the optimizer
rewrites its self-calls into `CachedCallV`, and that flag means *"this
body could acquire a live vframe pure cache, which a record-less return
cannot stash"*.

**And the hazard cannot occur in the configuration this table was
measured in.** `-npc` disables the pure-call cache and does NOT change
codegen (`-vd` byte for byte), so the body carries the opcode, can
never build a cache, and was refused anyway. The `-4.75%` was therefore
never the tier's cost; it was the cost of the runtime FORK plus a dead
record-less arm, paid on every call for a tier that then declined.

The fix is one conjunct — `chunk.norec_had_cached && g_pure_cache_enabled`
— with the soundness precondition made an `ML_CHECK` inside
`Frame::ensure_pure_cache()`, the single allocation site. The row now
reads:

    09_fib_recursive      120,995,037   145,422,118        +20.74%

so the record-less tier wins on **all four** benches, and the frameless
tier — which additionally removes the record READ — keeps its premise
intact. Blast radius nil: 16 benches, max |delta| 0.12%.

**WALL CLOCK, 2026-09-03** (interleaved `--baseline`, `OPT=1
ASSERTS=0`, `-npc`, 90 benches): **09_fib 0.250s -> 0.177s = 0.71x**,
suite geomean 0.996x. The clock moved MORE than the Ir (-29% vs
-20.7%) — this is a store burst removed, not free instructions, which
is why it does not show the guard-elision signature §4 warns about.

⛔ **THIS DOES NOT MAKE FRAMELESS CHEAPER TO BUILD.** It moves 09_fib's
BASELINE, which means the frameless tier now has to beat a faster
starting point on its flagship bench. Increment 3 (E2, drop the leaf
rule) is the increment that serves 09_fib, and its gate should be
re-derived from the new baseline rather than from the -4.75% row.

Full record: `docs/jit-optimizations.md`, the *#97 increment 0* entry —
including the smaller fix that was built, measured at -4.01%, and
DROPPED because it CONFLICTS (eliding the fork stops the tier from ever
firing).

---

## 1. THE THREE ENABLERS

They are independent. Each is stated as a program, what it does today
verbatim, and what it should do.

### E1 — the SITE cannot name a callee the compiler ALREADY named

    func make_adder(int base) {
        return func [base] (int k) { return base + k; };
    }
    var add = make_adder(7);
    for (var i = 0; i < N; i++) s = s + add(i);      # 1,000,000 calls

TODAY:

    $ mylang -dcs 78_typed_param_call.my
    dcs 33  13  -  one  lambda@19:12          <- the compiler KNOWS
    $ MYLANG_FRAMELESS_WHY=1 mylang ...
    [frameless] OK  slots=1+1 ops=2 refs=[1]  <- the BODY qualifies
    $ MYLANG_JITSTATS=1 mylang ...
    frameless_calls 0                         <- and the site declines

CAUSE, one line — `jit_baked_callee`, jit.cpp:7357:

    if (is_value)                    /* the callee is a runtime VALUE */
        return nullptr;

The emitter's only naming source is a WRITE-ONCE GLOBAL FUNCTION SLOT.
`add` holds a closure a factory built at run time, so it has no such
slot — even though #116's callee-set analysis names it exactly, and
#116 increment 4 ALREADY stamps that answer on the AST as
`CallExpr::callee_fn` for the escape analysis to read.

PROPOSED: a second naming source. `jit_baked_callee` gains a value-callee
arm that reads the callee-set answer, subject to the same five
descriptor gates the named path uses, plus the identity compare that is
already emitted. The compare is what makes it sound: a value callee
CAN change, so the baked entry is a PREDICTION and the compare is the
check, exactly as it is today for a reassignable slot under
`MYLANG_JIT_FORCE=bakecallee`.

⛔ **THIS IS THE ONE THE MAINTAINER ASKED FOR FIRST**, and it is not
"the inference pass is not smart enough" — the inference is done. It is
a CONSUMER that never asked.

### E2 — a frameless callee may not contain a CALL

    func fib(n) {
        if (n < 2) return n;
        return fib(n-1) + fib(n-2);
    }

TODAY: `[frameless] not a leaf slots=1+9 ops=50 refs=[] last=52`. The
body passes every other gate — window 10 slots, `refs=[]` empty, a
terminal ReturnV — and is rejected solely for containing `CallV`.

PROPOSED: drop the leaf rule. "Frameless" means the frame carries no
RECORD; it does not mean the frame has no CALLS below it. What the rule
is really protecting is the unwinder: a frame with a call below it must
be walkable when the callee throws. That is a property of the MARKER
(§2.2), not of the op list.

⛔ The existing comment already says the reach here would be zero
without this: 09_fib is self-recursive, so E1 without E2 serves it not
at all.

### E3 — a callee resolved at RUNTIME

    func add_op(st, x) { st[0] = st[0] + x; }
    func sub_op(st, x) { st[0] = st[0] - x; }
    var ops = [add_op, sub_op];
    for (var i = 0; i < N; i++) { var fn = ops[i % 2]; fn(st, i); }

TODAY:

    dcs 19  5  -  many  add_op$0,sub_op$0

Two candidates, and which one runs depends on `i`. This is the case the
maintainer named as genuinely needing runtime resolution.

PROPOSED: a per-site POLYMORPHIC INLINE CACHE, two entries, keyed on the
FuncObject pointer, each entry holding a frameless entry address. Both
candidates are known at compile time, so both entry addresses are baked;
what is decided at run time is only WHICH. A miss falls to today's path.

⛔ NOT A NEW ANALYSIS. `callee_set(e)` already returns the two-element
set. What is new is spending it.

---

## 2. THE CALLEE-SIDE PROTOCOL — the shared bulk

All three enablers need this, and it is most of the work.

### 2.1 What changes

    TODAY, per call
      caller   writes a VmCallRec (ret_chunk, ret_pc, dst, boundary,
               cache_key, caller_captures) + a window on the segment
      callee   push rbp / mov rbp,rsp / push rbx / sub rsp,8 / mov rbx,rdi
      return   read the record, verify 5 conditions, compute the parent
               dst slot, type-check the old value, store 4 words

    FRAMELESS
      caller   binds arguments into the callee window; NO record
      callee   the same prologue, with the alignment pad repurposed (2.2)
      return   the value in RAX (or XMM0), a status in RDX; the CALLER
               stores it where it already knows it goes

The window does not go away — it is the callee's locals. The RECORD goes
away, and with it the return-side interrogation in §0.4.

### 2.2 The stack marker — free, by construction

An unwinder walking the rbp chain must tell a frameless frame from a C++
frame and from a recorded one. The prologue already emits `sub rsp, 8`
purely for 16-byte alignment; that slot becomes the marker
(`mov qword [rbp-16], FRAMELESS_MAGIC`), so the frame costs ONE store
and no extra bytes of stack.

⛔ The magic must be a value no legitimate slot content can hold, and the
walker must ALSO verify the frame's shape, not the word alone — a
constant matched by luck is a wrong answer, not a missed one.

### 2.3 The four sub-problems

1. **A SECOND ENTRY per chunk.** The existing entry expects a record;
   the frameless one does not. Two prologues, one body. `Chunk` gains
   `frameless_entry_off`. The two entries share everything after the
   prologue, so the body is emitted once.

2. **The RETURN arm.** `emit_ret_native` writes the parent's dst today.
   The frameless arm puts the value in RAX/XMM0 and `ret`s; the ref-slot
   release scan still runs (the window is still addressable through rbx),
   which is why the gate keeps `ref_slots.size() <= RET_REF_GUARD_MAX`
   rather than requiring it empty.

3. **The EXCEPTION path.** A frameless callee has no record for
   `vm_dispatch_exc` to resume on, so the throw must be conveyed to the
   caller. RECOMMENDED: a status in RDX, tested by the caller with
   `test rdx,rdx; jnz <site landing pad>` — one compare and a
   never-taken branch per call. The two alternatives are worse:
   forbidding throws is the `op_never_exits` gate that already measured
   a reach of ZERO, and a stashed landing-pad address is more mechanism
   for the same result.

4. **The BACKTRACE.** Settled with the maintainer: reconstruct from rbp.
   The VIRTUAL (inlined) frames are not at risk — the two sources are
   already disjoint, PHYSICAL from the rbp chain and VIRTUAL from
   pc-keyed side tables baked at compile time, and neither consults the
   other. The no-record tier's `norec_walk_chain` already does this walk
   and has three nets on it (Nets 2, 3 and 4).

---

## 3. THE INCREMENTS, in the order the evidence supports

Each ends with a MEASUREMENT that can kill the next one.

**0. WHY IS 09_fib NEGATIVE UNDER norec?** ✅ **DONE 2026-09-02,
   commit `50af0f4`.** The guess in this line was close and not right:
   the tier does decline for fib and does pay the fork anyway, but the
   reason is `norec_had_cached`, a refusal whose hazard only exists
   when the pure-call cache is ON — and every measurement runs `-npc`.
   One conjunct, **09_fib -20.74% Ir**, tier reach 0 -> 555,823 pushes,
   blast radius nil. It was NOT "most of E2's win": E2 removes the
   record READ in the CALLER, which is untouched. See §0.5.

**1. E1 — the value callee gets named.** Smallest, uses analysis that is
   already done and already stamped, and it is the maintainer's stated
   first priority. GATE: `frameless_calls` must go from 0 to ~2,000,000
   on 78 and ~1,000,000 on 11. If it does not, stop — the naming path is
   wrong and nothing downstream matters.

**1b. THE CALL SITE BRACKETS ITS PINS (§3b, inherited from #123).**
   The caller runs on 4 pinnable registers of 13 today, and increment
   2's gate is a wall-clock number — so this is a CONFOUNDER REMOVAL,
   not a side quest. See §3b for why the obvious fix does not work and
   what the real one is.

**2. The callee-side protocol (§2), behind `MYLANG_JIT_OFF=frameless`.**
   The bulk. Landed with the tier ADMITTING ONLY LEAF CALLEES first, so
   E2's unwinder question is not entangled with the entry/return/
   exception work. GATE: Ir on 78 and 11; `corpus_diff` over the five
   matrices; the norec nets re-run, since the rbp chain now has a second
   kind of frame in it.

**3. E2 — drop the leaf rule.** Serves 09_fib, and only after the
   marker and the walker are proven by increment 2. GATE: 09_fib Ir, and
   `norec_enum --depth 4` (2272 programs x 4 engines) because a throw
   crossing a frameless frame is exactly its shape space.

**4. E3 — the two-entry inline cache.** Serves 76. Last because it is
   the only one that adds a RUNTIME decision, and because it is worth
   nothing until 2 exists.

---

## 3b. INHERITED FROM #123 — THE CALL SITE MUST BRACKET ITS PINS
## (added 2026-08-29, when #123 deleted the register pools)

**THE SYMPTOM, measured:** a fragment that emits a MyLang call gets
**4 pinnable registers of 13**. `jit_run_blocks_xcache` denies the
whole CALLER-SAVED half of the pin pool to any run containing `CallV`,
`CachedCallV` or `CallValueV`, so `fib$0` — the flagship shape of this
whole task — runs on the callee-saved four alone.

**⛔ AND THE DENIAL IS CORRECT TODAY. The first write-up of this called
it a register wasted for nothing, and that was wrong.** Read
`emit_sync_push_native`: it holds

    rax  rbx  rcx  rdx  rsi  r8  r9  r10  r11

as PROTOCOL across one long straight-line sequence — **8 allocatable
registers of 13**, plus rbx which is reserved anyway. So the tempting
fix, *"route its r10/r11 raw scratch through `alloc_scratch()`"*, cannot
work: there is no spare register to route them to. And a MyLang call is
a real SysV `call` — the callee clobbers every caller-saved register
regardless of what the emitter does with them first.

`jit_assert_no_volatile_pin` is the standing check that no caller-saved
pin ever reaches that emitter, and it fires in every debug build.

**THE ACTUAL FIX: BRACKET THE CALL SITE.** Emit the same
`emit_call_prologue` / `emit_call_epilogue` pair that every HELPER call
already uses:

  - the prologue stores each caller-saved pin to its slot's PAYLOAD
    (the type word stays stale, which is fine — a pinned slot is never
    memory-read by any op in the run) and bumps `trk_bracket`, after
    which the allocator treats those registers as free scratch;
  - the emitter's r8/r10/r11 use is then legitimate rather than
    scratch-by-convention;
  - the epilogue reloads.

The denial then drops to nothing, or at most to the registers the
protocol needs live ACROSS the `call` itself.

**THE COST is a store/load pair per pin per call site** — which is
exactly the trade the caller-saved extension already makes for helper
calls, and was measured worth it there (#96). It is NOT obviously worth
it here, which is why it needs its own gate.

**WHY IT BELONGS TO #97 AND NOT TO #123.** It changes the emitted CALL
SEQUENCE, which is this task's subject and not the register model's.
It also overlaps §2 directly: a frameless callee changes what the
caller must preserve, so bracketing and the callee-side protocol want
designing together rather than one on top of the other's assumptions.

**SEQUENCING.** Do it AFTER increment 1 and BEFORE increment 2, for a
reason that is not aesthetic: increment 2's gate is *"Ir on 78 and 11
plus the wall clock"*, and if the caller is still running on 4 of 13
registers then a flat wall clock there cannot be distinguished from
"the frameless tier does not pay". Fixing the register starvation first
removes a confounder from the measurement this plan turns on.

**GATE, and it is a two-sided one:**
  - `-vdj` on `fib$0` must show the entry pushing more than the
    callee-saved four, i.e. the pin count actually rises;
  - suite geomean must not regress. A run whose pins all sit in
    callee-saved registers pays NOTHING new (the prologue's loop skips
    them), so a regression would mean the newly-admitted pins are worth
    less than their spill — in which case the honest answer is to admit
    caller-saved pins only when the run's call count is low, and to say
    so rather than to keep the widened pool.

**⛔ AND RE-READ `jit_run_blocks_xcache`'s OPCODE LIST WHEN DOING
THIS.** It is an audited table with the documented staleness risk, and
its own comment records why ReturnV and Halt are deliberately absent
(`emit_ret_native`'s first act is `flush_cache()`). If the bracket
lands, the list may be deletable outright — which is the better outcome,
one fewer table to go stale.

---

## 4. THE HONEST RISK

**#97's own central finding argues against this tier**: instruction
shaving on the call protocol has a wall-clock ceiling near zero — steps
1-6 removed ~20 instructions per call for a suite geomean of 1.002x.
This plan is only worth doing because what it removes is NOT twenty
independent instructions: it is a dependent load chain gating a branch
(§0.4), which is the one shape that has moved the clock every time
(#111: 0.86x; #112: ~1%).

**If increment 2's gate comes back byte-flat on the wall clock while Ir
drops, STOP AND SAY SO.** That is the guard-elision signature this
project has recorded three times, and it would mean the record read is
retiring free alongside the real work — in which case E2 and E3 buy
nothing and should not be built.

Second risk, smaller: the tier adds a SECOND FRAME KIND to the rbp
chain, and every consumer of that chain (backtrace, exception
reconstruction, the norec walker, `MYLANG_RECON_AT`'s sweep) has to
learn it. Those consumers have good nets; the nets must run at every
increment, not at the end.
