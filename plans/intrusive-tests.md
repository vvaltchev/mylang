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

1. **Observe from INSIDE, assert from a test.** Instrumentation records
   facts at the place they are decided; a test (C++ in `-rt`, or a
   `.my` program) asserts on them.
2. **One registry of sites, and a site nobody exercises is a failure.**
   *A hook nobody calls is a hook that lies* (in-flight §2). Every site
   is declared in ONE X-macro list (`src/intsites.h`); the INT runner
   ends with a census and fails on any site no test reached - the
   `opcode_table_census` ratchet applied to the hooks themselves.
3. **Instrumentation must not perturb what it measures.** A probe that
   flushes registers to inspect them tests a different register plan.
   Section 4.3 is the hard part of this design for that reason.
4. **Compiled out completely without `INT_TESTS`**, and INERT at
   runtime until a test turns it on. Oracle for both: an INT build with
   every probe off must emit BYTE-IDENTICAL `-vdj` to a `TESTS=1` build
   of the same commit (`scripts/vdjcmp.sh`), and `-rt` must agree.
5. **Prefer a SWEEP to a hand-written case.** Where an axis can be
   enumerated (every guard site, every legal register choice), the
   harness walks all of it over the corpus against the tree-walker, the
   way `norec_sweep.py` walks every call event.

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

### 4.2 The DECISION PERTURBER (B4) - the strongest item

Every heuristic with more than one LEGAL answer asks the core instead
of deciding alone: `int_choose(site, n_legal, default)`. Without
`INT_TESTS` it is `default`. With it, a seeded perturber may return any
legal index. Candidates: which free register a pin takes (generalises
`XROT`), which slot spills, inline-or-not under the cost model,
splice-or-not, pin budget (generalises `MAXPINS`), tier choice where
two are sound, unroll depth, literal-pool admission.

Correctness must not depend on any of these, so the harness runs the
corpus x N seeds and requires output identical to the tree-walker.
This is `MYLANG_JIT_FORCE`'s "test the gate independently of its
profitability" for EVERY gate at once, and it is the general answer to
*"a pool ordered by preference hides its own tail"*. A failing seed is
reproducible from `(program, seed)`; the ledger names which choices it
made, so the harness can bisect to the one choice that breaks.

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

Cost: one call per checked op, so checking is SAMPLED - every op in a
`tests/int` program, every Nth op in a corpus sweep. Its own oracle:
a checked run must print what an unchecked run prints, and probes off
must emit byte-identical code (principle 4).

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
- **P3 - perturber** for register choice, pin budget, inline/splice;
  the seed sweep in the runner.
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
3. **Perturber scope in CI:** seeds x corpus grows fast. Recommended:
   a fixed small seed set per push, a large one on `workflow_dispatch`.
4. **Probe-stub sampling rate** for the corpus sweep - to be set from a
   measured lane time in P5, not guessed now.
