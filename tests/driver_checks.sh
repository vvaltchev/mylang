#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Behaviour of the CLI DRIVER itself - the part `-rt` structurally cannot
# reach. The `-rt` suite runs in-process: it calls lexer/parser/infer/
# resolve directly and never goes through mylang.cpp's argument handling, so
# a flag that is wired to the wrong side of an `if` is invisible to all 1866
# of those tests.
#
# That is not hypothetical. `-nr` ("compile and validate, don't run") called
# run_optimizers only when it was going to RUN, so every diagnostic living in
# resolve_names - the step 7 prover, the whole warning tier, FIX-1, the TDZ,
# the duplicate-decl check - was silently skipped, and `-nr` exited 0 on a
# program a plain run refuses (#147). Every prover test passed throughout.
#
#   tests/driver_checks.sh ./build/mylang
#
# Exit 0 if every check passes, 1 otherwise. POSIX sh, no dependencies.

BIN="${1:-./build/mylang}"
[ -x "$BIN" ] || { echo "usage: $0 <path-to-mylang>"; exit 2; }

TMP="${TMPDIR:-/tmp}/mylang-driver-$$"
mkdir -p "$TMP" || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

rc=0
pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; rc=1; }

# $1 = label, $2 = expected exit code, $3 = a grep -E pattern stderr+stdout
# must match ("" = must produce NO output), $4.. = the program lines
check() {
    label=$1 want_rc=$2 want=$3
    shift 3
    : > "$TMP/p.my"
    for line in "$@"; do printf '%s\n' "$line" >> "$TMP/p.my"; done

    out=$("$BIN" -nr "$TMP/p.my" 2>&1)
    got_rc=$?

    if [ "$got_rc" != "$want_rc" ]; then
        fail "$label (exit $got_rc, wanted $want_rc)"
        return
    fi
    if [ -z "$want" ]; then
        if [ -n "$out" ]; then
            fail "$label (expected silence, got: $out)"
            return
        fi
    elif ! printf '%s' "$out" | grep -Eq "$want"; then
        fail "$label (no match for '$want' in: $out)"
        return
    fi
    pass "$label"
}

echo "driver checks: $BIN"

# -nr must REFUSE what a plain run refuses. Each of these lives in a
# different pass reached only after run_optimizers, and each exited 0 in
# silence before #147.
check "-nr: step 7 prover" 1 'UseBeforeBindingEx' \
    'func fetch() { return g; }' \
    'var dyn t = fetch();' \
    'var g = 5;'

check "-nr: prover through a write-once alias" 1 'UseBeforeBindingEx' \
    'func fetch() { return g; }' \
    'var f = fetch;' \
    'var dyn t = f();' \
    'var g = 5;'

check "-nr: FIX-1, a name declared nowhere" 1 'nosuchname' \
    'var x = nosuchname + 1;'

check "-nr: a duplicate declaration in one scope" 1 'AlreadyDefined' \
    'var a = 1;' \
    'var a = 2;'

# ... and it must EMIT the warning tier, which needs g_warnings drained on
# this path too - a second, separate wiring mistake with the same symptom.
check "-nr: the warning tier, definite arm" 0 'warning:.*may fail' \
    'func fetch() { return g; }' \
    'if (runtime(0) > 1) { var dyn t = fetch(); }' \
    'var g = 5;'

check "-nr: the warning tier, weak arm" 0 'warning:.*might fail' \
    'func fetch() { return g; }' \
    'var ops = [fetch];' \
    'var dyn t = ops[0]();' \
    'var g = 5;'

# The other direction, which matters just as much: -nr must stay QUIET and
# exit 0 on a correct program, or it is useless as a CI gate.
check "-nr: a correct program is silent" 0 '' \
    'func fetch() { return g; }' \
    'var g = 5;' \
    'var dyn t = fetch();'

# -nr must not RUN the program. `print` would put "ran" on stdout.
check "-nr: does not execute" 0 '' \
    'print("ran");'

# -s promises the const-folded tree AND the post-optimizer one; under -nr it
# used to print only the first, because there was no optimized tree to show.
: > "$TMP/s.my"
printf 'var a = 2 + 3;\nprint(a);\n' > "$TMP/s.my"
if "$BIN" -nr -s "$TMP/s.my" 2>&1 | grep -q '^Optimized syntax tree'; then
    pass "-nr -s: both trees"
else
    fail "-nr -s: the optimized tree is missing"
fi

[ $rc = 0 ] && echo "all driver checks passed"
exit $rc
