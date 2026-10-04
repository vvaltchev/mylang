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


Quick start
-----------

Build a debug binary with the suite compiled in (sanitizers are on by
default in debug builds):

    make -j TESTS=1 OPT=0 BUILD_DIR=build-dbg

Then, from the fastest check to the slowest:

    build-dbg/mylang -rt

    tests/corpus_diff.sh build-dbg/mylang

    tests/driver_checks.sh build-dbg/mylang

    python3 tests/nested_fuzz.py --mylang build-dbg/mylang --count 200

To run everything CI runs, on every build lane, in one go:

    python3 tests/run_battery.py

While any of these is running, see how far along it is from another
terminal:

    python3 tests/testctl.py


The test categories
-------------------

| Category                | What it answers                                  |
|-------------------------|--------------------------------------------------|
| Unit suite              | Does each feature behave as specified?           |
| Engine differentials    | Do the tree-walker, VM and JIT agree?            |
| Fuzzers                 | Does random or broken input ever crash us?       |
| Intrusive tests         | Is every legal compiler decision also correct?   |
| Driver and system       | Do the CLI and a real release build work?        |
| Machine-code checks     | Is the JIT's disassembly telling the truth?      |
| Coverage                | Which code has no test reaching it?              |
| Mutation testing        | Would a test notice if the code were wrong?      |

An "oracle" below is whatever a test compares against. Most tests here use
the tree-walker as the oracle: it is the simplest engine and has no JIT, so
when the fast engines disagree with it, the fast engines are wrong.


The tools
---------

Times are rough: "local" is a 16-core machine, "CI" a 4-core GitHub runner.
The build column says what the binary must be built with.

Engine differentials

| Tool              | Checks                     | Build    | Local | CI     |
|-------------------|----------------------------|----------|-------|--------|
| corpus_diff.sh    | engines and JIT settings   | any      | ~10 s | 2-7 m  |
|                   | all match the tree-walker  |          |       |        |
| bt_oracle.py      | inlining never changes an  | any      | ~30 s | 1 m    |
|                   | error's backtrace or caret |          |       |        |
| norec_enum.py     | every program of a bounded | any      | ~1 m  | 2 m    |
|                   | shape agrees in 4 engines  |          |       |        |
| norec_sweep.py    | a forced call-stack        | any      | ~2 m  | 3 m    |
|                   | rebuild at every call      |          |       |        |
|                   | event changes nothing      |          |       |        |

Fuzzers

| Tool              | Checks                     | Build    | Local | CI     |
|-------------------|----------------------------|----------|-------|--------|
| nested_fuzz.py    | random nested programs     | any      | ~1 m  | 8 m    |
|                   | agree with CPython on the  |          |       |        |
|                   | same code                  |          |       |        |
| myv_fuzz.py       | a damaged .myv image never | any      | ~30 s | 1.5 m  |
|                   | crashes or hangs us        |          |       |        |
| repl_fuzz.py      | random REPL sessions never | any      | ~15 s | 0.5 m  |
|                   | crash it                   |          |       |        |

Intrusive tests (need an INT_TESTS=1 build, see "Builds" below)

| Tool              | Checks                     | Build    | Local | CI     |
|-------------------|----------------------------|----------|-------|--------|
| int_run.py        | tests/int programs, REPL   | INT      | ~6 m  | 8 m    |
|                   | sessions, leak census, VM  | (+GCOV)  |       |        |
|                   | and JIT state checkers     |          |       |        |
| int_enum.py       | forcing any legal compiler | INT      | 1-6 m | 16 m   |
|                   | decision changes no output |          |       |        |
| int_select.py     | which tests cover what;    | INT GCOV | ~6 m  | manual |
|                   | shrinks a test to the      |          |       |        |
|                   | lines that matter          |          |       |        |

Driver, system, documentation

| Tool              | Checks                     | Build    | Local | CI     |
|-------------------|----------------------------|----------|-------|--------|
| driver_checks.sh  | the CLI flags do what they | any      | ~20 s | 1 m    |
|                   | say (-rt cannot see them)  |          |       |        |
| system_smoke.py   | a release build with no    | TESTS=0  | ~1 m  | 2 m    |
|                   | -rt suite runs scripts     |          |       |        |
| myv_doc_check.py  | the .myv spec in docs/     | any      | ~1 s  | 1 s    |
|                   | matches every byte         |          |       |        |

Machine code, coverage, mutation

