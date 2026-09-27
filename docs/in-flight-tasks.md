# IN-FLIGHT TASKS — the resume-from-here document
## written 2026-09-04, end of the #97-E1 session

**WHAT THIS FILE IS.** A self-contained handoff. It exists because the
maintainer works from several machines and a fresh Claude session starts
with none of this session's context. Everything a successor needs in
order to continue is stated HERE, from scratch, with concrete programs
and exact numbers — not as a pointer to a conversation nobody else can
read. Where a claim was MEASURED, the measurement is quoted; where it is
an assumption, it says so.

**IT IS A SNAPSHOT, NOT A SPEC.** `plans/frameless-callee.md` and
`docs/jit-optimizations.md` remain authoritative for #97's design and
for the per-change record. This file says WHERE WE STOPPED and WHAT IS
UNDECIDED. If it and the plan disagree, the plan is newer unless this
file's date is later.

---

## 0. GIT STATE

Branch `exp-work`, HEAD `e626b84`. The commits of this session, oldest
first:

    50af0f4  jit (#97 increment 0): the cached-call refusal is about the
             CACHE, so it does not apply when the cache is OFF
    94c818b  docs: repay the record for #103 and #97 increment 0, with
             the wall clock
    77943e6  jit (#97 E1): a VALUE callee gets NAMED, so a
             factory-returned closure bakes
    1767291  tests: the ASan corpus lane BOUNDS ITSELF
    0ad4384  docs (#97 E1): the JIT record entry, with the wall clock
    96656ad  plans (#97): the COERCING-CALLEE BAKE becomes increment 1c
    e626b84  plans (#97 1b): the symptom was wrong - a call-containing
             run pins ZERO, not four

`origin/exp-work` is at `77943e6`; the last FOUR commits are local only.
The working tree is clean. Nothing is half-applied.

**BUILD LANES PRESENT** (`build-claude/`): `release`, `dbg` (TESTS=1
OPT=0, ASan+UBSan), `clang`, `rel-hard`, `perf` (OPT=1 ASSERTS=0, the
measurement binary). The throwaway measurement lanes and the fork's
worktree were deleted, per the lane rule. Two `myv_fuzz` findings are
kept in `myv-fuzz-bad/` (git-ignored) - see the loader item in §3.

---

## 1. TASK #97 — CALL PROTOCOL / THE FRAMELESS CALLEE  [IN PROGRESS]

**THE STANDING REQUIREMENT:** the worst `my/cpp` benches must reach
<= 5x. Plan: `plans/frameless-callee.md`. Per-change record:
`docs/jit-optimizations.md`.

**WHERE THE WORST BENCHES STAND after W4 (the maintainer's own run,
2026-09-21, `my/cpp`; after W3 the same table read 34 5.22x, 11 7.59x,
63 7.95x, 78 8.47x, 76 9.80x; before E3/W3: 78 10.35x, 76 11.45x):**

    34_sort_custom_cmp            0.106      0.020    5.23x
    64_struct_create              0.105      0.016    6.49x
    11_closure_counter            0.082      0.012    6.68x
    73_multi_unpack               0.228      0.032    7.02x
    58_structs                    0.077      0.011    7.12x
    09_fib_recursive              0.156      0.022    7.12x
    63_closures                   0.194      0.025    7.65x
    35_map_filter                 0.193      0.025    7.80x
    78_typed_param_call           0.057      0.007    7.85x
    75_indexed_unpack             0.208      0.024    8.79x
    76_funcval_dispatch           0.170      0.018    9.72x

**AND AFTER W5 / #25 / W6 / SP1-SP3 / 1b(ii) - THE MAINTAINER'S OWN
RUN, 2026-09-22 (`my/cpp`), which is the table above one arc later:**

    34_sort_custom_cmp            0.106      0.020    5.21x
    64_struct_create              0.103      0.018    5.73x
    11_closure_counter            0.079      0.012    6.42x
    76_funcval_dispatch           0.113      0.017    6.66x
    78_typed_param_call           0.051      0.007    6.85x
    09_fib_recursive              0.152      0.022    6.99x
    73_multi_unpack               0.227      0.032    7.09x
    63_closures                   0.190      0.025    7.54x
    35_map_filter                 0.190      0.024    7.86x
    75_indexed_unpack             0.209      0.024    8.69x
    58_structs                    0.079      0.009    8.87x

**NOTHING IS OVER 9x ANY MORE** (76 was 9.72x and 11.45x before it),
and 34 is within 0.21x of the standing <= 5x requirement.

⛔ **READ THE `my` COLUMN, NOT THE RATIO, FOR MyLang'S PROGRESS.** The
C++ side is CACHED, so a ratio moves when the DENOMINATOR moves and
says nothing about this work - both directions are in this table:
64_struct_create improves 6.49x -> 5.73x on a `my` that went 0.105 ->
0.103 (its cpp went 0.016 -> 0.018), and **58_structs "regresses"
7.12x -> 8.87x on a `my` that went 0.077 -> 0.079 while its cpp went
0.011 -> 0.009**. Neither is a MyLang change. The same trap the
machine-speed marker exists for on the python side (CLAUDE.md: *`cur/
base` from `--baseline` is the trustworthy number; my/py alone is
not*).

By the `my` column the real movement since the W4-era table is:
**76_funcval_dispatch 0.170 -> 0.113 (-34%)** - E3, W5 and #25 landing
on the bench they were aimed at - 78 0.057 -> 0.051 (-11%), 11 0.082
-> 0.079, 09 0.156 -> 0.152, 35/63 0.193/0.194 -> 0.190; 34, 73 and 75
flat.

RESUMED 2026-09-21 with W4 (done), W5 (done: the inline borrow at
the site, the arm's borrowed skip, the scalar parameter tails - 76
-13.5% Ir, 78 -2.5%) and #25 (done: `visit_use_def` learned six store
ops - 76 -9.0% Ir with `refs=[0 1]` and a 76-instruction callee, and
far more elsewhere: 60_bit_sieve -23%, 14 -20%, 86 -20%, 68 -18%,
zero per-iteration regressions), and W6 (done 2026-09-22: the capture
base is CALLER-saved in a call-free frameless body - 78 -1.30% Ir, 11
-1.40%, 63 -0.32%; HALF the predicted step, see §1.6), then SP1/SP2/
SP3 (the 16-alignment invariant: one call seam, an emit-time rsp
model, aligned AT the call), their follow-up (the dead-model
population to zero) and 1b(ii) (`lea dst, [src+k]`, 09_fib -2.69% Ir)
- with the shape tests moved onto a STRUCTURED decode along the way.
Next per §1.6: E2 for 09.

### 1.1 What is DONE

    steps 1-3    the low arena, the capture store-to-load forward,
                 main's nslots, the inline closure store
    step 4/4a    THE BAKED CALLEE - a write-once global slot's callee
                 named at compile time (09_fib -3.3% Ir, emitted -13.8%)
    steps 5/5b   ref_slots learns MoveV and LoadConstV
    step 6       the FRAMELESS GATE - built, measured, INERT
    #111 #112    the proven-scalar capture; the capture base pinned
    increment 0  the norec/pure-cache refusal (commit 50af0f4)
    increment 1  E1, the value callee gets NAMED (commit 77943e6)
    increment 1b the call is a classified op, pins survive it
    increment 1c the coercing callee bakes + the probe elision
    increment 2  THE FRAMELESS CALLEE (F1-F6, 2026-09-19):
                 78 -32.9% Ir / 0.70x, 11 -35.0% / 0.64x, 63 -18.1% /
                 0.80x - record: docs/jit-optimizations.md, *#97
                 increment 2*; plan section 3 item 2 lists what is left
                 per call and in which order
    increment 3  T0 (the dump driver), W1 (the caller-built window),
                 W2 (the fill binds from the sources), E3 (the two-way
                 site: 76 -20.2% Ir), W3 (the init elision: 78 -7.6%
                 Ir / 0.85x, 11 -8.2% / 0.90x, 63 -2.5% / 0.92x) -
                 commits 17d1662 ce17271 9c99f8c 9017e80 856bf06; and
                 RULE 2's caret work beside it (e4f8b8f the argument
                 list, d370a76 the failing argument, myv v16)

### 1.2 Increment 0, in full (commit `50af0f4`, docs `94c818b`)

**THE ANOMALY.** `MYLANG_JIT_OFF=norec` turns the record-LESS call tier
off, so the A/B measures what NOT writing a `VmCallRec` buys. Three
benches won 12-33%; 09_fib_recursive — the one bench the whole arc
exists for — LOST 4.75%.

**THE CAUSE.** The tier never fired there: 555,823 call gates, ZERO
record-less pushes. Every gate was refused by `norec_had_cached`,
because fib is a pure tree-recursion whose self-calls the optimizer
rewrites into `CachedCallV`, and that flag means *"this body could
acquire a live vframe pure cache, which a record-less return cannot
stash"*. **The hazard cannot occur in the configuration every
measurement uses**: `-npc` disables the pure-call cache and does not
change codegen (`-vd` byte for byte), so the body carries the opcode,
can never build a cache, and was refused anyway.

**THE FIX** is one conjunct in `jit_chunk_norec_ok` (jit.cpp):

    || (chunk.norec_had_cached && g_pure_cache_enabled)

**MEASURED:** 09_fib Ir 152,662,523 -> 120,995,037 (**-20.7%**), wall
**0.250s -> 0.177s = 0.71x**, `norec_pushes` 0 -> 555,823, suite geomean
0.996x, blast radius nil (16 benches, max |0.12%|).

**SOUNDNESS IS MACHINE-CHECKED.** The fix rests on *"no PureCache exists
when the flag is off"*. That held by audit, but an audit is a snapshot
and this is now a precondition of EMITTED CODE, so the invariant is an
`ML_CHECK_MSG` inside `Frame::ensure_pure_cache()` (eval.h), the single
allocation site. Watched failing: dropping the flag test in
`jit_cached_probe` gives rc=134 with the named assertion.

**A SMALLER FIX WAS BUILT, MEASURED AT -4.01%, AND DROPPED BECAUSE IT
CONFLICTS**: eliding the self-recursive call's runtime fork stops the
tier from ever firing, so it actively prevents the -20.7% one. Kept at
`/tmp/fixA_dropped.cpp` — **that path is a scratch file and will not
survive a reboot; it is not needed, only recorded.**

### 1.3 Increment 1 = E1, in full (commit `77943e6`, docs `0ad4384`)

**THE SUBJECT.** Before E1 the emitter could name a callee only through
a write-once GLOBAL FUNCTION SLOT. So this program re-established the
callee's properties from memory on every call:

    func mk(base) { return func [base] (k) { return base + k; }; }
    var add = mk(7);
    for (var i = 0; i < N; i++) s = s + add(i);

even though `mylang -dcs` prints, for that exact program (the `while`
form, so the call site is on line 5):

    dcs  2  11  -  direct  mk$0
    dcs  5  25  -  one     lambda@1:24~1

— #116's callee-set analysis names the callee exactly, and the `~1`
suffix says it is the INSTANCE's lambda (`mk` is a template over its
un-annotated parameter, so the call was redirected to `mk$0`). It was a
CONSUMER that never asked.

**THE CARRIER.** The JIT runs AST-FREE, so the answer must survive the
AST teardown and a trip through a `.myv`. A new per-chunk, pc-keyed
table `Chunk::value_callees` (bytecode.h), each entry an index into that
chunk's existing `closure_defs` pool. Stored in the image: **myv format
v15, section 9.20 of `docs/myv-format.txt`**.

Packing the index into `CallValueV`'s `b` field beside nargs — the idiom
`CallDynV` already uses — was REJECTED: `b` is read as nargs in THREE VM
hot-dispatch sites and a missed mask there is a silent wrong argument
count.

**THE EMITTER NEEDED NO CHANGE.** RAX already holds the live callee's
descriptor on both the slot path and the value path, so only
`jit_baked_callee` gained an arm.

**SOUNDNESS IS THE EMITTED IDENTITY COMPARE, NOT THE ANALYSIS.** The
baked arm compares the live descriptor against the baked one and falls
to the generic tier on a mismatch, so this is a PREDICTION: a wrong one
costs a slow path, never a wrong answer.

**REACH (`-npc`, scale 1).** ⛔ The two counters are DIFFERENT UNITS.
`bake_push` is bumped by EMITTED code (executions); `frameless_calls` by
a C++ `++` in the emitter (sites). The plan's original gate conflated
them — it asked for "`frameless_calls` 0 -> ~2,000,000", which is an
execution number written against a site counter.

    bench                bake_push (exec)        frameless_calls (sites)
    11_closure_counter   1 -> 1,000,001          1 -> 2
    63_closures          400,000 -> 1,000,000    2 -> 5
    12_higher_order      -                       0 -> 1
    76_funcval_dispatch  unchanged               unchanged
    78_typed_param_call  unchanged               unchanged

**76 IS A CORRECT DECLINE**: `ops[i % 2]` really reaches two functions,
so the set has two candidates and no stamp is written.

**78 DOES NOT MOVE AND NOT FOR A NAMING REASON** — see increment 1c.

**MEASURED:** 11_closure_counter -3.02% Ir / 0.95x wall; 63_closures
-0.96% Ir / 0.99x wall; suite geomean **0.999x**.

⛔ **FOUR BENCHES READ OUTSIDE ±5% ON THE CLOCK AND ARE PURE NOISE,
PROVEN.** 19_foreach_indexed 0.80x, 18_foreach_array 0.83x,
20_foreach_unpack 1.09x, 60_bit_sieve 1.09x — none contains a value call
at all, and their callgrind Ir deltas are **-0.00%, -0.00%, -0.56%,
-0.01%**. Had the Ir check been skipped this run would have "shown" a
20% win and a 9% regression that do not exist. **On a 60-160 ms bench
the wall clock is not evidence on its own.**

⛔ **THE FINDING THAT MATTERS MORE THAN E1'S OWN NUMBER** — the contrast
with increment 0, same arc, same benches:

    increment 0   removed a ~19-field VmCallRec STORE BURST
                  -20.7% Ir  ->  0.71x wall  (the clock moved MORE)
    E1            removed a 3-entry cache PROBE plus five descriptor
                  GATES, leaving one compare
                  -3.0% Ir   ->  0.95x wall  (about flat)

E1's removal is perfectly-predicted branches over L1-hitting loads — the
GUARD-ELISION family, wall-clock ceiling near zero. Increment 0's was
memory traffic. **So the frameless tier's value is the record WRITE it
removes, NOT the gates it collapses. Size increment 2 on the store burst
and expect nothing from the gate collapse.**

**THREE BUGS E1 SURFACED**, each caught by a net, none by reading:

1. **Use-after-free reading `callee_fn->desc` at codegen.**
   `CallExpr::callee_fn` is a raw AST pointer stamped at the END of
   inference; its only previous consumer (#93's escape analysis) runs
   moments later inside `resolve_names` while every decl is alive.
   CODEGEN runs after the whole optimizer stack, which frees decls.
   Fixed by stamping `CallExpr::callee_desc` — the program-lifetime
   FuncDescriptor — in `stamp_callee_fn` (inferencer.cpp).
2. **The descriptor can die too.** An inline lambda passed as an
   argument dies with the call expression that held it, taking the
   `FuncDeclStmt::desc_owner` unique_ptr with it. So codegen never
   TRUSTS a stamp: `CodegenLiveDescs` (codegen.h/.cpp) builds a set from
   `collect_funcs` over the FINAL tree — the same walk that decides
   which bodies to compile — and membership is a pointer COMPARE, never
   a deref. A recycled address can only cause a wrong PREDICTION, which
   the identity compare turns into a slow path.
3. **There are THREE codegen drivers and only one was wired.**
   `vm_compile` (which covers `vm_precompile_all`, nested inside it) and
   `disassemble_program`, which runs its own two-pass codegen over
   throwaway chunks. Installing the live set in the wrong one left the
   whole tier **silently inert**, and the reach counter read exactly
   like *"the liveness gate is too strict"* rather than *"the gate never
   ran"*. It is an RAII type now so the second site is one line.

**AN INSTRUMENT FIX THAT WAS MASQUERADING AS A FINDING:**
`MYLANG_JITSTATS` printed NOTHING for a `.myv` run — the image branch in
`mylang.cpp` has its own `return 0` and skipped both reports at the end
of `main`. A loaded image therefore looked like it did not bake. With
the report restored it bakes 1,000,001, exactly as a fresh compile does.

**A NEW SHAPE-EATER for CLAUDE.md's vacuous-test list.**
`jit_callee_cache`'s MONOMORPHIC case was `var f = mk(3); f()` —
monomorphic AND nameable, so since E1 it BAKES and the cache arm the
test exists for never runs. It now picks its callee out of an array of
two DIFFERENT lambda decls (two candidates, no stamp, so the cache tier)
while staying monomorphic at run time. **An optimization that names more
callees eats every test whose subject is the tier for UNNAMED ones.**

**NETS RUN FOR E1, all green** (lane `build-claude/e1`, TESTS=1 OPT=0,
ASan+UBSan, unless noted):

    -rt                             1978/1978 + 4 differential modes
                                    1696/1696 each
    corpus_diff plain               34/34
    corpus_diff --levers            34/34 x 23 configs - AND verified
                                    IDENTICAL on the pre-E1 build
    corpus_diff --cold              rc=0
    corpus_diff --xrot              34/34 x 16 rotations
    corpus_diff --nolowmem          34/34
    driver_checks.sh                all passed
    vdjcmp.sh (self-test)           127 identical, 0 differing
    disasmcheck.py (objdump)        239,032 instructions, 0 disagreements
    myv_fuzz.py release + ASan      0 crashes / 3,200 mutations each;
                                    1 hang, triaged "loads cleanly - a
                                    non-terminating program" (benign)
    myv_doc_check.py                gcd.myv AND an image containing the
                                    new v15 record both consume to EOF
    norec_enum.py --depth 3         480 programs / 1920 runs, all agree
    norec_sweep.py                  OK (shadow + production)
    nested_fuzz.py --count 300      300 programs x 5 engines, 0 diverged
    image vs source run             byte-identical output, and both
                                    report bake_push 1,000,001

### 1.4 ⛔ INCREMENT 1b — ✅ DONE 2026-09-19 (see the note at the end)

**STATUS AS OF 2026-09-19: LANDED.** The barrier form was built first
and measured (it costs 3 instructions per executed call for nothing;
09_fib +2.6% Ir, 11_closure_counter 1.10x on the clock), then replaced
by the PRECISE classification: `pick_visit_op` marks the argument run,
the callee temp and the dst as memory-touched and nothing else, so a
callee-saved pin stays live across the `call`; the two SWITCH exits
flush before their `ret`. Q1 (barrier vs precise) is therefore
ANSWERED BY MEASUREMENT - precise - and Q3's ordering question is moot
for 1b. Four `-rt` entries pin it, each watched failing; the full record
is `docs/jit-optimizations.md`, *#97 increment 1b*, and the plan's §3b
carries what it found and what it leaves open (caller-saved pins in a
call run; `lea` for `n - k` off a pinned source; the scan's unused
register at K=4; `visit_use_def`'s missing store family). The text
below is the pre-landing analysis, kept because its measurements are
the argument.


The plan's §3b USED TO SAY: *"a fragment that emits a MyLang call gets
4 pinnable registers of 13"*, because `jit_run_blocks_xcache` denies the
caller-saved half of the pin pool to any run containing `CallV`,
`CachedCallV` or `CallValueV` — so the increment was "bracket the call
site so the denial can drop".

**THAT PREMISE IS FALSE. MEASURED 2026-09-04: SUCH A RUN GETS ZERO
PINS.** `MYLANG_JITSTATS` on 09_fib_recursive prints `pick_decided 2`
and `runs_compiled 2` and **no pin row at all** — no `cache`, no
`lsra_pins`, no `xcache`. Same for 78, 11, 63, 76.

**THE TWO-PROGRAM EXPERIMENT that isolates it.** One identical hot loop
with five live accumulators, differing only in whether the body calls an
impure function that survives to codegen:

    # A - no call in the loop            -> lsra_pins 1  lsra_trans 11
    var g = 0;
    func hot(int n) {
        var a = 0; var b = 1; var c = 2; var d = 3; var e = 4;
        for (var i = 0; i < n; i++) {
            a = a + i * 3;  b = b ^ (i >> 1);  c = c + a - b;
            d = d + c * 2;  e = e ^ (d + i);
        }
        return a + b + c + d + e;
    }
    print(hot(runtime(200000)));

    # B - the SAME loop plus `g = g + sink(i);` in the body, where
    #     func sink(int x) { g = g + x; var q = x * 3; var r = q - 1;
    #                        var t = r ^ 5; g = g + t; return t; }
    #     (impure so it is not folded, and over the inline weight gate
    #      so it is not spliced - `call.v` verified present at pc 19 in
    #      `mylang -nj -vd`)
    #                                    -> NO pin row at all

**THE REAL BLOCKER IS `pick_visit_op` (jit.cpp), NOT THE XCACHE
DENIAL.** That switch classifies which slots an op touches; its
`default` arm returns **false**, meaning *"an unclassified op — the
caller must assume EVERY slot is touched"*, which disqualifies the whole
run from pinning. The CALL family lands there deliberately, and the
comment already names this increment:

    the CALL family (CallV/CachedCallV/CallValueV/CallValueGenericV -
    whether a call should be a barrier instead is #97's decision,
    entangled with the call protocol)

`jit_run_blocks_xcache` is therefore a SECOND gate, denying half of a
pool the run never reaches. **Deleting it alone — which §3b prescribed —
would change nothing measurable.**

**WHAT 1b ACTUALLY IS:** classify the CALL family in `pick_visit_op` as
a BARRIER instead of leaving it unclassified. `IncDecChainV` is the
precedent already in that switch (`v.mark_barrier(pc)`, commented
"BRACKET (not a branch)"). The call must additionally declare what it
really touches — the argument RUN and the callee slot are read from
memory, the dst is written.

**WHAT IS ALREADY IN PLACE (good news, verified by reading the code):**

 - `emit_sync_call_inline` (jit.cpp ~8924) ALREADY brackets its whole
   sequence: `emit_call_prologue(e)` at the top, and FIVE
   `emit_call_epilogue(e)` copies — one per exit — ALL of them after the
   `call rdx` at ~9082/9090. So a caller-saved pin would be spilled
   before the raw scratch and reloaded after the callee returns.
 - `emit_call_prologue` already spills caller-saved GP pins AND the
   float pins to their slot PAYLOADS; `emit_call_epilogue` reloads them.
 - The register tracker already encodes the rule:
   `if (trk_bracket > 0 && !(gp_caps(r) & CAP_CALLEE_SAVED)) return;`
   with the comment *"spilled by the prologue; reloaded after"*.

**WHAT IS NOT DECIDED — see the QUESTIONS section below.** Three
hazards a successor must not walk past:

 1. **Barrier vs. precise classification.** A barrier is conservative
    and simple. Declaring the arg run / callee slot / dst keeps more
    pins alive but must be exactly right, or the analysis pins a slot
    the call clobbers.
 2. **The exception path.** A callee can throw through the fragment.
    The exceptional exit already does `emit_call_epilogue` then
    `exit_pc`, so pins are reloaded and then flushed — but that must be
    PROVEN with `norec_enum.py` (a throw crossing a call frame is
    exactly its shape space), not assumed.
 3. **The staging hazard, which has already shipped a wrong answer
    once.** A bracketed helper call's own ARGUMENT STAGING can clobber
    pins that later cache-aware argument reads still trust; it was fixed
    with `load_operand_avoid` and the first `alloc_scratch` conversion.
    Admitting pins into call runs re-opens exactly that surface.

**GATE (unchanged in spirit, restated in the right units):** `-vdj` on
`fib$0` must show the entry pushing more than the callee-saved set, i.e.
`lsra_pins`/`cache` must become NON-ZERO on a call-containing run; and
the suite geomean must not regress. A run whose pins all sit in
callee-saved registers pays nothing new, so a regression would mean the
newly-admitted pins are worth less than their spill.

**⛔ RE-READ `jit_run_blocks_xcache`'s OPCODE LIST when doing this.** It
is an audited table with the documented staleness risk. If the barrier
lands, the list may be deletable outright — the better outcome.

### 1.5 INCREMENT 1c — ✅ DONE 2026-09-19 (with the probe elision)

**STATUS AS OF 2026-09-19: LANDED**, together with the pure-cache PROBE
ELISION (the CachedCallV site emits no probe / fork test / stash / key
store while `g_pure_cache_enabled` is off - an emit-time fact since
increment 0's ML_CHECK). 78_typed_param_call's 2,000,001 calls all take
the baked arm now (`bake_push` 0 -> 2,000,002, `bake_widen` 1,000,000,
the generic coercing counters 0): -13.6% Ir. 09_fib -17.5% Ir from the
probe elision alone. Record: `docs/jit-optimizations.md`, *#97 increment
1c*. The text below is the pre-landing analysis.


E1 named 78_typed_param_call's callees and the bench did not move one
instruction, **because naming was never what stopped it**. The FIRST
bake gate is

    if (bake && !(bake->fast_bind
                  && (int)bake->params.size() == NARGS
                  && bake_ck && bake_ck->plain_frame))
        bake = nullptr;

and `compute_bind_flags` (eval.cpp) clears `fast_bind` for ANY parameter
annotated `int` or `float`:

    for (const auto &p : d->params)
        if (p.decl_type == DeclType::i || p.decl_type == DeclType::f)
            fast = false;

**THE TWO PROGRAMS, differing in one word:**

    func mk(base)     { return func [base] (k)     { return base+k; }; }
    var add = mk(7);  for (...) s = s + add(i);
    # BAKES: bake_push 1 -> 1,000,001        (11_closure_counter's shape)

    func mk(int base) { return func [base] (int k) { return base+k; }; }
    var add = mk(7);  for (...) s = s + add(i);
    # DECLINES: fast_bind is false, bake_push stays 0   (78's shape)

The annotated one is the shape 78 exists to measure — its own header
comment calls it *"the shape a typed language should make FASTER, not
slower"* — and it is the one that loses the tier. It was kept out of #97
step 4 as *"a coercing one ... its per-argument checks are a separate
shape"*, which was right then and is now what stands between this arc
and its own flagship bench.

**WHAT IT NEEDS:** the emitted bind copies each argument raw. A coercing
callee needs, per parameter, either a proven-exact copy or a widening
(`int -> float`) — and that per-argument decision is a COMPILE-TIME
constant once the callee is baked, which is exactly what E1 made
available at a value site. `FuncDescriptor::bind_req` already holds the
per-parameter Type singleton the copy must match, computed by the same
`compute_bind_flags`; the gate can consult it instead of refusing.

**GATE:** `bake_push` on 78 must go 0 -> ~2,000,001, and `-vdj` must
show the widening emitted inline for `scale_it` (int argument, float
parameter) rather than a call to the coercing helper. If the emitted
per-argument sequence is LONGER than the helper call it replaces, STOP —
that is a decline, not a tier.

**⛔ SIZE IT ON THE HELPER CALL IT REMOVES, NOT ON THE GATE COLLAPSE.**
E1 measured the gate collapse at ~0 wall clock. What 1c removes is a
per-argument HELPER CALL and its argument marshalling, which is real
work — but profile it before building, the way increment 0 was.

### 1.6 The REST of #97

    2.  THE CALLEE-SIDE PROTOCOL - ✅ DONE 2026-09-19 (increment 2,
        LEAF callees only; the gate met, see 1.1). Two things a
        successor must know that the record spells out: the tier
        EXPOSED a pre-existing -2 conveyance bug (a warmed call's
        callee frame rendered "at line 0" - fixed in
        vm_jit_stamp_callee_frame, pinned by
        vm_warmed_throw_backtrace_parity, shape-eater #9 in CLAUDE.md),
        and the release scan on the exception exit was only testable
        with the new refcount() dev builtin (a leaked reference is
        otherwise invisible: aliases never copy, pooled objects hide
        from LSan).
        THE NEXT MICRO-INCREMENTS ON THIS PROTOCOL, ranked by the
        per-call profile (plan section 3 item 2): the capture protocol
        (a base register from the site), the window init (skip tails
        no helper writes), the ref-listed arg-temp staging guard, the
        caller-built window (recovers the #162 fusion), the float pin
        spill around the call (#124).
        INCREMENT 3 (2026-09-20, in progress) takes them in the order
        that makes the patches simpler, verified on the expected -vdj
        text first (the maintainer's instruction; perf at the end):
        T0 the dump driver (it lied about main - fixed, one JIT driver
        for compile/load/dump), W1 the caller-built window (done, a
        relocation), W2 the fusion into it (done: the staging move
        gone, a pin stored from its register, the coercing check an
        emit-time fact - 78's add(i) 90 -> 73 instructions per call),
        then E3 (done the same day: the two-way site serves 76), then
        W3 the init elision (done: a raw-written unlisted slot is left
        uninitialised, poisoned in TESTS builds - 78's add(i) 73 -> 66
        per call), W4 the capture base (done 2026-09-21: the entry
        loads it from fo, ctx.captures untouched, poisoned in TESTS
        builds - 78 -7.1% Ir, 11 -8.9%, 76 -2.0%), W5 the inline
        borrow at the site + the arm's borrowed skip + the scalar
        parameter tails (done 2026-09-21: the noescape bit is baked, a
        non-slice reference is a raw copy with the flag set, the
        helper kept for a slice; myv v17 stores the bit - 76 -13.5%
        Ir, 78 -2.5%), then W6 the CALLER-SAVED capture base (done
        2026-09-22: a call-free frameless body's base joins no save
        list - 78 -1.30% Ir, 11 -1.40%, 63 -0.32%. ⛔ HALF the step the
        W4 record predicted: the entry's 16-alignment filler buys the
        dropped push straight back, so the win is the exit's `pop`
        alone; and `--xrot` caught the return arm's raw use of r11 on
        the first run, which is why the base now carries a LIVE RANGE
        ending at the terminal ReturnV), and then SP1/SP2/SP3 - the
        16-alignment invariant itself (done 2026-09-22, the
        maintainer's call after the W6 finding): ONE call seam, an
        emit-time rsp MODEL that checks it, and the rule relaxed from
        "aligned everywhere" to "aligned AT the call". W6's second
        instruction is recovered (11 -1.56% Ir, 78 -1.39%, 34 -0.42%,
        35 -0.37%, 63 -0.34%; wall geomean 1.005x - flat, as the
        guard-elision family always is). ⛔ It is the prerequisite
        #124(b) needed: the number of caller-saved pins spilled around
        a call varies per run, and half those shapes would otherwise
        have paid a wasted push/pop PER CALL on the hot path. Record:
        SP1/SP2/SP3 in the JIT record; the four rules a new emitter
        must obey are in CLAUDE.md under THE 16-ALIGNMENT RULE.
    3.  E2 - drop the leaf rule. Serves 09_fib. GATE includes
        norec_enum --depth 4 (2272 programs x 4 engines), because a
        throw crossing a frameless frame is exactly its shape space.
        ⛔ Its gate must be re-derived from 09_fib's NEW baseline
        (increment 0 made it 20.7% faster), not from the old -4.75% row.
        ✅ **v1 DONE 2026-09-23** (self sites + E2b, the call dst raw;
        record: *#97 E2* in the JIT record): 09_fib **-30.5% Ir, 0.68x
        wall**, 10_recursion_deep **-23.5% Ir, 1.00x wall** (1.09-1.13x
        SLOWER before E2b - the per-call window init; found by attributing
        CYCLES to JIT code with perf + a map from MYLANG_JIT_MAP), every
        other call bench flat. ✅ **E2c DONE 2026-09-23**: the self
        site's callee identity chain is elided (09 -11.0% Ir, 10 -9.1%;
        record: *E2c* in the JIT record). ✅ **E2d DONE 2026-09-24**:
        on the native stack a self site keeps no depth count - the floor
        bounds it, and a boundary blocks the floor (09 -4.5% Ir, 10
        -3.6%). ✅ **E2e DONE 2026-09-24**: the vframe is published
        lazily in a calling frameless body - before its C++ calls only
        (09 -7.0% Ir, 10 -5.7%). ⛔ MEASURED BEFORE BUILDING: the next
        listed item, a non-SELF callee from a non-main caller, has ZERO
        hot reach in bench/ (the one candidate, 12's apply -> sq, is
        inlined away; the rest are chain entries or cold outer calls) -
        it serves programs with helper functions called from functions,
        which no bench measures. ✅ **F1/F2 DONE 2026-09-24** (the
        maintainer: write the bench first): benches 91/92 written, then a
        function calls a LEAF framelessly (F1, 91 -80% Ir) and a calling
        frameless body may call a leaf (F2, 92 -60% then -29%; record:
        *#97 F1/F2*). ✅ **G1 DONE 2026-09-24** (bench 93 first): a
        CALLING callee at another function's site, placed at RUN time
        through Chunk::frameless_entry_abs (93 -36.5% Ir; record: *#97
        G1*). ✅ **G2 DONE 2026-09-24**: no identity chain for ANY
        named write-once capture-free callee (93 -8.9%, 91 -6.7%, 92
        -4.4% Ir - but WALL FLAT, the guard-elision signature; record:
        *#97 G2*). ✅ **H1 DONE 2026-09-24** (benches 94/95 first): a
        frameless body may call builtins (95 0.87x wall; 94 0.99x - its
        cost is the BUILTIN CALL itself, 51% in jit_call_builtin; record:
        *#97 H1*). ✅ **B1 + B2 DONE 2026-09-24** (the maintainer: do
        both): SmallArgs for the builtin argument buffer (94 -14.6%, 41
        -11.0%, 32 -7.5%) and abs/min/max on proven ints lowered to int ops
        (94 -82.8%, 95 -76.1%; records: *#97 B1*, *#97 B2*). Then #124.
        NEXT was: a non-SELF callee
        (needs the callee's placement facts at a non-main site - the
        seventh audit-table shape), builtin calls in a calling body.
        **2026-09-23 - THE DESIGN, after the maintainer revised RULE 2**
        (recursion depth is an unspecified property of the
        environment, like speed; what must hold is that a runaway
        recursion ends in a catchable StackOverflowEx). A calling
        frameless callee's window stays on the NATIVE stack. What is
        left hard is the depth-cap SWITCH (a deep sync call hands its
        callee to the interpreter and returns -3 up the native chain,
        which the materializer turns into records - impossible for a
        window about to be unwound with the native stack). The rule
        that makes it tractable: **A SWITCH NEVER PASSES THROUGH A
        FRAMELESS FRAME.**
         - D1 THE GATE: frameless_ok admits a body whose only MyLang
           calls are SELF calls (v1: 09_fib's shape; other callees
           later, they need placement facts the compile order hides).
           A CachedCallV counts as a call only while the pure cache is
           ON (with it off the probe is elided at emit time, #97
           probe-E1). Builtin calls stay refused in v1.
         - D2 THE SELF SITE: in a non-main caller, a self call is a
           frameless site (window on the native stack, `call rel32`
           to the fragment's own frameless entry). It keeps the sync
           DEPTH GUARD.
         - D3 AT THE CAP: the decline asks whether the CALLER is itself
           frameless (the arm's discriminator, `lea rax,[rbp+32]; cmp
           rax,rbx`). No -> today's slow tier, which may switch. Yes ->
           a BOUNDARY call: the callee runs under a nested dispatch
           loop that consumes every switch below it and returns the
           value synchronously. Depth stays >= cap inside, so no
           frameless frame exists there and no second boundary nests:
           one extra C frame per chain. The frameless site ML_CHECKs it
           never sees -3.
         - D4 EXCEPTIONS convey natively level by level, as a leaf's do
           today (the callee's pre_ret release, the site's postexit
           stamping the callee frame) - generalised, not new.
         - D5 THE WALKERS: the materializer never meets a frameless
           frame (asserted); the release-mode reconstruction
           (MYLANG_RECON_AT) learns bit 0 of [rbp+24].
        Then the lean-site items below apply to the frameless self
        site too (the entry offset, the captures, the typed argument).
        GATE: 09_fib Ir per scale unit vs today; driver_checks'
        overflow case; norec_enum --depth 4; every matrix.
        The earlier revision, kept for its profile: the site reads at
        RUN time facts that are emit-time constants for a self call -
        the callee's entry offset, the `norec_ok` fork and its
        high-water gate, the captures relay, the boxed four-qword
        argument copy (fib$0: 144 Ir per call, the site 74).
        ✅ **CB1 DONE 2026-09-25 (the maintainer: the callback
        protocol next)**: the raw scalar callback bind
        (`VmInvoker::call_scalars`) - 34_sort_custom_cmp -16.1% Ir,
        0.82x wall. The August rejection of the same idea did not
        reproduce on native hardware (backend-bound per top-down, a
        same-binary A/B -23% cycles); record in plans/top5-cpp-gap.md.
        ✅ **CB2 DONE 2026-09-25**: map/filter pass a flat array's raw
        element - 35_map_filter -8.8% Ir, 0.92x wall (the map half;
        map's result is general, so filter still boxes).
        ✅ **CB3 DONE 2026-09-25**: map/filter build a flat result for a
        proven flat destination (myv v23) - 35_map_filter -22% Ir,
        **0.50x wall**.
        ✅ **CB4 DONE 2026-09-25**: find/sum keys take a flat element
        raw - new bench 96_find_sum_key -6.4% Ir, **0.66x wall**;
        make_dict measured +5 Ir/call (its key is boxed anyway), kept
        boxed.
        NEXT on the callback path: the post-call release scan of
        seeded-but-scalar lambda params.
    4.  E3 - the two-entry inline cache. ✅ DONE 2026-09-20 as the
        two-way frameless site (76 -20.2% Ir; record: *#97 E3*).

**✅ FIXED 2026-09-22 (the maintainer asked the same day) - THE
DEAD-MODEL POPULATION, 18 -> 0.** Two causes, different in kind.
`MoveV` emitted an UNREACHABLE helper arm whenever neither slot was
ref-listed (15 of the 18) - #113's rule for the capture read, which
this op never got; eliding it also drops the `jmp` that hopped over
it, one EXECUTED instruction per move, and un-bumps `n_prologues` so
such a run reads as call-free and drops its entry filler too. 45_gcd
**-2.93% Ir** from five fewer emitted instructions, 10_recursion_deep
-1.25%. The other 3 were NOT dead code: `emit_reg_shift` hand-encoded
its three branches (`0F 88`, `0F 8C`, `E9` + a bare `patch32`), so the
seams never saw them and the model went blind over perfectly reachable
code - routed through `j32`/`jmp32`/`patch32_here`, byte-identical,
RAWENC 12 -> 10. `jit: SP - the CALL SEAM is total` is the ratchet at
zero; both causes watched failing.

**⛔ THE STANDING KILL CRITERION (plan section 4):** if increment 2's
gate comes back byte-flat on the WALL CLOCK while Ir drops, STOP AND SAY
SO. That is the guard-elision signature, recorded three times in this
repo, and it would mean E2 and E3 buy nothing and should not be built.
E1's result (gate collapse -> ~0 wall) is direct evidence bearing on it.

**DECLINED ON MEASUREMENT (do not retry):** caching the window SIZE or
the fragment ENTRY in the callee-cache cell.
**DECLINED ON DESIGN:** pinning {fo, entry, nslots} in callee-saved
registers (the original "call preheader") — 4 of r12-r15, and the
baked-callee form gets the same facts for ZERO registers; and
maintaining a derived `ctx->captures->data()` in an arena cell.

---

## 2. TASK #107 — INTRUSIVE_TESTS  [PENDING, NOT DESIGNED]

**IDEA CAPTURE (maintainer, 2026-08-26) — deliberately not designed.**

A dedicated build define (working name `INTRUSIVE_TESTS`, separate from
`TESTS`) under which the interpreter grows a large, unapologetically
invasive test surface:

 - MANY more test-only BUILTINS, callable from ordinary `.my` test
   programs, that read internal C++ state directly: counters, tier
   ledgers, register-allocation decisions, refcounts, storage kinds,
   pool contents, frame/slot state, live-slices sets, chunk and fragment
   metadata.
 - The same surface can FORCE conditions, not just observe them — make a
   tier decline, make an allocation fail, pin/deny a register, take a
   cold arm, perturb a heuristic — so a rare path becomes the ONLY path
   from inside a test program (the `MYLANG_JIT_COLD` philosophy,
   generalised and made per-call-site).
 - HOOKS at every pipeline stage: lexer, parser, const-eval, inferencer,
   resolver/optimizers, the AST tree-walker, codegen, the VM's dispatch,
   and the JIT's helpers/emitted code. A test registers a callback and
   observes or steers what that stage did.

It may be as big and invasive as it needs to be **provided all of it
sits behind one fat `#ifdef INTRUSIVE_TESTS`**, so a shipping build and
even an ordinary `TESTS` build are byte-identical to today.

**WHY (motivating evidence, from the #97 increment-1 session):**
 - an emitted-code counter proves a tier RAN, never that the test
   OBSERVED what it does — `g_jit_elemv_fast` bumped 17 times while the
   test was vacuous;
 - the value a tier writes can be LAUNDERED before a program can look at
   it (a raw slice copy in a temp, re-copied properly by the next
   `MoveV`), so a whole class of defects is unobservable from MyLang
   source no matter how the test is shaped;
 - the shape-eaters (LICM, const-fold, inlining, tail-inline,
   specialization) keep a test's program from reaching the code under
   test, and every one was found by hand, one reintroduced-defect run at
   a time.

**PRECEDENT ALREADY BUILT, worth reusing:** `MYLANG_JITSTATS`,
`MYLANG_JIT_OFF` / `_FORCE` / `_COLD` / `_XROT` / `MAXPINS`,
`MYLANG_RECON_AT`, `MYLANG_NO_LOWMEM`, the `JitProbe` structs, and the
per-decline-reason counters from #97 increment 2. This task is the
general form of all of them.

**OPEN QUESTIONS for its design phase:** whether the builtins are a
separate namespace or reuse `make_dev_builtin`'s gating; how a hook is
registered from a `.my` program vs. from C++ (`-rt`); which CI lane
builds it; and how to stop the surface itself from rotting — *a hook
nobody calls is a hook that lies*.

**THIS SESSION ADDS ONE MORE PIECE OF MOTIVATING EVIDENCE:** E1's live
descriptor set had to be installed in three different codegen drivers,
and getting one wrong made the whole tier inert while every counter
still read plausibly. An intrusive surface that could assert *"this
compile installed a live-descriptor set"* would have caught it
instantly; nothing available today could.

---

## 3. TASK #110 — VM `foreach` OVER A FLAT STRUCT ARRAY  [PENDING]

**MEASURED 2026-08-26** while sizing #97's sibling cases.

`foreach (var p in flat_struct_array)` where the WHOLE `p` is used
lowers to `LoadStructElemV`, whose helper is

    f->at(dst).put(vm_struct_elem(f->at(base).get(), idx));

and `vm_struct_elem` MATERIALIZES A FRESH `StructObject` per iteration —
a heap allocation plus a bytes copy, then a free when the loop var is
overwritten next iteration.

**THE TREE-WALKER DOES NOT DO THIS.** CLAUDE.md's struct section records
that its `foreach` REUSES ONE `StructObject` across iterations,
overwriting in place behind a `use_count` COW guard, so a captured
element keeps its value. The VM never got that optimization.

**EVIDENCE** (callgrind, `OPT=1 ASSERTS=0`, a 1000-element flat struct
array x 300 reps): 521 Ir per iteration, of which

    _int_free            12.15%
    vm_struct_elem       11.92%
    malloc                8.16%
    jit_load_struct_elem  8.14%

malloc+free alone is ~20%. **This is why an inline JIT tier is the WRONG
lever here** — it could only remove the 8% call, while the allocation it
cannot touch is 2.5x that. Port the reuse instead.

**SOUNDNESS IS THE WHOLE JOB**, and the tree-walker's guard is the
model: the object may be reused only while nothing else holds a
reference (`use_count`), else the loop body captured it
(`append(out, p)`, a closure) and each captured element must stay
distinct. **Get that wrong and the corpus will not tell you:**
65_struct_field_sum reads only FIELDS, so it never materializes an
object at all. The shape needs a dedicated test, and probably a bench.

**REACH:** `load.selem` occurs ZERO times in `bench/` + `samples/` — a
corpus hole of the same family as the field store's.

---

## 3b. TASK #124 — REMOVE THE 4-REGISTER CAP ON CALL-CONTAINING RUNS
## [PENDING — maintainer's call 2026-09-19: "completely fix this with
## both approaches, but not as a detour right now"]

**THE CAP, precisely.** A native run holding a MyLang call may pin only
in CALLEE-SAVED registers. SysV has six (rbx, rbp, r12-r15); rbx is the
slots base and rbp the frame anchor the record-less walk depends on, so
FOUR. The nine caller-saved ones (rax, rcx, rdx, rsi, rdi, r8-r11) are
denied to such a run by `jit_run_blocks_xcache`, for two reasons: the
`call` clobbers all nine by ABI, and the sync push
(`emit_sync_push_native` / `emit_sync_call_inline`) uses EIGHT of them
as raw protocol registers across one straight-line stretch -
`jit_assert_no_volatile_pin` is the tripwire. A call-free run gets all
thirteen.

**IT IS NOT WHAT STANDS BETWEEN THE SIX WORST BENCHES AND 5x.** The
`MYLANG_JIT_MAXPINS` sweep (2026-08-18) found every pin past the first
one or two worth +0.00% Ir on the int loops, and on 09/11/63/75/76/78
the call protocol is 74-169 Ir per call on the caller side alone -
10-20x any register effect (the per-call map is in the #97 1b record).
So this is filed as its own task, after the protocol work.

**THREE PARTS, all wanted:**

 (a) **THE ALLOCATOR BUG - fix first, it is small.** At K=4 with six
     candidates the linear scan uses THREE registers: an eviction
     victim's early piece is demoted (no use inside it) and the freed
     register is never re-offered to the losers. Repro, no call needed:

         MYLANG_JIT_MAXPINS=4 MYLANG_LSRADBG=1 mylang -npc progA.my
         # progA = program A of plans/frameless-callee.md §3b:
         #   a/b/i -> regs 0/1/3, c/d/e -> reg -1 (homes), reg 2 IDLE

     On program B (the call twin) that is c in a stack home while r15
     sits idle. Gate: all four used at K=4; `-vdj` shows r15.

 (b) **SPILL CALLER-SAVED PINS AROUND THE CALL.** The mechanism exists:
     `emit_call_prologue` stores each caller-saved pin to its slot
     payload and `emit_call_epilogue` reloads (every helper call does
     this). Lifting the denial for a sync call costs a store + a load
     per caller-saved pin per call - and the 1b barrier measurement
     says stores on the call path are exactly what the wall clock sees
     (11_closure_counter 1.10x for 2.4% Ir). So it needs a
     PROFITABILITY rule: pay only in a loop with more than four hot
     locals and few calls per iteration. The push emitters' own use of
     r10/r11/rax/rcx/rdx/rsi/r8/r9 is legitimate inside the bracket
     (the prologue already spilled); `jit_run_blocks_xcache`'s list
     may then be deletable outright. Gate: Ir + wall on a loop with
     8 hot int locals and one call per iteration, vs the same loop
     with the cap.

 (c) **IPA-RA FOR A BAKED CALLEE - what g++ does.** On program B's C++
     twin g++ -O2 keeps all six loop values in CALLER-saved registers
     (rcx, r8-r10, rsi) across `call sink`, legal only because it sees
     `sink`'s clobber set. A BAKED callee's fragment is known to the
     JIT at compile time just as well: its own pins are callee-saved
     (saved/restored by its frag_entry), so what it clobbers is its
     scratch use. The obstacle is OUR side: the push protocol's eight
     scratch registers are clobbered before the callee ever runs, so
     (c) needs the push rewritten to use two or three scratch registers
     - which is also what increment 2 (the frameless callee) changes.
     Do (c) after increment 2, on the frameless protocol, not before.

**WHY NOT DURING #97:** the maintainer's rule - no detour. #97's
remaining increments (1c, the probe elision, increment 2) are where the
5x lives; this task is filed so it is not forgotten and so a successor
does not rediscover (a) as a mystery.

---

## 3c. ✅ FIXED 2026-09-20 (the maintainer's call, the same day) — A
## BIND-COERCION ERROR'S CARET DIFFERED PER ENGINE (found during #97
## inc 3 W2; RULE 2)

**REFINED the same day, on the maintainer's "as accurate as possible":**
a bind coercion now carets the FAILING ARGUMENT alone (`f2(i, z)` with a
string in `z` underlines `z`), in every engine and from a `.myv` image -
the bind records the parameter index (`Exception::bind_arg`), the call
ops carry one span per argument (`Chunk::arg_locs`, myv v16), and the
JIT's conveyance stamp selects on the index at run time. An arity error
keeps the list. Record: docs/jit-optimizations.md, *RULE 2, refined*.
The list-span fix it refines follows.

The fix, in one line: the user call ops carry the ARGUMENT LIST's span as
their second caret (`base_locs`), the interpreter and the JIT stamp a
loc-less setup throw with it, and the tree-walker's devirtualized
`DirectCallExpr`/`CachedCallExpr` reproduce the plain `CallExpr` catch -
which was a FOURTH divergence the fix turned up: a named function's call
marked the whole call, a closure's the arguments, in the tree-walker
itself. Four 5-mode `err loc:` tests with the exact spans (each throwing
on a warmed re-descent, so the JIT's emitted sites are what is tested),
watched failing in every mode. Record: docs/vm-ops.md, `Chunk::base_locs`.
The original finding follows.

    func mk2(int z) { return func [z] (int a, int b) { ... }; }
    var f2 = mk2(1); var dyn z = 0; z = runtime("str");
    ... s = s + f2(i, z);        # a string into `int b`

The C++ call tier raises `TypeErrorEx: cannot store a non-int value in
an 'int' variable ...` under every engine, but the CARET is not the
same: the VM+JIT carets the whole call `f2(i, z)` (col 44:52), the
tree-walker the argument (col 47:51), and the interpreted VM (`-nj`)
prints NO location at all. Present on the inc 2 and W1 trees and with
`MYLANG_JIT_OFF=frameless`, so it is the C++ sync tier's stamp, not
the frameless site's. The reach harness compares backtraces and
messages, not carets, which is why no test fails. A `tests` entry with
`ex_col` pinned per engine would; the fix is in the bind's raise site
(the argument's caret is what the tree-walker's `bind_param` stamps).
Small, but a caret that differs between engines is a RULE 2 violation
and is on record. Not a detour from #97 - the maintainer's call.

## 3d. FOUND 2026-09-20/21 — THE LOADER FINDINGS (`myv_fuzz`,
## #137/#142 class; ✅ ALL FIXED 2026-09-22) AND ONE TABLE GAP (the
## tenth shape, again)

**The loader (task #26).** All reached by the mutation space a format
change shifted (v16 `arg_locs`, then v17 `noescape_params`), all
pre-existing, reproducers kept in `myv-fuzz-bad/`: `fat-676.myv` - an
`InternalErrorEx` thrown through the `noexcept` `jit_load_struct_elem`
-> `vm_struct_elem` -> `flat_structs()`' untrusted check =
`std::terminate` (the same on the debug and the assert-free build);
`small-938.myv` - a HANG in the load-time JIT's `jit_liveness_core`,
whose fixpoint never converges on a mutated chunk. Both still open,
both violate "no input may crash the interpreter".
✅ FIXED 2026-09-21: `fat-486.myv` - ONE byte turns a `LoadLiteralObjV`
into a `PopHandler` with no `PushHandler` before it, and the
interpreter's `act.handlers.pop_back()` ran on an EMPTY vector (a SEGV
in the ASSERTS=0 build, a hardening abort in the debug one). Fixed
STATICALLY, not by a run-time gate: `handler_balance_fault` (codegen.h)
is a forward dataflow of the handler depth over the chunk's CFG, run
by `verify_chunk` on every stored chunk BEFORE the load-time JIT (which
inlines the same pop as a bare `finish -= 4`) and, ASSERTS-only, on
every chunk codegen emits - the whole corpus and `-rt` prove it accepts
our own output. Pinned by `myv_verify_handler_balance` (three arms:
a pop at depth 0, a join at two depths, a region pushed twice).
✅ FIXED 2026-09-21, found by READING (auditing the store family's
operand layouts for #25), not by the fuzzer: `StoreElem2V`'s
`chain_locs` index was unbounded (the VM indexes the pool with it, the
load-time JIT bakes the entry's address) and the two-level consumers
read a PAIR out of the entry that a bounded index alone did not
guarantee (LoadElem2Int/Float had that half); and the verifier SKIPPED
the frame-slot bound on a lit-flagged operand of DictStore /
StoreElemValue / StoreMemberV / StoreElem2V, which the VM reads as a
slot unconditionally. `chain_pair`, `a_slot_only`/`b_slot_only` in
verify_chunk; pinned by `myv_verify_store_operands` (nine patched
chunks, all nine ACCEPTED before the fix).
✅ FIXED 2026-09-22 (the maintainer's call: derive, do not accept):
`small-1305.myv` - one bit in the STORED `ref_slots` list unlisted the
temp two closures were built in; the release scan skipped it and the
image ran to the right answer while LeakSanitizer reported both. The
record is GONE from the format (myv v18): `myv_read` rebuilds
`ref_slots` from the verified code with the descriptor's seeds
(`compute_ref_slots` / `ref_seeds_of`, codegen.h, shared with the
compile), after `vm_verify_program` and before the JIT. Pinned by
`myv_ref_slots_derived` and the round trip's entry-for-entry compare.
✅ FIXED 2026-09-22 (the maintainer's call: "fix all five loader
findings first, then back to #97"): `fat-676.myv` - `jit_load_struct_elem`
is a STATUS helper now (`catch (RuntimeException &)` -> g_vm_jit_exc,
the emitter's `test eax` after the call), and LoadStructElemV moved from
`op_never_exits` to `op_fully_native`'s convey family - a conveying op
cannot sit in a NATIVE_LEAF, whose direct caller ignores the status.
Reach: NO corpus program emitted `load.selem` at all (a whole-`p` bind
needs a value use of `p`; every corpus foreach read fields only), so
the leaf-bar move costs nothing; `tests/functional/27_struct_whole_p.my`
gives the corpus_diff matrices the shape. Pinned by
`myv_struct_elem_base` (two retargets: a flat int array -> the
untrusted tier's InternalErrorEx, the finding; an int -> TypeErrorEx;
the emitted helper must have RUN). Watched: the void shape terminates
the whole suite. `small-938.myv` - the planned StructCtorV's `a` dual
(the computed-arg mini-run: read by NO handler, only by visit_use_def's
`run(base, cnt)`) was unbounded, and a count of 0x73A07676 was the
"never-converging liveness". `verify_chunk` bounds it like every run,
and a `-1` base must carry a zero count; pinned by
`myv_verify_ctor_minirun` (both ACCEPTED before). Note the finding was
misfiled as a JIT fixpoint bug for a day: a HANG in the loader is a
COUNT until proven otherwise.
**And the v18 images shifted the seeded mutation space onto FOUR more
(2026-09-22, all pre-existing classes):** ✅ `small-60.myv` - a
mutated `StructCtorV` dst leaves `p` never written, and W3's
`jit_chunk_frameless_init_free` asks only that every WRITE of a slot
be raw, so a slot with NO write at all kept its bit and the frameless
site left it uninitialised: a SEGV in both builds (`-nj`: a clean
TypeErrorEx). FIXED the same day: `chunk_read_before_write` (codegen.h,
a definitely-written MUST dataflow over the CFG) feeds
`Chunk::frameless_read_first`, which the site subtracts from the LOCAL
half of init_free (a parameter's bit is ignored - the fill writes it);
`jit_chunk_frameless_derive` is the one derivation point now. Emitted
code byte-identical corpus-wide (vdjcmp 127/127); pinned by
`jit_frameless_init_free_read_first`. ✅ `fat-260.myv` / `fat-845.myv` -
debug-only `ML_VM_CHECK`s on codegen-proven arms (`vm_unpack_elem_body`'s
array base, `read_float_slot`'s float) that a corrupt image violates, a
clean TypeErrorEx / 0.0 on the ASSERTS=0 build. FIXED 2026-09-22 with the
`ml_untrusted_bytecode()` idiom (defs.h): the tripwire stays for our own
bytecode and is silent on an image, so the outcome is the same in every
build; pinned by `myv_untrusted_proven_arms` (both watched aborting).
✅ `fat-316.myv` - an LSan report (256 bytes) after an uncaught
OutOfBoundsEx from a mutated image. ROOT CAUSE: `root_slot_count` is
stored TWICE - the root chunk's `slot_count` and a second record in the
globals section - with different consumers: the loader pushed main's
window from the second while the JIT baked main's frame size into its
return arm from the chunk's; one mutated byte (26 -> 145) made the
exception path release a frame of a different size than the one pushed.
JIT-only, ANY mismatch reproduced it (27, 40). FIXED 2026-09-22 as myv
v19 - in two steps: a first fix DROPPED the second record ("derive it"),
and the next fuzz run's `fat-311.myv` mutated the now-single copy
(22 -> 101) undetected: the register cache took the temps for locals,
pinned one that also held a reference, and leaked it (`-nj` clean,
`MYLANG_JIT_OFF=cache` clean). `slot_count` is primary data with no
derivation, so the copy is kept and CHECKED (`vm_verify_program`, the
function chunks' slot_count == frame_size rule); pinned by
`myv_root_slot_count_checked` (both copies tampered, each refused;
watched: both accepted with the check out).
**And the new functional test found a CODEGEN bug on the way (the same
day, fixed):** the struct-foreach direct-read mapping (`sfe_*`) was four
scalars, so an INNER fields-only struct foreach overwrote the outer's and
cleared it - `foreach (var a in pts) foreach (var b in pts) if (a.x <
b.x)` compiled `a.x` as a member read of a slot the direct-read design
never writes, and the VM threw where the tree-walker printed 255. A RULE
2 divergence no corpus program had reached. `sfe_maps` is a stack now;
pinned by a 5-mode `struct array:` entry (fails 1704/1705 in every VM
mode with the scalars back) and case 4 of the functional test.
**And a SIXTH loader finding, from running every corpus image against
its source (a check this batch added to the net battery): `mylang
prog.myv` resumed main at a STACK ADDRESS OUT OF SCOPE on any image
whose main takes a switched call (12_deep_switch, 07_exceptions,
23_baked_callee - ASan stack-use-after-scope; the release build printed
the right answer by luck). Main's VmProgram moves out of myv_read's
return slot into the driver's variable, and the JIT'd root chunk is
address-baked (NorecSite::caller, the fragment map); the compile path
called `jit_norec_rebind` after its move, the load path never did.
FIXED 2026-09-22 in the TYPE: VmProgram's move operations rebind (vm.h);
pinned by `vm_program_move_rebinds` and a driver_checks deep-image case,
both watched failing. -rt could not see it (its loads are elided
initialisations), the fuzz corpus has no switched call from main.
(`small-1305.myv`, the LSan report the fixed fuzzer reported the same
day - two `FuncObject`s from `jit_make_closure_ptr` never released - is
the v18 fix above: the maintainer answered the "does an LSan report on
a hostile image count" question with "derive ref_slots at load".)
**And the fuzzer could not SEE the third in the debug lane** (fixed
2026-09-21): UBSan under `-fno-sanitize-recover` reports and exits 1,
the SAME code a clean `MyvError` refusal uses, so `myv_fuzz.py` counted
it clean while the release build segfaulted on the identical bytes. A
sanitizer report in stderr is a CRASH now, whatever the exit code.

**The table (task #25) - ✅ DONE 2026-09-21.** `visit_use_def` did not
know the element-STORE family: each op hit its `default:` BARRIER.
Found by W3, the newest consumer, exactly as CLAUDE.md's tenth shape
predicts. Six ops audited (uses only, no defs - the COW rewrite of a
base is in place, type unchanged); the two CHAIN forms stay barriers
(their keys live in a pool the Instr-only signature cannot reach; zero
corpus reach). Measured: 76 -9.0% Ir (the site's tails and the
callee's C5-class store guards + two arm scans, 92 -> 76 per call),
60_bit_sieve -23%, 14_array_subscript -20%, 86 -20%, 68_nested -18%,
87 -14%, 38 -10%, 46 -7%, 90 -7%, 88 -6%; every other bench a
compile-time constant. Record: docs/jit-optimizations.md, the *#25*
entry. The residue this time: the chain forms, which need a
`const Chunk *` through every consumer of the table.

**Two caret residues, reported not fixed:** a LITERAL argument through
a `dyn` callee still carets the argument list (a const-folded literal
node carries no loc - a parser property, the same in every engine),
and a builtin CALLBACK's bind keeps the builtin call's list.

## 4. QUESTIONS FOR THE MAINTAINER

These are the decisions a successor session must NOT guess at. Each is
stated with the concrete alternatives.

**Q1 — 1b: BARRIER or PRECISE classification of the CALL family?**
✅ ANSWERED 2026-09-19 by measurement: PRECISE (§1.4).
A barrier (`v.mark_barrier(pc)`, IncDecChainV's precedent) is safe and
simple: pins may live in the run, and the call kills them across itself.
PRECISE additionally declares that the call reads the argument RUN and
the callee slot and writes the dst, which keeps more pins alive across
the call but must be exactly right.
*My recommendation:* start with the BARRIER and measure. If a barrier
does not make `lsra_pins` non-zero on `fib$0`, precision will not
either, and the increment stops there having cost one afternoon.

**Q2 — 1b: what is the ACCEPTANCE bar, given E1's result?**
✅ ANSWERED 2026-09-19 by the numbers: the call-containing LOOPS win
4-6% on the clock (11/63/76/78), the recursive bodies pay the pin's
push/pop per invocation (fib flat, 10_recursion_deep 1.03x), geomean
0.999x. Landed on the geomean gate; the loop-free profitability rule
is the open follow-up (plan §3b (iii)).
Admitting pins into call-containing runs means spilling and reloading
them around EVERY call in the run. On a call-dense body such as `fib$0`
that could easily cost more than it saves. §3b's stated gate is "the
suite geomean must not regress", which is a floor, not a target.
*The question:* is 1b worth landing if it is geomean-neutral but makes
`fib$0` pin (i.e. it is a PLATFORM for increment 2 rather than a win on
its own)? Or should it be judged on its own wall clock, and dropped if
flat?

**Q3 — ORDERING: 1b before 1c, or 1c first?**
✅ MOOT 2026-09-19: 1b landed first (the maintainer's call).
1b is the plan's order and is a confounder removal for increment 2's
wall-clock gate. 1c is what actually unblocks 78_typed_param_call, the
arc's flagship bench, and is independent of 1b.
*My recommendation:* 1c first. It has a clear, measurable target
(`bake_push` 0 -> ~2,000,001 on 78) and removes a real helper call,
whereas 1b's payoff is now known to be a platform effect rather than a
win. But the maintainer sequences, so this is a question and not a plan.

**Q4 — Does the FRAMELESS TIER still justify itself?**
E1 measured the gate-collapse family at ~0 wall clock, and increment 0
measured the store-burst family at 0.71x. The frameless tier removes BOTH
a record write (store burst — pays) and a set of gates (pays nothing).
*The question:* should increment 2 be re-scoped to "remove the record
WRITE for a named leaf callee" — the part with measured value — rather
than the full frameless protocol with its second frame kind, second
entry point and new exception path? That is a much smaller change with
most of the predicted benefit.

**Q5 — the local commits.** ✅ pushed by the maintainer before 2026-09-19.
 Four commits (`1767291`, `0ad4384`,
`96656ad`, `e626b84`) are ahead of `origin/exp-work`. Push, or leave for
the maintainer?

---

## 5. ENVIRONMENT NOTES A SUCCESSOR WILL OTHERWISE RE-DISCOVER

**⛔ AN ASan BUILD RESERVES ~20 TB OF VIRTUAL ADDRESS SPACE.** Measured
on this box: the ASan lane reports `VmSize` 20,480 GB with a peak
`VmRSS` of 135 MB; the release lane is 1 GB / 7 MB. The machine has
94 GB and `free` showed 89 GB FREE immediately after every incident, so
nothing was actually short of memory — but a background-task monitor
reading VmSize or commit accounting kills long ASan matrices anyway
(`corpus_diff --levers` is 23 configs x 34 programs; `--xrot` is 16 x
34). It happened three times.

Consequences, both already applied:
 - `tests/corpus_diff.sh` now sets
   `ASAN_OPTIONS=hard_rss_limit_mb=8000` unless the caller overrides it,
   so a genuine runaway aborts as a NAMED ASan report attributed to one
   program and one config, instead of the harness dying with its output
   still buffered (which is exactly what happened, twice, telling us
   nothing);
 - run the ASan matrices ONE STEP AT A TIME, not as one long background
   batch. `ulimit -v` is useless here: it kills any ASan binary
   outright, which briefly looked like "every lever is broken".

**SCRIPT INVOCATION DIFFERS BETWEEN NETS, and both forms fail loudly
(rc=2 + usage), so a mis-invocation cannot silently pass:**

    tests/norec_enum.py  BINARY --depth 3      # positional
    tests/norec_sweep.py BINARY                # positional
    tests/nested_fuzz.py --mylang BINARY --count 300
    tests/myv_fuzz.py    BINARY [--triage]     # positional
    tests/corpus_diff.sh BINARY [--levers|--cold|--xrot|--nolowmem]

**RULE B1 REMINDER for any perf run:** `rm -rf build` first, pass
`--mylang build-claude/<lane>/mylang` AND `--baseline` explicitly, read
run.py's `mylang : <path>` header line, never pass `--force`, and
confirm the header says `purecache: off (-npc, the default)`.
