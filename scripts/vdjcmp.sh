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
    echo "usage: $0 OLD_BINARY NEW_BINARY [-v] [--same-shape]" >&2
    echo "  -v            also print each differing program's diff" >&2
    echo "  --same-shape  compare the INSTRUCTION STREAM, allowing" >&2
    echo "                encoding LENGTHS to differ; reports the" >&2
    echo "                byte delta (see the note below)" >&2
    exit 2
fi
A="$1"; B="$2"; shift 2
VERBOSE=""; SHAPE=""
for arg in "$@"; do
    case "$arg" in
    -v)           VERBOSE="-v" ;;
    --same-shape) SHAPE="1" ;;
    *) echo "error: unknown option '$arg'" >&2; exit 2 ;;
    esac
done

#
# --same-shape: THE WEAKER ORACLE, FOR A CHANGE THAT ALTERS INSTRUCTION
# LENGTH BUT NOT MEANING (2026-08-19).
#
# The default `cmp` is the strong claim - "my change cannot have
# altered what the machine does" - and it is the one to reach for. But
# some changes legitimately alter LENGTH: converting a hand-encoded
# `mov rcx, [rax+disp32]` to the load_base encoder gets the disp8 form
# for a small offset, and every later jump displacement shifts with it.
# Byte identity then reports every such program as differing and says
# nothing about whether the INSTRUCTIONS are the same.
#
# This mode normalises exactly two things and nothing else:
#   - the `+ NN:` instruction-offset column, and
#   - a relative branch target that is the WHOLE operand (`je +107`),
#   - a `frag@+NNNN` fragment BASE offset, which shifts for every
#     fragment placed after one that changed size. Found the hard way:
#     the first run of this mode reported 39 programs as "instruction
#     stream, not just lengths" on nothing but these.
# Registers, memory displacements, immediates, tags and helper names
# are all still compared, so a wrong register or a wrong offset still
# fails. It also reports the total byte delta, which is the point of
# making the change at all.
#
# ⛔ IT IS STRICTLY WEAKER, so say which mode a result came from. A
# change that reorders two independent instructions passes `cmp` never
# and passes this never either (the stream differs); a change that
# swaps a register passes neither. What this mode CANNOT see is a
# branch that now goes somewhere else - the displacement is exactly
# what it erases. Pair it with the objdump oracle and the corpus
# differential, which is what the length-changing batches did.
#
shape_norm() {
    sed -e 's/^\( *\. *\)+ *[0-9][0-9]*:/\1+ N:/' \
        -e 's/frag@+[0-9][0-9]*/frag@+N/g' \
        -e 's/^\(.*[[:space:]]\(jmp\|je\|jne\|jl\|jle\|jg\|jge\|jb\|jbe\|ja\|jae\|js\|jns\|jo\|jno\|jp\|jnp\|loop\) \)[+-][0-9][0-9]*$/\1<rel>/'
}
last_off() {
    sed -n 's/^ *\. *+ *\([0-9][0-9]*\):.*/\1/p' "$1" | tail -1
}

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

