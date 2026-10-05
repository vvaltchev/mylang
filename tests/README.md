MyLang tests
============

MyLang is tested in two ways.

The built-in unit suite lives inside the interpreter itself (src/tests.cpp).
You run it with "mylang -rt" on a build made with TESTS=1. It runs every test
in five execution modes, from the tree-walker to the JIT, and it is the
first thing to run after any change.

Everything else is in this directory: standalone programs that test what
the built-in suite cannot reach from the inside. They compare engines
against each other, feed the interpreter random or corrupted input, check
the command-line driver, force the compiler down unusual paths, and measure
coverage. None of them needs anything beyond Python 3 and a POSIX shell.

One command runs them all: tests/run.


Quick start
-----------

    $ tests/run

That builds what it needs (an incremental make of a debug build under
build-tests/dbg), runs the quick check - every short and medium test,
about four minutes on 16 cores - and prints one line per test:

    tests/run: 18 test(s), 1 build(s), 21 cores, seed 134701015
    logs: build-tests/test-logs/20261004-160952

    [ BUILT   ] build dbg                     0.3 s
    [ PASSED  ] cli: driver_checks           41.0 s
    [ PASSED  ] unit: rt                   2 min 33 s
    ...
    ------------------------------------------------------------------------
    Unit tests (-rt)           passed:   2/2
    Engine differentials       passed:   9/9
    Fuzzers                    passed:   3/3
    Driver, system and docs    passed:   2/2
    Machine-code checks        passed:   2/2

    PASSED: 18/18 tests passed in 3 min 54 s

A few more:

    $ tests/run -l                     # list what would run

    $ tests/run -n -t long             # every build and command it would run

    $ tests/run -t long                # everything but the hours-long tests

    $ tests/run rt corpus_diff         # just these tests

    $ tests/run -T fuzz                # one type of test

While it runs, from another terminal:

    $ tests/testctl


The test runner
---------------

A test is one run of one tool. It has a name (rt, corpus_diff-levers,
nested_fuzz, ...), a type, a time class and the build it runs on. See them
all with

    $ tests/run -L

Any test also runs on another build: NAME@BUILD. rt@clang is the unit
suite on the clang build, driver_checks@release the CLI checks on the
shipping one. The catalog lists the combinations the long run covers;
--builds lists every build.

| Type     | What it runs                                               |
|----------|------------------------------------------------------------|
| unit     | the built-in suite (-rt), on every build that has it       |
| diff     | the engines against each other (corpus_diff, bt_oracle...) |
| fuzz     | the fuzzers, with a fresh seed on every run                |
| int      | the intrusive tests (an INT_TESTS build)                   |
| cli      | the command line, a shipping build, the .myv spec          |
| jit      | the disassembler against objdump, -vdj reproducibility     |
| coverage | the coverage floors                                        |
| mutation | mutation testing                                           |

| Class  | Runs for         | Included                                 |
|--------|------------------|------------------------------------------|
| short  | up to 30 seconds | by default                               |
| med    | up to 3 minutes  | by default                               |
| long   | up to 30 minutes | with -t long (or -t any)                 |
| manual | hours            | only with -a; meant for the CI workflows |

Choosing what runs:

| Option       | Effect                                                |
|--------------|-------------------------------------------------------|
| NAME ...     | exactly these tests (any class except manual)         |
| NAME@BUILD   | a test on another build (--builds lists them)         |
| -t CLASS     | up to this class: short, med (the default), long, any |
| -T TYPE      | only these types, comma-separated; a prefix is enough |
| -f REGEX     | only tests whose name matches                         |
| -a           | also the manual tests                                 |
| -l, -L       | list instead of running (-L: every test)              |
| -n           | dry run: print every build, check and test command    |
| -d           | with -l: list each test's cases too                   |
| --case REGEX | run only the matching cases of the selected tests     |
| -o           | show each test's whole output, even when it passes    |
| -j N         | use N cores                                           |
| --seed N     | the fuzzers' seed, to repeat a run exactly            |
| --no-build   | build nothing; skip tests whose build is missing      |
| --bin B=PATH | use this binary as build B instead of making it       |
| --shard I/N  | run part I (from 0) of N of a test that can split     |
| -- ARGS      | pass ARGS to the one selected test's tool             |

