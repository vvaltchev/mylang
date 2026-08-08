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
#
# Excluded from samples/: rand_sort (rand()), shopping/phonebook (they
# read stdin and reprint their menu forever on EOF).
set -u
cd "$(dirname "$0")/.."
BIN=${1:-build-claude/dbg/mylang}
MODE=${2:-}
LEVERS="cache fcache telide fread flit fwd ffwd resreg hoist hoist2 mfact cest relent"
COLD_TIERS="refstore"

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
esac
exit $rc
