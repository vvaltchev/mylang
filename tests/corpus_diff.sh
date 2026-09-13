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
# --xrot exists because take_reg hands out the caller-saved pin pool in
# PREFERENCE order, so its last member is reached only by a run with the
# maximum pin count - and an unsafe register can therefore sit in the
# pool exercised by nothing. r9 did, for a day, as a wrong answer
# (939f5a9 .. 2026-08-17). Rotating puts every member in the
# first-choice seat.
#
# Excluded from samples/: rand_sort (rand()), shopping/phonebook (they
# read stdin and reprint their menu forever on EOF).
set -u
cd "$(dirname "$0")/.."
BIN=${1:-build-claude/dbg/mylang}
MODE=${2:-}
# ⛔ KEEP IN SYNC WITH jit_lever_names (jit.cpp) - this list went stale
# once (#101 found argfuse/xcache/scache/rshare missing, so those four
# levers' per-lever-off configs were tested by nothing but `all`).
LEVERS="cache fcache telide fread flit fwd ffwd resreg hoist hoist2 \
mfact cest relent norec argfuse xcache scache rshare peep"
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
# `mylang -v` reports `jit_pins N (int pin budget; xcache M ...)`; M is
# what XROT rotates. A binary that does not report it (an older build,
# a non-JIT platform) falls back to the literal and SAYS SO.
XCW=$("$BIN" -v 2>/dev/null | sed -n 's/.*xcache \([0-9][0-9]*\).*/\1/p')
if [ -n "$XCW" ] && [ "$XCW" -gt 0 ] 2>/dev/null; then
  XROTS=$(seq 0 $((XCW - 1)) | tr '\n' ' ')
else
  echo "warning: '$BIN' does not report its xcache width; --xrot is" >&2
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

run_one() {                      # $1 = env assignment ("" = plain)
  local bad=0 n=0 a b
  for f in $(progs); do
    a=$(timeout 60 "$BIN" -tw "$f" 2>&1 | tail -3)
    b=$(env $1 timeout 60 "$BIN" "$f" 2>&1 | tail -3)
    n=$((n + 1))
    if [ "$a" != "$b" ]; then
      echo "DIFF [${1:-plain}] $f"; echo "  tw : $a"; echo "  jit: $b"
      bad=$((bad + 1))
    fi
  done
  printf "  %-28s %d/%d agree\n" "${1:-plain}" "$((n - bad))" "$n"
  return $bad
}

rc=0
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