Some tests have cases: each -rt entry, each corpus program, each tests/int
program and REPL session, each backtrace program. A test runs as one unit,
but its cases can be listed and run on their own:

    $ tests/run -l -d rt --case elem2

    $ tests/run rt --case '^elem2: '

When a test fails, the runner prints the end of its output, the log file,
and the command that reruns exactly what failed - the failing cases, or a
fuzzer's seed:

    [ FAILED  ] unit: rt                       2 min 31 s  exit 1
        ...
        log: build-tests/test-logs/latest/rt.log
        reproduce: tests/run rt --case '^beta: two$'
               or: build-tests/dbg/mylang -rt --only '^beta: two$'

Builds live under build-tests/ (--build-root or MYLANG_TEST_BUILD_ROOT
to change it). Before a test runs, its binary's "mylang -v" is checked
against the build's recipe, so a wrong --bin or a stale directory is
refused instead of tested. A test of a configuration also proves the
binary is in it first: the no-arena tests check that MYLANG_NO_LOWMEM=1
really refuses the arena, the alignment tests that the check is really
emitted. Otherwise such a test would pass while testing the default.

Logs go to build-tests/test-logs/, the newest run under latest/, and each
test's measured time is remembered in build-tests/test-times.json for the
next run's estimates.

The exit status is 0 when everything passed, 3 when a test failed, 2 when
a build failed, 4 when nothing matched, 1 for a bad option and 5 when
interrupted.


The test categories
-------------------

| Category             | What it answers                                |
|----------------------|------------------------------------------------|
| Unit suite           | Does each feature behave as specified?         |
| Engine differentials | Do the tree-walker, VM and JIT agree?          |
| Fuzzers              | Does random or broken input ever crash us?     |
| Intrusive tests      | Is every legal compiler decision also correct? |
| Driver and system    | Do the CLI and a real release build work?      |
| Machine-code checks  | Is the JIT's disassembly telling the truth?    |
| Coverage             | Which code has no test reaching it?            |
| Mutation testing     | Would a test notice if the code were wrong?    |

An "oracle" below is whatever a test compares against. Most tests here use
the tree-walker as the oracle: it is the simplest engine and has no JIT, so
when the fast engines disagree with it, the fast engines are wrong.


The tools
---------

Times are rough: "local" is a 16-core machine, "CI" a 4-core GitHub runner.
The build column says what the binary must be built with.

Engine differentials

| Tool        | Checks                         | Build | Local | CI    |
|-------------|--------------------------------|-------|-------|-------|
| corpus_diff | engines and JIT settings all   | any   | ~10 s | 2-7 m |
|             | match the tree-walker          |       |       |       |
| bt_oracle   | inlining never changes an      | any   | ~30 s | 1 m   |
|             | error's backtrace or caret     |       |       |       |
| norec_enum  | every program of a bounded     | any   | ~1 m  | 2 m   |
|             | shape agrees in 4 engines      |       |       |       |
| norec_sweep | a forced call-stack rebuild at | any   | ~2 m  | 3 m   |
|             | each call changes nothing      |       |       |       |

Fuzzers

| Tool        | Checks                        | Build | Local | CI    |
|-------------|-------------------------------|-------|-------|-------|
| nested_fuzz | random nested programs agree  | any   | ~1 m  | 8 m   |
|             | with CPython on the same code |       |       |       |
| myv_fuzz    | a damaged .myv image never    | any   | ~30 s | 1.5 m |
|             | crashes or hangs us           |       |       |       |
| repl_fuzz   | random REPL sessions never    | any   | ~15 s | 0.5 m |
|             | crash it                      |       |       |       |

Intrusive tests (need an INT_TESTS=1 build, see "Builds" below)

| Tool       | Checks                        | Build    | Local | CI     |
|------------|-------------------------------|----------|-------|--------|
| int_run    | tests/int programs, REPL      | INT      | ~6 m  | 8 m    |
|            | sessions, leak census, VM and | (+GCOV)  |       |        |
|            | JIT state checkers, and every |          |       |        |
|            | declared value boundary (a    |          |       |        |
|            | shift by 64, index -1, ...)   |          |       |        |
|            | exercised by some test        |          |       |        |
| int_enum   | forcing any legal compiler    | INT      | 1-6 m | 16 m   |
|            | decision changes no output    |          |       |        |
| int_select | which tests cover what;       | INT GCOV | ~6 m  | manual |
|            | shrinks a test to the lines   |          |       |        |
|            | that matter                   |          |       |        |