#
# ⛔ THE --same-shape NORMALISER MUST BE PROVED NON-VACUOUS, because a
# normaliser is exactly the shape that silently erases the difference
# it was meant to keep (this repo's own history: a sed pipeline that
# masked so much it reported every program as differing, and one that
# masked a real mis-decode). A regex one character too greedy turns
# this mode into "everything matches", which reads as a clean run.
#
# So: feed it a pair that differs ONLY in a register, and in the
# operand position the normaliser touches, and require it to SAY SO.
#
if [ -n "$SHAPE" ]; then
    printf '%s\n' '       .   +  4: mov rcx, [rax+0x8]' \
                   '       .   + 11: je +107' > "$TMP/nv1"
    printf '%s\n' '       .   +  7: mov rdx, [rax+0x8]' \
                   '       .   + 14: je +203' > "$TMP/nv2"
    shape_norm < "$TMP/nv1" > "$TMP/nv1n"
    shape_norm < "$TMP/nv2" > "$TMP/nv2n"
    if cmp -s "$TMP/nv1n" "$TMP/nv2n"; then
        echo "error: the --same-shape normaliser is VACUOUS - it" >&2
        echo "       reported 'mov rcx' and 'mov rdx' as the same" >&2
        echo "       instruction. Every result it prints would be" >&2
        echo "       meaningless. Fix shape_norm." >&2
        exit 2
    fi
    # ...and the two things it IS meant to erase must actually be
    # erased, or the mode reports a difference for every program and
    # is useless in the other direction.
    printf '%s\n' '       .   +  4: mov rcx, [rax+0x8]' \
                   '       .   + 11: je +107' \
                   '   1  enter.nat    frag@+7219' > "$TMP/nv3"
    printf '%s\n' '       .   + 91: mov rcx, [rax+0x8]' \
                   '       .   + 98: je +203' \
                   '   1  enter.nat    frag@+7213' > "$TMP/nv4"
    shape_norm < "$TMP/nv3" > "$TMP/nv3n"
    shape_norm < "$TMP/nv4" > "$TMP/nv4n"
    if ! cmp -s "$TMP/nv3n" "$TMP/nv4n"; then
        echo "error: the --same-shape normaliser does not erase the" >&2
        echo "       offset column and the branch displacement, which" >&2
        echo "       is its whole job. Fix shape_norm." >&2
        exit 2
    fi
fi