| Tool              | Checks                     | Build    | Local | CI     |
|-------------------|----------------------------|----------|-------|--------|
| disasmcheck.py    | -vdj decodes each JIT      | TESTS=1  | ~10 m | 3x15 m |
| (in scripts/)     | instruction like objdump   |          |       |        |
| vdjcmp.sh         | two binaries emit          | any      | ~30 s | 1 m    |
| (in scripts/)     | identical machine code     |          |       |        |
| norec_coverage.py | no-record call tier keeps  | GCOV     | ~5 m  | 10 m   |
|                   | its coverage floor         |          |       |        |
| mutate.py         | planted bugs are caught by | builds   | hours | manual |
|                   | some test                  | its own  |       |        |

Helpers (not tests themselves)

| Tool           | Does                                                    |
|----------------|---------------------------------------------------------|
| run_battery.py | builds every lane and runs the whole set in parallel    |
| testctl.py     | shows the progress of every running test tool           |
| testrun.py     | the progress and resume library the tools share         |
| testmon.py     | progress for a shell tool, by counting its result files |
| jobs.sh        | how many cores to use, and at what priority             |
| testjobs.py    | the Python face of jobs.sh                              |


How to run them
---------------

All tools take the binary to test as an argument, so you can point any of
them at any build. They run at idle priority, so they never slow down the
machine for you.

Unit suite

    build-dbg/mylang -rt

    build-dbg/mylang -rt -s            (dump the tree of a failing test)

Engine differentials

    tests/corpus_diff.sh build-dbg/mylang

    tests/corpus_diff.sh build-dbg/mylang --levers --cold --xrot

    python3 tests/bt_oracle.py build-dbg/mylang

    python3 tests/norec_enum.py build-dbg/mylang --depth 3

    python3 tests/norec_sweep.py build-dbg/mylang --max-events 25

corpus_diff.sh modes can be combined in one run:

| Mode        | What it adds                                             |
|-------------|----------------------------------------------------------|
| (none)      | tree-walker, VM and JIT, plus the -nc and -nti forms     |
| --levers    | a run per JIT optimization, each turned off in turn      |
| --cold      | a run per JIT fast path, each forced onto its fallback   |
| --xrot      | a run per rotation of the register allocator's order     |
| --nolowmem  | a run with the other type-tag encoding                   |
| --spcheck   | a run that checks stack alignment at every native call   |

Fuzzers

    python3 tests/nested_fuzz.py --mylang build-dbg/mylang --count 250

    python3 tests/myv_fuzz.py build-dbg/mylang -n 400

    python3 tests/repl_fuzz.py build-dbg/mylang -n 400

Each fuzzer prints its seed first. To reproduce a failure, run it again
with that seed:

    python3 tests/nested_fuzz.py --mylang build-dbg/mylang --seed 1234

A .myv finding cannot be regenerated from a seed (an image contains its
source path), so myv_fuzz.py saves every crashing image instead; run the
saved file directly.