Driver, system, documentation

| Tool          | Checks                         | Build   | Local | CI  |
|---------------|--------------------------------|---------|-------|-----|
| driver_checks | the CLI flags do what they say | any     | ~20 s | 1 m |
|               | (-rt cannot see them)          |         |       |     |
| system_smoke  | a release build with no -rt    | TESTS=0 | ~1 m  | 2 m |
|               | suite runs scripts             |         |       |     |
| myv_doc_check | the .myv spec in docs/ matches | any     | ~1 s  | 1 s |
|               | every byte                     |         |       |     |

Machine code, coverage, mutation

| Tool           | Checks                        | Build   | Local | CI     |
|----------------|-------------------------------|---------|-------|--------|
| disasmcheck    | -vdj decodes each JIT         | TESTS=1 | ~10 m | 3x15 m |
| (in scripts/)  | instruction like objdump      |         |       |        |
| vdjcmp         | two binaries emit identical   | any     | ~30 s | 1 m    |
| (in scripts/)  | machine code                  |         |       |        |
| norec_coverage | no-record call tier keeps its | GCOV    | ~5 m  | 10 m   |
|                | coverage floor                |         |       |        |
| mutate         | planted bugs are caught by    | builds  | hours | manual |
|                | some test                     | its own |       |        |

Helpers (not tests themselves)

| Tool    | Does                                                          |
|---------|---------------------------------------------------------------|
| run     | the test runner: builds, runs and reports every test above    |
| testctl | shows the progress of every running test tool                 |
| testmon | progress for a shell tool, by counting its result files       |
| jobs    | how many cores to use, and at what priority                   |
| lib/    | the Python code the tools share: progress and resume          |
|         | (testrun.py), core counts (testjobs.py), coverage (intcov.py) |

Everything directly in tests/ is a program you can run; lib/ holds only
library code, and the other directories hold test data.


Running a tool directly
-----------------------

tests/run is the usual way in, but every tool also runs on its own. All of
them take the binary to test as an argument, so you can point any of them
at any build. They run at idle priority, so they never slow down the
machine for you.

Unit suite

    $ build-tests/dbg/mylang -rt

    $ build-tests/dbg/mylang -rt -s                 # dump a failing tree

    $ build-tests/dbg/mylang -rt --list             # every case's name

    $ build-tests/dbg/mylang -rt --only '^elem2: '  # only these cases

corpus_diff, int_run and bt_oracle take the same --list and --only REGEX.

Engine differentials

    $ tests/corpus_diff build-tests/dbg/mylang

    $ tests/corpus_diff build-tests/dbg/mylang --levers --cold --xrot

    $ tests/bt_oracle build-tests/dbg/mylang

    $ tests/norec_enum build-tests/dbg/mylang --depth 3

    $ tests/norec_sweep build-tests/dbg/mylang --max-events 25

corpus_diff modes can be combined in one run:

| Mode       | What it adds                                           |
|------------|--------------------------------------------------------|
| (none)     | tree-walker, VM and JIT, plus the -nc and -nti forms   |
| --levers   | a run per JIT optimization, each turned off in turn    |
| --cold     | a run per JIT fast path, each forced onto its fallback |
| --xrot     | a run per rotation of the register allocator's order   |
| --nolowmem | a run with the other type-tag encoding                 |
| --spcheck  | a run that checks stack alignment at every native call |

Fuzzers

    $ tests/nested_fuzz --mylang build-tests/dbg/mylang --count 250

    $ tests/myv_fuzz build-tests/dbg/mylang -n 400

    $ tests/repl_fuzz build-tests/dbg/mylang -n 400

Each fuzzer prints its seed first. To reproduce a failure, run it again
with that seed:

    $ tests/nested_fuzz --mylang build-tests/dbg/mylang --seed 1234

A .myv finding cannot be regenerated from a seed (an image contains its
source path), so myv_fuzz saves every crashing image instead; run the
saved file directly.

