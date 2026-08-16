#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# vdjcmp.sh - compare the EMITTED NATIVE CODE of two mylang binaries
#             across the whole corpus (bench/my + samples + functional).
#
#   scripts/vdjcmp.sh OLD_BINARY NEW_BINARY [-v]
#
# This is the oracle for "my change to the JIT is a pure restructuring":
# if every program's `-vdj` disassembly is unchanged, the change cannot
# have altered what the machine does, and no amount of test-passing is
# needed to argue it. It is far cheaper and far stronger than reasoning,
# so REACH FOR IT FIRST on any emitter refactor.
#
# ⛔ WHY THE NORMALISATION IS NOT OPTIONAL - it cost three false results
# in one session, each of which looked like "my change broke everything":
#
#   1. Two separately-LINKED binaries bake DIFFERENT absolute addresses
#      (`movabs rsi, 0x5960...` vs `0x640c...`) for the same helper, and
#      different rel32 displacements for the same call. Unmasked, 108 of
#      108 programs "differ".
#
#   2. A displacement is signed EITHER WAY - `call +0x7cb` and
#      `call -0xbfbcc` both occur. A rule matching only `-0x...` leaves
#      every forward call looking changed.
#
#   3. The CALL rule must run BEFORE the address-width rule. Under ASLR
#      40_math_builtins' libm displacement is sometimes 5 hex digits and
#      sometimes 6, so a `0x[0-9a-f]{6,}` rule applied first masks it on
#      some runs and not others - and the file appears to differ from
#      ITSELF at random. (Verified: 6 runs of one binary, 2 mismatched.)
#
#   4. Masking changes token WIDTH, so the disassembler's column padding
#      shifts. Squeeze runs of spaces or you get pure-whitespace diffs.
#
# Both binaries should be built the SAME way - VM_HARDENING and ASSERTS
# change the emitted code (a hardened build emits jit_ret_audit calls),
# so a debug-vs-release comparison reports real but irrelevant deltas.
#
# Exit status: 0 when every program matches, 1 otherwise.

if [ $# -lt 2 ]; then
    echo "usage: $0 OLD_BINARY NEW_BINARY [-v]" >&2
    echo "  -v   also print each differing program's diff" >&2
    exit 2
fi
A="$1"; B="$2"; VERBOSE="$3"

for bin in "$A" "$B"; do
    if [ ! -x "$bin" ]; then
        echo "error: '$bin' is not an executable" >&2
        exit 2
    fi
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Order matters - see note 3 above.
NORM='s/(call +)[-+]?0x[0-9a-f]+/\1REL/g; s/0x[0-9a-f]{6,}/0xADDR/g; s/  +/ /g'

same=0
diffs=0
for f in bench/my/*.my samples/* tests/functional/*.my; do
    [ -f "$f" ] || continue
    "$A" -vdj "$f" 2>/dev/null | sed -E "$NORM" > "$TMP/a"
    "$B" -vdj "$f" 2>/dev/null | sed -E "$NORM" > "$TMP/b"
    if cmp -s "$TMP/a" "$TMP/b"; then
        same=$((same + 1))
    else
        diffs=$((diffs + 1))
        echo "DIFF: $f"
        if [ "$VERBOSE" = "-v" ]; then
            diff "$TMP/a" "$TMP/b" | sed 's/^/    /'
        fi
    fi
done

echo "identical: $same   differing: $diffs"
[ "$diffs" -eq 0 ]
