# #107 - INTRUSIVE TESTS (`INT_TESTS`)

Status: P0 DONE (the build flag). P1 CORE DONE 2026-10-02: `inttest.h`
(ML_INT / ML_INT_FIELD / ML_INT_ONLY), `intsites.h` (the X-macro
registry, typed payloads), `inttest.cpp` (the log + the MYLANG_INT_OUT
exit census), `int_hits` / `int_events`, the first site (`inline_ast`,
all three AST inline engines) and its test, `tests/int_run.py`, the `int`
CI job with the non-perturbation `vdjcmp`. P1 COVERAGE DONE the same day:
`GCOV=1` in the Makefile, MC/DC wherever the compiler has it,
`int_run.py --gcov` (per-unit vectors, ownership, `INT-COV-EXEMPT`, the
per-compiler floor ratchet in `tests/int/coverage-floor.txt`), sites
counted only when CHECKED. First measurement (gcc 16.2): 144,746
elements, 45,055 uncovered; `-rt` owns 77,561, `01_inline_ast` 270.

P2 PART 1 DONE 2026-10-02: sites `splice` (every verdict, by reason),
`ast_transform` (licm / slice_hoist / for_range, fired - plus asserted
refusals), `pin` (each variable held in a register, by source name, from
the final emission), `call_tier` (frameless / push / native_direct);
`IntDefer` (a discarded JIT attempt records nothing; duplicates within one
attempt collapse); `int_events` sorted (a pointer-keyed walk made program
order nondeterministic); runner headers INT-ENGINES / INT-CONFIGS; the
universe narrowed to the PRODUCT (tests.cpp, the INT core and `*int_*`
helpers excluded): 114,495 elements, 32,433 uncovered (gcc 16.2).
**Findings so far (the first FIXED 2026-10-02):** a direct call with a
typed parameter never splices
(`typed_params`) while the same callee through a value call does; a value
call inside a template INSTANCE gets no callee set (only the base's site
is analysed, as top).
P2 PART 2 DONE the same day: `forward` (lever A, at `emit_fwd_bump`, the
one consumer path) and `guard_elided` (C5 store tests at the three
`store_dst*` points, C4d member guards), with `06_forward_guards`; every
new site's test watched failing.

