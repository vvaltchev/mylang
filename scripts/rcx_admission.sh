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
# Exit 0 when the worklist is EMPTY (and then, and only then, run the
# real nets: -rt, corpus_diff, corpus_diff --xrot with REG admitted).
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
s = s.replace("static const uint8_t XCACHE_ORDER[] = { 10, 11, 8, 7, 6, 9, 2 };",
              "static const uint8_t XCACHE_ORDER[] = { %s, 10, 11, 8, 7, 6, 9, 2 };" % reg)
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
n=$(grep -c SCRATCH-PIN "$LOG" 2>/dev/null || echo 0)
echo "pin-pool admission survey for register $REG"
echo "  conflicts: $n     corpus programs that failed to compile: $fails"
if [ "$n" -gt 0 ]; then
    echo "  worklist (count, source line - the emitter that clobbers a pin):"
    grep SCRATCH-PIN "$LOG" | sort | uniq -c | sort -rn | sed 's/^/    /'
fi
[ "$n" -eq 0 ] && [ "$fails" -eq 0 ]
