#!/bin/bash
#
# THE FUNCTIONAL DIFFERENTIAL: tree-walker vs the default engine (VM +
# JIT + splice) over tests/functional and samples/.
#
# ⛔ IT DOES NOT RUN bench/. Benchmarks measure THROUGHPUT - millions of
# iterations - and are not functional tests; using them as one makes a
# correctness run take minutes for no extra coverage, because a JIT bug
# shows on iteration 1 or not at all. tests/functional/ exists for this:
# tiny, self-asserting programs that construct the hazard SHAPES on
# purpose instead of hoping a benchmark happens to contain them.
#
# WHY IT EXISTS at all: a JIT register bug needs a program shape, a
# runtime state AND an emitter decision to coincide, and -rt varies only
# the first (hand-written tests have few locals, so their temps rarely
# collide with reference-producing code). Two such bugs were green under
# -rt AND the 400-program fuzzer.
#
#   tests/corpus_diff.sh [binary]            - the differential
#   tests/corpus_diff.sh [binary] --levers   - once per JIT lever off
#   tests/corpus_diff.sh [binary] --cold     - once per forced cold tier
#   tests/corpus_diff.sh [binary] --xrot     - once per pin-pool rotation
#   tests/corpus_diff.sh [binary] --nolowmem - with the low-address arena
#                                              REFUSED (the imm32 tags fall
#                                              back to registers)
#
# --nolowmem exists because the JIT emits MATERIALLY different code when
# ml_lowmem_fits_imm32 is false: every type tag becomes a register read
# instead of an imm32, which also decides whether rsi/r8 may hold a pin
# at all. That configuration ships - lowmem.h says in bold that a failed
# MAP_32BIT is reachable on Linux - and until MYLANG_NO_LOWMEM existed
# NO build and NO lane could enter it. Its first run found 7 corpus
# programs crashing or answering wrongly: store_dst_bool named a
# fallback register for the t_bool tag and the movabs that filled it had
# been deleted as "an instruction saved" when the imm32 form landed.
#
# --xrot exists because the allocator hands out registers in
# PREFERENCE order, so its last member is reached only by a run with the
# maximum pin count - and an unsafe register can therefore sit in the
# pool exercised by nothing. r9 did, for a day, as a wrong answer
# (939f5a9 .. 2026-08-17). Rotating puts every member in the
# first-choice seat.
#
# Excluded from samples/: rand_sort (rand()), shopping/phonebook (they
# read stdin and reprint their menu forever on EOF).
set -u

# ⛔ RESOLVE $BIN *BEFORE* THE cd, AND PROVE IT RUNS.
#
# This script cd's to the repo root, so a RELATIVE binary path given by
# the caller is silently re-anchored - and CI passes exactly that:
# `corpus_diff.sh ./mylang` with working-directory `build/`. After the
# cd, `./mylang` is <repo>/mylang, which does not exist.
#
# ⛔ THAT MADE THE WHOLE Nets DIFFERENTIAL VACUOUS, and it could not
# have announced itself: run_one compares the tree-walker's output with
# the default engine's, and when NEITHER binary runs both sides are the
# same "No such file or directory" - so the comparison AGREED, printed
# `33/33 agree`, and exited 0. An oracle whose two sides fail
# identically reports success. It was the compile gate - the first
# check here that reads an EXIT CODE rather than diffing two outputs -
# that exposed it.
#
# Both halves are needed: resolving the path fixes it, and the
# executable test is what stops it ever being silent again.
_bin_arg=${1:-build-claude/dbg/mylang}
case "$_bin_arg" in
  /*) BIN=$_bin_arg ;;
  *)  BIN=$PWD/$_bin_arg ;;
esac
cd "$(dirname "$0")/.."
if [ ! -x "$BIN" ]; then
    echo "corpus_diff.sh: '$_bin_arg' is not an executable ($BIN)." >&2
    echo "  This script cd's to the repo root, so a relative path is" >&2
    echo "  resolved against your CURRENT directory, not the root." >&2
    exit 2
fi
MODE=${2:-}
# ⛔ KEEP IN SYNC WITH jit_lever_names (jit.cpp) - this list went stale
# once (#101 found argfuse/xcache/scache/rshare missing, so those four
# levers' per-lever-off configs were tested by nothing but `all`).
# `lsra` joined 2026-08-31 (#103 A'): it selects the LEGACY pick over
# the linear scan, i.e. a different PIN SET for the same program.  It
# was an env var (MYLANG_JIT_LSRA=0) and NOT a lever, so this matrix -
# the one CI runs - never covered it, and a whole second allocator was
# exercised by four hand-written -rt cases and nothing else.
#
# NOT VACUOUS, measured when it was added: 21 of the 35 programs below
# emit DIFFERENT machine code under the two allocators (compare
# `MYLANG_VDJ_ADDRS=0 mylang -vdj` with and without the lever).  So
# this row buys 21 programs' worth of alternative (slot -> register)
# assignment - the axis the r9 bug lived in, where a hot local landing
# in a register that was also raw scratch was a WRONG ANSWER that only
# appeared for the programs whose pin set happened to reach it.
# Re-measure that ratio if it ever looks like this row is free.
LEVERS="cache fcache telide fread flit fwd ffwd resreg hoist hoist2 \
mfact cest relent norec argfuse xcache scache rshare peep bakecallee \
lsra"
COLD_TIERS="refstore"
# ⛔ DERIVED FROM THE BINARY, NOT HARDCODED (2026-08-18). This was
# `XROTS="0 1 2 3"`, a literal, and the whole point of the mode is that
# `take_reg` scans the pool in preference order so its LAST member gets
# first-choice traffic from nothing - which is how an unsafe r9 sat
# there for a day as a shipping wrong answer. A hardcoded rotation
# count re-creates exactly that blind spot one level up: admit a fifth
# caller-saved register and the sweep silently stops covering the
# newest, least-exercised member, while still printing PASS.
#
# It got lucky once already - the literal covered 0..3 while the pool
# held THREE, so rotation 3 was a duplicate of 0; when rdi joined the
# count happened to be right. Not a property to rely on twice.
#
# `mylang -v` reports `jit_pins N (int pin budget; M xrot rotations)`;
# M is XROT's period. #123: it used to be the caller-saved ARRAY's
# length, because the rotation permuted that array - the rotation now
# moves the start of RegAlloc::take's scan over the whole register
# file, so M is 16 and the sweep covers the FLOAT file too. A binary
# that does not report it (an older build, a non-JIT platform) falls
# back to the literal and SAYS SO.
XCW=$("$BIN" -v 2>/dev/null | sed -n 's/.*; \([0-9][0-9]*\) xrot.*/\1/p')
if [ -n "$XCW" ] && [ "$XCW" -gt 0 ] 2>/dev/null; then
  XROTS=$(seq 0 $((XCW - 1)) | tr '\n' ' ')