P2 DONE (part 3, 2026-10-02): `IntWatch` (tests.cpp) - the "ran HERE"
half of a counter check. An emitted-code counter proves a tier EXECUTED,
which no log can, but only somewhere in the program; an IntWatch asks
whether a site recorded a matching event since it was made, so a test
names the function and variable it is about. Outside INT both answers
are true, so every test keeps its counter checks and gains the site
check in the INT lane. Applied to one representative per site:
`jit_temp_regs` (a temp OF g pinned with the lever, none without),
`jit_regcall_pins_caller_saved` (g's parameters in rdi/rcx, the protocol
registers), `jit_release_c5` (a store guard of main elided),
`jit_fwd_deadtemp` (forwards IN f, per case), `jit_frameless_gate` (the
call TO leaf emitted frameless - the test had added a builtin call purely
to keep the DRIVER out of a global chunk count). Not migrated: tests whose
counter is already per-program (the splice parity), where a site check
adds nothing. Parameters now print by name (the descriptor's), not
`r<slot>`. GCC 14 floor measured on CI: 32,256.

## 0. What already exists (P0)

- `make INT_TESTS=1` / CMake `-DINT_TESTS=ON`, default 0. Implies
  `TESTS`; `defs.h` refuses a bare `-DINT_TESTS`.
- `mylang -v` prints `int_tests 0/1` in EVERY build.
- `bench/run.py` refuses a binary reporting `int_tests 1` (and so does
  `tune_scales.py`, which runs the same gate).
- `driver_checks.sh` pins the `-v` line; `MYLANG_EXPECT_INT_TESTS=1`
  names an INT lane.

Nothing is instrumented yet. Everything below is the proposal.

## 1. The problem: what our tests CANNOT see today

Every net we have observes a program from the OUTSIDE: its output, its
exception, its exit code, plus a few counters and the `-vd`/`-vdj` text.
CLAUDE.md records, over and over, the same five blind spots. They are
the requirements of this design.

**B1. A speed-only optimization has no correctness oracle.** Forwarding
whitelists, guard elisions, the register give-back (+45% Ir, every net
green), the stale shift rows: the answer is right whichever path ran,
so the differential cannot fail. Whole-lever counters say "ran at all",
never "ran HERE".

**B2. A wrong value can be LAUNDERED before a program can look at it.**
A raw slice copy left in a temp is re-copied properly by the next
`MoveV`; a borrowed flag set on an int is never read; a stale pinned
register is flushed before anything prints. The defect is real and no
`.my` source can expose it.

**B3. The optimizer eats the test's shape.** Nine shape-eaters are
listed in *THE VACUOUS-TEST TRAP*; each was found by a reintroduced
defect that the test failed to catch. Three splice gates (in-flight §2)
guard shapes our codegen NEVER emits, so no `.my` program can reach
them at all.

**B4. A rare path is reached only by coincidence.** A cold arm, a
decline, the last register of a preference-ordered pool, a first vs a
warmed descent. `MYLANG_JIT_COLD`, `_FORCE`, `_XROT`, `_MAXPINS` and
`MYLANG_RECON_AT` each opened ONE such axis by hand.

**B5. A lifetime defect is invisible to the value and to LSan.** A
leaked reference to a pooled object, an alias that never copies, a
refcount one too high on a warmed path. `refcount(x)` exists for the
cases someone thought of.

## 2. Principles

1. **100% DETERMINISTIC.** MyLang is single-threaded with no timing
   input, so a compile and a run are a pure function of (source, build,
   flags). The instrumentation keeps that property: no sampling, no
   seeds, no "every Nth". Every op is checked; every decision space is
   walked in a FIXED order (4.2). Two sources of variation exist inside
   the system and the instrumentation must not inherit them: ADDRESSES
   (ASLR, pointer-keyed maps) and DICT ORDER (unordered by spec). So
   nothing it records or reports is keyed or ordered by a pointer - it
   uses source Locs, `node_id`, opcode pcs, op marks and interned
   names - and a ledger is printed in a canonical sort.
2. **INT MAY CHANGE MEMORY AND SPEED, AND NOTHING ELSE - INCLUDING NO
   DECISION.** Output, exceptions, carets, backtraces and exit code stay
   identical, and so does every INTERNAL decision: the register plan,
   the tier taken, the splice/inline/specialize choice, the bets, the
   emitted machine code (apart from the probes themselves). An INT
   build that decides differently tests a different program than the
   one that ships. Section 2b is how this is enforced and checked.
3. **Observe from INSIDE, assert from a test.** Instrumentation records
   facts where they are decided; a test (C++ in `-rt`, or a `.my`
   program) asserts on them.
4. **One registry of sites, and a site nobody exercises is a failure.**
   *A hook nobody calls is a hook that lies* (in-flight §2). Every site
   is declared once (`src/intsites.def`); the runner ends with a census
   and fails on any site no test reached.
5. **Compiled out completely without `INT_TESTS`.** Every abstraction
   in 2b expands to nothing (or to its default argument) when the flag
   is 0, so a shipping and a `TESTS=1` build are byte-identical to
   today. Checked by building both and comparing the binaries' `-vdj`
   over the corpus.
6. **Prefer an EXHAUSTIVE WALK to a hand-written case.** Where an axis
   can be enumerated (every guard site, every legal choice at every
   decision), the harness walks it over the corpus against the
   tree-walker, the way `norec_sweep.py` walks every call event.

## 2b. The abstractions, and how they stay out of the way

All of them live in `src/inttest.h` and expand to nothing - or to the
plain expression they wrap - when `INT_TESTS` is 0. The goal is that a
reader of `jit.cpp` sees a handful of one-line markers, not a second
program interleaved with the first.

| Abstraction | INT_TESTS=0 | INT_TESTS=1 |
|---|---|---|
| `ML_INT(site, fields...)` | nothing | records a typed event |
| `ML_INT_CHOICE(site, n, dflt)` | `dflt` | the enumerator's pick |
| `ML_INT_FORCE(site)` | `false` | true when a test forces it |
| `ML_INT_SCOPE(site, loc)` | nothing | RAII context push/pop |
| `ML_INT_FIELD(type, name)` | nothing | an extra struct member |
| `IntShadow<K>` side tables | absent | state the checker reads |

- **The site registry** (`intsites.def`, an X-macro list like
  `ML_FOR_EACH_OPCODE`): each row is `name, payload struct, what it
  means`. It generates the site enum, a TYPED payload struct per site
  (so `ML_INT(splice_value, .line = l, .declined = why)` is checked by
  the compiler), the census table, and the canonical printer.
- **Context without parameter threading.** Most sites need "which
  function, which pc, which source line". Rather than add parameters
  through call chains, `ML_INT_SCOPE` pushes that context onto a global
  stack (single-threaded, so no thread_local) that `ML_INT` reads.
  Extra parameters are used only where a scope cannot reach - mainly
  the emitter's per-op state - and then as `ML_INT_ARG(...)` trailing
  defaults that vanish when off.
- **Shadow state lives in SIDE TABLES, never in hot layouts.** `LValue`
  (48 bytes), `EvalValue`, `Frame` and the `JitProbe` structs are BAKED
  into emitted code and into the `.myv` format; growing them under INT
  would change the emitted code - a different program (principle 2).
  `ML_INT_FIELD` is for structs nothing emitted reads (`Chunk`,
  `FuncDescriptor`, the emitter's own state, the inferencer's records);
  everything else goes into `IntShadow` tables keyed by a stable id.
- **Emitted code: probes the emitter cannot see.** The emitter's
  decisions depend on its own accounting - the call seam's rsp model,
  `n_prologues` (the W6/SP3 "made no call" bets), the pin plan. A probe
  emitted through the ordinary `call_direct` would flip those bets and
  change the plan. So probes go through ONE raw path,
  `Emitter::int_probe(mark)`, that bypasses every counter and model:
  it emits `call <int_probe_stub>` and nothing else, and the STUB does
  all the work - save flags and every GP/XMM register, realign rsp
  itself (it cannot know the site's alignment, so it computes it), call
  the checker with the op mark and the saved register file, restore,
  return. The site loses nothing and learns nothing.
- **Proving non-perturbation, not asserting it.** Two oracles, both
  deterministic:
  1. *Decisions:* an INT build with probes OFF must emit byte-identical
     `-vdj` to a `TESTS=1` build of the same commit (`vdjcmp.sh`).
  2. *Probes:* with probes ON, the emitted code must equal the probes-
     off code with every probe call deleted. Compared at the
     INSTRUCTION level through the `DecodedIns` model - branch targets
     as instruction indices, not byte offsets, since inserting 5-byte
     calls moves every offset. Probe calls are recognised by their
     target, so the comparison needs no marker bytes.

## 3. The surface

Two front ends over one C++ core (`src/inttest.{h,cpp}`, the only new
TU, compiled empty without `INT_TESTS`):

- **C++ (`-rt`)**: `IntSite` ids, `int_on(site, fn)`, `int_force(site,
  arm)`, `int_ledger()`. For tests that need to EDIT internal state
  (the chunk hook, 4.4).
- **`.my` builtins**, `int_*`, registered with `make_dev_builtin` under
  `#ifdef INT_TESTS` (so a script outside the harness still refuses
  them): `int_decisions(site)`, `int_force(site, arm)`,
  `int_slot_state(name)`, `int_live(kind)`, `int_assert_ran(site)`.
  Programs live in `tests/int/*.my` and self-assert, like
  `tests/functional`.
- **Runner**: `tests/int_run.py BINARY` - refuses a binary whose `-v`
  says `int_tests 0`, runs `-rt` (which carries the C++ half), every
  `tests/int/*.my` under the default engine and its forced variants,
  then the sweeps, then the site census.

Macro at an instrumented point: `ML_INT(site, args...)` - expands to
nothing without `INT_TESTS`, to one predicted branch on a site-enabled
bit with it.

## 4. The instruments, one per blind spot

### 4.1 The DECISION LEDGER (B1, B3)

Every optimization decision point records `(site, source Loc, decision,
reason)`: inline/splice/specialize taken or declined and why, LICM
hoisted, for-range matched, a forwarding pair armed, a guard elided, a
register granted, a tier chosen at a call. The ledger is keyed by
SOURCE LOCATION, so a test says *"the call at line 7 was spliced"*,
*"the store at line 12 elided its ref guard"*, *"`t` at line 3 lived in
a register"* - not *"some counter moved"*.

What it buys:
- B1 becomes testable: an optimization that silently stops firing at a
  site fails a test that names the site. The shift-row and
  give-back regressions would each have been one failing assertion.
- B3 becomes VISIBLE: a test can assert that its shape REACHED the code
  under test (`int_assert_ran("splice.value", line)`) before asserting
  anything else, so a vacuous test fails as vacuous instead of passing.
  This replaces the reintroduce-the-defect step as the first line of
  defense (it stays as the second).

### 4.2 The DECISION ENUMERATOR (B4) - the strongest item

Every heuristic with more than one LEGAL answer asks through
`ML_INT_CHOICE(site, n_legal, default)` instead of deciding alone.
Candidates: which free register a pin takes (generalises `XROT`),
which slot spills, inline-or-not under the cost model, splice-or-not,
the pin budget (generalises `MAXPINS`), tier choice where two are
sound, unroll depth, literal-pool admission.

Correctness must not depend on any of these choices, so the harness
re-runs each corpus program under OTHER legal choices and requires
output identical to the tree-walker. No randomness: the walk is a
fixed enumeration, in three deterministic tiers, recorded by the
ledger so every run is named by its exact choice vector.

1. **Every SINGLE deviation.** For each decision instance the program
   actually reached (from the default run's ledger), run once with
   that one instance taking each other legal value. Complete for
   single-site bugs - the r9 bug, the give-back bug and the missing
   whitelist rows were all single-site.
2. **The full product, where it is small.** A program whose decision
   space is under a fixed bound is run under every combination.
3. **Every PAIR elsewhere,** through a fixed covering array (a
   deterministic construction, not a sample): every pair of values at
   every pair of instances appears in some run.

The full product is exponential, so tiers 1 and 3 do not claim
completeness beyond what they state; they are reproducible and their
claims are exact. A failing run names its choice vector, and the
harness reduces it to a minimal one by re-running subsets in a fixed
order.

**Decisions taken inside a decision.** A different register choice
can change which LATER decisions are reached at all. The enumeration
is therefore over decision INSTANCES as the ledger records them for
the run in hand (site + stable id + ordinal), and a deviation that
makes a later instance disappear simply has fewer instances.

### 4.3 The STATE CHECKER (B2, B5)

At op boundaries, verify the frame against what the compiler PROVED:

- every slot's type word agrees with its proven type (inference `th`,
  `proven_type`, the JIT's elided-tag set);
- a slot not in `ref_slots` holds no reference; a `borrowed` flag is
  only on a non-slice reference whose callee param is `noescape`;
- every live slice is registered in its parent's set and nothing
  dangling is;
- every pinned register's value equals what its slot WOULD hold
  (compare register to the shadow value, without writing either);
- refcount sanity: no count below the number of slots this frame holds.

The VM half is plain C++ in the dispatch loop's `VM_NEXT` under the
site bit. The JIT half is the design problem: the check must run
INSIDE emitted code without changing the register plan. Proposal: a
PRESERVING PROBE STUB - at a chosen op mark the emitter (only when the
probe is enabled at emit time) emits `call int_probe_stub` through the
call seam; the stub saves every GP and XMM register, calls the checker
with the op mark and a pointer to the saved file, restores everything.
The emitter already knows, per op mark, which slot lives in which
register (the LSRA snapshot/transitions); it publishes that map into a
per-fragment side table the checker reads. Nothing is flushed, so the
pins the checker sees are the ones the program runs with.

Every op is checked, in every INT run: one probe per op mark in every
fragment, one VM check per dispatched op. That is a large slowdown and
a large memory cost - both explicitly allowed - and it is the price of
the 100% claim. If a CI lane cannot afford the whole corpus at that
rate, the lane runs FEWER PROGRAMS, never fewer checks per program.

B5 extends it: an object CENSUS per kind (StructObject, SharedObject,
DictObject, StrObj, FuncObject, closure captures) counted at the pooled
allocator, readable as `int_live(kind)`; the runner asserts every
`tests/int` program and every corpus program ends with the census back
at its start. Pooled objects are exactly what LSan cannot see.

### 4.4 The CHUNK HOOK and a BYTECODE ASSEMBLER (B3)

For shapes codegen never emits:
- `int_on(Stage::post_codegen, fn)` / `post_splice` / `pre_jit` - a C++
  test gets the real compiled `Chunk` and may edit it before the next
  stage. This is exactly what the three splice gates in in-flight §2
  are waiting for (live staging temp, read-before-write of the returned
  slot, dst == callee slot): edit, run, assert the splice DECLINED and
  the program still prints what `-nbi` prints.
- An ASSEMBLER from `-vd`'s text to a `Chunk`, so a test can state a
  bytecode sequence directly. Its oracle is a round trip over the
  corpus: `assemble(disasm(chunk)) == chunk`, field for field, and
  `verify_chunk` runs on every assembled chunk. (Unlike re-parsing
  `-vdj`, this is not a second decoder of something we already decode:
  bytecode text is our own format with one writer.)

### 4.5 PER-SITE FORCING SWEEP (B4)

Every guarded tier's decline / cold arm gets a site id (today only
`refstore` and `guard` have a switch). `int_force(site, arm)` takes it
for ONE site; the sweep forces each site in turn over the corpus and
compares to the tree-walker - the per-call-site opt-in the
"DESIGNED and NOT BUILT" fourth JIT net asked for, without its
lane-time blowup because each run forces one site, not all.

### 4.6 COMPILE-PASS QUERIES (B1)

Structured reads of what the analyses concluded, for `.my` tests:
callee set (today only through `-dcs` text), `noescape` per param,
purity, `ref_slots`, `nonneg_slots`, the frameless facts, the pinned
set of a fragment. These turn several `-rt` text scrapes into
assertions on values.

## 5. How each recorded failure would have been caught

| Recorded failure (CLAUDE.md) | Instrument |
|---|---|
| shift ops missing from the forwarding whitelist | 4.1 ledger at the site |
| #123 register give-back, +45% Ir | 4.1 (pin not granted) + 4.2 |
| r9 unsafe in the pin pool | 4.2 register choice |
| raw slice copy laundered by `MoveV` | 4.3 slot check |
| catch-bind slot missing from `ref_slots` | 4.3 (ref in unlisted slot) |
| `t_bool` tag written from a garbage RCX | 4.3 type-word check |
| leaked reference on a warmed path | 4.3 census |
| three unreachable splice gates | 4.4 chunk hook |
| E1 live-descriptor set missing in one driver | 4.1 (decision never recorded) |
| tests vacuous by shape-eater (nine kinds) | 4.1 `int_assert_ran` |

## 6. Phases (each lands green, with its own watched-failing sabotage)

- **P1 - core + registry + census + runner.** `inttest.{h,cpp}`,
  `intsites.h`, `ML_INT`, the `int_*` builtin plumbing, `int_run.py`,
  the never-hit-site failure. One real site to prove the pipe.
- **P2 - ledger** at the inliner, splice, LICM, for-range, forwarding,
  guard elision, pin grant and call-tier sites; migrate the vacuity
  guards of existing shape tests onto `int_assert_ran`.
- **P3 - enumerator** for register choice, pin budget, inline/splice;
  the three-tier walk in the runner.
  **STATUS (2026-10-02): tier 1 over REGISTER CHOICE is built** -
  `int_choose` + the `reg_choice` site (RegAlloc::take/ftake),
  `tests/int_enum.py` (every single deviation, oracle = the
  tree-walker, a deviation must be TAKEN - `choose_applied`, written at
  once so a crashing run still names it) on the long-run harness
  `tests/testrun.py` / `tests/testctl.py` (heartbeat, a control socket,
  resume by a `<mark>@<fingerprint>` low-water-mark token). On an
  `OPT=1 ASSERTS=1 INT_TESTS=1` lane the whole of tests/functional
  (68,379 deviations) runs in ~40 s - 35x the debug+ASan lane, REGTRACK
  and the ML_CHECKs still live - so the planned coverage-chosen program
  subset was DROPPED: it would save seconds, and a default run's
  coverage does not measure what its deviations reach. First full run:
  94 failures, all fixed - two xmm0 writes outside the call bracket,
  the call-site bake's scratch taking the staged rdi, a capture base a
  pin conflict left live, and the float-stage class (see
  docs/jit-optimizations.md, 2026-10-01/02). Since then: the CI step
  (`int-enum`, tests/functional + samples/), the PIN BUDGET site (its
  budget-0 deviation found the rel8 flush hazard in Throw/Rethrow/
  EndFinally), per-RUN register keys, the SPLICE and AST INLINE sites
  (every single decline over the corpus is RULE-2 clean), and stderr in
  the oracle. 2026-10-02/03: the TYPED-callee bytecode inlining (the
  `typed_params` finding, fixed - it exposed a shipped wrong answer, a
  reassigned function's declared body inlined, and an entry-stub
  `flit_load` clobbering a REGCALL pin); then two more decision sites,
  `flit_choice` (the literal pool's cost gate - forcing it is
  FORCE=flit - and each literal it holds; watched: with the call
  epilogue's pool restore removed, the forced-gate deviations fail 4
  corpus programs and tests/int/10_flit.my's forced config) and
  `spill_choice` (the scan's pressure contest: any loser is legal;
  12 of 20 sampled deviations emit different code, all agree - no
  sabotage was found that ONLY a forced loser reaches, since breaking
  an eviction breaks the default evictions too). 2026-10-03:
  `unroll_choice`, the inliner's recursion unroll depth (any depth to
  REC_UNROLL_MAX is legal; tests/int/13_unroll.my forces fib to 0 and
  tribonacci to 3, every depth changes the tree, 21 corpus deviations
  all agree). Then the CALL TIER: `frameless_choice` - a site the JIT
  would make frameless takes the push (always legal), asked inside the
  one predicate the site, the fusion decision and the pre-passes share,
  so a forced decline is consistent; tests/int/14_frameless.my forces
  two sites, and the CI corpus gains 239 deviations, all agree (shown
  live and clean, like spill_choice: no sabotage found that only a
  forced push reaches). Then TIERS 2-3 (`int_enum.py --tier 2`): per
  SCOPE (keys sharing the prefix before the last `/` - one JIT run, one
  caller's inlining sites, all frameless sites) every 2+-deviation
  combination when there are at most 64, else a deterministic greedy
  covering array of every NON-default value pair (a default-side pair is
  a tier-1 run); a failing row is reduced by dropping overrides in key
  order while it still fails. Pairs ACROSS scopes are not claimed. CI
  corpus: 111,896 rows, 0 failures, ~5 min. Watched: with the call
  epilogue's float-pool restore removed, tier 1 fails 4 runs; tier 2
  fails 301 in 12 programs, 11 of which tier 1 passes, each reduced to
  2-4 decisions (01_float_chain_ref_temp: three float picks together).
  Not in CI (it would add ~5 min to the int-enum job).
- **P4 - VM state checker + object census.**
  **STATUS (2026-10-03): the OBJECT CENSUS is built** - per-kind
  counters at the pooled `operator new/delete`, `int_live(kind)`,
  `MYLANG_INT_CENSUS=1` (`census LEAK ...` at exit, checked after main's
  locals and before static destructors), `int_run.py` checking every
  tests/int run and the corpus under both engines (69 programs, 0
  leaks), `tests/int/12_census.my`. Its first run found one real
  leftover: a program's function chunks outlived it in the process-global
  chunk map (keyed by its freed descriptors, holding its constants) -
  `~VmProgram` erases them now. Watched: a frame release skipping
  closures fails 12_census, 02_splice and 14 corpus runs.
  **The VM STATE CHECKER, first increment (2026-10-03):** every op of
  every INT run (no switch - a never-taken branch at every dispatch
  would join the coverage universe) checks the frame: no reference in a
  slot outside `ref_slots`, a `borrowed` slot holds a non-slice
  reference. -rt (all five modes), the corpus x 3 engines and all 83,213
  enumerator deviations pass it. Watched: the #78 catch-bind gap
  reintroduced aborts at its pc.
  **Second increment, PROVEN TYPES (2026-10-03):** a typed or C3-proven
  parameter holds its scalar (or none when opt) at every op of its body,
  and the dst of the op just run holds the one kind that op writes (the
  specialized int/float family, IntBin/FloatBin, LoadImm*, StrLen,
  CmpIntV/CmpFloatV), read at the fall-through successor in the same
  frame. Skipped for a loaded image. -rt, int_run and all 83,064 CI-corpus
  deviations pass; watched: a float-param bind that does not widen, and
  CmpIntV writing an int, abort at their pc. `th` per instruction operand
  stays VM_HARDENING's read-side check.
  **Third increment, SLICES (2026-10-03):** a slice held in a slot is
  registered in its storage's set and its window lies inside the
  storage; the storage's `has_slices` mirror equals the set's emptiness
  and every registered view is a slice of it. Watched: copy-assign not
  registering (sl.my and four corpus programs abort at the op) and an
  overwrite not unregistering (a stale entry, caught at the next op).
  The enumerator's 83,064 deviations and int_run pass.
  **Fourth increment, REFCOUNTS (2026-10-03):** no object counts fewer
  handles than the owning (non-borrowed) slots of the frame holding it.
  Watched: a MoveV copying a reference without a retain aborts four
  corpus programs at the next op. With this the VM half of 4.3 is done
  except the pinned-register comparison (see P5).
- **P5 - JIT preserving probe stub** + the per-op register map.
  **STATUS (2026-10-03): BUILT.** `MYLANG_INT_PROBE=1`: a `call [abs32]`
  at every op boundary of every fragment (main stream, cold copies,
  containers) through a low-arena cell to `jit_int_probe_stub` (saves
  15 GPRs, flags, xmm0-15; aligns; restores bit for bit), whose checker
  finds the probe's record by return address - the chunk's ref_slots
  and parameter kinds, and a SKIP mask built from the emitter's view at
  that pc (pins, spill homes, elided types; plus ret_unflushed and W3
  poison at check time) - and runs the shared `int_slots_fault`. The
  red-zone audit found no emitted access below rsp (`load_rsp_disp`
  asserts disp >= 0; frameless windows are carved above rsp). The
  invisibility oracle is int_run's: probed -vdj minus probe lines with
  byte offsets masked == plain -vdj, over the corpus. 30.7M probes over
  the corpus x 3 engines, 0 findings. Watched: xmm3 clobbered by the
  stub (8 outputs change), an extra emitted byte (63 programs flagged),
  a slice borrowed by a frameless site (55_regcall aborts at the
  callee; invisible to the VM checker and to the outputs). The first
  run's one false positive changed a rule: a borrowed byte is checked
  on LISTED slots only (a frameless window leaves a scalar parameter's
  tail unwritten by design). NOT done: the pinned-register comparison
  (no shadow value exists); edge probes (P2/P5 in 9.7).
- **P6 - chunk hook + assembler**; build the three splice-gate tests.
- **P7 - per-site forcing sweep.**
- **CI:** a `int` job in `nets.yml` from P1 on (Debug + ASan, runs
  `int_run.py`); its sweeps sized to the lane's budget.

## 7. Decided (maintainer, 2026-10-02)

1. `int_*` builtins exist only in INT builds.
2. `.my` programs observe and force; only C++ tests edit internal state.
3. CI: enumeration tier 1 on every push, tiers 2-3 on dispatch.
4. **An INT binary never runs under the random-program fuzzers**
   (`nested_fuzz`, `myv_fuzz`, `repl_fuzz`). They keep running on the
   ordinary builds; INT runs only the suite of section 9.
5. **The INT suite is EXHAUSTIVE BUT MINIMAL** - section 9.

## 8. How hard

Honest sizing, hardest first:

- **The JIT probe stub and its invisibility (P5).** The emitter has
  several pieces of self-accounting (rsp model, prologue counts, bets,
  re-emission retries); the probe must bypass all of them, and the
  stub must preserve flags and the full register file and find its own
  alignment. And a `call` writes its return address just below `rsp`:
  any emitted code that keeps data there (a red zone) would be
  clobbered, so the first P5 step is an audit of that, with an
  `ML_CHECK` in the emitter that no probe lands where it would. The instruction-level probe-stripping comparison is what
  makes it safe to attempt: any accidental influence shows up as a
  diff in a deterministic check.
- **The enumerator's coverage of decision sites (P3).** Each heuristic
  has to be restated as "a default among N legal answers", which means
  finding where its legality is decided. The register allocator is the
  big one; the inliner and splice are simpler.
- **Not making a mess.** The abstractions in 2b keep each site to one
  line. The real discipline is the side-table rule: nothing goes into a
  layout emitted code or the `.myv` format reads.
- **Everything else** (registry, ledger, VM checker, census, chunk
  hook, assembler) is ordinary C++ against data structures we already
  have.

## 9. Exhaustive but minimal

Goal (maintainer, 2026-10-02): cover EVERYTHING, and run each piece of
code exactly as many times as covering it needs - SQLite's discipline
(100% branch coverage, MC/DC) applied to an interpreter that also
writes machine code. Determinism is what makes it possible: a test's
coverage is a FACT, identical on every run, so "this test adds nothing"
is decidable and stays true until the code changes.

### 9.1 "Everything" must be a finite, explicit list

The suite is measured against a COVERAGE UNIVERSE, the union of six
element kinds. Each element has a stable name.

| Kind | Element | Recorded by |
|---|---|---|
| C++ branch | each outcome of each branch in `src/` | gcov (`GCOV=1`) |
| C++ MC/DC | each condition of a compound `&&`/`||` shown to flip its decision on its own | gcov conditions (gcc >= 14 `-fcondition-coverage`) |
| Emitted edge | each conditional branch an EMITTER emits, both outcomes, keyed by (emitter site, edge) - NOT per fragment | INT edge probe |
| Decision alternative | each legal value at each `ML_INT_CHOICE` site | ledger |
| Value class | a declared boundary of an operation: shift count 63/64/-1, index -1/len/len+1, int min/max, empty/one-element container, ... | `ML_INT(value_class, ...)` at the operation |
| Configuration | each build/env axis that changes emitted code (low-mem arena on/off, native stack on/off) - an element only through the edges it changes | the config is part of a test's identity |

Two kinds are worth calling out.
- **Emitted edges:** gcov sees the C++ that EMITS a guard, not the
  guard. A guard emitted into 300 fragments is ONE element per outcome;
  one test that takes its cold arm covers it everywhere.
- **Value classes:** branch coverage cannot see an off-by-one - a shift
  by 63 and by 64 take the same C++ branch in a saturating
  implementation that is wrong at 64. The boundaries are DECLARED where
  the operation is defined, so they become countable elements and not
  "tests someone remembered".

**MC/DC's one tool limit.** GCC instruments at most 64 conditions per
decision and has no parameter to raise it. Five decisions in `vm.cpp`
exceed it (found 67 / 114 / 144 / 150 / 276 conditions, all inside the
dispatch function) and keep plain BRANCH coverage; the GCOV build still
prints the warning for each (non-fatal there), which is the list.

**Uncoverable by design** - an `ML_CHECK` failure arm, a `default:` over
a closed enum, an allocation failure. As in SQLite (`ALWAYS()` /
`NEVER()`), these are MARKED in the source (the `NOREC-COV-EXEMPT`
convention we already use, generalised to `INT-COV-EXEMPT: reason`), so
the universe excludes them by an explicit, reviewable claim. Where an
INT forcing switch can reach the arm (4.5), it is covered instead of
exempted - an exemption reached by forcing is reported STALE.

### 9.2 Exploration is not the suite

Exhaustiveness and minimality are reconciled by separating TWO
activities:

1. **Exploration (offline, at suite-construction time).** Generate
   CANDIDATES - every corpus program x every configuration x the
   enumerator's tier-1 deviations (tiers 2-3 when needed), hand-written
   shapes, and every bug reproducer ever found (including fuzzer
   findings, which are converted on a NORMAL build and enter only as
   programs). Run each candidate once under INT and record its coverage
   vector. This is where the expensive, exhaustive work happens, and it
   runs only when the code changes.
2. **The suite (what CI runs).** An IRREDUNDANT COVER chosen from the
   candidates: every reachable element is covered by at least one
   test, and removing any test would uncover something.

### 9.3 What "minimal" can honestly mean

- **Globally minimal** - the fewest tests that cover everything - is
  set cover, which is NP-hard. It is not claimed.
- **IRREDUNDANT** - no test can be dropped without losing an element -
  IS claimed, and it is checkable exactly: for each test, the elements
  only it covers (its OWNED set) must be non-empty. Greedy selection
  (largest new coverage first, ties broken by smaller cost, then by
  name, so the result is deterministic) followed by a removal pass
  gives this, and stays within a ln(n) factor of the true minimum.
- **Cost-minimal per test** - after selection, each test's program is
  shrunk by delta debugging while it keeps its owned elements, so a
  covering test is the smallest program that covers what it owns. A
  bench-sized loop never survives into the suite.

### 9.4 Every covering run is also an oracle run

Coverage without a check proves only that code RAN. Each suite test
is run with:
- the tree-walker as the output oracle (output, exception, caret,
  backtrace, exit code byte-identical);
- the state checker at EVERY op (4.3);
- the census back at its start (4.3);
- its expected ledger entries, where it owns a decision element.

So covering an element means checking it, once.

### 9.5 Keeping it minimal as the code changes

On a change, the suite is re-run (cheap - it is minimal) and its
coverage diffed against the universe:
- **newly uncovered elements** - new code, or a test that stopped
  reaching its owned set: exploration runs for THOSE elements only,
  searching the candidate pool for a covering run, and the change
  cannot land with an element uncovered and unexempted;
- **a test whose owned set became empty** - redundant now; it is
  dropped (reported, never silently);
- **an owned set that moved to another test** - recorded, so the
  ownership table stays the reviewable answer to "why does this test
  exist?".

CI enforces both directions on every push: no uncovered element, no
test that owns nothing.

### 9.6 What this does NOT replace (yet)

- The **random fuzzers** stay, on ordinary builds: they find programs
  nobody wrote, and a finding becomes a candidate.
- The existing **`-rt` suite, corpus_diff and the Nets jobs** stay
  until the INT suite's coverage provably SUBSUMES each of them - then
  the redundancy is a measured number per net, and retiring one is the
  maintainer's call.
- **Branch + MC/DC + value classes is not correctness.** It is the
  strongest MEASURABLE bar we know; data-dependent bugs on a covered
  branch are exactly what value classes exist to name, and the list
  grows when a bug escapes it - every escape adds an element first,
  then the test that owns it.

### 9.7 Phase changes

- **P1** gains the coverage universe: gcov + MC/DC in the INT lane, the
  `INT-COV-EXEMPT` marker, the ownership table, and the two CI checks.
- **P2 / P5** gain emitted-edge probes (P5 makes them invisible to the
  emitter, as for the state probes).
- A new **P8 - selection and reduction tooling** (`tests/int_select.py`:
  candidate runs, greedy + removal, delta-debugging shrink, the
  ownership table). It is offline tooling, not CI.
