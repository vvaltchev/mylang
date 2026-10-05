# tests/CLAUDE.md - working notes for the test tools

The human overview is tests/README.md (keep it human: plain prose, aligned
tables, no markup clutter). This file is the operational detail: rules,
traps, APIs. The root CLAUDE.md holds the project-wide testing rules
(the INSTRUMENTS list, THE VACUOUS-TEST TRAP, Testing an AST TRANSFORM,
the INT_TESTS section); read those first.

## Layout

- Every program in tests/ (and the test tools in scripts/) has NO
  extension, mode 755 and a shebang: people run `tests/nested_fuzz`, not
  `python3 tests/nested_fuzz.py` (maintainer-set, 2026-10-04). A new tool
  follows suit - `git add --chmod=+x` if the mode does not stick.
- Library code lives in tests/lib/ and keeps `.py` (Python cannot import
  an extensionless file): testrun.py (progress, resume), testjobs.py
  (core counts, the idle re-exec), intcov.py (`mylang -v`, the coverage
  universe). A tool reaches it with
  `sys.path.insert(0, os.path.join(<dir of __file__>, "lib"))`. Never
  import one tool from another: move the shared code into tests/lib.
- Data directories: functional/, int/ (and int/repl/), backtrace/ (the
  bt_oracle programs - renamed from bt_oracle/ because a file and a
  directory cannot share the tool's name).

## Rules (maintainer-set)

- **Hour-scale runs go to a MANUAL CI workflow** (workflow_dispatch:
  int-deep.yml, mutate.yml), never the local machine for hours, never
  per-push CI. Locally, check a long tool on a tiny input only
  (mutate --count 2, int_enum on one program). Dispatching is the
  maintainer's call unless he asked.
- **State the cost before starting anything multi-minute**, and pick the
  smallest run that answers the question.
- **Every tool that can run > 1 minute serves its progress** so
  `tests/testctl` lists it with a percentage. A new long tool uses one
  of: `Run` (independent ordered items, resumable), `Monitor` (phases or a
  loop), or tests/testmon beside a shell tool. No exceptions.
- **Every tool re-execs at idle priority** (`testjobs.ensure_idle()`
  first in main; shell tools exec through `tests/jobs run`) and sizes
  its pool with `testjobs.count()` / `tests/jobs count`.
- **Per-push CI stays bounded** (Nets ~18 min). A new per-push step goes
  into a job that is not the long pole, or gets its own parallel job.

## tests/run - the runner (after Tilck's run_all_tests)

- The CATALOG is `catalog()` at the top of tests/run: one `add(name,
  type, class, build, est_seconds, cmd, ...)` per test, `cmd(c)` reading
  the binary as `c.b` (never a hard-coded build), so the same test runs
  on any build as NAME@BUILD; `on(test, build, est)` puts such a
  combination in the catalog (the long run covers it). A new tool or a
  new configuration of one gets a line there, with an HONEST class (the
  run alone, build excluded): short <= 30 s, med <= 3 min, long <= 30
  min, manual = hours. short + med is the default run, and a test whose
  build is not `dbg` is `long` - building that lane is minutes on its
  own. test-times.json keeps the measured times; trust them over `est`.
- CI RUNS THE CATALOG (2026-10-04): every workflow builds its binary
  itself (CMake mostly - CI is where that build system is exercised,
  and the coverage floors were measured on those recipes) and runs
  `tests/run -o --logs test-logs --bin LANE=PATH NAME@LANE ...`. So a
  check CI needs is a catalog test, never a workflow shell step: a new
  per-push check means a catalog entry plus its name in the job's
  tests/run line. A CI build stands for a lane (Windows Debug for dbg,
  hence `opt unknown` in dbg's expect; both Linux Release lanes for
  rel-hard; `ship` and `lto0` are build-check lanes with no catalog
  test of their own).
- A test of a CONFIGURATION carries `guards=`: in-process checks run
  before the tool that raise when the binary is not in that
  configuration (guard_nolowmem, guard_spcheck, guard_rex). They replace
  the workflows' old vacuity-guard steps; a new env lever whose test
  could pass vacuously gets one.
- `uses=[lane]` adds a build the command reads (int-vdjcmp's reference);
  `exclusive=True` runs the test alone on its build - any tool that
  clears and reads gcov counters (int_run --gcov, norec_coverage,
  int_select): another run of the same binary would add to them.
  `shard=` turns --shard I/N into the tool's own option; `-- ARGS` go to
  the single selected test's tool (the Mutation workflow's inputs).
- The heartbeat line carries each running test's latest output line:
  in a CI log that is the only live view of a long tool.
- BUILDS are `LANES`: a make or cmake recipe plus an `expect` dict that
  the binary's `mylang -v` must match. Add the expectation that would
  catch the wrong binary (int_tests 1 for an INT lane, ...), never an
  empty one. Builds go to --build-root: default build-tests/, the
  maintainer's; Claude passes --build-root build-claude (or exports
  MYLANG_TEST_BUILD_ROOT=build-claude), since it builds only there.
