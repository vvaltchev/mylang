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

**BUILD LANES PRESENT** (`build-claude/`, 843 MB): `release`, `dbg`
(TESTS=1 OPT=0, ASan+UBSan), `clang`, `rel-hard`, and `e1` (TESTS=1
OPT=0 — the lane every net in this session ran against). The throwaway
measurement lanes and their worktrees were deleted, per the lane rule.

---

## 1. TASK #97 — CALL PROTOCOL / THE FRAMELESS CALLEE  [IN PROGRESS]

**THE STANDING REQUIREMENT:** the worst `my/cpp` benches must reach
<= 5x. Plan: `plans/frameless-callee.md`. Per-change record:
`docs/jit-optimizations.md`.

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

### 1.4 ⛔ INCREMENT 1b — RE-SCOPED TODAY. READ THIS BEFORE TOUCHING IT.

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

### 1.5 INCREMENT 1c — THE COERCING-CALLEE BAKE (new, added today)

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

### 1.6 The REST of #97, unchanged

    2.  THE CALLEE-SIDE PROTOCOL (plan section 2), behind
        MYLANG_JIT_OFF=frameless, admitting LEAF callees only at first.
        The bulk: a second entry point, the stack marker, a new return
        arm, a new exception path.
        GATE: Ir on 78 and 11; corpus_diff over the five matrices; the
        norec nets re-run, since the rbp chain gains a second frame kind.
    3.  E2 - drop the leaf rule. Serves 09_fib. GATE includes
        norec_enum --depth 4 (2272 programs x 4 engines), because a
        throw crossing a frameless frame is exactly its shape space.
        ⛔ Its gate must be re-derived from 09_fib's NEW baseline
        (increment 0 made it 20.7% faster), not from the old -4.75% row.
    4.  E3 - the two-entry inline cache. Serves 76. Last, because it is
        the only one adding a RUNTIME decision.

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

## 4. QUESTIONS FOR THE MAINTAINER

These are the decisions a successor session must NOT guess at. Each is
stated with the concrete alternatives.

**Q1 — 1b: BARRIER or PRECISE classification of the CALL family?**
A barrier (`v.mark_barrier(pc)`, IncDecChainV's precedent) is safe and
simple: pins may live in the run, and the call kills them across itself.
PRECISE additionally declares that the call reads the argument RUN and
the callee slot and writes the dst, which keeps more pins alive across
the call but must be exactly right.
*My recommendation:* start with the BARRIER and measure. If a barrier
does not make `lsra_pins` non-zero on `fib$0`, precision will not
either, and the increment stops there having cost one afternoon.

**Q2 — 1b: what is the ACCEPTANCE bar, given E1's result?**
Admitting pins into call-containing runs means spilling and reloading
them around EVERY call in the run. On a call-dense body such as `fib$0`
that could easily cost more than it saves. §3b's stated gate is "the
suite geomean must not regress", which is a floor, not a target.
*The question:* is 1b worth landing if it is geomean-neutral but makes
`fib$0` pin (i.e. it is a PLATFORM for increment 2 rather than a win on
its own)? Or should it be judged on its own wall clock, and dropped if
flat?

**Q3 — ORDERING: 1b before 1c, or 1c first?**
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

**Q5 — the local commits.** Four commits (`1767291`, `0ad4384`,
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
