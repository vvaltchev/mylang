#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# sabotage.sh - break an invariant on purpose, rebuild, run a check, and
#               ALWAYS restore the source.
#
#   scripts/sabotage.sh SRC_FILE OLD_TEXT NEW_TEXT [CHECK_CMD]
#
# CLAUDE.md requires that a test be WATCHED FAILING: write the test,
# reintroduce the defect, confirm it fails. Doing that by hand means
# editing a source file and remembering to put it back - and a forgotten
# restore is a sabotaged tree that looks fine until something else
# breaks. This does the edit, the build, the check and the restore, and
# restores even if the build or the check dies.
#
# OLD_TEXT must occur EXACTLY ONCE (the script refuses otherwise, since
# a sabotage applied in two places proves nothing about either).
#
# CHECK_CMD defaults to the debug suite. It is run with `sh -c`, so
# anything works:
#
#   scripts/sabotage.sh src/jit.cpp 'a && b' 'a' \
#       './build-claude/dbg/mylang -rt'
#   scripts/sabotage.sh src/codegen.cpp 'x : all;' 'x : 0;' \
#       'tests/corpus_diff.sh ./build-claude/dbg/mylang'
#
# The check is EXPECTED TO FAIL. Exit status:
#   0  the check failed  -> the test has teeth (what you want)
#   1  the check PASSED  -> the test is blind to this defect; either the
#      sabotage does not reach the code, or the test is vacuous
#   2  usage / build error

if [ $# -lt 3 ]; then
    echo "usage: $0 SRC_FILE OLD_TEXT NEW_TEXT [CHECK_CMD]" >&2
    exit 2
fi
SRC="$1"; OLD="$2"; NEW="$3"
CHECK="${4:-./build-claude/dbg/mylang -rt}"
BUILD="${SABOTAGE_BUILD:-make -j BUILD_DIR=build-claude/dbg TESTS=1 OPT=0}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
[ -f "$SRC" ] || { echo "error: no such file: $SRC" >&2; exit 2; }

BAK=$(mktemp)
cp "$SRC" "$BAK"
restore() { cp "$BAK" "$SRC"; rm -f "$BAK"; }
trap 'restore' EXIT INT TERM

OLD="$OLD" NEW="$NEW" SRC="$SRC" python3 - <<'PY' || exit 2
import os, sys
src, old, new = os.environ['SRC'], os.environ['OLD'], os.environ['NEW']
s = open(src).read()
n = s.count(old)
if n == 0:
    sys.exit("sabotage: OLD_TEXT not found in %s" % src)
if n > 1:
    sys.exit("sabotage: OLD_TEXT occurs %d times in %s - make it unique, "
             "a sabotage applied twice proves nothing about either site"
             % (n, src))
open(src, 'w').write(s.replace(old, new, 1))
PY

echo "sabotage: applied to $SRC; rebuilding..."
if ! sh -c "$BUILD" > /tmp/sabotage_build.log 2>&1; then
    echo "sabotage: BUILD FAILED (see /tmp/sabotage_build.log)" >&2
    grep -iE 'error' /tmp/sabotage_build.log | head -5 >&2
    exit 2
fi

echo "sabotage: running: $CHECK"
sh -c "$CHECK" > /tmp/sabotage_check.log 2>&1
rc=$?
restore
trap - EXIT INT TERM
echo "sabotage: restored $SRC; rebuilding clean..."
sh -c "$BUILD" > /tmp/sabotage_rebuild.log 2>&1

if [ $rc -eq 0 ]; then
    echo "RESULT: the check PASSED with the defect in place."
    echo "        The test is BLIND to it - vacuous, or the sabotage"
    echo "        never reached the code under test."
    exit 1
fi
echo "RESULT: the check FAILED (exit $rc) - the test has teeth."
echo "        Evidence in /tmp/sabotage_check.log:"
# `-rt` prints a `[ RUN ]` line per test and many test NAMES contain the
# word "fails" (they assert a rejection), so those lines are dropped -
# otherwise every run shows five irrelevant hits and the real one
# scrolls off.
grep -vE '^\[ RUN' /tmp/sabotage_check.log \
    | grep -iE 'FAIL|abort|Assertion|mismatch|VACUOUS|diverge|DIFF:|does not agree' \
    | head -5 | sed 's/^/          /'
exit 0
