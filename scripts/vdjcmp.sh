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

#   5. ⛔ A BAKED ADDRESS IS NOT ALWAYS SPELLED IN HEX. #96 step 3 put
#      the Type singletons in a low-address arena so a tag encodes as an
#      imm32 - and the disassembler prints an imm32 in DECIMAL:
#
#         mov r2.type, 1095139376      vs      mov r2.type, 1093046320
#
#      Same instruction, different arena mmap. The hex rules above
#      cannot see it, so from #96 step 3 until 2026-08-17 this script
#      reported 0 identical / 108 differing for ANY pair of separately
#      linked binaries - i.e. it was not an oracle at all, it was a
#      constant "everything changed". The rule below masks a decimal
#      immediate large enough to be a pointer (>= 7 digits); a real
#      immediate operand in emitted code is a small constant, a slot
#      offset or a length, none of which reach that magnitude.
#
#      The general lesson, and it is the same one that produced the
#      script: WHEN A NORMALISER STOPS MATCHING, IT FAILS LOUD IN THE
#      "everything differs" DIRECTION, WHICH READS EXACTLY LIKE A
#      CATASTROPHIC CHANGE - so a 0-identical result is a reason to read
#      one diff, never a reason to believe the change broke everything.
#
# Order matters - see note 3 above.
NORM='s/(call +)[-+]?0x[0-9a-f]+/\1REL/g; s/0x[0-9a-f]{6,}/0xADDR/g;'
NORM="$NORM"' s/\b[0-9]{7,}\b/DEC_ADDR/g; s/  +/ /g'

# ⛔ RUN WITH ASLR OFF, AND VERIFY IT BELOW. Masking cannot reach every
# baked address: an address the disassembler MIS-DECODES is printed as
# individual `.byte 0xe0` / `nop` lines, with no 0x-prefixed token and
# no long decimal for a rule to match. 31 of 108 programs differed from
# THEMSELVES that way. `setarch -R` fixes the mmap layout, so the arena
# and every helper land at the same address in both runs and the whole
# question disappears - strictly better than a cleverer regex, because
# it removes the nondeterminism instead of hiding it.
if command -v setarch >/dev/null 2>&1 && setarch -R true >/dev/null 2>&1
then
    NOASLR="setarch -R"
else
    NOASLR=""
    echo "warning: setarch -R unavailable - ASLR noise may cause" >&2
    echo "         spurious DIFFs; check the self-test result below." >&2
fi

# SELF-TEST: the same binary twice must be 100% identical. Without this
# the script cannot tell "your change altered the code" from "the
# normalisation stopped covering something", and it reported the second
# as the first for weeks (see note 5).
"$A" -vdj samples/gcd 2>/dev/null | sed -E "$NORM" > "$TMP/s1"
$NOASLR "$A" -vdj samples/gcd 2>/dev/null | sed -E "$NORM" > "$TMP/s2"
$NOASLR "$A" -vdj samples/gcd 2>/dev/null | sed -E "$NORM" > "$TMP/s3"
if ! cmp -s "$TMP/s2" "$TMP/s3"; then
    echo "error: '$A' does not disassemble DETERMINISTICALLY even with" >&2
    echo "       ASLR off - the normalisation has a hole, and every" >&2
    echo "       result below would be meaningless. Fix that first." >&2
    exit 2
fi

same=0
diffs=0
for f in bench/my/*.my samples/* tests/functional/*.my; do
    [ -f "$f" ] || continue
    $NOASLR "$A" -vdj "$f" 2>/dev/null | sed -E "$NORM" > "$TMP/a"
    $NOASLR "$B" -vdj "$f" 2>/dev/null | sed -E "$NORM" > "$TMP/b"
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