same=0
diffs=0
fails=0
flakes=0
sum_a=0
sum_b=0
# Where a NON-REPRODUCING difference's evidence is kept (see the
# confirm step below). Not $TMP - that is deleted on exit.
EVID=${VDJCMP_EVIDENCE:-./vdjcmp-flake}
for f in bench/my/*.my samples/* tests/functional/*.my; do
    [ -f "$f" ] || continue
    "$A" -vdj "$f" > "$TMP/a" 2>"$TMP/ae"; rca=$?
    "$B" -vdj "$f" > "$TMP/b" 2>"$TMP/be"; rcb=$?
    #
    # ⛔ A RUN THAT FAILED IS NOT A DIFFERENCE, AND SAYING SO IS THE
    # WHOLE POINT (2026-08-18). This loop used to send both dumps to
    # /dev/null and compare them without ever looking at the exit
    # status. A binary that died - a signal, an OOM, a sanitizer abort,
    # a bad argument - therefore produced a truncated or EMPTY dump,
    # `cmp` duly reported it as different, and the script printed
    # `DIFF: <prog>` - the exact output a genuine emitted-code
    # regression produces. The two are indistinguishable, and the
    # failure is the more likely of the two during an emitter
    # conversion, which is precisely when this script is trusted most.
    #
    # It bit for real: one run of this script reported
    # `108 identical / 1 differing` for bench/my/29_str_slice_readonly,
    # while the same two binaries produced byte-identical dumps for
    # that program 200 times in a row when asked directly. A one-off
    # flake that reads as a code change is worse than no oracle,
    # because the next real regression gets waved away as "that flake
    # again".
    #
    # Verified premise: `-vdj` DUMPS and exits - it never runs the
    # program - so across the whole corpus it exits 0 with a non-empty
    # dump. A non-zero status or an empty file is therefore
    # unambiguously a failed run, never a legitimate result.
    #
    # This is CLAUDE.md's instrument property 2, "it says when it does
    # not know", applied to the script rather than to the disassembler
    # it reads.
    if [ $rca -ne 0 ] || [ $rcb -ne 0 ] || \
       [ ! -s "$TMP/a" ] || [ ! -s "$TMP/b" ]; then
        fails=$((fails + 1))
        echo "FAIL: $f  (exit $rca/$rcb," \
             "$(wc -c < "$TMP/a")/$(wc -c < "$TMP/b") bytes)"
        head -3 "$TMP/ae" "$TMP/be" 2>/dev/null | sed 's/^/    /'
        continue
    fi
    if [ -n "$SHAPE" ]; then
        shape_norm < "$TMP/a" > "$TMP/an"
        shape_norm < "$TMP/b" > "$TMP/bn"
        oa=$(last_off "$TMP/a"); ob=$(last_off "$TMP/b")
        if [ -n "$oa" ] && [ -n "$ob" ]; then
            sum_a=$((sum_a + oa)); sum_b=$((sum_b + ob))
        fi
        if cmp -s "$TMP/an" "$TMP/bn"; then
            same=$((same + 1))
            continue
        fi
        diffs=$((diffs + 1))
        echo "DIFF: $f  (instruction stream, not just lengths)"
        if [ "$VERBOSE" = "-v" ]; then
            diff "$TMP/an" "$TMP/bn" | sed 's/^/    /'
        fi
        continue
    fi
    if cmp -s "$TMP/a" "$TMP/b"; then
        same=$((same + 1))
        continue
    fi
    #
    # ⛔ CONFIRM A DIFFERENCE BEFORE REPORTING ONE (2026-08-18).
    #
    # A real code change differs EVERY time; `-vdj` is documented
    # reproducible, so a difference that does not reproduce means the
    # ORACLE is unreliable for this program on this run, which is a
    # third outcome and not the same as "your change altered the code".
    #
    # This is not a mask - the run above is not discarded, it is
    # RE-ASKED, and a non-reproducing answer is reported LOUDLY, saved
    # with its evidence, and made to fail the script. Masking would be
    # ignoring it; this is instrument property 2, "it says when it does
    # not know", plus the evidence needed to eventually root-cause it.
    #
    # ⛔ IT IS NOT HYPOTHETICAL AND IT IS NOT YET EXPLAINED. Twice in
    # one session a full-corpus run reported exactly ONE differing
    # program - 29_str_slice_readonly, then 67_make_dict - and neither
    # reproduced: 250 same-binary runs and 200 cross-binary runs of
    # each of those two programs, plus six clean self-tests, all
    # identical. Rate is on the order of 1 in 1000 program
    # comparisons. Until it is understood, a run that hits it must say
    # so rather than accuse the change under test - the failure mode
    # this whole script exists to avoid is a real regression being
    # waved away as "that flake again".
    #
    "$A" -vdj "$f" > "$TMP/a2" 2>/dev/null; rca2=$?
    "$B" -vdj "$f" > "$TMP/b2" 2>/dev/null; rcb2=$?
    if [ $rca2 -eq 0 ] && [ $rcb2 -eq 0 ] && \
       [ -s "$TMP/a2" ] && [ -s "$TMP/b2" ] && \
       cmp -s "$TMP/a2" "$TMP/b2"; then
        flakes=$((flakes + 1))
        mkdir -p "$EVID"
        base=$(echo "$f" | tr '/' '_')
        cp "$TMP/a"  "$EVID/$base.run1.a"
        cp "$TMP/b"  "$EVID/$base.run1.b"
        cp "$TMP/a2" "$EVID/$base.run2.a"
        echo "FLAKE: $f  (differed once, identical on re-run;" \
             "evidence in $EVID)"
        continue
    fi
    diffs=$((diffs + 1))
    echo "DIFF: $f"
    if [ "$VERBOSE" = "-v" ]; then
        diff "$TMP/a" "$TMP/b" | sed 's/^/    /'
    fi
done

echo "identical: $same   differing: $diffs   failed: $fails" \
     "  flaky: $flakes"
if [ -n "$SHAPE" ]; then
    echo "mode: --same-shape (encoding LENGTHS allowed to differ)"
    echo "emitted bytes: old=$sum_a new=$sum_b  delta=$((sum_b - sum_a))"
fi
if [ "$flakes" -ne 0 ]; then
    echo "error: $flakes program(s) differed on one run and not on the" >&2
    echo "       next. THE ORACLE, NOT THE CHANGE, IS AT FAULT for" >&2
    echo "       those - do not read them as a code difference, and" >&2
    echo "       do not read this run as a clean one either. The" >&2
    echo "       dumps are saved; root-cause them." >&2
    exit 3
fi
if [ "$fails" -ne 0 ]; then
    echo "error: $fails program(s) could not be disassembled by one or" >&2
    echo "       both binaries. Those are NOT differences - the run" >&2
    echo "       died - and no verdict below covers them." >&2
    exit 2
fi
[ "$diffs" -eq 0 ]
