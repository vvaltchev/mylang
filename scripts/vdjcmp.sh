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
# ⛔ THIS SCRIPT USED TO NORMALISE ITS INPUT WITH REGEXES. IT NO LONGER
# DOES, AND THE HISTORY IS THE POINT (2026-08-17).
#
# `-vdj` bakes absolute addresses - Type singletons, helper entry
# points, rel32 call displacements - whose values move with every
# process under ASLR. This script used to mask them with a sed pipeline
# and run both binaries under `setarch -R`. Both were WORKAROUNDS FOR A
# DEFECT IN THE DISASSEMBLER, and they failed exactly the way
# workarounds do:
#
#   - the masks were written against the spellings that existed when
#     they were written. #96 step 3 moved the Type singletons into a
#     low-address arena so a tag encodes as an imm32, which the
#     disassembler prints in DECIMAL - and every rule was hex-only. From
#     that day the script reported 0 identical / 108 differing for ANY
#     pair of separately linked binaries. It was not an oracle at all;
#     it was a constant "everything changed", and nobody noticed because
#     that is also what a broken change looks like;
#   - a mis-decoded instruction emerged as bare `.byte 0xe0` / `nop`
#     lines with no maskable token whatsoever, so 31 of 108 programs
#     differed from THEMSELVES. No regex can reach that.
#
# The fix went into `disasm.cpp`, where it belonged: a baked address now
# prints as `<addr>` / `<int-tag>` / `<helper>` (numerically only under
# MYLANG_VDJ_ADDRS=1), and the six opcodes and the SIB-decoding bug that
# produced the `.byte` debris are fixed. `-vdj` is now byte-for-byte
# REPRODUCIBLE with no help from this script - verified across the whole
# corpus - so this file is a plain `cmp` again.
#
# THE RULE: fix the tool, not the script that reads it. A workaround in
# a consumer leaves the tool broken for every OTHER consumer, and then
# rots silently the next time the tool's output changes shape.
#
# Both binaries should be built the SAME way - VM_HARDENING and ASSERTS
# change the emitted code (a hardened build emits jit_ret_audit calls),
# so a debug-vs-release comparison reports real but irrelevant deltas.
#
# Exit status: 0 when every program matches, 1 otherwise, 2 on a setup
# error (including a failed self-test).

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

# SELF-TEST, and it is NOT ceremony: the same binary disassembling the
# same program twice must be byte-identical. Without this the script
# cannot tell "your change altered the emitted code" from "the
# disassembler went non-deterministic again", and for weeks it reported
# the second as the first. Two runs of A, on a program with a float
# chain and a call so the dump covers baked tags AND a rel32 helper.
"$A" -vdj samples/gcd > "$TMP/s1" 2>/dev/null
"$A" -vdj samples/gcd > "$TMP/s2" 2>/dev/null
if ! cmp -s "$TMP/s1" "$TMP/s2"; then
    echo "error: '$A' does not disassemble DETERMINISTICALLY - the same" >&2
    echo "       binary gave two different dumps for samples/gcd. Every" >&2
    echo "       result below would be meaningless; fix the" >&2
    echo "       disassembler (see the note at the top of this file)." >&2
    exit 2
fi

same=0
diffs=0
for f in bench/my/*.my samples/* tests/functional/*.my; do
    [ -f "$f" ] || continue
    "$A" -vdj "$f" > "$TMP/a" 2>/dev/null
    "$B" -vdj "$f" > "$TMP/b" 2>/dev/null
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
