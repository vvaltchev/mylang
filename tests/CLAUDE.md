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
  type, class, build, est_seconds, cmd, ...)` per test. A new tool or a
  new configuration of one gets a line there, with an HONEST class (the
  run alone, build excluded): short <= 30 s, med <= 3 min, long <= 30
  min, manual = hours. short + med is the default run, and a test whose
  build is not `dbg` is `long` - building that lane is minutes on its
  own. test-times.json keeps the measured times; trust them over `est`.
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

| tool             | per push (job)                    | on demand       |
|------------------|-----------------------------------|-----------------|
| -rt              | Linux x7, macOS, Windows,         |                 |
|                  | Coverage, nolowmem x2, spcheck x2 |                 |
| corpus_diff      | differential, differential-levers |                 |
|                  | -fuzz, nolowmem, spcheck          |                 |
| bt_oracle        | differential                      |                 |
| norec_enum/sweep | differential                      |                 |
| vdjcmp           | differential                      |                 |
| disasmcheck      | disasmcheck x3 (--shard I/3)      |                 |
| nested_fuzz      | differential-levers-fuzz          |                 |
| myv_fuzz         | myv-fuzz (debug-asan, release)    |                 |
| repl_fuzz        | repl-fuzz (RECYCLE + ASan)        |                 |
| int_run          | int (--gcov --require-floor)      |                 |
| int_enum         | int-enum (tiers 1, 2)             | int-deep tier 3 |
| int_select       |                                   | int-deep select |
| norec_coverage   | coverage-gate                     |                 |
| driver_checks    | Linux, lto0, nolowmem             |                 |
| system_smoke     | Linux release-smoke               |                 |
| myv_doc_check    | Linux                             |                 |
| mutate           |                                   | Mutation        |

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
