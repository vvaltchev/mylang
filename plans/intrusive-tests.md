# #107 - INTRUSIVE TESTS (`INT_TESTS`)

Status: P0 DONE (the build flag), design below for review. 2026-10-02.

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
- **P4 - VM state checker + object census.**
- **P5 - JIT preserving probe stub** + the per-op register map.
- **P6 - chunk hook + assembler**; build the three splice-gate tests.
- **P7 - per-site forcing sweep.**
- **CI:** a `int` job in `nets.yml` from P1 on (Debug + ASan, runs
  `int_run.py`); its sweeps sized to the lane's budget.

## 7. Questions for the maintainer

1. **Builtin namespace:** `int_*` names registered only in INT builds
   (recommended - a stray use in a normal build is then a plain
   "undefined name" compile error), or reuse `make_dev_builtin`'s
   reservation so the names exist everywhere?
2. **`.my` vs C++ balance:** recommended - observation and forcing from
   `.my` programs, state EDITING (chunk hook) from C++ only.
3. **Enumeration budget in CI:** tier 1 (single deviations) on every
   push; tiers 2 and 3 on `workflow_dispatch`. All three deterministic.

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
