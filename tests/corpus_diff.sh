#!/bin/bash
#
# The CORPUS DIFFERENTIAL: tree-walker vs the default engine (VM + JIT +
# splice) over every bench/my and samples program.
#
# WHY THIS EXISTS, beyond -rt and the fuzzer: a JIT register bug needs a
# program shape, a runtime state AND an emitter decision to coincide,
# and -rt varies only the first (weakly - hand-written tests have few
# locals, so their temps rarely collide). Two bugs in one day were green
# under -rt (1715 tests, 5 modes) AND the 400-program fuzzer, and BOTH
# showed here immediately: a real `main` reuses its low temps for argv/
# print AND for the hot loop, which is exactly the collision.
#
#   tests/corpus_diff.sh [binary]                 - the plain differential
#   tests/corpus_diff.sh [binary] --levers        - ALSO once per JIT
#                                                   lever disabled
#
# The --levers matrix is the localisation half: when the plain run
# diverges, it says WHICH lever owns it in one command instead of a
# bisect over a 3000-line disassembly. (Its catch power over the plain
# run is small - the plain run already compares against a JIT-free
# oracle - but a bug needing two levers in combination shows only here.)
#
# Excluded: rand_sort (uses rand()), shopping/phonebook (read stdin and
# re-print their menu forever on EOF - see the CLAUDE.md note).
set -u
cd "$(dirname "$0")/.."
BIN=${1:-build-claude/dbg/mylang}
LEVERS="cache fcache telide fread flit fwd ffwd resreg hoist hoist2"

run_one() {                       # $1 = MYLANG_JIT_OFF value ("" = none)
  local bad=0 n=0 a b
  for f in bench/my/*.my samples/*; do
    [ -f "$f" ] || continue
    case "$f" in *rand_sort*|*shopping*|*phonebook*) continue ;; esac
    a=$(timeout 120 "$BIN" -tw "$f" 1 2>&1 | tail -3)
    b=$(MYLANG_JIT_OFF="$1" timeout 120 "$BIN" "$f" 1 2>&1 | tail -3)
    n=$((n + 1))
    if [ "$a" != "$b" ]; then
      echo "DIFF [$1] $f"; echo "  tw : $a"; echo "  jit: $b"; bad=$((bad + 1))
    fi
  done
  printf "  %-10s %d/%d agree\n" "${1:-<none>}" "$((n - bad))" "$n"
  return $bad
}

rc=0
run_one "" || rc=1
if [ "${2:-}" = "--levers" ]; then
  for L in $LEVERS all; do run_one "$L" || rc=1; done
fi
exit $rc
