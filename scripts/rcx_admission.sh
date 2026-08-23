#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# rcx_admission.sh - THE PIN-POOL ADMISSION SURVEY (#96).
#
#   scripts/rcx_admission.sh [REG]        (REG defaults to 1 = rcx)
#
# Builds a probe binary with REG placed FIRST in XCACHE_ORDER and with
# Emitter::scratch() REPORTING instead of aborting, then reports every
# site that would clobber a pin, with a COUNT and a SOURCE LINE. One run
# gives the whole worklist instead of one abort at a time.
#
# ⛔ IT DRIVES BOTH `-rt` AND THE FILE CORPUS, AND THAT IS THE POINT.
# The first version ran `-vdj` over bench/my + samples + tests/functional
# only. It reported ZERO conflicts for rcx while `./mylang -rt` still
# aborted and corpus_diff answered 21/23 - because `-rt`'s programs are
# built and run IN PROCESS and never appear as a file. A survey whose
# corpus is not the test corpus reports "clean" for the shapes it cannot
# see, which is the most expensive possible wrong answer from an
# admission test: it says a register is safe to pin.
#
# Same family as CLAUDE.md's "an oracle's corpus hole is a test hole".
#
# ⛔⛔ AN EMPTY WORKLIST IS NECESSARY, NOT SUFFICIENT - AND THAT IS NOT A
# CAVEAT, IT IS THE SECOND THING THIS SCRIPT GOT WRONG (2026-08-20).
#
# It can only see a clobber that was DECLARED, i.e. one whose emitter calls
# Emitter::scratch(). A site that simply WRITES the register - a ROLE with a
# fixed default, like ElemRead's `data = RCX` in the nested-read tiers -
# declares nothing and is invisible here. rcx reached a ZERO worklist and was
# still not admissible: 4 `-rt` JIT cases failed and two corpus programs
# printed WRONG ANSWERS (16_elem2_fused, 17_elem2_divmod_roles).
#
# Same family as the corpus hole above, one level in: the first version
# measured the wrong PROGRAMS, this one measures the wrong THING - declared
# intent instead of actual writes.
#
# So the script does not stop at the worklist. When it is empty it BUILDS THE
# REGISTER IN and runs the real nets, and only THAT exiting clean means the
# register can be admitted.
set -e
REG=${1:-1}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cp src/jit.cpp "$TMP/jit.cpp.orig"
python3 - "$REG" <<'PY'
import re, sys
reg = sys.argv[1]
p = "src/jit.cpp"; s = open(p).read()
# rcx (1) LANDED as pin 12 on 2026-08-20, so the pool string this
# rewrites moved. If the replace ever stops matching the script goes
# VACUOUS (the probe would test the unmodified pool) - hence the
# assert instead of a silent no-op.
old = "static const uint8_t XCACHE_ORDER[] = { 10, 11, 8, 7, 6, 9, 2, 1 };"
assert s.count(old) == 1, "XCACHE_ORDER moved - update rcx_admission.sh"
s = s.replace(old, ("static const uint8_t XCACHE_ORDER[] = "
                    "{ %s, 10, 11, 8, 7, 6, 9, 2, 1 };" % reg))
# __builtin_LINE() as a DEFAULT ARGUMENT gives the CALLER's line on gcc
# and clang - the C++17 stand-in for std::source_location, and what turns
# a conflict COUNT into a worklist.
s = s.replace("    void scratch(uint8_t r) const\n    {",
              "    void scratch(uint8_t r, int line = __builtin_LINE()) const\n    {")
s = s.replace('''        ML_CHECK_MSG(!reg_holds_pin(r),
                     "a raw-scratch emitter is about to clobber a "
                     "PINNED register - it must be excluded from the "
                     "pin pool for this run (see XCACHE_ORDER)");''',
              '''        if (reg_holds_pin(r))
            fprintf(stderr, "SCRATCH-PIN reg=%u line=%d\\n", r, line);''')
s = s.replace("void scratch2(uint8_t a, uint8_t b) const { scratch(a); scratch(b); }",
              "void scratch2(uint8_t a, uint8_t b, int line = __builtin_LINE())"
              " const { scratch(a, line); scratch(b, line); }")