Intrusive tests

    make -j OPT=1 ASSERTS=1 LTO=0 TESTS=1 INT_TESTS=1 BUILD_DIR=build-int

    python3 tests/int_run.py build-int/mylang

    python3 tests/int_enum.py build-int/mylang --tier 1 tests/functional/*.my

    python3 tests/int_enum.py build-int/mylang --tier 2 tests/functional/*.my

With coverage, on a GCOV build:

    make -j OPT=0 TESTS=1 INT_TESTS=1 GCOV=1 BUILD_DIR=build-gcov

    python3 tests/int_run.py build-gcov/mylang --gcov --require-floor

    python3 tests/int_select.py build-gcov/mylang --with-rt

Driver, system, documentation

    tests/driver_checks.sh build-dbg/mylang

    python3 tests/system_smoke.py build-rel/mylang

    build-dbg/mylang -c samples/gcd -o /tmp/gcd.myv
    python3 tests/myv_doc_check.py /tmp/gcd.myv

Machine code

    python3 scripts/disasmcheck.py build-dbg/mylang --matrix

    scripts/vdjcmp.sh build-old/mylang build-new/mylang

Everything at once

    python3 tests/run_battery.py

    python3 tests/run_battery.py --no-build

    python3 tests/run_battery.py --dry-run

The battery builds its own lanes under build-claude/ and writes one log
per step; the summary at the end names the logs of anything that failed.


Builds
------

| Build      | Command                                                     |
|------------|-------------------------------------------------------------|
| debug      | make -j TESTS=1 OPT=0                                       |
| release    | make -j OPT=1                                               |
| INT        | make -j OPT=1 ASSERTS=1 LTO=0 TESTS=1 INT_TESTS=1           |
| GCOV       | make -j OPT=0 TESTS=1 INT_TESTS=1 GCOV=1                    |
| no suite   | make -j OPT=1 TESTS=0                  (for system_smoke)   |

Add BUILD_DIR=some-dir to keep builds apart. A debug build has the address
and undefined-behaviour sanitizers on, which is where most memory bugs
show up first. "mylang -v" prints how a binary was built.

Never benchmark an INT or debug build: they are slow on purpose.


Watching progress
-----------------

Every tool that can run for more than a minute reports its progress. From
any terminal:

    python3 tests/testctl.py

prints one line per running tool:

    TOOL             PID      %  DONE/TOTAL  PHASE   ELAPSED     ETA  FAIL
    int_run       790151  42.2%       19/45  units     2m10s   2m58s     0
    corpus_diff   764141  74.7%     361/483  runs      0m03s   0m01s     0

To keep it on screen, refreshed every two seconds:

    python3 tests/testctl.py watch

The same numbers appear in each tool's own output as a heartbeat line
every minute, which is also what you see in a CI log.

Some tools (int_enum, int_select, mutate) can also be paused, stopped and
resumed:

    python3 tests/testctl.py status int_enum

    python3 tests/testctl.py stop int_enum

    python3 tests/testctl.py jobs 4 int_enum

A stopped run prints a resume token; pass it back with --resume to carry
on where it stopped.


What CI runs
------------

On every push:

| Workflow  | Runs                                                         |
|-----------|--------------------------------------------------------------|
| Linux     | -rt on seven builds (debug and release, gcc and clang,       |
|           | adversarial allocator, non-LTO, debug info), driver_checks,  |
|           | myv_doc_check, system_smoke                                  |
| macOS     | -rt                                                          |
| Windows   | -rt                                                          |
| Coverage  | -rt with coverage, uploaded                                  |
| Nets      | corpus_diff (plain, --levers, --nolowmem, --spcheck),        |
|           | bt_oracle, norec_enum, norec_sweep, vdjcmp, disasmcheck,     |
|           | the three fuzzers, int_run, int_enum tiers 1 and 2, and the  |
|           | coverage gates                                               |

Nets takes about 18 minutes; the others finish in under 10. The fuzzers
use a new seed on every run, so each push tries new programs.

On demand only (Actions tab, "Run workflow"):

| Workflow  | Runs                                              | Takes     |
|-----------|---------------------------------------------------|-----------|
| INT deep  | int_enum tier 3, int_select over everything       | ~1 hour   |
| Mutation  | mutate.py over 200 planted bugs, in 8 jobs        | 1-3 hours |

Run these after a series of complex changes rather than on every push.


Adding a test
-------------

| You want to test                      | Put it in                         |
|---------------------------------------|-----------------------------------|
| a language feature or an error        | src/tests.cpp (the -rt suite)     |
| a JIT or VM shape, on every engine    | tests/functional/NAME.my          |
| a backtrace or caret                  | tests/bt_oracle/NAME.my           |
| a compiler decision, under INT hooks  | tests/int/NN_NAME.my              |
| a REPL session                        | tests/int/repl/NAME.session       |
| a command-line flag                   | tests/driver_checks.sh            |

A functional test is a small program that checks its own results and
builds the tricky shape on purpose. corpus_diff then runs it on every
engine and every JIT setting automatically.

A REPL session is the text you would type, one input per line, ending with
:quit. Its expected output lives next to it in NAME.expected; write it with

    python3 tests/int_run.py build-int/mylang --update-repl

and read it before committing: that file is the assertion.

Before trusting a new test, break the code it tests on purpose and watch
the test fail. A test that passes either way checks nothing.


About the fuzzers
-----------------

nested_fuzz.py writes thousands of random, deeply nested programs (loops
inside conditions inside loops, with arrays, dicts, break and continue)
together with the same program in Python, and requires every MyLang engine
and CPython to print the same result. CPython is an independent oracle: it
also catches a bug where all MyLang engines agree on the same wrong
answer. The programs stay inside the subset where MyLang and Python are
meant to agree (no negative modulo, no overflow, no dict iteration order),
so any difference is a real bug.

myv_fuzz.py takes two compiled images and damages them in five ways
(flipped bits, random bytes, truncation, random bursts, 0xFF words). A
damaged image may be refused or may even run, but it must never crash or
hang the interpreter.

repl_fuzz.py feeds the interactive REPL random sessions built from
fragments: declarations, redefinitions, unfinished input, meta-commands
with nonsense arguments. The REPL must survive every one.

Fuzzers find what nobody thought to write a test for. When one finds
something, the program that found it becomes a fixed test, so the bug
stays caught.