else
  echo "warning: '$BIN' does not report its xrot width; --xrot is" >&2
  echo "         falling back to a fixed 0..3 sweep, which may NOT" >&2
  echo "         cover every pool member." >&2
  XROTS="0 1 2 3"
fi

progs() {
  ls tests/functional/*.my 2>/dev/null
  for f in samples/*; do
    [ -f "$f" ] || continue
    case "$f" in *rand_sort*|*shopping*|*phonebook*) continue ;; esac
    echo "$f"
  done
}

# ⛔ A PROGRAM THAT STOPS COMPILING "AGREES" WITH ITSELF.
#
# This is the `tail -3` lesson in a new shape (see run_one below): the
# comparison is between two ENGINES, so a COMPILE-TIME refusal - which
# happens before either engine runs and prints the identical message on
# both sides - reads as perfect agreement. Watched: reopening #115's
# multi-site hole made tests/functional/25_factory_closure_param.my
# fail to compile at all, and this script still reported 33/33.
#
# `-nr` is the discriminator ("compile and validate, don't run": lex,
# parse, infer AND run_optimizers, exit 1 on a refusal). Exit CODE
# alone would not do - samples/gcd legitimately exits 1 with a usage
# message when given no arguments.
#
# A comparison oracle must compare everything it claims to; this one
# claims the corpus RUNS.
compiles_all() {
  local bad=0 n=0
  for f in $(progs); do
    n=$((n + 1))
    if ! out=$(timeout 60 "$BIN" -nr "$f" 2>&1); then
      echo "REFUSED $f"
      printf '%s\n' "$out" | head -4 | sed 's/^/  /'
      bad=$((bad + 1))
    fi
  done
  printf "  %-28s %d/%d compile\n" "compile gate" "$((n - bad))" "$n"
  return $bad
}

run_one() {                      # $1 = env assignment ("" = plain)
  local bad=0 n=0 a b
  for f in $(progs); do
    # ⛔ COMPARE THE WHOLE OUTPUT. This used to pipe both sides through
    # `tail -3`, so a divergence anywhere but a program's last three
    # lines was INVISIBLE - the tool answered "28/28 agree" for a
    # binary that printed `1 0` where the tree-walker printed
    # `true false`, because the offending lines were at the top. The
    # truncation was only ever meant to keep the DIFF MESSAGE short,
    # which is what the diff below does instead: compare everything,
    # print the first few DIFFERING lines (more useful than the tail).
    a=$(timeout 60 "$BIN" -tw "$f" 2>&1)
    b=$(env $1 timeout 60 "$BIN" "$f" 2>&1)
    n=$((n + 1))
    if [ "$a" != "$b" ]; then
      echo "DIFF [${1:-plain}] $f"
      diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") \
        | head -8 | sed 's/^/  /'
      bad=$((bad + 1))
    fi
  done
  printf "  %-28s %d/%d agree\n" "${1:-plain}" "$((n - bad))" "$n"
  return $bad
}

rc=0
compiles_all || rc=1
run_one "" || rc=1
case "$MODE" in
  --levers)
    for L in $LEVERS all; do run_one "MYLANG_JIT_OFF=$L" || rc=1; done ;;
  --cold)
    for T in $COLD_TIERS all; do run_one "MYLANG_JIT_COLD=$T" || rc=1; done ;;
  --xrot)
    for K in $XROTS; do run_one "MYLANG_JIT_XROT=$K" || rc=1; done ;;
  --nolowmem)
    run_one "MYLANG_NO_LOWMEM=1" || rc=1 ;;
esac
exit $rc