Intrusive tests

    $ make -j OPT=1 ASSERTS=1 LTO=0 TESTS=1 INT_TESTS=1 \
          BUILD_DIR=build-tests/int-rel

    $ tests/int_run build-tests/int-rel/mylang

    $ tests/int_enum build-tests/int-rel/mylang --tier 1 tests/functional/*.my

    $ tests/int_enum build-tests/int-rel/mylang --tier 2 tests/functional/*.my

With coverage, on a GCOV build:

    $ make -j OPT=0 TESTS=1 INT_TESTS=1 GCOV=1 BUILD_DIR=build-tests/int-gcov

    $ tests/int_run build-tests/int-gcov/mylang --gcov --require-floor

    $ tests/int_select build-tests/int-gcov/mylang --with-rt

Driver, system, documentation

    $ tests/driver_checks build-tests/dbg/mylang

    $ tests/system_smoke build-tests/release/mylang

    $ build-tests/dbg/mylang -c samples/gcd -o /tmp/gcd.myv
    $ tests/myv_doc_check /tmp/gcd.myv

Machine code

    $ scripts/disasmcheck build-tests/dbg/mylang --matrix

    $ scripts/vdjcmp build-old/mylang build-new/mylang

Builds
------

tests/run makes these on demand, each under build-tests/NAME:

| Build       | make (or cmake) options                     | Used by        |
|-------------|---------------------------------------------|----------------|
| dbg         | TESTS=1 OPT=0                               | the quick run  |
| clang       | CXX=clang++ TESTS=1 OPT=0                   | rt@clang       |
| rel-hard    | TESTS=1 OPT=1 VM_HARDENING=1                | rt@rel-hard    |
| release     | OPT=1                                       | system_smoke   |
| ship        | OPT=1 ASSERTS=0                             | CI only        |
| rna         | OPT=1 ASSERTS=0 TESTS=1                     | no-assert runs |
| lto0        | OPT=1 TESTS=1 LTO=0                         | CI only        |
| recycle     | TESTS=1 OPT=0 RECYCLE=1                     | rt, repl_fuzz  |
| nojit-gcc   | TESTS=1 OPT=0, JIT compiled out             | rt@nojit-gcc   |
| nojit-clang | CXX=clang++ TESTS=1 OPT=0, JIT compiled out | rt@nojit-clang |
| int-rel     | OPT=1 ASSERTS=1 LTO=0 TESTS=1 INT_TESTS=1   | int tests      |
| int-gcov    | OPT=0 TESTS=1 INT_TESTS=1 GCOV=1            | int_run-gcov   |
| cmake-gcov  | cmake -DTESTS=1 -DGCOV=1 -DVM_HARDENING=ON  | norec_coverage |

"CI only" builds have no test of their own in the catalog: CI builds them
to check the build itself, then runs tests on them with NAME@BUILD.

A debug build has the address and undefined-behaviour sanitizers on,
which is where most memory bugs show up first. "mylang -v" prints how a
binary was built. To build one by hand:

    $ make -j TESTS=1 OPT=0 BUILD_DIR=build-dbg

Never benchmark an INT or debug build: they are slow on purpose.


Watching progress
-----------------

Every tool that can run for more than a minute reports its progress. From
any terminal:

    $ tests/testctl

prints one row per running tool: its percentage, done/total, phase,
elapsed time, ETA, failures so far, and what it is working on now (NOW):

    TOOL           PID     %    DONE PHASE ELAPSED   ETA FAIL
    int_run     790151 42.2%   19/45 units   2m10s 2m58s    0
        tests/int/12_census.my under -nj
    corpus_diff 764141 74.7% 361/483 runs    0m03s 0m01s    0

Each column is as wide as its content, so the listing fits the terminal.
On a wide terminal NOW is the last column, its text wrapped beneath
itself; on a narrow one it moves to lines of its own, as above, and a very
narrow terminal drops PID, ELAPSED and DONE. On a terminal it is in color
(NO_COLOR=1 turns that off).

To keep it on screen, refreshed every two seconds:

    $ tests/testctl watch

Every command, and what RUN means, is in

    $ tests/testctl -h

The same numbers appear in each tool's own output as a heartbeat line
every minute, which is also what you see in a CI log.

Some tools (int_enum, int_select, mutate) can also be paused, stopped and
resumed:

    $ tests/testctl status int_enum

    $ tests/testctl stop int_enum

    $ tests/testctl jobs 4 int_enum

A stopped run prints a resume token; pass it back with --resume to carry
on where it stopped.


What CI runs
------------

CI builds its binaries itself, mostly with CMake (the one place that
build system gets exercised), and runs every test through tests/run:

    $ tests/run -o --logs test-logs --bin rna=build/mylang \
                rt-nolowmem@rna corpus_diff-nolowmem@rna driver_checks@rna

So a CI test is exactly the local one: the same command, the same checks
on the binary, the same reproduce lines. A failed job uploads its
test-logs/ directory, findings included.

On every push:

| Workflow | Runs                                                        |
|----------|-------------------------------------------------------------|
| Linux    | -rt on seven builds (debug and release, gcc and clang,      |
|          | adversarial allocator, non-LTO, debug info), driver_checks, |
|          | myv_doc_check, system_smoke                                 |
| macOS    | -rt                                                         |
| Windows  | -rt                                                         |
| Coverage | -rt with coverage, uploaded                                 |
| Nets     | corpus_diff (plain, --levers, --nolowmem, --spcheck), -rt   |
|          | with the arena refused and with the alignment check, on a   |
|          | debug and a no-assert build, bt_oracle, norec_enum,         |
|          | norec_sweep, vdjcmp, disasmcheck, the three fuzzers,        |
|          | int_run, int_enum tiers 1 and 2, and the coverage gates     |

Nets takes about 18 minutes; the others finish in under 10. The fuzzers
use a new seed on every run, so each push tries new programs.

On demand only (Actions tab, "Run workflow"):

| Workflow | Runs                                        | Takes     |
|----------|---------------------------------------------|-----------|
| INT deep | int_enum tier 3, int_select over everything | ~1 hour   |
| Mutation | mutate over 200 planted bugs, in 8 jobs     | 1-3 hours |

Run these after a series of complex changes rather than on every push.


Adding a test
-------------

| You want to test                     | Put it in                     |
|--------------------------------------|-------------------------------|
| a language feature or an error       | src/tests.cpp (the -rt suite) |
| a JIT or VM shape, on every engine   | tests/functional/NAME.my      |
| a backtrace or caret                 | tests/backtrace/NAME.my       |
| a compiler decision, under INT hooks | tests/int/NN_NAME.my          |
| a REPL session                       | tests/int/repl/NAME.session   |
| a command-line flag                  | tests/driver_checks           |

A new tool also gets a line in tests/run's catalog (the catalog() function
at the top of tests/run): its name, type, class, build and command.

A functional test is a small program that checks its own results and
builds the tricky shape on purpose. corpus_diff then runs it on every
engine and every JIT setting automatically.

A REPL session is the text you would type, one input per line, ending with
:quit. Its expected output lives next to it in NAME.expected; write it with

    $ tests/int_run build-tests/int-rel/mylang --update-repl

and read it before committing: that file is the assertion.

Before trusting a new test, break the code it tests on purpose and watch
the test fail. A test that passes either way checks nothing.


About the fuzzers
-----------------

nested_fuzz writes thousands of random, deeply nested programs (loops
inside conditions inside loops, with arrays, dicts, break and continue)
together with the same program in Python, and requires every MyLang engine
and CPython to print the same result. CPython is an independent oracle: it
also catches a bug where all MyLang engines agree on the same wrong
answer. The programs stay inside the subset where MyLang and Python are
meant to agree (no negative modulo, no overflow, no dict iteration order),
so any difference is a real bug.

myv_fuzz takes two compiled images and damages them in five ways
(flipped bits, random bytes, truncation, random bursts, 0xFF words). A
damaged image may be refused or may even run, but it must never crash or
hang the interpreter.

repl_fuzz feeds the interactive REPL random sessions built from
fragments: declarations, redefinitions, unfinished input, meta-commands
with nonsense arguments. The REPL must survive every one.

Fuzzers find what nobody thought to write a test for. When one finds
something, the program that found it becomes a fixed test, so the bug
stays caught.
