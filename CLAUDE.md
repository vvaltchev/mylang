# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

> **READ `README.md` IN FULL BEFORE TOUCHING ANYTHING.** This is a small project
> and the README is
> the complete language specification (every keyword, every builtin, every
> semantic rule, with
> examples). It is not optional reference material to consult on demand — read
> the whole thing up
> front, once, so you know the language the interpreter implements. This
> CLAUDE.md covers the *C++
> implementation*; the README covers the *language*. You need both in your head
> before making changes.
>
> **`docs/` is the on-demand half.** CLAUDE.md keeps what you must know
> before you start: the rules, the invariants, the traps, the map of what
> lives where. The detail that only helps once you are already inside a
> subsystem lives in `docs/` and is named at the point where you would need
> it — `docs/jit-optimizations.md` (the JIT/VM per-change record) and
> `docs/myv-format.txt` (the stored-bytecode byte spec). A pointer to one of
> those is an instruction to READ IT, not a footnote.

> **KEEP THE DOCS IN SYNC WITH THE SOURCE — IN THE SAME CHANGE.** `README.md`
> and this `CLAUDE.md`
> are part of the codebase, not afterthoughts. Any code change must carry its
> documentation update in
> the *same commit*, never as a follow-up:
> - **`README.md`** whenever script-visible behavior changes — a
>   new/removed/renamed keyword, builtin,
>   operator, or numeric constant; changed semantics; new error conditions.
>   README is the language
>   spec; if it and the interpreter disagree, that's a bug.
> - **`CLAUDE.md`** whenever the *implementation* shape changes — a new source
>   file or `.cpp.h`, a new
>   `TypeE`/AST node, a changed design rule or invariant, a new convention, or
>   anything that would make
>   a sentence in this file wrong. After editing code, reread the relevant
>   CLAUDE.md section and fix any
>   statement the change just falsified.
> - **`docs/jit-optimizations.md`** whenever you change the JIT or the VM's
>   op set — it holds the per-change record (what each optimization does,
>   what it measured, which guard makes it sound, which sabotage was watched
>   failing). Add your entry there, not here; only a RULE that a future
>   reader must obey *before* knowing they are in that area belongs in
>   CLAUDE.md.
> - **⛔ `docs/myv-format.txt` IS ALWAYS UP TO DATE — HARD RULE
>   (maintainer-set, 2026-08-09).** It is not "documentation of" the format,
>   it IS the format's specification: it describes every record, in order,
>   with its exact byte encoding, and a third party must be able to write a
>   reader from it ALONE. So any change to what `myv_write` emits or
>   `myv_read` accepts — a new field, a changed width, a new pool, a bumped
>   `MYV_FORMAT_VERSION`, a new validation rule — updates this file in the
>   SAME commit. There is no "I'll sync the doc later": later means a reader
>   who trusts it produces a corrupt image.
>
>   **IT HAS A MACHINE CHECK, SO "I THINK IT'S FINE" IS NEVER THE ANSWER:**
>
>       make -j BUILD_DIR=build-claude/dbg TESTS=1 OPT=0
>       ./build-claude/dbg/mylang -c samples/gcd -o /tmp/gcd.myv
>       tests/myv_doc_check.py /tmp/gcd.myv
>
>   That script is written from the DOCUMENT and deliberately not from
>   `serialize.cpp`; consuming an image to exactly EOF is what proves the doc
>   complete. **RUN IT after any serializer change** — and note it was found
>   FAILING on 2026-08-09, the doc stale at v10 while the format was at v13
>   (v11 `proven_type`, v12 capture Locs, v13 `base_locs` had all landed
>   without it). That is the failure this rule exists to prevent.
>
> A change that alters behavior or architecture but leaves the docs stale is
> incomplete.

## ⛔ INFRASTRUCTURE FIRST, TESTS SECOND, FEATURES/OPTIMIZATIONS LAST
## (maintainer-set, 2026-08-18)

The priority order, always. **Fixing a tool, a script or a test needs
NO approval** — do not ask, do not offer it as an option, do not defer
it to a follow-up task. Fix it, then continue.

**THE INSTRUMENTS IN THIS REPO — the things whose output is treated as
fact, and which must therefore be correct before anything built on
them means anything:**

- **`-vdj`** (disasm.cpp) - what machine code was emitted. Oracle:
  **`scripts/disasmcheck.py`**, which cross-checks EVERY emitted
  instruction against **objdump** - both the BOUNDARIES (a wrong length
  desynchronises the rest of the fragment) and the mnemonics; plus the
  self-report `DUMP IS UNRELIABLE` and the `-rt` entry
  `jit: -vdj decodes every emitted form, address-free`.
  **⛔ THE SELF-REPORT IS NOT ENOUGH AND CANNOT BE.** It counts `.byte`
  lines - bytes we KNOW we failed on - and says nothing about a
  sequence decoded CONFIDENTLY AND WRONGLY, which is the failure that
  cost weeks (the SIB displacement bug). A decoder cannot check itself.
  On 2026-08-19 the objdump oracle found `F7 /3` (neg) and `F7 /5`
  (imul) missing on 284 corpus sites, printing a placeholder `f7/? rdx`
  that was neither a mnemonic nor a `.byte` - silent to the banner AND
  claiming a length it had not earned. **Every "I do not know" path in
  `decode_one` must end at the single `undecoded:` label**; adding a
  new one that prints a placeholder re-opens the hole. Current status:
  2,209,682 instructions over the corpus x both arenas x 7 pin
  rotations x 5 pin budgets, ZERO disagreements. `MYLANG_VDJ_HEX=1`
  puts the raw bytes in the dump, which is what makes the check
  possible.
- **`-vd`** (disasm.cpp) - what bytecode was emitted. Oracle:
  `myv_round_trip` (a loaded image vs a fresh compile).
- **`-s` / `-a` / `-dti`** - what the optimizers did to the tree.
  Oracle: the `analyze:` `-rt` entry.
- **`scripts/vdjcmp.sh`** - "is my JIT change a pure restructuring?"
  Oracle: a SELF-TEST - one binary against itself must be 124/124.
  **⛔ IT RUNS IN CI NOW (`nets.yml`, the differential job), because
  for one day it was correctly REFUSING every comparison and nobody
  invoked it** (see the `-vdj` reproducibility note below). Two more
  layers cover the same property: `-rt`'s address-free invariant (which
  a new OPERAND FORM once walked straight past) and
  `driver_checks.sh`'s two-process dump comparison.
- **`scripts/regcensus.py`** - how much work is left to free a
  register. Oracle: it DERIVES its accessor list from the source.
- **`bench/run.py`** - is it faster. Oracle: RULE B1, the build-config
  gate, and the machine-speed marker.
- **`MYLANG_JIT_MAXPINS=N`** - cap the JIT's pin budget, so the
  MARGINAL value of one register is a measured number. Oracle: a
  NON-BINDING cap must be a no-op - `N >= jit_pin_budget()` has to
  emit byte-identical code to unset, over the whole corpus. (It is
  NOT the same as `MYLANG_JIT_OFF=cache`, which clears the picks
  AFTER the pick and so leaves the C3/C4a side-outputs derived from
  a full budget.)
- **`MYLANG_JITSTATS`** - which TIER a program's calls actually took.
  Oracle: the counters are bumped from EMITTED code only.
- **`tests/corpus_diff.sh`** - do the engines agree. Oracle: the
  `--levers` / `--cold` / `--xrot` / `--nolowmem` matrices.
  **⛔ IT COMPARED ONLY `tail -3` UNTIL 2026-08-26** - the last THREE
  LINES of each program - so a divergence anywhere earlier was
  INVISIBLE, and the tool answered "28/28 agree" for a binary that
  printed `1 0` where the tree-walker printed `true false`. The
  truncation was only ever meant to keep the DIFF MESSAGE short; it
  compares the WHOLE output now and truncates the printed `diff`
  instead. The re-run over every matrix found nothing else hidden -
  the corpus was clean, the tool simply could not have proved it.
  **A comparison oracle must compare everything it claims to.**
- **`MYLANG_NO_LOWMEM=1`** - refuse the low-address arena, so the
  JIT's REGISTER-form type tags (the shipping configuration wherever
  `MAP_32BIT` is unavailable or fails) are reachable by a test.
  Oracle: `mylang -v` reports `lowmem 0/1`, which the `nolowmem` CI
  lane ASSERTS in both directions before it tests anything - a lane
  that cannot prove it is in the configuration it tests goes vacuous
  the day a default moves.

- **`tests/myv_doc_check.py`** - is `docs/myv-format.txt` still the
  spec. Oracle: it is written from the DOC, not from serialize.cpp.
- **`scripts/jitprofile.py` + `MYLANG_JIT_MAP=<path>`** - WHICH
  EMITTED INSTRUCTION does the JIT spend its instructions on. It joins
  a per-fragment instruction map, written AT RUNTIME BY THE PROFILED
  PROCESS (so the addresses are the real ones - ASLR and where mmap
  lands make a separate `-vdj` run useless for this), against a
  callgrind `--dump-instr=yes` profile. `--listing` prints a fragment
  in ADDRESS order with each instruction's Ir and a blank line at every
  not-executed gap, which is how you read a hot PATH rather than a hot
  instruction.
  **⛔ IT EXISTED FOR NOBODY UNTIL 2026-08-27, AND THE WHOLE #97 CALL
  ARC WAS COSTED WITHOUT IT** - by counting the emitted sequence BY
  HAND out of `-vdj` and hoping the counted path was the one that ran.
  JIT code lives in an anonymous mapping with no symbols, so callgrind
  put every emitted instruction in one nameless blob; `-vdj` says what
  was EMITTED, never what EXECUTED. Its first run answered in seconds a
  question three sessions had estimated: of 142 Ir per call, 28 are the
  ARGUMENT round trip and 71 are ACTIVATION BOOKKEEPING.
  Oracle: a mandatory join SELF-TEST - mapped + unmapped Ir must equal
  the run's own `summary:` line, and it EXITS 3 when it does not.
  Watched failing: dropping the `calls=` skip (which charges a call
  site its callee's whole subtree) inflates the profile 8x and is
  otherwise completely plausible and completely silent.

**FIX THE TOOL, NOT THE SCRIPT THAT READS IT.** Three cases from this
repo, all found in one week, all the same shape:

 - **the disassembler (2026-08-17/18).** `-vdj` could not decode **5543
   bytes** corpus-wide — six missing opcodes and a SIB decoder that
   never consumed its displacement — and printed confident wrong
   mnemonics rather than admitting it. The first response was to mask
   the symptoms in `vdjcmp.sh` with a sed pipeline plus `setarch -R`.
   The mask was hex-only, #96 step 3 made the values decimal, and from
   that day the script reported **0 identical / 108 differing for every
   comparison** — it had stopped being an oracle and looked exactly
   like a catastrophic change. It also reported a binary as differing
   from ITSELF (31 of 108), which no regex could ever have fixed. The
   fix belonged in `disasm.cpp`; afterwards the comparison script is a
   plain `cmp`;
 - **the register census (2026-08-17).** `regcensus.py`, written
   expressly to count hardcoded register uses, reported **14** for r9
   where the truth is **87** — it could not see `movabs_r9`,
   `cmp_r9_rdx`, `lea_rdi`, `slots_to_arg0`, `store_elem_byte_dil`,
   because the register is in the METHOD NAME. Acting on that number
   put an unsafe register in the JIT pin pool and shipped a wrong
   answer for a day;
 - **`bench/run.py` defaulting to the maintainer's binary** (RULE B1) —
   the same class: a plausible number measured from the wrong subject.

**THE THREE PROPERTIES a MyLang instrument must have**, and where each
already exists so a new one can copy it:

 1. **REPRODUCIBLE.** `-vdj` prints `<int-tag>`/`<addr>`/`<helper>`
    instead of baked pointers, so two runs and two separately-linked
    binaries give identical text (`MYLANG_VDJ_ADDRS=1` for the digits).
    Before that it was not comparable at all.
 2. **IT SAYS WHEN IT DOES NOT KNOW.** `-vdj` counts undecoded bytes
    AND skipped op marks (a mark is an offset the JIT recorded at a
    real instruction boundary, so stepping past one proves the decode
    drifted) and prints a banner. `run.py` prints its
    `mylang : <path>` header and refuses a wrong build config.
 3. **IT SELF-TESTS.** `vdjcmp.sh` compares a binary with ITSELF and
    exits 2 if that is not identical — otherwise it cannot distinguish
    "your change altered the code" from "the tool broke", and it
    reported the second as the first for weeks.

**And the corollary for TESTS of an instrument: watch it fail.** The
`-vdj` coverage check was VACUOUS THREE TIMES before it caught
anything — it counted a bytecode op NAME for its vacuity guard;
`-vdj` is `g_jit_annotate`, not a dump flag, so the dump had no native
section at all; and its programs were all statically typed while the
missing opcode lives in a boxed TYPE CHECK. See *THE VACUOUS-TEST
TRAP*.

## What this is

MyLang is an educational, dynamically-typed scripting language (C-looking
syntax, Python-ish
semantics) implemented as a tree-walking interpreter in portable C++17. It has
**no dependencies**
beyond the standard library, including for its tests. The single `mylang`
executable both compiles
(lex + parse + const-fold) and runs scripts. Author's goal was to have fun
writing a recursive-descent
parser; correctness and clarity matter more than raw speed, though performance
shaped several core
design choices (see the value model below).

## Build & run

```
make -j                    # release build (-O3) -> build/mylang
make -j TESTS=1 OPT=0      # debug build, unit tests compiled in (for -rt)
make -j BUILD_DIR=other    # out-of-tree build
make clean
```

> **⛔ CLAUDE BUILDS ONLY UNDER `build-claude/` (maintainer rule,
> 2026-07-26).** Every build Claude makes goes in a subdirectory of
> `build-claude/` — e.g. `make -j BUILD_DIR=build-claude/release`,
> `BUILD_DIR=build-claude/dbg`, `BUILD_DIR=build-claude/meas-<sha>` — NEVER
> in `build/`, `build-rel/`, or any other top-level directory. The
> maintainer's own build dirs (whatever he creates: `build`, `build-rel`,
> ...) are OFF-LIMITS: never build into or measure with them (for
> DELETING `build/`, see the mandatory pre-benchmark step below — the
> maintainer's 2026-08-12 instruction, which overrides the older
> "never delete" wording for that one case).
>
> ## ⛔⛔ RULE B1 — NEVER BENCHMARK THE MAINTAINER'S BINARY ⛔⛔
> ### (maintainer-set 2026-08-12, after it produced two invalid runs)
>
> **`bench/run.py` DEFAULTS `--mylang` TO `build/mylang` — THE
> MAINTAINER'S BINARY, NOT YOURS.** A bare `python3 bench/run.py` does
> NOT measure your change. It silently measures HIS build, at whatever
> commit he last compiled, and prints a plausible geomean you will then
> report as your result. That happened on 2026-08-12: two full-suite
> runs and a reported "post-flip 2.514x" were all his binary. The
> maintainer's words: *"it's truly unacceptable."*
>
> **THE TWO MANDATORY STEPS, BOTH REQUIRED, EVERY TIME:**
>
> 1. **DELETE `build/` BEFORE ANY PERFORMANCE RUN** — `rm -rf build`.
>    This is the maintainer's explicit instruction and it OVERRIDES the
>    "never delete his build dirs" clause above for this one purpose.
>    The point is FAIL-FAST: with `build/` gone, a forgotten `--mylang`
>    makes run.py die immediately instead of quietly timing the wrong
>    program. He rebuilds his own tree when he wants it; a silent wrong
>    number costs far more than his rebuild.
> 2. **PASS `--mylang build-claude/<lane>/mylang` EXPLICITLY** (and
>    `--baseline` likewise, when comparing). Never rely on the default.
>
> **AND CHECK THE HEADER.** run.py prints one line - `mylang : <path>` -
> as its first output. READ IT before quoting any number from that run.
> If it does not name a `build-claude/` path, the run is INVALID: throw
> the numbers away, do not "sanity-check" them, do not report them.
>
> This is restated at the benchmark sections below on purpose. It is the
> easiest mistake in the repo to make and the hardest to notice, because
> the wrong answer looks exactly like the right one.
> Name the lanes descriptively and DELETE throwaway measurement lanes when
> the measurement is done — 40+ stale `build-meas*` dirs once littered the
> repo root, which is what triggered this rule. Standard lanes:
> `build-claude/release` (plain `make OPT=1` — the ONLY binary to
> benchmark), `build-claude/dbg` (gcc `TESTS=1 OPT=0`, ASan+UBSan),
> `build-claude/clang` (`CXX=clang++ TESTS=1 OPT=0`),
> `build-claude/rel-hard` (`TESTS=1 OPT=1 VM_HARDENING=1` — tests only,
> never benchmarked).

`OPT` defaults to 1 (`-O3`); `OPT=0` drops it. `TESTS=1` adds `-DTESTS`, which
is what compiles the
`-rt` suite into the binary. Base flags:
`-std=c++17 -Wall -Wextra -Wno-unused-parameter
-fwrapv`. The Makefile auto-generates header dependencies under `.d/`.

**⛔ DEBUG INFO IS OFF BY DEFAULT — `DEBUG_INFO` (default 0, EVERY build
type, both build systems; maintainer-set 2026-08-26).** The `-ggdb` that
used to sit in the base flags multiplied the binary's size >10x
(a debug+TESTS build: 155MB -> 12MB) for information unused in most
builds — pure disk wear. `make DEBUG_INFO=1` (CMake `-DDEBUG_INFO=ON`)
restores it; pass it whenever you want a debugger, a SYMBOLIZED
sanitizer report, or line-accurate callgrind attribution (a bare
address-only ASan trace is the tell that you forgot). On the CMake side
the OFF default also strips the `-g`/`/Zi` that CMake's own
Debug/RelWithDebInfo per-config defaults inject. The `dbginfo` CI lane
(linux.yml) builds `DEBUG_INFO=1` on both build systems and asserts the
ELF's `.debug_info` in BOTH directions (present with the flag, absent
without), so neither configuration can rot.

**LTO is on by default for optimized builds.** `LTO` defaults to `OPT`, so a
release build links with `-flto=auto` (added to `BASE_FLAGS`, which the link
line passes too) — ~7% smaller binary and ~8-9% faster on `bench/`. It works on
both GCC and clang and is verified to keep `-rt` green. Build with `LTO=0` to
disable (e.g. for a faster/debuggable link); an `OPT=0` build is non-LTO anyway.

**⛔ `LTO=0` IS A LANE, NOT A FOOTNOTE — AND IT CARRIES DIAGNOSTICS LTO
CANNOT (2026-08-17).** Because `LTO` defaults to `OPT`, every optimized
build anywhere — CI included — was an LTO build, so `make OPT=1 LTO=0` was
compiled by nobody and had been BROKEN for a day before anyone tried it.
The break was not cosmetic: **GCC's `-Wclobbered` needs the ordinary
per-TU pipeline to see a whole function at compile time, so an LTO build
is SILENT about it**, and what it was silent about was real UB —
`jit_str_probe_verify` read a non-`volatile` local after `siglongjmp`
(indeterminate per C17 7.13.2.1p3), so a faulting probe could have
reported SUCCESS and the JIT would have baked a `std::string` layout the
toolchain does not have. The `lto0` job in `.github/workflows/linux.yml`
(both compilers, Makefile + CMake, `-rt` + `driver_checks` +
`system_smoke`) exists to keep that closed. It ASSERTS `mylang -v`
reports `lto 0` before testing, so the lane cannot go vacuous if a
default changes — and for that to be possible on the CMake side,
**CMake now defines `ML_LTO` too** (a per-config generator expression
tracking the OUTCOME of `check_ipo_supported`, not the request; a CMake
build used to answer `lto unknown`). **A warning that only one build
configuration can see is a warning nobody sees.**

**⛔ AND IT CUTS BOTH WAYS: THE LTO BUILD SEES WARNINGS THE PER-TU ONE
CANNOT, SO A GREEN `make -c` DOES NOT MEAN A GREEN LINK (2026-08-25).**
`-Werror` fires at **lto1**, i.e. during the LINK, on code the LTRANS
inliner assembled — so the failure names a line in a file that compiled
silently minutes earlier. The first instance: `TypeFunc::clone` read
`func.capture_slots.empty()` through a handle obtained from
`EvalValue::get<T>() const`, which returns a **COPY** — the temporary's
`~intrusive_ptr` inlines `release()` -> `delete` -> the POOLED
`operator delete`, which writes the freelist link over the object's
first 8 bytes and ends its lifetime. GCC cannot prove the count never
reaches 0, so the read after it is "may be used uninitialized"; the
plain `-O3` compile of types.cpp inlines the free differently and says
nothing. **The idiom, not a suppression: read a handle you only borrow
with `get_ref<T>()`** (no retain/release pair, no phantom free before
the read — `TypeDict::clone` already did). When a link-only warning
appears, reproduce it with the LINK STEP ALONE (the objects are already
built: ~3 min vs a full rebuild) and add `-Wno-error
-fopt-info-inline-optimized=FILE` to see what the inliner put there.

**Sanitizers default on for debug builds.** `ASAN` and `UBSAN` (AddressSanitizer
/ UndefinedBehaviorSanitizer) both default to **on when `OPT=0`** and **off when
`OPT=1`**, and either can be forced: `make ASAN=0` (debug, no ASan),
`make OPT=1 UBSAN=1` (sanitized release). The flags go into `BASE_FLAGS` so they
reach the compile and link lines. UBSan is configured `-fno-sanitize=signed-
integer-overflow`, because the codebase relies on `-fwrapv` wraparound (that
overflow is *defined* here, not a bug); it also runs with
`-fno-sanitize-recover=undefined`, so a UBSan finding **aborts** (non-zero exit)
instead of diagnose-and-continue — otherwise a real UB could print and still
exit 0 past CI's exit-code check. `-fno-omit-frame-pointer` is added whenever
either sanitizer is on. `-rt` is verified green under both.

**Assertions: `ASSERTS` (default 1).** The C `assert()` + the project's
`ML_CHECK()` invariant net (see *Invariants & hazards*) are **on for every build
type** (debug AND release), so every build and CI lane exercises them. With
`ASSERTS` on the build also enables libstdc++ container hardening
(`-D_GLIBCXX_ASSERTIONS`, ABI-safe; the libc++ analog is set per-OS in CI).
`make ASSERTS=0` defines `-DNDEBUG`, compiling all of that away — use it on an
optimized build to measure the assertion overhead, e.g. `make OPT=1 ASSERTS=0`
vs the default `make OPT=1`. **`RECYCLE` (default 0):** `make RECYCLE=1 TESTS=1`
builds the adversarial node allocator (see *Invariants & hazards*).

**Warnings-as-errors: `WERROR` (default 1).** Every build (both build systems,
every type, ALL THREE compilers) treats a warning as an ERROR, so a warning
CANNOT be pushed — it fails the build and must be addressed (see the warning
rule under *Conventions*). GCC/Clang get `-Werror`; MSVC gets `/WX` (its CMake
default level is `/W3`). CMake also gains the Makefile's `-Wall -Wextra
-Wno-unused-parameter` for GCC/Clang (it set none before, so CI was laxer than a
local build — that gap is why the MSVC narrowing/`getenv` warnings accumulated
unnoticed). `make WERROR=0` (or CMake `-DWERROR=OFF`) opts out for a
work-in-progress build.

**VM hardening: `VM_HARDENING` (default = debug).** The heavy per-op VM
invariants (`ML_VM_CHECK`, `defs.h`) — a **frame-slot bounds check on every
register access** (`Frame::at`) plus an **operand type-tag check** in the VM's
int/float readers — are too hot for a normal release (a compare per register
access), so they default **ON for a debug build (`OPT=0`) and OFF for a release
(`OPT=1`)**. `make VM_HARDENING=1` forces them on; **CI turns them ON in the
*release* lanes** (`-DVM_HARDENING=ON`), so a CI release runs with far more
safety than a local one — a layout-dependent VM UB (a bad slot index reading an
unconstructed/out-of-range `LValue` → a garbage type pointer → a crash only on
some toolchains) fails as a **loud, located `ML_VM_CHECK` assertion** instead of
a mystery segfault. Still gated by `ASSERTS` (a no-op under `NDEBUG`). CMake:
`-DVM_HARDENING=ON/OFF` (default follows the build type). See *Invariants &
hazards*.

**THE JIT / VM OPTIMIZATION RECORD LIVES IN `docs/jit-optimizations.md`.**
Everything the native tier and the VM's op set are made of - the register
pools (N5/C2a/C2b) and the literal pool, the forwarding levers (A, C4a-ii,
C4b), the type-store elisions (C3), the loop hoisting and versioning family
(C1a-e), the struct baked layout and its guard elisions (C4d/C4e/C5), the
call protocol (lever 1, M5a/b/c), delete-originals (#56), the exception path
(#74/#78/#80/#82/#88), the element-store tiers (#92-#96), the specialized
arithmetic (B1/B2) and the fusions (E4, #9) - is recorded there, one entry
per change, with its measurement, its soundness guard, and its sabotage
result. **READ THE RELEVANT ENTRY BEFORE CHANGING ITS SUBSYSTEM**; each one
documents a trap that cost real time to find. The `-vd`/`-vdj` dumps and the
lever kill switches (`MYLANG_JIT_OFF`, `MYLANG_JIT_FORCE`, `MYLANG_JIT_COLD`)
are described there too.

**`MYLANG_JITSTATS=1` answers "which TIER did this program's calls actually
take?"** — the emitted-code counters, printed after a script run (a
**`TESTS=1` build only**; the bumps are `#ifdef TESTS`, and it says so
rather than print zeros in a release). It exists because the counters were
readable only from inside `-rt`, so that question could be asked of a
hand-written probe and of nothing else — which is exactly how a per-call
cost came to be quoted for a path most real calls turned out to skip, and
then how the OPPOSITE error was caught: the recorded finding "recursion
never reaches the inline push" stayed in the plan for two days after the
increments that made it false. **Re-measure reach after any change to a
tier's GATES, not just its body.**

The RULES that came out of that work stay HERE, because a rule must be
obeyed before you know you are in the area it governs:

**⛔ THE AUDIT-TABLE STAGE TRAP (2026-08-04) - `visit_use_def` did not
know the B1/B2 SPECIALIZED family, and NOTHING could see it.** The
table (codegen.cpp) is consulted by the PEEPHOLE's E1 liveness and by
`compute_ref_slots`, and BOTH run before `specialize_arith_ops` - so
the 23 specialized opcodes (IntAddRR .. FloatMulRI) simply never
reached it and their absence was invisible. But **`jit_fwd_info` runs
at JIT TIME**, on the specialized code, and its contract for an
unaudited op is the conservative BARRIER `lin = all` - every temp live
at every pc. So both of its consumers were silently inert: lever A's
`skip_write` (whose own note already suspected it) and C4a-i's
read-elision entry gate, which refused every float temp and made the
whole increment measure FLAT. C4a-i had a second, smaller bug of the
same family - it tested `fwd_lout` (live-OUT) where it meant live-IN,
so a run head whose first op writes the temp trivially "read" it as
live; `jit_fwd_info` now exports the fixpoint's own `livein` beside
`liveout`. FIXED: the family joined the table. Measured (Ir/scale,
OPT=1 ASSERTS=0): **55_float_sum -15.0%**, 40_math_builtins -3.2%;
54/04 byte-flat (C2a already pinned their float locals), every int
bench byte-flat (`skip_write` stays blocked there by ref_slots'
conservatism - lever A's documented throttle). PINNED by
`jit_fread_temps_audited` over a compile-time `g_jit_fread_temps`
count of admitted TEMPS: `g_jit_fread` cannot see this - it bumps per
fragment ENTRY, which the two LOCALS alone satisfy - and removing the
family from the table again fails the test (watched).
**IT RECURRED ON THE SIBLING TABLE (2026-08-05), and that is the
strongest possible argument for the rule below.** `op_writes_scalar`
lives in the SAME file, is consulted by `compute_ref_slots` at the SAME
pre-specialization stage, and was NOT fixed when `visit_use_def` was -
because at that stage it cannot see the family either, so its absence
was equally invisible. **C5** is the first consumer to ask it at JIT
time, on the specialized code, and its conservative "not a scalar
write" answer silently refused most real arithmetic: 64_struct_create
kept every store guard it was supposed to lose. Adding the 23 opcodes
changed NOTHING for `compute_ref_slots` (they do not exist at its
stage - `-vd` dumps are byte-identical) and read **-32.0% on
54_mandelbrot, -28.1% on 03_int_arith, -24.1% on 64_struct_create,
-16.7% on 06_if_branch, -15.4% on 07_nested_loops, -14.8% on
04_float_arith**, -7.1% / -5.8% / -1.2% on 55/46/40, everything else
byte-flat. The net is a `jit_release_c5` case whose qualifying temps
are written ONLY by the specialized family, so the table losing them
again fails a test instead of costing 20% in silence.
**⛔ AND THE SAME TRAP HAS A SECOND SHAPE: A WRITE THAT IS NO LONGER
AN OPCODE (2026-08-13).** `compute_ref_slots` derives a chunk's
`ref_slots` from instruction write-dsts via `visit_use_def` - complete
only while every write to a frame slot IS an instruction. **#78 step D
broke that premise**: deleting the interpreted `CatchTest` chain moved
the catch binding into the RAISE PATH, where `vm_dispatch_exc_body`
does `ctx.frame->at(cl.bind_slot).put(...)`. No opcode writes that
slot, so it never entered `ref_slots` - and `catch (T as e)` binds a
STRUCT INSTANCE, a reference. `pop_window`'s release scan skipped it
and the hardened re-scan aborted (`rec.window[i]...->t < Type::t_str`)
on any frame catching a CALLEE's throw with a bind. The same-frame and
top-level spellings release elsewhere and were fine, which is why no
corpus program in the project's history had tripped it. Both `-rt` and
`corpus_diff` stayed GREEN with the fix removed; the Net 3 enumeration
failed 16 of 96 at depth 2. Fixed by feeding `handler_sites`' bind
slots into `compute_ref_slots`; pinned by
`tests/functional/11_catch_bind_release.my`.
**When a RUNTIME path starts writing a frame slot, it must join
`ref_slots` - an opcode-derived table cannot see it.**

**⛔ AND AN EIGHTH SHAPE, ONE LEVEL IN: A TABLE'S INPUTS MUST BE
COMPLETE BEFORE ANY RULE IN IT BECOMES RELATIONAL (#97 step 5,
2026-08-27).** `compute_ref_slots` marks a slot that may hold a
reference, and `compile_func_body` unioned the non-scalar PARAMETER
slots into the result AFTERWARDS - the bind writes them and no
instruction does, so `visit_use_def` cannot see them. That order was
safe for years because every marking was INDEPENDENT: a late union of
extra members changes nothing about the members already decided.

It stopped being safe the instant one rule READ another entry to decide
its own. Teaching the table that a `MoveV`'s dst is a reference only if
its SOURCE is - the fix for "every argument temp in every program is
reference-carrying" - made `move t = param` ask about a source the pass
believed trivial, because the params had not gone in yet. The seeds are
an INPUT now (`codegen_chunk`'s `ref_seeds`), not a post-hoc union.
`jit_ret_audit` caught it on the first run and the old order is watched
failing (rc 134).

**When you add a rule that derives one entry from another, re-ask where
every entry comes from** - including the ones no instruction writes.

**THE RULE this earns:** a table is "audited" only for the PIPELINE
STAGES that existed when it was written. A pass added later, at a
DIFFERENT stage, sees a different opcode universe - and a conservative
fallback means the gap costs silence, not failure. When you add a
consumer of an audited enumeration, ASSERT THE TABLE COVERS ITS INPUT
(the way `visit_pc_fields`' remap net and the `ref_slots` audit do),
or the next such gap will also be found only by someone reading a
disassembly for an unrelated reason.
**⛔ AND A THIRD SHAPE: THE TABLE STAYED PUT WHILE AN OP CHANGED
UNDER IT (2026-08-13).** `op_writes_pure_target` (codegen.cpp) lists
the ops whose `.target` may be RETARGETED, so `<produce t>; MoveV
rArg = t` fuses into `<produce rArg>` for a call argument. Its
soundness needs the op to be the SOLE producer of that temp, and its
own comment has excluded `MoveV` since 2026-07-26 for exactly the
JOIN shape that breaks it. **`LogV` was in the list and belonged
there** - a `&&`/`||` chain was a straight run of LogV ops, one
producer each - until **#138 gave the chain real short-circuit
branches**. `emit_logical_chain` then wrote ONE dst from N arms: a
join whose tail is a LogV, not a MoveV. Retargeting only the last arm
left the short-circuiting path writing the old temp, so
`var dyn s = runtime(3); print(s > 5 && s < 6);` printed **`3`**
under the VM and `false` in the tree-walker - a wrong answer and a
RULE 2 divergence, latent in 8 corpus programs (bytecode changed,
output did not). **When you change HOW a construct lowers, re-audit
every table that classifies its OPCODE** - the entry does not have to
be edited to become false.
**⛔ AND A FOURTH SHAPE, THE CHEAPEST TO PREVENT: THE OPTIMIZATION
WHITELIST NOBODY RE-READ (2026-08-16).** `jit_fwd_producer` /
`jit_fwd_consumer` (jit.cpp) list the ops lever A may forward a value
between. They were written over the B1/B2 specialized family as it
stood - Add/Sub/Mul/And/Or/Xor, RR and RI - and `specialize_arith_ops`
LATER grew **IntShlRR/RI, IntShrRR/RI** and IntModRI. Nothing
re-audited the lists, and nothing could notice: an unlisted op simply
does not forward, so `t = a >> 3; a = a ^ t` - a temp alive for ONE
instruction - kept paying a type store, a payload store and a reload,
per shift, per iteration. Admitting the shifts read **-21.0% Ir on
83_regs_int_40 and -17.97% on 80_regs_int_08**, 11 corpus programs
changed and every other one byte-identical.
**The generalisation: a table whose stale entry costs an OPTIMIZATION
is not self-announcing the way one whose stale entry costs
CORRECTNESS is** - `verify_chunk` fails the BUILD, this failed
nothing. FOUR nets ran over it and every one was structurally
incapable of seeing it, which is the part worth internalising:
 - the 5-mode differential, `corpus_diff` and all four fuzzers are
   blind BY CONSTRUCTION - forwarding changes no observable behaviour,
   so the answer is right whether the value travelled in RAX or
   through a slot. **An optimization that only affects SPEED has no
   correctness oracle**, the same structural gap as *Testing an AST
   TRANSFORM*;
 - `jit_counter_coverage` asks "did this lever run AT ALL?" and
   `g_jit_fwd` was non-zero throughout - the add/mul shapes fire. A
   whole-lever counter cannot see "runs for 12 of 17 opcodes";
 - the lever's OWN test, `jit_fwd_deadtemp`, had its cases written
   FROM the whitelist, so it exercised exactly the opcodes already in
   it. **A test derived from a table can never find a hole in that
   table** - this is the deepest of the four and the one to watch for
   everywhere;
 - no corpus program had a dense shift loop until #96 wrote one.

**THE FIX, AND THE PATTERN TO REUSE: derive the test from the OPCODE
ENUM, not from the table.** The B1/B2 specialized family is a
CONTIGUOUS range (`IntAddRR .. FloatMulRI`, bytecode.h), so
`jit_fwd_family_coverage` (tests.cpp) walks the range and requires
every member to be CLASSIFIED - forwardable, or exempt with a written
reason - and requires each row's claim to MATCH the live predicate in
both directions. Adding an opcode to that range without deciding now
fails with the opcode NAMED. Watched failing three ways: the shifts
dropped from both whitelists (8 lines, one per opcode per side), a row
deleted ("opcode #104 (IntShlRR) joined the specialized family with no
row here"), and a row claiming an exemption for an opcode that is
whitelisted.
For that to be possible the whitelists had to be answerable at the
OPCODE level (`jit_fwd_op_is_producer` / `jit_fwd_op_consumer_slots` /
the float twins, jit.h) - and the Instr-taking predicates are now
BUILT on those, so this is one edit site, not a second copy free to
drift. Verified as a pure restructuring: emitted code byte-identical
on all 94 corpus programs.
**Do this for the next opcode family that gates an optimization.**
And when you add a lever, give it a JITSTATS row too - `g_jit_fwd` was
bumped from emitted code and appeared in no REPORT table, the third
counter found that way.
**DONE WHOLESALE (#98, 2026-08-25): `opcode_table_census` (tests.cpp)
is that ratchet over the ENTIRE opcode enum x SIX opcode-keyed
optimization tables** - `jit_op_eligible`, `op_fully_native`,
`pick_visit_op` (register caching / LSRA qualification),
`op_is_simple_island`, `op_writes_pure_target` (the arg-staging
retarget whitelist) and `bc_inline_op_ok` (the splice whitelist) -
one row per opcode, each claim checked against the LIVE predicate
through exported forwarder shims (jit_test_* / bc_test_*), in both
directions. **Adding an opcode now fails this test until all six
columns are decided**; a deliberate non-answer is a row note, not a
silent `default`. Its first run found and fixed two real holes: the
CallBuiltinLV family was unlisted in `pick_visit_op` (one sort/pop in
a run turned register caching off for the WHOLE run), and both struct
ctors were left out of the #56 deletability batch though convey-only
(the boxed one also missing its emit exc-stamp and the `catch (...)`
eptr net - a #142-class terminate hazard). Record:
docs/jit-optimizations.md, the #98 entry.

**⛔ A SEVENTH AUDIT-TABLE SHAPE: A PROPERTY WRITTEN BY THE PASS YOU
ARE INSIDE (#97 step 4, 2026-08-27).** The first six are about a table
that went stale. This one is about a FIELD that is not written YET.
`Chunk::sync_entry_off` and `Chunk::norec_ok` are both set at the END
of `jit_compile_chunk` for that chunk, so a caller emitting a call to
it reads them correctly - unless the callee IS the chunk being
compiled. A SELF-RECURSIVE call (fib -> fib) therefore sees
`sync_entry_off` unset, and `norec_ok` unset reads as **false**, which
is a legal value meaning "no record-less tier". Gating on them took
09_fib_recursive's baked reach to **4 of 555,823 calls** and would have
silently disabled the no-record tier at the one site that needs it.

**AND THE FIX IS NOT "READ IT IF IT HAPPENS TO BE SET".** That makes
the EMITTED CODE DEPEND ON COMPILATION ORDER, and the JIT's Pass B
walks a POINTER-keyed map - so `-vdj` would stop being reproducible,
which is precisely what `scripts/vdjcmp.sh` is. The elision is allowed
for exactly ONE caller, on a structural argument rather than on luck:
**MAIN is compiled LAST**, after every function body, so every callee
it can name is already placed (`bake_final`; a null
`g_cur_caller_desc` IS main). **When you read a field at emit time,
ask which PASS writes it and whether that pass has run for THIS
object** - and if the answer varies, do not let it vary per run.

**⛔ A HELPER'S REGISTER ABI IS THE EMITTER'S JOB, NOT THE CALLER'S
(2026-08-05).** THREE bugs in two days were one shape - an implicit
register contract violated by a caller: C4a-ii forwarded a value in
xmm0 across a store whose cold arm calls jit_put_float, which clobbers
xmm0; C4b inc 2 let the result live in xmm1 and handed THAT to the same
helper (bench/my/55 off by 1.5); and store_dst's cold arm clobbered RAX,
which ForLoopStep reads immediately after for its loop test (an
OutOfBounds). Detectors kept finding them one at a time, so the
contracts moved INTO the emitter: `emit_put_scalar_call` takes the
VALUE REGISTER and materialises xmm0 itself, and store_dst's cold arm
restores RAX unconditionally - its `keep_rax` parameter is DELETED
rather than defaulted, so no caller can believe it still decides
anything. A caller can still pass the wrong value; it can no longer
forget an ABI. Measured byte-flat (55/46/01 per scale) - the cold arm
already paid the move, and the hot two-store path preserves RAX free.
**When you add a helper the emitter calls, make its argument registers
PARAMETERS and say what it clobbers.**

**⛔ A FIFTH AUDIT-TABLE SHAPE: AN `&&` OVER A FAMILY IS A TABLE THAT
DOES NOT LOOK LIKE ONE (#96, 2026-08-16).** The four traps above are all
`switch` statements, so "go re-read the audited tables" never pointed at
this one. The JIT emitter holds THREE vectors describing what currently
lives in a register — `cache` (N5 int pins), `fcache` (C2a float pins)
and `tflush` (C3 type-elided slots) — and FOUR sites enumerated that
family by hand:

    flush_cache()   three loops, one per vector                    OK
    exit_pc()       cache.empty() && fcache.empty() && tflush.empty()  OK
    the brk guard   !cache.empty() || !fcache.empty() || !tflush...    OK
    the barrier's CLEAR   cache.clear(); fcache.clear();           ⛔ 2 of 3

C3 added `tflush` and updated the first three. The fourth — two
`.clear()` calls that were supposed to mean "empty EVERYTHING" — was
missed, so a barrier'd exit answered "something is still cached", took
the FLUSHING epilogue, and that epilogue writes the whole RESTORED cache:
pre-call register values over the slots the helper had just written,
which is the exact clobber the clear exists to prevent.

**A boolean chain and a run of `.clear()` calls are the same enumeration
in different clothes.** When you add a member to a family, grep for every
site that mentions ALL the existing members together. FIXED
STRUCTURALLY: `snapshot_cache()` / `restore_cache()` /
`clear_cache_state()` / `cache_live()` are adjacent in `Emitter`, are the
only places the family is listed, and `clear_cache_state()` ML_CHECKs
itself against `cache_live()` — so dropping a vector from one and not the
other aborts BY NAME (watched: reintroducing exactly the original miss
fails `-rt` instantly).

**⛔ A SIXTH SHAPE, AND THE CHEAPEST TO WALK INTO: A WRAPPER WHOSE NAME
ENCODES ITS OPERAND IS INVISIBLE TO AN AUDIT THAT GREPS FOR THE OPERAND
(#96 step 3, 2026-08-17).** The JIT held the `t_int` / `t_float` Type
singletons in rsi/r8 and re-materialised them at every fragment entry
and after every helper call. Once the low-address arena made a tag an
`imm32`, those `movabs` became dead — *provided nothing reads the
registers*. The audit for readers was `grep -n "R8R\|RSI"` over the
relevant emitters, and it came back clean, twice. It was wrong, twice,
because the four readers are spelled

    e.cmp_rax_r8();   e.cmp_rax_rsi();   e.cmp_rdx_r8();   e.cmp_rdx_rsi();

— nine call sites, and **not one of them mentions the register as an
argument**. The register is in the METHOD NAME.

The two failures are worth separating, because the second is the nastier
diagnostic:
 - deleting the materialisation first cost four FLOAT tests, via
   `emit_float_load`'s type dispatch — a visible wrong answer;
 - after fixing those it cost the whole **#95 nested-store tier**, which
   **still emitted perfectly**. `-vdj` showed the inline fast path
   present and correct; at RUNTIME its value-type guard compared against
   a garbage register, so every store declined to the helper and the
   emitted-code counter read **0 of 64**. An emitted-code counter proves
   the code was EMITTED; when the thing that broke is a GUARD, reach is
   zero and the counter looks exactly like "the tier was never
   nativized".

Fixed structurally: `store_type_tag(disp, tag, fallback)` and
`cmp_reg_tag(reg, tag, fallback)` are the ONLY tag store / tag compare
entry points, both take the tag as an ARGUMENT, and the four fixed-pair
wrappers are DELETED so they cannot be reached again. **When you audit
for uses of a register, a value, or a slot, grep the ACCESSORS as well
as the operand — and prefer a seam that makes the operand an argument,
so the grep can work at all.**

**⛔ AN ORACLE THAT SHARES ITS SUBJECT IS NOT AN ORACLE (#96,
2026-08-16).** #96 widened lever A's temps-only liveness into an
all-slot one (`jit_slot_liveness`) by making BOTH wrappers over one
`jit_liveness_core` - correct, and it silently destroyed the natural
test. "The new analysis must agree with the old one on every temp"
reads like an independent computation of the same fact; after the
unification a bug in the core is on both sides and CANCELS. Watched:
making an unaudited op contribute NOTHING live - the direction whose
loss is a silent miscompile - left that comparison green. What catches
it is a check derived from the written CONTRACT ("an op
`visit_use_def` does not know leaves every covered slot live-in"),
plus a count that FAILS VACUOUS when no unaudited op appeared.
**The general rule: the refactor that unifies two implementations is
exactly the moment a cross-check between them stops being evidence -
re-derive at least one check from the SPEC.** Same family as "a test
derived from a table can never find a hole in that table".

**⛔ A POOL ORDERED BY PREFERENCE HIDES ITS OWN TAIL - AND THE SIXTH
SHAPE ATE THE TOOL BUILT TO PREVENT IT (#96, 2026-08-17).** r9 was added
to the JIT's caller-saved pin pool on 2026-08-16 and was a **shipping
wrong answer for a day**. The comment that added it claimed "r9 is used
in exactly TWO local scopes and both are already safe". False: r9 is raw
scratch in the CAPTURE ops and in every element tier
(`emit_elem_int_read`, `emit_store_elem_inline`,
`emit_load_elem2_inline`, `emit_store_elem2_inline`, `ForStepElemInt`) -
about 80 sites outside any `emit_call_prologue` bracket. Twelve lines
reproduce it: eight hot int accumulators in a closure loop plus
`s7 += cap` prints **56640** under `-tw`/`-nj` and **88854283473440**
under the JIT, because the entry emits `mov r9, s5` and
`emit_ctx_chain_r9` then walks the ctx through r9.

Three separate failures let that ship, and each earns a rule:

- **THE CENSUS WAS BLIND TO ITS OWN SUBJECT.** `scripts/regcensus.py`
  reported **14** sites for r9; the true figure is ~87. The misses are
  `movabs_r9`, `cmp_r9_rdx`, `lea_rdi`, `slots_to_arg0`,
  `store_elem_byte_dil` - the register is in the **METHOD NAME**, so a
  scan for the operand cannot see it. This is *exactly* the sixth
  audit-table shape recorded above, hit by the tool written to keep the
  arc honest. It now **DERIVES** the accessor set from the source (every
  `void name(` whose name, split on `_`, contains a register token) - so
  a new fixed-pair wrapper is counted the day it is written. **When an
  audit tool and your intuition disagree about a register, suspect the
  tool's blind spot before the intuition.**
- **THE POOL'S PREFERENCE ORDER IS A COVERAGE HOLE.** `take_reg` scans
  the pool in order, so the LAST member is handed out only to a run with
  the maximum pin count. r9 was 4th of 4, so `-rt`, all four
  differentials, `corpus_diff`, and every fuzzer exercised the first
  three heavily and r9 essentially never. A bigger corpus does not fix
  this - the allocator's own preference is what hides the tail.
  **`MYLANG_JIT_XROT=N` rotates the pool** so member N is handed out
  first; `corpus_diff.sh --xrot` runs the matrix, and the
  `jit_xcache_pins` `-rt` case sweeps every rotation in-process. Making
  r9 FIRST fails `-rt` in seconds - which is how it was found.
  **Generalise: when a resource is allocated in a fixed preference
  order, the test net must permute that order.**
- **"SAFE BY THE SAME GATES AS X" IS NOT AN ARGUMENT.** r9 was admitted
  by analogy to r10/r11. A register joins only with its OWN sites
  enumerated against the gates. r8, audited that way, is genuinely safe
  (its raw-scratch use is confined to `emit_sync_push_native`,
  `emit_sync_call_inline` - both blocked by `jit_run_blocks_xcache` -
  and `emit_ret_native`, whose first act is `flush_cache()`); r9 is not.

**⛔ A FRAGMENT RETURN MUST NAME ITS WRITE-BACK CONTRACT (#96,
2026-08-16).** The N5/C2a register cache holds frame slots in registers
between an entry load and an exit flush, so a `ret` that leaves one
there resumes the interpreter on a STALE slot - a silent wrong answer.
`exit_pc` flushes automatically; **`frag_ret()` does not**, and 13 sites
call it directly. Seven of them were relying on an invariant written
nowhere near them: `pick_cached_slots` lists no call opcode, so it hits
`default: return {}` and a run containing a call is not cached. So
`frag_ret` now takes a `RetFlush` - `flushed` (a `flush_cache()` was
emitted on this path), `empty` (**ML_CHECKed**), or `epilogue`
(`emit_epilogues` only, where `exit_pc` already chose per exit). **A new
return site must pick one**, and `empty` is the one that will fail
loudly the day the allocator keeps a value in a register across a call.
Do not "fix" that abort by switching to `flushed` - emit the flush.

**⛔ AND IT BROKE AGAIN, ONE DAY AFTER THE ABS32 MEMORY OPERAND LANDED,
WITH THREE NETS PRESENT AND ALL THREE BLIND (2026-08-26 -> found
2026-08-27).** `mov rax, [+0x41250108]` - the arena global reached in
ONE instruction (#97 step 1a) - printed its RAW ADDRESS. Every other
baked pointer masks to `<addr>`/`<int-tag>`/`<helper>` for exactly one
reason, that two runs and two separately-linked binaries must produce
IDENTICAL text, and the new operand form was added without learning it.

**THIS IS THE MOST INSTRUCTIVE INSTRUMENT FAILURE IN THE REPO, because
the defences existed and each failed DIFFERENTLY:**

1. **`vdjcmp.sh`'s SELF-TEST was correct and refused every comparison**
   from that commit on ("the same binary gave two different dumps for
   samples/gcd"). It was not lying - it was shouting. **Nobody ran it.**
   A self-test that fires only when someone remembers is not a net; it
   is now a CI step (`nets.yml`, the differential job).
2. **`-rt`'s ADDRESS-FREE INVARIANT check - written expressly to make
   this bug impossible - did not fire, because it ENUMERATED WHERE AN
   ADDRESS CAN APPEAR.** It walked `", "` operand separators and
   skipped anything whose first character was not a digit, on the
   reasoning that `[` begins "a register / [mem]". True when written,
   when an address could only be an IMMEDIATE; the new operand put one
   inside the brackets it was skipping. **⛔ THE NINTH AUDIT-TABLE
   SHAPE: a check that enumerates the PLACES a hazard can occur goes
   stale exactly like a table that enumerates the OPCODES that can
   cause it.** It enumerates nothing now - every number anywhere on the
   line, brackets included - with ONE exemption stated as a property
   (a control transfer's operand is a fragment-RELATIVE offset).
3. **Nothing checked the property END TO END.** `-rt` runs in-process,
   so it can only assert the invariant that IMPLIES reproducibility,
   never reproducibility itself. `tests/driver_checks.sh` now spawns
   the binary twice and compares - the same split as every other
   driver-visible property.

**THREE RULES, all of them earned here:**

- **⛔ ADDING AN OPERAND FORM TO THE EMITTER IS A DISASSEMBLER CHANGE.**
  A new addressing mode, a new immediate position, a new prefix: the
  rendering obligation (mask every baked address) attaches to it, and
  no existing check can be assumed to cover a shape it was not written
  against. Run `scripts/vdjcmp.sh BIN BIN` - not only after a change
  you believe is a pure restructuring.
- **⛔ A SELF-TEST THAT IS NOT IN CI IS NOT A NET.** The whole point of
  one is that it fires without being asked. If a tool can check itself,
  that check belongs in `nets.yml`.
- **⛔ WHEN AN INSTRUMENT REFUSES, THAT IS A RESULT.** "The tool errored
  so I moved on" is how a correct refusal becomes a silent regression.
  A refusal is a failing test.

(`MYLANG_VDJ_HEX=1` remains non-reproducible BY CONSTRUCTION - the raw
bytes contain the address - which is why the comparison path does not
use it, and why the driver check compares the plain dump.)

**⛔ `-vdj` IS REPRODUCIBLE - AND THE FOUR YEARS OF WORKAROUNDS THAT
SAY WHY YOU MUST FIX A TOOL, NOT ITS CALLERS (2026-08-17).** Comparing
two binaries' emitted code is a plain `cmp` now
(`scripts/vdjcmp.sh OLD NEW`): baked addresses print as
`<int-tag>`/`<addr>`/`<helper>` - the operand SHAPE, which is all a
reader or a differ needs - with the digits behind `MYLANG_VDJ_ADDRS=1`.

This paragraph used to say the opposite ("comparing needs
normalisation") and prescribed a sed pipeline, ordered just so, because
a libm displacement is 5 or 6 hex digits depending on where the code
page lands. Every word was true and the whole approach was wrong:

 - **the masks rotted silently.** They were hex-only, and #96 step 3
   moved the Type singletons into a low-address arena so a tag encodes
   as an `imm32` - which the disassembler printed in DECIMAL. From that
   day `vdjcmp.sh` reported **0 identical / 108 differing** for ANY
   pair of binaries. It was not an oracle; it was a constant
   "everything changed", and nobody noticed because that is also what a
   broken change looks like;
 - **no regex could ever have been enough.** A mis-decoded instruction
   emerges as bare `.byte 0xe0` / `nop` lines with no maskable token,
   and 31 of 108 programs differed from THEMSELVES that way;
 - **the disassembler was ALSO just wrong**, which the masking hid:
   5543 undecoded bytes corpus-wide, from six missing opcodes and a SIB
   decoder that never consumed its displacement (so `mov rdi, [rsp+8]`
   printed as `mov rdi, [rsp+rsp*8]` and desynchronised the fragment).

**THE RULE: fix the TOOL, not the script that reads it.** A workaround
in one consumer leaves the tool broken for every other consumer - and
for you, reading the dump by eye, which is the consumer that matters
most. `-vdj` now self-reports: a fragment it cannot fully decode prints
`⛔ DUMP IS UNRELIABLE: N undecoded byte(s), M skipped op mark(s)`,
and the `jit: -vdj decodes every emitted form, reproducibly` `-rt`
check fails the day a new emitted instruction form appears.

**⛔ BENCHMARKS ARE NOT FUNCTIONAL TESTS (maintainer-set, 2026-08-05).**
`bench/` measures THROUGHPUT - millions of iterations - and must NEVER
be used as a correctness corpus. A JIT bug shows on iteration 1 or not
at all, so running benches for it buys no coverage and costs minutes:
the first cold-arm stress lane took `-rt` from 2.6s to over TEN
MINUTES purely because the shapes it needed were buried in benchmark
loop counts. Functional tests live in **`tests/functional/`** - tiny,
self-asserting programs that CONSTRUCT the hazard shape on purpose
(a reference parked in the very temp a float chain reuses, a
ref-listed loop counter, each element tier and its decline) rather
than hoping a benchmark happens to contain it. `tests/corpus_diff.sh`
runs those plus `samples/` in **0.2 seconds**, and its `--cold` matrix
in 0.7. Write a new dedicated test for the construct under test; do
not reach for a bench.

**⛔ EMITTER-ONLY CODE LIVES INSIDE `#if ML_JIT_SUPPORTED` (2026-08-05).**
The lever switches below first sat ABOVE the guard and broke three CI
lanes at once, none of them reproducible locally: macOS clang
`unused function 'jit_lever_forced' [-Werror]` (off-platform nothing
calls them) and MSVC `C4996` on their `getenv` (inside the guard it is
never compiled - which is why jit.cpp's OTHER getenv calls are fine).
The same push also hit `std::binary_search` in vm.cpp with no
`#include <algorithm>`: GCC and the local clang pulled it in
transitively, the runners' newer stdlib does not. **Before pushing a
JIT batch, compile the NON-JIT path**: temporarily flip jit.h's
`#if defined(__linux__) && defined(__x86_64__)` to `#if 0`, build with
BOTH g++ and clang++, run `-rt` (JIT mode self-skips; still
1715/1715), restore. And build once with **CMake** - that is what CI
runs, and `make` alone passed all three broken configurations.

**⛔ THE JIT'S TEST NETS - AND THE AXIS THEY ALL MISSED (2026-08-04).**
A JIT register bug needs THREE things to coincide: a PROGRAM SHAPE
(which temps are shared between reference-producing code and the hot
chain - slot allocation), a RUNTIME STATE (that slot actually holding a
reference when the guarded op runs, so the cold arm is taken), and an
EMITTER DECISION (which register the value is in - forwarding armed?
result in xmm0 or xmm1? pinned?). Measured against that: `-rt` varies
only the first, weakly (hand-written tests have few locals, so their
temps rarely collide); `tests/nested_fuzz.py` varies control flow but
is **INT-ONLY** by construction - it maintains a CPython twin, which
forbids int `/`, negative `%`, >64-bit ints and dict iteration - so it
can never build a reference/float temp collision; the 5-mode
differential is an excellent ORACLE over those same programs. Nothing
varied axes 2 or 3. That is why TWO bugs in one day were green
everywhere and both died instantly on a bench program (a real `main`
reuses its low temps for argv/print AND for the hot loop - exactly the
collision). Three nets now:
- **`tests/corpus_diff.sh`** - tree-walker vs the default engine over
  `tests/functional/` + samples (14 programs, 0.2s). RUN IT with -rt,
  rel-hard, clang and the fuzzer on any JIT/register batch.
- **`MYLANG_JIT_COLD=tier[,...]`** - force a guarded tier's DECLINE arm
  so a path normally reached only by a runtime coincidence becomes the
  ONLY path (the RECYCLE=1 philosophy for the JIT). `refstore` forces
  the ref-listed scalar store's release-helper arm. On its FIRST run it
  found a real latent bug in 0.7s: `store_dst`'s cold arm gated its RAX
  reload on `keep_rax`, but `ForLoopStep`/`IntAddStep` store the
  counter and then `cmp rax, <bound>` - so a ref-listed counter taking
  that arm compared garbage and the loop ran the wrong number of times.
  The reload is unconditional now (it costs the hot path nothing).
  Opt in PER CALL SITE via emit_ref_check's `cold` parameter - a
  first version overrode the shared helper for every caller at once,
  which is the wrong granularity for a knob meant to isolate one tier.
- **⛔ LEVER A FORWARDS LOCALS TOO, AND THE TEMP RULE MOVED (#96,
  2026-08-16).** Lever A's arming test used to require a TEMP
  destination. That restriction belongs to the WRITE ELISION - only a
  temp's liveness proves the slot dead - not to the READ, and eliding
  just the read still removes a real instruction: on a memory-backed
  local it is a store-to-load-forwarding stall ON the dependency chain
  (`mov a4, rax; mov rax, a4; sar rax, 7`). The test now sits inside
  `skip_write`, which is ALSO where `tb = fdst - slot_count` stops
  being negative - **`1 << tb` for a local is UB**, and that is the
  trap in doing this the quick way. Measured -8.86% Ir / 0.89x on
  83_regs_int_40, 28 corpus programs changed, none regressed.
- **THE LEVER SWITCHES** (`MYLANG_JIT_OFF=lever[,...]`, jit.cpp): the
  JIT analogue of `--no-opt`, which CLAUDE.md already mandates for AST
  transforms for the same reason (`-nj` is all-or-nothing and localises
  nothing). Levers: cache, fcache, telide, fread, flit, fwd, ffwd,
  resreg, hoist, hoist2, mfact, cest, relent, norec (the no-record
  call tier, DEFAULT ON since 2026-08-12 - its TESTS nets self-adjust
  per mode, so `-rt` is green with it on OR off), argfuse (#162: a
  reference argument already in a named local is bound STRAIGHT from
  that slot and its staging MoveV is not emitted; the cold arms
  materialise the run), xcache (#96: the CALLER-saved pin
  extension - r8/r10/r11 hold up to three more hot locals,
  spilled/reloaded around every helper call by
  emit_call_prologue/epilogue. **Which members a run may spend is a
  per-register CLOBBER MASK, `jit_xcache_clobber` - not a boolean.**
  It was a boolean until 2026-08-18, and the boolean cost a register
  for nothing: one C1 hoist region denied the WHOLE pool, though the
  hoist claims only r10/r11, and an element read on a loop-invariant
  base is exactly what CREATES a region - so "walks an array in a
  loop" and "may pin a caller-saved register" were mutually exclusive,
  0 of 20 hoist runs corpus-wide having a register left. Each
  contributor now names its own: hoist -> {r10,r11}, a MyLang call ->
  the whole pool; a type singleton still in a register is a B1 GRANT
  claimed as ra.busy, not a clobber entry - Emitter::grant_tag_regs
  decides it once per run and tag_holder() is the one query.
  **⛔ A DENY IS ABOUT SPENDING, NOT OCCUPANCY (#97, 2026-08-26):**
  it says this fragment may not HOLD a value there across a helper
  call, which is what a PIN does - it does NOT say the register is
  in use. So `alloc_scratch(..., transient=true)` ignores `denied`
  for a grant that lives and dies inside ONE op (acc_take, which
  always ignored it, is the precedent), while `busy` - a hoist
  region's claim, a tag grant - still excludes. Getting that
  backwards is not a wrong answer, it is a SILENT STARVATION: a run
  containing a MyLang call denies the whole pool, so every inline
  tier asking for scratch in exactly the shape it exists for
  declined to its helper and the counter read zero),
  bakecallee (#97 step 4: a call to a WRITE-ONCE global slot has a
  callee the emitter can NAME - `jit_baked_callee` reads it out of
  `JitCtx::slot_desc`, the same map `callv_native_ok` has always
  used - so the five descriptor gates, the window SIZE, the
  record-less fork and each proven-scalar argument's reference test
  become COMPILE-TIME constants and one identity compare is what
  survives. **Its FORCE half is not decoration**: the write-once
  gate is about PROFITABILITY (a reassigned slot fails the compare
  on every call and loses the cache that would have hit), so the
  compare is unreachable under our own compilation and deleting it
  leaves every net green - `MYLANG_JIT_FORCE=bakecallee` lifts the
  cost gate alone and makes it fail in `Frame::at`),
  `all`.
  `tests/corpus_diff.sh BIN --levers`
  runs the whole matrix. NOTE a lever-off config FAILS `-rt` by
  design - the coverage tests assert their own lever ran - so the
  matrix runs against the CORPUS, not the suite.
- **⛔ `MYLANG_NO_LOWMEM=1` - REFUSE THE LOW-ADDRESS ARENA (2026-08-18).**
  The JIT names `t_int`/`t_float`/`t_bool` with a sign-extended `imm32`
  only while they live below 2^31 (`lowmem.h`); everywhere else -
  Darwin, Windows, a hardened kernel, an exhausted low 2GB, **a failed
  `MAP_32BIT` on an ordinary Linux box** - every tag store and tag
  compare falls back to a REGISTER form instead. That is materially
  different emitted code, it ships, and **no build and no CI lane could
  enter it**, because the arena is decided by an `mmap` at static init
  with no switch. This is the `LTO=0` shape exactly: a configuration
  only one platform can take is a configuration nobody tests.
  Its FIRST run found a shipped wrong answer. `store_dst_bool` wrote
  the `t_bool` tag as `store_type_tag(a.type, t_bool, RCX)` - and when
  #96 step 3 made tags immediates, the `movabs RCX, t_bool` that made
  that call TRUE was deleted as "an instruction saved". Nothing else
  ever loads RCX with a tag, so on the fallback path every bool store
  wrote whatever RCX happened to hold **as the slot's type pointer**:
  7 corpus programs crashed or answered wrongly, `-rt` was 1922/1922
  and plain `corpus_diff` 20/20 (both watched). The seam is split now -
  `store_type_tag(disp, tag, held_reg)` ML_CHECKs that the tag is one
  of the two something actually materialises, and
  `store_type_tag_via(disp, tag, scratch)` BUILDS the value for any
  other - so the next tag cannot repeat it.
  **The general rule: an optimization that makes a register
  UNNECESSARY must not leave an argument behind claiming it is still
  LOADED.** Delete the parameter or honour it.
  **NET: the `nolowmem` JOB** (`.github/workflows/nets.yml`) - `-rt`
  off-arena, `corpus_diff.sh --nolowmem` and `driver_checks.sh`, over
  TWO builds because they catch different things, MEASURED by
  reintroducing the defect: **debug+ASan/UBSan ABORTS (rc=134)** with a
  located report, **release `ASSERTS=OFF` SEGFAULTS (rc=139)** - the
  raw crash a user gets once the ML_CHECK net is compiled away, in the
  configuration a shipped program actually runs. It opens with a
  VACUITY GUARD asserting `mylang -v` reports `lowmem 1` by default and
  `lowmem 0` under the env, in both directions: a lane that cannot
  prove it is in the configuration it tests goes green doing nothing
  the day a default moves (the `lto0` lane's reason for asserting
  `lto 0` first).
- **`MYLANG_JIT_XROT=N` - ROTATE the caller-saved pin pool** so member
  N is handed out FIRST (`tests/corpus_diff.sh BIN --xrot` runs the
  matrix; `g_jit_xrot` is settable in-process, and `jit_xcache_pins`
  sweeps every rotation). `take_reg` scans the pool in PREFERENCE
  order, so its last member is reached only by a run with the maximum
  pin count - which is how an UNSAFE register (r9) sat in the pool for
  a day, exercised by no net in the project, as a wrong answer. Unlike
  a lever switch this is not an A/B: every rotation must produce the
  SAME output, so it is a pure differential axis.
- **`MYLANG_JIT_LSRA` - the D3 REGISTER-ALLOCATOR lever, ⛔ DEFAULT
  ON since 2026-08-26** (`g_jit_lsra`, settable in-process;
  `MYLANG_JIT_LSRA=0` is the debugging OPT-OUT). ON means the linear
  scan (`jit_lsra_assign` + snap/transitions + the float twin)
  chooses the register plan; OFF restores the legacy pick end to
  end, so a suspected allocator bug has a same-binary A/B one env
  var away. Flipped on the completed two-axis ledger: Ir zero
  per-iteration regressions corpus-wide (wins to -16.8%), wall
  geomean 0.997x over 87 (wins to 0.73x). The opt-out config stays
  covered: -rt/corpus run it in the flip battery, and the coverage
  tests that pin the PICK's mechanisms force g_jit_lsra = false for
  their duration (the documented convention). ⛔ TWO RULES
  it already earned: GP admission needs uses_int > 0 - an int-op
  touch is the TYPE evidence the t_int flush relies on; a ReturnV
  read is weight, never evidence (a ret-only closure slot pinned on
  its ReturnV was flushed t_int over t_func - NotCallableEx + a leak;
  the pick's >= 3 threshold had been carrying this rule silently).
  And a coverage test that pins the DEFAULT allocator's mechanism
  (jit_xcache_pins, jit_hoist_pair_conflict) must force
  g_jit_lsra = false for its duration, or the lever-on suite starves
  its crafted shape.
- **`MYLANG_JIT_FORCE=lever[,...]`** - ignore a lever's COST heuristic
  but NOT its soundness guards, so it engages in shapes it would
  normally decline as unprofitable. This is the half with real catch
  power: it tests a gate's CORRECTNESS independently of its
  PROFITABILITY. `FORCE=flit` is how C4b's "correctness lives in
  emit_call_epilogue, not the gate" claim is checked.
- **`tests/norec_coverage.py` - the NO-RECORD COVERAGE RATCHET (Net 4,
  built 2026-08-13).** Reads gcov's JSON from the existing `-DGCOV=1`
  lane and reports LINE + BRANCH coverage of the tier's walk /
  reconstruction / verification surface, function by function (the
  `SCOPE` list in the script — **a function missing from it is
  silently ungated**, so add yours when you add one). Exemptions live
  in the SOURCE, as a trailing `/* NOREC-COV-EXEMPT: reason */`, so
  they cannot rot when line numbers shift and the next editor sees the
  claim they must keep true; a marker that is no longer needed is
  reported STALE. gcc's exception edges are excluded by default
  (`--with-throw` to look).
  **The number that justifies the whole thing: a plain `./mylang -rt`
  leaves `norec_walk_chain` at ZERO** — it is gated `!jit_norec_on()`,
  so the project's primary shadow oracle only runs in SHADOW mode,
  which `-rt` does not select. `--run` drives a workload that does
  (corpus_diff `--levers`, Net 3, Net 2, and the deep-switch program),
  taking the surface from 54.8%/47.0% to 77.4%/62.6%.
  **The 100% goal (`--gate`) is NOT met** — see the Net 4 entry in
  `docs/jit-optimizations.md` for what is left and why. CI pins the
  current floor instead (`--min-lines`/`--min-branches`), so the
  surface can only improve.
- **`tests/norec_enum.py` - the EXHAUSTIVE SMALL-SCOPE ENUMERATION
  (Net 3, built 2026-08-13). NOT a fuzzer:** it emits EVERY program in
  a bounded shape space - depth 1-4 x per-level frame kind
  {plain, try, try/finally, dict-iter} x terminal {int, float, throw}
  x the level a throw is caught at (or uncaught) - and runs each
  through **four** engine configurations (`-tw`, `-nj`, jit,
  `MYLANG_JIT_OFF=norec`), comparing stdout, stderr and exit status
  BYTE-FOR-BYTE. RULE 2 is the spec, and an uncaught throw's rendered
  BACKTRACE is the hard consumer - it is built from the reconstructed
  frames. 2272 programs / 9088 runs at depth 4, ~1.3 min.
  It reports TIER REACH from a sample, so a space that never engages
  the tier says so instead of printing a green zero.
  **It found two real bugs on its first run** - the mixed-kind ret
  audit abort, and the catch-bind ref_slots gap below - neither of
  which `-rt` or `corpus_diff` caught.
  Two axes are deliberately SUBSTITUTED rather than dropped (recorded
  in the script): "cached-call" as a frame kind is unreachable in an
  impure chain by construction, and there is no builtin that captures
  a backtrace without throwing, so capture is covered by every throw
  variant and rendering by the uncaught ones.
  **The three cases the depth bound does NOT cover are enumerated
  explicitly instead** (they are reached by DEPTH, not shape, so the
  generator cannot make them) and are all covered by the
  `norec_segment_boundary` `-rt` check: 12000 levels of mutual
  recursion that throws at the bottom and catches at the top. That
  crosses TWO `SEG_SLOTS` boundaries in every lane and drives the
  native stack to 12001 on the non-sanitized ones (vs the cap of 32
  under ASan, where `ML_NSTACK_OFF` is set). `g_vm_seg_advance` and
  `g_jit_sync_depth_max` (TESTS-only, in `MYLANG_JITSTATS`) make it
  measurable - depth leaves no other trace, and the check ASSERTS the
  boundary was crossed so it cannot decay into a merely-deep test.
  It is an `extra_check`, not a `tests` entry, and that is FORCED: the
  differential reruns `tests` entries in the TREE-WALKER, which
  recurses on the C stack and overflows at this depth.
- **`MYLANG_RECON_AT=N` + `tests/norec_sweep.py` - the NO-RECORD
  tier's DETERMINISTIC EVENT SWEEP (Net 2, built 2026-08-13).** The
  G1 tier does not write a call record it can REBUILD later, and the
  rebuild runs only where an exception happens to fall - so its
  correctness was tested wherever a corpus throws, and nowhere else.
  `MYLANG_RECON_AT=N` forces a reconstruction at the Nth CALL EVENT
  instead; the driver walks N over a program's whole event count, one
  process per N, in BOTH `MYLANG_JIT_OFF=norec` (records still
  written, so the rebuild is compared field-for-field - the oracle)
  and the shipping config (record-less, where only the record-free
  half can be asserted, but which is the mode that ships AND the only
  mode that walks the real mixed chain - note the Net 1 walk is gated
  `!jit_norec_on()`, so nothing else traverses it there). Every sweep
  run's stdout/exit must match a probe-free baseline, so a probe that
  PERTURBS a program fails too. **Why it earns its keep, watched
  failing:** corrupting the emitted push's `seg_top_before` store by
  one leaves `-rt` GREEN at 1907/1907, `corpus_diff` green, and the
  program printing the right answer, while the sweep fails at N=1 -
  the slot stack leaks a slot per call and nothing else in the tree
  looks. The in-suite seed is the `Net 2` `-rt` entry (a few forced
  events, asserting frames were actually walked); the full sweep is
  the script, ~5.5 min over the default corpus.
A fourth - forcing a guarded tier's DECLINE arm so a rare cold path
becomes the only path (the RECYCLE=1 philosophy for the JIT) - is
DESIGNED and NOT BUILT: a first attempt overrode the SHARED
`emit_ref_check`, whose call sites have different fall-through
expectations, and 43_sieve raised OutOfBounds; it needs per-CALL-SITE
opt-in plus a bounded lane (forcing every scalar store through a helper
took `-rt` from 2.6s to over 10 minutes).

**⛔ THE GUARD-ELISION FAMILY IS AN INSTRUCTION-COUNT WIN WITH A
WALL-CLOCK CEILING NEAR ZERO (measured 2026-08-05).** The
specialized-family table fix above is the controlled experiment: it
deleted guards worth **-15% to -32% Ir on six benches** and its
wall-clock is **FLAT** (best-of-9 at table scale: 06_if_branch 1.005x,
03_int_arith 0.995x, 54_mandelbrot 0.983x, 64_struct_create 0.963x;
suite `cur/base` 1.006x, inside the run-to-run spread). The reason is
what the guards ARE: a perfectly-predicted branch over two L1-hitting
loads, with no dependency on the store's data - they retire nearly free
alongside the real work on a wide out-of-order core. This is the
documented instruction-vs-time divergence at its extreme, and it is the
measured argument that killed the first-iteration PEEL (see
plans/archived/typed-invariant-arrays.md: its reach across bench/ is
ZERO guards, and even with reach it would trade real I-cache for free
instructions).
C4d/C4e/C5 stay - they cost nothing and shrink the emitted code - but
**do not push this line further on Ir evidence alone.**

**VM dispatch: `CGOTO` (default 1).** On GCC/clang the VM's dispatch loop
(`vm_run_chunk`) is COMPUTED-GOTO (direct-threaded): a static table of
code-label addresses generated in enum order from `ML_FOR_EACH_OPCODE`
(bytecode.h, order/coverage static-asserted) and a `goto *tbl[op]` at the
tail of every handler (`VM_NEXT`), so each opcode gets its OWN indirect
branch instead of one 92-target switch hub. `make CGOTO=0` (CMake
`-DCGOTO=OFF`) forces the portable `switch` dispatch - the A/B lever, and
what MSVC always compiles (no computed goto there). The SAME handler bodies
compile either way via `VM_CASE`/`VM_NEXT`/`VM_NEXT_COLD` (see vm.cpp's
"DISPATCH MODE" comment; the _COLD variant is a direct-goto trampoline for
the cross-frame exception sites - clang forbids an INDIRECT goto from
exiting a scope with live destructors, which is also why each handler's
terminal dispatch sits AFTER its case braces). Measured (release,
ASSERTS=0, scale 10, best-of-7): ~10% geomean on the dispatch-bound loop
benches (up to -22% on 44_primes_sqrt/07_nested_loops), suite my/py
0.26x -> 0.25x, VM/TW 0.56x -> 0.53x; cachegrind -11-12% instructions and
-25-42% indirect-branch mispredicts on untouched loops.

CMake is also supported (used by CI):
`cmake -DTESTS=1 -DCMAKE_BUILD_TYPE=Debug .. && cmake --build .`
(`-DGCOV=ON` for a coverage build, GCC only). It enables LTO via
`INTERPROCEDURAL_OPTIMIZATION` for the `Release`/`RelWithDebInfo` configs (so
Debug and coverage builds are untouched), portable across GCC/clang/MSVC;
configure with `-DLTO=OFF` to disable. The same `ASAN`/`UBSAN` options exist
(`-DASAN=ON/OFF`), defaulting **on for a `Debug` build** and off otherwise — the
analog of `OPT=0` vs `OPT=1` — and are applied on GCC/clang only (skipped on
MSVC). CMake now also passes `-fwrapv` (non-MSVC), matching the Makefile so the
relied-upon signed wraparound is defined in CMake builds too. It also has the
**`-DASSERTS=ON/OFF`** (default ON; OFF defines `NDEBUG`, and since CMake's
optimized configs add `-DNDEBUG` themselves, ASSERTS=ON appends `-UNDEBUG` to
keep asserts in Release) and **`-DRECYCLE=ON/OFF`** (default OFF) options,
mirroring the Makefile. It ALSO now sets `-Wall -Wextra -Wno-unused-parameter`
for GCC/clang (it set no warning flags before) and **`-DWERROR=ON/OFF`**
(default ON) — warnings-as-errors (`-Werror` / MSVC `/WX`), matching the
Makefile's `WERROR`. CI (`.github/workflows/`) builds Debug+Release ×
g+++clang on Linux (plus a `RECYCLE=ON` lane and an **`lto0` lane** — see the
non-LTO note above), macOS (with libc++ hardening),
and Windows, and runs `./mylang -rt` — correctness only, no timing, so the
lanes carry as many checks as possible.

**THE `Nets` LANE (`.github/workflows/nets.yml`, added 2026-08-13) runs
everything `-rt` is BLIND TO.** Until it existed CI ran `-rt`,
`driver_checks.sh` and `myv_doc_check.py` and nothing else — the
differential corpus and all four fuzzers only ran when someone
remembered to, by hand. That gap was not theoretical: `corpus_diff`
catches a JIT abort (a mixed-kind call chain tripping `jit_ret_audit`)
that `-rt` passes straight through, and the Net 3 enumeration catches a
`ref_slots` leak that BOTH miss. Four jobs, in parallel with the fast
lanes so an `-rt` failure still reports quickly:
- **differential** (Debug, ASan+UBSan+hardening): `corpus_diff.sh`
  plain AND `--levers`, `norec_enum.py --depth 3`,
  `norec_sweep.py`, `nested_fuzz.py`;
- **myv-fuzz** on BOTH a Debug/ASan and an `ASSERTS=OFF` Release build,
  because those catch different things (a memory error vs. a check the
  debug build was relying on being compiled away). Findings are
  uploaded as artifacts — a `.myv` finding cannot be regenerated from
  a seed, since an image embeds its source path;
- **repl-fuzz** under `RECYCLE=ON` + ASan, the combination this file
  names for the REPL's retained-AST/stale-node class;
- **coverage-gate** — Net 4's ratchet (below).

Running scripts:
```
./build/mylang FILE              # run a script; extra args become argv
./build/mylang -e 'EXPR'         # run inline source (-e args concatenated)
./build/mylang -s FILE           # dump syntax tree (const-folded), then ALSO
                                 # the post-optimizer tree (inline/unroll), run
./build/mylang -t FILE           # dump tokens
./build/mylang -nc FILE          # disable const-eval (compare -s with/without)
./build/mylang -ni FILE          # disable function inlining (debug)
./build/mylang -npc FILE         # disable the per-frame pure-call cache
                                 # (recursion still unrolls; for measurement)
./build/mylang --no-opt LIST F   # disable AST transforms (comma-separated:
                                 # licm, slice-hoist, for-range, typed, all).
                                 # The same-binary A/B lever for a pass that
                                 # rewrites the tree - see "Testing an AST
                                 # TRANSFORM": the engine differential cannot
                                 # see a bug in one, so the only oracle is the
                                 # same program with the pass off
./build/mylang --strict FILE     # require every NON-LOCAL to be DECLARED
                                 # above its first use (step 7's opt-in half,
                                 # FIX-2's original shape). Off by default: it
                                 # refuses programs that are CORRECT today -
                                 # "helpers at the top, configuration at the
                                 # bottom" - so it is the place for rules too
                                 # aggressive to impose on everyone. What it
                                 # buys is that UnboundSymbolEx becomes
                                 # UNREACHABLE. FUNC/STRUCT names are exempt
                                 # (they bind at SCOPE ENTRY, #134, so a
                                 # forward call is not a forward reference and
                                 # requiring order would forbid mutual
                                 # recursion), as is a LAZY builtin's argument
                                 # (`isbound(g)` is how a program COPES with
                                 # an order it cannot fix). Measured: 0 of the
                                 # 95 corpus programs trip it
                                 #
                                 # FUTURE (maintainer, 2026-08-09 - agreed as
                                 # an idea, NOT scheduled): make `--strict`
                                 # stronger by requiring define-before-use for
                                 # FUNCTIONS and STRUCTS too, which needs
                                 # FORWARD DECLARATIONS (`func f(x);` with no
                                 # body) to be added to the language first.
                                 # Without them the rule is not compliable:
                                 # mutual recursion is a CYCLE and a linear
                                 # file cannot linearise one, so whichever of
                                 # `ev`/`od` is written second leaves the other
                                 # pointing forward. The motivation would be
                                 # top-down readability discipline (C's rule),
                                 # NOT safety - a func/struct name binds at
                                 # SCOPE ENTRY (#134), so a forward reference
                                 # to one can never raise UnboundSymbolEx and
                                 # ordering it prevents nothing. Note MyLang
                                 # already has ONE such requirement, arrived at
                                 # for a parsing reason: a struct used as a
                                 # TYPE annotation (`P p;`) must be declared
                                 # first, because the parser resolves the type
                                 # name to tell a declaration from an
                                 # expression - while `P(1)` and `P.CONST`
                                 # work forward. Before building it, MEASURE
                                 # how many corpus programs call a function
                                 # declared below them; that number says
                                 # whether the discipline is paid for
./build/mylang -it N FILE        # inline threshold: max inlined body (nodes)
./build/mylang -v                # how THIS binary was built (opt, asserts,
                                 # lto, sanitizers, vm_hardening, cgoto,
                                 # tests, recycle, jit, compiler) - one
                                 # `key value` line each, read from
                                 # COMPILER-SET macros (`__OPTIMIZE__`,
                                 # `NDEBUG`, ...) so it cannot drift from
                                 # what was compiled. bench/run.py parses
                                 # it; see the ASSERTS=0 rule below
./build/mylang -nr FILE          # COMPILE and validate, don't run: lex,
                                 # parse, infer, AND run_optimizers (#147,
                                 # 2026-08-09 - it used to stop before the
                                 # optimizers, so every diagnostic that lives
                                 # in resolve_names was invisible to it: the
                                 # step 7 prover, the whole WARNING tier,
                                 # FIX-1, the TDZ, the duplicate-decl check.
                                 # `-nr` exited 0 in silence on a program a
                                 # plain run REFUSES, which is backwards for
                                 # the flag a CI job reaches for. It is no
                                 # longer a cheap parse - it is the whole
                                 # compile minus execution.) Exit 1 on a
                                 # refusal, 0 with warnings on stderr
./build/mylang -vm FILE          # execute via the bytecode VM — the
                                 # DEFAULT engine (flipped 2026-07-18);
                                 # kept for pre-flip scripts/CI
./build/mylang -tw FILE          # execute via the TREE-WALKER instead (the
                                 # pre-flip default; still the const-eval +
                                 # REPL engine, and the differential oracle)
./build/mylang -vd FILE          # dump the VM bytecode disassembly (the
                                 # bytecode analogue of -s), exit — disasm.h;
                                 # dumps 100% of the serializable image: the
                                 # custom TYPES (struct defs), every chunk +
                                 # its POOLS (consts/catch_types/…/side tables);
                                 # closures + their capture struct shown, 256-
                                 # color syntax-highlighted on a TTY
./build/mylang -nbi FILE         # disable the bytecode-level INLINER (the
                                 # "splice": a callee's BYTECODE pasted into
                                 # the caller's chunk, so the call protocol
                                 # disappears). DEFAULT ON since 2026-08-02;
                                 # -bi forces it on, MYLANG_BCINLINE=0/1.
                                 # The same-binary A/B lever - the
                                 # un-inlined bytecode is a splice's only
                                 # oracle (plans/bytecode-inliner.md)
./build/mylang -vdj FILE         # -vd + the native x86-64 disassembly of
                                 # each JIT fragment, interleaved under its
                                 # `enter.nat` line with `; vm pc N` markers.
                                 # REPRODUCIBLE: a baked address prints as
                                 # <int-tag>/<addr>/<helper>, so two runs
                                 # and two separately-linked binaries give
                                 # byte-identical output and `cmp` is the
                                 # whole comparison (scripts/vdjcmp.sh).
                                 # MYLANG_VDJ_ADDRS=1 shows the numbers
                                 # when you need one specific pointer.
                                 # It SELF-REPORTS: a fragment it cannot
                                 # fully decode prints "DUMP IS UNRELIABLE"
                                 # with the undecoded-byte and skipped-mark
                                 # counts, instead of quietly printing
                                 # confident wrong mnemonics (it did that
                                 # for 5543 bytes until 2026-08-17)
./build/mylang -nti FILE         # disable static type inference / checking
./build/mylang -dti FILE   # dump every identifier's inferred type + uses
./build/mylang -a FILE           # analyze: source colored by optimization
./build/mylang -a --no-color F   # same, plain (for piping / diffing)
./build/mylang -T CATS FILE      # trace the compiler's reasoning to stderr
./build/mylang --trace all FILE  # CATS: infer,inline,specialize,template,
                                 # autoconst,autopure,arrays,fold, or all
```
`-T`/`--trace` enables diagnostic trace categories (see `trace.{h,cpp}` and the
REPL `:trace`) BEFORE compilation, so a script run narrates each optimizer
decision to stderr (colored on a stderr TTY). The same categories drive the
`trace()`/`traceoff()`/`tracing()` builtins and the REPL `:trace [<cat>...]
on|off`. OFF by default — zero cost on a normal run (one mask test per guarded
site).
`-dti` runs inference (non-strict) and prints one tab-separated `ti`
record per declared identifier — `name, kind (var|const|param|func), line, col,
const, type, uses(line:col,...)` — then exits without running. It is the audit
tool for the mandatory-`dyn` / type-driven work (see
`plans/archived/type-driven-specialization.md`): used to find identifiers inferred `dyn`
/ `array<dyn>` and decide whether each is justified (annotate with `dyn`) or an
inference gap (fix the inferencer).

`-a` / `--analyze` reprints the source **verbatim** with ANSI colors marking
where each compile-time optimization fired (a legend header is printed; exits
without running; `--no-color` for plain). It is **non-strict** (analyzes code
that a normal run would reject for a bare `dyn`). The legend: **yellow** =
became const-like automatically (an auto-const `var` at decl + folded uses, an
un-mutated parameter, an auto-`pure` function); **green** = a flat (unboxed)
`array<int>`/`array<float>` **or** the `for` keyword of a counted loop that
specialized to a `ForRangeStmt` (the two never collide — one lands on an
identifier, the other on the `for` keyword); **red** = an `array<dyn>`;
**blue** = an inlined call (expression-body or tail); **cyan** = a call
redirected to a `name$sN` specialization clone; **magenta** = a call folded to a
literal at compile time (pure/auto-pure/const-builtin with const args);
**dim** = dead code the optimizer eliminated (a const-condition branch /
`while (false)`). Precedence: call-site (blue/cyan/magenta) > array storage
(green/red) > yellow; a dead range dims regardless. Implementation: a
`Loc`-keyed `AnalysisInfo` (`analyzer.h`) populated by the passes only when
threaded in — array colors from a non-strict inference pass
(`collect_array_analysis`), auto-pure/param from a post-resolve walk
(`collect_resolver_analysis`), the counted-`for` mark from `specialize_types`
(it records `counted_for` for each `for` it rewrites — the analyze pipeline now
runs it last, as `run_optimizers` does, gated on a non-null `AnalysisInfo *` so
a normal run records nothing), and the mutation-time decisions (auto-const vars,
dead code, inlined/specialized/folded) recorded *as they happen* by the parser
(`ParseContext::analysis`), AutoConst, and the Inliner; `mylang.cpp` renders.
Unlike `-s`/`-dti`, the analyze rendering now has a headless `-rt` test
(`analyze:`, via `analyze_and_render` with color on).
`-s` / `-nc` are the two indispensable debugging tools: `-s` shows you exactly
what survived
const-folding (and, after the optimizer runs, a second **"Optimized syntax
tree"** dump showing the post-inline/unroll/specialize AST — the actual node
shapes, e.g. an `InlinedCall(Block(...))`), and `-nc` lets you see the tree as
written before folding. Reach for them whenever behavior surprises you.

## Tests

```
./build/mylang -rt               # run the whole suite (needs a TESTS=1 build)
./build/mylang -rt -s            # same, but dump the tree of a failing test
./build/mylang --weights         # inlining cost-model calibration (any build;
                                 # use OPT=1 ASSERTS=0 for meaningful numbers)
```

**`--weights` — the inlining cost-model calibration** (`run_weight_bench`,
eval.cpp). Measures the per-node-type eval cost of the tree-walker by building
the AST nodes **by hand in C++** (never parsed, so no fold/inline/specialize can
perturb the measured nodes or the loop count) and timing each in a tight C++
loop; per-node marginal cost is isolated by subtracting child-subtree costs and
printed relative to a slot read. The **CALL** weight is the reference for the
inliner's benefit function (inline a body when the sum of its node weights is
below the call weight — see `plans/archived/function-inlining.md`). Re-run when
the interpreter changes; the weights are tree-walker-specific but the benefit
function is not. Current (`OPT=1 ASSERTS=0`): id/lit/add/cmp ≈ 1,
return ≈ 3, if ≈ 7, assign ≈ 11, **CALL ≈ 21** (×id-read).

**⛔ `-rt` RUNS IN-PROCESS, SO IT CANNOT SEE THE CLI DRIVER (#147,
2026-08-09).** It calls lexer/parser/infer/resolve directly and never goes
through `mylang.cpp`'s argument handling — so a FLAG wired to the wrong side
of an `if` is invisible to every one of its tests. `-nr` ("compile and
validate, don't run") called `run_optimizers` only when it was going to
*run*, which silently skipped the step 7 prover, the whole warning tier,
FIX-1, the TDZ and the duplicate-decl check: `mylang -nr prog.my` exited 0
in silence on a program `mylang prog.my` refuses. The whole suite was green
throughout, and stays green when the bug is reintroduced. **`tests/
driver_checks.sh` (POSIX sh, in CI) is the net** — spawn the binary and
assert the flag's behaviour; reverting the wiring fails 7 of its 9 checks.
**Add a case there when you add or change a CLI flag**, because no `-rt`
entry can cover one.

Tests are **not** a separate framework — they are entries in the `tests` table
(a `static const std::vector<test>`) in `src/tests.cpp`. Each entry is a tuple:
a name, a list of source-line strings, and an optional `&typeid(ExpectedEx)`.
`check()` lexes+parses+evals the joined source lines. A test **passes** if it
throws nothing — or, when an expected exception type is given, throws *exactly*
that type (compared via `&typeid(e) != t.ex`). There is no single-test CLI
selector; `-rt` runs all of them and `exit(1)`s if any fail. Add a test by
appending an entry to that table (no registration needed). Note the expected
exception is matched against the *static* C++ type; a user-level
`throw <struct>` always surfaces as `ExceptionObject` (a.k.a.
`DynamicExceptionEx`), not a distinct C++ type.

**THE 3-WAY DIFFERENTIAL (maintainer-set, 2026-08-02): the `tests` list
runs in EVERY EXECUTION MODE, but is COUNTED ONCE.** The headline
`Tests passed: N/total` is unchanged (`total = tests + extra_checks +
repl_tests`, the tree-walker pass). After it, `run_tests` reruns the
**same** list — not a new set, so not added to the headline — in each
mode, printing one `Differential (same K tests) - <mode>: M/K` line per
mode. ALL must be green to `exit(0)`:

1. **the AST tree-walker** — the reference. It has NO JIT path: the only
   pipeline is AST → bytecode → native, so this is a JIT-free oracle by
   construction.
2. **the bytecode VM, JIT OFF** (`vm:`) — pure interpreted bytecode.
3. **the bytecode VM, JIT ON** (`jit:`) — native code; the script default.
4. **JIT OFF, splice ON** (`vm:`).
5. **JIT ON, splice ON** (`jit:`) — **the script DEFAULT**.

Modes 1-3 run with the splice OFF (`-nbi`) and 4-5 with it ON. BOTH SHIP
— the bytecode splice went default-ON on 2026-08-02 and `-nbi` turns it
off — so each is a configuration a user can select and neither may be the
untested one. ("The configuration nobody runs" is exactly how a disabled
branch remap survived 39 commits.) Two passes per splice state, not one:
a splice bug can live in the bytecode it PRODUCES (visible with the JIT
off) or in how the JIT CONSUMES it (visible only with it on). The full
suite is green in all five; `-rt` costs ~17s in the debug lane.

**WHY MODE 2 IS NOT OPTIONAL.** With only tw-vs-VM, a codegen bug and a
JIT bug are the SAME SYMPTOM, and a JIT bug that happens to be
unobservable in the default corpus is invisible — which is exactly how a
silently-disabled branch remap survived 39 commits (see the switch
fall-through note under *Invariants & hazards*). Splitting the VM into
with- and without-native says WHICH LAYER broke: PROVEN by injecting a
JIT-only fault (an off-by-one in the loc remap), which fails mode 3
`1483/1486` while mode 2 stays `1486/1486`.

**Mode 3 runs only where `ML_JIT_SUPPORTED`** (jit.h — Linux x86-64
today); elsewhere mode 2 IS what the VM does, so a third pass would prove
nothing, and the summary says so. `check()` dispatches its final run on
`g_exec_engine`, and the harness sets `g_jit_enabled` per mode
(save/restored). A new functional test therefore auto-covers all three
modes with no extra work.

## Benchmarks

> ## ⛔⛔ BEFORE YOU RUN ANYTHING HERE — RULE B1 ⛔⛔
>
> **1. `rm -rf build`** (the maintainer's binary; deleting it before a
> perf run is his 2026-08-12 instruction — it makes a forgotten flag
> FAIL instead of silently timing his program).
> **2. `--mylang build-claude/<lane>/mylang` EXPLICITLY**, every run,
> plus `--baseline` when comparing. `bench/run.py`'s default `--mylang`
> is **`build/mylang`, the MAINTAINER'S binary** — a bare
> `python3 bench/run.py` measures HIS build, not your change, and
> prints a believable geomean you will report as yours.
> **3. READ the `mylang : <path>` header line run.py prints.** If it is
> not a `build-claude/` path, the run is INVALID — discard it.
>
> Full statement of the rule under "CLAUDE BUILDS ONLY UNDER
> `build-claude/`". Two full-suite runs and a reported geomean were
> invalid this way on 2026-08-12; the maintainer's words were "it's
> truly unacceptable". The failure is invisible without step 3 —
> the wrong answer looks exactly like the right one.

`bench/` is a standalone performance suite comparing MyLang against CPython,
construct by construct
(`bench/my/NN_name.my` paired with `bench/py/NN_name.py`; a few MyLang-only
features like const-folding
have no `.py`). MyLang scripts use the `.my` extension. `python3 bench/run.py`
times every pair (best of N), prints a
`my/py` ratio table (the ratio number is colored on a TTY — a green→red
gradient, plain when piped or `--csv`), and
checks the two implementations printed matching results. Each script takes a
`scale` multiplier as its
first argv. It is *not* wired into `make`/CI and has no third-party deps.
`bench/README.md` is also the
written answer to "do MyLang and Python behave the same?": they do *observably*
— assignment aliases,
slices act like independent copies (MyLang via lazy copy-on-write, so read-only
slicing is far cheaper),
`clone()` makes a shallow copy and `deepclone()` a deep one — with the
divergences (64-bit wrapping vs bignum,
truncating vs flooring division, unordered vs insertion-ordered dicts)
enumerated there. (Floats match: both are 64-bit IEEE `double`.)
`bench/verify_semantics.{my,py}` assert that equivalence and must both print the
same line.

**⛔ THE PURE-CALL CACHE IS OFF FOR EVERY BENCHMARK RUN (maintainer-set,
2026-08-16).** `bench/run.py` passes **`-npc`** by default, to the current
binary AND to `--baseline`; `--pure-call-cache` opts back in, and the header
line says which way the run went. **Claude never runs the suite with the
cache on.** Two reasons, both measured:
- **It is not a language comparison.** The per-frame cache memoizes a pure
  call's result within one frame. C++, CPython, Ruby and Lua all re-execute
  the call, so with the cache on a bench that repeats a pure call times
  MyLang's memo table against the other language's real work.
- **It makes `scale` NON-LINEAR, which invalidates the startup
  correction.** `09_fib_recursive` is `for (k...) r = fib(29);` — iterations
  2..N are cache HITS, so it costs **93,116,925 Ir at scale 1 and
  93,121,763 at scale 3**. With `-npc`: 235.6M -> 561.5M, exactly linear.
  The scale3-minus-scale1 correction therefore divided a 4,838-instruction
  delta of cache hits by C++'s real one and produced a recorded
  "09_fib_recursive 6.41x -> 0.09x, FASTER than C++" that was an artifact
  (now marked INVALID in `plans/cpp-gap-ladder.md`). **Only
  startup-correct a bench whose work actually SCALES** — and check that it
  does, rather than assuming.

**BENCHMARK-RUN DISCIPLINE — 1-vs-1 BY DEFAULT, the maintainer controls the
deep runs (maintainer-set, 2026-07-19).** Do NOT run the bench suite over and
over on your own initiative — `--repeat 3`, hand-rolled 25x loops, run-after-run
to chase a fluctuating number — it WASTES the maintainer's time. Context: the
`>=2 interleaved runs` rule below was set during the regression-prone AST→VM
CONVERSION, where new bytecode kept introducing regressions and heavy A/B was
warranted. For ordinary OPTIMIZATION work the default is lean:
- **Run the basic 1-vs-1: ONE run before the change (or a series of changes),
  ONE run after — then MOVE ON.** No extra repetitions, no larger scale, on
  your own initiative. Bring the maintainer the data; HE interprets it. You do
  NOT get to re-run because a number looks off or surprising.
- **Escalating to more repetitions / larger scale / a deep perf session is the
  MAINTAINER'S call, not yours.** He decides when to stop and focus on perf; you
  cannot take that initiative. Only run heavier measurements when he explicitly
  asks (and approves the repetitions/scale). The `>=2 interleaved` rule below
  applies to those maintainer-initiated deep sessions (and to conversion work),
  not to routine optimization.
- **If a bench is too noisy to trust, the fix is a more RELIABLE BENCH, not more
  runs.** There is a planned, dedicated effort to LOWER bench VOLATILITY
  test-by-test, with various approaches per bench (bumping the scale of the
  most-fluctuating ones, reducing allocation/OS-interference noise, etc.) — a
  specific work item, done with the maintainer, so each bench gives a stable
  signal from a single run. (TODO, agreed 2026-07-19.)
- **Spend most of your time WRITING CODE and doing FUNCTIONAL testing** (`-rt`,
  the fuzzer, samples, reading the diff for mistakes) — correctness errors have
  been creeping in. Benchmarks are a small, final check, not the main activity.
- **Do NOT revert a change that SHOULD optimize things** — especially one the
  maintainer approved — because a measurement looks neutral. Bring the data and
  let him decide (see the "ask before reverting" convention + the "distrust a
  surprising result" rule below).
- **The other-language bench results are CACHED, and a measure run is
  cache-ONLY (DONE, 2026-07-19).** Python (and now C++, later Ruby/Perl/Lua/…)
  don't change between MyLang edits, so their timings live in a GIT-IGNORED
  `bench/.bench_cache/<lang>.json`, keyed by **(scale, sha1 of the comparison
  SOURCE file)** — NOT a git commit, so a MyLang edit/commit never invalidates
  them; only a scale change or a real `.py`/`.cpp` (or `bench.h`, for C++)
  change does. A normal (**measure**) run re-times ONLY MyLang and reads the
  cache **read-only**: `run.py` checks EVERY selected bench up front and
  **fails fast** if any comparison entry is stale/missing, naming the
  `--recompute` command — it NEVER re-times a comparison inline. Re-timing is a
  **separate, explicit step**: `bench/run.py -cl <lang> --recompute [--filter …]`
  (re)caches the stale entries — or all, with `--force` — then exits
  without timing MyLang. This is the load-bearing fix for a real asymmetry: a
  comparison subprocess re-run inline used to interleave with the variance-
  gated MyLang reps of the *next* bench and perturb them (so a stale Python
  cache made py-mode abort on variance where a fresh cpp cache did not — the
  inline re-run is now impossible). MyLang itself is NEVER cached (its time is
  the whole point). Cuts a normal suite run's wall-time roughly IN HALF.
- **The cache carries a MACHINE-SPEED MARKER, and a mismatch WARNS (2026-07-31,
  warning-only by maintainer instruction — it never blocks a run).** Caching the
  comparison creates one asymmetry the halved wall-time is worth: MyLang is
  timed LIVE and the comparison is read from disk, so a box that is merely
  SLOWER TODAY reads as a MyLang REGRESSION with no code change. That is not
  hypothetical — it cost a full false-alarm investigation: the suite geomean
  read 8.2x where it had read 10.7x, while an interleaved `--baseline` A/B
  proved HEAD was 1.02x FASTER than the 10.7x binary and callgrind Ir was flat
  or better on 13 benches; re-timing the byte-identical `.py` scripts showed
  CPython itself **1.148x slower** than the Jul-20 cache (median 1.205x). So a
  whole-cache `--recompute` stores a `__machine__` entry (best-of-3 of a fixed
  in-process CPU loop, ~0.1s), and a measure run re-times it and prints — above
  the table AND under the geomean — how far off the box is plus the CORRECTION
  FACTOR to apply to the printed "x faster" figures. A cache with no marker gets
  a one-line note. **A PARTIAL recompute (`--filter`, or stale-only) does NOT
  stamp**: most entries would still be from the old machine, and a fresh marker
  would wrongly certify them — a false negative is worse than no marker. The
  lasting lesson: **`cur/base` from `--baseline` is the trustworthy number**
  (both binaries timed interleaved, so drift cancels); my/py alone is not.

**THE FULL-SUITE MEASUREMENT HARD RULE (maintainer-set, 2026-07-16; applies to
DEEP, maintainer-initiated perf sessions — see the 1-vs-1 default above).** Any
claim about a VM perf change is made ONLY from FULL-SUITE `bench/run.py`
runs, BOTH binaries built and run in the SAME session — never from comparing
my/py prints across sessions (the CPython denominator drifts several percent
day to day on this box; it masked a real ~5% Phase-C regression as "noise"
once). Compare the VM WALL-CLOCK per bench (the my column) between the
binaries, plus the my/py geomean, which run.py prints with THREE digits
(0.256x / ~3.91x) for exactly this. For a deep session the maintainer may ask
for >=2 interleaved A/B/A/B runs to cancel machine drift; that is HIS call, not
a routine default. A phase does not land on a probe geomean.

**THE ASSERTS=0 MEASUREMENT RULE (maintainer-set, 2026-08-01).** EVERY
performance measurement — callgrind Ir or wall-clock, a one-off A/B or a
full-suite run — is taken with **`OPT=1 ASSERTS=0` on BOTH sides**. A plain
`make -j` is `ASSERTS=1` (the default), and **an ASSERTS=1 delta CANNOT be
extrapolated to ASSERTS=0**: assertion cost is NOT a uniform multiplier. It
lands unevenly per code path (the per-op `ML_VM_CHECK` tier;
`_GLIBCXX_ASSERTIONS` bounds-checking every `vector::operator[]`), so a change
that MOVES work between paths measures better or worse purely because of where
the asserts happen to sit. This is not theoretical — both directions were
measured on the same day: **#81** read **+0.22%** at ASSERTS=1 and **+1.14%**
at ASSERTS=0 (5x the delta — the nested baseline was the side paying the
hardened access, so removing hardening exposed the flat form's real cost), and
**69_exc_crossframe** read **−2.36%** at ASSERTS=1 but **+0.22%** at ASSERTS=0
— a SIGN FLIP, i.e. a "2.4% win" that was mostly the removal of hardened
container access, not the mechanism it was credited to. Corollary for the
code: with hardening on, `vector::operator[]` in a hot scan is NOT free —
read through `.data()` when you have already proven the range (a flattened
pool indexed with `operator[]` cost +28 Ir per throw versus the nested form
it replaced).

**⛔ RULE B1 AGAIN, BECAUSE THIS IS WHERE THE FLAGS ARE CHOSEN: `rm -rf
build` FIRST, then pass `--mylang build-claude/<lane>/mylang` (and
`--baseline`) EXPLICITLY, then READ run.py's `mylang : <path>` header.
The default is the MAINTAINER's `build/mylang`; a bare
`python3 bench/run.py` times HIS binary and reports a wrong number that
looks right. The build-config gate below checks `opt`/`asserts` — it
does NOT check WHOSE binary you handed it, so passing the gate proves
nothing about this.**

**THE BUILD-CONFIG GATE — AND `--force` IS THE MAINTAINER'S ALONE
(maintainer-set, 2026-08-01).** `bench/run.py` runs `mylang -v` on EVERY
binary it will time (the current one AND `--baseline`) and **REFUSES to run**
unless each reports `opt 1` and `asserts 0` — a mixed-config A/B is exactly
the trap, so the baseline is checked too. The refusal names the rebuild
command. **`--force` skips the gate and is RESERVED FOR THE MAINTAINER:
⛔ CLAUDE MUST NEVER PASS `--force` TO `bench/run.py`.** There is no
situation in which Claude should produce, quote, or record a forced number —
if the gate refuses, REBUILD (`make -j OPT=1 ASSERTS=0
BUILD_DIR=build-claude/perf`) and measure properly. When the maintainer uses
it, a full-width WARNING banner prints at the START and again at the END of
the output (both ends deliberately: this output is routinely piped through
`head` or `tail`, and a reader who sees only one end must still learn the
numbers are invalid).

**THE "PROVE THE CODE RAN + DISTRUST A SURPRISING RESULT" HARD RULE
(maintainer-set, 2026-07-19).** Two joined rules, both learned the hard way in
ONE investigation. **RULE B1 is the third member of this family and the
crudest: before asking whether the code PATH ran, make sure the BINARY
was yours — `rm -rf build`, pass `--mylang build-claude/<lane>/mylang`
explicitly, and read run.py's `mylang :` header line. A run of the
maintainer's binary is not a weak measurement, it is a measurement of
something else entirely.**

(1) NEVER draw a conclusion from a performance measurement until you have HARD
DATA that the code path under test ACTUALLY EXECUTED in that measurement. If
the code *might* not have run, you do not have a result — you have nothing.
INSTRUMENT (a counter, `-vd`/`-vdj` for the `enter.nat`, callgrind) and debug
until you are 100% sure it ran; ONLY THEN measure. **The trap:** a JIT change
was declared "dispatch doesn't matter for dict" from a `MYLANG_JIT=0` vs JIT-on
A/B that came out ~1.00x — but the JIT compiled **ZERO fragments** for that
bench (the store split the loop into sub-MIN_RUN pieces), so BOTH sides ran the
identical interpreted code. "JIT-on == JIT-off" proves NOTHING when the JIT
never engaged; a "neutral" result on an unverified path is INDISTINGUISHABLE
from "the optimization didn't run" — treat it as the latter until proven. This
is the general form of the ref-store-bail lesson (all those "native builtins
are neutral" numbers were interpreter-vs-interpreter).

(2) DISTRUST A SURPRISING RESULT — especially a null/zero one. When I finally
made the JIT engage (nativized the dict store; PROVED it: `native_entries=1`,
`dict_store_calls=1.5M` from native, `compiled_frags=1`) the first wall-clock
A/B came out ~1.00x and I ACCEPTED IT ("dispatch removal changes nothing") — a
SECOND methodological failure. It was WRONG: at low scale + few repeats on an
allocation-heavy bench (1.5M `unordered_map` inserts), timing NOISE swamped the
effect. Re-measured with rigor — 25 runs (min AND median agree at 0.934) plus
the DETERMINISTIC callgrind instruction count (213.6M vs 242.7M = **12% fewer
instructions**, zero timing noise) — the real answer is a stable **~6.5%
wall-clock / 12%-instruction win**. So: (a) a surprising result (0% where the
mechanism predicts non-zero) is a signal to RE-MEASURE, not to conclude; (b)
for a SMALL effect on a NOISY (alloc/cache-bound) bench, best-of-N wall-clock
is unreliable — reach for a DETERMINISTIC metric (callgrind I-refs) and/or many
repeats where min AND median must agree; (c) the removed dispatch was ~12% of
INSTRUCTIONS but only ~6.5% of TIME, because the removed ops are the CHEAP ones
(predicted branches / L1 hits) while the time is the dict's alloc/cache-miss
work — instruction-count and wall-clock deltas differ and BOTH are worth
knowing. Net for the dict tier: dispatch is a real, capturable chunk (~6-12%);
the BIGGER headroom is still the value model (boxing/alloc/refcount, `my/cpp`
~5x in bench/cpp/ → the N7 arc) — but "dispatch is irrelevant here" was FALSE
and was asserted twice without adequate data.

**`--vm` — the bytecode-VM performance gate.** `run.py` measures the VM by
DEFAULT (the binary's default engine since the 2026-07-18 flip); `--vm`
passes the flag explicitly (needed for a PRE-flip binary in a cross-flip
A/B — pass `--vm` on both sides), and `--tw` runs the tree-walker. Combined
with `--baseline <the same post-flip binary>`, `--vm` gives the baseline
`-tw`, so the `cur/base` column and geomean are **VM / tree-walker** (<1 ==
VM faster). This is the
per-phase performance gate for the VM build-out (see "Execution strategy" +
`plans/archived/bytecode-vm.md`): run it release + `ASSERTS=0` at the end of each phase;
a phase must not regress the tree-walker unless the regression is flagged as
temporary + tracked to a later phase that erases it. Phase 0 (pure fallback) is
neutral (geomean 1.00x), as it must be.

## Source layout & compilation model

**Only `src/*.cpp` are compiled** (the Makefile globs them) — twenty-three
translation units:
`lexer.cpp`, `parser.cpp`, `syntax.cpp`, `resolver.cpp`, `inferencer.cpp`,
`eval.cpp`, `types.cpp`, `statictype.cpp`, `trace.cpp`, `coderender.cpp`,
`backtrace.cpp`, `errfmt.cpp`, `highlight.cpp`, `lineedit.cpp`, `replhelp.cpp`,
`repl.cpp`, `codegen.cpp`, `vm.cpp`, `jit.cpp`, `disasm.cpp`, `serialize.cpp`,
`mylang.cpp`, `tests.cpp`
(six of them are the REPL — see "The interactive REPL" below; `trace.cpp` is the
diagnostic tracer and `coderender.cpp` the optimized-AST "decompiler", both used
by the REPL; `codegen.cpp`/`vm.cpp` are the bytecode-VM engine and `disasm.cpp`
its text disassembler (`-vd`) — see "Execution strategy" — added by the glob,
nothing to register).

- `mylang.cpp` — CLI entry point, arg parsing, the top-level `try/catch` that
  turns thrown
  `Exception`s into formatted error output (`dumpLocInError` prints the source
  line + a `^` caret).
- `lexer.cpp` / `lexer.h` — `lexer(src, start_line, tokens)` appends tokens for
  a WHOLE source buffer (not one line): it scans `src` in a single pass,
  tracking the current line + line-start offset so each token's `Loc` is
  (line, column). Scanning the whole buffer is what lets a **string literal or a
  `/* */` block comment span newlines** (the embedded `\n` is ordinary content;
  the line just advances). Callers join their source lines with `\n` and lex
  once (`mylang.cpp`'s `lex_all`/`source`, the REPL's per-input `source`,
  `tests.cpp`'s `check`); `start_line` lets the REPL continue line numbering
  across inputs. `#` is a line comment; an unterminated string / block comment
  at EOF throws `InvalidTokenEx` with `unterminated=true` (the REPL reads that
  flag to keep the input open for more lines).
- `parser.cpp` / `parser.h` — recursive-descent parser, const-folding woven in.
  `pBlock()` is the
  entry point.
- `funcdesc.h` — **`FuncDescriptor`**, the SERIALIZABLE runtime function
  identity (name/params/captures/frame data/purity/chunk pointer), plus the
  `DeclType`/`SymKind`/`ResolvedSym` enums it needs (moved here from
  syntax.h). The runtime call model reads ONLY this - see the value-model
  section and plans/archived/vm-ast-free-runtime.md.
- `syntax.h` / `syntax.cpp` — the `Construct` AST node hierarchy, its
  `serialize()` (what `-s` prints), and `clone()` (a deep copy of a subtree,
  used by inlining; pure virtual so every concrete node must provide one — see
  the `clone_as`/`copy_base_fields`/`clone_ops_into`/`clone_elems_into`
  helpers). Also `desugar_named_call` (over a `ParamSpec` view) — the shared
  named-argument → positional rewrite used by both the parser and the
  inferencer (see *Static type inference*).
- `resolver.cpp` / `resolver.h` — `resolve_names(root)`, a post-parse pass that
  assigns function
  params slot indices for O(1) access at runtime (see the value model section).
  Optional and always
  safe: anything it leaves unresolved falls back to the runtime map lookup. The
  same file also hosts the **auto-const** folder (the `AutoConst` class), run at
  the end of `resolve_names` (see the const-evaluation section), and the
  **inliner** (the `Inliner` class), run after it (gated by `-ni`; see the value
  model section / `plans/archived/function-inlining.md`), and the **parameter
  escape analysis** (`stamp_noescape_params`, last — see below).
- `eval.cpp` — the `do_eval()` bodies: the actual tree-walking interpreter.
- `types.cpp` — the single TU that stitches the type system and builtins
  together (see next section).
- `statictype.cpp` / `statictype.h` — the **static-type lattice** for type
  inference
  (`StaticType`/`StaticTypeArena`:
  `resolve`/`unify`/`assignable`/`join`/`equal`/
  `to_string`). Distinct from the runtime `Type *` ops table — this is what the
  compile-time inferencer reasons over (type variables, nullability `opt`,
  structural array/dict/func shapes). See `plans/archived/type-inference.md`.
- `inferencer.cpp` / `inferencer.h` — `infer_types(root)`, the **whole-program
  static type inference + checking** pass (see the dedicated section below).
- `backtrace.cpp` / `backtrace.h` — `format_backtrace()`, which renders an
  `Exception`'s captured call-stack (see the error model section).
- `analyzer.h` / `analyzer.cpp` — the `AnalysisInfo` `Loc`-keyed annotation
  collector + `AnnoKind` for the `-a`/`--analyze` colored optimization view, and
  the shared `analyze_and_render` pipeline and `render_analysis` renderer (used
  by both the `-a` file driver and the REPL `:analyze`). The collectors live in
  the relevant passes (`collect_array_analysis` in `inferencer.cpp`,
  `collect_resolver_analysis` in `resolver.cpp`, the counted-`for` mark inside
  `specialize_types` in `inferencer.cpp`, mutation-time records in
  `parser.cpp`/`resolver.cpp`). See the `-a` description under "Build & run".

**The `.cpp.h` convention.** Files under `src/types/` and `src/builtins/` are
named `*.cpp.h` and are
`#include`d *once* into `types.cpp`. Each starts with a comment: "this is NOT a
header file… it's a
C++ file in the form of a header, just because it's faster to compile it this
way." So `types.cpp` is
the one TU that compiles every `TypeXxx` class and every `builtin_xxx`. When
adding type or builtin
code, put it in the matching `.cpp.h` and rely on it being pulled into
`types.cpp` — it will not
compile standalone, and there is nothing to add to the Makefile.
`builtins/reflect.cpp.h` holds the **runtime reflection** builtins
(`globals`/`typestr`/`kindstr`/`signature`/`layout`/`specializations`) and the
shared `reflect_*` rendering helpers (signature/type/layout strings) the REPL's
introspection commands reuse; it is `#include`d last (after the other builtins)
so it can call `arr_elem_at`. See `plans/archived/repl-introspection.md`.
**`layout(S)` returns a structured value, not a string** — a **native composite
type** (`StructLayout`, holding an `array<StructField>`), the first of the
reflection objects. The two native `StructTypeDef`s are built in C++
(`native_struct_field_def`/`native_struct_layout_def`, `eval.cpp`), the
inferencer registers them in `struct_by_name` (`setup()`) and types `layout()`
via `builtin_result`, and `reflect_make_layout` (reflect.cpp.h) constructs the
boxed instance. `Type` objects (the `type()`/`decltype()` return value, see
*Compile-time TYPE QUERIES*) reuse this same mechanism — design record:
`plans/archived/reflection.md`.

**Why so many headers are templates.** `type.h` (`TypeTemplate`),
`sharedarray.h`
(`SharedArrayObjTempl`), `shareddict.h`, `exceptionobj.h` are templated *not*
for genericity but to
break a circular include order: they need `EvalValue`/`LValue`, which need them.
`evalvalue.h`
instantiates them with concrete types via typedefs
(`typedef TypeTemplate<EvalValue> Type;`,
`typedef SharedArrayObjTempl<LValue> SharedArrayObj;`, …). Treat these typedefs
as the real types.

## The pipeline

**lexer → recursive-descent parser (with const-folding woven in) →
type-inference + checking → name-resolution pass → typed specialization →
tree-walking evaluator.**

Type inference runs *between* parsing and `resolve_names` (on the clean,
un-inlined tree); the typed-node *specialization* it enables runs *after*
`resolve_names`. Both are gated by the CLI's `-nti` and on by default. See
"Static type inference" below.

**Implicit top-level `var` (`mark_implicit_globals`, `resolver.cpp`).** A tiny
pre-pass run by every driver right after parse and *before* inference (script
`mylang.cpp`, REPL `repl.cpp`, and the `-rt` harness's `check()`), so it is NOT
gated by `-nti`. It scans the root block's **direct** statements and, for a
plain `name = expr` (`Expr14`, `op == assign`, bare `Identifier` lvalue) whose
name is not yet declared and is not a builtin, **sets `pInDecl` on that node** —
turning it into an ordinary `var` decl that every later pass already handles (no
other change anywhere). Only the OUTERMOST scope qualifies (a nested block /
function body still needs `var`). The "already declared" set is built forward
(explicit decls, func/struct names, earlier implicit vars) seeded with `known` —
empty for a script, the prior-input globals (runtime + const scopes) for the
REPL, so a later `a = 2` re-targets the existing global instead of re-declaring
it. See README *Declaring variables*.

**The post-inference optimizer pipeline is one shared function,
`run_optimizers` (`resolver.cpp`/`.h`): `resolve_names` (slotting + auto-const
+ inlining + specialization) then `specialize_types` (M8).** Both drivers call
it — the script path (`mylang.cpp`) with `repl_mode=false`, the REPL
(`repl.cpp` `do_eval`) with `repl_mode=true` + the prior-input scope — so the
REPL transforms the tree EXACTLY like a script (only the inference *front* end
differs: one-shot `infer_types` vs incremental `ReplInfer::check_input`, both
built on the same `Inferencer::infer_one`). A new pass added to
`run_optimizers` reaches both identically — the REPL's optimization parity is
not maintained by hand. Likewise the `-a`/`:analyze` collect-and-render
pipeline is one shared `analyze_and_render` (`analyzer.cpp`).

### Lexer

Produces a `vector<Tok>`. A `Tok` is `{ TokType, Loc, value/op/kw }` where
`TokType` ∈ {integer, id,
op, kw, str, floatnum, unknown}. Operators (`Op` enum, `operators.h`) and
keywords (`Keyword` enum,
`lexer.h`) are recognized via `std::map`s built once from the `OpString` /
`KwString` arrays — keep
those arrays index-aligned with the enums if you add tokens. `invalid_tok` is
the EOF sentinel.
Chars are 8-bit; **no Unicode** (deliberate, to stay small). A **trailing `!`**
is part of an identifier (Ruby/Scheme "bang" convention, e.g. `get!`), but only
when it is not the start of `!=` — so `x!=y` still lexes as `x != y`.
**The lexer scans the whole buffer in one pass** (see the file bullet above): a
`lexer_ctx` tracks `cur_line`/`line_start` and stamps each token's start `Loc`
into `tok_loc` when it begins (so a multi-line string reports the loc of its
**opening quote**, not its close). Two **multi-line constructs**: a `"..."`
string keeps embedded newlines in its value (the `string_view` spans them —
which is *why* the lexer must see the whole buffer, not a line at a time), and a
`/* ... */` **block comment** (`skip_block_comment`) is skipped across lines.
`/*` can never be valid code (there is no unary `*`), so adding it broke
nothing. Both throw `InvalidTokenEx` with **`unterminated=true`** at EOF if not
closed (vs. a malformed token like `2_`, which is `unterminated=false`); the
REPL's `is_incomplete` returns that flag to keep reading. `#` line comments and
the trivial value paths are unchanged.

### Parser — operator-precedence ladder

Binary-operator precedence is encoded as a ladder of parse functions
`pExpr01 … pExpr14`, each
producing a matching AST node class `Expr01 … Expr14`. **The numbering has
gaps** — only
`02,03,04,05,06,07,08,09,10,11,12,14` carry real operators (the level numbers
match C precedence; an unused level is skipped):

- `pExpr01` — primaries + postfix chains: literals, `()`, `[...]` array, `{...}`
  dict, identifiers, then any run of call `(...)`, subscript/slice `[...]`,
  member `.id`. A call's argument list is parsed by `pArgList` (not the generic
  `pList`), which also accepts **named arguments** `name: value` (label = a bare
  IDENT followed by `:`, one-token lookahead) — see the inferencer's
  `lower_named_args` and README *Named arguments*. A trailing **`++`/`--`**
  (`Op::inc`/`Op::dec`) after the postfix chain makes a **postfix**
  `IncDecExpr`. **Optional member access** `a?.b` (`Op::qmdot`, a new 2-char
  token) sets `MemberExpr::optional`: `do_eval` returns `none` when the base is
  `none` (else the member); the inferencer types it `opt(member)` and the check
  pass skips the non-opt-base requirement for it. Each `?.` guards only its own
  base (an all-optional chain `a?.b?.c` short-circuits; a plain `.c` after `?.`
  is not guarded — a deliberate simplification vs JS's whole-chain guard).
  Optional subscript/`?.[`/optional call `?.()` are not implemented.
- `pExpr02` — unary `+ - ! ~` (right-recursive, so `!!x`, `-+x` work); `~` is
  bitwise NOT here (the same `Op::bnot` token is the `dyn` alias in a *param*
  position, but that is handled in `pFuncParam` before any expression, so they
  never collide). A leading **`++`/`--`** makes a **prefix** `IncDecExpr` (so
  `--1` now lexes as decrement-of-`1`, a compile error like C — not `-(-1)`)
- `pExpr03` — `* / %`
- `pExpr04` — `+ -`
- `pExpr05` — `<< >> >>>` (shift; `>>` signed/arithmetic, `>>>` unsigned/logical)
- `pExpr06` — `< > <= >=`
- `pExpr07` — `== !=`
- `pExpr08` — `&` (bitwise AND), `pExpr09` — `^` (XOR), `pExpr10` — `|` (OR)
- `pExpr11` — `&&`
- `pExpr12` — `||`
- **`pExprCoalesce`** — null-coalescing `a ?? b` (right-assoc; `Op::coalesce`),
  between `||` and the ternary
- **`pExpr13`** — the ternary `cond ? a : b` (right-assoc else: `a?b:c?d:e` ==
  `a?b:(c?d:e)`; the middle is a full `pExpr14`, bounded by `:`). Fills the
  long-unused level-13 gap — exactly where C puts the conditional operator. Both
  reuse the existing `?`/`:` tokens; only `??` is a new token. `pExpr14`'s lside
  now enters at `pExpr13` (so every condition/slice/element reaches them). Both
  const-fold (a const cond → the taken branch; a const `??` lhs → lhs/rhs); the
  AutoConst analogue is in `fold_reads` (`resolver.cpp`), which **must** descend
  into `TernaryExpr`/`CoalesceExpr` (it has no generic fallthrough — a missing
  case silently fails to fold their children).
- `pExpr14` — assignment `=  +=  -=  *=  /=  %=  <<=  >>=  >>>=  &=  |=  ^=`
  (the compound set is defined ONCE, `compound_assign_base` in operators.h —
  the parser's accept list, the inferencer's typing, both engines' RMW
  dispatch, the codegen lowerings, the REPL continuation sets and the disasm
  spellings all ask that function), plus `var`/`const` decls and id-list
  targets. A compound lowers byte-identically to its spelled-out
  `x = x OP rhs` (pinned by test); the shift/bitwise compounds are int-only
  like their binary forms. The lexer's maximal munch probes 4 chars first
  (`>>>=`), then 3, then 2.

`pExprGeneric<ExprT>` implements the common left-associative chain: it collects
`(Op, operand)` pairs
into a `MultiOpConstruct`, evaluated left-to-right in `do_eval` by mutating an
accumulator
(`val.get_type()->add(val, rhs)`, etc.). `pBlock` is the entry point; `pStmt`
dispatches statements
(`if`/`while`/`for`/`foreach`/`func`/`try`/`throw`/`return`/braced
block/expression-statement).

**`func f(x) => expr` is parse-time SUGAR for `func f(x) { return expr; }`.**
The parser's arrow branch (`pAcceptFuncDecl`) desugars immediately — the body
is ALWAYS a `Block` (one body shape everywhere: the VM compiles every function
body to a chunk; no later pass needs an expression-body special case; the
synthetic `ReturnStmt`/`Block` carry the expression's locs so carets and
backtraces are unchanged). The passes that OPTIMIZE the sugar look through the
wrapper via **`func_expr_body(fd)`** (`syntax.h`: the inner expression iff the
body is exactly `{ return <expr>; }` — hand-written or desugared, the two
spellings are deliberately indistinguishable; tag-based, no dynamic_cast):
the EXPRESSION inliner's classification + splice (`inlinable_decl`, the clone
/ size gate / per-param use counts all run on the inner expr, so the `-it`
threshold measures what it always measured), `do_func_call`'s direct-eval
fast path (the tree-walker evaluates the inner expr with no Block loop /
FlowState round-trip — skipped under `-vm` when the body's chunk exists,
which is the native form of the same body), and coderender (a single-return
body renders back as `=> expr;`). Two Inliner rules keep the engines
disjoint and sound: a func registered in `funcs` (the `-it`-gated expression
engine) stays OUT of `block_funcs` (block-inline is CALL_WEIGHT-gated and
would bypass `-it`; a single-return body the expr engine REFUSES — a
recursive one, for the unroll; a param-mutator — still reaches the block
engines), and **`refold` never folds a LAZY builtin call**
(`defined`/`isconst`/`isconstdecl` — `defined(g)` tolerates an UndefinedId
arg, so a cctx eval "succeeds" and would answer a RUNTIME-order-dependent
question at compile time; exposed by an inlined `return defined(gg)` body).

**Bitwise / shift operators (`~ & ^ | << >> >>>`) are int-only.** New `Type`
virtuals `band`/`bor`/`bxor`/`shl`/`shr`/`ushr` (binary) and `bnot` (unary) —
base `Type` throws `TypeErrorEx`, only `TypeInt` implements them, so a `float`
operand (which `num_bin_op` routes to `TypeFloat`) raises a type error; a `bool`
promotes to `int` first like the other numeric ops, and the result is always
`int`. `>>` is a SIGNED (arithmetic, sign-extending) right shift, `>>>` the
UNSIGNED (logical, zero-filling) one (JavaScript semantics). The shift bounds /
sign handling live in **`bitops.h`** (`bit_shl`/`bit_shr`/`bit_ushr`: count must
be `>= 0` or `InvalidValueEx`, a count `>= 64` saturates to `0` / sign-fill
instead of UB) — shared by `TypeInt` AND the **M8** unboxed path so they compute
identically. M8: the BINARY bitwise nodes (`Expr05`/`08`/`09`/`10`) specialize
into a `TypedScalarExpr` (`Cat::arith`, the int `eval_int` loop, which gained
the `band`/.../`ushr` cases) — so a bit-manip tight loop is unboxed (~2x over
`-nti`); unary `~` (an `Expr02`) stays the boxed path. The inferencer's
`binop_result`/`unary_result` type them (int, int/bool operands; a float is
`dyn` → the check pass reports "operator does not apply"). Precedence matches C
exactly (see the `pExpr0N` ladder above), including the `a & b == c` →
`a & (b == c)` trap.

A `fl` bitmask of `pFlags` (`pInDecl`, `pInConstDecl`, `pInLoop`, `pInStmt`,
`pInFuncBody`,
`pInCatchBody`) is threaded through every parse function and gates legality:
`break`/`continue` only
under `pInLoop`, `return` only under `pInFuncBody`, `rethrow` only under
`pInCatchBody`, and
`var`/`const` set `pInDecl`/`pInConstDecl` so `pExpr14` knows to *declare*
rather than *assign*.

### Const-evaluation — the central design feature

Constants are evaluated **at parse time**, like C++ `constexpr`. This is the
project's defining trait
and it lives *inside the parser*. Mechanics:

- `ParseContext` owns a chain of **const `EvalContext`s** (`const_ctx`).
  `pBlock` pushes a fresh
  nested const ctx on entry and pops it on exit, so the const scope chain
  mirrors lexical scope.
- Every `Construct` carries an `is_const` flag, propagated bottom-up: a node is
  const iff all its
  children are const. String literals are const; `var`/freshly-declared
  identifiers are not.
- As soon as the parser finishes a const node, it evaluates it against
  `const_ctx` and calls
  **`MakeConstructFromConstVal()`** to replace the subtree with a literal node.
  That function inlines
  `int`/`float`/`none`/`str` unconditionally, and `arr`/`dict` only when
  `process_arrays` is set — in which case it bakes the whole value into **one
  `LiteralObj` node** (`syntax.h`), not one literal per element. (It stores
  `v.clone()` so a small slice of a huge const array doesn't pin the huge
  buffer.) `LiteralObj` carries an **`immutable`** flag. The materializer sets
  it when **either** the target is a `const` decl (`fl & pInConstDecl`) **or the
  value itself is already read-only** (`is_readonly_value()`). The second case
  is how **const-ness propagates**: a slice/element/result derived from a const
  is read-only, so `var s = y[1:3]` (with `y` const) keeps `s` read-only —
  `var` only makes the *name* rebindable, not the value mutable. (This mirrors
  runtime, where the value carries the flag; a *fresh* literal isn't read-only,
  so `var a = [1,2,3]` stays mutable.) `LiteralObj::do_eval` (`eval.cpp`) then
  either:
  - for an `immutable` result: hands out the **deep read-only** value (baked via
    `make_const_clone()` — every array/dict in it, recursively, is flagged
    read-only — and shared, since it can't be mutated), so it is immutable
    through *any* alias (e.g. a non-const function parameter, or a `var`); or
  - for a mutable result: a *fresh, fully-mutable deep copy* via
    `make_mutable_clone()`, exactly what the old per-element
    `LiteralArray`/`LiteralDict` produced at runtime — a `var` bound to it must
    be writable, and re-evaluating the node (loop body, function called twice)
    must not see a prior mutation.

  Direct reads of a const symbol (`y[k]`, `len(y)`) still fold to literals at
  parse time; the runtime read-only flag is what additionally enforces
  immutability through aliasing (see the copy-on-write section). `LiteralObj` is
  `is_const` but deliberately **not** a `Literal` (which in this codebase means
  a *scalar* literal — see auto-const's `is_scalar_literal`), so an array/dict
  value is never mistaken for a promotable scalar.
- **Scalars vs. containers (`ShouldConstSymbolExistAtRuntime`).** Const scalars
  are inlined
  everywhere and their declaration is dropped from the runtime AST entirely (the
  decl parses to a
  `NopConstruct`, which `pBlock` discards) — `-s` shows them simply gone. Const
  **arrays, dicts, and
  funcs** are *kept* as real runtime symbols (declared once), because inlining a
  million-element array
  at every use site would be wasteful; operations on them like `arr[2]` or
  `len(arr)` still get
  const-folded to literals though.
- **Const-expression de-duplication (CSE).** The three sites that bake an
  array/dict (`pAcceptCallExpr`, `pAcceptSubscript`, the `pExpr14` decl-rvalue)
  go through **`cse_materialize()`** (`parser.cpp`) instead of calling
  `MakeConstructFromConstVal` directly. It builds a canonical string key for the
  expression (`cse_key`/`cse_key_rec`: identifiers resolved to their const
  `LValue *` so shadowing can't alias; only cheap leaves — ids and scalar
  literals — are eval'd, structural nodes recurse without evaluating; a
  `CSE_KEY_CAP`-byte cap bounds key cost and skips huge literals) and looks it
  up in **`CseCache`** (`parser.cpp`), a stack of `unordered_map<string,
  EvalValue>`
  scopes pushed/popped by `pBlock` in lockstep with `const_ctx`. On a hit it
  shares the already-baked **deep read-only** value (no re-eval, no re-clone) —
  this is why two identical const exprs report equal `intptr()`. On a miss it
  bakes via `make_const_clone` and caches the value *only when it is read-only*
  (the sole safely-shareable case; mutable `var`-bound literals are never
  cached/shared). Popping a scope with its block is what stops a freed block's
  reused stack addresses from colliding with a live key. `pExpr14` skips
  re-materializing an rvalue that is *already* a `LiteralObj` (a subscript/call
  result baked at its own site), so the de-dup lives at one layer and no double
  clone happens. CSE is a pure optimization (miss == old behavior); its win is
  compile time + memory, not runtime speed (folding already makes each use a
  literal). `bench/52_cse_dedup` and the `CSE:` tests cover it.
  `cse_materialize` is PIMPL-friendly: `CseCache` is forward-declared in
  `parser.h` with an out-of-line `~ParseContext()`.
- **Statement folding:** an `if` with a const condition is replaced by just its
  taken branch; a
  `while`/`foreach` proven to never execute (const-false condition / const-empty
  container) is dropped.
- **`pure func`s are the escape hatch.** `pure func f(...)` parses with
  `is_const = true` and is
  registered into `const_ctx`, so it *can* be invoked during const-eval (a plain
  `func` cannot). Pure
  funcs may not have a capture list and see only consts + their own params. A
  `CallExpr` folds when
  the callee and all args are const — that's how
  `sort(arr, pure func(a,b) => a<b)` runs at parse time.
- **THE PROVER: a call that is GUARANTEED to fail is a COMPILE error
  (step 7 tier 2, `prove_unbound_calls` in resolver.cpp).** An
  unconditionally-evaluated call to a function that unconditionally reads a
  global declared further down can only ever raise `UnboundSymbolEx`, so it is
  refused — **even inside a `try`** (maintainer's call; the two exception
  kinds exist so this is expressible, `UseBeforeBindingEx` being the
  uncatchable compile one). **The whole soundness argument is one idea: only
  UNCONDITIONAL code counts, on BOTH sides** — a false "provable" refuses a
  program that would have worked, which is worse than a late error. So
  `for_each_unconditional`'s exclusion list IS the proof: if/loop bodies,
  catch/finally, ternary arms, `??` and `&&`/`||` tails, a function body, and
  **a lazy builtin's argument** (`isbound(g)` is how a careful program AVOIDS
  this error — counting it as a read refuses the code that got it right).
  Two traps: `for_each_child` does NOT descend into `Block` or `Expr14`
  (`walk()` handles them), so a generic descent silently stops at
  `var t = f();`; and it runs on the **CLEAN tree, before AutoConst and the
  inliner**, because whether a program COMPILES must not depend on which
  optimizations ran (RULE 2) — after DCE, `if (false) { fetch(); }` would
  compile while `-nc` refused it; after the inliner, the analysed shape would
  differ between `-ni` and the default.
  **IT IS TRANSITIVE** (2026-08-09): the globals a call can reach are a
  FIXPOINT over the CALL GRAPH (`build_reachable_reads`), not a one-level look
  at the callee's own body — `outer` calling `fetch` which reads `g` is
  proven, where before the two-hop program compiled and died at run time while
  the one-hop one was refused. A fixpoint and not a walk **because mutual
  recursion makes the graph cyclic**, and a cycle has no traversal order. ONE
  switch keeps the error tier sound: `unconditional_only` picks the walker, so
  with it BOTH the reads and the CALLS are the unconditional ones and a global
  enters a set only if every step from the call to the read is guaranteed — a
  conditional link ANYWHERE drops the case to the warning tier, and all three
  positions (outer call, inner call, read) are pinned.
  **IT SEES THROUGH A WRITE-ONCE NAME** (#140/#146, 2026-08-09):
  `var f = fetch; var t = f();` is refused exactly like the direct `fetch()`
  spelling, at the top level and through a call hop alike
  (`index_func_aliases` + `callee_of`, consulted by BOTH tiers and by the
  fixpoint's call edges). Three rvalue shapes name one function: a top-level
  NAMED func, a LAMBDA literal (`var f = func() { return g; };` — then the
  lambda is the whole call graph, so `build_reachable_reads` must iterate
  the alias TARGETS and not just the name index, and the cheap early bail
  must ask about both or a program with no named function is skipped
  entirely), and a CHAIN (`var f2 = f;` — free, because the index is built
  in statement ORDER, so each link sees the one above it, which is also
  exactly when it holds that value).
  Sound for the same reason AutoConst's promotion is: the declaration is the
  ONLY write, so wherever the name is bound at all it holds that one
  function — proven from the write counters the resolver has already
  collected (`writes[slot] == 1` for a main-frame local, absence from
  `reassigned_globals` for a global), and a reassignment at any depth in any
  scope disqualifies it. A capture cannot defeat it: captures are BY VALUE.
  **The decline direction is the one that matters** — a false alias refuses a
  program that RUNS (`var f = fetch; f = other; f();` where `other` never
  reads the global), which is why the write-once sabotage is watched failing
  on a case that asserts its own result, not merely that it compiled. One
  deliberate limit stays silent: a top-level call ABOVE the binding is left
  alone (the failure there is the unbound NAME, and reporting what it will
  later hold would name the wrong cause — the TDZ already refuses it). A
  callee that needs a real callee-SET analysis to bound — a container
  element (`ops[0]()`), a parameter, an alias declared inside a body — is
  not proven, but it IS reported by the warning tier's weak arm below.
- **`isbound(name)` — the TDZ pair's runtime half (#131 step 6).**
  `defined(name)` asks whether the name EXISTS (true throughout its scope,
  ABOVE the declaration included — the name is declared, merely unbound);
  `isbound(name)` asks whether its declaration has RUN. LAZY (unevaluated
  argument — `mark_lazy_builtin` + the F1 rule: callable directly, never a
  value), and its argument must be an **identifier** (or `none`), checked by
  `Inferencer::check_isbound_args`. **Three things a future reader will get
  wrong:** (1) the arg-shape rule must be a COMPILE error, because the runtime
  body's own throw is the tree-walker's answer while the no-fail codegen
  refuses the shape (`NotLoweredEx`) — an engine divergence; (2) it must run
  UNGATED, not inside `reject_dev_builtins` (which the `-rt` harness and the
  REPL skip wholesale via `g_dev_builtins_allowed`, so a check placed there is
  invisible to every test) nor behind `checks_enabled` (`-nti` must not turn a
  syntactic rule off); (3) the FIX-1 exemption is NARROWER than the TDZ one —
  every lazy builtin is exempt from BOTH (`no_tdz_check` and `no_undef_check`),
  so a name declared NOWHERE answers `false` rather than erroring. That was
  the reverse until 2026-08-09, when the maintainer made the short form the
  point: `isbound(x)` ALONE is the feature test, where the conjunction
  `defined(x) && isbound(x)` used to be required. The typo hazard it accepts
  is one `defined()` already had, so the two lazy queries are now consistent.
  `isbound` is a #135 NARROWING GUARD for the same reason — without that, the
  `print(x)` inside `if (isbound(x)) { ... }` would still be refused and the
  short form would not work at all.
  `builtin_isbound` reads `sym` off the Identifier and never evaluates it —
  evaluating an unbound global throws the very `UnboundSymbolEx` the call
  exists to avoid. The lexical kinds fold in `try_fold_isbound`; a GLOBAL is
  the one runtime query and reuses `DefinedGlobalV` (which `defined` no longer
  reaches, since a declared global folds to `true`).
- **⛔ A DECLARATION BEATS THE CONST BUILTIN IT SHADOWS (#133, 2026-08-09).**
  `pAcceptId` used to resolve a name straight to a const builtin without ever
  asking what the program declared, so `func abs(x){return 42;}` gave
  `abs(-1)` == **1** (the builtin, folded) and `abs(runtime(-1))` == **42**
  (the function) — the same call, two answers, and a RULE 2 violation (`-nc`
  printed 42 twice). It hit five shapes: the func name used after OR before
  its decl, a param in a `=> expr` body (which folded `<builtin> + 1` into a
  COMPILE error), a lambda param, and a foreach loop var.
  The fix is **`ParseContext::shadowed`** (parser.h): names that also name a
  const builtin, which `pAcceptId` refuses to const-resolve. Two feeders,
  deliberately different in reach — a **token PRE-SCAN in the ParseContext
  ctor** for every `func NAME`/`struct NAME` in the stream (whole-parse, since
  #134 binds those at SCOPE ENTRY so a use ABOVE the decl already means the
  user's function, which a single-pass parser cannot know at the use), and a
  **scoped push/pop** for params (`pAcceptFuncDecl`, around BOTH body forms —
  the `=> expr` sugar never reaches `pBlock`) and foreach vars. `pBlock`
  pushes a scope in lockstep with the CSE cache. A `pure func` is NOT
  shadowed: it IS the const evaluator's own binding and must keep folding.
  The pre-scan is intentionally over-broad (a nested `func abs` shadows the
  name everywhere): shadowing only ever COSTS A FOLD, never an answer, since
  the resolver then binds the name correctly at run time — so when in doubt,
  shadow. **A `var`/`const` of that name is still refused outright**
  (`CannotRebindBuiltinEx`, `declExprCheckId`); this rule is for the forms
  that were always allowed.
- **Early failure:** exceptions raised *during* const-eval propagate immediately
  and are *not*
  catchable by script `try/catch` (the parser never enters a const assignment
  inside a try). This is
  intentional — const errors should fail the build, not be swallowed.
- A const decl in `pInConstDecl` whose rvalue isn't actually const throws
  `ExpressionIsNotConstEx`.

`-s` (with vs. without `-nc`) is the way to *see* all of the above happening.

**Auto-const (`AutoConst` in `resolver.cpp`).** A post-parse folding pass that
does for plain `var`s what the parser does for `const`. It runs at the end of
`resolve_names()` (so it can use the resolver's per-slot write counts and
slot identity). For each function (and the top-level "main"), it:
- **promotes** a `var` that is *write-once* (`slot_writes[slot] == 1`, i.e. only
  the declaration writes it) with a constant **scalar** initializer into a
  compile-time constant (keyed by slot), drops the declaration, and folds every
  use to the literal — cascading in declaration order, so a `var` derived from
  earlier auto-consts also promotes. Uses are folded in every read position,
  including a `return` expression (`fold_child` handles `ReturnStmt` explicitly:
  it's a plain `Construct`, not a `SingleChildConstruct`, so `fold_reads` skips
  it — without that a promoted `var` used only in a `return` would
  have its decl dropped but the use left dangling as an undefined variable);
- **folds** all-literal arithmetic/logic/comparison (`MultiOpConstruct`) to a
  single literal, reusing the interpreter (`mo->eval(&cctx)` against a const
  `EvalContext`);
- **short-circuit / identity folds** a logical op with const LEADING operands.
  mylang's `&&`/`||` yield a **bool** (not the operand, unlike Python — `false
  || 5` is `true`, not `5`), which shapes the rules. A const that **determines**
  the result — `false && rest` → `false`, `true || rest` → `true` — folds the
  whole expression to that bool (sound regardless of `rest`: it is
  short-circuited, so never evaluated - including its side effects. **That
  claim was FALSE until #138**: only the FOLD skipped `rest`; the runtime
  evaluated every operand, so the same operator behaved differently depending
  on whether const-eval could see the left side - a RULE 2 violation this
  paragraph was quietly asserting away. `&&`/`||` short-circuit at run time
  now, in all three engines - see *Short-circuit* under the eval section. An
  UNDEFINED NAME there is nonetheless a compile error since FIX-1 (#130):
  the resolver rejects it before this fold runs. Early dead-code
  elimination, which would let a name survive under a false guard, is a
  separate parked idea (#135).)
  This is what makes a feature-flag guard fold: `const DEBUG = false; if (DEBUG
  && heavy())` → `if (false)`, which the DCE then drops — matching C++ `-O3`. A
  **non-determining** leading const (`true && rest`, `false || rest`)
  contributes nothing and is dropped: if ≥2 operands remain it stays a logical
  op (`false || a || b` → `a || b`, sound for any types); if exactly ONE remains
  the result is `bool(operand)`, so the const is dropped **only when that
  operand is already bool** (`false || (x>0)` → `x>0`, via `produces_bool`: a
  comparison / `&&` / `||` / `!` / bool literal, or its M8 typed form) — `false
  || x` for a plain int `x` is left alone, since `bool(x) ≠ x`.
- performs **dead-code elimination**: an `if`/`while` whose condition folds to a
  literal has its dead branch dropped (`while (false)` removed). Crucially, a
  branch it proves dead is *eliminated, not folded* — auto-const only analyzes
  code it proves reachable (this is its DCE; eager fail-on-error in dead code is
  the *parser's* behavior for explicit `const`/literals, not auto-const's).
- **Safety (`prescan_blocked`)**: a slot is *not* promoted if the variable is
  captured by a nested function (the capture must stay an identifier), passed as
  the **first arg** of a builtin that takes it as an lvalue/identifier
  (`append`/`push`/`pop`/`insert`/`erase`/`intptr`, listed in
  `is_lvalue_arg_builtin` — a literal there throws `NotLValueEx`), used as a
  subscript/member base (`a[i]`, `a.k`), or is a `foreach`
  loop variable (implicitly reassigned each iteration despite its write count).
  Args to pure/user functions and read-only builtins are **not** blocked, so
  they fold — this is what lets pure-call folding and `isconst()` work.
- **Same early-failure rule as the parser:** a fully-constant expression in
  *reachable* code that throws when evaluated (e.g. `6/0`, a type mismatch) is
  **not** deferred to runtime — the exception propagates out of `resolve_names`
  and aborts before execution. `try/catch` does not catch it. The `runtime()`
  builtin is the documented opt-out: it is a non-const builtin, so it is not in
  the folder's const context and a call to it never folds; the containing
  expression stays a runtime computation (its *argument* is still folded, so
  `runtime(1/0)` still fails at compile time).
- **Pure-call folding.** `register_pure_funcs` first registers every
  effectively-pure NAMED function (`FuncDeclStmt::effective_pure`) into the
  folder's const `EvalContext` (`cctx`), which already holds the const builtins.
  Then a `CallExpr` with all-const arguments folds by simply `eval`-ing it
  against `cctx`: a pure func / const builtin runs and yields a literal;
  anything else (a non-pure func, `runtime()`, `print`, ...) is absent, the
  lookup throws `UndefinedVariableEx`, it's caught, and the call is left for
  runtime. This is how an auto-pure func's const-arg calls fold even though
  auto-pure is decided *after* parsing (explicit `pure` funcs already fold at
  parse time via the parser's const-eval).

Implementation notes: slots are never reused across sibling scopes
(`FuncState::next_slot` is monotonic), so the slot-keyed map can't collide.
The pass needs a *complete* tree traversal, but `for_each_child` deliberately
omits the nodes `walk()` handles itself (Block/for/foreach/try/`Expr14`), so
`prescan_blocked`, `register_pure_funcs` and the folders descend into those
explicitly.

**Pure functions: no observable side effects.** A function is pure iff it has
no side effects: it reads only consts + its params (+ calls pure functions),
nests no function, **and does not mutate a reference parameter.** The last
clause matters because mylang passes arrays/dicts/structs **by reference**, so
`a[i] = v` / `a.f = v` / `append(a, …)` on a param *is* observable by the
caller — such a function is NOT pure (mutating a **scalar** param is fine, it is
a copy; mutating a **fresh local** container is fine, it never escaped).
`func_mutates_input` (`resolver.cpp`) proves this with a small taint analysis:
a non-scalar param is tainted, an *identifier-lvalue* assignment from a tainted
value (`var b = a`, `var r = [a]`) taints the lhs, a `foreach` var over a
tainted container taints the var — then an element/field **write** via a tainted
base is a mutation. An element *store* `r[i] = a` does **not** taint `r` (it
writes the possibly-fresh `r`, it doesn't make `r` alias `a`) — exactly what keeps
the fresh-local builder `var r = [..]; r[i] = param` pure. Conservative
(a `clone(a)`/slice of a param taints the result, costing a pure-classification,
never soundness); the one residual gap is storing a param into a fresh empty
local then deep-mutating (`var r=[]; r[0]=a; r[0][0]=v`). A mutating function is
demoted to `effective_pure = false` for **both** auto-pure *and* an explicit
`pure func` (its `explicit_pure` — the user's word — is kept, so `ispuredecl()`
still reports it while `ispure()` does not; no error, to avoid breaking
conservative false-positives like clone-and-mutate). **Why redefining `pure`
this way costs ~no optimization:** a param-mutator can't const-fold (a const arg
is read-only → the write throws), isn't inlined (mutators are block-bodied), and
the for-range already excluded it — but it *enables* a sound pure-container-arg
for-range bound (`compute(arr)`); see the eval below.

**⛔ THE PARAMETER ESCAPE ANALYSIS (#93, `stamp_noescape_params` in
resolver.cpp) — A FALSE "SAFE" IS A USE-AFTER-FREE, SO EVERY RULE FAILS
CLOSED.** `func_mutates_input`'s transitive, escape-aware sibling: per
parameter, *can the reference this is bound to still be reachable after the
call returns?* The answer is a bit in `FuncDescriptor::noescape_params`, and
its consumer is **#94 THE BORROW BIND** — a reference argument bound with NO
retain, since the caller's slot holds one for the whole of a synchronous
call. Four rules a future editor must not soften:

- **It runs LAST in `resolve_names`, after `devirtualize_direct_calls`** — it
  reads `SymKind::global` (pass 2) and `direct_func_slot` (that call). A
  first version sat in `process_function` beside `func_mutates_input` and
  marked a global-writing function's parameter safe. The audit-table stage
  trap, with a dangling-pointer failure direction.
- **An unknown NODE SHAPE declines** (`esc_known_shape`). `for_each_child` is
  a dynamic_cast chain whose fallthrough means "no children", so a node kind
  missing from it HIDES occurrences — and a hidden occurrence is exactly how
  a parameter gets wrongly marked. `ForRangeStmt` is the live example, out of
  reach today only because this pass runs before `specialize_types`.
- **An unlisted BUILTIN captures everything** (`esc_builtin_transparent`). A
  listed one claims three things at once: stores no argument, does not return
  one, and invokes no callback. **Audit the third by grep, not by eye** —
  `sum` was listed on the reasoning that a sum is a number, but `sum(arr, f)`
  is a reduce; the awk that finds every builtin reaching `VmInvoker`/
  `eval_func` is quoted at the table. A new builtin needs no entry.
  A SECOND, weaker list (`esc_builtin_no_invoke`) claims only "runs no
  MyLang code" and gates the callback rule alone. It is an allowlist of
  NON-invokers on purpose: **that grep misses `map`, `filter` and `sort`**,
  which reach their callback through a shared helper not named `builtin_*`,
  so a list built FROM the grep would have declared `sort` safe. Inverted,
  the same imprecision costs only an optimization.
- **A callee it cannot NAME poisons the whole function**, not just the
  arguments handed to it: it might reassign the global that some caller
  passed us and drop the last reference mid-call. A HIGHER-ORDER BUILTIN's
  callback is a callee like any other, and IS named when it is an inline
  lambda or a global function slot nothing reassigns (`esc_callback_fn`) —
  the fixpoint then propagates its `unsafe` along an ordinary call edge.
  One reached through a parameter or a container element still poisons.
- **`esc_collect` walks function BODIES explicitly**, because
  `for_each_child` has no `FuncDeclStmt` arm and would otherwise stop at
  the top level. That is not only an enabler for naming callbacks:
  `written_slots` — the reassigned-global-slot set that decides whether a
  call may be resolved through `slot2fn` at all — was collected from
  top-level statements ONLY, so a function body doing `helper = other;`
  was invisible and a call to `helper` elsewhere was answered for a callee
  that may no longer be there.

Every rule is pinned by a `param_escape_analysis` row that fails when the
rule is deleted (watched, one sabotage build per rule), except the
reassignment guard, which is proven REDUNDANT by an `ML_CHECK` — no test can
reach it, and the check fires if a future change ever makes it load-bearing.

**⛔ #94 THE BORROW BIND — THE CONSUMER, AND ITS THREE RUNTIME DECLINES
(2026-08-15).** With the bit set, the callee slot takes a RAW BIT-COPY of the
caller's and skips the retain at the bind AND the release at the frame pop.
The decision is ONE function, **`vm_bind_arg`** (vm.cpp), shared by the C++
`fast_bind` and the emitted push's reference arm, and the slot records the
answer in **`LValue::borrowed`** (in `is_const`'s tail padding, so the slot
did not grow — the emitter bakes a 48-byte stride). Measured on
76_funcval_dispatch: **−13.5% instructions, 0.88x wall clock**, from
1,000,000 borrows — the whole call count.

Four things a future editor must not soften, each watched failing:
- **Only a REFERENCE is ever borrowed** (`t >= t_str`). The release scan
  skips a trivial slot, so a borrowed int would keep its flag set forever and
  the next call to reuse that window slot would rebind over it. Not
  hypothetical: it fired on the first run, in ackermann, whose un-annotated
  TEMPLATE parameter is not `binds_scalar` (so the analysis claims it) but
  holds an int.
- **Never a SLICE.** A slice registers itself in its parent's live-slices set
  when COPIED, and an element write to the parent detaches every registered
  view in place (`clone_aliased_slices`); a borrowed view is in no such set,
  so the write leaves it reading storage the detach just gave away — and
  freed, if the caller's slot held the last reference. Every other reference
  type's copy is a plain retain, which is what makes the bit-copy symmetric
  with the abandon.
- **`LValue::frame_release()` IS THE ONE RELEASE POINT.** Seven scans used to
  open-code `lv = LValue()`; a borrow makes the decision PER SLOT, and a scan
  that forgets it decrements a count the slot never took.
- **In the emitted push, the slot zeroing runs BEFORE the copy loop.** The
  qword at +40 covers `container_idx` and BOTH flag bytes, so zeroing it
  afterwards wiped the flag the reference arm had just set — a use-after-free
  from a store that reads as tidy-up.

REACH is `MYLANG_JITSTATS`' `arg_borrow` (the retain actually skipped) and
`arg_borrow_slice` (cleared by the analysis, declined by the value) — two
counters so "the tier ran" and "the tier was reachable and every value
declined" cannot be confused.

**The emitted INLINE borrow arm was BUILT AND REVERTED** — −3.27% Ir on
bench 76 (999,999 of its calls served by generated code) for **1.00x wall
clock**, while two benches that never borrow paid +1.44% / +0.48% Ir for
the emitted bytes. The guard-elision signature again. Full record,
including how to re-introduce it and the condition that would make it pay:
`plans/archived/inline-borrow-arm.md`. **Remaining cases, none built:** the
builtin-CALLBACK bind paths (`argv[i]` — sort/map/filter/make_dict) and the
tree-walker's `do_func_bind_params`.

**Auto-pure & const/pure introspection.** `func_body_is_pure` (`resolver.cpp`),
run after a function body is resolved, promotes a non-pure, capture-free func to
`effective_pure` when every free identifier (`sym.kind != local`) is
`is_const` (a const global/builtin/explicit-pure func) **or the name of a
function already proven pure** (`Resolver::pure_func_names`, populated in
walk order as `process_function` decides each), it nests no function, **and it
does not mutate a reference parameter** (`func_mutates_input`, above). So a
function that calls an *earlier* auto-pure helper is itself recognized pure —
`func f(x,y)=>add(x,y)` is pure once `add` is, so `f(1,2)` const-folds (the
whole pure chain folds at compile time, like `-O3`). **A SELF-recursive function
is auto-pure** when its body is otherwise pure: `process_function` optimistically
adds the function's own name to `pure_func_names` before checking its body, so a
recursive self-call counts as a call to a pure function (sound by induction — a
recursive call to a pure function is pure), undoing the add if the body turns out
impure. This is the purity the inliner needs to CSE duplicate calls when
unrolling a recursive body (`fib(n-3)` appears twice → compute once). **But a
recursive pure func is NOT eagerly const-folded** (`func_is_self_recursive`
excludes it from AutoConst's fold context — `register_pure_funcs` + the ctor's
`prior_pure` loop): evaluating `fib(40)` at compile time would hang. A const-arg
recursion folds only through the depth/budget-bounded inliner unroll. Still
conservative: a call to a *not-yet-decided* func (forward-referenced or
**mutually**-recursive) stays impure. **Cross-input** (REPL): `resolve_names`'s
`prior_pure` arg (the persistent runtime scope) seeds both `pure_func_names`
(so a new input's `f` calling an earlier-input `add` is recognized pure) and
`AutoConst`'s fold context with the earlier inputs' effectively-pure FuncObjects
(so a call to one *folds* across inputs — `func f2()=>f(1,2)` with `f`'s
instance from an earlier input becomes `=> 5`). The same `prior_pure` scope is
also handed to the **Inliner** (`Inliner(.., prior_scope)`): its `run()`
registers earlier inputs' **effectively-pure** functions (and their
instances) into `funcs`/`spec_funcs`, so a call to a prior-input pure function
**inlines / specializes** across inputs too — `func caller(a,b)=>f(a,2*b)`
then (later) `func c2(x)=>caller(x,3)` folds `c2`'s instance body to `x + 6`,
matching what one compilation (or C++ `-O3`) produces. Cross-input is restricted
to **pure** functions on purpose: the inliner only ever CLONES a callee body, so
reusing a prior retained decl is safe, but an *impure* prior function reads/
writes mutable global state that may differ at the new site (and its result
isn't compile-time-known anyway), so it is left a runtime call. Only pure
*functions* are seeded (never a runtime var); registration skips any name the
current input defines, so a redefinition wins and a redirected call already
points at the current input's own instance. **A prior input's body is
POST-specialization** (it already ran `specialize_types`, so it can hold M8
`TypedScalarExpr` nodes — which the inliner never sees in a normal single
compilation, since it runs *before* `specialize_types`). So
`for_each_child`/`for_each_child_slot` (and `is_foldable_expr`) were taught the
`TypedScalarExpr` case: substitution descends into it (else a param used twice —
once outside, once inside a typed `a*2` — is only half-replaced, dangling the
inner `a`), and a const-operand one refolds to a literal (`4*2`→`8`). Without
this, `func mk(a)=>[a,a*2]` inlined cross-input crashed with "Undefined 'a'".
**AutoConst folds an
EXPRESSION-bodied function's body** (`fold_func_body` handles a bare-expr
body, not only a `{...}` block — the older `fold_function` skipped the former,
so a pure call in `func g()=>f(1,2)` never folded).
`FuncDeclStmt::{explicit_pure, effective_pure}` back the
runtime builtins `ispuredecl()`/`ispure()` (they evaluate the arg to a
`FuncObject` and read its `FuncDeclStmt`). `isconst()`/`isconstdecl()` are
resolved in the auto-const pass (`fold_isconst`): `isconstdecl` is true for
parse-time consts and `const` params; `isconst` also accepts auto-const vars and
auto-const params. All four are registered as runtime builtins (with fallback
bodies) so the names resolve even when the pass doesn't fold them.

**Function inlining & specialization** (design record:
`plans/archived/function-inlining.md`). The pieces: "inlined-at" chains
(`InlineCtx`, `errors.h`) keep backtraces identical with inlining on or off
(see *Inlined (virtual) frames* under the error model), **AST deep-clone**
(`Construct::clone()`, all node types), and the **size-only inliner**
(`Inliner` in `resolver.cpp`, run after `AutoConst`; gated by `-ni`).
It splices eligible direct calls — top-level, expression-bodied, non-capturing,
non-recursive, no nested function, arity match, body ≤ a node threshold (`-it
N`, default 24), sound arg use
(an arg is evaluated as often as the param is used; side-effecting args neither
dropped nor duplicated). The spliced body's params are replaced by the args
(which inherit the parameter occurrence's loc), and the whole splice is tagged
with an `InlineCtx`. **The inliner re-scans each splice** (`walk(slot, depth+1)`
after splicing), so a g-into-f-into-h chain collapses in one pass even when
declaration order defeats the bottom-up walk (h declared before the callees it
transitively reaches) or a call is newly exposed by the re-fold — this is the
"fixpoint." Two bounds keep it finite: `MAX_INLINE_DEPTH` (16) caps nesting so
mutual recursion (`a()=>b(); b()=>a()`) terminates, and `inline_budget`
(`max(4096, 8 * program nodes)`) caps total nodes added so breadth-doubling
(`f()=>g()+g()`) can't blow the tree up; hitting either bound just leaves the
remaining calls in place (still correct — they run at runtime). A re-scanned
splice's call site already carries an `InlineCtx`, so the new frame's parent is
that existing chain (not null) and `rebase` (in `tag_inline`) re-roots the
body's own chains under it — arbitrarily deep nesting renders correctly, and a
chain mixing inlined and physical/recursive frames shows them in order. The
inlined-frame flush is keyed off
`Exception::inline_origin_emitted` (a bool), **not** the loc once-guard: the
innermost inlined node's `Construct::eval` emits its frames once and sets the
flag, so an error that arrives with a loc already set (a builtin like
`append(tbl, 9)`, a not-an-lvalue assignment `d.k = v`) still keeps its frames.
`do_func_call` sets the same flag after its own call-site flush so the enclosing
`CallExpr` doesn't re-emit, while each physical call's flush stays unconditional
(multi-level inlined call sites all show). Backtraces for **body** errors are
byte-identical with/without inlining;
**known limitation** — an error *evaluating an argument* (e.g. an undefined var)
is attributed to the inlined callee rather than the call site (the arg node is
both the call-site value and the in-body operand). After splicing, the inliner
**re-folds** (`Inliner::refold`): a `MultiOpConstruct`, subscript, slice, member
access, or const-builtin call folds to a literal when its operands are
compile-time constants — scalar/array/dict literals *and* const globals (the
top-level const array/dict decls are seeded into `cctx` by
`seed_const_globals`). A `has_slotted_local` guard keeps it from dereffing a
missing frame (it never evaluates a node referencing a runtime local), and it
skips lvalue positions (an assignment target, an lvalue builtin's first arg) so
a write target is never turned into a value. So a const arg propagates into a
*non-pure* expression function (`f(3)` with `f(x) => x*10+g` -> `30+g`;
`a[0]` -> `10`, `len(a)` -> `3`, `tbl[0]` -> the element) — the half AutoConst's
whole-call folding misses. A non-inlined call to a **block-bodied** function
with const arg(s) is instead **specialized**: the body is cloned, those params
bound, and folded with DCE via `AutoConst::fold_specialized` (which *catches*
const errors and discards, so a runtime error never becomes a compile one) then
`refold`. Both **scalar** and **deep read-only array/dict** const args seed a
specialization: a read-only array/dict is sound to substitute because it is
only ever folded in read positions - `fold_reads` never rewrites an assignment
lvalue or an lvalue builtin's first arg, and (fixed 2026-08-02)
`fold_lvalue_reads` folds only a write chain's INTERIOR reads (the indexes),
never its base spine: folding the seed into `arr[j] = n`'s base produced
`Obj([...])[j] = n`, which the tree-walker survived (same NotLValueEx as the
un-specialized call) but the NO-FAIL codegen refused with NotLoweredEx - a
compile refusal of a legal must-throw-at-runtime program. Any *mutation* of
the seed throws the same error at runtime as the un-specialized call
(`prescan_blocked` gained a
`block_subscript_bases` flag; the relaxed seed set keeps the genuinely-unsafe
blocks — capture, lvalue-builtin first arg, callee, foreach var — but lets a
subscript/member READ base fold, since the
param decl is kept so an lvalue base can't dangle). The shrink decision uses
`count_all_nodes` (a *complete* traversal, unlike `node_count`, so a fold buried
in a kept `var t = a[0]+a[1]` rvalue is visible). If it shrinks, a shared clone
`name$sN` is registered (deduped by (func, const-arg tuple) — an array/dict keyed
by its `intptr` identity in `value_repr`, so the same const object shares one
clone — inserted at the root block's front) and the call redirected; the clone
keeps the same frame (no re-resolution) and a `FuncDeclStmt::display_name` makes
backtraces show the original name, not `name$sN`. A **tail call to a block-bodied
function** (`return f(args);` where f's body always returns — its last statement
is a `ReturnStmt`) is inlined *directly* (`try_inline_tail`): f's body block
replaces the return statement, sound because f's own returns become the caller's
returns and f never falls through. f's params are substituted (so they must be
non-reassigned and value-stable — `tail_arg_ok`: a caller local or a const
literal, never a global or side-effecting expr, since the body reads the param
at its use points rather than once up front), and f's locals are
**re-resolved**: a single `splice_tail` pass decides each identifier by its
ORIGINAL slot — a slot `< nparams` is a param (substitute the arg), a slot `>=
nparams` is a local (remap by `caller_fsize - nparams` into a fresh range at the
top of the caller's frame). The caller's frame (`FuncDeclStmt::frame_size`, or
the root block's `slot_count` for "main", threaded through `walk` as `fsize`)
grows by f's local count, capped at 64 (`Frame::live` is one 64-bit word); over
that, the call is left as-is. The spliced body is tagged with an `InlineCtx`
like an expression splice, so a runtime error shows f's virtual frame above the
caller's real one.

**Non-tail block-body inlining (`try_inline_block` → `InlinedCallExpr`).** A
call to a block-bodied function in ANY expression position (not just
`return f(args)`) is inlined by replacing the `CallExpr` with an
**`InlinedCallExpr`** (`syntax.h`, a `SingleChildConstruct` so every
`for_each_child` walk auto-descends into its body): the callee's cloned,
param-substituted body. `InlinedCallExpr::do_eval` runs that body behind its OWN
return boundary — it points `ctx->flow` at a stack-local `FlowState` for the
body's duration and restores it (so `flow` is no longer `const`), making the
body's `return`s yield THIS expression's value instead of returning from the
caller. No statement hoisting and no eval-order change — the node sits exactly
where the call was — and no child `EvalContext` (the body block is `scope_free`,
so it runs in place; far cheaper than the EvalContext+Frame+bind a real call
pays — ~1.4x on a call-heavy non-const loop). **Scope (`block_inlinable_decl`):**
**resolved** (params + locals slotted), non-capturing, no nested function
(`contains_func` — a COMPLETE walk; the plain `for_each_child` form missed a
closure that is a decl rvalue `var h = func[..]..`, which let an inline break the
capture), **non-recursive** (a COMPLETE `refs_uid` check), no scalar-param
reassignment (a reassigned param is a by-value copy — substituting the arg would
alias it). **Args (use count by SLOT):** a **value-stable** arg (`tail_arg_ok` —
a caller LOCAL or const literal the body can't reassign) is substituted DIRECTLY;
**any other arg** (a non-trivial expression, a global, a side-effecting call) is
bound to a fresh frame **temp once** at the top of the body (`$a = arg`) and the
param reads that temp — **"args as locals"**. Evaluating once captures the
call-time value, so a body that mutates the passed-in global, or a multi-use
side-effecting arg, stays sound (the earlier `sub_ok` allowed a bare global
identifier, an unsoundness); it also lets `f(a+b)`, `f(g())`, `f(global)` inline,
and is what the recursion-unroll needs (a self-call's arg is `n-1`). The size
gate is the **cost model**: `body_weight` (a weighted
node sum, weights from `--weights`/`run_weight_bench`: a CALL is ~21x an arith
op, assign 11, if 7, return 3) must be **below `CALL_WEIGHT` (21)**. **Bodies WITH
locals are handled** (v2): `splice_tail` substitutes the params (by slot) and
**remaps the locals** into a fresh range at the top of the caller's frame (which
grows by the local count, capped at 64) — the same machinery tail-inline uses;
two inlines in one expression get distinct ranges. **Block-inline is SUPPRESSED
inside a loop condition** (`walk`'s `no_block` flag, threaded into `ForStmt`/
`WhileStmt` conds): the for-range specializer (run later) caches a pure-call
bound, but only recognizes the call shape, not an opaque `InlinedCallExpr`, so
inlining a `for (i; i < f(n); i++)` bound first would turn a once-evaluated
`ForRangeStmt` into a per-iteration `ForStmt` — a regression; the call is left for
for-range (expression-body inlining is NOT suppressed there — it yields a
for-range-recognizable arithmetic expression). Bounded by `MAX_INLINE_DEPTH` +
`inline_budget`, re-scanned (depth+1) so nested calls collapse, `InlineCtx`-tagged
for backtraces. A const-arg call to such a (auto-pure) func is folded by AutoConst
*before* the inliner, so block-inline only fires on the non-const-arg calls.
Registered in `block_funcs` independently of `funcs`/`spec_funcs` (a func can be
both tail-inlined and block-inlined; walk order: expr-inline, tail-inline,
block-inline, specialize). coderender renders an `InlinedCallExpr` as its
value (a single-return body) or an `inlined <name>` marker.

**Guard bodies inline to an EXPRESSIBLE ternary, not an `InlinedCallExpr`.** An
`InlinedCallExpr(Block(... return ... return ...))` sitting in expression
position is NOT writable MyLang (a block-with-returns is not an expression) — an
optimized AST that isn't a subset of the source AST. So when the spliced body is
a **temp-free guard chain** — `{ if(c1) return a1; ...; return b; }`,
`guard_to_ternary` — it is converted to the equivalent **`TernaryExpr`**
`(c1 ? a1 : (... : b))`, real code that runs in the caller's frame like any
expression (no flow boundary, no `InlinedCallExpr`). "Temp-free" requires the
args to be DIRECT-substituted, not bound to temp statements; for a **pure**
callee an arg is side-effect-free, so a cheap one (`body_weight ≤ ARG_SUBST_MAX`,
which excludes a nested call) is substituted directly and re-evaluated at each
param use (sound: a pure body can't change the arg's value). The recursion
unroll therefore produces **nested ternaries** (fib →
`(n-1<2 ? n-1 : fib(..)+fib(..)) + (n-2<2 ? n-2 : ..)`), and the frontier
self-calls in the ternary branches still hit the per-frame cache. **A body with
write-once LOCALS** is first run through **`collapse_locals`** (copy propagation):
a `var t = <cheap side-effect-free expr>` whose slot is written once is
substituted into its uses and the decl dropped, so `{ var t=a*a; return t; }`
collapses to `{ return a*a; }` → a temp-free guard chain → a ternary/expression
(`uc(x)` → `x*x+1`). Sound — a write-once local with a side-effect-free rvalue is
just a name for that expression (re-evaluating a cheap rvalue is exact). With
this, the small bodies that used to inline as a non-expressible
`InlinedCallExpr(Block(...))` now inline as **expressible** code; the
`InlinedCallExpr` form survives only for a residual that can't collapse (a
reassigned / expensive / side-effecting local), which is almost always already
over the block-inline weight gate (so not inlined at all). For this,
`for_each_child_slot` gained `TernaryExpr` / `CoalesceExpr` cases — previously
absent, so a call inside a `? :` was never inlined or devirtualized (a latent
missed-optimization, now fixed). coderender no longer prints a per-node `inlined`
marker (a ternary is self-evidently real code; the flood made a deep unroll
unreadable) — only the `InlinedCallExpr` case emits one.

**Int-algebra fold (`fold_int_arith`, run once after the inline fixpoint).** A
bottom-up algebraic simplifier for **int-typed** (`th == i`) arithmetic chains,
in three parts: (1) **`+`/`-` constant combining** — `(n-1)-1`→`n-2`, so the
unrolled frontier reads **`fib(n-3)`/`fib(n-4)`** not `fib(((n-1)-1)-1)`;
(2) **`+`/`-` like-term collection** (`fold_addsub`) — structurally-equal
side-effect-free operands merge by net coefficient: `a+a`→`2*a`, `a-a`→`0`,
`a+b-a`→`b`, `x+1+x+2`→`2*x+3` (`expr_equal` compares ids/int-literals/arith
chains; a side-effecting operand is kept verbatim per occurrence); (3) **`*`
constant-factor combining** (`fold_mul`) — `x*2*3`→`x*6`, `x*1`→`x`, `x*0`→`0`
(the last only when the non-const factors are side-effect-free, so dropping them
is sound). **SOUND for int ONLY** — `+`/`-`/`*` are associative & commutative mod
2⁶⁴ under `-fwrapv`, so regroup/reorder/merge is exact — hence **gated on
`th == i`**: a float chain (non-associative rounding) or a string `+`
(concatenation) is never touched. `/` and `%` are excluded (truncating int
division isn't associative). A rebuilt chain's base op is `Op::invalid` (asserted
in `eval_first_rvalue`, so a structural mistake aborts rather than miscomputes).
**A rebuilt chain carries the ORIGINAL's base fields** (task #75, 2026-07-28):
fold_addsub/fold_mul/make_int_mul copy_base_fields from the node they replace -
a fresh chain with EMPTY fields lost the caret AND the inlined-at chain for
errors thrown through it, which (compounded by the TYPED eval path bypassing
Construct::eval's flush hook) made the tree-walker render NO backtrace where
the VM rendered the virtual frames. TypedScalarExpr::eval_int/eval_float now
flush their own chain on an unwinding exception (a chain-less node calls the
un-wrapped *_body directly - zero cost); pinned by the
typed_inlined_backtrace_parity test (byte-identical engines).
This is the type-narrowed realization of the long-deferred "algebraic
simplification" pass, now safe because inference proved the type. **Caveat:** an
UNTYPED template base
(`func fib(n)`) has no `th`, so its body stays literal (`fib(((n-1)-1)-1)`) — its
arithmetic CAN'T be folded soundly (a future instance might be float); a concrete
/ `int`-annotated function (or any int instance) folds. The base-case guards are
never dropped (`(n-1<2 ? ... )` stays — a flat "4 calls" is unsound for a
variable `n`).

**Recursion unroll + per-frame pure-call cache (the fib win).** A pure,
TREE-recursive function (≥2 self-calls, `func_is_cacheable_recursive`) is
admitted to `block_funcs` and **unrolled in place** ("unroll the definition"):
the walk inlines its own self-calls, to a **per-function depth**
(`rec_unroll_depth`) chosen from the COST MODEL — keep adding levels while the
projected body weight (×branching each level) fits `REC_UNROLL_BUDGET` (tuned so
fib, w0≈71/branching 2, gets 2 levels; tribonacci, w0≈97/branching 3, gets 1; a
body weight >~150 gets 0 — 2 levels of a huge body is a waste, the fib-class is
small). The runtime bound is **recursion DEPTH** (`rec_depth`, bumped around the
recursive re-scan), NOT body size: a depth cap expands EVERY self-call to the
same depth (BALANCED), while a size cap stops mid-level and leaves some
self-calls un-expanded (LOPSIDED, which dedups far worse — e.g. a 3-self-call
`tribonacci` lost its third branch under the old size cap). A LEVEL is
all-or-nothing (every self-call at it is expanded). Two more correctness points:
each self-call splices a clone of the **saved ORIGINAL body** (`rec_orig`), not
the in-place-growing body, so the unroll does not compound; and `REC_NODE_CAP` is
a high size BACKSTOP for a pathological body.
The unroll's **duplicate self-calls**
(`fib(n-1)` and `fib(n-2)`'s bodies both call `fib(n-3)`) land in ONE frame (the
`InlinedCallExpr` shares the caller frame) and **dedup at runtime via a per-frame
cache**: the
inliner sets `FuncDeclStmt::cache_results`, the devirt pass turns a call to such
a func into a **`CachedCallExpr`** (a `DirectCallExpr` subclass — a SEPARATE node
so the plain `DirectCallExpr` path pays NO per-call cache check), and
`cached_call` (eval.cpp) checks the caller `Frame`'s lazily-allocated `PureCache`
(`{func, arg values}` → result): hit → reuse, miss → call + store. **Sound**:
lazy (only calls actually made are cached → never evaluates a call the program
wouldn't, so a recursion whose base case misses negatives — `fact(-1)` — can't
diverge), and frame-scoped (the cache dies with the frame → not global
memoization, which would be the script's job). Only a **scalar** result is cached
(a pure func may return a fresh mutable container; caching it would alias it
across callers). **The unroll only pays WITH the cache** — the per-frame dedup is
what reduces the exponential; the unroll ALONE grows the body without dedup, so
it is neutral (1 level) to harmful (deeper). So depth is tuned for the
cache-on default; **`-npc`** disables the cache (the unroll still runs) to MEASURE
its contribution. **Effect: ~14x on naive `fib(32)`** (0.01s vs 0.14s CPython;
`-npc` 0.5s, `-ni` 0.28s — i.e. cache off is *slower* than no-inline, which is
why the two ship together); more when a caller calls the same `fib(k)` repeatedly
(the cache dedups those too); broad-suite geomean a slight net win. Correct for
`fib`, `ackermann`, container-returning tree recursions. The unrolled body is
**expressible** (guard bodies → ternaries, locals copy-propagated, arithmetic
int-folded), so `:show fib$0` reads
`(n-1<2 ? n-1 : fib$0(n-2)+fib$0(n-3)) + …` — real, writable MyLang. The
base-case guards CANNOT be dropped for a variable arg (a flat "4 calls" is
unsound — `n` could be 2). The only inlining-adjacent item still **deferred** is
*general* flow-sensitive type narrowing beyond the null-narrowing the check pass
already does (the int-algebraic part is done — see `fold_int_arith`).

## Static type inference (`inferencer.cpp`)

A **whole-program, compile-time** pass (`infer_types(root)`) that gives every
variable, parameter, and function return a fixed static type and **rejects type
violations before the program runs**. Gated by `-nti` (default ON; also runs
under `-nr`, since type-checking is validation). It runs *after* parsing but
*before* `resolve_names`, on the clean tree (not the inlined one), and stores
nothing on the AST — it owns an `StaticTypeArena` (`statictype.h`) and side
tables,
so it
leaves the tree untouched for the later passes (the one exception is the
named-argument desugaring below, a deliberate lowering). Full design + the
decisions behind it: `plans/archived/type-inference.md`,
`plans/archived/type-inference-questions.md`.

- **Named-argument desugaring (`lower_named_args`).** A call may pass arguments
  by name (`f(x: 1, z: 3)`, see README *Named arguments*). The parser attaches
  the labels to `ExprList::arg_names` (a transient `vector<const UniqueId *>`,
  parallel to `elems`, empty == all positional) via `pArgList` (replacing the
  generic `pList<ExprList>` for call args). The desugaring to positional form
  (filling a skipped *interior* optional with an explicit `LiteralNone`,
  enforcing the strict ordering — a leading run of positional args, then names
  in declaration order; reordering / duplicate / unknown name / missing-required
  is a compile error) happens in **two places**, because a named call must
  const-fold *identically* to its positional twin (the syntax cannot cost an
  optimization):
  - **At parse time** (`pTryDesugarNamedCall` → `pDesugarNamedArgs`,
    `parser.cpp`): when a named call's callee is a parse-time-const `pure func`
    (its `FuncObject` is available, giving the param `Identifier`s), the parser
    desugars *before* its const-fold step, so `const C = f(x: 1, z: 5)` folds
    just like `f(1, none, 5)`. A non-pure / forward / builtin callee can't be
    resolved here, so the parser marks the call non-const and leaves the names
    for the inferencer.
  - **In the inferencer** (`lower_named_args` → `lower_call_named_args`): right
    after the structural pass and **before** the fixpoint/check, every remaining
    named call is desugared. It resolves the callee with `callee_funcinfo` (so
    names need a directly-named function — a `dyn`/func-value/builtin callee is
    the error here), then maps labels to params (by interned name) the same way.

  After both, the tree holds only positional calls, so the fixpoint, the check
  pass, `resolve_names`, the optimizers, `specialize_types`, and eval **never
  see a name** — named args have provably zero effect on them. The inferencer
  lowering is syntactic, not type-checking, so it runs even when checks are
  disabled: `-nti` no longer makes `infer_types` a full no-op — it sets
  `checks_enabled = false`, and `run()` still does the structural pass +
  lowering, then returns before the fixpoint.

  Both sites share **one** mapping implementation, `desugar_named_call`
  (`syntax.cpp`, declared in `syntax.h`): it takes the call plus a normalized
  `vector<ParamSpec>` (`{const UniqueId *name; bool opt;}`) and owns all the
  rules (label→position by interned name, the strict ordering, the `none`
  fill, the errors). Each caller just builds the `ParamSpec` view from its own
  param representation — the parser from a `FuncObject`'s param `Identifier`s
  (`uid`/`opt_mod`), the inferencer from its `TypeSym`s (`name`/`opt_decl`) —
  and the callee-resolution + "names need a directly-named function" decision
  stays caller-specific. So a named call is rewritten, and therefore optimized,
  byte-identically wherever it is lowered. (`intern_msg`, the stable-message
  helper the compile errors need, is likewise shared from `errors.h`.)

- **Static types** are `StaticType` (`statictype.h`), distinct from the runtime
  `Type *`:
  `None` (the only-none / not-yet-pinned unit), `Bool`, `Int`, `Float`, `Str`,
  `Array<elem>`, `Dict<k,v>`, `Func(params)->ret`, `Exception`, `Dyn` (explicit
  top), each with an `opt` (nullable) flag. The lattice ops are
  `assignable`/`join`/`unify`/`equal` with the numeric promotion chain
  **`Bool <= Int <= Float`** (`join` climbs by numeric rank: `join(bool,int)` is
  int, `join(int,float)` is float; `assignable` lets bool fit int/float;
  arithmetic over bools promotes to int — `binop_result`'s `arith_join` bumps a
  bool result to int, so `true+true` is int 2; comparisons/logical → `Bool`).
  `None`/`opt` give nullability; mixed container elements fall to `Dyn`; a
  scalar/kind conflict is an error.
- **Three passes**: (1) *structural* — build scopes, one `TypeSym` per
  declaration, resolve every `Identifier` to its `TypeSym`, one `FuncInfo` per
  function; (2) *fixpoint* — **Jacobi** iteration: each round recomputes every
  symbol/return type into `acc` (the `join` of all contributions) while reading
  the previous round's stable `type`, then commits; reading stable values makes
  rounds order-independent, and since kinds only climb the lattice a `join`
  conflict (e.g. `int` vs `str`) is a real, stable error raised immediately;
  (3) *check* — with final types, validate every operator, call, assignment, and
  return, throwing on a violation.
- **Inference rules of note** (the non-obvious ones): a never-(concretely-)
  called function's parameter finalizes to `Dyn` (its body must still
  type-check), while an unconstrained *local* finalizes to `None`. Param
  nullability is *declared* (`opt`/`opt dyn`) **and enforced**: the
  mandatory-`opt` rule errors if a non-opt param can actually receive `none`
  (see below) — this holds for `dyn` params too (a nullable dyn must be
  `opt dyn`; nullability is orthogonal to dyn). Local/return nullability is
  *inferred* (`None` joined with a concrete `T` is `opt T`; a `dyn` var that
  gets `none` becomes `opt dyn`). `runtime(x)` returns `Dyn`
  (its documented opt-out: it defers to runtime). `==`/`!=` are always
  well-typed (→ int); ordering is numeric-or-string. `str + anything` → str.
  **`int OP dyn` → `dyn`** — the NATURAL result of mixing a concrete with a
  variant; it is NOT forced to the concrete type. So a FRESH `var r = 3 + d`
  (d dyn) correctly infers `dyn` → `DynRequiredEx` (must be `var dyn r`), and
  `var dyn r = 3 + d` holds the actual result (int/float/…). The accumulator
  `var s = 0; s = s + x` keeps `s` int NOT by typing `s + x` int but by the
  **dyn-into-concrete COERCION** (see the mandatory-`dyn` section): `s`'s type
  comes from its non-dyn contribution (`0`), and the `dyn` rhs is a
  runtime-checked coercion.
  Higher-order builtins (`map(func,c)`, `filter(func,c)`, `sort(c,func)`,
  `make_dict(keys,gen)`,...) feed the container's element type into the
  callback's params (named **or** inline lambda; `callee_funcinfo`) — for
  `make_dict` the callback's param is the KEYS array's element type (the key),
  and the result is `dict<K, V>` (K the key type, V the callback's return), the
  dict analogue of `make_array(N,gen)` (whose callback param is the int index).
  This feeding **defers while the container type is Unknown** (never feeds `dyn`
  — a premature `dyn` param is sticky and poisons the callback's ret, leaking a
  sticky `dyn` into any accumulator of the higher-order result, e.g.
  `total += make_dict(ks,f)[k]` in a loop with an inline `f`); `make_array` is
  immune since it feeds `int` unconditionally. A `var f = <lambda>` binds `f`'s
  `TypeSym` to the lambda's `FuncInfo`, so calls to `f` type its params and
  check arity.
- **New surface syntax**: the `opt` and `dyn` keywords, usable as modifiers on a
  parameter (`func f(opt x, dyn y)`) or a var/const decl (`var dyn z = ...;`,
  `var opt w;`), and **combinable as `opt dyn`** (in that order) on either.
  `opt` = nullable (may hold `none`); `dyn` = dynamically *typed* (variant — any
  type; type ops are runtime-checked). **Nullability is orthogonal to `dyn`
  (Phase B):** a bare `dyn` is *non-null*, `opt dyn` is nullable — so the four
  combinations are `T` / `opt T` / `dyn` / `opt dyn`. Implemented as
  `Identifier::{opt_mod,dyn_mod}` (params, via `pFuncParam`) and
  `pFlags::{pInOptDecl,pInDynDecl}` (decls; both can be set, for `opt dyn`).
  In `StaticType`, `opt dyn` is `g_dyn[1]` (the opt-`Dyn` ground); `with_opt`
  carries
  the opt bit onto a `Dyn` kind, and `join` keeps it when a mix collapses to dyn
  (`dyn | none` → `opt dyn`).
- **Explicit type annotations** (`int x = 5;`, `func f(str s)`,
  `array a = [...]`): the primitive type keywords `bool`/`int`/`float`/`str`/
  `array`/`dict` may replace `var` on a decl/`for`-init or precede a parameter
  name, combinable as `[const] [opt] TYPE name`. They are **not lexer keywords**
  (they stay the builtins `int()`/`array()`/…); the parser disambiguates by
  one-token lookahead — a type name is a type only when the next token (after an
  optional `const`/`opt`) is the variable name (`pAcceptDeclPrefix` in
  `parser.cpp`, shared by `pStmt` and `for`-init; uses `TokenStream::peek`). The
  annotation rides on `Identifier::decl_type` (a `DeclType` enum, `syntax.h`),
  threaded from the prefix via `ParseContext::pending_decl_type` and **also
  propagated by the resolver from the declaration to every use** (so a
  reassignment can coerce). Semantics, in the inferencer (`TypeSym::ann`): a
  **scalar** annotation *pins* the symbol's type (`reset_round` seeds the
  declared `StaticType`, `contribute` keeps it and checks each value is
  `assignable` —
  so `int x = 3.5` / `int x = 5; x = 2.5` / a wrong-typed arg to a typed param
  are errors, while `float f = 3` widens). A **non-`opt` typed var can never be
  `none`**: `int a = none` / a later `a = none` / `Point p; p = none` are a
  `NullabilityEx`, checked in the **check pass** (`Expr14` branch, via
  `ann_scalar_static_type` + `is_optish` on the rvalue) — *not* `contribute`,
  which
  defers on `none` so a transient none during the fixpoint (an `array(N)`
  element before a write) isn't misflagged. A plain `var x` (no annotation) is
  implicitly nullable and exempt; `int? a` / `opt int a` accepts `none`.
  `array`/`dict` are **generic** (only
  the kind is checked, by `enforce_decl_types` in the strict block — element
  types stay inferred, so `array a = [1,2,3]` is still flat `array<int>`). A
  scalar error is gated by `strict_dyn` (off for the `-a`/`-dti`
  non-strict passes). **Runtime:** `coerce_to_decl_type` (`eval.cpp`) does the
  numeric widenings (float←int/bool, int←bool) so a typed-float var/param holds
  a float — at the decl/assign (`handle_single_expr14`, `op == assign`, lvalue's
  `decl_type`) and at param bind (`bind_param`). **It NARROWS NOTHING and ERRORS
  on a bad type** (a `float` into an `int` throws, never truncates — use an
  explicit `int(x)`); a statically-typed rvalue can only be widening (the check
  pass rejects a narrowing at compile time), so the throw fires ONLY for a `dyn`
  value whose *runtime* type doesn't fit (the dyn-into-concrete coercion below).
  `none` passes through (nullability is a separate `opt` check). Auto-const
  inlining
  (`resolver.cpp`) and the parser's const-scalar inlining both coerce too (their
  own `coerce_decl_scalar`/`check_coerce_const_scalar` copies, since a `const`
  scalar is inlined *before* the inferencer runs — that path also does the
  type-check the inferencer can't). An **uninitialized** typed decl
  (`int x;`) gets the type's zero value (`zero_value_literal`: 0/0.0/false/""/
  []/{}), or `none` when `opt`.
- **A user STRUCT type as an annotation** (`A obj;`, `A obj = A(10)`): a struct
  name is a type in declaration position too (`StructName name`).
  **Context-free recognition:** `pAcceptDeclPrefix` (and `pFuncParam`) decide
  `A x` is a typed declaration by SHAPE alone — a (non-keyword) `IDENT`
  followed, after an optional `?`, by another `IDENT` (the var name). An
  `IDENT IDENT` run
  is never a valid expression (MyLang has no juxtaposition), so this needs **no
  symbol-table lookup for the parse decision** — the grammar stays context-free
  (we deliberately avoid the C "typedef" hack of consulting the symbol table to
  decide decl-vs-expr). `A(...)`/`A.x`/`a = ...` don't match the shape and stay
  expressions. *Then* a SEMANTIC step resolves the type name via
  `lookup_struct_type` (an identifier bound to a `StructTypeDef*` in the const
  ctx; needs const-eval on, since structs register their descriptor there at
  parse time): a name that doesn't resolve to a struct type is a clear
  `SyntaxErrorEx` ("'foo' is not a type"), not a silent fall-through.
  **Decl-vs-ternary:** a `T ? name` run is ambiguous with a ternary
  (`flag ? a : b`), so when a `?` was seen the scanner requires the token after
  `name` to be a decl terminator (`is_decl_terminator`: `;` `=` `,` `}` EOF) —
  otherwise (`:`, `(`, an operator, …) it bails to expression parsing **before**
  `lookup_struct_type` runs, so a non-struct condition name isn't wrongly
  rejected as "not a type". A plain `T name` (no `?`) is unambiguous and skips
  the check. Rides on
  `Identifier::decl_struct` (the `StructTypeDef*`) with `decl_type ==
  DeclType::strct`, threaded via `ParseContext::pending_decl_struct`. The
  inferencer pins it exactly like a scalar annotation:
  `ann_scalar_static_type` returns
  `A.struct_ty(ann_struct, ...)` (the TypeSym gains `ann_struct`), so
  `reset_round`/`contribute` pin the var and reject a wrong struct
  (`A x = B(...)` / a later `x = B(...)` → `TypeMismatchEx`, via `struct_def`
  identity in `static_type_assignable`). **Runtime:** no coercion (a struct
  binds
  as-is). An **uninitialized** struct var **zero-initializes recursively**
  (`build_zero_struct_init`, `parser.cpp`): it desugars `A obj;` to the
  constructor call `A(<zero per field>)` - 0/0.0/false/""/[]/{} per field
  kind, `none` for an `opt` field, and a nested zero constructor for a struct
  field. Being an ordinary `CallExpr`, it constructs a fresh value each eval and
  is type-checked like a hand-written construction. (`opt A obj;` / `A? obj;` →
  `none`.) **Parameters too** (`func f(A p)`): `pFuncParam` recognizes a typed
  param the same way (by shape - a type `IDENT` before the param name; then
  `lookup_struct_type` resolves it, an unresolved name being the same "not a
  type" error), sets the param's `decl_struct`, and the inferencer copies it to
  the param `TypeSym`'s
  `ann_struct` - so the param pins to struct `A` and `check_call` rejects a
  wrong-struct argument. (No runtime coercion; a struct binds as-is.)
- **Parameterized containers `array<T>` / `dict<K, V>`** (compose recursively:
  `dict<str, array<Point>>`, `array<array<int>>`). The element/key/value type is
  a **`TypeAnnot`** (`structtype.h`): a small recursive struct
  (`kind`/`opt`/`strct`/`elem`/`key`/`val`) built by **`pTypeAnnot`**
  (`parser.cpp`), carried on `Identifier::decl_annot` (vars/params) and
  `FieldDef::annot` (struct fields), shared (immutable after parse). **Parsing
  stays context-free:** `pAcceptDeclPrefix` recognizes `array`/`dict` + `<` by
  SHAPE (then `skip_angle_balanced` peeks past the balanced `<...>` to confirm
  the var name follows); `pFuncParam` and the struct-field parser do the same.
  Nested generics' merged closing token is handled by **`pAcceptCloseAngle`**
  (the `pending_gt` counter splits a `>>`/`>>>` across levels - the C++11
  trick), so no `>>` lexer change. The inferencer's **`annot_to_static_type`**
  turns a
  `TypeAnnot` into an `StaticType`; `ann_scalar_static_type` returns it (so a
  parameterized
  container is **pinned to its full type** exactly like a scalar - `reset_round`
  seeds it, `contribute` checks each value/element is `assignable`, the non-opt
  `none` rule applies), and `field_static_type` uses `fd.annot`.
  `enforce_decl_types`
  (the generic kind-only check) skips a pinned (`ann_annot`) symbol. A wrong
  element type (`array<int> a = ["x"]`, a reassign, a struct-field arg, a
  dyn-laundered `append(a,"x")`) is a compile error. **Flat storage:** the
  existing `ArrHint` path makes a typed array flat - and an **empty** typed
  array now starts flat too (`LiteralArray::do_eval` honors
  `flat_i`/`flat_f`/`flat_b` for `elems.empty()`, matching the existing `flat_s`
  case), so `array<int> a; append(a, 5)` stays unboxed. **The explicit/
  inferred `= []` form too (2026-07-18 profile #1):** `eval_literal_obj`
  gained the flat_i/f/b empty-array arms (only flat_s existed), so
  `array<int> e = []` AND the inferred `var a = []; push(a, i)` (typed
  array<int> by the fixpoint) start flat — the whole grow-from-empty class
  ran on general 48-byte LValues before (bench 13: 3x faster fixed). Generic `array a` /
  `dict d` (no `<...>`) are unchanged (element inferred). See
  `plans/archived/typed-containers-syntax.md`.
- **Compile-time TYPE QUERIES: `type`/`decltype` (-> `Type` object),
  `typestr`/`kindstr` (-> string).** All four are non-const builtins with an
  **UNEVALUATED operand** (like C++ `decltype`/`sizeof` - the arg is never
  evaluated). `fold_type_query` (`inferencer.cpp`, run in the **check pass**
  where types are final) recognizes the call, takes the argument's STATIC type
  (`decltype` requires an identifier-in-scope, else `TypeMismatchEx`/
  `WrongArgCountEx`; the others take any expression via `type_of`), and
  **replaces `args->elems[0]`** with the folded literal: a `LiteralStr` for
  `typestr` (`static_type_to_string`) / `kindstr` (`static_type_kind_string` -
  the bare kind,
  matching runtime `TypeNames`), or a baked **const `LiteralObj` Type object**
  (`build_type_value(StaticType)`, recursive) for `type`/`decltype`. It also sets
  **`CallExpr::tq_folded`**, so the call (which now just returns `args[0]`) is
  **ELIDED by BOTH engines** — the VM at codegen (a `LoadConstV`/
  `LoadLiteralObjV` of the baked literal, no builtin call), the tree-walker in
  `CallExpr`/`DirectBuiltinCallExpr::do_eval`. The **runtime builtins run ONLY
  for a NON-folded query** (an `Unknown`-typed arg, or `-nti`, where no fold
  ran): both the tree-walker `func` AND the VM's `func_v` (a **dual-ABI**
  `make_builtin_customv` registration) always build the `Type`/string from the
  runtime VALUE (`make_runtime_type_value` - a flat Type; `reflect_typeof` for
  the strings). The `tq_folded` FLAG - not a node `dynamic_cast<Literal>` check -
  is what keeps this `-nti`-correct: a user's own `typestr("hi")` is a literal
  too, so under `-nti` it must still report `"str"`, not `"hi"` (the old
  node-check heuristic was a latent tree-walker bug, now fixed). `Type` is a
  native composite type (`native_struct_type_def`,
  recursive via `opt Type` elem/key/val) registered in `struct_by_name` and
  typed by `builtin_result`. So `type(a)?.elem?.kind` works (the `opt Type`
  elem/key/val are read with optional chaining `?.`, or narrowed with `if`);
  `typestr(x)`/`kindstr(x)` are the cheap string forms. The arg-slot
  rewrite (not a whole-node replacement) needs no slot-based inferencer walk
  (`args->elems` is a direct vector). The `?`-suffix nullability format
  (`static_type_to_string`) matches `:type` and error messages.
- **Nullable `?` suffix, `~` short form, `null` alias.** `?` is a token
  (`Op::questionmark`, `operators.h`) that is the canonical short form of `opt`:
  `int? x` ≡ `opt int x`, `var? x`, `dyn? x`, `array? a`. `pAcceptDeclPrefix` is
  a run-scanner that consumes a prefix of `{const,var,dyn,opt,?,TYPE}` ending at
  the name — `dyn` is now a standalone decl starter (`dyn z;`, not only
  `var dyn z`), and a leading `?`/`opt` alone is *not* a starter (needs
  const/var/dyn/TYPE), so `int(5)`/`x = 5` stay expressions. **Param-only short
  forms** (`pFuncParam`, rejected in body decls since they're not in
  `pAcceptDeclPrefix`): a leading **`~`** = `dyn` (reusing the otherwise-unused
  `Op::bnot` token), and a trailing **`?` on the name** = `opt` — so
  `func f(x, y?, ~z?)`. `null` is a keyword (`kw_null`) the parser treats
  identically to `none` (a `LiteralNone`). The opt flag from `?` flows through
  the existing `pInOptDecl`/`Identifier::opt_mod` path, so inference/runtime
  need no `?`-specific code.
- **Errors** are compile-time (`DECL`-style plain `Exception`s, **not**
  `RuntimeException`s, so script `try/catch` cannot catch them; `errors.h`):
  `TypeMismatchEx` (type change / bad operator / wrong arg type / not callable),
  `NullabilityEx` (`none`/`opt` used where a non-opt value is required),
  `WrongArgCountEx` (arity), `DynRequiredEx` (the mandatory-`dyn` rule below),
  `OptRequiredEx` (the mandatory-`opt` rule for params, below), `ShadowingEx`
  (a `foreach` loop var written WITHOUT `var` that would shadow an outer
  variable — `ForeachStmt::implicit_var`, checked in the inferencer's structural
  pass via `lookup` of the enclosing scope; `var` is now OPTIONAL in a `foreach`
  header and always DECLARES a fresh loop-scoped var — the old bare-name
  reuse-an-existing-var path is gone — so `idsVarDecl` is set for both forms).
  Each carries an interned custom message + a `Loc`.
- **Mandatory `dyn`** (`enforce_concrete_decls`, ON by default via
  `infer_types(strict=true)`, off under `-nti`): a plain `var`/`const` must
  infer a *concrete* type; if its type is `dyn` it throws `DynRequiredEx`
  demanding an explicit `dyn`/`var dyn`. Phase A (`strict_deep=false`) flags a
  top-level `dyn` — `array<dyn>` is tolerated; Phase B (the flag) would recurse
  into containers. Skips params (a never-called func's param is legitimately
  `dyn`), foreach loop vars (type derived from the container), and func names.
  Runs **after** the check pass, so a var that is `dyn` *because of* a real type
  error surfaces that error first. See `plans/archived/type-driven-specialization.md`.
- **dyn-into-concrete COERCION** (a plain `var` accepts a `dyn` value). `int OP
  dyn` is `dyn` (above), so `var s = 0; s = s + x` (x `dyn`) contributes `dyn`
  to `s`. Rather than widen `s` to dyn (which mandatory-`dyn` would then
  reject), a `dyn` value is **assignable to a concrete NUMERIC local** — a
  runtime-checked COERCION: `s` keeps the type of its NON-dyn contributions and
  the store coerces the dyn value to it (a wrong runtime type — a float into an
  int — throws; use `int(x)` to narrow). So `var s = 0; s = s + x` keeps `s`
  int, works iff `x` is an int at runtime. Mechanism: `contribute` records a
  `dyn` contribution (`round_got_dyn`) but does NOT join it (the accumulator
  collects only non-dyn contributions); `commit_round` decides — a NUMERIC
  accumulator + a dyn contribution → keep numeric and set `coerces_dyn`
  (sticky); a non-numeric / dyn-only var → fold the dyn back in → `dyn` →
  `DynRequiredEx`. For a `coerces_dyn` var the inferencer STAMPS the decl
  `Identifier::decl_type` (i/f) — so `resolve_names` propagates it and the
  store's `coerce_to_decl_type` fires, exactly like an explicit `int s`.
  Order-independent (the numeric-vs-dyn decision is at commit, not when the dyn
  arrived). A FRESH `var r = 3 + d` (only a dyn contribution) correctly stays
  `dyn` → `DynRequiredEx`; `var dyn r = 3 + d` holds the actual result.
  (`TypeSym::{round_got_dyn,coerces_dyn,decl_id}`.)
- **Mandatory `opt` for params** (`enforce_nonnull_params`, same gate/timing as
  mandatory-`dyn`): a parameter that can receive `none` from *some* call path,
  if not declared `opt`, throws `OptRequiredEx` **at the param's declaration**
  ("declare it 'opt'", or **"'opt dyn'" for a `dyn` param** — Phase B: even dyn
  params are null-checked). The check pass sets `TypeSym::received_optish` when
  a possibly-none argument reaches a non-opt param (`check_call`, where the
  nullability check now runs *before* the dyn type-escape, so it applies to dyn
  too), and this rule turns that into the declaration-site error — so
  nullability is *proven* (a non-opt param, dyn or not, is guaranteed never
  `none`, body uses it without a check), not merely checked per call site. A
  call to a function *value* (no decl to point at) still reports the old
  per-call `NullabilityEx`. The nullability analogue of mandatory-`dyn`; see
  `[[nullability-opt-roadmap]]`. **A dict read is non-`opt`:** `type_of` types
  `d[k]` / `d.k` (Subscript/MemberExpr on a `Dict`) as **`V`** (the value type):
  a missing key *throws* `KeyNotFoundEx` at runtime (or returns the default of a
  default dict), so the read is a value or an exception, never `none`. The
  explicit accessors are `get(d,k)` → `opt V` (nullable lookup) and `get!(d,k)`
  → `V` (value or throw); `dict(default_value)` → `dict<dyn, typeof default>`
  (a default dict). See the dict-access runtime notes below and
  `[[nullability-opt-roadmap]]`.
- **The defer-on-Unknown/None invariant (soundness of the fixpoint).** Any type
  computation (`binop_result`, `unary_result`, `elem_of`, `type_of` of
  Subscript/Slice/Member/CallExpr-callee, `accumulate_foreach`) that meets an
  operand the fixpoint hasn't pinned yet — `Unknown` (bottom) **or** a transient
  `None` (an `array(N)` element before a write pins it) — must return
  Unknown/defer, **never `dyn`**. A premature `dyn` is sticky (it climbs the
  lattice and never comes back) and permanently poisons a self-referential
  accumulator (`acc = (acc+i)*3`, `s += sum(a)`, a foreach loop var). The check
  pass re-validates genuine errors (`require_nonopt`, not-subscriptable) with
  the final types, so deferring during accumulate hides nothing. **When
  touching the inferencer, audit any new `return A.dyn_ty()` for this.**
  `-dti` dumps
  every identifier's inferred type + uses to find spurious `dyn`s. The
  invariant also applies to *contributions*, not just return types:
  `accumulate_call`'s `contribute_container` (for `append`/`push`/`insert`)
  defers when the element/key type is still `Unknown` — else it would
  contribute `array<?>`, whose **outer** kind isn't `Unknown` so `contribute`'s
  own pinned-symbol guard wouldn't catch it, tripping a PINNED global array's
  cross-input assignability check before a template-instance arg settles
  (`var g=[1,2,3]; func f(x){append(g,x);} f(3)`). The invariant also covers a
  **call to a TEMPLATE**: `type_of` of a `CallExpr` whose callee is a template
  returns `bottom` (defer), NOT the template's `ret` — a template's `ret`
  finalizes to `dyn` (it is never inferred in isolation), and instantiation is
  about to redirect this call to a concrete clone whose `ret` is the real type.
  Returning `dyn` would be sticky and survive the redirect. This is what made a
  **cross-input** `var x = fib(10)` (calling a prior-input, *pinned* template)
  reject with `DynRequiredEx` — `x` got the template's `dyn` before the redirect,
  stuck — which rolled back the instance, so a REPL template first called in a
  *later* input was left un-instantiated (unlike a script). With the defer, the
  call settles to the clone's type and the instance is retained + optimized
  exactly as in a script (`repl:` cross-input test).
- **Finalization of unconstrained symbols.** An unconstrained *param* or
  *foreach loop var* → `dyn` (could be anything); a plain local → `none`. A func
  with an Unknown *return* → `dyn` (it returns a value that depends on
  unconstrained inputs, e.g. a func only ever passed as a value); a func with no
  value-returning path → `none` (it contributed `none` to `ret_acc`). An
  unresolved identifier / callee defers to Unknown (so the enclosing var isn't
  forced to `dyn`, and the runtime `UndefinedVariableEx` surfaces); a *builtin*
  used as a value is genuinely `dyn`.
- **Null narrowing** (`check_if`/`narrow_target`, check pass only): inside a
  proven branch a nullable var reads as non-opt — `if (x != none)` / `if (x)`
  (then), `if (x == none) ... else` (else), and the guard clause
  `if (x == none) return/throw; ...` (rest of the block). Sound (the branch
  guarantees non-none). Not flow-narrowed elsewhere.
- **Const-container types are exact** (`static_type_from_value` recurses): a
  folded
  const array/dict is typed `array<T>`/`dict<K,V>` from its actual elements
  (heterogeneous -> `array<dyn>`; individual elements stay exact via const-fold
  of a constant-index access). Container element joins absorb `None`
  (`join_elem`) so `array(N)`-then-fill stays `array<int>`, not opt-element;
  an empty `[]` (`array<none>`) or a `dyn`-element container fits any
  `array<T>` (`static_type_elem_compat` — invariance relaxed at the bottom/top
  element).
- **Interaction**: const scalars are already inlined to literals before this
  pass runs, so it never sees them as symbols. A statically-known type error
  that used to surface as a runtime `TypeErrorEx`/`NotCallableEx` is now a
  compile error — to keep such an error catchable at runtime, make the value
  `dyn`. **Not yet done** (deferred): cross-statement narrowing beyond the
  patterns above.
- **FUNCTION SUBTYPING CHECKS THE SIGNATURE (option B, 2026-08-12) — and it
  is a TYPE-SOUNDNESS rule, not a style rule.** Assigning a function to a
  func-typed name requires the arity AND every *settled* param/return type to
  match; `dyn` anywhere opts that position out. It used to check **arity
  alone**, which made a function type a promise the runtime did not keep:

      func a(int k) { return k; }
      func b(int k) { return 2.5; }
      var g = a; g = b;          # was ACCEPTED
      var s = 0; s = s + g(1);   # -> s = 2.500000, though g : func(int)->int

  Every consumer of `g(1)` — the inferencer, M8, and each of the JIT's
  unboxed tiers — is entitled to treat the result as the `int` the signature
  promises, and none of them could. **This is the same principle as the
  brace-less-body note under *Invariants & hazards*: a hole in the type
  system is a hole in every unboxed tier built on it**, because they all
  elide their guards on the strength of inference's proof. Closing it here,
  once, is what lets a typed call-result tier be built with no per-call
  runtime guard.
  **TWO doors had to close, and finding only one is the trap:** an
  *annotated decl or an argument* goes through `static_type_assignable`, but
  a **REASSIGNMENT contributes through the fixpoint's `join`**, whose Func
  arm used to widen a conflicting component to `dyn` (`rj ? rj : g_dyn[0]`)
  — so tightening `assignable` alone changed nothing observable and the
  probe still ran. Both arms now consult `static_type_sig_compat`.
  **It DEFERS on an unsettled component** (Unknown / None / `dyn`), which is
  the whole reason the old code gave for staying arity-only: a callback's own
  param and return types are themselves inferred, so a strict rule would fire
  on inference ORDER rather than on a mismatch (`apply(sq, i)`, a func array,
  an inline `map` lambda — all three are pinned as ACCEPT cases). Only two
  settled concretes can refuse. `param_opt` is deliberately not compared —
  opt-ness governs call arity, which `check_call` enforces per site.
  Measured blast radius before landing: **0 of 96** corpus programs
  (bench/my + samples + tests/functional) refused.

### M8 — typed scalar specialization (`specialize_types`, the speed payoff)

After inference, `infer_types` stamps a `TypeHint` (`th`: `i`/`f`) on every node
it proved is a non-null int/float. **A `bool` node is stamped `i` too** — bool
flows through the int (`eval_int`) path, computing as `0`/`1`, which is exactly
its promoted value, so a typed scalar over bool operands is unboxed like int
(the boxing in `TypedScalarExpr::do_eval`/`LiteralBool` keeps a bool where it
must — a comparison/logical/`!` result, a bool literal — while arithmetic over
bools correctly yields int). `Construct`/`Identifier`/`Subscript`
`eval_int`/`eval_float` read a bool value/slot/flat element as `0`/`1`.
**⛔ SO `th == i` DOES NOT MEAN "int" - IT ALSO MEANS BOOL, AND A
CONSUMER THAT NEEDS THE VALUE MUST ASK (`Construct::th_bool`, added
2026-08-26).** The VM lost that distinction for four shapes:
`compile_boxed_expr` lowered a `th==i` node through the typed INT path
on the written argument that the result "is a valid int EvalValue, so a
boxed consumer reads it fine" - true for an int, FALSE for a bool. So
`a[i]`, `p.b`, `d[k]` and a slice element printed **`1`/`0`** under the
VM and the JIT where the tree-walker printed `true`/`false` - a RULE 2
divergence that shipped, while a bool VARIABLE, RETURN, `foreach`,
unpack and `dyn` read were all correct (which is exactly why nothing
noticed). `th_bool` is set wherever the inferencer stamps `th=i` for a
Bool type, and `compile_boxed_expr` falls through to the BOXED lowering
for it. **The `truthy_only` exemption is not a shortcut**: a consumer
that only asks `is_true()` cannot tell an int 0/1 from a bool, so the
four JumpUnlessTrueV feeders (if/while/for/ternary conditions) keep the
typed path - without it, `if (a[k])` stopped emitting LoadElemInt and
the E4 peephole that fuses it into `JumpUnlessElemInt`
(57_bool_reduce's whole hot loop) no longer matched. Pinned by
`tests/functional/20_bool_value_reads.my`, which asserts BOTH halves -
the value shapes render as bools, the condition/arithmetic shapes still
promote to 0/1. After
`resolve_names`, `specialize_types`
(`inferencer.cpp`, called from `mylang.cpp` + the test harness, gated by `-nti`)
rewrites hot scalar nodes — `Expr03/04` (arith), `Expr06/07` (compare),
`Expr11/12` (logical), `Expr02` (unary) — over typed operands into a single
**`TypedScalarExpr`** node (`syntax.h`). It computes via **`eval_int()` /
`eval_float()`** — typed (unboxed) eval virtuals on `Construct` (default boxes
through `eval()`/`RValue`, so a typed node may call them on any child) — with no
`num_bin_op` promotion dispatch, no PMF virtual call, and no intermediate
`EvalValue` boxing. `Identifier`/`Subscript` override `eval_int`/`eval_float`
to read a resolved-local slot / an array element's scalar directly
(`EvalValue::get_ref<T>()` avoids a refcount bump); loop/if conditions take the
unboxed path via `eval_cond` when the condition is a known int (a comparison
result is bool-typed but specialized with `result_th = i`, so conditions stay
fast). The specializer
recurses bottom-up so nested typed subtrees chain `eval_int` calls with no
boxing between them. **Effect:** ~2.8x on `bench/44_primes_sqrt`, ~2x on
float-heavy reductions; the once-slower-than-Python primes benchmark is now
faster. `th` is copied by `copy_base_fields` (clones/inliner preserve it), and
the typed eval's `get<int_type>()` throws `TypeError` if inference were ever
wrong (a safety net, not silent corruption). See `plans/archived/type-inference.md` M8.
**A base template's body is NOT specialized** (`FuncDeclStmt::is_template`,
skipped in `specialize_types`): it is a monomorphization shell, cloned per
signature (each clone specialized separately) and run boxed for indirect
dispatch — specializing it would corrupt a different-signature clone (a float
instance's `eval_int` on a float param). `type_of` learned the `TypedScalarExpr`
case (a REPL retains a post-specialization body a later-input clone re-enters
inference on) — a defensive robustness fix (the inliner already handles
cross-input `TypedScalarExpr`).

**Loop-invariant slice hoisting (`try_hoist_loop_slices`,
inferencer.cpp; lever 3 inc 2, 2026-07-27).** A DIRECT loop-body decl
`var sl = base[a:b]` hoists ABOVE a For/While loop (a `Block{decl,
loop}` with scope_free; the decl's slot is frame-wide and resolution
already happened, so both engines just run decl-then-loop) when every
part is provably iteration-independent AND unobservable: base is an
inlined scalar LITERAL (an auto-const string) or a slotted local
NEITHER in `mut_len` NOR in `mut_content` - the content half is the
COW-DETACH hazard: an element write to a base with a LIVE slice clones
the base away (clone_aliased_slices), so a hoisted view would keep
reading the detached OLD storage while per-iteration fresh views see
the new one (pinned by the "base content write keeps per-iteration
views" test); bounds are absent or fr_immutable AND int-proven; and
with `Slice::base_sliceable` (a new inferencer stamp: statically
non-opt array/str) the slice CANNOT throw (pure clamping), so hoisting
is safe even for a ZERO-iteration loop. The slice var itself must be
written only by its decl (no write-through/mutating builtin - those
would COW the view privately, diverging from fresh-view-per-iteration)
and unreferenced by cond/inc. Escaping READS are fine (a view's
identity is unobservable - intptr shows the SHARED storage). Runs in
`specialize()` BEFORE try_for_range (the wrapper block's children then
specialize individually, so the loop still becomes a ForRangeStmt).
Measured with the pooled slices set (inc 1): 15_array_slice_readonly
-56% Ir / ~0.46x wall combined, 29_str -20.9% Ir; 16/47 flat.

**Loop-invariant CONTAINER-SUBSCRIPT hoisting - LICM
(`try_hoist_loop_subscripts`, inferencer.cpp, 2026-08-01).** The sibling
of the slice hoister for a nested-container READ. `for (var k = 0; k < n;
k++) s += a[i][k] * b[k][j]` re-materializes the ROW `a[i]` every
iteration - a boxed EvalValue plus an intrusive_ptr retain/release, about
half of 46_matrix_mult's inner loop - although nothing in the loop can
change it. It is hoisted into a SYNTHETIC temp (unlike the slice hoister,
which moves an existing decl, there is no decl here: `$licm<N>` gets a
fresh frame slot, so `specialize()`/`specialize_children`/`try_for_range`
now thread an `int *fsize` - the root Block's `slot_count` for main, else
`FuncDescriptor::frame_size`, exactly as the Inliner's `walk` does; null
= unresolved = no hoisting, capped at 64 slots):
`Block(scope_free){ if (0 < n) { var $licm0 = a[i]; } for (...) s +=
$licm0[k] * b[k][j]; }`.
**THE GUARD is the load-bearing part.** A slice only CLAMPS, so the slice
hoister is safe for a zero-trip loop; a SUBSCRIPT throws on OOB, and
hoisting one above a loop that runs zero times turns "never evaluated"
into "throws before the loop". The guard is the loop's own entry test
with the loop variable replaced by the init value (`COND[k := INIT]`), so
a zero-trip loop still evaluates nothing and a >=1-trip loop evaluates it
exactly once - the value iteration 1 would have read. Restricted to the
counted shape `for (var k = INIT; k CMP BOUND; ...)` with INIT and BOUND
both `fr_immutable` (hence side-effect-free), which makes the guard
literally `INIT CMP BOUND` - no substitution machinery and no risk of
duplicating a side effect into it. Two int literals decide it at compile
time: false means the body never runs (no hoist at all), true means no
guard is emitted. **The ForStmt is left UNTOUCHED and merely WRAPPED** -
try_for_range runs next and must still match the counted shape, and
losing `ForRangeStmt` would cost more than the hoist gains.
Gates on the candidate: `base_array` (not a dict/string), a CONTAINER
result (`th` neither i nor f - a scalar element is already a raw slot
read), `fr_immutable` with the loop var's uid (which also requires the
base absent from `mut_content`, so an element write anywhere in the loop
disqualifies it - that is what rules out the COW-detach hazard), no
reference to anything DECLARED INSIDE the body (`licm_collect_decls`:
above the loop such a slot is stale - `fr_collect_mutated` deliberately
SKIPS a pInDecl assignment, so a body-local `var m = map(..)` taints
nothing and `m[1][0]` looked invariant; the -rt suite caught exactly
this), and UNCONDITIONAL evaluation (the scan stops at the first body
statement that is not a plain expression statement, and never descends
into a ternary arm / `??` rhs / `&&`-`||` tail). Occurrences of the same
expression share one temp (`licm_expr_equal`). The result is expressible
MyLang - `:show` renders the guard and the decl as real code.
**CALLS IN THE BODY - `licm_has_opaque_call` + `CallExpr::
callable_arg_mask`.** A pure USER function is always fine (`pure` forbids
captures, global reads AND mutating a reference param, so it cannot reach
the enclosing frame at all). A CONST BUILTIN is fine too - `len(arr)`,
`abs(x)`, `str(v)` cost nothing - EXCEPT that the higher-order ones
(map/filter/sort/make_array/make_dict/find) run a CALLBACK that
`fr_collect_mutated` cannot see (it stops at a FuncDeclStmt), so a lambda
appending to the base would taint nothing and the base would look
invariant. The discriminator is a new inferencer stamp,
**`CallExpr::callable_arg_mask`** (bit i = argument i's static type is a
`Func`, or a `dyn` that might hold one) - stamped in `annotate_hints`
where types are live, because only the TYPE question is answerable there:
`effective_pure` is the RESOLVER's answer and does not exist yet. LICM
then requires each masked argument to be provably pure - an inline lambda
whose `desc->effective_pure` holds, or a name in `g_fr_pure`. Anything
else (a local variable holding a lambda, an impure named callback) is
unprovable and refuses the loop. **The mask DEFAULTS to `~0u`** so an
unstamped call - no inference ran, a node a later pass built, a field a
copy forgot - reads as "every argument may be callable" and declines.
`licm_is_const_builtin` additionally requires `sym.kind == builtin`,
since `fr_is_const_builtin` matches the NAME only and a user's own
`func len(x)` answers true there (a pre-existing looseness in
try_for_range's bound analysis, left alone).
Measured (callgrind Ir, `OPT=1 ASSERTS=0`): 46_matrix_mult whole-program
-24.1% at scale 1, and **369 -> 233 Ir per inner iteration** (the
scale-1-vs-scale-3 delta, so compile time is excluded); wall-clock
interleaved A/B **0.68x**, suite geomean cur/base 0.998x. It fires on
exactly ONE program in bench/ + samples/ (46) - a narrow blast radius by
construction, and every other program's output is byte-identical to the
pre-LICM binary.

**THE NESTED-READ FUSION - `LoadElem2Int`/`LoadElem2Float` (2026-08-01,
plans/archived/unboxing.md option A).** LICM above removes the row read
whose index is loop-INVARIANT (`a[i][k]` in the k-loop). What it cannot
touch is the
one whose outer index VARIES with the inner loop - `b[k][j]` - and that is
where 46_matrix_mult's remaining cost sat: a profile put **61% of the
inner iteration inside C++ helpers**, ~134 Ir of it in the two-op pair
`LoadElemValue t = b[k]` + `LoadElemInt dst = t[j]`. Materialising `t`
costs a helper call, an `intrusive_ptr` retain, a 32-byte `EvalValue`
copy, the `SharedArrayObjTempl` live-slices registration, and a release
when the temp is overwritten next iteration - for a row that is **alive
for exactly one instruction**. C++ gets the row with one `mov`.
The fused op BORROWS it instead: `vm_elem2_borrow_row` returns a raw
`const EvalValue &` into the outer array's storage and the scalar read
runs off that, so nothing is boxed. **The soundness argument is short,
which is the point:** the borrow is taken and consumed inside ONE
instruction (no user code, no allocation, no mutation, no unwinding can
run in between), the OUTER array holds the row's reference throughout,
the op only READS so it can trigger no copy-on-write detach, and it
creates no VIEW, so there is nothing to register in the parent's slices
set. The scalar read itself is the SHARED single-level
`vm_load_elem_int/float_core`, so the two levels cannot drift.
**The asymmetry that made this a gap rather than a design choice:** the
STORE side already did exactly this - `StoreElem2V` walks `a[i][j] = v`
in one op and `StoreElemChainV` generalises it - and the read side simply
never got the twin. It also reuses the store side's caret machinery: the
two levels need DIFFERENT carets (an OOB on `i` reports the `base[i]`
span, one on `j` the whole `base[i][j]` span) which ONE loc-table entry
per pc cannot hold, so the pair comes from `chain_locs` exactly as the
per-step store carets do. That is also what fixes the encoding: `a` is a
DUAL (outer index SLOT, chain_locs idx), `b` the inner index operand,
`target2` the base, `target` the dst - so a LITERAL outer index cannot
ride it and DECLINES to the byte-identical unfused pair, as do a
non-array level, a dict, and a `dyn` base. A deeper chain fuses its last
two levels and materialises the rest through `compile_array_base`.
The JIT emit is a plain helper call (`jit_load_elem2_int/float`): the win
is the deleted materialisation, not the call, and both indices stay
cache-aware VALUE args so a matrix loop keeps its counters pinned. It is
`op_fully_native` - the helper throws WITH the baked per-level caret
rather than loc-less, and the emit-side stamp (first conveyor wins)
supplies only the inlined-at chain. myv **v10**: the two opcodes are
APPENDED, so no ordinal moved and a v9 image would still decode - but the
version bump is what stops a v9 BINARY from mis-dispatching a v10 image.
Execution-proven by `g_jit_op_run` in the `jit_load_elem2_native` test
(the counters are bumped ONLY from emitted code; the interpreted case
goes straight to the core), plus dual-engine `elem2:` tests for values,
negative wrap at both levels, ragged rows, a slice base, both OOB carets,
and the declines. NOTE the test shape: the outer index must vary with the
INNER loop, or LICM hoists the row before codegen sees a nested read.
Measured (callgrind Ir, `OPT=1 ASSERTS=0` both sides, scale-1-vs-scale-3
delta so compile time is excluded; n=70 -> 343k inner iterations per
scale unit): 46_matrix_mult **221.5 -> 170.5 Ir per inner iteration
(-23.0%)**, whole-program -12.3% / -17.9% at scale 1 / 3; everything else
neutral (14_array_subscript +0.03%, 43_sieve +0.04%, 18_foreach_array
+0.02%, 01_while_loop +0.06%, 15_array_slice_readonly -0.55%), so the new
opcode costs no `vm_dispatch` layout tax. The suite geomean does not move
and was not expected to - the pair occurs in ONE corpus program, at three
sites, one hot. **The plan predicted ~90 Ir/iter and that was WRONG:** the
ceiling was right, the assumption that a HELPER-CALL tier would reach it
was not. The residual splits as ~87 Ir inside the helper (two levels of
`is<SharedArrayObj>` + `skind`/`size`/`offset`/`get_vec` + bounds, none of
it visible through a call boundary), ~22 in `EvalValue::operator=(&&)`
(the helper's `dst->put()` boxing an int the emit could `store_dst` in two
stores), and ~54 native.
**The INLINE tier LANDED (#93, 2026-08-02):** the two guard/read
sequences emitted at the call site (general outer -> the LValue row at
stride sizeof(LValue), bounds by an unsigned BYTE-length compare that
also catches a negative index -> non-slice flat-INT row -> the shared
count compare -> `mov rax,[rcx+r9*8]` -> the ref-aware store_dst), the
helper kept as the slow tier for every decline - slice outer/row,
non-general outer, bool/float/str/general rows, OOB (whose per-level
carets stay the helper's baked chain_locs pair). Measured: 46_matrix_mult
whole-program **-22.4%** and **170.5 -> 91.5 Ir per inner iteration** -
the ~90 the plan named as the ceiling and the helper tier missed.
Execution-proven by `g_jit_elem2_fast` (bumped by the EMITTED code; the
fusion test requires it STRICTLY, since the helper makes the values right
too). Two guards were sabotage-verified through new 5-mode `elem2:`
entries, each needing a shape-eater defeated first: SLICE rows share the
parent's SharedObject, so a raw read skipping `off` returns the PARENT's
elements (608 vs 128 - and the corpus had no slice-ROW case at all); and
BOOL rows fuse to LoadElem2Int too (a bool node is stamped `i`) but store
ONE byte per element - the catching shape needs rows >= 8 wide (a
3-element row is saved by accident: byte-length/8 rounds its count to 0
and bounds decline anyway) and must SUM the elements, not branch on them
(garbage truthiness matched an if-based version by luck).

**Counted-loop specialization (`ForRangeStmt`).** Also in `specialize_types`
(via `try_for_range`, run on the RAW `for` before its cond/inc are specialized),
the four hottest loop shapes are rewritten to a dedicated `ForRangeStmt`
(`syntax.h`): `for (var i = start; i </<= bound; i += step)` and
`for (var i = start; i >=/> bound; i -= step)` (the comparison `Op` is kept in
`cmp_op`) — matched when `i` is a resolved **int slot** (`sym.kind == local`,
`th == i`), the comparison/step directions agree (`<`/`<=` with `+`, `>=`/`>`
with `-`), and `bound`/`step` are **loop-immutable** (`fr_immutable`): a
side-effect-free **int** expr built from literals, slotted-local ids,
arith/bitwise chains, subscript/member READs, and **pure calls with immutable
args** — a **const builtin** (`len(arr)`), or an **effectively-pure USER
function** (`fr_is_pure_func`, from `g_fr_pure` — this program's
`effective_pure` funcs plus, in the REPL, prior inputs' via `prior_scope`), with
any immutable args including containers (`compute(arr)`). This is sound because
**`pure` forbids mutating a reference parameter** (see *Pure functions* below /
`func_mutates_input`): a pure call has no side effects and, given immutable
args, the same result every iteration, so it is safe to evaluate once.
Immutability is proven against two sets from `fr_collect_mutated` (which reuses
the complete `Inferencer::for_each_child` so no write is missed): **`mut_len`**
(an id whose value/length/identity may change — a direct reassign/`++`, or an
*impure* call passed the container, since a mylang array/dict/struct is a
reference an impure callee can `append`/mutate) and **`mut_content`**
(additionally an `arr[i] =`/`obj.f =` element write). A bare id / `len(arr)` arg
needs length-stability (`∉ mut_len`) — so the common fill
`for(i;i<len(a);i++) a[i]=…` still specializes (an element write doesn't change
the length); a subscript READ `arr[k]` additionally needs `∉ mut_content`. A
*pure* call taints nothing (it can't mutate its args); an *impure* call taints
the length and content of each non-scalar arg. `ForRangeStmt::do_eval`
evaluates `bound`/`step` **once** (cached as raw `int_type`), then the
per-iteration condition test and increment are plain C on the slot's
`int_type` — no expression eval, no `num_bin_op`, no `TypedScalarExpr` dispatch
(the body still gets M8). `step` is null for the `i++`/`i--` form. The slot is
re-fetched each iteration so a body that reassigns `i` is honored;
break/continue/return go through the same `FlowState` as `ForStmt`. **Effect:**
~10% on the bench geomean (0.61x→0.55x). Cross-input guard: a prior REPL body
is post-specialization and may hold a `ForRangeStmt` whose `i_slot` the
inliner's substitution / tail re-resolution would not remap, so the inliner's
cross-input registration **skips** a prior function containing one
(`has_for_range`) — it still runs correctly as a call. coderender renders it as
the equivalent `for (...) /* counted */` so `:show`/`-a` make the optimization
visible. **Not yet specialized:** a pure-user-func bound with a *container* arg
(would need to prove the callee doesn't mutate it), and a float loop var.

### Function templates (monomorphization)

**VALUE-USED templates instantiate too (plans/value-template-
instantiation.md).** A template stored in a container/var and called
INDIRECTLY (`ops = [add_op, sub_op]; fn = ops[i%2]; fn(st, i)`) gets a
typed instance when every such call agrees on ONE settled signature: the
`Func` StaticType carries a **finfo set** (`StaticType::finfos`, seeded
by `func_static_type` for a template, UNIONED by `join`, copied by
`with_opt` - metadata, never part of equal/assignable), which rides the
ordinary lattice through array/dict elements and var joins, so each
indirect call's callee type names its candidate templates with no new
dataflow machinery. `value_instantiate_round` (run beside
`instantiate_round`) checks per-template UNIFORMITY over the attributed
sites, makes the instance through the ordinary clone machinery
(`tmpl_cache`/`make_template_clone` - same `$N` naming/display_name),
REDIRECTS every value-use Identifier IN PLACE (uid + id_sym rebind), and
seeds the clone's params with the signature each fixpoint round (a
phantom call - no direct call feeds them). **ESCAPES disable it**
(`FuncInfo::value_escaped`): a join that DROPS the finfo set (collapse
to dyn / conflict - recorded in the arena's `escaped_finfos` ledger,
drained per round), an ARG-position value use (any call/builtin arg -
`map(f, ...)`, `runtime(f)`), or a capture-list use - an untracked call
site could reach the typed instance with a mismatched signature, so an
escaped template keeps its boxed base (sound, as before). Non-uniform
signatures across sites → no instantiation. **An UNINFORMATIVE signature
is declined too** — a `dyn` param (the clone would be just as boxed) or a
container carrying the BOTTOM `none` element type (`type_has_bottom_elem`:
`array<none>` / `dict<none,none>`, what an empty `[]`/`{}` infers and what
a container symbol KEEPS while nothing the inferencer can see writes it —
a callee filling it through its reference param contributes to the PARAM's
symbol, never back to the caller's). That case is not merely useless but
HARMFUL: the clone's params are seeded from the signature, so a body that
only READS the container types every element `none` and an ordinary use of
one (`k + ":"`) becomes a spurious `NullabilityEx` — even though the
container is non-empty at runtime, filled by a SIBLING function
(`samples/phonebook`: `cmd_add` fills `data`, `cmd_view` iterates it; a
WRITER body repairs its param type by contribution, a reader cannot). The
decline is a DEFER, not a veto — a later fixpoint round that settles the
element type instantiates. (The DIRECT-call `instantiate_round` has the
same bottom-signature blind spot, pre-dating this feature; there a
container arg is usually written by the callee, which repairs the type.)
All value uses redirect to
ONE instance, so `ops[0] == add_op` identities hold. v1 is INPUT-LOCAL
in the REPL (a prior input's array called later keeps the base -
correct, unoptimized; pinned by a `repl:` test). This closed the last
CPython-losing bench (76_funcval_dispatch 1.05-1.12x → **0.68x**; its
callee bodies 6 boxed ops → 4 typed).

**⛔ A DEAD BASE TEMPLATE IS DECIDED FROM THE TREE, AND A BAKED CONST
VALUE IS NOT IN THE TREE (2026-08-25).** `is_template_base` excludes a
template NEVER used as a value from codegen - every call to it was
redirected to an instance, so the base never runs. "Used as a value" was
read off `TypeSym::value_used`, i.e. off IDENTIFIERS the inferencer can
see - and the parser's const-fold gets there FIRST: `const OPS = [sq];`
becomes ONE `LiteralObj` carrying the array, FuncObject and all, so by
inference time nothing NAMES `sq`. The base was dropped, and
`var dyn f = OPS[runtime(0)]; f(7);` reached a chunk-less body: an
ML_CHECK abort under ASSERTS, a walk of the FREED AST without them,
while `-tw` (which never tears the tree down) printed the right answer -
a RULE 2 divergence on top of the RULE 1 one. **The #149 closure now
also walks BAKED VALUES** (`keep_in_value`, inferencer.cpp): const
arrays, dicts, struct instances, a struct's folded `const` MEMBERS, and
nesting - a flat array is skipped, since it cannot hold a function and
`get_view()` would PROMOTE it. The generalisation: **when a pass asks
"is this thing referenced?", const-eval may have turned the reference
into a VALUE** - the same shape as the `abs` shadowing bug and the
brace-less-body scope bug, all three being "the parse-time evaluator
knows something the later passes cannot see". Pinned by
`tests/functional/const_pool_func.my` (watched failing: the tree-walker
printed `49 50 60 42 81`, the VM aborted).



A **named** function with ≥1 *template param* — un-annotated, non-`dyn`,
non-`opt` — is a **template** (`FuncInfo::is_template`, set in
`declare_funcdecl`): not type-checked in isolation, but **instantiated per
call-site signature** as a typed clone the ordinary concrete-function path
handles. So `func f(x){var t=x+1; return t;}` never needs `var dyn t`, a
never-called template never errors, and `f(1); f("s")` makes two instances not a
type conflict. `dyn` is the explicit one-instance-any-type param. Full design +
deferrals: `plans/archived/function-templates.md`.

**`opt`/typed params coexist with template params** and just `join` within each
clone — the signature is keyed by the *template params only*. So `func f(a, opt
b)` is a template over `a` (`b` joins; arity in `instantiate_round` is the
`[min,nparams]` range). `func f(opt x)` with no template param keeps the join
model. **A var-bound lambda** (`var id = func(x)=>x`) becomes a template iff it
is non-capturing and the var is **write-once + calls-only** (`mark_lambda_
templates`, after the structural pass, using per-symbol `writes`/`value_used`
bookkeeping — the decl write is counted in `walk_struct`, not `declare_target`,
which runs twice via hoist); a value-used / capturing / reassigned lambda stays
join. **D4:** past 64 instantiations a template's further calls run dynamically
(`tmpl_inst_count`, a one-time stderr warning).

Mechanism (`inferencer.cpp`): the fixpoint and check pass **skip** an
un-instantiated template (`accumulate`/`accumulate_call`/`check`/`check_call`
test `is_template`); an outer loop in `infer_one`, between the main fixpoint and
finalize, runs `instantiate_round` — for each template call whose arg types have
settled, it gets-or-makes the instance for that `(template, signature)`
(`make_template_clone`: a `<name>$N` id - the user name plus a per-name
monotonic counter, so it is readable AND inspectable, `typeof(f$0)`; with
`display_name` keeping the original for backtraces -
`walk_struct`'d, `is_template=false`, inserted at the root block's front),
**redirects** the call to it, and re-runs the fixpoint; the clone's params
accumulate their one signature through the concrete path. Arity is still checked
for a template call; per-arg type/nullability is checked inside each clone.
**The `(template, signature)` cache (`tmpl_cache`) is SESSION-persistent, NOT
cleared per input** — a signature already instantiated by a prior input
**reuses** that instance instead of building a duplicate (`f(2,3)` then `f(2,3)`
again in a later input both run `f$0`, not `f$0` then `f$1`). To stay
clear of the node-identity hazard below, the cache is keyed by the stable
`template_sig_key` (the template's arena-stable `FuncInfo*` + the signature's
type strings) and **valued by the instance's interned NAME** (a `UniqueId *`,
never a node pointer); the redirect resolves that name in the global scope
(`global->syms`, whose `TypeSym`/`FuncInfo` are pinned), so a prior input's
instance is reached by name, not by a stale node. `infer_input` snapshots the
cache and restores it if the input is rejected (so a rolled-back clone leaves no
entry); the clone-name counter stays monotonic. **Subtlety:** `id_sym`/
`func_of_decl` are keyed by node POINTER and
persist for the session, so a fresh input node can reuse a freed node's address
— `walk_struct` therefore **always re-resolves** an identifier (never
`if (!id_sym.count(id))`, which would keep a stale entry and bind an input's
callee to a prior clone), and `make_template_clone` clears the clone subtree's
`id_sym`/`func_of_decl`. (This was an MSVC-only, address-dependent,
non-deterministic bug, root-caused via CI instrumentation; GCC/clang +
sanitizers never reproduced it.)

## The value & type model (the subtle part)

- **`EvalValue`** (`evalvalue.h`) is a hand-rolled tagged union: a `ValueU`
  union plus a `Type *type`
  tag. It deliberately avoids `std::variant` — the comment in `flatval.h`
  records that `std::variant`
  made the whole interpreter ~50% slower on a simple loop. `size_type` is
  `uint32_t` (not `size_t`)
  specifically to keep `EvalValue` small and fast to copy.
- **`FlatVal<T>`** (`flatval.h`) is an `alignas(T) char[sizeof(T)]` buffer with
  placement-new ctors.
  It's what lets non-trivial C++ objects (`SharedStr`, `shared_ptr<…>`,
  `SharedArrayObj`) live inside
  the union. Because a union can't run their ctors/dtors, `EvalValue`'s
  copy/move/destroy route
  through **type-erased ops** (`TypeErasureOps` in `type.h`:
  `default_ctor`/`dtor`/`copy_ctor`/
  `move_ctor`/`copy_assign`/`move_assign`), which `TypeImpl<T>` (in
  `evaltypes.cpp.h`) implements via
  placement-new + `reinterpret_cast`.
- **`Type`** is a polymorphic operations table, **one singleton per kind**, held
  in the global
  `AllTypes` array (`types.cpp`) indexed by the `Type::TypeE` enum (`type.h`).
  Every operation —
  `add`, `sub`, `mul`, `lt`, `eq`, `is_true`, `to_string`, `len`, `subscript`,
  `slice`, `clone`,
  `hash`, `use_count`, `intptr`, … — is a `virtual` on `TypeTemplate` dispatched
  through the value's
  `type` pointer. The base implementations throw `TypeErrorEx`; a type "gains" a
  behavior purely by
  overriding the relevant virtual (see `src/types/int.cpp.h` for the canonical
  example). Binary ops
  **mutate the left operand in place**
  (`void add(EvalValue &a, const EvalValue &b)` does `a += b`).
- **`to_string` vs `to_string_repr`.** `to_string` is the plain conversion
  (`print`/`str` of a bare value: a string renders unquoted). `to_string_repr`
  (`type.h`, default == `to_string`) is the **repr** form used when a value is
  rendered *inside a container* and by the REPL `=>` echo: only `TypeStr`
  overrides it, returning the **quoted + escaped** literal (`quote_str` in
  `str.cpp.h`, the inverse of `unescape_str`). So `TypeArr`/`TypeDict`/
  `TypeStruct::to_string` call `elem.to_string_repr()` on their elements/keys/
  values — `["a", 1]`, `{"k": "v"}`, `P(name: "bob")` — matching how Python
  prints a list (a container's *own* repr is just its `to_string`, which already
  quotes its elements). The REPL echoes the top-level value via `to_string_repr`
  too, so a bare string echoes `=> "hello"` (IRB-style); `print`/`str` of a bare
  string stay unquoted.
- **`pretty` (REPL multi-line echo).** A third `Type` virtual
  (`pretty(a, indent, width)`, default == `to_string_repr`): a container whose
  single-line repr would overflow `width` from column `indent` is **expanded one
  element per line, indented, recursively**; anything that fits stays on one
  line. Only `TypeArr`/`TypeDict`/`TypeStruct` override it (each iterates its own
  elements and recurses via `EvalValue::pretty`). A dict/struct passes each
  *value's* actual start column (`indent + key + ": "`) as the child indent, so a
  nested value's fit check is accurate and its closing bracket lines up under the
  opening one. Used ONLY by the REPL `=>` echo (`r.pretty(3, ...)`, then
  `show_colorize` colors it line-by-line when color is on); `print`/`str` are
  unaffected. A small value still echoes on one line.
- **`bool` is a real scalar type (`t_bool`, `TypeBool` in
  `src/types/bool.cpp.h`).** `true`/`false` are its only two values (parsed to
  `LiteralBool`, `syntax.h`). It is stored in `EvalValue`'s `bval` union member
  (which aliases `ival`'s low byte; the `EvalValue(bool)` ctor zeroes the full
  `ival` first, so reading the slot as the int `0`/`1` is also valid). `bool`
  sits at the bottom of the numeric promotion chain **`bool <= int <= float`**:
  `num_bin_op` promotes a bool operand to `int 0/1` before dispatch, so
  `TypeBool` itself only implements `is_true`/`to_string`/`hash` (the hash
  matches the equal int `0/1`, so `true` and `1` are one dict key). Comparisons
  (`Expr06`/`Expr07`), logical ops (`Expr11`/`Expr12`, via `logop_loc`), and
  unary `!`/`-`/`+` produce a `bool` (`-true`/`+true` promote to int first); the
  `EvalValue` comparison operators read the result via `is_true()`, **not**
  `get<int_type>()` (a comparison result may now be a bool — `TypeArr::noteq`
  was the one internal consumer that had to switch). `MakeConstructFromConstVal`
  and `value_repr` (specialization-dedup key) handle bool. Bool-returning
  predicate builtins (`defined`/`isconst`/`isconstdecl`/`ispure`/`ispuredecl`/
  `startswith`/`endswith`/`isinf`/`isfinite`/`isnormal`/`isnan`) return a real
  bool; `int()`/`float()` accept a bool.
- **Mixed int/float promotion is centralized in `num_bin_op()`**
  (`evalvalue.h`), not in the type
  classes. The type virtuals are single-type: `TypeInt::add` only handles an int
  RHS, `TypeFloat::add`
  also accepts an int RHS (promoting it). `num_bin_op(a, b, &Type::op)` is the
  dispatch chokepoint:
  it first promotes a **bool** operand (either side) to `int 0/1`, then, when
  `a` is int and `b` is float, promotes `a` to float, so `int OP float` lands in
  `TypeFloat` and behaves identically to `float OP int` (and `bool OP x` like
  `int OP x`). **Dispatch binary
  arithmetic/comparison
  through `num_bin_op`, never by calling `a.get_type()->add(...)` directly** —
  every call site does
  (the `ExprNN::do_eval` ladder and compound-assign in `eval.cpp`, `EvalValue`'s
  `== != < <= > >=`
  operators, `builtin_sum`). It is a no-op for any non-`(bool/int,float)`
  operand pair, so string/array/etc.
  comparisons pass through unchanged. (Logical `&&`/`||` and unary ops are *not*
  routed through it.)
  Note for dict keys: an integer-valued float hashes as the equal int
  (`TypeFloat::hash`) so that
  `1` and `1.0`, which compare equal, are the same key.
- **The trivial / non-trivial boundary is `t_str`.** `TypeE` order matters:
  `t_none, t_lval, t_undefid, t_int, t_builtin, t_float, t_bool` (`< t_str`,
  trivial, stored inline,
  bit-copyable) then `t_str, t_func, t_arr, t_ex, t_dict` (`>= t_str`,
  non-trivial, need the
  type-erased lifecycle ops). Hot paths branch on `type->t < Type::t_str` /
  `>= Type::t_str` (e.g.
  `EvalValue::clone()` short-circuits for trivial types). If you add a type, its
  position relative to
  `t_str` decides which machinery applies.
- `t_lval` and `t_undefid` are **internal pseudo-types**, never visible to
  scripts (blank entries in
  `TypeNames`). They tag the two special `EvalValue` payloads below.
- **`LValue`** (`evalvalue.h`) = an assignable slot: an `EvalValue` + `is_const`
  flag + an optional
  back-pointer (`container`, `container_idx`) used when the slot is an *element
  of an array* (needed
  for copy-on-write, below).
- **`RValue(v)`** collapses an `EvalValue` holding an `LValue *` down to the
  contained value, and
  throws `UndefinedVariableEx` if it holds an `UndefinedId`. Builtins and
  operators call `RValue(...)`
  on every operand. `Identifier::do_eval` returns an `EvalValue` wrapping
  `LValue *` when the symbol
  is found (walking the parent chain), else an `UndefinedId{name}` sentinel —
  *not* an immediate
  error, which is what lets `defined()` and declaration-vs-assignment logic
  work.
- **`EvalContext`** (`eval.h`) is a lexical scope:
  `map<const UniqueId *, LValue>` + `parent` pointer
  + `const_ctx`/`func_ctx` flags. The root context auto-loads `const_builtins`
    (always) and `builtins`
  (only when not a const ctx). A `Block` evaluates in a fresh child
  `EvalContext` — **except** a `scope_free` block (the resolver sets the flag
  when every declaration in it is a frame slot: no capture, nested-func name, or
  slot-budget overflow), which never touches the map and so runs its statements
  directly in the parent context, skipping the per-entry `EvalContext`
  build/teardown (a measurable win for loop/if/function bodies, re-entered every
  iteration/call). The root block always builds its context.
- **Slot resolution (`resolver.cpp`) bypasses the map for resolved locals.**
  The post-parse `resolve_names()` pass assigns slot indices so an
  `Identifier::do_eval` for a resolved local is an O(1) read of
  `EvalContext::frame->slots[slot]` instead of the `map`+parent-chain walk. A
  call's `Frame` (an inline slot buffer + heap spill past 8 — just
  default-constructed slots, **no liveness bitmask**, so **no 64-slot limit**)
  is created in `do_func_call` when `FuncDeclStmt::resolved`, and
  for the program's implicit "main" by `Block::do_eval` on the root block;
  nested blocks inherit the `frame` pointer.
  **Slotted: a function's params and its locals** (`var`/`const`, `for`-init,
  `foreach`, `catch` variables) **and top-level variables a function does NOT
  read** (`SymKind::local`, the current call's `frame`), **plus top-level
  FUNCTION names and every top-level variable a function DOES read**
  (`SymKind::global`, the program-wide global table — see next bullet), **plus a
  closure's captured variables** (`SymKind::capture`, the closure's per-instance
  vector — see *capture slotting* below), **plus builtins not shadowed by a user
  symbol** (`SymKind::builtin`, the program-wide builtin table — see *builtin
  slotting* below). So *every* name a script resolves — local, global, function,
  capture, or builtin — is an O(1) slot, never a scope-chain map walk; the
  resolver's pass 1 collects the names functions read (`escaped`) and pass 2
  routes an escaped top-level var into the global table rather than a main-frame
  slot. **Not slotted (stay in the map):** REPL top-level names (open-world
  redefinition). **A name declared in NO scope is a COMPILE error since
  FIX-1 (#130)** - `resolve_ref` refuses it rather than leaving it for the
  runtime map, which was guaranteed to fail anyway (the script map is empty
  and asserted). Two exemptions: a LAZY builtin's argument
  (`defined`/`isconst`/`isconstdecl` never evaluate it - they ask a question
  ABOUT the name, answered `false`) and the `_` placeholder. A name declared
  BELOW its use is NOT this error - see `Scope::all_names`, the whole-scope
  pre-scan that distinguishes them. The resolver does a forward
  lexical walk (no hoisting **for locals**), but since the TDZ (#131) a use
  that a LATER declaration in the same scope will shadow is a COMPILE error
  (`UseBeforeBindingEx`) rather than a read of the outer binding - so
  `var x = x + 1` is refused, where it used to read the outer `x`. Top-level
  *functions* ARE hoisted, binding and all — see the next bullet. **No per-slot
  liveness**: a slot is default-constructed when the `Frame` is built, and a
  local can only *resolve* to its slot AFTER its decl (forward resolution), so a
  re-entered loop body re-binds its locals via their decls (which re-run each
  iteration) and a use-before-decl is a compile error (the TDZ, #131) — so the
  slot's stale value is never observed. (This is why there is no
  script `undef`: removing a binding would need per-slot definedness; the REPL's
  `:undef` works on its map-resident globals instead.) Same-block duplicate
  declarations are caught here (`AlreadyDefinedEx`) so the runtime decl path can
  just overwrite. `Identifier::sym` and `FuncDeclStmt::{resolved, frame_size,
  slot_writes}` carry the results; `slot_writes` (per-slot write counts:
  write-once == 1 for a local, 0 for a never-reassigned param) is what the
  auto-const folder uses to find promotable write-once vars (see the
  const-evaluation section). The const-eval path runs before resolution, so pure
  funcs invoked at parse time use the map (`resolved` is still false then).
  **Slots also bypass the map on the WRITE side:** `handle_single_expr14`
  (`eval.cpp`) fast-paths an assignment / compound-assignment to a resolved,
  live, non-const local — it read-modify-writes `frame->slots[slot]` in place,
  skipping the `lvalue->eval()` → `LValue*` → `doAssign()` round-trip (it falls
  through to that general path when the slot is undefined or const, so the same
  errors still fire). When both the slot and the rhs are ints, a
  compound-assign (`+=`/`-=`/`*=`) does the op **directly** on the slot's int —
  no `num_bin_op` PMF dispatch, no copy in/out (`div`/`mod` stay general, for
  the zero check). And `Expr14::do_eval` has a sibling fast path for `local
  += N` with an **int-literal** rhs: it skips evaluating the literal node too
  (what an `i++` would compile to — there is no `++` operator). The literal is
  recognized by a cheap `is_lit_int()` tag check (`ConstructType::lit_int`),
  **not** a `dynamic_cast` — this path runs on every `i += 1`, so an RTTI
  lookup there is a measurable tax (it dominated tight `while`/`for` loops).
  `foreach` binds a resolved-local loop var the same way via `bind_loop_var`.
  All use `as_resolved_local`, a cheap `is_id()` tag check (`ConstructType::id`),
  not a `dynamic_cast`.
- **Top-level functions AND escaped variables are slotted in a GLOBAL table
  (`SymKind::global`).** A global symbol can't be a *frame* slot — a function
  body runs parented to its definition scope (`capture_root`, the program
  root), not the
  call site, so a global reference from deep in a recursion isn't in the current
  `frame`. Instead each top-level function AND each top-level variable some
  function reads gets a static slot in a program-wide **`GlobalFuncTable`**
  (`eval.h`: a plain `vector<LValue> slots` + a `defined` flags vector + a
  slot→name list for reflection — despite the name, it holds global *variables*
  too), reachable from any call depth via `EvalContext::gfuncs` (inherited from
  the parent; the root block owns the table). So `fib`/mutual-recursion/any
  named-function call AND any function's read/write of a global variable is an
  **O(1) table read, not a scope-chain map walk** — a real win on call-heavy and
  global-heavy code (`bench/09_fib`). A plain vector with **no slot limit** — a
  program may have any number of global functions/variables. The resolver
  **hoists functions AND top-level structs** before resolving bodies
  (`hoist_global_funcs` handles both; a struct name binds its descriptor in a
  global slot like a func — so `P(...)`/`P.CONST` are O(1) slot reads, not a map
  walk), so a forward / mutually-recursive reference resolves; for **variables**
  it instead
  records each function-side use site (`escaped_refs`) during pass 1 and stamps
  it `SymKind::global` after pass 2 has given the var its table slot (a var, not
  hoisted, gets its slot when its decl is walked). `resolve_ref` resolves a
  reference innermost-out (a closure's capture scope is searched before the
  global table, so a capture shadows a same-named global). A slot is `defined`
  only once its decl executes (a function by
  `FuncDeclStmt::do_eval`, a variable by `handle_single_expr14`'s global-decl
  branch), so a reference reaching a symbol before its definition runs reads
  "undefined" (same as the old map late-binding); `Identifier::do_eval`/
  `eval_int`/`eval_float` read the slot, the assignment fast paths in
  `handle_single_expr14` write it, and `globals()` enumerates the table's names.
  **A top-level var is global only if a function reads it AND it's in the
  OUTERMOST main scope** — a main nested-block local that merely shares a name
  with a global stays a frame slot (a function is parented to root, so it can
  only read an outermost top-level var); such a nested var legitimately shadows
  the global. A non-escaped top-level var stays a main-frame `SymKind::local`
  slot, so **auto-const (which only sees frame slots) is untouched** — only vars
  no function reads are promotable, exactly as before. **Top-level STRUCT names
  are hoisted into the global table too** (`hoist_global_funcs` handles both
  `FuncDeclStmt` and `StructDeclStmt`): a struct is a non-capturing named decl
  visible from any function body, so its name binds its type descriptor in a
  global slot exactly like a function (`StructDeclStmt::do_eval` writes the slot
  when `id->sym.kind == global`), and `P(...)`/`P.CONST` resolve to it — no map.
  **Non-top-level named decls are SCOPED global slots
  (`hoist_scoped_decls`).** A non-capturing function or struct declared inside a
  block (nested in a function body, an `if`/`for`/`{ }`) is *effectively global*
  (it closes over nothing), so it gets a real global-table slot — but via
  `add_anon_global_slot` (appended to `global_names` for the table size, **NOT**
  entered into `global_func_slots`) and registered **only in its lexical scope**.
  So it is **block-scoped** (resolvable just within that block, popped with it)
  and two same-named nested decls in sibling scopes get **distinct slots** and
  never collide. The block handler pre-scans and hoists these into the scope
  *before* walking the block's statements, so a forward/mutual reference within
  the block resolves. This makes nested functions/structs lexically scoped like
  variables (a script-visible change: a func in an `if`-block is no longer
  visible after it — previously only TOP-LEVEL block funcs leaked; function-
  nested ones already didn't). A **capturing** named func is excluded (it closes
  over locals, so it is the enclosing scope's local via `declare_masking`, as
  before) — though NOTE that the grammar rejects a capture list on a NAMED
  func everywhere (`func f[x]()` is a SyntaxError; captures are for closure
  EXPRESSIONS only), so that exclusion is dead in practice.
  **Known limitation (pre-existing):** mutual recursion between *sibling
  nested* functions doesn't resolve (each function's body resolves in its own
  scope stack, which doesn't see the enclosing scope's scoped globals) — it was
  already broken (a runtime `UndefinedVariableEx`), and stays so.
  **⛔ A BRACE-LESS BODY IS ITS OWN SCOPE — and getting there took three
  fixes to the same two functions (`pStmtDeclaresName` / `pWrapDeclBody` /
  `pBraceLessBody`, parser.cpp).** A declaration written as a brace-less
  `if`/`else`/loop body used to land in the ENCLOSING scope, unlike its
  braced form. The three ways that was wrong, in the order they were found:
  - **(2026-07-14, a CRASH)** a func/struct decl there — `if (c) func g() =>
    1;` — ABORTED the tree-walker: `hoist_scoped_decls` only pre-scans
    **Block** statement lists, so a bare-statement body's decl was never
    hoisted to a scoped global; it fell to `declare_masking` (a local map
    entry) and `FuncDeclStmt::do_eval`'s `ctx->emplace` tripped the
    asserted-EMPTY script map (`in_const_eval() || repl_mode`).
  - **(2026-08-08, a SILENT WRONG ANSWER)** a `var`/`const` decl there stayed
    visible after the statement even when the branch NEVER RAN, so its slot
    held `none` while inference still proved the declared type — and the
    engines then disagreed about a typed read of it:
    `if (runtime(false)) var x = 5; return x + 1;` **printed 1 under the
    JIT** (reading `none` as int 0), aborted `ML_VM_CHECK` under `-nj`, and
    threw `TypeErrorEx` in the tree-walker. The JIT is NOT at fault — it
    elides the tag check BECAUSE inference proved the type (see *static types
    imply runtime guards*), and that proof is sound only once a declaration
    cannot be skipped. **This is the general principle: a scoping hole is a
    TYPE-SOUNDNESS hole here, because every unboxed tier is built on the
    inferencer's proof.**
  - **(2026-08-08, an OPTIMIZATION CHANGING SEMANTICS)** the const-folded
    taken branch was hoisted UNWRAPPED, so const-eval changed scoping:
    `if (1) func g() {...} g();` worked by default and threw
    "Undefined variable 'g'" under `-nc`.

  The fix: `pBraceLessBody` parses a brace-less body inside its own const +
  CSE scope (pushed AROUND the parse — a `const` registers its VALUE as it is
  parsed, so wrapping the finished statement is too late for
  `if (c) const K = 7;`) and then `pWrapDeclBody` wraps it in a synthetic
  single-statement `Block` **iff it DECLARES** (`pStmtDeclaresName`: a
  `FuncDeclStmt`, a `StructDeclStmt`, or an `Expr14` carrying `pInDecl`).
  A non-declaring body is left ALONE on purpose — a blanket wrap would put an
  extra `Block` around `for (...) sum += a[i];` and perturb the shapes
  for-range and LICM match on, for no scoping benefit. **ADD TO
  `pStmtDeclaresName` if a new declaring statement form appears**; a missing
  kind silently restores the leak. Both halves have watched-failing coverage
  (`brace-less * is block-scoped` in tests.cpp: removing the `Expr14` case
  fails 5, removing the const-scope push fails the const one).
  Two subtleties that survive: a PURE expr-bodied `g` is const-folded/inlined
  at its call sites regardless of scope (a fold-vs-scope quirk, identical in
  both engines — the block-scoping tests must use an IMPURE func); and with
  the masked route gone, a SCRIPT's named func/struct decl ALWAYS has a
  global slot — the VM codegen's `gen_stmt` `ML_CHECK`s that invariant
  instead of falling back.
  **A NAMED func/struct decl BINDS AT SCOPE ENTRY, not where the statement
  sits (#134, 2026-08-08).** So a call to a function declared BELOW it works,
  mutual recursion works regardless of order, and a struct can be constructed
  above its `struct` line — at EVERY scope, not just the top level. Sound
  because such a declaration cannot depend on the enclosing frame: the grammar
  REJECTS a capture list on a named func (`func f[a](x)` is a SyntaxError —
  captures are for closure EXPRESSIONS only) and a named func's body is
  parented to `capture_root`, the program root, so it cannot read an enclosing
  local either. A LAMBDA is deliberately excluded: `var f = func(x) {...}` is
  an `Expr14` var declaration, so it keeps its declaration-point binding and
  its TDZ — its capture snapshot must happen where it is written.
  TWO twins that must stay in lockstep, or the engines diverge on WHEN a name
  becomes callable: `bind_hoistable_decls` (eval.cpp, both the scope_free and
  general Block paths) and `gen_stmts`' decls-first emission (codegen.cpp).
  **Re-entering a block re-runs them**, so a decl in a LOOP BODY is still
  re-bound per iteration exactly as before.
  ⛔ **MEASURING THIS NEEDS AN IMPURE CALLEE AND A `runtime()` STRUCT ARG.**
  A tiny func is INLINED at the call site and a const-arg construction is
  CONST-FOLDED — either makes a test pass without the binding ever being
  needed, and both masked this bug for a long time (they are why the earlier
  claim "top level already works" was believed).
  **THREE PASSES MUST AGREE ON THIS, and the third was the last to learn
  it (#136).** The resolver hoists the names (`hoist_scoped_decls`), the
  engines bind them at scope entry (above) — and the INFERENCER's
  structural pass has to pre-declare them per BLOCK too. It did that only
  for the root (`hoist_globals`); inside a block it walked declarations in
  statement order, so a call above the declaration got a callee whose
  static type had not settled to `Func`, `annotate_hints` left
  `CallExpr::vm_direct_func` false, and the codegen REFUSED to lower the
  call (`NotLoweredEx`) while the tree-walker ran it. A struct used above
  its `struct` line typed as `none`, so `p.x` became a bogus
  `NullabilityEx`. The tell is `typestr`: `func inner()` before the decl
  vs `func()->int` after, where the top level said `func()->int` in both.
  **If you add a fourth pass that cares about declaration order, it needs
  the same pre-scan.**
  **Scope, still map-bound:** lambdas (anonymous — no name binding; their
  params/locals ARE slotted) and, in the **REPL** (top-level names stay
  redefinable), all top-level names; template-instance clones, inserted before
  resolve_names, ARE hoisted. **Optimizer-inserted `name$sN` specialization
  clones**, though created *after* the hoist, are ALSO given a global slot in
  SCRIPT mode (the Inliner appends the clone name to the root block's
  `global_func_names` and stamps the clone decl + the redirected call's callee as
  `SymKind::global`) — so a specialized call is an O(1) slot read, not a map
  walk. In the REPL (no global table) a spec clone stays map-resident.
- **The script runtime symbols map is EMPTY (asserted).** Once const-evaluation
  finishes, a SCRIPT (not the REPL) has 100% of its names slotted (locals/
  globals/captures/builtins/spec clones/structs/nested decls), so the runtime
  `EvalContext::symbols` map is never a resolution fallback. The script root
  therefore loads **no** builtins into the map (they are `SymKind::builtin`
  slots); only a const-eval root (`const_builtins`, for parse-time folding) and
  the REPL (both, open-world) populate it. `EvalContext::repl_mode` (a new
  inherited flag, set on the REPL runtime root) distinguishes the REPL. The
  invariant is enforced: `emplace` asserts `in_const_eval() || repl_mode`, and
  `lookup` asserts `in_const_eval() || repl_mode || symbols.empty()` —
  `in_const_eval()` walks the parent chain for a `const_ctx` ancestor, because
  AutoConst folds pure functions in throwaway non-const args contexts whose ROOT
  is the const `cctx` (so a struct/func decl inside a folded body legitimately
  emplaces into a discarded map). Since FIX-1 (#130) a genuinely-undefined
  name is refused at COMPILE time, so that runtime path is now reachable only
  in the REPL (open world) - which is exactly why the invariant holds.
- **Captured variables are slotted (`SymKind::capture`).** A closure's explicit
  `[x,y]` capture list is snapshot into a per-instance **`CaptureSlots`**
  **`FuncObject::capture_slots`** at closure creation (`func.cpp.h`), in
  declaration order; a body reference to a captured name resolves to
  `SymKind::capture` + its index there, read/written via **`EvalContext::
  captures`** (the called closure's vector, set in `do_func_call`, inherited by
  nested blocks) — an O(1) slot, **no map walk**. This storage lives in the
  `FuncObject`, NOT the per-call `Frame`, because a mutable-by-value capture
  must **persist across calls** to the same closure (a counter) — captures are
  per-closure-instance, not per-invocation; the per-call Frame would reset them.
  The resolver gives each function a **capture scope** (outermost, so a param
  shadows a same-named capture) with `SymKind::capture` indices in a slot space
  separate from the frame's `next_slot` (`process_function`). Capture indices
  match the ctor's fill order, so a nested capture chain
  (`func[a]{func[a,b]{…}}`) resolves correctly — the inner's capture-list entry
  reads the middle's capture/param slot, snapshot into the inner's capture slot.
  `Identifier::do_eval`/`eval_int`/`eval_float` read a capture slot;
  `handle_single_expr14` writes it (the shared `slot_rmw` helper backs the local
  / global / capture assignment fast paths). `clone()` of a capturing
  `FuncObject` deep-copies `capture_slots` (independent per clone); a
  non-capturing one clones to itself (the `capture_slots.empty()` check in
  `TypeFunc::clone`). **`CaptureSlots` (eval.h) is a hand-rolled tiny vector,
  not a `std::vector`** (G2, 2026-08-06): `inline_cap` (2) slots live inside
  the `FuncObject`, so the common closure — every capture list in bench/,
  samples/ and tests/functional/ is exactly ONE name — allocates NOTHING,
  and only a wider list takes a heap buffer. **Its data pointer MUST stay the
  FIRST member**: the JIT walks `ctx->captures->data()` as a bare
  `mov r9,[rax+0]` (`emit_ctx_chain_r9`), which is where libstdc++ puts
  `vector`'s `_M_start`; a `static_assert` in `layout_contract()` enforces it.
  A `CaptureSlots` is neither movable nor assignable, since its pointer may
  point into itself.
- **Builtins are slotted (`SymKind::builtin`).** A builtin reference the
  resolver couldn't shadow with a user symbol resolves to `SymKind::builtin`
  plus an index into the program-wide **builtin table** (`builtin_slot`,
  `types.cpp`: a flat `vector<LValue>` built once from `const_builtins` +
  `builtins`), read by
  `Identifier::do_eval` — an O(1) slot, **no scope-chain map walk for `print`,
  `len`, `max`, …**. With this, a compiled script's runtime `EvalContext::
  symbols` map is **empty and never a resolution path** (asserted — see *The
  script runtime symbols map is EMPTY* above): the map is populated only by the
  REPL (open-world redefinition) and the parse-time const-evaluator (which runs
  before slots exist). Since FIX-1 (#130) such a name is a COMPILE error in a
  SCRIPT, so that path is now REPL-only. **A user symbol always wins** — the
  resolver checks scopes (local/param/capture)
  then the global table (user functions + escaped vars) BEFORE the builtin
  table, so a `func len(x)` shadow resolves to `SymKind::global` and the builtin
  is unreachable by name (`var <builtin>` for a *const* builtin is still a
  parse-time `CannotRebindBuiltinEx` via the parser's `declExprCheckId`,
  untouched). Builtin resolution for a function-body name is **deferred** to
  post-pass-2 alongside escaped-ref stamping (`stamp_builtin`), because a user
  `var print` read by a function (vars aren't hoisted) must still win — a user
  global, else a builtin, else the map. Table entries are forced **is_const**,
  so an `aBuiltin = x` assignment to an unshadowed builtin still raises
  `CannotRebindBuiltinEx` and the shared singleton table can't be corrupted
  (it outlives any one program, e.g. across `-rt` tests). **Const-context
  subtlety:** in a const-eval context (`AutoConst` / the inliner's `refold`,
  both `cctx(nullptr, true)`) `do_eval` makes a SymKind::builtin read return
  `UndefinedId` for a *runtime* builtin (only `const_builtins` are visible
  there, mirroring the const `EvalContext`) — so an `append()`/`print()` call
  stays unfoldable (those passes catch the resulting `UndefinedVariableEx` to
  keep it a runtime call); without this an `append(const_arr, …)` would be
  wrongly evaluated at compile time. Not used in the REPL (builtins stay
  map-resident, so they remain redefinable).
- **`UniqueId`** (`uniqueid.h`) interns identifier strings in a global
  `std::set`; symbols are keyed
  by the interned *pointer*, so lookup is pointer comparison. (Global mutable
  state lives in
  `types.cpp`: `UniqueId::unique_set`, `EvalContext::builtins`, `AllTypes`,
  `empty_str/empty_arr/none`.)

## Evaluation specifics worth knowing before editing `eval.cpp`

- **`return`/`break`/`continue` are signaled via `FlowState`, NOT C++
  exceptions.** Each
  `EvalContext` carries a `FlowState *flow` (`eval.h`) pointing at one
  `FlowState` (a `type` enum + an `EvalValue value`) per *function invocation*:
  function-boundary contexts (`func_ctx`) and the root own theirs, nested
  blocks/loops inherit the parent's pointer (so a fresh one per call —
  recursion never shares).
  `BreakStmt`/`ContinueStmt`/`ReturnStmt::do_eval` just set `ctx->flow->type`
  (and `->value` for ret)
  and return; `Block::do_eval` stops its statement loop the moment
  `flow->type != none`; the loop
  evaluators (`While`/`For`/`Foreach::do_iter`) consume `brk`/`cont` (resetting
  to `none`, with `for`
  still running its `inc` on `cont`) and let `ret` pass through; `do_func_call`
  reads `flow` after the
  body and returns `flow->value`. `finally` (the scope guard in
  `TryCatchStmt::do_eval`) *suspends* an
  in-flight signal around the finally body, then resumes it (unless finally
  raises its own). This
  replaced exception-based control flow because a C++ `throw` costs ~1.6µs here
  (heap alloc + DWARF
  unwinding, irreducible by build flags) and `return` fires constantly — see
  `bench/` for the ~9×
  speedup on recursion. **Only genuinely exceptional control flow still throws
  C++ exceptions:**
  runtime errors (`RuntimeException` subclasses), user `throw`
  (`ExceptionObject`), and `rethrow`
  (`RethrowEx`, defined locally in `eval.cpp`) — caught by
  `do_catch`/`TryCatchStmt`.
- **⛔ `&&` / `||` SHORT-CIRCUIT — in THREE places that must agree (#138,
  2026-08-09).** The determining operand (false for `&&`, true for `||`) stops
  the chain: the rest is not evaluated, so its side effects do not happen and
  its errors are not raised. Before this only the CONST-FOLD skipped the tail,
  so `const F = false; F && side()` skipped it while
  `runtime(0) > 1 && side()` did not — the same operator, two behaviours,
  decided by whether const-eval could see the operand.
  The three implementations are **`Expr11`/`Expr12::do_eval`** (the boxed
  tree-walker), **`TypedScalarExpr::eval_int_body`'s `Cat::logical`** (the M8
  typed form), and **`emit_logical_chain`** (codegen — a logical chain is the
  one boxed chain that is NOT a straight op run: it needs real branches, one
  `JumpUnlessTrueV` per operand plus an extra `Jump` for `||`, every path
  writing the SAME dst). Which form a node gets is an inference detail the
  program cannot see, so a divergence between them is invisible until it
  bites. **A VALUE assertion cannot test any of this** — `a && b` was always
  `bool(a) && bool(b)`, so only side effects and errors changed; test with a
  call counter or a would-throw tail.
- **`Construct::eval()` wraps `do_eval()`** to attach the node's source `Loc` to
  any in-flight
  `Exception` that doesn't already carry one — this is how runtime errors get
  pointed at source.
  Override `do_eval`, not `eval`.
- **Function call scoping is lexical/closure-based, and the call model is
  AST-FREE.** `do_func_call` binds params into an `args_ctx` whose parent is
  the function's **`capture_root`**, not the call site. A `FuncObject` holds a
  **`const FuncDescriptor *`** (funcdesc.h — NEVER a `FuncDeclStmt*`), the
  per-instance **`capture_slots`** (captured values, read via
  `SymKind::capture`; snapshot at creation from the descriptor's RESOLVED
  capture kind/slot list via `read_sym`, the descriptor twin of
  `Identifier::do_eval` — no capture Identifier is evaluated), and
  **`capture_root`** — the program root (`get_root_ctx` at creation), which
  the body's `args_ctx` parents to so it reaches `gfuncs` + the builtins map.
  That used to be an `EvalContext capture_ctx` BY VALUE, an empty node whose
  only job was to be that parent; constructing one cost ~39 Ir per closure and
  carried a `std::map` nobody wrote, and since the ctor inherits
  repl_mode/frame/gfuncs/captures from the parent and the node passed all four
  through unchanged, parenting straight to the root is byte-identical (G2).
  Binding reads the descriptor's `ParamDesc` snapshot (uid, opt, const,
  decl_type); the tree-walker reaches the body via `desc->decl` (null after
  the `-vm` AST teardown, where every call runs the compiled chunk). The
  descriptor is created by the `FuncDeclStmt` ctor, param-synced at parse end
  (`sync_params`, also after a template/spec clone gets its synthetic id),
  and holds the SINGLE storage of `resolved`/`frame_size`/`min_args`/purity/
  `display_name`/`cache_results`/`vm_chunk` — the compiler passes write it
  directly (no decl-side copy to drift). So functions cannot see caller
  locals or globals, only captures (and, for pure funcs, only consts +
  params). Builtins are different: they receive the **caller's `ctx`** and
  the **unevaluated** `ExprList`.
- **Devirtualized direct calls (`DirectCallExpr`, `syntax.h`).** When the
  resolver proves a `CallExpr`'s callee is an identifier bound to a global-table
  slot (a top-level/scoped function, an escaped global, or a struct descriptor),
  it records the slot on `CallExpr::direct_func_slot`, and a slot-based swap pass
  at the end of `resolve_names` (`devirtualize_direct_calls`, run after the
  inliner so spec clones + redirected calls are covered, before
  `specialize_types`) replaces that `CallExpr` with a **`DirectCallExpr`** (a
  subclass; `CallExpr` is no longer `final`). Its `do_eval` reads the callee
  **straight from the global slot** and, when the slot holds a `FuncObject`,
  jumps to `do_func_call` — skipping the generic callee eval (the
  `Construct::eval` wrapper + `Identifier::do_eval`), the `RValue` copy /
  refcount bump, and the `Builtin`/`FuncObject`/`StructTypeDef` dispatch. A
  runtime `is<FuncObject>` check keeps it sound: a struct construction
  (`P(...)`, a global-slot call too), a slot reassigned to a non-function, an
  undefined slot, or the REPL (no global table) **falls back to
  `CallExpr::do_eval`**. It is a SEPARATE node, not a flag on the hot
  `CallExpr::do_eval`, precisely so the plain-call path (builtin / closure /
  lambda calls) is left byte-for-byte unchanged. **Effect:** ~15% on
  `bench/09_fib_recursive` and call/recursion/dict-heavy code; neutral on the
  broad suite (most micro-benchmarks aren't call-bound). Never set in the REPL
  (top-level names are map-resident, not global slots), so REPL calls stay plain
  `CallExpr`. coderender / serialize treat a `DirectCallExpr` as the `CallExpr`
  it subclasses.
  The same swap also specializes a call whose callee is an **unshadowed builtin**
  (`SymKind::builtin`) into a **`DirectBuiltinCallExpr`**: the builtin table is an
  immutable singleton, so it **bakes the builtin's function pointer** at compile
  time (`builtin_slot(slot).getval<Builtin>()`) and `do_eval` calls it straight
  (`builtin.func(ctx, args.get())`) — even leaner than `DirectCallExpr` (no slot
  read, no `defined` check, no soundness fallback, since a builtin is always
  callable and never reassigned). The builtin still gets the **caller's `ctx`**
  and the **unevaluated** args, and `do_eval` reproduces the generic path's
  argument-list loc stamping so **error reporting is byte-identical**. **Effect:**
  ~20% fewer instructions on `bench/40_math_builtins` (cheap builtins in a tight
  loop, where the per-call dispatch dominated); negligible where the builtin body
  dominates (`sort`, big-array `sum`). Like `DirectCallExpr`, it is a separate
  node so `CallExpr::do_eval` is untouched; never created in the REPL.
- **Trailing `opt` parameters are skippable at the call site.** The
  `do_func_bind_params` overloads (`eval.cpp`) accept any arg count in
  `[FuncDescriptor::min_args, nparams]` and bind each omitted trailing param
  to `none`. `min_args` is `1 + the index of the last non-opt param` (0 if
  all opt), computed EAGERLY by `sync_params` (the old lazily-cached
  `min_args_cache` is gone) — so a non-opt param *after* an opt
  one raises the minimum and can't be skipped (`f(x, opt y, z)` still needs 3).
  The inferencer's `check_call` enforces the same `[min, nparams]` range
  (`WrongArgCountEx` with a "MIN to MAX" message); no per-call type contribution
  is needed for the omitted params, since an `opt`-declared param is already
  typed `opt T` at finalization (so the body must null-check it). The inliner /
  tail-inliner / specializer all bail on an arg-count ≠ nparams mismatch, so an
  under-arity call simply runs through `do_func_call` at runtime (correct).
- **Const parameters.** A param declared `const` (`func f(const x, y)`, parsed
  by `pFuncParam`, flagged `Identifier::const_param`) is bound as a const
  `LValue`, so reassigning it throws — caught at compile time by the resolver
  (a `const` param with a nonzero body write count → `CannotRebindConstEx`) and,
  as a fallback, at runtime. Params are otherwise bound **mutable** — even
  during const-eval — so a (pure) function may reassign its own by-value params;
  binding const-ness is keyed off `const_param`, *not* `ctx->const_ctx`. A plain
  param the resolver finds is never reassigned (`slot_writes == 0`) is tagged
  `auto_const_param` (effectively const; used by `isconst()`).
- **`clone()` semantics differ by capture.** A non-capturing `FuncObject` clones
  to *itself* (shared
  `shared_ptr`); a capturing one is deep-copied (its `capture_slots` vector
  copied) so each clone has independent
  captured state. This is
  the mechanism behind the counter/closure examples in the README. (Decided by
  `capture_slots.empty()` in `TypeFunc::clone`.)
- **Multiple assignment & array expansion** all funnel through `Expr14` +
  `handle_single_expr14`. An `IdList` lvalue with an **array** rvalue
  destructures element-wise (`var a,b = [1,2]`); with a **non-array** rvalue it
  spreads the same value to each (`var a,b = 0` → both 0 — a deliberate
  convenience). Both the multi-assign array case (`handle_single_expr14`) AND
  `foreach` array-destructuring (`do_iter`) are **STRICT**: the array must have
  EXACTLY as many elements as targets, else `TypeErrorEx` ("cannot unpack an
  array of length M into N variables"). The old lenient behavior — pad short
  with `none`, drop extras, treat a scalar element as the first value — was
  removed (it hid bugs and blocked a native lowering to plain scalar reads).
  `foreach` runs strict on both engines (the VM falls back to `do_iter` for
  unpack). The `indexed` keyword rides the same path. **`_` is the destructuring
  PLACEHOLDER** (`Identifier::is_underscore`, syntax.h): in an `IdList`
  destructure target OR a `foreach` loop-var position it is NOT declared and NOT
  bound (its array slot is skipped), so it may **repeat** (`var a, _, _, d =
  [1,2,3,4]`) and reading it is an `UndefinedVariableEx` — but it still **counts
  toward the strict arity**. It is skipped at four sites: the two eval binders
  (`handle_single_expr14`'s `IdList` loop, `bind_loop_var`) and the two declare
  paths (resolver `declare_lvalue` IdList + the foreach loop, inferencer
  `declare_target` IdList + the walk_struct foreach — which also skips its
  no-`var` shadow check for `_`). **`_` is RESERVED** — it may ONLY appear in a
  skipped placeholder position: a readable declaration of it (a single `var`/
  `const`, a parameter, a `catch` var, a function name) is a compile
  `SyntaxErrorEx`, enforced by ONE guard in the inferencer's `new_sym` (every
  such name funnels through it, while the destructuring/foreach `_` is skipped
  *before* reaching it — so the guard reserves `_` without touching the
  placeholder). It runs for script + REPL + `-nti` (the structural pass). A `_`
  **struct field** or **dict key** is still allowed (a member/key accessed via
  `.`/`[]`, not a bare readable identifier).
- **`++` / `--` (`IncDecExpr`, `syntax.h`)** — C-style pre/postfix increment and
  decrement, **int/float only**. `IncDecExpr::do_eval` evaluates the operand
  exactly ONCE via two paths: when the inferencer proved it int/float (`th` is
  `i`/`f` — the usual case, incl. flat-array elements and POD fields that have
  no `LValue`) it routes the mutation through `handle_single_expr14`
  (`operand += 1`), reusing every store fast path (slot, flat array, COW,
  struct), and **derives `old = new ∓ 1`** for postfix so it never re-reads the
  operand; a `dyn`/un-hinted operand (always `LValue`-backed) goes through a
  read-modify-write so the int/float requirement is enforced at runtime. The
  **inferencer** types it (`type_of` = `operand ± 1`) and the **check pass**
  rejects a non-lvalue, a `const`, or a non-int/float operand (bool included) at
  compile time — `var b=true; b++` is a `TypeMismatchEx`, not a silent int.
  The **resolver** counts it as a write (`count_write`), so a `++`'d var is not
  auto-const-promoted; the **inliner** refuses to inline an expression body that
  reassigns a SCALAR param (`func f(x)=>x++` — `mutates_a_param`), since the
  param is a by-value copy (a mutation *through* a param — `p.x++`, `a[i]++` —
  is allowed: that already has reference semantics, so inlining matches the
  call). Lexing is maximal-munch, so `--1` is decrement-of-`1` (a compile error,
  like C), not `-(-1)`.
- **Dict access: throw-on-missing-read, insert-on-write, or default.**
  `TypeDict::subscript(what, key, for_write)` and `MemberExpr::do_eval` (which
  share the logic) handle a missing key by: returning the dict's default (a
  *default dict* from `dict(default_value)`, `DictObject::{has_default,
  default_val}`); else, on a **plain-assignment target** (`for_write`),
  auto-vivifying (insert `none`/default) so `d[k] = v` inserts; else **throws**
  `KeyNotFoundEx` — so a *read* or *compound assign* (`d[k]`, `d.k`, `d[k]+=1`)
  of a missing key in a plain dict throws rather than yielding `none` (the read
  is non-`opt`). `for_write` is `EvalContext::assign_target`, set by
  `handle_single_expr14` only for `op == assign` and **consumed** by the
  outermost subscript/member (so a nested base like `d[k1]` in `d[k1][k2]=v` is
  read, not vivified). `get!()`/`get()` are the explicit fail-fast / nullable
  accessors. A `const` dict folds known-key reads at parse time (a known-missing
  key therefore throws at *compile* time). The clone paths
  (`TypeDict::clone`, `clone_to_mutable`, `make_const_clone`) preserve
  `has_default`/`default_val` (`dict()` stays a const builtin, so `dict(0)`
  folds — hence the clone-preservation matters).
- **Typed (M8) dict read fast path.** When the inferencer proves `d.k` / `d[k]`
  is a non-null int/float (a `dict<_, int>`/`<_, float>` value), the specialized
  arithmetic calls `MemberExpr`/`Subscript::eval_int`/`eval_float`. Those read a
  **present** key's value directly via `dict_present_value` (`eval.cpp`) — no
  re-evaluation of the base. The OLD code fell through to `Construct::eval_int`,
  which re-ran `do_eval` and **re-fetched the dict** (a double eval per access);
  removing that is why `bench/25_dict_member` beats CPython. A **missing** key
  falls back to `do_eval`, so the default-dict vivify / key-freeze /
  `KeyNotFoundEx` behavior is byte-for-byte unchanged (only the common
  present-key path is fast).

## Copy-on-write containers

Strings, arrays, and dicts are reference-counted with value semantics preserved
via COW. The handle is **`intrusive_ptr<T>`** (`intrusiveptr.h`), not
`std::shared_ptr`: a single-threaded interpreter doesn't need shared_ptr's
two-word layout (object + separate control block) or its *atomic* refcount ops,
so the count lives in the pointee (which inherits `RefCounted`) and retain/
release are plain `++`/`--`. This is what keeps `SharedArrayObj`/`SharedStr` at
24 bytes (so `EvalValue` is 32 and the array element `LValue` is 48) and removes
the atomic-refcount churn from copy-heavy array/dict code. **Gotcha:**
`RefCounted`'s copy/move ctors reset the count to 0 — a cloned object owns a
fresh count, never the original's (else it would never be freed). `use_count()`
keeps shared_ptr's meaning (handles sharing the pointee), so the `> 1` COW tests
are unchanged.

- **`SharedStr`** (`sharedstr.h`): an `intrusive_ptr<StrObj{string}>` plus an
  `off`/`len` WINDOW (StrObj just wraps the string so it can carry the count).
  Copies are forbidden (`= delete`), only moves — enforcing the
  no-accidental-copy intent.
  **THE WINDOW MODEL (2026-08-06):** EVERY SharedStr is a window over a
  shared, APPEND-ONLY buffer — what a slice always was; a "full" string is
  just the window that covers the whole buffer. Because the buffer only ever
  GROWS (the one in-place mutation is `TypeStr::append`), a window taken at
  any moment keeps denoting the same characters forever — that single fact is
  the entire soundness argument, and it is what gives strings their
  documented VALUE semantics with **no copy-on-write clone at all**: after
  `var b = a; a += "!"`, `a`'s window grew and `b`'s did not, so `b` still
  reads "hi" while sharing the buffer. Three things to preserve: `len` is
  AUTHORITATIVE for both forms (so `size()` is a field read, not a load
  through `obj`); `append` may grow in place ONLY when `owns_whole_buffer()`
  (this window ends where the buffer ends — otherwise it would swallow the
  bytes past a PREFIX window, so it rebuilds); and `after_inplace_append()`
  must BOTH resync `len` AND drop `obj->hash_valid`, because the StrObj's
  cached hash is the hash of the WHOLE buffer — without that, a string used as
  a dict key and then appended to lands under a stale hash and becomes
  findable by NO key while still printing and comparing equal.
  Before this, `append` grew the shared string with NO test, so an alias, a
  bound parameter, an array element and even `clone()` saw a later `+=`,
  contradicting the README. The naive fix (`use_count() == 1`) is WRONG and
  was measured: the compound path copies the value out of the slot first, so
  the count is never 1 and every append rebuilds — 28_str_concat **+18268%
  Ir**, O(n²).
- **`SharedArrayObj`** (`sharedarray.h`):
  `intrusive_ptr<SharedObject{ vec, set<live slices> }>` +
  `off`/`len`/`slice`. A slice registers itself in the parent's `slices` set
  (and unregisters on
  move/destroy — **and on `operator=`**: a copy/move *assign* into a
  slice-holding value must first `slices.erase(this)` from its OLD `shobj`, or
  that set keeps a **dangling pointer** to a value that now holds a different
  array — the dtor did this but the two assign operators used not to. A
  tree-walker temporary is torn down via the dtor, which masked it; a **VM frame
  slot** holding a slice and then *overwritten* (slot reuse) hits the assign
  path and freed the memory, so the stale registration became a genuine
  use-after-free — UBSan-caught during an array COW). Writing through an
  array-element `LValue`
  (`LValue::get_value_for_put` in `eval.cpp`)
  triggers COW: if the container is a slice, or is aliased (`use_count > 1` /
  has live slices), it is
  cloned first so the write doesn't bleed across logically-distinct arrays.
  **Length invariant:** for a *non-slice*, `len` is only the size at
  construction and goes stale once `+=`/`append`/`insert`/... grow the vector in
  place — a non-slice reports its length via `size()` (= `vec.size()`), and
  `offset()`/`size()` are the only correct way to read its range. (Bug to avoid:
  `clone_internal_vec` must use `offset()`/`size()`, not the raw `off`/`len`, or
  it truncates a grown array. `clone_aliased_slices` therefore clones each slice
  while its `slice` flag is still set, so `offset()`/`size()` report the slice
  range.)
- **Flat STRING storage (`Storage::strs`, top-10 #7)** - a
  `vector<SharedStr>` (24B handles vs 48B LValues), following the STRUCT
  model, NOT the scalar one: VALUE-driven creation (`split()`/
  `splitlines()` always; a string dict's `keys()`/`values()` under a
  dflt hint; plain literals stay general), and any cold/unhandled op
  AUTO-PROMOTES via `get_vec()` - a non-string element write PROMOTES
  (flat_store_core stores a plain str flat, anything else promotes +
  defers to the general path; `TypeArr::subscript` promotes on
  for_write THROUGH THE ORIGINAL LVALUE - promoting the RValue() temp
  freed the fresh vector with it, an ASan-caught UAF). `array<str>`
  destinations get NO ArrHint (dflt - the value keeps its storage; the
  old forced `general` would general-ify a baked flat literal).
  `array_storage()` reports `"str"`. Fast paths: arr_elem_at/boxed,
  join (direct SharedStr reads), foreach (a tree-walker do_iter branch
  + the VM LoadElemValue strs arm), clone/const-clone stay flat
  (strings are immutable - a handle copy IS the deep copy); sum/min/max
  promote a LOCAL handle copy (the caller keeps flat; min/max's old
  `!= general` guard mis-read the union for strs AND structs - a
  latent pre-existing struct-array InternalError, fixed the same way).

- **Flat (unboxed) int/float/bool storage.** `SharedObject` carries a `Storage
  kind` (`general`/`ints`/`floats`/`bools`) and an **anonymous union** of `vec`
  (the `vector<LValue>`, 48-byte slots), `ivec` (`vector<int_type>`), `fvec`
  (`vector<float_type>`), and `bvec` (`vector<unsigned char>`, **one byte** per
  element) — the flat members are unboxed, so a homogeneous int/float array
  moves ~6× less memory in bulk ops and a bool array ~48× less. A `union`
  member can't have non-trivial ctors/dtors, so `SharedObject` placement-news
  the live member per kind and the dtor switches on `kind`.
  **Representation is type-driven and fixed at creation — there is NO runtime
  promotion** (`promote_to_general` was deleted). An array-producing node is
  built flat **iff** the inferencer proved its *destination* is
  `array<int>`/`array<float>`/`array<bool>`; a destination that is `array<dyn>`
  (or any other element type) is built **general from the start**. The
  inferencer's
  `set_array_repr_hint` (in `annotate_hints`, runs on `a = <rvalue>` decls and
  assigns) stamps an **`ArrHint`** (`syntax.h`: `dflt`/`general`/`flat_i`/
  `flat_f`/`flat_b`) on the rvalue — on a
  `range()`/`array()`/`make_array()`/`keys()`/`values()` call's args `ExprList`,
  or directly on an array literal / folded `LiteralObj`. A
  `dyn`-typed destination (`var dyn d = [1,2,3]`) also gets `general`, so
  declaring `dyn` builds a polymorphic array from the start (else a later
  `d[0]="x"` would wrongly hit the flat-array error on an already-`dyn` var).
  Creators honor it: `range`, `builtin_array` (1-arg `flat_i`/`flat_f`/`flat_b`
  → flat `0`/`0.0`/`false` fill, replacing the old `array(N)` rewrite;
  `general` → general),
  `make_array`, `keys`/`values` (`dict_extract` — a scalar dict's keys/values
  build a flat `array<int>/<float>/<bool>` straight from the dict, no
  per-element boxing; the big win for `keys()`/`values()` of a large dict, and
  why `bench/27_dict_keys_values` beats CPython — only the bound form
  `var k = keys(d)` gets the hint, an inline `keys(d)` arg stays general),
  `LiteralArray::do_eval`, and `LiteralObj::do_eval` (a flat baked
  literal bound to an `array<dyn>` dest is made general via
  `make_general_array_clone`).
  The fixpoint propagates the destination type through direct aliases
  (`var b = a`), so they agree. Value-driven flat (`dflt`) is the fallback for
  contexts the hint doesn't cover. The flat fast paths (each branches on
  `skind()`, reads `flat_ints()`/`flat_floats()`/`flat_bools()`/`arr_elem_at`
  directly, never
  promotes): `sum`/`reverse`/`sort` (comparator and not — `comparator_heapsort`
  is templated over the element vector)/`min`/`max`/`append`/`pop`/`top`/`find`/
  `erase`/`insert`/`map`/`filter`/`foreach`/`intptr`/`dict`(from pairs)/`join`/
  `writelines`, `TypeArr::{subscript (rvalue read), to_string, eq, add}`,
  `Subscript::eval_int`/`eval_float`, the array-spread reads
  (idlist/`foreach`-tuple, via `arr_elem_boxed`), and the flat subscript-store
  `try_flat_subscript_store` (`eval.cpp`: `a[i] = v` / `a[i] OP= v` writes the
  scalar straight into the flat vector — gated on a side-effect-free lvalue
  *chain* via `no_side_effects`). `arr_elem_at`/`arr_elem_boxed` box a flat bool
  element as a real `bool`; `sum` of an `array<bool>` returns an int (counts the
  trues). `clone_internal_vec`, `make_const_clone`, and
  `clone_to_mutable` are kind-aware so clone/COW/const keep flat. `size()` is
  kind-aware. **`get_vec()`/`get_view()` are general-only** — they throw
  `InternalErrorEx` on a flat array (an invariant tripwire, not a promotion);
  every caller either guarantees general or reads flat directly. **The
  dyn-launder error:** the only way a flat (statically-typed) array can be asked
  to hold a non-fitting element is by laundering it through a `dyn` alias and
  mutating it (`var dyn d = int_array; append(d, "x")` / `d[0]="x"` / `insert`).
  Since the storage stays int-typed and an alias-affecting write can't change
  its representation without promotion, that **throws a `TypeErrorEx`** (message
  `flat_array_violation_msg`) — declare the array `dyn` from the start, or
  promote an existing one with the **`dynarray(a)`** builtin
  (`builtin_dynarray`: a fresh general copy, typed `array<dyn>`, usable in any
  position; `clone`/`deepclone` deliberately preserve the layout). `runtime()`
  does *not* promote — it only relabels the static type, not the storage.
  `array_storage(a)` reports the element-type name
  `"int"`/`"float"`/`"bool"`/`"struct"`/`"general"` (tests pin it). **Gotcha:**
  any pass that
  inspects a const array's element type must read from `skind()`, not
  `get_view()`/`get_vec()` (now they'd throw on flat anyway). `array()` is a
  **non-const** builtin (never folds to a baked literal, so `array(N)` is always
  a runtime call the hint reaches, and a huge `array(1000000)` isn't baked). See
  `plans/archived/typed-arrays.md` (approach B) and
  `plans/archived/type-driven-specialization.md`.
- **`DictObject`** (`shareddict.h`): the value handle is
  `intrusive_ptr<DictObject>` (the object inherits `RefCounted`); the map is a
  **`std::unordered_map<EvalValue, LValue>`** inside it — an O(1) hashmap (NOT a
  sorted tree), keyed by `std::hash<EvalValue>` (→ `EvalValue::hash()` →
  `Type::hash`) and `EvalValue::operator==`.
- **Universal `hash()` (deep, by value).** `Type::hash` returns `size_t`; the
  base throws, the leaves (`int`/`bool`/`float`/`str`) hash directly, and
  `none` + the containers/structs hash deeply (`hashing.h` combiners,
  `#include`d into `types.cpp`): **`hash_combine`** (a SplitMix64-avalanche
  fold, order-DEPENDENT) for sequences — `TypeArr::hash` over `arr_elem_at`
  elements,
  `TypeStruct::hash` over fields in declaration order (salted with the def
  pointer so distinct struct types differ; field-wise via `pod_get`, consistent
  with `eq` for POD and boxed) — and **`hash_unordered`** (a commutative
  SplitMix64-avalanche sum, NOT a weak xor) for `TypeDict::hash`, so two equal
  dicts hash equal regardless of insertion order. `hash(none)` is a constant
  (`none` is now hashable — a deliberate spec change from the old throw). The
  scalar `hash()` builtin (`builtins/generic.cpp.h`) is unchanged and folds at
  compile time. **String hashes are cached** on `StrObj`
  (`mutable hash_cache`/`hash_valid`, computed lazily — strings are immutable),
  so repeated string-key probes don't recompute; a *slice* hashes its sub-view
  on demand. No cycle guard (matches `==`/`to_string`).
- **Flat-scalar arrays cache their hash incrementally** (`SharedObject::
  hash_cache`/`hash_valid`). `TypeArr::hash` returns the cache when valid;
  `append` **maintains** it in O(1) (`arr_append_maintain_hash` — an append is
  one more `hash_combine` step), and every other mutation **invalidates** it
  (`invalidate_hash` at `pop`/`insert`/`erase`/`sort`/`reverse`/`+=`, the flat
  element store, and `get_value_for_put`). Caching is restricted to a non-slice
  **int/float/bool** array (`hash_cacheable`): its elements are scalars, so the
  only way to change its hash is a mutation OF that array, all of which are
  instrumented. A **general/struct** array is *not* cached — a nested mutation
  (`a[i][j]=v`, a struct field, replacing an element) changes the outer array's
  hash with no back-pointer to invalidate it — so it recomputes on demand
  (correct, just not O(1)). A COW clone is a fresh object (the `SharedObject`
  copy ctor is deleted) → starts invalid → recomputes; a read-only array never
  mutates, so its cache, once set, is valid forever (the frozen-flat-key fast
  path). The per-path safety net is `hash(a) == hash(deepclone(a))` after each
  mutation (a stale cache fails it loudly). (An order-dependent removable hash —
  polynomial / Rabin-Karp — could also make `a[i]=v` O(1), but it weakens the
  hash, taxes the write path, and still can't do O(1) insert/erase; rejected for
  B. See `plans/archived/hash-and-dict.md`.)
- **Any-type dict keys, frozen on insert.** Because `hash` is total, an array/
  dict/struct/`none` can be a key. A key would corrupt the map if mutated after
  insertion (its hash would change, leaving it in the wrong bucket — COW does
  NOT protect a stored key), so every insert site (`TypeDict::subscript`'s two
  `emplace`s, `LiteralDict::do_eval`, `builtin_dict`-from-pairs) stores
  **`make_const_clone(key)`** — a deep read-only freeze. Scalars/strings are
  returned as-is (cheap); only a container key pays a one-time clone.
  `MemberExpr` keys are interned strings (already immutable), so they need no
  freeze. See `plans/archived/hash-and-dict.md`.
- **Deep-const read-only flag.** Both `SharedObject` (arrays) and `DictObject`
  carry a `readonly` bool (`is_readonly()`/`set_readonly()`). It backs `const`
  values: `make_const_clone()` (`eval.cpp`) sets it on every array/dict in the
  value, recursively, so `const` is *deep* read-only. The flag lives on the
  shared object, so it travels with every alias and slice; `clone()` builds a
  fresh object and is therefore always mutable (`TypeDict::clone` clears it
  explicitly; `TypeArr::clone` gets a new `SharedObject` for free). Write paths
  check it: `TypeArr::subscript`, `TypeDict::subscript` and `MemberExpr` return
  an *rvalue* (and don't auto-vivify) for a read-only container, so the
  element/member
  assignment fails with `NotLValueEx`; `TypeArr::add` (`+=`) and the mutating
  builtins (`append`/`pop`/`insert`/`erase`) throw `CannotChangeConstEx`;
  `sort()` clones instead of sorting in place. This is what makes a const
  immutable through a non-const alias (e.g. a function parameter) — parse-time
  read-folding alone didn't. (Aside: `builtin_sum`'s general path must seed its
  accumulator with a `clone()` of the first element, since `+=` mutates it in
  place — it would otherwise mutate, or be rejected on, a read-only argument.
  `builtin_sum` also has an all-int fast path that accumulates a raw `int_type`
  in a tight loop — skipping `num_bin_op`'s promotion check and the per-element
  virtual `TypeInt::add` — and falls back to the general loop at the first
  non-int element, so a mixed int/float array still promotes correctly.)
- **Getting a mutable copy of a const: `clone()` vs `deepclone()`.** Two helpers
  in `eval.cpp` make mutable copies (scalars/strings returned as-is):
  `make_mutable_clone` builds a fresh mutable *top* but **shares** any read-only
  sub-object as-is, while `make_deep_mutable_clone` copies every level and drops
  `readonly` (a fully independent writable value). `make_mutable_clone` backs
  the per-eval copy a `var`-bound materialized value needs, and its
  share-the-const behavior is what keeps **`clone()` shallow** (a const nested
  in the result stays read-only) and makes const-ness propagate into fresh
  literals (`var a = [y]` with `y` const keeps `a[0]` read-only). The `clone()`
  builtin is the type's own shallow `clone` (one level); `deepclone()` (a
  runtime builtin, `make_deep_mutable_clone`) is the deep one — the way to
  obtain a fully mutable version of a const. (`deepclone` is *not* a const
  builtin: it yields a mutable value that must be copied fresh per eval anyway,
  so folding it would only bloat the tree.)
- The non-const `intptr(symbol)` builtin exposes the underlying object pointer;
  the test suite uses it
  to assert exactly when two slices do/don't share storage. If you change COW
  logic, those tests are
  your spec.

## Custom struct types

User-defined value types (`struct Point { int x; int y; }`). Full design +
phasing: `plans/archived/structs.md`. **Status: complete (all 8
phases)** — decl,
construction, field/const access, inference, const-folding, COW; the POD
C-layout; nested (recursive) POD; flat `array<PodStruct>` storage; M8-style
*direct* (unboxed) field access; and construct-in-place append. A flat struct
array is now both a memory/cache win and array<int>-speed on field ACCESS
(`a[i].x` reads straight from the bytes, within ~6% of `array<int>`'s `a[i]`).
Construction stays more expensive than an inline int (a struct is an object),
but the per-element `StructObject` allocation is gone (build overhead
2.47x→1.76x vs `array<int>`; `bench/58_structs` ~26x→~21x CPython).

- **POD vs boxed storage** (`StructTypeDef::compute_layout`, run by the parser).
  A struct is **POD** iff every field is a non-opt scalar (`bool`/`int`/`float`)
  **or a non-opt POD struct** (embedded *inline*, recursively); it then has a
  native C byte layout (`pod_field_metrics` assigns offsets with our own fixed
  alignment rules — `pod_field_size`/`align` in `structtype.h`, the one place an
  arch assumption lives, depending only on `sizeof(int_type)`). Any ref/opt/
  boxed-struct field makes it **boxed** (a `vector<LValue>` slot array). A
  nested struct field embeds inline only if its type was *declared before* this
  one (resolved via `const_ctx` in the parser → `FieldDef::struct_def`); a
  forward/self reference to a **non-recursive** type stays a boxed pointer.
- **Recursive-struct rejection** (`check_struct_no_recursion`, parser, run
  before `compute_layout`). A **non-opt** struct field whose type — directly or
  transitively through other non-opt struct fields — contains the struct itself
  is a compile error (`SyntaxErrorEx`, "box it as `dyn? <name>`"): such a value
  can never be constructed (infinite nesting; and inlined, infinite size). The
  field-reference graph among earlier structs is a DAG (inline only goes
  backward), so the only back-edge that can close a cycle is a forward/self
  reference to the struct being declared — detected by a DFS over non-opt struct
  fields (`struct_field_target` resolves each edge by `struct_def`, the
  root's own name, or a `const_ctx` lookup for an intermediate forward ref). An
  **`opt`** field (e.g. `dyn? next`) breaks the cycle (it can terminate with
  `none`) and is allowed — the way to write a linked list / tree.
- **`StructObject`** holds EITHER `bytes` (POD: a `def->size` C-laid-out buffer;
  `pod_get`/`pod_set` load/store a typed scalar or an inline nested struct at a
  field offset) OR `fields` (boxed). A POD field WRITE goes through
  `try_pod_struct_store` (a direct byte store, mirroring
  `try_flat_subscript_store`); a POD field READ in `MemberExpr` returns a value
  (no per-field LValue). `==` is `memcmp` for POD, field-wise for boxed.
- **Flat `array<PodStruct>`** — `SharedArrayObj::Storage::structs`: a contiguous
  byte buffer + the element `StructTypeDef*` + cached `stride` (so the template
  never needs `StructTypeDef` complete). Created value-driven (a literal of
  same-type POD structs → `LiteralArray` mode 5) or type-driven for an empty
  `[]` whose destination is `array<PodStruct>` (`ArrHint::flat_s` +
  `Construct::arr_hint_struct`, honored by `LiteralArray`/`LiteralObj`), so
  `var a=[]; append(a, S(..))` stays unboxed. Hot paths touch bytes directly
  (subscript read/store, append, `==`, `to_string`, clone/const-clone,
  `static_type_from_value`); **`foreach` reuses one `StructObject`** across
  iterations
  (overwrite-in-place with a `use_count` COW guard, so a captured element keeps
  its value). Cold ops (insert/sort/map/...) **auto-promote** to a general array
  via `get_vec()`'s `promote_structs_to_general()`, so every existing array op
  works with no dedicated case and nothing throws. `array_storage` reports
  `"struct"`.

- **Direct (unboxed) field access** (M8, phase 8). The inferencer stamps a
  `TypeHint` on `s.x` when the field is a non-null int/float; `MemberExpr::
  eval_int`/`eval_float` then read it unboxed. `a[i].field` on a flat struct
  array reads the scalar STRAIGHT from the array bytes
  (`member_pod_array_scalar`) with **no per-element `StructObject`** built
  (guarded by `no_side_effects` so the base evals once) — `a[i].x` is within ~6%
  of `array<int>`'s `a[i]`. The resolved-local compound-assign fast path
  (`sx += rhs`) treats a `th==i` rhs (e.g. `p.x`) as an `eval_int()` read, so a
  reduction is fully unboxed.

- **Construct-in-place append** (phase 8). `append(flat_struct_arr, S(...))`
  recognizes a struct-constructor arg for the array's exact POD type and builds
  the element straight into the array's byte buffer
  (`try_construct_into_struct_array`, `eval.cpp`) — no temporary `StructObject`.
  Field args are coerced into a stack buffer first (a throw mid-construct leaves
  the array unchanged), then committed. Named/mixed args (already desugared) and
  nested POD work; a non-constructor / different-type arg falls back to the
  normal value-append (the arg is never evaluated twice). The byte store is the
  shared `pod_store_field` (also backs `StructObject::pod_set`).

- **Two value kinds, mirroring func's decl/object split** (`type.h` `TypeE`):
  `t_structtype` (trivial, `< t_str`, a raw `StructTypeDef *`) is the **type
  descriptor** the `struct` decl binds as a `const` in scope — callable
  (construction) and `.CONST`-accessible; `t_struct` (non-trivial, `>= t_str`,
  `intrusive_ptr<StructObject>`) is an **instance**. `TypeStruct` /
  `TypeStructType` in `src/types/struct.cpp.h`; wired through `ValueU`,
  `TypeToEnum`, `TypeNames`, `AllTypes`.
- **`structtype.h`** defines `StructTypeDef` (owned by its `StructDeclStmt`
  under the tree-walker; under `-vm`, `vm_compile` MOVES ownership into the
  `VmProgram` so it outlives the AST teardown — the decl keeps a raw `def`
  alias. Program-lifetime either way:
  `name`, `fields` as `FieldDef{name, FieldKind, struct_ty, is_opt, slot,
  offset, annot}`, folded `consts`, plus the `Layout`/`size`/`align` fields the
  POD phases will fill) and `StructObject` (`RefCounted`: `def`, `readonly`, and
  a boxed `vector<LValue> fields`). `evalvalue.h` forward-declares both. It also
  defines **`TypeAnnot`** - the recursive parsed type behind the parameterized
  container syntax `array<T>`/`dict<K,V>` (see the "Parameterized containers"
  bullet above); `FieldDef::annot` and `Identifier::decl_annot` hold one.
- **Parser** (`pAcceptStructDecl`, `parser.cpp`): the `struct` keyword
  (`kw_struct`) → a `StructDeclStmt` (owns the `StructTypeDef`;
  `do_eval` binds the descriptor like a func name, so it works inside a function
  too). Fields are `[opt] TYPE name;` (a primitive keyword, `dyn`, or a
  struct-type name → `FieldKind`); `const NAME = expr;` members fold immediately
  (`make_const_clone`). v1 rejects `var` fields and `opt` on a non-ref field;
  and duplicate member names.
- **Construction is a call**: `Point(...)` parses as a `CallExpr`; what makes it
  construction is only that the callee is a struct descriptor (decided in the
  inferencer + at eval, never in the grammar). It reuses the named-arg pipeline:
  the inferencer's `lower_named_args` and `check_call` recognize a struct callee
  and build the `ParamSpec`/param-type list from the fields, so positional /
  named / mixed and the arity-range / per-field-assignability / non-opt-not-none
  checks all come from the shared machinery. `CallExpr::do_eval` →
  `construct_struct` builds the boxed instance; `coerce_struct_field`
  coerces a numeric field and **runtime-validates** each field's type (guarding
  a `dyn`-laundered value; the `dynarray`-style escape hatch is just declaring
  the field `dyn`). `type_of(Point(..))` is `StaticTypeKind::Struct`
  (`statictype.h`'s
  reserved `Struct` kind + `struct_def`/`struct_name`;
  `StaticTypeArena::struct_ty`).
- **Member access** (`MemberExpr::do_eval`): dispatch on the base — a `t_struct`
  instance → `def->slot_of(memUid)` field (an lvalue when mutable, an rvalue for
  a read-only/const instance so a write fails `NotLValueEx`) else a `const`
  member else not-found; a `t_structtype` descriptor → only `const_of`; a dict
  unchanged. `MemberExpr::memUid` (interned `UniqueId`, set in `pAcceptMember`
  beside the dict-key `memId`) drives the slot lookup. The inferencer types
  `s.field`/`Type.CONST` and validates membership; a `dyn`-base read resolves at
  runtime via the tag.
- **`const` works fully**: a struct construction folds **inside a `const` decl**
  (`construct_struct`'s own validation makes it safe), baked deep read-only by
  `make_const_clone`; `MakeConstructFromConstVal` / `clone_to_mutable` /
  `is_readonly_value` / `ShouldConstSymbolExistAtRuntime` /
  `static_type_from_value` all
  handle a struct value. Outside a `const` decl, construction is left a runtime
  `CallExpr` so the inferencer gives the precise field errors.
- **Value semantics**: COW like arrays/dicts (`StructObject::readonly` backs a
  deep `const`; plain assignment aliases; `clone()` shallow, `deepclone()`
  deep). `==`
  is structural between same-`def` instances (`TypeStruct::eq`); `hash`
  combines the field hashes (see *Universal `hash()`* above), so a struct can be
  a dict key. **Deferred** (plans/language-deferred.md): `var` fields
  (call-site field inference), `opt` scalar fields, methods, and struct
  SUBTYPING (`assignable` accepts equal struct types only).
- **A FIELD-LESS struct is SUPPORTED, not deferred** (settled 2026-08-13 —
  the docs had said "rejected at decl time" while the parser accepted it,
  untested, since the feature shipped). `struct Unit {}` — and a struct
  declaring only `const` members, since consts are type-level — parses to a
  zero-field descriptor, and `compute_layout` returns EARLY with
  `layout = boxed, size = 0`. That early return is the load-bearing part:
  a field-less struct is never POD, so an `array` of it is a general array
  and **the flat-struct path's stride is never 0**. WATCHED FAILING —
  making a zero-field struct `Layout::pod` (which the archived plan
  proposed as "size 0, trivially POD") does not merely change
  `array_storage`, it is **undefined behaviour**: UBSan reports "null
  pointer passed as argument 1, which is declared to never be null" at
  eval.cpp's flat-struct copy. Construction,
  equality (always true within a type), `hash`, dict-key use, `clone`/
  `deepclone`, `Unit u;` zero-init, `throw`/`catch` and `.myv` round-trip
  all work and are pinned by the `struct:` empty-struct tests. The useful
  case is a payload-less exception marker type.

## Error model

`errors.h` defines an `Exception` base (`name`, `msg`, `loc_start`, `loc_end`)
and two macros:

- `DECL_SIMPLE_EX` — parse-time / internal errors (`SyntaxErrorEx`,
  `InternalErrorEx`,
  `CannotRebindConstEx`, `ExpressionIsNotConstEx`, …). **Not catchable** from
  script.
- **Compile-time type errors** (`TypeMismatchEx`, `NullabilityEx`,
  `WrongArgCountEx`, `DynRequiredEx`, `OptRequiredEx`) — from the inferencer
  (see
  "Static type inference"). Plain `Exception`s (not `RuntimeException`s), so
  **not catchable** from script; each carries a custom interned message + `Loc`.
  A statically provable type error is reported here, before the program runs.
- **The ASSIGNABLE-SHAPE rule (maintainer's call, 2026-08-06).** When
  not-an-lvalue is decidable from the target's SHAPE, it is a COMPILE failure
  (`SyntaxErrorEx`, from `pExpr14` in the PARSER - so it needs no type
  information and no pass a flag can disable); when it depends on a runtime
  VALUE it stays the catchable `NotLValueEx`. Exactly four forms can denote a
  location: `Identifier`, `IdList`, `Subscript`, `MemberExpr`. A literal, a
  call result, an arith/compare/logical chain, a ternary and a SLICE are
  values, so `s[0:1] = v` / `(a+b) = 3` / `f() = 3` are refused at compile
  time. A CONST element target lands there too, because the parser already
  folded `K[0]` to its literal - which is right: it IS decidable. The same
  const reached through a PARAMETER is not folded, keeps its Subscript shape,
  and still raises the runtime `NotLValueEx`. This closed two divergences: the
  tree-walker reported a slice target as `TypeErrorEx` "does NOT support slice
  operator []" (misleading - the type slices fine, the RESULT is not a
  location), and the no-fail codegen could lower neither a slice nor an
  arithmetic target, so the VM raised a NON-catchable `InternalErrorEx` where
  the tree-walker raised a catchable `NotLValueEx`.
- **`UncatchableRuntimeException` (2026-08-08) — a RUNTIME exception a
  script may NEVER handle.** `RuntimeException` used to conflate two
  unrelated things: *"travels the VM/JIT conveyance, so it gets frames and a
  caret"* (an IMPLEMENTATION property) and *"a `catch` clause may name it"*
  (a LANGUAGE property). They are split now, because the two channels out of
  a JIT fragment are NOT equal: `g_vm_jit_exc` is typed on `RuntimeException`
  and re-raises through `vm_raise` (frames + the baked caret), while a plain
  `Exception` is not carried at all and propagates as a raw C++ throw through
  JIT-generated code — which has **no unwind information**, so the unwinder
  finds no handler and calls `std::terminate`. That was a real SIGABRT
  (`var c = func[zz]() => zz;`, exit 134).
  Members: **`InternalErrorEx`** (an interpreter-BUG tripwire — `get_vec()`
  on a flat array, `pod_get` field validity, a `default:` over a closed enum,
  the codegen-proved arms in vm.cpp; it must RENDER, never abort) and
  **`UndefinedVariableEx`** (REPL-only since FIX-1 #130 makes it a compile
  error in a script). Declared with `DECL_UNCATCHABLE_EX`.
  **Enforcement is BOTH ways**: naming one in a catch clause is a COMPILE
  error (`is_uncatchable_ex_name`, checked in the parser's catch-clause
  parse), and `is_catchable()` is tested by BOTH matchers
  (`vm_catch_match`, `do_catch`) **before any name comparison**, so the
  parenless catch-all cannot swallow one either. Both halves are
  sabotage-pinned.
- `DECL_RUNTIME_EX` — subclasses of `RuntimeException` (adds `clone()` +
  `[[noreturn]] rethrow()`):
  `DivisionByZeroEx`, `TypeErrorEx`, `OutOfBoundsEx`, `KeyNotFoundEx`,
  `NotLValueEx`, `NotCallableEx`,
  `AssertionFailureEx`, `CannotOpenFileEx`, `InvalidValueEx`. These are the ones
  script `try/catch`
  can handle (matched by name).
- **User exceptions are STRUCTS.** `throw <struct instance>`
  (`ThrowStmt::do_eval`, `eval.cpp`) wraps the instance in an
  `ExceptionObjectTempl` (`exceptionobj.h`) — a `RuntimeException` whose C++
  type name is `"DynamicExceptionEx"`, whose `dyn_name` is the **struct type's
  name** (so `catch (T)` matches by type), and whose `data` EvalValue is the
  struct itself. `throw` accepts only a struct instance, or a caught built-in
  exception object (re-throw); anything else is a `TypeErrorEx` (and a
  statically-known non-struct/non-exception throw is a compile-time
  `TypeMismatchEx` from the inferencer). `do_catch` matches a catch clause by
  the `dyn_name`/built-in `name` string, and `catch (T as e)` binds `e` to the
  **struct instance** (`exObj->get_data()`, so `e.field` works) when the
  exception carries one, else to a fresh `ExceptionObject` wrapper (the
  payload-less built-in case — printable/re-throwable). The catch variable is
  typed `dyn` by the inferencer (member access resolved at runtime). `finally`
  runs via a scope guard; `rethrow` re-throws the saved exception with the
  rethrow site's `Loc`. (The old `exception()`/`ex()`/`exdata()` builtins were
  removed once structs existed — a struct field IS the payload.)
- Always pass `Loc start, end` to thrown exceptions where you can, so
  `mylang.cpp` can render the caret.

### Error location & caret rendering

- **A `Loc`'s `end` is "last-char-column + 2"**, so the caret width is
  `loc_end.col - loc_start.col - 1` (and the printed end column is
  `loc_end.col - 1`). A construct that ends with a closing token sets
  `end = <that token's loc> + 2` (e.g. `CallExpr`/`Subscript`/`Slice` through
  their `)`/`]`, array/dict literals through `]`/`}`). Keep this convention when
  adding constructs, or carets will be off by one or two.
- **`Construct::eval`** stamps a node's `start`/`end` onto any escaping
  exception that has no loc yet — so an error gets the loc of the *innermost*
  node whose `eval` it traversed. Because `RValue()` and the type ops throw with
  *no* loc, that used to be the whole enclosing expression. The operator ladder
  (`eval.cpp`) now routes operand evaluation through `num_binop_loc` /
  `logop_loc` / `stamp_operand_loc`, and `Expr14`/`CallExpr` wrap their
  rhs/callee evals, so undefined-variable / type / division errors point at the
  **offending operand** (e.g. `var y = foobar` marks `foobar`, not `y =`).
- **Multi-line spans**: `dumpLocInError` (`mylang.cpp`) renders every source
  line in `[loc_start.line, loc_end.line]` with a caret row per line (start from
  `loc_start.col` to EOL, full middle lines, end line up to `loc_end`).
- **`defined()`-GUARDED NARROWING (#135, 2026-08-09).** FIX-1 refuses a name
  declared nowhere, which left no way to feature-test one. Dart's
  promote-after-a-test is the model: the name that was CHECKED is tolerated
  and nothing else — deliberately NOT "code the DCE will delete may say
  anything", which would lose the typo protection wholesale. The guarded code
  still VANISHES (`defined(x)` folds false, the DCE drops the branch); the
  narrowing only lets it COMPILE. `collect_defined_guards` + a scoped
  `guarded` stack in the resolver's `walk`, consulted at BOTH FIX-1 sites —
  the in-walk one and the `escaped_refs` one, which needs the answer recorded
  on the EscapedRef at PUSH time because it runs after the walk.
  **⛔ TESTING IT NEEDS A COMPILE-ONLY ORACLE.** A tolerated name in a script
  stays unresolved and throws `UndefinedVariableEx` at RUN time — the SAME
  type the compile refusal throws — so a `tests` entry cannot tell "refused"
  from "failed later", and a polarity case written there passes either way
  (watched: with the else-arm polarity broken, that version stayed green).
  The polarity cases run parse+infer+resolve only.
- **COMPILE WARNINGS — the THIRD tier (step 7, 2026-08-09).** The rule is
  PROVE -> fail, SUSPECT -> warn, `--strict` -> enforce. `prove_unbound_calls`
  refuses what it can prove; `warn_unbound_calls` reports the residue it
  DECLINED — a conditional call, a conditional read, a call in a loop body —
  in GCC's spirit ("this variable might be uninitialized"). The two cannot
  double-report **by construction**: the prover THROWS on everything it
  proves, so anything reaching the warning pass is exactly what it could not.
  Warnings are **COLLECTED** into `g_warnings` (inferencer.h), not printed at
  the raise site: the compiler has no diagnostic stream, and a pass writing to
  stderr directly would be untestable and would interleave with the script's
  own output. The driver drains the list and renders each with the same caret
  machinery an error uses; `-rt` reads the vector instead. **A warning that
  cannot be located is barely a warning** — the test asserts every one carries
  a loc and a message. It is TRANSITIVE and resolves write-once aliases
  exactly like the error tier — the two share `build_reachable_reads` and
  `callee_of` and differ only in which paths count. The one-hop case in that
  table expected ZERO until the call-graph fixpoint landed, and the pin
  worked as intended: it FAILED the moment the behaviour improved, instead of
  letting the improvement go unnoticed. Keep writing them that way.
  **IT HAS TWO STRENGTHS, and the WORDING is the contract (#146,
  maintainer-set 2026-08-09).** `"this call MAY fail: g is not bound until
  later"` means the callee is KNOWN and provably reaches `g` on some path —
  only whether that path runs is open. `"this call MIGHT fail: the callee is
  not known here, and g is not bound until later"` covers the shapes no
  cheap analysis can bound — a container element, a parameter, a REASSIGNED
  name — and **is allowed to be a false positive**, which is exactly what
  "might" announces. Two things keep the weak arm from being noise, and both
  are sabotage-pinned: it fires only for a global that SOME function
  actually reads (an opaque callee cannot fail on a global nobody touches),
  and `call_is_opaque` excludes BUILTINS and STRUCT CONSTRUCTIONS — `print`
  is C++ and `P(1)` binds fields, so counting either as "a callee we cannot
  analyse" warns on nearly every program (removing that exclusion makes
  `runtime(0)` itself warn, watched). Measured over samples/ + bench/ +
  tests/functional/: **ZERO** warnings, while 76_funcval_dispatch — which
  has exactly this call shape — warns the moment one of its globals moves
  below the call. **A test of this tier must assert the STRENGTH, not just
  the count**: the two arms are both one warning, so a bare count lets a
  "might" satisfy a case that means the definite statement (the `needle`
  field in the `unbound_call_warnings` table).
- **⛔ NO INPUT MAY CRASH THE INTERPRETER (#137, 2026-08-09).** Every `.my`
  file ends in a DEFINED outcome — a compile refusal, a thrown exception, or a
  clean run. Two real crashes were found by simply trying degenerate inputs:
  an **EMPTY program** (also a comment-only file, or one the DCE empties like
  `if (false) { print(1); }`) had no `Frame`, and `EnterNative` formed
  `ctx.frame->slots` on the null pointer — UB, and SILENT in a release build
  because nothing reads it; and **~800 levels of nesting** blew the C stack of
  the recursive-descent parser (a SIGSEGV in release, an ASan DEADLYSIGNAL in
  debug). Fixed by refusing to JIT a frameless chunk (plus an `ML_VM_CHECK`
  stating the invariant at the one line that would form the bad address) and
  by `ParseContext::MAX_NEST`, a scoped depth cap in `pExpr01`, `pStmt` **and
  `pBlock`** — a bare `{ ... }` reaches `pBlock` from its own loop and never
  through `pStmt`, so the statement guard alone left `{{{...}}}` unbounded.
  **The oracle is the debug+ASan+UBSan lane; a release build hides all of
  this.** When you add a runtime path, ask what it does with a program that has
  no statements, no slots, or absurd depth.
- **⛔ ONE OP CAN NEED TWO CARETS, AND `locs` HOLDS ONE (#127, 2026-08-08).**
  A container store `g[0] = v` has two error spans: an OOB / key / type error
  carets the WHOLE lvalue (`g[0]`), an UNBOUND-GLOBAL base carets only `g` —
  because in the tree-walker they come from different nodes (the Subscript vs
  the base `Identifier::do_eval`). `Chunk::locs` is keyed by pc, so it can
  carry only one of them, and the VM reported the whole subscript for every
  store form; the CHAIN stores, whose per-step carets live in `chain_locs` and
  which therefore record NO `locs` entry, reported **no location at all** — a
  backtrace reading "line 0". The fix is a SECOND pc-keyed table,
  **`Chunk::base_locs`** (bytecode.h, `base_loc_at`), fed by
  `CgInstr::base_node_idx` which the emit sites set through
  `add_base_node(kind, base)` — a no-op for a non-global base, so the table
  stays sparse. **When you add a store-like op, give it a base caret**: the
  emit site has the base expression in hand from `as_container_base`, and it
  is one line. Full note: *docs/vm-ops.md*, `Chunk::base_locs`.
- **Context keywords**: `break`/`continue`/`return`/`rethrow` outside their
  valid context (gated by `pFlags` in `pStmt`) raise a clear `SyntaxErrorEx`
  ("... only allowed in a loop", etc.), not a generic "unexpected token".
- **Not-callable vs undefined**: a var used as a callee is excluded from
  auto-const promotion (`prescan_blocked` blocks `CallExpr::what`), so calling a
  defined non-function reports `NotCallableEx`, not a bogus "undefined var".
- **Uncaught user exceptions** print their throw-site loc + caret and their
  payload struct (`errfmt.cpp`'s `ExceptionObject` handler renders
  `get_data()` via `to_string`). The payload struct references its
  `StructTypeDef` (owned by the AST), so `main` (`mylang.cpp`) declares the
  `root` AST **outside** the `try` — unwinding must not free the def before the
  catch handler renders the value.
- **Backtrace frames are LAZY (2026-07-18 profile #3).** A captured
  `BacktraceFrame` holds `{const FuncDescriptor *desc, Loc call_site}` — NO
  strings; `format_backtrace` stringifies from the program-lifetime
  descriptor at RENDER time, so a CAUGHT exception (backtrace never shown)
  allocates nothing per frame (13% of the exception benches was that
  malloc churn). The eager string form survives for the INLINE (virtual)
  frames (their sources are AST-owned) and for a capture DURING CONST-EVAL
  (`do_func_call`'s catch passes `ctx->in_const_eval()`): a compile-time
  pure-fold can run a THROWAWAY clone whose descriptor dies with the fold
  while the early-failure rule propagates the exception — a lazy frame
  would dangle (ASan-caught during development). `vm_execute` (the
  harness/REPL entry) RETAINS its VmPrograms in a session-static list for
  the same reason: descriptors must outlive any exception's render.
  RuntimeException also carries **ML_POOL_NEW_DELETE** (profile #4) —
  inherited by every subclass, so the VM signal-path's make_unique/clone
  exception objects come from the pool (a C++ `throw X` value still uses
  the EH runtime's own allocation).

- **Backtrace.** `Exception::backtrace` (a `vector<BacktraceFrame>`, `errors.h`)
  is filled as the exception unwinds: `do_func_call`'s `catch (Exception &)`
  records each frame innermost-first, capturing the function's name+params **as
  strings** (the AST is torn down during unwinding, before the top-level handler
  runs; the name is `FuncDeclStmt::display_name` when set — a specialization
  clone's original name — else `id`) plus the *call site* (the `CallExpr`'s loc,
  passed as `do_func_call`'s
  `call_site`). `format_backtrace` (`backtrace.cpp` / `backtrace.h`) renders it:
  frame `[0]` is the innermost (its line = the error site, `loc_start`), each
  deeper frame's line is where it called the next, and a synthetic `main()` is
  the bottom. Two passes: pass 1 builds each `name(params)` (param list
  truncated to ~60 cols as `name(p1, ..., ...)`, the name never cut) and finds
  the widest; pass 2 zero-pads frame numbers to a common width (only when >9
  frames) and right-pads the name column so `at line N` aligns. It is a plain
  function so tests can format synthetic/real backtraces and assert on them.
- **Inlined (virtual) frames.** For function inlining, a node spliced from an
  inlined body carries an `InlineCtx` "inlined-at" chain
  (`Construct::inline_ctx`); `flush_inline_frames` (`backtrace.cpp`) appends one
  `BacktraceFrame` per chain element so the physically-absent inlined calls
  appear. It is flushed at two error-path points, both keyed off
  `Exception::inline_origin_emitted` (not the loc once-guard, which many errors
  pre-satisfy): `Construct::eval` at the innermost node (an error *inside*
  inlined code) emits the chain once and sets the flag, and `do_func_call`'s
  catch (a real call made *from* inlined code) flushes the call-site chain
  unconditionally and sets the flag so the enclosing `CallExpr` doesn't re-emit.
  `format_backtrace` is untouched. See `plans/archived/function-inlining.md`.
- **Tests** pin caret spans via the `test` struct's
  `ex_col`/`ex_line`/`ex_col_end`/`ex_line_end` (each checked only when nonzero;
  see the "err loc:" tests in `tests.cpp`); the "backtrace:" `extra_checks`
  cover the formatter (including synthetic inlined-frame reconstruction).

## The interactive REPL

`mylang` with no FILE/`-e` on a TTY (or `--repl` to force it off a TTY, for
testing) runs the REPL (`run_repl`, launched from `mylang.cpp`). Full design +
status: `plans/archived/repl.md`. Four TUs, split so the logic is
headless-testable behind a thin terminal shell:

- **`repl.{h,cpp}` — `ReplEngine`**, the headless evaluation core. Holds the
  persistent interpreter state: a persistent **const-eval `EvalContext`** + a
  persistent **runtime global `EvalContext`** (both roots, so they auto-load
  the builtins) + the **retained input ASTs** (`vector<unique_ptr<Construct>>`
  — never freed, so a prior pure func / struct / kept const stays valid and
  foldable for later inputs) + the ever-growing source `lines` (for error
  carets). `eval_input(src)` parses against the persistent const ctx with
  `pBlock(pc, 0, /*push_const_scope=*/false)` (so top-level consts/pure-funcs/
  structs survive — the parser drops a const *scalar* decl, but its *value*
  lives in the const ctx, which is what folding reads), then evaluates each
  top-level statement **directly in the persistent runtime ctx** (no fresh
  Block context/frame — that's why state persists and a redeclaration can
  rebind), capturing `print` output (via a `cout` rdbuf swap) and echoing the
  last value as `=> ...` (pretty-printed - see the value model). A `none` result
  is normally suppressed (a decl, `print`, `if`/loop, void call), **except** when
  the last statement is a plain VALUE LOOKUP - a bare variable, a member/
  subscript access, or the `none`/`null` literal (`repl_echo_none`) - so
  `nn`/`none` echo `=> <none>` while `print(x)`/`func f(){}` stay quiet. Errors
  are caught per input and the loop
  continues — **including a lexer error**: the lexer can throw
  (`InvalidTokenEx`, e.g. `2_`, or an unterminated single-line string), so every
  REPL lex site is guarded — `do_eval`'s lex runs inside the parse `try` (it
  reads the persistent `lines`, so the error's view stays valid for the caret),
  `is_incomplete` catches and reports the input *complete* (a bad token is a
  definitive error, not an input awaiting more — never let it escape the
  `Submitter`, which would crash the raw-mode editor), and the inspection
  meta-commands lex inside their own `try`. `InvalidTokenEx` now carries a `Loc`
  so it renders a caret like every other error. The whole-program optimizers
  (`resolve_names`/inliner/`infer_types`/`specialize_types`) are **not** run per
  input yet (see "deferred" below), so top-level names are map-based globals.
  - **Inputs auto-terminate** (`repl_auto_terminate`): you don't type `;` at a
    prompt. Per physical line a `;` is appended unless the line is empty/
    comment-only, ends inside an unclosed `(`/`[`, or ends on a continuation
    token — and a `{` is classified as a statement **block** vs a dict
    **literal** by whether the previous token expected a value, so a multi-line
    func/if body gets interior `;` but a multi-line dict does not.
    **Multi-line-string/comment aware:** `repl_scan_line` tracks an
    `in_string`/`in_comment` state across physical lines and returns each line's
    `[code_begin, code_end)` span (the part NOT inside a string/comment); a line
    wholly inside one is passed through verbatim with no `;`, and only the code
    span is lexed for the bracket/continuation logic (so it can never hit an
    unterminated token). A multi-line string that **closes** at the start of a
    line is itself a value, so the statement can be terminated there even with
    no following code token (`closed_string`); a line that **opens** one that
    continues gets no `;` (`open_at_end`).
  - **`EvalContext::allow_redeclare`** (set only on the REPL global scope) makes
    a re-declaration rebind the global instead of throwing `AlreadyDefinedEx`
    (the script rule is unchanged — the resolver still catches same-scope dups
    at compile time). **`pBlock`'s `push_const_scope`** flag (default true) is
    the const-ctx analogue. **Gotcha:** the lexer stores `string_view`s into the
    source lines, so all lines of a multi-line input are appended to `lines`
    *first*, then lexed from the stable buffers (growing-while-lexing dangles
    earlier tokens — an ASan-caught UAF).
  - **Meta-commands** (`eval_input` dispatches a leading `:`): `:tree <code>`
    (parse + serialize, non-committing — parsed with `push_const_scope=true` so
    prior consts fold but new decls land in the popped scope), `:analyze
    <code>` (the colored optimization view), `:source <file>` (split into
    complete units via `is_incomplete` and replay each through
    `do_eval(echo=false)`), `:help [topic]` (the documentation system,
    `replhelp.cpp`), `:trace [<cat>...] on|off` (toggle the diagnostic
    tracer), `:globals` (a table of every global — vars/consts/funcs/structs —
    with its inferred/declared type, merging the runtime scope with the const
    context so folded const SCALARS still appear), `:type <expr>` (a committed
    global's inferred static type via `ReplInfer::global_type`, else the
    runtime structural type of the expression evaluated in a throwaway child
    scope), `:show <function>` (the optimized-AST decompiler, `coderender.cpp`
    — renders the function and its `<name>$N` clones as synthetic code),
    `:quit`.
  - **`completions(buf, cursor)`** — Tab candidates: keywords + builtins + REPL
    globals (`EvalContext::collect_symbols`), or a struct value/type's fields/
    consts right after `base.`.
- **`lineedit.{h,cpp}` — the hand-rolled, IRB-style multi-line editor.** A
  **pure `LineEditor`** core driven one byte at a time via `feed()`: the edit
  buffer **holds embedded newlines** (a multi-line block) and the cursor is an
  offset with a 2-D view (`cursor_row`/`cursor_col`). **Enter SUBMITS only when
  a `Submitter` callback says the buffer parses complete**, else inserts a
  newline + auto-indents by bracket depth (`newline()`); **UP/DOWN move within
  the block** (same column, prev/next line via `move_up`/`move_down`) and fall
  through to history only at the first/last line; **Home/End and the kill keys
  are line-relative** (`line_start`/`line_end` — including the `ESC[1~`/`ESC[4~`
  variants). With no `Submitter` set it is the old single-line behavior, so the
  feed-based unit tests are unchanged. It emits no output and touches no fd, so
  it is unit-tested with raw byte scripts (assert `buffer()`/`cursor_row()`/
  `cursor_col()`). `read_line` is the only non-pure part: termios raw mode via
  an RAII guard, byte-at-a-time read, and the **2-D repaint** (each logical line
  under its `>>`/`..` prompt, the cursor positioned in two dimensions, the prior
  block cleared) — it assumes each logical line fits the terminal width (no
  soft-wrap). Off a TTY it accumulates physical lines until complete. History
  stores whole logical inputs, so UP recalls an entire block.
  - **Inline autosuggestion (PowerShell-style ghost text).** A `Suggester`
    callback (`std::function<string(const string&)>`) returns a full suggested
    line for the current buffer; `LineEditor::suggestion()` returns the un-typed
    **remainder** — but only when the cursor is at the end of a **single-line**
    buffer that the suggestion strictly extends (so the 2-D renderer never has
    to draw a multi-line ghost, mirroring PowerShell hiding the prediction once
    the cursor leaves the line end). `read_line` renders that remainder in dim
    gray (`\033[90m…\033[0m`) just past the cursor and repositions the cursor to
    its start; **Right-arrow / `Ctrl-F` at the line end accept it**
    (`accept_suggestion`: append the remainder; returns false otherwise so the
    key falls back to moving the cursor right). `read_line` wires the suggester
    to the **completer** (the same source as Tab: current variables, builtins,
    keywords - NOT history; history is `Ctrl-R`'s job): it completes the
    identifier ending the buffer with the shortest matching candidate's
    remainder. Enabled only when **color is on** (the ghost must be visually
    distinct; `NO_COLOR`/no-TTY get none). An un-accepted ghost is **erased on
    leaving the line** (`move_below` emits a clear-to-EOL when `suggestion()` is
    non-empty - which is only ever a single-line, cursor-at-end buffer, so it
    hits exactly the ghost), else the committed line would show it (`ar` + dim
    `ray` reading as `array`). The
    pure core is unit-tested with a synthetic suggester (`suggestion()` +
    accept); the gray rendering lives in `read_line` (untested, like the rest of
    the TTY shell). A navigable dropdown completion menu is still deferred (see
    `plans/archived/repl.md`).
  - **Reverse history search (`Ctrl-R`).** `class HistorySearch` (lineedit.h) is
    the **pure** analogue of `LineEditor` for searching: a query + a ranked,
    de-duplicated match list + a selected index, driven by `feed()` one byte at
    a time (Up/Down or Ctrl-P/N move the selection, Ctrl-R cycles to the next
    match, Enter → `accept`, Ctrl-G/Ctrl-C → `cancel`, Backspace/printable edit
    the query). Ranking is `fuzzy_score(query_lc, cand)` — a case-insensitive
    **subsequence** match scored by contiguity (gap-0 runs win big), word-
    boundary/camelCase hits, and a length tie-break; `INT_MIN` == no match; an
    empty query matches all with score 0, ordered by **recency** (a
    `stable_sort` over the newest-first deduped list). `read_line` reads the raw
    `Ctrl-R` byte (the editor never sees it) and renders a **bordered pane ~⅓
    the screen high** below the input: a rounded box whose **top edge is the
    search box** (`search: <query>` + an `N matches` count), over the live
    result rows best-first, the selected one a full-width reverse-video bar and
    the **matched query letters bolded** in every row (`fuzzy_match_positions`,
    the same greedy scan as `fuzzy_score`). The border is rounded UTF-8
    box-drawing when the locale is UTF-8 (`unicode_ok`), an ASCII fallback
    (`+ - |`) otherwise — emitted as explicit UTF-8 *byte escapes* so the source
    stays pure ASCII; each glyph is one display column, so the row-width math is
    in columns, not bytes. A lone `Esc` (distinguished from an arrow burst via a
    `byte_ready` select-timeout) cancels. Geometry is scroll-safe (reserve lines
    by printing newlines then moving back up; `term_size` via `TIOCGWINSZ`); on
    exit the pane is erased and the cursor returns to the input's first row.
    **Enter LOADS** the selected command into the editor (it is not auto-run).
    The scorer, match-position, and state machine are headless-tested
    (`histsearch:`); the pane rendering is in `read_line` (untested TTY shell,
    verified over a pty).
  - **Bracketed paste.** `RawMode` enables it (`ESC[?2004h`, off on exit), so
    the terminal wraps a paste in `ESC[200~ .. ESC[201~`. `LineEditor::feed`
    recognizes the start marker (in `csi_final`, `esc_params == "200"`) and then
    **swallows bytes verbatim** into `paste_buf` until the end marker - never
    interpreting them as keystrokes (a pasted newline doesn't submit, a Tab
    doesn't complete). On the end marker it calls `apply_paste`, which inserts
    the block as inert text **re-indented to the editor's brace-depth style**
    (`indent_depth`, shared with `newline()`): each line's own leading
    whitespace is dropped and replaced by 2-spaces-per-level, a line opening
    with a closing bracket dedents, and the first line is kept verbatim only
    when it continues a non-empty line (`cursor_col() != 0`). Safe because
    MyLang whitespace is purely cosmetic. `pasting()` lets `read_line` skip the
    Ctrl-R interception and per-byte repaints mid-paste; the normal `repaint`
    then re-renders the block syntax-highlighted. The re-indent + insert is
    headless-tested (`lineedit:` bracketed-paste cases); the mode toggle is in
    `read_line`.
- **`highlight.{h,cpp}`** — `highlight_line`, a self-contained scanner (NOT the
  lexer; tolerates mid-edit input) that wraps keywords/strings/numbers/comments/
  type-words in ANSI color, preserving the bytes exactly otherwise. The two-arg
  `highlight_line(src, int &state)` threads an `HlState` (`HL_NONE`/`HL_STRING`/
  `HL_COMMENT`) across lines, so a string or `/* */` block comment is colored
  **across rows**; `read_line`'s repaint carries that state row to row (the
  one-arg form is the stateless wrapper for isolated lines). The `read_line`
  highlight callback type is `string (*)(const string &, int &)` accordingly.
- **`errfmt.{h,cpp}`** — `format_exception`/`dump_loc_in_error`, the
  per-exception-type caret/backtrace rendering, factored out of `mylang.cpp`
  (parameterized by an `ostream` + the source `lines`) so the file driver and
  the REPL share it.
- **`replhelp.{h,cpp}`** — `repl_help(topic, color)`, the `:help`
  documentation system: a self-contained STATIC database (no interpreter
  state) of every builtin (`{name, category, signature, summary, long?}`) and
  of the language features — surface (values, vars, control, functions,
  arrays, dicts, strings, structs, exceptions, the type system) *and* the
  optimization passes (const-fold, auto-const/pure, inlining, specialization,
  template instantiation, M8, flat arrays, CSE, COW), each with a syntax
  sketch + prose and a pointer to its `:trace` category. `:help` overview,
  `:help builtins[/<cat>]`, `:help <builtin>`, `:help language`,
  `:help <category>`, `:help <feature>` all route through `repl_help`, plus a
  **commands DB** (`:help commands`, `:help <command>`) documenting the REPL
  meta-commands themselves. A leading `:` is stripped and means "this is a
  command" (`:help :trace`); a bare topic resolves builtin → command →
  language-category → feature, and when a builtin and a command share a name
  (`trace`/`type`/`globals`) the builtin entry shows with a pointer to the
  command. Feature ids are kept distinct from category ids. The trace category
  list (names + descriptions) has one source of truth, `trace_categories_help()`
  (`trace.cpp`), rendered as an aligned bullet list by `:trace help` and the
  `trace`/`:trace` entries alike. Pure/headless, so it is unit-tested (`replhelp:`
  extra_checks). See `plans/archived/repl-introspection.md`.
- **`coderender.{h,cpp}`** — `render_func_code(fn)` /
  `render_construct_code(c)`, the optimized-AST **"decompiler"**: it unparses
  the FINAL tree (after parse/fold/inference/`resolve_names`/`specialize_types`)
  back into synthetic MyLang-like code, so you see what actually runs — dead
  code gone, folded consts as literals, inlined call bodies spliced in
  (annotated `inlined f`), flat-array element types as `array<int>`,
  typed-scalar/annotation hints as the var's type. A precedence-aware
  expression printer + statement walker, with a comment fallback for any
  unhandled node (best-effort, NOT round-trippable). Backs the `show(f)` builtin
  (a **dev-only** builtin — see the reflection-builtins note below; reserved in
  scripts) and the REPL `:show <name>` (which also renders the `<name>$N`
  clones). It
  reads literal values via the public `ival()`/`fval()`/`bval()`/`strval()`/
  `literal_value()` accessors on the `Literal*` nodes. `render_func_code` takes
  an optional per-param inferred-type list: `:show` passes
  `ReplInfer::func_param_types` (the instance's `FuncInfo::params` types,
  empty for an un-instantiated template), so a template instance renders
  `int func dot$0(int x, int y)` (param **and** return types, via
  `func_param_types`/`func_return_type`) while the base shows untyped
  `func dot(x, y)`; the `show()` builtin (no persistent inferencer) renders
  AST-hint types only. `show()` / `:show` also accept an **expression** (not a
  function): `render_construct_code` renders its optimized tree
  (`:show 2 + 3 * 4` → `14`; `:show <expr>` parses + `resolve_names`'s the arg
  non-committing). `:show` output is **syntax-highlighted** via `highlight_line`
  (extended to color C-style block comments and treat `$` as an id char — the
  `f$0` names). See `plans/archived/repl-introspection.md`.
- **`trace.{h,cpp}`** — the **diagnostic tracer** ("MyLang's mind"): a per-
  category bitmask (`TraceCat`: infer/inline/specialize/template/autoconst/
  autopure/arrays/fold) in `g_trace_mask`, the hot guard `trace_enabled(c)`,
  and `trace_emit(c, indent, msg)` to a swappable sink (default `&std::cerr`).
  The `TRACE(cat, indent, msg)` macro builds `msg` ONLY when the category is on
  — so the guarded emits sprinkled at optimizer decision points (inferencer
  `commit_round`/finalize, and — Pillar 3b — the resolver's inliner /
  auto-const / auto-pure / specializer, the array-hint and fold sites) cost
  one mask test when off. Control surface (both, builtins-first): the
  `trace()`/`traceoff()`/`tracing()` builtins (`builtins/reflect.cpp.h`) and
  the REPL `:trace [<cat>...] on|off` meta-command. The REPL points the sink at
  its per-input capture stream (a `TraceSinkGuard` in `do_eval`) so an enabled
  trace narrates into the REPL output just above the result (and is testable);
  a script leaves it at `cerr` so trace never corrupts stdout. OFF by default,
  so scripts/tests are unaffected. See `plans/archived/repl-introspection.md`.

`run_repl` (in `repl.cpp`) drives it: history loaded/saved to
`~/.mylang_history`, colors gated on a TTY + `NO_COLOR` (passed into the
engine via `ReplEngine::set_color`, since the engine is headless and the
meta-commands `:help`/`:analyze`/`:show` would otherwise auto-detect stdout
and bake ANSI escapes into the returned string — breaking `-rt`'s substring
matches when stdout itself happens to be a TTY), Ctrl-C drops the
current input, Ctrl-D at the prompt exits. The editor owns multi-line
continuation (its `Submitter` wraps `ReplEngine::is_incomplete`, treating a
leading-`:` meta-command as always complete), so `run_repl` reads one whole
logical input per `read_line` — no per-line accumulation loop. **A function or
struct can be REDEFINED at the prompt** (the edit-and-resubmit workflow):
`EvalContext::allow_redeclare` is set on *both* the runtime and const scopes
(structs/pure funcs register in the const ctx at parse time), and the
`FuncDeclStmt`/`StructDeclStmt` eval paths erase-then-rebind under it instead of
throwing `AlreadyDefinedEx`. A plain `var`'s TYPE still sticks (the inferencer's
job — the type-commitment above; `:undef` resets it). **Redefining a function
GCs its now-orphaned template/spec instances** (`gc_redefined_instances` in
`do_eval`): an instance (`f$0`) created only by a throwaway top-level call
(`f(1,2)` at the prompt) is removed from both scopes + the inferencer when its
base `f` is redefined, so `globals()`/`specializations(f)`/`:show f` stop
showing it; an instance still **consumed by a function body** is kept (else
calling that function would break). "Consumed" is tracked soundly in the
inferencer: `collect_calls` carries an `in_func` flag (a call below a
`FuncDeclStmt` in the complete `for_each_child` walk), and `instantiate_round`
sets `FuncInfo::has_func_consumer` when it redirects an in-function call to the
instance; `ReplInfer::instance_has_consumer` exposes it. Only instances present
*before* the input (so not the new ones it just created) and whose base name
this input redefined are candidates.

**Faithful per-input pipeline.** `do_eval` runs the REAL pipeline on each input
(after parse): `ReplInfer::check_input` (type inference + checking) →
`resolve_names(..., repl_mode=true)` → `specialize_types` → eval. So the REPL is
the true interpreter — flat arrays, M8 specialization, inlining, slotted nested
locals all happen and are inspectable (`:analyze`).
- **`ReplInfer`** (`inferencer.{h,cpp}`) is a persistent `Inferencer`: the
  one-shot `run()` is factored into `setup()` (once) + `infer_one(root)`
  (per root), and `infer_input(root)` runs `infer_one` then marks this input's
  new `TypeSym`s/`FuncInfo`s **`pinned`**. A `pinned` symbol is a committed
  global: the fixpoint (`reset_round`/`commit_round`/finalize/the `enforce_*`
  passes) **skips** it, and `contribute()` instead **checks** an assignment is
  assignable to its committed type — the cross-input **type commitment** (`var
  x = 3` then a later `x = "hello"` is a `TypeMismatchEx`, "has type" for an
  inferred commit vs "is declared" for an annotated one). All `pinned` branches
  are no-ops in the one-shot path, so scripts/tests are byte-identical. A
  rejected input rolls back (`infer_input` restores the global scope + pins the
  half-built syms). **An uninitialized `var a;` commits as `dyn?`, not `none`**
  (`pin_new`): a script infers a plain `var`'s type from later uses
  (`var a; a = 3` → `int?`; a conflicting `a = "x"` is an error), but the REPL
  commits each input before seeing the next and so cannot defer — a `none`
  commitment would reject *every* later assignment (`a = 1`), so an
  unconstrained, un-annotated, non-`dyn`/non-param var is committed `opt dyn`
  (it accepts any future assignment). This is a deliberate REPL-vs-script
  divergence (the REPL can't see the future). The **`:undef <name>`**
  meta-command
  (`Impl::cmd_undef`) erases a global from the runtime + const scopes and calls
  `ReplInfer::undef_global` to drop its committed type, so a later `var x` of a
  new type is fresh. REPL redeclaration is **not** a feature: a re-declared
  global hits the type-commitment check, and `:undef` is the way to change a
  global's type. (There is no `undef` *builtin* — a script's symbols are fixed
  slots at compile time, so `undef` is a REPL-only convenience; a script just
  re-defines a name.)
- **`resolve_names`'s `repl_mode`** keeps EVERY top-level decl in the map as a
  persistent global (never slotted into "main", never auto-const-promoted — the
  open-world soundness point); nested function locals slot/inline/specialize
  normally. `eval` runs the input's elems directly in the persistent global
  scope, so globals persist and nested calls build their own frames.
- **`render_analysis`/`anno_code`** moved from `mylang.cpp` to `analyzer.cpp`
  (an `ostream` param) so `-a` and `:analyze` share one renderer.

**Tests:** all headless. The **`repl:`** tests drive ONE `ReplEngine` through a
sequence of `(input, expected-substring)` steps, so the persisted global scope
AND the cross-input type commitment / `:undef` reset / per-input optimizers are
exercised; the **`lineedit:`** / **`highlight:`** `extra_checks` feed byte
scripts / strings to the pure cores. Only `read_line`'s few syscalls are not
unit-tested. **Not built:** an IRB-style dropdown completion menu, and the
Windows raw-input backend (the editor is termios-only; on Windows the REPL
falls back to line-at-a-time input).

## Testing an AST TRANSFORM (the differential does NOT cover it)

> **⛔ THE TREE-WALKER-vs-VM DIFFERENTIAL CANNOT VALIDATE AN AST
> TRANSFORM.** A transform that rewrites the tree (LICM, the slice hoist,
> the counted-loop rewrite, M8, inlining, auto-const, DCE) runs BEFORE
> either engine sees the tree, so both engines faithfully execute the same
> WRONG tree and agree perfectly. The project's main correctness net is
> blind here by construction. This is not hypothetical: subscript LICM
> shipped with a real bug in its first version and the 1481-test
> differential was fully green on it.

**The only oracle is the same program with the transform OFF.** Hence the
per-pass kill switches (`OptPass` / `g_opt_disabled`, inferencer.h; CLI
`--no-opt licm,slice-hoist,for-range,typed,all`) - the same-binary A/B
lever the JIT has as `-nj`, one layer up. Three nets use them, and a new
AST transform joins **all three** on the day it is written:

1. **`opt_layer_equivalence` (`-rt`)** - a corpus run through EVERY
   single-pass-off configuration, plus the all-off one, on BOTH engines,
   requiring identical stdout AND identical exception behaviour. A
   divergence names the pass and the layer. Give a new transform a bit in
   `OptPass`, a case in the corpus, and cases for whatever its gates
   REFUSE (a refusal that silently stops refusing is the dangerous
   direction).
2. **`tests/nested_fuzz.py`** - its `noopt` engine (on by default) re-runs
   each random deep-nested program with every transform disabled, on both
   engines. Random programs hit gate COMBINATIONS no hand-written case
   covers.
3. **A shape test** (`hoist_subscript_shapes` / `hoist_slice_shapes`) -
   asserting the transform FIRED where it should and did NOT where it
   must not. Equivalence alone is satisfied by a transform that never
   fires.

> **⛔ AND PROVE THE TEST CATCHES THE BUG.** Write the test, then REINTRODUCE
> the defect and confirm it FAILS. The first version of the layer-equivalence
> corpus PASSED with the LICM bug put back - its case used a scalar element,
> which the pass skips, so it exercised nothing. A test for a bug you have
> not watched it catch is decoration. (Same rule as "prove the code ran" for
> a perf measurement, applied to correctness.)

> **⛔ THE VACUOUS-TEST TRAP: THE OPTIMIZER EATS YOUR TEST SHAPE.** The
> reintroduce-the-defect proof above keeps catching the SAME failure: a
> test whose shape never REACHES the code under test, because an earlier
> pass transformed it away - the test then passes with the defect in,
> vacuously. One session (2026-08-02) hit this FIVE times. Before writing
> any test of a VM/JIT/codegen path, defeat the known shape-eaters UP
> FRONT instead of rediscovering them one reintroduction at a time:
>
> 1. **A tail call `return f(x)` never reaches codegen as a call** - the
>    AST tail-inliner splices it first. Use `var r = f(x); return r;`.
> 2. **Const-folding deletes const-arg constructs at PARSE time.** A
>    literal-bounds slice of a const (`C[1:4]`) folds to a baked literal -
>    no live runtime slice exists; a 0-arg pure call folds away entirely
>    (no args == const args). Defeat with `runtime()` in an operand.
> 3. **A store/read must sit INSIDE a JIT-compiled run** to exercise an
>    emitted tier - a short literal array's single store never compiles.
>    Fill the array in a loop first, then exercise the shape.
> 4. **The "same" source can lower to a DIFFERENT op**: `a[i] = true` is
>    StoreElemInt, `a[i] = i < n` is StoreElemValue; `M[k][j]` fuses to
>    LoadElem2Int only when the pcs are adjacent, and LICM hoists the row
>    unless the outer index varies with the inner loop. Check `-vd` for
>    the opcode the test actually produces.
> 5. **Attribute counters PER SHAPE, never program-wide** - a decline
>    case's ordinary stores otherwise mask (or fake) the signal. And an
>    env-gated tool toggled by one test leaks into the next: save/restore.
> 6. **Const-ARG specialization eats "runtime" values derived from a
>    param.** `f(9)` specializes to `f$s0` with `n = 9` propagated, and
>    auto-const then folds a write-once local (`var z = n - n`) to a
>    LITERAL operand - so a test of a runtime-slot guard (a div-by-zero
>    divisor) silently exercises the emit-time literal path instead;
>    three divisor-guard cases were vacuous exactly this way. Defeat by
>    making the local write-TWICE (`var z = 1; z = n - n;` - blocked
>    from promotion), or by calling with a non-const argument.
> 7. **SHRINKING A BENCH TO PROBE IT CHANGES WHICH OPTIMIZATION
>    APPLIES.** 64_struct_create with `500000 * scale` replaced by a
>    literal `300` lowers its loop to the TOP-TESTED form, which C4e
>    refuses - so the probe showed H1 guards in `-vdj` that the real
>    bench does not have (a runtime bound gives the counted `for.step`
>    form, where C4e fires). A whole mechanism was nearly built for
>    that phantom. Keep the bound RUNTIME when probing a loop, and
>    diff `-vd` between the probe and the original before believing
>    the probe.
> 8. **A CALL is not a call by the time codegen sees it.** Testing what
>    an op does to a slot's DATAFLOW (a kill, a barrier, a liveness
>    edge) with `p = mk(i)` exercises nothing: a small callee is
>    inlined by the AST inliner or spliced by the bytecode inliner, and
>    the reads afterwards then read the INLINED BODY's own slot, not
>    the one the call would have written. C4d's KILL sabotage passed
>    against exactly this - 4 emitted guards either way. Use a shape
>    that survives to codegen (a plain `p = q` MoveV took the same
>    sabotage from 4 guards to 2), or check `-vd` for the op the test
>    actually produces.
>
> The mechanical safeguards, both mandatory: an **EMITTED-code counter**
> (the `g_jit_store_fast` pattern - the helper's counter also counts
> declines, so it cannot prove the fast path ran), and the
> reintroduce-the-defect run, which is what catches whatever this list
> does not yet name. When it does, ADD THE NEW EATER HERE.

**A caret/backtrace check needs the same treatment**: an AST transform can
move a `Loc` without changing any value, so compare the rendered error
POSITION across layers too, not just the exception type.

## ⛔ THE TWO HARD LANGUAGE RULES (maintainer-set, 2026-08-08)

These are not guidelines. A change that violates either is wrong, however
much it wins on a benchmark.

### RULE 1 — THERE IS NO UNDEFINED BEHAVIOR IN MyLang. EVER.

Every program has a DEFINED outcome, decided either at **compile time**
(a refusal) or at **run time** (a specified value, or a thrown
exception). There is no third category. C and C++ answer "read an
uninitialized variable" with UB — an unpredictable value, no diagnostic;
that is explicitly one of the things this language exists to not do.

Concretely, when you find a construct with no defined answer, the fix is
ALWAYS one of:
 - reject it at compile time, or
 - give it a specified value, or
 - throw a specified exception at run time.

Never: "it happens to read 0", "it depends on the allocator", "the slot
holds whatever was there". A `none` sitting in a slot that inference
proved is an `int` is UB by this rule even though no memory is unsafe —
it was found printing `1` under the JIT, aborting `ML_VM_CHECK` under
`-nj`, and throwing `TypeErrorEx` in the tree-walker (see the brace-less
body note under *Invariants & hazards*).

### RULE 2 — AN OPTIMIZATION MAY NEVER CHANGE OBSERVABLE BEHAVIOR

`--no-opt all` + `-nc` + `-tw` at one end, and the full VM + JIT + every
transform at the other, must be **observably identical in every
respect** — printed output, the exception raised, the message, the
**caret span**, the **backtrace**, and the exit code. The ONLY thing an
optimization may change is **how long the script takes**.

This is stronger than "the tests agree", and it is the reason the
engine differential exists. It applies to:
 - the AST transforms (const-fold, auto-const/pure, inline, specialize,
   DCE, LICM, for-range, M8) — see *Testing an AST TRANSFORM*, whose
   `--no-opt` kill switches are the oracle for exactly this rule;
 - the engines (tree-walker vs VM vs VM+JIT);
 - the stored image (`.myv` must render errors identically to source).

Two violations of this rule have already been fixed and are the canonical
examples: **const-eval changed SCOPING** (`if (1) func g() {...} g();`
worked by default and threw under `-nc`), and **const-fold ignored a
name binding** (`func abs(x) { return 42; } print(abs(-1), abs(runtime(-1)));`
printed `1 42` — the same call resolving to two different functions).
Both were "the parse-time const evaluator has its own, wrong, notion of
what a name means". **Expect more of that shape and look for it.**

A caret or a backtrace that differs between engines is a RULE 2
violation, not a cosmetic issue.

## Optimizations must generalize (the bar is a compiler, not an example)

The compile-time optimizations (const-fold, auto-const, auto-pure, inline,
specialize, DCE, short-circuit) exist to do for MyLang what a `-O3` compiler
does: **everything knowable at compile time should fold.** A recurring failure
mode here was an optimization that passed its one hand-written test but did
**not** generalize — it worked "pro forma." Several shipped silently broken
(auto-pure that didn't propagate through a call so a 2-deep pure chain didn't
fold; const-fold / inline / specialize that only saw the *current* REPL input so
a call to a prior-input function never folded; a const-false `if` with no `else`
that left a NULL statement and broke the *next* line; no short-circuit folding at
all, so a `const FLAG=false; if (FLAG && …)` guard was never eliminated). Each
was found by a user in minutes of REPL play, not by the suite. So, when you add
or touch ANY optimization, a passing example is the START of the test set, not
the proof. Before calling it done:

- **Test CROSS-INPUT, not just single-compilation.** `check()` (the whole `-rt`
  suite) joins all of a test's source lines into **one** compilation, so it
  *structurally cannot* catch a pass that only sees the current input — yet the
  REPL compiles each input separately and is where users actually hit this. An
  optimizer that doesn't bridge inputs (the `prior_pure` / `prior_scope`
  plumbing on `resolve_names`/`AutoConst`/`Inliner`) regresses there invisibly.
  Add a **`repl:`** test that defines a helper in one input and exercises the
  optimization from a LATER input (`:show` the result). This is the single
  highest-value rule: every cross-input gap above was invisible to single-
  compilation tests by construction.
- **Test COMPOSITION and TRANSITIVITY, not one shape.** A rewrite that removes
  or replaces a node must be tested *followed by another statement*, as the last
  statement, inside a function body, nested, and **chained** (`g`→`f`→`h`,
  partial-const, a side-effecting operand). The bugs live at the seams: a
  folded-away statement that returns NULL breaks the next one; auto-pure that
  doesn't cross a call breaks the chain; an expression-bodied function body that
  a fold pass skips never folds. One example proves nothing about the seam.
- **Use C++ `-O3` as an ORACLE** where the comparison is meaningful (const-prop,
  folding, inlining, specialization, DCE, short-circuit — not machine-level
  codegen). Write the equivalent C++, `g++ -O3 -S`, and confirm MyLang folds
  what the compiler folds — or document *precisely* why not. The legitimate
  "why not"s are dynamic-typing soundness (`x+0`≠`x` since `x` may be a string;
  `false||5`≠`5` since `||` yields bool) and deliberately-out-of-scope passes
  (general algebraic simplification, loop unrolling, recursion folding). "It
  folds my example" is not the bar; "it folds what a compiler folds, or there's
  a written reason it can't" is.
- **State the PROPERTY, then test the property.** "A pure call with const args
  folds to a literal" is a property — so test a chain, a partial-const, a
  cross-input, a nested, and a side-effecting-operand case. If only the example
  works, the feature is unfinished: either generalize it or pin the limitation
  with a test that documents the current (limited) behavior, so the gap is
  *visible* and intentional rather than a latent surprise.

## Execution strategy: strip compile-time overhead first, THEN a bytecode VM

> ## ⛔ THE ABSOLUTE, NON-NEGOTIABLE RULE — ZERO AST AT RUNTIME ⛔
>
> **AFTER COMPILATION IS DONE, A SCRIPT RUNS WITH *ZERO* AST NODES. NONE.**
>
> This is the WHOLE POINT of the bytecode VM. Read it ten times:
>
> 1. **After codegen, there is NO `Construct*` reachable from the runtime
>    image.** Not in `Instr`, not in a pool, not in a side table, not behind an
>    index, not behind a `node_idx`, not in a `node_table`, not ANYWHERE.
> 2. **Every `Construct*` object is FREED after compilation.** The whole AST is
>    droppable. If a chunk pins even one `Construct*`, the goal is NOT met.
> 3. **A native op must NEVER call `node->eval(...)` at runtime — and none
>    CAN: the fallback ops are ALL DELETED** (`EvalStmt`, `EvalToSlot`,
>    `JumpIfFalse` — the opcodes no longer exist). The codegen is NO-FAIL: a
>    statement none of the compilers accept THROWS `NotLoweredEx` at compile
>    time (`throw_not_lowered`, always-on, release included) — a compiled
>    chunk structurally cannot re-enter the AST, and a future lowering gap
>    is a loud compile refusal, never a silent tree-walk.
> 4. **All information an op needs at runtime is extracted into POOLED,
>    SERIALIZABLE, AST-FREE data DURING compilation** — source `Loc`s in the loc
>    side table, member keys / catch types / literals / struct defs / arg carets
>    in their pools, etc. Pooled data is plain values (ints, strings, `Loc`s,
>    interned `UniqueId*`), NEVER a `Construct*`.
> 5. **A "loc-keyed" or "pc-keyed" table of `Construct*` is STILL A VIOLATION.**
>    (The former `Chunk::node_table` and the `ast_nodes` pool are DELETED —
>    `Chunk` holds NO `Construct*`-typed member AT ALL (`closure_defs` holds
>    `FuncDescriptor*`); `verify_ast_free` asserts every `Instr`'s
>    codegen-transient `node_idx` was nulled when codegen finishes.)
> 6. **During compilation you MAY hold `Construct*`** — e.g. an
>    `unordered_map<const Instr*, const Construct*>` (or the current `node_idx`
>    handle) to associate an op with its node before its final pc is known. That
>    is fine, and often simpler than indices. **But it is a COMPILE-TIME-ONLY
>    scratch structure; it is destroyed when codegen finishes.** The finished
>    chunk carries none of it.
> 7. **The ONLY exceptions are the REPL and the `-rt` test harness** (they
>    retain the AST for their own reasons — decompilation, differential
>    testing). A plain script run (`mylang file.my`, `-vm`, `.myv`) has ZERO
>    AST at runtime.
> 8. **This is what makes the `.myv` file possible** (`mylang -c file.my` →
>    binary, run with no AST — SHIPPED, see the format section). It would be
>    IMPOSSIBLE while any `Construct*` survived codegen. So this rule is not
>    aspirational polish — it is the load-bearing invariant the whole VM
>    exists to satisfy.
> 9. **When you find a construct that isn't natively lowered, NATIVIZE IT.** Do
>    not add a node-holding op. Do not relocate the node. Extract its loc/data
>    to a pool and emit real bytecode. (Since the no-fail contract landed, an
>    unlowered construct is a NotLoweredEx compile abort, so a gap cannot hide
>    — fix it by lowering, never by re-adding a fallback.)
> 10. **Say it once more: after compilation, NO AST NODES, NO `Construct*`, NO
>     pointers, NO indexes, NO tables — the AST is FREED and the script runs on
>     bytecode + pooled data alone.**
>
> **THE `.myv` STORED-BYTECODE FORMAT (`serialize.{h,cpp}`; the byte-level
SPEC is docs/myv-format.txt — read it before touching the format; design +
phases in plans/archived/myv-serializer.md) — THE ENDGAME ARTIFACT.**
`mylang -c file.my [-o out.myv] [--strip-source]` runs the full pipeline
(run-side flags: `--source ROOT`, `-f`/`--force` - see the SOURCE
REFERENCE below)
(parse → infer → optimize → `vm_compile(root, jit=false)`) and writes the
VM image; `mylang file.myv` runs it with NO lexer, NO parser, NO optimizer
— the file argument is detected by CONTENT (`myv_is_image`, the "MYLV"
magic), never by extension. `-vd file.myv` disassembles a LOADED image via
**`disassemble_image`** (disasm.cpp), the loaded twin of
`disassemble_program` — the ROUND-TRIP ORACLE.
**ONLY the VM image is stored, never native code:** the JIT rewrites code
IN PLACE (inserting `EnterNative`, deleting originals) against fragments
that cannot be relocated, so `-c` compiles with `jit=false` (the new
`vm_compile(root, jit)` / `vm_precompile_all(root, jit)` parameter) and the
LOADER re-runs the tier — **`vm_jit_loaded_image`** (vm.cpp) mirrors
`vm_precompile_all`'s TWO PASSES: recompute every chunk's `native_leaf`
(derived from the ops - `jit_chunk_is_native_leaf`, so it is never stored),
then jit each body with a `JitCtx` REBUILT from the image (global slot →
descriptor resolved BY NAME through `global_func_names`, plus the newly
serialized `VmProgram::global_slot_reassigned`), then main. That is what
makes a loaded image's `-vd` byte-identical to a fresh compile's.
**The format** (all little-endian, fixed records, NO compression - the
no-deps rule): magic + `MYV_FORMAT_VERSION` (exact match) + an endian mark
+ a **BUILTIN-SET FINGERPRINT** + the **SOURCE REFERENCE** + the string
table, then structs, descriptors, chunks (root first), globals.
Cross-references
are INDICES or NAMES, never pointers: a `UniqueId*` is a string index (the
loader re-interns), a `Builtin` is its NAME (`vm_lookup_builtin` re-resolves;
an unknown name is refused), struct defs / descriptors are table indices.
Values go through a recursive codec (none/bool/int/float/str/array WITH its
storage kind + readonly/dict + default/struct instance/struct descriptor/
FuncObject) that ABORTS on an unlisted type - an image is never silently
lossy. Struct LAYOUT is recomputed at load (`compute_layout`), never
stored.
**THE COMPACT INSTRUCTION ENCODING (v3, 2026-07-29).** The fixed 27-byte
field-wise record was 37% of an image; an instruction is now
`op:u8 flags:u16 [the present fields]`, where the flags word gives a
per-field WIDTH CODE - `pa`/`pb` 3 bits each (0 = at its default and NOT
stored, else 1/2/4/8 bytes), `target`/`target2` 2 bits each (0/1/2/4
bytes), one bit each for `aop` and `opflags`, and bits 12-15 RESERVED
(the reader REFUSES a nonzero, so a later version can spend them without
a v3 reader misreading the file). Chosen from a CENSUS over bench/ +
samples/ (3483 instructions): `target` present 95% / one byte in 97% of
those, `target2` default 48%, `pa` default 32% + one byte 66%, `pb`
default 54% - so the dominant instruction is 5-6 bytes, not 27. Two
notes: (1) it is SELF-DESCRIBING on purpose - a per-opcode "which fields
does this op use" table would save the flags word, but this codebase has
a history of per-opcode tables going stale when an op is added
(`visit_use_def`, `op_writes_scalar`, `visit_pc_fields`) and here a stale
entry would mean SILENT DATA LOSS in a stored image; (2) `pa == -1` is
both "unset slot" and the literal -1, which is NOT ambiguous on disk -
the field is not stored and the reader's default restores exactly -1,
while `opflags` separately says whether the operand is a literal (pinned
by the round-trip test's edge-value program: a `-1` literal, every int
width, 8-byte float payloads). Still NOT in-format compression (no
dictionary, no entropy coding, no cross-field bit packing; every field is
a plain little-endian integer at a byte boundary, O(1) decode, two
compiles byte-identical) - the no-compression decision record stands.
Measured: shopping's code section 4741 -> 1086 bytes, the image
10243 -> 6588 (-36%); gcd is now 1.14x its source, shopping 2.44x.
**DERIVED POOLS ARE NOT STORED (v4, same day).** `boxed_ops` is a pure
function of the final code + the loc side table, so the image holds none
of its 49-byte entries: `read_chunk` calls **`build_boxed_ops`** - now
exported from codegen.h for exactly this - once a chunk is read (LAST,
since it needs `locs`), the same rebuild-a-derived-twin shape
`catch_uids` already used. The point is SINGLE SOURCE OF TRUTH as much as
bytes: the loader runs the function CODEGEN runs, so the pool the JIT
bakes `&boxed_ops[target2]` against cannot drift from what a compile
produced. `target2` (the pool index) is still stored and simply
overwritten with the identical value. The `Operand` read/write codec fell
out of serialize.cpp with it - the derived pool was its only user, which
`-Werror=unused-function` pointed out. **The obvious oracle is
INSUFFICIENT and the test says so:** `-vd` prints only each entry's
`target` + `aop`, so byte-identical dumps do NOT cover the rebuilt
operands or carets - the round-trip test compares the pools
FIELD-FOR-FIELD (both Operands' live union member per `lit_kind`, both
Locs) over the root and every function chunk, and COUNTS them so an edit
to its program cannot make the check vacuously pass on two empty pools;
separately, a `dyn` div0 in a JIT'd loop (whose caret the JIT stamps
straight from `boxed_ops[i].start/end`) renders byte-identically from an
image and from source. Measured: shopping 6588 -> 5641, gcd 1957 = 1.08x
its source; cumulative from v1 shopping -56%, gcd -60%.
**THE DELTA-CODED LOC TABLE (v5, same day).** `{u32 pc, u32 line, u32
col, u32 line, u32 col}` = 20 bytes per entry, ~19 of them zero (over
bench/ + samples/, 969 entries: the pc delta never exceeded 19, the
widest caret column was 99), became FOUR bytes - `pcd` u8 (pc - prev_pc),
`lined` i8 (start.line - prev_line), `col` u8, `ecol` u8 (end.col,
IMPLYING end.line == start.line). NO odd widths: a field is EITHER one
byte OR a reserved sentinel byte (255, or -128 for the signed one)
followed by a plain 4-byte little-endian int32 - `put_u8_esc` /
`put_i8_esc`. **Nothing in this format is a 5-byte (or any
non-multiple-of-8-bit) integer needing a shifted top byte** - the
maintainer's explicit constraint, which also rules out LEB128/varints
(byte-granular but bit-TAGGED, the same variable-length machinery the
16-byte-instruction experiment rejected). Folding the end LINE into
`ecol`'s escape is what got 4 bytes rather than 5: 97.7% of entries end
on the line they start, so a dedicated end-line byte would serve 22
entries of 969; a multi-line span costs 4+8 and the (unreachable)
all-four-escaped worst case 24. ON-DISK ONLY - the loader rebuilds the
same `vector<LocEntry>`, so `loc_at`'s binary search is untouched.
**The `-vd` oracle is again INSUFFICIENT** (the dump prints
`pc -> start.line:col`, NEVER `end` - half of every caret), so
`myv_locs_equal` compares tables ENTRY-FOR-ENTRY naming the first
mismatch, and the new `myv_loc_escapes` test forces BOTH escapes (a
statement indented 320 columns; a 200-line gap between two throwing ops),
ASSERTS they were taken so it cannot pass vacuously, and compares. It
caught a real trap on its first run: **`myv_read` runs the load-time JIT,
which inserts EnterNative and REMAPS every pc including the loc table's**
- so any test comparing a written image against an in-memory program must
write FIRST, then `vm_jit_loaded_image` the in-memory side (what
`myv_round_trip` already did). Measured: the loc section 796 -> 212,
the image 5641 -> 5057; **gcd 1765 = 0.98x its source, an image SMALLER
than the .my**; cumulative from v1 shopping -61%, gcd -64%.
**NARROW POOL LOCS (v6, same day).** Every caret pool writes its Locs
through ONE function, so narrowing `Writer::locv`/`Reader::locv` from two
`u32`s (8 bytes) to **`u16` line + `u8` col (3 bytes)** - same whole-byte
sentinel escapes to a plain 4-byte int32 - shrank `member_keys`,
`builtin_calls`, `call_sites`, `emplace_sites`, `incdec_sites`,
`incdec_chains`, `chain_locs`, `throws`, `boxed_ctors` and
`inline_frames` at once. `u16` for the line, NOT a byte, because these
Locs are ABSOLUTE: unlike the loc TABLE's they have no ordering
relationship to delta against, and a one-byte line would escape in any
file over 254 lines. Measured over bench/ + samples/: 1618 pool Locs,
widest line 206, widest column 77, so nothing escapes in practice
(12944 -> 4854 bytes corpus-wide). The escape test now covers ALL FOUR
Loc escapes in one program - the table's (col > 254, line delta > 127)
and a pool's (col > 254, line > 65534) - via a 320-column indent plus
65600 filler lines before an indented `append(arr, s.v + 1)` whose carets
land in builtin_calls/member_keys; it ASSERTS each of the four fired and
compares tables AND pools entry-for-entry (`myv_pool_locs_equal`). A
65605-line source compiles in 9ms, so `-rt` stays at 0.7s. Measured:
shopping 5057 -> 4327 = **1.60x** its source, **gcd 1580 = 0.87x**;
cumulative from v1 shopping **-66%**, gcd **-68%**.
**THE BUILTIN-SLOT HAZARD (found + fixed here):** `SymbolsType` is a
`std::map<const UniqueId *, ...>` keyed by the interned POINTER, so the
builtin table's slot order differed PER PROCESS - a baked
`SymKind::builtin` slot would resolve to a DIFFERENT builtin after a load.
`build_builtin_table_once` now sorts by NAME (deterministic given the
builtin SET), and the image carries `builtin_set_fingerprint()` (count + a
name hash) so a binary with a different set is refused instead of silently
calling the wrong builtin. `-c` also registers `argv` before compiling, so
the compile sees exactly the run's builtin set.
**THE SOURCE REFERENCE (v2, 2026-07-29) - the image does NOT embed the
program text.** v1 embedded the whole source so an error could quote the
offending line; that made the text the second-largest section of an image
(21% of samples/shopping) and meant a `.myv` could never be smaller than
its `.my`. It now stores a `MyvSourceRef` (serialize.h): `root` (the
project root) + `rel` (the source path relative to it) + `abs` (the
compile-time absolute path) + a **CRC32** and byte SIZE of the file -
~100 bytes instead of the program. `myv_source_ref(path)` builds it at
`-c` time; the ROOT is the compile-time CWD when the source lives under
it, else the source's OWN directory, so `rel` is ALWAYS non-empty and an
image is RELOCATABLE (a `rel` full of `..` hops would not be).
`resolve_source` (serialize.cpp) then decides at LOAD time: `--source
ROOT` given -> look ONLY under it (an explicit root that lacks the file
is a mistake worth reporting, not a reason to fall back to a stale
absolute path); else the stored `abs`, then `root/rel`. A file that is
THERE but whose size/CRC32 differs is REFUSED with a warning (a caret
drawn on the wrong text is worse than no caret) unless `-f`/`--force`,
which uses it and warns that carets may be misplaced. An ABSENT file is
SILENT - shipping an image alone is ordinary. `MyvSource` carries
`{lines, name, warning}`; the driver prints the warning to stderr and
passes `name` into `format_exception`, which now takes an optional
`src_name` and renders " at FILE, line L, col C" (a plain SCRIPT run
passes its path too, so the two forms agree; `-e`/the REPL pass "" and
are unchanged). CRC32 is the textbook reflected form computed a bit at a
time in `crc32_of` (no table to carry; it runs twice per file lifetime).
`--strip-source` now stores NO reference at all - so such an image also
leaks no local paths. Path math is hand-rolled (`is_absolute`/`cwd_path`/
`join_path`, `getcwd` behind an `#ifdef _WIN32`): `<filesystem>` would
need `-lstdc++fs` on the older GCCs CI still builds with.
Errors from an image are byte-identical to the source run when the
reference resolves (pinned by a byte-compare); with no usable source the
located header + backtrace still name file/line/func, `errfmt`'s
no-source mode. Compiling twice is byte-identical (pinned). A
corrupt/truncated/incompatible file is a clean `MyvError`.
**THE LOADER IS HOSTILE-INPUT HARDENED (#137), in THREE layers, and the
layering is the point** - each bounds a different KIND of field, and the
first two cannot see what the third checks:
(1) **`Reader::countv`** - an element count, by the bytes REMAINING;
(2) **`Reader::sizev`** - a STANDALONE count that no record array follows
    (a chunk's `slot_count`/`n_temps`, its dict/dyn iterator and try-region
    slices, a descriptor's `frame_size`), by the WHOLE FILE. These had no
    bound at all, and the failure was a HANG, not a crash: one mutated top
    byte made `n_dyn_iters` 234 million and the run sat there building the
    slice, with a BYTE-IDENTICAL disassembly either way;
(3) **`vm_verify_program` -> `verify_chunk`** - every INSTRUCTION OPERAND,
    against the table it indexes. This is the layer the byte-level ones
    structurally cannot do: the format stores instructions field-wise, so
    the reader cannot know which field is a pool index, a frame slot or a
    pc. Run BEFORE the load-time JIT, which bakes pool ENTRY ADDRESSES into
    machine code. It also bounds the pc-KEYED side tables - `locs`,
    `base_locs`, `inline_ctxs` - which look harmless (they are only ever
    binary-searched for an exact match) but are REMAPPED by indexing
    (`l.pc = remap[l.pc]`), and a closure's CAPTURE descriptors, which
    `read_sym` turns into frame/global/capture/builtin slot reads. Plus the
    STATIC CROSS-FIELD facts: a boxed op's `aop` must be one `vm_num_binop`
    can dispatch (asked of the dispatch's OWN tables via
    `vm_aop_dispatchable`, so the two cannot drift); a def a byte-level path
    writes through must be POD; a ctor's arg count must FIT its def (`<=`,
    not `==` - a trailing `opt` field is skippable at the call site, which
    the suite caught the moment `==` was tried); and a baked ctor plan's
    per-field byte offset plus ITS OWN store width must land inside the
    instance (the width matters: a blanket 8 falsely refuses a trailing
    bool).
**⛔ AND A FOURTH SHAPE ALL THREE LAYERS MISS BY CONSTRUCTION: A
CONSTRUCTOR THE LOADER ITSELF CALLS (2026-08-25).** The three layers
bound what an image CONTAINS. They cannot bound an argument the READER
supplies: `read_value`'s `func` case built
`FuncObject(desc, nullptr)`, whose ctor ran `get_root_ctx(nullptr)` -
`while (ctx->parent)` on a null pointer - so `mylang prog.myv`
SEGFAULTED for any image whose pool holds a FUNCTION value, i.e. on a
VALID image of our own making, not a hostile one. **A `pure func` in a
const array (`const OPS = [sq];`) is the whole reproducer**, and no net
saw it for one reason worth generalising: **no corpus program produced
that record**, so `myv_fuzz.py` mutated an image that never contained a
`func` value and `myv_round_trip` round-tripped one too. The fat fuzz
corpus now ends with exactly that shape, and `driver_checks.sh` runs the
image and compares it to the source run (watched failing: `img 139`).
A ctx-less closure is capture-free - the reader now REFUSES any other
kind rather than trusting the writer's assertion - and gets a null
`capture_root`, for which `do_func_call` substitutes the CALLER's root.
**When a loader hands a value to a runtime constructor, the constructor's
PRECONDITIONS are part of the format's trust model.**
**TIER 2 - THE PROVENANCE GATE (`ML_UNTRUSTED_CHECK`, defs.h).** A few facts
no load-time pass can decide, because they belong to the VALUE in a slot and
not to the image: a flat array's storage-kind union tag, a struct field index
against the def the instance actually holds. Those sites check at RUN time,
gated on `g_untrusted_bytecode` - set by `myv_read`, FALSE for bytecode this
process compiled. **NOT tied to `ASSERTS`**, deliberately: the build that most
needs the check is an optimized, assert-free release running a shipped image,
which is exactly the one an assert-gated check abandons. `UNTRUSTED_CHECKS=0`
is the A/B lever, not a shipping config. Measured (`OPT=1 ASSERTS=0` both
sides, interleaved `--baseline`): suite geomean **1.006x**, the affected
struct/array benches scattered 0.95-1.05x with no consistent penalty - but
callgrind says the branch is REALLY there, **+9.4% Ir on 65_struct_field_sum**
and +2.2% on 58_structs, flat elsewhere. The instruction-vs-time divergence
again: a perfectly-predicted branch on a hot-cached global retires free beside
the real work (the same finding as the guard-elision family). Judge it on the
wall clock, and remember which number is which.

**⛔ A `noexcept` JIT HELPER MUST NOT THROW - AND SEVERAL DID (#142).** The
emitter gives a `void`-returning helper NO status test (that is what makes it
cheap), so an exception leaving one is `std::terminate`: the process dies with
no message, no caret, no backtrace. Four did - `jit_arr_len`,
`jit_load_elem_bool`, `jit_load_str_char`, `jit_member_fact_audit` - because
they read their base through `get_ref<T>()`, which THROWS on a type miss, and
because `read_int_slot` / `read_float_slot` / the planned ctor's arms ended in
a throwing `getval<T>()` behind an `ML_VM_CHECK` that a release compiles out.
Two of them also indexed a string or a flat array with a compile-PROVEN index
and no bound - a WILD READ, which is worse than a throw and is fatal in the
interpreter too.
**⛔ AND AN AUDIT OF OUR OWN CODEGEN IS A FALSE ALARM ON A DISK IMAGE.**
`jit_ret_audit` and `jit_member_fact_audit` assert things about what
CODEGEN emitted (that `ref_slots` lists every slot the emitted code can put
a reference in; that a proven slot holds the struct it was proven to hold).
On bytecode read off a disk that premise simply does not hold, so the
assertion is not a bug report - it aborts the process over an image the
loader deliberately accepted. Both now return early when
`g_untrusted_bytecode`. A wrong `ref_slots` can then only LEAK a reference,
never index out of range, because verify_chunk bounds each entry.
**The rule:** in a helper the emitter cannot get a status from, a type miss or
a bad index takes a DEFINED fallback (`none`, `0`), never a throw. The type
test costs nothing - `get_ref` performs it anyway; only the miss branch
changes. And **fix the INTERPRETED TWIN in the same change**: `ArrLen`,
`LoadStrChar` and `LoadElemBool` each have a hand-inlined copy in `vm.cpp`'s
dispatch with the identical hazard, and the unbounded index there is a wild
read even though the interpreter may legally throw.

Two producer-side rules fell out: **only PRE-JIT bytecode is storable**
(`myv_write` ML_CHECKs it - the JIT rewrites code in place and fragments
are not serialized, so a post-jit image names fragments that do not
exist), and `vm_jit_loaded_image` iterates **this program's** chunks, not
the process-global `g_func_chunks` (which nothing prunes, so it re-JIT'd
freed programs' chunks). Measured over 2000 mutations of two images: **0
crashes and 0 hangs in the LOAD**, from 19 hangs / 20 crashes. **AND THE
BUILD TYPE NO LONGER CHANGES THE ANSWER** - `OPT=1` and `OPT=1 ASSERTS=0`
now behave IDENTICALLY on the corrupt corpus, where before the assert-free
build turned named aborts into SIGSEGVs. That is the point of doing this
at LOAD: a check that runs once, in every build, beats one that a release
flag removes. What
remains, deliberately, is a structurally VALID image whose VALUES are
nonsense: it can still trip a runtime `ML_CHECK` (a named abort, never a
SIGSEGV) or loop forever - proving a slot's type at every pc would be a
bytecode type-checker, and halting is undecidable. `-rt`'s
`myv_corrupt_refused` is the net: it writes 0xFFFFFFFF over every
4-byte-aligned word and requires each load to be a clean Exception,
knowing nothing about which words are counts or indices. The BROAD net is
**`tests/myv_fuzz.py BINARY`** (stdlib-only, like `nested_fuzz.py`): five
mutation modes over a small and a fat image, a crash always failing the
run and `--triage` telling a non-terminating PROGRAM (legitimate - it
loads cleanly) from a hang in the LOADER (a bug). **Run it after any
format or loader change**, against BOTH a debug (ASan+UBSan) and an
`OPT=1 ASSERTS=0` build - they catch different things, and it found the
tier-2 throw-through-noexcept and the codegen-audit false alarm below
within minutes of being checked in. A finding is SAVED, because it cannot
be regenerated from the seed: an image embeds its SOURCE PATH. Its sibling
**`tests/repl_fuzz.py BINARY`** does the same for the REPL - a THIRD front
end that shares almost none of the script path (its own incremental
inferencer with cross-input type commitment, retained per-input ASTs, an
open world of map-resident redefinable globals, and a dozen `:` commands
that parse arbitrary text). Its generator is TEMPLATE-based, not
byte-random: random bytes bounce off the lexer, while these reach the
passes that hold state ACROSS inputs. Run it under **RECYCLE=1 + ASan** -
the REPL retains ASTs, so it is where a stale-node identity bug would
live, and that is the detector CLAUDE.md already names for the class. Verified:
83/84 of bench/ + samples/ run identically from an image (the one
exception, rand_sort, calls `rand()`), 84/86 also dump byte-identically
(the 2
residuals differ ONLY in the printed ORDER of a const dict's entries -
MyLang dicts are unordered by spec, and a rebuilt hash map's iteration
order legitimately differs). Sizes vs the source (after v3's compact
instructions, v4's dropped derived pool, v5's delta locs, v6's narrow
pool Locs): fib 2.26x, shopping 1.60x, gcd 0.87x - SMALLER than its
source (v1 was 4.7x / 4.76x / 2.72x). The REPL is out of scope
(it retains ASTs).

**THE RULE IS NOW FULLY SATISFIED AND MACHINE-PROVEN** (2026-07-15,
> plans/archived/vm-ast-free-runtime.md): the call model runs on the serializable
> **`FuncDescriptor`** (funcdesc.h) — `FuncObject::func` points at it, never
> at a `FuncDeclStmt`; params bind from its `ParamDesc` snapshot, captures
> snapshot via its resolved kind/slot list (`read_sym`), backtraces and the
> reflection builtins read it, `Chunk::closure_defs` holds descriptors, and
> chunks are keyed/stamped on the descriptor. `vm_compile` MOVES every
> descriptor + `StructTypeDef` into the **`VmProgram`** image, and the script
> driver then calls **`vm_ast_teardown`** (ASSERTS builds, debug AND default
> release; a no-op under ASSERTS=0): every descriptor's compile-time `decl`
> back-pointer is NULLED, the whole AST is destroyed — each freed node
> `memset(0)`ed by the Construct class `operator delete` — and
> `Construct::live_nodes == 0` is ML_CHECKed. The VM then runs with the AST
> provably gone. The historical inventory lives in
> `plans/archived/vm-fallback-elimination.md`; the descriptor/teardown design in
> `plans/archived/vm-ast-free-runtime.md`.

MyLang's performance philosophy is a two-front strategy, in this deliberate
order — the same order a real C/C++ compiler works in:

1. **Do at compile time everything a compiler would.** The AST tree-walker
   runs *at parse time* (const-eval) and the optimizer stack (fold,
   auto-const/pure, inline, specialize, monomorphize, M8, flat arrays,
   recursion unroll, int-algebra) rewrites the tree into an **optimal AST**
   with the pointless work already removed. This is the project's defining
   trait and is **not** compromised for the VM: the tree-walker is a permanent
   part of the compiler — both as the parse-time const evaluator (woven into
   the parser; it can *never* be a VM's job, since const-eval happens before
   any bytecode exists) and as the producer of the optimized AST the VM
   consumes. A `-vm` run still runs every optimizer pass, then lowers the
   result; the VM never re-does compile-time work.
2. **THEN execute that optimal AST fast at runtime with a bytecode VM.** The
   tree-walker is already ~2.7x faster than CPython (geomean); a flat bytecode
   interpreter removes the residual per-node virtual-dispatch tax — which we
   *measured* to be the tree-walker's floor (an instruction-count study with
   cachegrind killed node-level "superinstructions": fusing a typed operator's
   operand reads saved ~1 instruction/iteration, 0.03%, because it only trades
   a well-predicted monomorphic vcall for an equal-length inline branch; the
   per-node dispatch can only be removed *wholesale*, which is what a VM does).
   The target was **5-10x CPython**; with the native tier it was crossed
   (10.25x on 2026-07-28, see the JIT sections).

The VM does **not** replace the runtime value/scope model — it reuses
`EvalValue`, `EvalContext`, `Frame`/slots, the global/builtin tables,
`num_bin_op`, the `Type` ops, and every builtin unchanged. It is a *dispatch
strategy* (a flat instruction stream + a switch loop) layered over the same
runtime, not a second interpreter — which is what keeps it small and correct.

**THE OP INVENTORY LIVES IN `docs/vm-ops.md`** - which source construct
becomes which opcode, what its operands mean, which pool carries its caret,
and which shapes deliberately decline to a slower tier; plus the side tables
and the `-vd` dump's contents. The authoritative opcode LIST is
`ML_FOR_EACH_OPCODE` in `src/bytecode.h`; that file explains what they are
for. **Read the entry for a construct before changing how it lowers.**

Two consequences of it are load-bearing everywhere, so they are stated here:
the codegen is **NO-FAIL** (every construct lowers or the compiler throws
`NotLoweredEx` — there is no fallback opcode and no path back to the AST, so
a gap is a loud compile refusal, never a silent tree-walk), and **a new op
that writes a frame slot must join `visit_use_def` AND be classified in
`op_writes_scalar`**, while one with a pc field must join
`visit_pc_fields` — those tables are audited, and their consumers run at
different pipeline stages (see THE AUDIT-TABLE STAGE TRAP).

**A NEW OPCODE MUST ALSO BE CLASSIFIED IN `verify_chunk`** (codegen.cpp,
#137) — the post-load structural verifier that bounds every operand against
the table it indexes. That switch has **no `default` case on purpose**, so
adding an opcode FAILS THE BUILD (`-Werror=switch`) until it is classified;
do not "fix" that error with a default. It is the same audit-table trap in
its worst form — a stale entry there is not a lost optimization but SILENT
acceptance of an unchecked operand from a hostile file. `vm_compile` runs
the verifier over its own output under ASSERTS, so a mis-classification
fails the whole suite rather than a `.myv` in the field; that net caught
three wrong entries the day it was written, and one real defect (the
bytecode splice emitting a `MoveV` into frame slot **-1** when the callee's
result is discarded).

The register choice (over a stack machine, which the already-M8-optimized
tree-walker would beat) is also the right IR for the native x86-64 tier.
Design record: `plans/archived/bytecode-vm.md`.

> **⛔ VERIFY NATIVE EMISSION AND EXECUTION WITH HARD EVIDENCE — DON'T ASSUME
> "IT WORKS" (maintainer, 2026-07-21). ⛔** When you nativize a VM op (add it to
> `jit_op_eligible` + an `emit_op` case) or emit ANY new machine code, you have
> NOT finished until you have PROVEN, with hard evidence, that (1) the native
> instructions you expect are actually EMITTED, and (2) they are actually
> EXECUTED at runtime. **Be pessimistic: assume it does NOT work until the
> evidence says otherwise beyond any doubt — VERY OFTEN it doesn't.** A passing
> differential (`-tw` == `-vm`) proves only the RESULT is right, which the
> INTERPRETER can produce — it does NOT prove your native code ran. The trap
> that motivated this rule: six ops were "nativized" and claimed to "shrink
> islands to 0", but a per-op runtime counter revealed FOUR of the six never
> executed in common loop shapes (an op isolated by a boxed neighbour is
> sub-`MIN_RUN` → no fragment → interpreted; the container gate's stale
> `op_is_simple_island` whitelist classifies `MoveV`/`LoadConstV` as ISLANDS, so
> their `jit_*` helpers are never called; and a `SubscriptV` container was
> byte-identical to a `MemberV` one that ran, yet `SubscriptV` executed 0 times —
> an unresolved discrepancy). The "island-op occurrences → 0" claim was the M1
> container-plan (ELIGIBILITY) view — hypothetical, since the model isn't
> flipped — NOT execution. **How to get the evidence, every time:** a PER-OP
> runtime coverage counter (`g_jit_op_run[op]`, bumped inside the helper) + a
> `jit:` TEST that runs a loop exercising the op and asserts BOTH the result AND
> that the counter bumped; `-vdj` to read the actual emitted machine code;
> `MYLANG_JITOPS`-style instrumentation to see per-op call counts on real
> benches. If the counter doesn't bump, the op is nativized-but-dead — a real
> gap, not a nitpick. This is the per-op form of the "prove the code ran" HARD
> RULE (see *Benchmarks*); apply it to EVERY native-codegen change.

**THE MODEL FLIP — native CONTAINERS with bytecode ISLANDS
(`plans/archived/model-flip.md`, M1-M4a landed; M4b measured MOOT — see
the end of this paragraph).** The endgame inversion of the JIT: flip
"BYTECODE with native ISLANDS" (a bytecode chunk, some runs replaced by
`EnterNative`, the interpreter driving between them) into "NATIVE with bytecode
ISLANDS" — EVERY function becomes ONE `call`-able native BLOB; its
un-nativizable regions become islands reached via `call vm_exec_block(from)`;
the native code DRIVES, the interpreter is called only per-island (block-level
dispatch, not per-op). This universalizes native `call <offset>` (not just leaf
callees) and is the platform for the N7 unboxing arc. **The flip is a MILESTONE
toward FULL-NATIVE AOT compilation, not primarily a short-term perf play**
(maintainer, 2026-07-21): every function becomes a `call`-able native blob whose
un-nativized regions are bytecode ISLANDS, and those islands become
progressively RARER as we teach the JIT to emit more ops natively — EXACTLY like
the AST→VM conversion removed its fallbacks (`EvalStmt`…) one op at a time until
the codegen was no-fail. `vm_exec_block` is the general escape hatch that lets us
ship the framework now and nativize ops incrementally; in the limit the islands
vanish and the program is 100% native. So judge the arc by "are islands
shrinking / is the call graph going native," not only by a single milestone's
benchmark delta (loop containers measured marginal — see M4b — yet the framework
is the point). Milestones **M1**
(container-plan analysis + `-vd`, no runtime change — LANDED) → **M2**
(`vm_exec_block` island executor — LANDED) → **M3** (whole-function container,
simplest mixed shape — LANDED) → M4 (island-exit dispatch + branches/loops) → M5
(native calls to EVERY function: the checked-return unwind + growable native
stack) → M6 (delete-originals / `.myv`-ready). **M2:** `vm_exec_block(ctx, act,
chunk, from_pc, *resume)` (vm.cpp) runs a single-entry interpreted ISLAND and
hands control back to the container — chosen model **(b)**: reuse `vm_dispatch`
unchanged (a `start_pc` param + a new `ExitBlock` terminator op that `return`s
with the resume pc), and FLIP the current frame's `boundary` bit for the run so
an island `ReturnV`/`Halt`/uncaught-throw hands back (set flow / signal) instead
of popping the frame — the container owns the real return; a caught island
exception continues. Returns `FellThrough`/`Returned`/`Raised`. **M3:**
`jit_try_container` (jit.cpp, tried first in `jit_compile_chunk`) compiles a
leaf STRAIGHT-LINE body that is exactly ONE contiguous island of simple boxed
scalar ops (`op_is_simple_island`) ending in `ReturnV` into a container — ONE
`EnterNative` at pc 0, the island as `call jit_exec_block(desc, island_pc)`
(`emit_island_call`: `test rax; jns` → FellThrough continues / RAISED exits so
`EnterNative` re-raises via the `g_vm_jit_exc` bridge), a native `ReturnV`, an
inserted `ExitBlock` + pc/side-table remap. A straight-line container is a
MECHANISM PROOF, not a win (the island runs interpreted either way — the win is
M4's native loop around an island), so it is gated OFF for small bodies
(`MIN_CONTAINER_ISLAND`). **M4a:** `jit_try_container` now admits NATIVE
BRANCHES (the loop control — `emit_branch` + `label[]`/`fixups`, whole body so
every target is fragment-local; a back edge may target the island START but not
an interior), so a native LOOP iterates in machine code around a straight-line
boxed island (only the island calls the interpreter). A loop container forms
regardless of island size (the win); a straight-line one keeps the MIN gate.
MEASURED −3.4% instructions on a synthetic loop (container vs `-nj`) — a real but
MODEST win, LIMITED by the per-island `vm_dispatch` RE-ENTRY overhead. Still one
island of simple boxed ops + no calls, so M1-M4a match NO bench/sample (real
loops have a multi-island init+body / richer ops) — ZERO suite impact; M4a is a
mechanism step + the M5 prerequisite. **M4b IS MOOT AS A PERF ITEM, MEASURED
(2026-07-28, plans/archived/model-flip.md "the prize is collected"):**
the nativize-ops
arc got there from the other side. 100% of bench + sample chunks are
container-READY, and `vm_dispatch`'s callgrind SELF cost is a CONSTANT 47
instructions (invocation entry + Halt) across every hot bench — the
interpreter loop does not execute in the steady state anywhere on the suite,
so there is no dispatch left for a flip to capture. The runtime already IS
"native driving, C++ helpers for the hard ops", reached by growing the islands
to cover everything rather than by flipping the driver. The one nonzero
residue is 70_exc_runtime_error's post-throw resume, an exception-PATH cost
(#74's territory, not a flip target). What remains of the flip is the
ARCHITECTURE milestone: M5-flip is largely subsumed by the M5b/c native sync
(docs/jit-optimizations.md)
calls, and M6 (delete-originals + `.myv`) has since landed. Proven
by the `jit_container` `-rt` test (`g_jit_container_calls` coverage + a
throw-from-island + a loop container) + differential + fuzzer + `-vdj`. Builds on
`plans/archived/native-call-impl.md` (v1 native calls) and
`plans/archived/native-aot.md` (approach A, the fragment ABI).

## Invariants & hazards (defense in depth)

This project deliberately builds many overlapping correctness layers (a
"Swiss-cheese" model: every check has blind spots, but stacked checks with
*different* blind spots make a bug clear them all very unlikely). The rules
below came out of a real, nasty bug — an MSVC-only, non-deterministic
wrong-result in cross-input REPL template instantiation, root-caused via CI
instrumentation (see `plans/archived/function-templates.md`).

- **A red test on ANY platform is a real bug — never route around it.** A
  one-platform CI failure you can't reproduce locally is NOT "flakiness" to
  revert or disable the feature over; it is a defect to root-cause. "Can't
  reproduce locally" raises the instrumentation bar, it never lowers the bug's
  reality. Multi-compiler / multi-platform CI exists precisely to expose UB and
  logical-identity bugs the dev allocator hides — lean into it.

- **MyLang is single-threaded — there is NO "I can't reproduce this bug."** No
  threads, no races, no timing nondeterminism. A failure is a pure function of
  (source commit, build config, inputs), plus at most ASLR — and ASLR only
  shifts a memory bug's *manifestation*, never its *existence*; ASan / valgrind
  are deterministic. So "I couldn't reproduce it" is NEVER an acceptable
  stopping point — it means you have not yet matched one of those inputs. In
  practice the miss is almost always **the commit**: build the failing CI run's
  EXACT `headSha` (a rebase diverges SHA *and* code, so a local same-named
  commit may already contain the fix) with the SAME config (RECYCLE+ASan for a
  RECYCLE-lane failure — plain ASan can miss an AST-node UAF), and it
  reproduces. Keep going until it does.

- **On a CI failure, read the logs of ALL failing jobs — never sample.** Do NOT
  look at one job, form a theory, and stop. Pull every failing lane's full log
  (`gh run view <id> --log-failed`) and read it — different lanes fail
  DIFFERENTLY and one of them usually hands you the answer. A real case: several
  lanes showed only a bare `Segmentation fault`, but ONE lane (the RECYCLE/ASan
  job) carried a complete AddressSanitizer use-after-free backtrace pinpointing
  the bug — and sampling the wrong lane turned a five-minute fix into a long,
  wrong "unreproducible heisenbug" hunt. Grep the FULL log for
  `AddressSanitizer`/`runtime error`/`Assertion`/backtrace frames, per job.

- **DO NOT BE LAZY. Do all the work, check everything, do not give up.** Every
  failing lane, every log, the exact SHA, the actual reproduction — not a
  plausible-sounding shortcut. Sampling one artifact, guessing from a partial
  read, declaring a deterministic bug "unreproducible", or handing back a theory
  instead of a proven root cause are all failures of diligence, not of skill.
  When the evidence and a convenient conclusion disagree, chase the evidence.

- **Never key a long-lived map by a raw AST-node pointer.** A `Construct *` is
  NOT a stable identity: the allocator recycles a freed node's *address*, so a
  map that outlives the node (e.g. across REPL inputs) can match a stale entry
  with a fresh node — a silent wrong-lookup, invisible to ASan (the memory is
  valid; the *identity* is wrong). Use a stable identity instead: the codebase's
  established ones are **`UniqueId *`** (interned names, never freed) and the
  **monotonic arenas** (`all_syms`/`all_funcs` — never truncated, only marked
  `pinned`). For nodes there is now **`Construct::node_id`** (a monotonic
  `uint64`; a clone gets a fresh one). The two remaining node-keyed maps
  (`id_sym`, `func_of_decl`, inferencer) are instead **scoped to one input**
  (cleared each `infer_input`) and re-resolved every pass, so no stale entry can
  survive — that combination is the fix for the bug above.

- **`ML_CHECK` / `ML_CHECK_MSG` (`defs.h`) are the assertion layer.** Use them —
  not bare `assert` — to state an invariant the code RELIES ON but a wrong
  change could break: "this is impossible if the code is correct." They must be
  **side-effect-free**. They follow the C `assert()` exactly: active unless
  `NDEBUG`, i.e. gated by the **`ASSERTS`** build flag (defaults **ON for every
  build type**, debug AND release, in both build systems — so every build and
  every CI lane exercises the full net). `ASSERTS=0` defines `NDEBUG`, compiling
  both the C asserts and these away — the way to measure their overhead
  (`make OPT=1 ASSERTS=0` vs `make OPT=1`). When ASSERTS is on, the build also
  enables stdlib container hardening (`_GLIBCXX_ASSERTIONS` / libc++
  `_LIBCPP_HARDENING_MODE`). **Assert the right things:** logical
  invariants the sanitizers CANNOT see — a union/tag mismatch (reading the wrong
  active member is valid memory, wrong meaning), a refcount underflow, a state
  the type system should have made impossible, a stable-identity check. Do NOT
  add asserts for plain out-of-bounds / shift-overflow / use-after-free —
  ASan/UBSan already catch those, and a real *runtime* condition (bad user
  input, I/O failure, a genuine type error) must `throw` a proper `Exception`,
  not assert. Examples in place: the `flat_*()` union-kind checks
  (`sharedarray.h`), `intrusive_ptr::release` refcount-underflow
  (`intrusiveptr.h`), `pod_get`/`pod_set` field validity (`structtype.h`),
  `Frame::init` `frame_size <= 64` (`eval.h`).

- **`ML_VM_CHECK` (`defs.h`) — the VM's HEAVY per-op layer.** A separate, hotter
  tier than `ML_CHECK`, for invariants too expensive to run on every register
  access in a normal release: a **frame-slot bounds check** (`Frame::at(i)`, now
  used for *every* `frame->slots[i]` in `vm.cpp` and `eval.cpp` — the
  resolved-local path both engines share) and an **operand type-tag check** (a
  `th==i/f` operand must actually hold an int/float;
  `read_int_operand`/`read_float_operand`). This is the net for a
  **layout-dependent VM UB**: a bad slot index reads an *unconstructed* inline
  slot (`[size, 8)`) or an *out-of-range* one — a garbage `LValue` whose garbage
  type pointer crashes only on some toolchains — and the bounds/tag check turns
  that into a **loud, located** failure *everywhere* it occurs, not just where
  the garbage happens to be a bad pointer. Gated by `ML_VM_HARDENING` (the
  **`VM_HARDENING`** build flag: default ON for a debug build, OFF for a plain
  release; CI forces it ON in the *release* lanes — see *VM hardening* under
  "Build & run") **and** by `ASSERTS` (a no-op under `NDEBUG`). It was added
  after a rare, non-deterministic CI-only segfault at a VM inline test that
  reproduced under NO local tool (Release loops, ASan, UBSan, RECYCLE, valgrind
  memcheck — 100+ runs on the failing commit and HEAD): the bug spans a *range*
  of commits and is layout/toolchain-specific, so the response is *more
  instrumentation on CI* (this + the Linux core-dump artifact), not a bisect.
  `Frame` gained a `size` field (the constructed-slot count) for the bound.

- **The TREE-WALKER recurses on the C stack — Windows needs a bigger stack
  reserve.** The tree-walker (`do_eval`) recurses per call; the VM no
  longer does (the native call stack runs VM->VM calls inside ONE
  `vm_run_chunk` activation - C-stack depth is O(1) per activation, and a
  runaway recursion throws the CATCHABLE `StackOverflowEx` at the
  `MYLANG_VM_STACK` slot cap instead of crashing). The C++ recursion that
  remains under `-vm` is per BOUNDARY entry (a builtin callback re-entering
  the loop), not per call. Deep
  recursion (the ackermann test, deep `fib`) needs more than **Windows' 1 MB
  default** stack; Linux and macOS default to **8 MB**, which is why they never
  hit this. The **fix is a linker flag** — `CMakeLists.txt` sets
  `/STACK:8388608` (8 MB) for the MSVC build, giving Windows parity (legitimate
  provisioning for an interpreter, not a workaround). This was a **Windows-CI-
  only crash** the VM's ackermann test hit (exit 127), that **no Linux tool
  reproduces** (ASan and `ML_VM_CHECK` do not catch a stack-DEPTH overflow — it
  is not a slot-bounds bug); it surfaced only after P8 added a `try/catch` EH
  frame to `vm_run_chunk` (MSVC Debug's EH + `/RTC` + no-scoped-local-overlap
  frame is much larger than GCC/clang's, tipping it over 1 MB). **Complementary
  hygiene (keep the recursive frame lean anyway):** an op whose handler needs a
  sizable stack buffer puts it in an `ML_NOINLINE` helper (e.g. `vm_make_array` /
  `vm_struct_ctor` / `vm_make_struct_array_op` — the buffer lives in the HELPER's
  frame, alive only during that op, NOT reserved in every recursive
  `vm_run_chunk` frame), never inline in the `vm_run_chunk` switch. **To
  reproduce/measure a deep-recursion stack cost on Linux:** run `-rt` under a
  smaller stack (`( ulimit -s 1024 && ./mylang -rt )`) — ASan then reports a
  `stack-overflow in vm_run_chunk`; and `g++ -fstack-usage` (grep the `.su` for
  `vm_run_chunk`), optionally with `-fstack-reuse=none` to mimic MSVC's
  no-overlap frame (still an under-estimate — MSVC Debug's EH/`/RTC` overhead is
  extra).

- **`RECYCLE=1` — the adversarial allocator.** `make RECYCLE=1 TESTS=1` builds a
  `Construct` allocator (a size-keyed LIFO free-list, `syntax.cpp`) that hands a
  just-freed node's address straight back to the next allocation, so any
  "AST pointer used as a stable identity" bug manifests DETERMINISTICALLY under
  `-rt` instead of depending on the host allocator's luck (ASan-poisons the
  free window too, so a dangling read is still caught). It is a general stress
  tool for that whole class and a future-regression net. **Honest scope:** it
  did *not* by itself reproduce the specific cross-input bug above — the REPL
  test path retains its ASTs, so there were no intra-test frees to recycle; the
  per-input map clear is the guard there. It runs as a dedicated CI lane
  (`linux.yml`, `-DRECYCLE=ON`) and a local matrix entry — not a replacement for
  the structural fixes.

- **Reproducing an AST-node use-after-free: RECYCLE+ASan is the detector, and
  build the EXACT failing SHA.** A dangling `Construct*` UAF is fully
  DETERMINISTIC — MyLang is single-threaded, so it is never a race; the only
  variable is the allocator's address-reuse *pattern*, which decides whether a
  freed node's memory is poisoned at the stale read. **Plain ASan can MISS it**
  (its quarantine reuses the address before the read — a real case here passed
  under plain ASan even with `ASAN_OPTIONS=quarantine_size_mb=2048`), while
  **`make RECYCLE=1 TESTS=1 OPT=0` reproduces it 3/3** (immediate reuse + poison
  of the freed `Construct`). So reach for **RECYCLE+ASan**, not plain ASan, for
  any suspected node UAF. **And when a CI job fails, check out the run's exact
  `headSha`** (`gh run view <id> --json headSha`), not your local same-named
  commit: after a rebase the SHAs *and the code* diverge, so a local build can
  silently already contain the fix (this exact trap cost a long "unreproducible
  heisenbug" hunt — the local commit had the fix, the CI's pre-rebase SHA did
  not). No exotic tooling (a GCC plugin, etc.) is needed; the deterministic
  repro was there under the existing RECYCLE lane the whole time.

- **The VM body hook must not compile a chunk during compile-time folding.**
  `do_func_call`'s Phase-4 hook reads (and, via the never-hit safety net, can
  compile) a function body's `Chunk`, stamped on the `FuncDescriptor`. During
  `resolve_names` (AutoConst / the inliner's refold) the tree is STILL being
  mutated, so a fold that reached the hook would cache pointers into nodes the
  optimizer then frees → a UAF (exactly the bug above). Two layers stop it: the
  **`!ctx->in_const_eval()` gate** in `do_func_call` (a fold's ctx is rooted at
  the const `cctx`, so `in_const_eval()` is true and the hook is skipped) and a
  **`g_vm_executing` assert in `vm_func_chunk`** (set only inside
  `vm_compile`/`vm_run`), so any future fold path that slips past the gate
  fails LOUDLY instead of corrupting memory.

- **The ZERO-AST teardown proof (`vm_ast_teardown`, plans/vm-ast-free-
  runtime.md).** In every ASSERTS build (debug AND the default release; a
  no-op under `ASSERTS=0`), a `-vm` SCRIPT run destroys the ENTIRE AST between
  compilation and execution: `vm_compile` MOVES every `FuncDescriptor` +
  `StructTypeDef` into the `VmProgram` image (the in-memory shape of the
  future `.myv`), then the driver NULLs each descriptor's `decl` back-pointer,
  `root.reset()`s the tree, and ML_CHECKs `Construct::live_nodes == 0`. The
  Construct class `operator new/delete` (defined in all non-NDEBUG builds;
  the RECYCLE allocator maintains the same counter) count live nodes and
  `memset(0)` every freed node, so a residual `Construct*` anywhere reads
  zeroed, freed memory — a loud crash, never a silent dependence. The REPL
  and the `-rt` harness retain their ASTs by design (`vm_execute` =
  compile+run with no teardown); `mylang.cpp` owns the `VmProgram` OUTSIDE
  the try block for the same reason `root` is out there — an uncaught struct
  exception's payload references its (now program-owned) `StructTypeDef`.

- **CI maximizes correctness checks (it does not time anything).** Every CI lane
  builds with `ASSERTS` on (C asserts + `ML_CHECK` + stdlib container
  hardening) **and `VM_HARDENING=ON`** (the `ML_VM_CHECK` per-op tier — so even
  the **release** lanes, where a local release runs *without* it, get the
  frame-slot bounds + operand type-tag checks), Debug lanes add ASan + UBSan,
  UBSan runs with `-fno-sanitize-recover` (a finding ABORTS, so it can't
  print-and-still-exit-0 past the exit-code check), and there is a `RECYCLE=ON`
  lane. Slower is fine — performance is measured separately (`bench/`, a plain
  `make` release, which is NOT hardened). Adding a new check here is cheap
  insurance; reach for it.

- **CI core-dump capture (`linux.yml`).** Both Linux jobs (the Debug/Release
  matrix and the `recycle` lane) enable core dumps (`ulimit -c unlimited` + a
  `core.%e.%p` pattern in the build dir) before `-rt`, and on ANY failure a
  `Backtrace on crash` step installs `gdb` and prints `thread apply all bt full`
  from each core to the log, while an `Upload crash artifacts` step
  (`if: failure()`) uploads the core **and the exact binary** as a downloadable
  artifact. So when the rare layout-dependent VM crash DOES occur on CI it is
  immediately debuggable (a real backtrace, an offline-loadable core) instead of
  a bare "Segmentation fault". This is the deliberate response to a
  can't-reproduce-locally bug: *raise the instrumentation bar on CI*. (A blind
  loop-`-rt`-N-times was rejected — it burns CI without improving the odds much;
  the `VM_HARDENING` checks, which fire on the bad access itself rather than
  only when its garbage is a bad pointer, are the higher-value catch.)

## Recipes

### Adding a builtin
1. Implement `EvalValue builtin_xxx(EvalContext *ctx, ExprList *exprList)` in
   the appropriate
   `src/builtins/*.cpp.h`. Builtins get **unevaluated** argument expressions —
   evaluate each yourself
   with `RValue(exprList->elems[i]->eval(ctx))`, and validate arity
   (`InvalidNumberOfArgsEx`) and
   types (`TypeErrorEx`), passing the argument's `start`/`end` `Loc`s for good
   error messages.
2. Register it in `types.cpp`: `make_const_builtin(...)` in the `const_builtins`
   map if it is pure and
   safe to run during const-eval, otherwise `make_builtin(...)` in the
   `builtins` map (runtime only —
   I/O, `rand`, mutation, `exit`, …). Const builtins are what const-folding is
   allowed to call. **A builtin that inherently needs the AST** (like `show()`,
   which decompiles it) should instead be a **dev-only builtin**:
   `make_dev_builtin(...)` — it registers like `make_builtin` but records the
   name in `g_dev_builtin_ids`, so the inferencer (`reject_dev_builtins`) makes a
   SCRIPT call a compile-time error while the REPL / test harness (which set
   `g_dev_builtins_allowed`) allow it. This keeps the AST out of serialized
   script bytecode. **A builtin whose argument is a NODE property (never
   evaluated — `defined`/`isconst`/`isconstdecl`) must additionally be wrapped
   in `mark_lazy_builtin(...)`** at registration: a script may only CALL it
   directly — using the name as a VALUE is a compile error (the F1 rule; an
   indirect dyn call could never reproduce the lazy semantics AST-free).
3. Document it in `README.md` (const vs. non-const section) and add a test in
   `src/tests.cpp`.

**⛔ A HIGHER-ORDER BUILTIN CALLS ITS CALLBACK THROUGH `VmInvoker::call`,
AND THROUGH NOTHING ELSE (2026-08-14).** Construct one `VmInvoker inv(ctx,
funcObj)` outside the loop and write `inv.call(args...)` per element —
passing the arguments in whatever C++ types you already hold (a flat
array's raw `int_type`, an `EvalValue`, a `SharedStr`). `call` boxes each
argument exactly ONCE and picks the tier itself: the prepared window
(one boundary frame for the whole loop) or `eval_func` when there is no
activation to run one on. Do NOT hand-roll the
`inv.ready() ? inv.invoke(argv, n) : eval_func(...)` ladder — all five
existing sites did, each slightly differently, and the shared entry is
what stops the sixth from inventing a sixth spelling. It is also a
measured win, not just tidier: reaching the invoker through a
`cmp2(EvalValue, EvalValue)` helper made `sort` box each operand TWICE
(once for the helper's parameters, once into the argv), and removing that
double boxing read −16.2% instructions and 0.89x wall clock on
34_sort_custom_cmp. **Do not "improve" it by writing the raw scalar
straight into the callee's window slot** — that was built in four shapes
and lost the wall clock every time while winning every simulated metric;
the record, including how to prove reach with `MYLANG_JITSTATS`'
`cb_prepared`/`cb_fallback`, is in `plans/top5-cpp-gap.md`.

**Memory safety with user callbacks.** A builtin that drives a sort/search with
a *user-supplied* callback must not assume the callback is well-behaved — it is
arbitrary script code. In particular `sort(arr, cmp)` (`builtins/arr.cpp.h`)
uses a **hand-rolled iterative heapsort**, not `std::sort`, for the
custom-comparator path: `std::sort`'s unguarded partition/insertion reads off
the ends of the buffer when the comparator isn't a strict weak ordering (a
heap-buffer-overflow reachable straight from a script), whereas the heapsort's
`sift_down` index strictly descends — so it terminates for *any* comparator —
and only ever indexes within `[0, n)`. It is hand-rolled rather than
`std::make_heap`/`std::sort_heap` because MSVC's debug STL wraps those in
comparator-validity instrumentation that *hangs* on a non-ordering comparator.
The default (no-comparator) path keeps `std::sort` — its `operator<` is a valid
ordering for homogeneous types and throws `TypeErrorEx` for incomparable ones.
Keep this distinction if you touch sorting or add another callback-driven
algorithm. **Callback handle lifetime:** when a builtin keeps a raw
`FuncObject *` to the callback, the `shared_ptr` that owns it must outlive every
call — an inline lambda (`find(a, x, func(e)=>…)`) has *no other owner*, so a
raw pointer extracted from a `RValue()` temporary that goes out of scope before
the loop is a use-after-free (the `find()` key-func bug: the handle was pulled
out inside the `if (3 args)` block but used after it; fixed by holding a
`shared_ptr` for the whole call). Bind the owning value at the same scope as the
use, or copy the `shared_ptr` to keep it alive.

### Adding a value type
Touch all of: the `TypeE` enum (`type.h`) — mind the trivial/non-trivial
position vs. `t_str`; the
`TypeToEnum` specialization + `ValueU` union member (`evalvalue.h`); the
`TypeNames` and `AllTypes`
arrays (`types.cpp`, kept index-aligned with `TypeE`); and a new `TypeXxx` class
overriding the
needed virtuals in `src/types/xxx.cpp.h` (extend `TypeImpl<T>` for non-trivial
types to inherit the
type-erased lifecycle ops). Then `#include` the new `.cpp.h` in `types.cpp`.

### Adding an operator or keyword
Add to the `Op`/`Keyword` enum and the matching `OpString`/`KwString` array
(keep indices aligned),
wire it into the right `pExprNN` level (or `pStmt`) in `parser.cpp`, add the
`do_eval` behavior (a new
`Type` virtual for an operator, or a new node in
`syntax.h`/`syntax.cpp`+`eval.cpp` for a statement),
and cover it in `tests.cpp`. A **new `Construct` node must also implement
`clone()`** (pure virtual) — usually a few lines using the shared helpers;
omitting it is a compile error. **It must also stamp its `ConstructType`
tag** (#102, 2026-08-25): the compile-pass walkers (`Inferencer::
for_each_child`, the resolver's `for_each_child` / `for_each_child_slot`
/ `fmi_children`) dispatch on `Construct::ct` with EXHAUSTIVE switches —
no `default`, so `-Werror=switch` fails the build until the new kind is
classified in each — and the `Construct` ctor ML_CHECKs the tag is not
`other`, so an unstamped class aborts `-rt` on its first construction.
This replaced the walkers' dynamic_cast chains, which were ~86% of
compile Ir, and increment 2 then converted the per-node LADDERS inside
the passes the same way - `ctag(e) == ConstructType::x` is exactly
`dynamic_cast<X *>(e) != nullptr` for a leaf class (ctag is null-safe:
null -> `other`, which no real node carries), so ~400 guarded arms in
inferencer/resolver/codegen became tag tests via a scripted transform
restricted to leaf classes and double-eval-safe operands. Casts to a
BASE (SingleChild/MultiOp/MultiElem/Literal) or to a CallExpr SUBCLASS
(Direct/Cached/DirectBuiltin - they share the `call` tag) stay
dynamic_cast. Total: 139.7M -> 23.8M on 83's compile, -83%.

## Conventions

- **MyLang is a NEW, actively-evolving language — nothing is written in stone;
  FIX the design, don't work around it (maintainer-set, 2026-07-22).** The
  language is not perfectly designed and is changing right now. When you hit a
  problem whose real root cause is a language/design wart, do NOT silently code
  around it — **PROPOSE the design change GENTLY to the maintainer (never do it
  ALONE / unilaterally)**, ideally with a "for now" fix plus a note to reconsider
  the interface later. Concrete example that set this rule: nativizing
  `CallBuiltinV` in the JIT tripped over `InvalidNumberOfArgsEx` /
  `InvalidArgumentEx` being **non-runtime** (non-script-catchable, `DECL_SIMPLE_EX`)
  exceptions that the native code's `RuntimeException`-shaped conveyance could not
  carry (→ `std::terminate`). The RIGHT fix was not to widen the native code to
  support non-runtime exceptions, but to **make those two exceptions inherit from
  `RuntimeException`** (`InvalidArgumentEx` is inherently DYNAMIC so it belongs
  there permanently; `InvalidNumberOfArgsEx` is a *for-now* move — the deeper fix
  is to give builtins FIXED arities, e.g. split `write(str[, file])` into
  `write(str)` + `fwrite(str, file)`, so arity becomes a COMPILE-time check and
  the runtime throw disappears). The bar: surface the language-level option,
  recommend, and let the maintainer decide — same as any design fork.

- **⛔ CLAUDE OWNS THE CASE MATRIX AND THE MAP (maintainer-set,
  2026-08-02).** The maintainer runs this as a SIDE PROJECT with limited
  time, and deliberately holds LESS context than Claude does about the
  work in flight. Waiting for him to discover a hole is a failure of the
  arrangement — the motivating case: the inline element-store tier landed
  for INT and the float twin silently kept paying ~85 Ir per element
  until HE noticed. Therefore:
  - **When a change implements case X of a design, ENUMERATE the sibling
    cases before calling it done**: type variants (int/float/bool/str/
    dyn), op variants (load/store, single-level/nested, plain/compound),
    engine variants (tree-walker/VM/JIT), creation vs mutation. Finish
    the cheap ones in the SAME change; for the rest, END THE REPORT with
    the explicit remaining-cases list and tracked tasks — UNPROMPTED.
    "If we did it for X, either do it for Y or say Y is missing."
  - **After each approved task, PROACTIVELY propose the next steps that
    fit the same design** — including "there are N cases left to reach
    the coherent state that unblocks the next level", when that is the
    truth. Draw the whole map, honestly sized.
  - This does NOT license unilateral scope-widening. The rule below
    ("the maintainer prioritizes") stands: he sequences the work and
    signs off on new directions. The division of labor is: **Claude
    draws the map — completely; the maintainer picks the route.**
    PARITY within an already-approved design (the float twin of a landed
    int tier) is in scope by default; a NEW design is not.
  - A task report that lands case X without the sibling-case list is an
    INCOMPLETE report, the same way a behavior change without its README
    update is an incomplete change.

- **⛔ MyLang DOES NOT CODEGEN WHAT THE C++ STANDARD LIBRARY ALREADY
  DOES (maintainer-set, 2026-08-14).** A builtin's ALGORITHM stays
  hard-coded C++ - `std::sort`, the hash map, the string search. Do not
  propose, and do not build, a JIT that EMITS a specialized copy of one
  (a sort generated per comparator call site, a map/filter fused into
  its callback). The maintainer's words: *"overkill for MyLang"*, and
  *"I don't want MyLang to produce with codegen implementations of
  everything that already exists in the C++ std library."*
  **THE CONSEQUENCE IS A PERMANENT, ACCEPTED ASYMMETRY**, and it is why
  bench/cpp is written the way it is: `std::sort` INLINES its comparator
  and `builtin_sort_lv` structurally cannot, so the C++ twin is required
  to hand its comparator to a `noinline` helper as a `std::function`
  (see plans/archived/bench-fairness.md, the 2026-08-14 reversal). The
  fairness fix goes on the BENCH side because the implementation is not
  going to change.
  **WHAT IS STILL ALLOWED, and the line is worth stating precisely:**
  making the CALL CHEAPER is not reimplementing the algorithm. A typed
  callback entry - entering a proven `(int,int)->bool` comparator with
  two raw `int_type`s instead of two boxed `EvalValue`s bound through
  the generic parameter path - leaves `std::sort` exactly where it is
  and still calls the comparator once per comparison. That is in scope.
  Emitting the sort is not.
- **THE MICRO-STEP ORDER INSIDE A TASK IS YOURS — DON'T ASK
  (maintainer-set, 2026-08-14).** The maintainer sequences work at the
  HIGH level: optimization vs feature vs test coverage, and which task
  comes next. Inside a task he has already chosen, the order of the
  small steps is Claude's call, and the criterion is **whichever order
  makes the work EASIER TO EXECUTE** — not which reads better in a
  report. If the NET RESULT of the task is the same, do not stop to ask
  permission to reorder.
  The case that set this: the call-window work needed its
  correctness ORACLE rewritten before the thing it verifies could
  change (the test rests on a field the change deletes), i.e.
  test-then-build instead of build-then-test. That was surfaced as a
  fork for the maintainer; it is not one. Rewriting the oracle first IS
  part of the task, so just do it in that order.
  **What still goes to him:** a change to the LANGUAGE or the
  ARCHITECTURE, a new direction, dropping a piece of the task, or a
  result that contradicts the task's premise. Not "may I do step B
  before step A".
- **DON'T pick work by "highest value" — the MAINTAINER prioritizes; YOU pick
  the SMALLEST step that makes progress in the chosen direction
  (maintainer-set, 2026-07-19).** When deciding what to do next, do NOT rank
  candidate tasks by estimated value/impact and choose the biggest win — that is
  the maintainer's call, made on what is valuable to HIM. Your job, within the
  direction he has set, is to take the SMALLEST incremental step that makes real
  progress toward the goal (and keeps the tree green + tested). So when asked
  "what next," propose the smallest concrete progress step in the CURRENT
  direction — not a detour to a higher-value-but-different task. (This came up
  when, asked for the next native-function step, the response drifted into a
  higher-value value-model profiling detour instead of the smaller native-call
  foundation step. Wrong: stay in the maintainer's chosen lane and shrink the
  step.) Surfacing a value observation is fine; re-prioritizing on it is not.

- **NEVER do anything LAZILY unless the maintainer explicitly asked for it.**
  As a decision heuristic, ~95% of the time on THIS project the right call is
  the NON-lazy one — so default to non-lazy / deterministic / upfront, and only
  consider a lazy design after explicit sign-off. (The "95%" describes how to
  bias YOUR choices, NOT a fraction of the work.) Do NOT introduce
  lazy/on-demand/first-touch computation, caching, compilation, or allocation on
  your own initiative; if one seems warranted, PROPOSE it, get sign-off first.
  The ONLY approved lazy exception to date is the **per-frame pure-call cache**
  (a lazily-populated `PureCache`, sound because frame-scoped — see recursion),
  reviewed + approved case-by-case. **The former lazy-VM-compile deviation is
  FIXED:** the VM now compiles EVERY function body to its `Chunk` UPFRONT
  (`vm_precompile_all`, run by `vm_compile`), so the AST is 100% bytecode
  before the VM runs (full AOT); `do_func_call` reads a precomputed
  `FuncDescriptor::vm_chunk` and never compiles at call time (the old lazy
  `min_args_cache` is gone too — `min_args` computes at `sync_params`). When
  in doubt: upfront, not lazy.

- **The TYPE MODEL is C++ (static everywhere), with `dyn` as the one variant.**
  Every type is decided at COMPILE time and NEVER changes at runtime. `var` is
  C++'s `auto` — a statically-inferred concrete type, not a dynamic slot. A
  function with un-annotated params is a TEMPLATE, monomorphized per concrete
  call signature (like a C++ template), so `foo(x)` called with `int` and with
  `str` is two separate typed instances. **`dyn` is the ONE variant type — big,
  fat, and slower** — the deliberate escape hatch: a `dyn` variable's STATIC
  type is `dyn` forever (it does NOT change), but the runtime VALUE it holds may
  be a different concrete type moment to moment (checked at runtime; a mismatch
  is a runtime error). So "types don't change" holds even for `dyn`: the type
  stays `dyn`, only the boxed value's dynamic type varies. This is WHY it ALL
  AOT-compiles upfront (all types + template instances — incl. a `dyn`
  instance — known before any bytecode runs) and why a native op must have
  BOTH a typed fast tier and a boxed `dyn`/variant tier, never an AST fallback.

- **A breaking language change must update `samples/` too, in the SAME change.**
  The extensionless scripts in `samples/` (`fib`, `gcd`, `loop`, `phonebook`,
  `primes`, `primes2`, `rand_sort`, `shopping`, `strloop`, …) are part of the
  codebase, not throwaway demos — they are the human-facing showcase of the
  language and must always run. When a change alters script-visible behavior
  (a removed/changed keyword, a stricter rule, a new error condition), run every
  sample and fix any that broke, in the same commit as the change (alongside the
  `README.md` update — see the doc-sync rule at the top). Verify with the actual
  interpreter, not by eye; some samples are interactive (`phonebook` reads
  stdin), so a bare run "erroring" may just be the script's own handling of
  empty input, not a regression — distinguish a MyLang `Exception`/compile error
  from the script's own output. (A sample that was ALREADY broken by an earlier,
  unrelated change is a separate bug to fix on its own, but don't let a breaking
  change ADD to the pile.)

- **Incremental is fine; ending in a half-measure is not.** Landing a feature in
  stages — even with temporary duplication or a stubbed corner — is welcome, as
  long as the *task* ends at a proper solution. Don't stop at "works but
  duplicated/limited" and call it done; either finish the clean version or
  leave a written, tracked follow-up that says exactly what remains. A specific,
  non-negotiable instance: **the named-argument syntax must never cost an
  optimization.** A named call has to be optimized (const-fold, inline,
  specialize) *identically* to the positional call it desugars to — if a name
  ever disabled a fold that the positional form would get, the feature is a
  regression and would be better not used at all. Hold any sugar to the same
  bar: it may add spelling, never subtract capability.
- **ASK before reverting a working optimization the maintainer asked for, even
  if it measures perf-neutral.** (Maintainer, 2026-07-19.) If an optimization is
  CORRECT (tests green) and the maintainer requested it (or it came out of
  fulfilling their request), do NOT unilaterally revert it just because a
  measurement says it's neutral — PRESENT the data and ASK. They may want it
  kept anyway (it's a building block — e.g. a native dict store is a prerequisite
  for the model flip), or want it IMPROVED (extend its reach), not dropped. This
  compounds with the "distrust a surprising result" rule above: the very
  measurement that says "neutral" may itself be the noisy/unproven artifact — a
  perf-neutral finding is exactly when you should double-check before acting on
  it. (Concrete case: the native dict store was reverted as "0%", which turned
  out to be measurement noise hiding a real ~6.5%.)
- **Interactive `git rebase -i` is permitted in this repo** (the environment's
  general "no interactive flags" restriction is waived here by the maintainer) —
  use it to keep history clean / bisectable, e.g. squashing a fix into the
  commit that introduced the bug. Drive it non-interactively from an agent with
  `GIT_SEQUENCE_EDITOR` (rewrite the todo) and `GIT_EDITOR` (supply messages).
  `exp-work` is a topic branch whose history may be rewritten freely.
- **Never edit a source file while a build that compiles it is running.** A
  background `make`/`cmake` reads `src/` AND writes shared dep files (`.d/`) as
  it goes; editing during that window makes it compile a half-written file or
  corrupt a dep, producing a *bogus* "BUILD FAILED" that looks like a real
  regression and wastes a debugging cycle. Serialize: let a background build (or
  the test matrix) finish before touching sources, or run it in a separate
  `BUILD_DIR` / worktree. Docs (`CLAUDE.md`, `README.md`, plans) are not
  compiled, so editing those during a build is fine.
- **Every line stays within 80 columns** — code, comments, and the Markdown
  docs (`CLAUDE.md` included). Wrap long expressions; put a comment that would
  overflow on its own line above the code instead of trailing it. (A few legacy
  files predate this and still have long lines; hold new or edited code to 80.)
- C++17, `-Wall -Wextra -Wno-unused-parameter`, compiled with `-fwrapv` (signed
  overflow wraps — and
  is *relied upon*; don't "fix" wrap-dependent arithmetic).
- **Compiler warnings must be ADDRESSED, not SILENCED — the build stays
  warning-clean on ALL THREE CI compilers (g++, clang, MSVC).** A warning is a
  real signal; fix the ROOT CAUSE, never suppress it with a `#pragma`, a
  `-Wno-...` / `/wd####` flag, `_CRT_SECURE_NO_WARNINGS`, or a cast / `(void)`
  that hides it. When a warning looks like a false positive, make the INTENT
  EXPLICIT (which also documents it) instead of disabling the diagnostic — e.g.
  a `-Wmissing-field-initializers` on aggregate-initializing a struct with an
  anonymous union → give the struct a **constructor** that inits every member
  (done for `Builtin`); a `-Wextra` "base should be explicitly initialized in
  the copy constructor" → **name the base** in the init list (`FuncObject`'s
  copy ctor lists `RefCounted()` for "a clone owns a fresh count").
  **MSVC is stricter about narrowing than g++/clang** (silent under the project
  flags): `C4244`/`C4267` (`int_type`/`size_t` → `int`) are fixed by widening
  the target param to `int_type` where it is a slot/index (`Frame::at`, the VM's
  `write_*_slot`, `disasm`'s `reg`/`arglist`, `vm_struct_field_*` — a widening
  call is warning-free everywhere) or an explicit `static_cast<int>` at a
  one-off site; `C4996` (MSVC deprecating standard `getenv`) by a cross-platform
  wrapper (`env_get`, `_dupenv_s` on `_WIN32`), NOT `_CRT_SECURE_NO_WARNINGS`.
  After any
  change a clean `make clean && make` must show ZERO warnings on g++ AND clang
  (`make CXX=clang++`); MSVC's set is only visible on the Windows CI lane, so
  read its log after a build-touching change.
  **WINDOWS IS LLP64: `long` is 32 BITS there** (64 on Linux/macOS), so any
  `long`-typed conversion of an `int_type` value SILENTLY behaves differently
  per platform — no warning, no test failure until the Windows lane runs.
  `parser.cpp` parsed integer literals with `stol`, which made every literal
  in `[2^31, 2^63)` a "Integer literal out of range" SYNTAX ERROR on Windows
  alone (`const BIG = 4611686018427387903;` compiled everywhere else). Use
  **`stoll`** — or the `sizeof(int_type)`-dispatched `if constexpr` chain that
  `int()` (builtins/num.cpp.h) already uses, which is correct — and then
  range-check the fit into `int_type`. The reason nothing caught it: the only
  literal-range test used a value beyond `2^63`, which EVERY platform
  refuses; when testing a LIMIT, cover the range on BOTH sides of it.
- Every file starts with `/* SPDX-License-Identifier: BSD-2-Clause */`.
- Core typedefs (`defs.h`): `int_type = intptr_t`, `float_type = double`
  (printf/snprintf with `%f`/`%.*f`; the comment warns to update the format
  strings and math builtins if you change it). `double` (not `long double`)
  keeps `EvalValue` small — long double's 16-byte alignment padded it from 40
  to 48 bytes, inflating array memory traffic and every value copy — matches
  Python's float, and uses the faster double libm. `size_type = uint32_t`
  (`size_t` on MSVC).
- No third-party dependencies, ever — that's a hard design constraint of the
  project, including for the
  test harness. Don't introduce one.
- **No arbitrary copy-paste from other open-source projects.** Code must be
  **original**. Do NOT paste a snippet from a copyrighted project even when its
  license is permissive; a comment labeling code as `"<project>-style"` is
  itself a red flag that it was copied. If a piece of logic is genuinely needed
  from elsewhere, two acceptable routes: **(a)** reimplement it from scratch (a
  short algorithm — a hash mix, a formula — is best rewritten originally, or
  taken from a clearly **public-domain / CC0** reference such as SplitMix64;
  mathematical constants like 2^64/phi or a hash prime are facts, not
  copyrightable); or **(b)** bring the upstream code in *deliberately and
  legally*: first confirm its license is **compatible with our BSD-2-Clause**
  (MIT/BSD/ISC/Apache-2.0/BSL-1.0/public-domain are), then add a **`NOTICE`**
  file at the repo root naming the project, pasting its **full license text**,
  and stating **which file(s)** use it, and attribute it at the use site.
  **Evaluate (b) with the maintainer before doing it** — it is the exception,
  not the default; prefer (a).
- When implementation behavior and the README disagree, treat it as a bug to
  surface, not a silent
  choice to make — the README is the spec.