- CASES: a test with `cases=` can list (`--list`) and run a subset
  (`--only REGEX`) through its tool; `CASES` maps the kind to the list
  command, the selection suffix and a FAILED-CASE PARSER that reads the
  tool's own failure lines (rt: `[ RUN  ]` + `[ FAIL ]`; corpus_diff:
  `DIFF [..] path`, `CRASH [..] path`, `REFUSED path`; int_run: `  FAIL
  path`; bt_oracle: `FAIL name`, `VACUOUS name:`). **Changing a tool's
  failure line breaks its reproduce line silently: update the parser in
  the same change.** A fuzzer's reproduce line comes from its printed
  seed (nested_fuzz: `seed=N`).
- --case regexes go through `rx_exact`, which escapes only what is
  special in all three dialects the tools speak (ECMAScript for -rt,
  Python, POSIX ERE for corpus_diff's grep -E).
- The runner never tests a binary it did not check: every build gets a
  `check:` step (in-process `check_build`) before its tests.

## The progress API (tests/lib/testrun.py)

    from testrun import Run, Monitor

    # independent ordered items: heartbeat, socket, stop/pause/jobs N,
    # resume by token; work(i, item) returns failure text or None
    r = Run("name", items, work, fingerprint(...), jobs,
            heartbeat=60, describe=str, phase="exploring")
    r.execute()

    # anything else: status only
    mon = Monitor("name", total=n, phase="units")
    with mon:
        mon.set_current(what); ...; mon.advance(failed=bad)
        mon.phase("census", total=m)       # restarts done at 0

`percent_fn=` overrides done/total (a time-budgeted shrink); `pid=` makes
a watcher (testmon) report the watched tool's pid. Status fields: name,
pid, phase, percent, done, total, elapsed_s, eta_s, eta, failures,
current (+ extras via set_extra). Sockets live in $XDG_RUNTIME_DIR/
mylang-tests; testctl unlinks stale ones.

**Kill a tool by an ANCHORED pattern** (`ps -eo pid,args | awk '$2 ==
"python3" && $3 == "tests/x.py"'`): `pkill -f x.py` also matches the
shell that runs the pkill (exit 144, the command dies with it).

## Where each tool runs in CI

Every job: its own build, then one `tests/run --bin` line naming these
catalog tests.

| workflow job             | build (lane)       | tests                     |
|--------------------------|--------------------|---------------------------|
| Linux build x4           | cmake (dbg, clang, | rt, myv_doc_check,        |
|                          | rel-hard x2)       | driver_checks @lane       |
| Linux recycle            | cmake (recycle)    | rt@recycle                |
| Linux release-smoke x2   | cmake (release,    | system_smoke @lane        |
|                          | ship)              |                           |
| Linux lto0 x2            | make + cmake       | rt@lto0, driver_checks@   |
|                          | (lto0, ship)       | lto0, system_smoke@ship   |
| Linux dbginfo x2         | make (dbg)         | rt                        |
| macOS                    | cmake (clang)      | rt@clang                  |
| Windows x2               | cmake MSVC (dbg,   | rt @lane                  |
|                          | rel-hard)          |                           |
| Coverage                 | cmake (cmake-gcov) | rt@cmake-gcov + codecov   |
| Nets differential        | cmake (dbg)        | corpus_diff, vdjcmp,      |
|                          |                    | bt_oracle, norec_enum,    |
|                          |                    | norec_sweep               |
| Nets levers-fuzz         | cmake (dbg)        | corpus_diff-levers,       |
|                          |                    | nested_fuzz               |
| Nets disasmcheck x3      | cmake (dbg)        | disasmcheck-matrix        |
|                          |                    | --shard I/3               |
| Nets nolowmem x2         | cmake (dbg, rna)   | rt-nolowmem, corpus_diff- |
|                          |                    | nolowmem, driver_checks   |
| Nets spcheck x2          | cmake (dbg, rna)   | rt-spcheck,               |
|                          |                    | corpus_diff-spcheck       |
| Nets int                 | cmake g++-14       | int-vdjcmp, int_run-gcov, |
|                          | (int-gcov, dbg)    | driver_checks@int-gcov    |
| Nets int-enum            | cmake (int-rel)    | int_enum-1, int_enum-2,   |
|                          |                    | disasmcheck-rex           |
| Nets myv-fuzz x2         | cmake (dbg, rna)   | myv_fuzz                  |
| Nets repl-fuzz           | cmake (recycle)    | repl_fuzz@recycle         |
| Nets coverage-gate       | cmake (cmake-gcov) | norec_coverage            |
| int-deep (on demand)     | cmake, make        | int_enum-3, int_select    |
| Mutation (on demand) x8  | its own            | mutate --shard I/8        |

The CI fuzzers take `--seed ${{github.run_id}}` (a re-run keeps it): a
fixed seed made them a regression corpus. A finding reproduces from the
printed seed; a .myv finding only from the saved image (it embeds its
source path).

## Reach is not checking

Coverage proves a line was REACHED, never that a wrong result there would
fail a test (`2*x` vs `2+x` agree at x = 2). So:

- never cut a fuzzer or a test on coverage alone - the evidence for a cut
  is mutation testing (mutate: the fuzzers run as the LAST stage, so
  "killed by fuzz" counts what no deterministic test noticed);
- a test must print or assert VALUES. The int_select shrink oracle is
  agreement with the tree-walker ON THE SHRUNK PROGRAM, so it accepts any
  deletion both engines agree on: unpinned, it shrank programs to no
  output; with print lines pinned, to `var acc = 0; print(acc)`. It now
  pins `print(`/`assert(` lines AND ends with `observe_globals` (print
  every top-level var). A hand-shrunk test needs the same care.
- a self-check counter (`g_jit_*`, int_hits) proves the PATH ran; the
  value check proves it was right. A test usually needs both.

## int_run units (tests/int)

- `# INT-ENGINES:` names from ENGINE_FLAGS (default tw nj vm nbi noopt),
  `+` joins (`nbi+nj`); engines of one config must print the same stdout,
  so include `tw` when the point is a value check.
- `# INT-CONFIGS:` `;`-separated env sets.
- tests/int/repl/NAME.session + NAME.expected: fed to `--repl` on stdin
  under a FRESH HOME (history file); stdout must equal NAME.expected,
  stderr be empty, rc 0. `--update-repl` rewrites the .expected files -
  READ the diff, it is the assertion. (Not .out: .gitignore drops *.out,
  which is how the first push lost all 24 of them.)
- VALUE CLASSES (src/intsites.h, ML_INT_VCLASSES): declared operation
  boundaries, counted at the operation (`vc <class> <hits>` lines in the
  MYLANG_INT_OUT census). A class no run exercises fails int_run; with
  --gcov each is a `vc:` element. 22_value_classes.my asserts every one
  against README.md - extend it with each new class, and give the
  README the rule first if it does not state the boundary.
- With --gcov every unit must OWN an element no other unit covers, and
  uncovered must stay <= tests/int/coverage-floor.txt for the compiler.
  Lower the floor when a change covers more (the note says by how much);
  never raise it. CI's gcc 14.2 number comes from the `int` job log.
- nested_fuzz's generator has a WORK BUDGET (WORK_BUDGET: the product of
  the enclosing loops' iteration counts; past it a level nests an `if`).
  Without it a depth-15 program ran 1.7 million innermost iterations and
  the debug tree-walker timed out on it (26 s alone, CPython 0.5 s) -
  with fresh seeds in CI that would be a red run with no bug behind it.
- 16-21_nested_*.my are GENERATED (nested_fuzz -> int_select --shrink ->
  observe_globals -> wrapped to 80 columns, output checked identical).
  Do not tidy them by hand; regenerate.

## int_select

- Coverage collection runs in worker PROCESSES (forkserver): gcov JSON
  parsing is pure Python and the GIL made the threaded pass ~10x slower.
  Any new coverage tool: same rule.
- Default configs = 5 engine configs + nested_fuzz's four (-nbi, -nbi -nj,
  --no-opt all, -tw --no-opt all); a config word with no `=` is the
  previous flag's argument.