open(p, "w").write(s)
PY
make -j BUILD_DIR=build-claude/adm TESTS=1 OPT=0 > "$TMP/build.log" 2>&1 || {
    echo "error: probe build failed - see $TMP/build.log" >&2
    cp "$TMP/jit.cpp.orig" src/jit.cpp; exit 2; }
cp "$TMP/jit.cpp.orig" src/jit.cpp
BIN=build-claude/adm/mylang
LOG=$TMP/hits.log
: > "$LOG"
# (1) the -rt suite, whose programs exist only in process
"$BIN" -rt >/dev/null 2>>"$LOG" || true
# (2) the file corpus
fails=0
for f in bench/my/*.my samples/* tests/functional/*.my; do
    [ -f "$f" ] || continue
    "$BIN" -vdj "$f" >/dev/null 2>>"$LOG" || fails=$((fails + 1))
done
# grep -c PRINTS 0 and EXITS 1 when there are no matches, so a
# `|| echo 0` appends a SECOND zero and every later [ ] test dies on
# "Illegal number". Take the count and default it instead.
n=$(grep -c SCRATCH-PIN "$LOG" 2>/dev/null) || true
n=${n:-0}
echo "pin-pool admission survey for register $REG"
echo "  conflicts: $n     corpus programs that failed to compile: $fails"
if [ "$n" -gt 0 ]; then
    echo "  worklist (count, source line - the emitter that clobbers a pin):"
    grep SCRATCH-PIN "$LOG" | sort | uniq -c | sort -rn | sed 's/^/    /'
fi
if [ "$n" -ne 0 ] || [ "$fails" -ne 0 ]; then
    echo "  => NOT admissible: work the list above first."
    exit 1
fi

# THE WORKLIST IS EMPTY. That says every DECLARED clobber is handled; it says
# nothing about an undeclared one, so now actually admit the register and run
# the nets that would notice.
echo "  worklist empty - now testing the register ADMITTED for real"
python3 - "$REG" <<'PY'
import sys
reg = sys.argv[1]
p = "src/jit.cpp"; s = open(p).read()
# rcx (1) LANDED as pin 12 on 2026-08-20, so the pool string this
# rewrites moved. If the replace ever stops matching the script goes
# VACUOUS (the probe would test the unmodified pool) - hence the
# assert instead of a silent no-op.
old = "static const uint8_t XCACHE_ORDER[] = { 10, 11, 8, 7, 6, 9, 2, 1 };"
assert s.count(old) == 1, "XCACHE_ORDER moved - update rcx_admission.sh"
s = s.replace(old, ("static const uint8_t XCACHE_ORDER[] = "
                    "{ %s, 10, 11, 8, 7, 6, 9, 2, 1 };" % reg))
open(p, "w").write(s)
PY
make -j BUILD_DIR=build-claude/adm-real TESTS=1 OPT=0 > "$TMP/b2.log" 2>&1 || {
    echo "error: admitted build failed - see $TMP/b2.log" >&2
    cp "$TMP/jit.cpp.orig" src/jit.cpp; exit 2; }
cp "$TMP/jit.cpp.orig" src/jit.cpp
rc=0
build-claude/adm-real/mylang -rt > "$TMP/rt.log" 2>&1 || rc=1
echo "  -rt         : $(grep -oE 'Tests passed: [0-9/]+ \[ [A-Z]+ \]' "$TMP/rt.log" \
                       || echo 'ABORTED')"
cd_out=$(tests/corpus_diff.sh build-claude/adm-real/mylang 2>&1 | tail -1)
echo "  corpus_diff :$cd_out"
echo "$cd_out" | grep -q "agree" || rc=1
echo "$cd_out" | grep -qE "^ *plain +([0-9]+)/\1 agree" || rc=1
if [ "$rc" -ne 0 ]; then
    echo "  => NOT admissible: the worklist is empty but the nets FAIL, so a"
    echo "     site writes the register WITHOUT declaring it (see the note at"
    echo "     the top of this file). Find it in the failing programs."
    exit 1
fi
echo "  => register $REG is ADMISSIBLE (also run --xrot before landing it)"
exit 0