- `--shrink` takes a selected test name OR a program path (kept for every
  config it was selected under; several shrink in parallel).
  `--shrink-budget S` bounds each; the best so far is on disk throughout.
  Line-level ddmin on brace-structured code is slow (most deletions break
  the parse): ~1000 trials for a 450-line program, and a trial can loop
  forever (30 s leash on its reference run).

## mutate

- Mutates product .cpp only; skips ML_INT spans, INT-COV-EXEMPT lines,
  int_*/jit_int_*/bc_int_* bodies, #ifdef TESTS / INT_TESTS regions.
- Works in a private COPY of the tree per worker (a mutation is a
  sabotage; never touch the checkout). Build: the INT fast lane.
- The pristine build must pass every stage first, and sets each stage's
  timeout (5x, min 60 s). A timeout is a kill.
- Stages: rt, corpus, int, enum, fuzz, in that order, stop at first kill.

## Adding a test - where

| test of                               | goes in                      |
|---------------------------------------|------------------------------|
| language semantics, an error, a caret | src/tests.cpp (-rt, 5 modes) |
| a JIT/VM shape on every engine/lever  | tests/functional/*.my        |
| a backtrace across inlining           | tests/backtrace/*.my         |
| a compiler decision via INT hooks     | tests/int/NN_*.my            |
| the REPL front end                    | tests/int/repl/*.session     |
| a CLI flag                            | tests/driver_checks          |

Then: watch it fail against a sabotaged build (commit first, sabotage a
COPY, rebuild inside the restore - root CLAUDE.md), and if it is a new
tool, give it progress and a line in README.md and in the tables above.
