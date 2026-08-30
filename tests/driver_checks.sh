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

# ---------------------------------------------------------------------
# THE DEPTH CAP (MYLANG_VM_STACK) - untested until 2026-08-13, though the
# whole point of the segmented slot stack is that a runaway recursion
# throws a CATCHABLE StackOverflowEx where the old per-call C-stack model
# SEGFAULTED. It can only be tested from here: the cap is read ONCE per
# process into a static, so an in-process `-rt` entry cannot set it.
: > "$TMP/so.my"
cat > "$TMP/so.my" <<'EOF'
func down(int n) {
    if (n <= 0)
        return 0;
    var r = down(n - 1);
    return r + 1;
}
var caught = 0;
try {
    var d = down(runtime(100000));
    print("NO THROW", d);
} catch (StackOverflowEx) {
    caught = 1;
}
print("caught:", caught);
EOF
out=$(MYLANG_VM_STACK=4000 "$BIN" "$TMP/so.my" 2>&1)
got_rc=$?
if [ "$got_rc" = 0 ] && printf '%s' "$out" | grep -q '^caught: 1'; then
    pass "depth cap: deep recursion throws a CATCHABLE StackOverflowEx"
else
    fail "depth cap: wanted a caught StackOverflowEx, got rc=$got_rc [$out]"
fi

# ... and the cap must be the only thing stopping it: the SAME program at
# a depth that fits must run to completion. A cap change that throws too
# EARLY is the dangerous direction (it refuses a working program), and
# nothing else in the tree would notice.
: > "$TMP/ok.my"
cat > "$TMP/ok.my" <<'EOF'
func down(int n) {
    if (n <= 0)
        return 0;
    var r = down(n - 1);
    return r + 1;
}
print("depth:", down(runtime(300)));
EOF
out=$(MYLANG_VM_STACK=4000 "$BIN" "$TMP/ok.my" 2>&1)
got_rc=$?
if [ "$got_rc" = 0 ] && printf '%s' "$out" | grep -q '^depth: 300'; then
    pass "depth cap: a recursion that FITS still completes"
else
    fail "depth cap: the fitting program failed rc=$got_rc [$out]"
fi

# An UNCAUGHT overflow must still be a clean, located error - not a crash.
: > "$TMP/sou.my"
cat > "$TMP/sou.my" <<'EOF'
func down(int n) {
    if (n <= 0)
        return 0;
    var r = down(n - 1);
    return r + 1;
}
print("d:", down(runtime(100000)));
EOF
out=$(MYLANG_VM_STACK=4000 "$BIN" "$TMP/sou.my" 2>&1)
got_rc=$?
if [ "$got_rc" = 1 ] && printf '%s' "$out" | grep -q 'StackOverflowEx'; then
    pass "depth cap: an UNCAUGHT overflow is a located error, not a crash"
else
    fail "depth cap: uncaught overflow rc=$got_rc [$out]"
fi

# -v REPORTS THE ARENA, and MYLANG_NO_LOWMEM=1 refuses it. This is the
# VACUITY GUARD for the no-arena CI lane: a lane that tests a
# configuration must be able to prove it is IN that configuration, or it
# goes green doing nothing the day a default moves (the lto0 lane's
# reason for asserting `lto 0` before it tests anything). Checked in
# BOTH directions, so neither a stuck-on nor a stuck-off answer passes.
out=$("$BIN" -v 2>&1)
if printf '%s' "$out" | grep -q '^  lowmem  *1'; then
    pass "-v: the low-address arena is reported, and is ON by default"
else
    fail "-v: expected 'lowmem 1' by default [$out]"
fi
out=$(MYLANG_NO_LOWMEM=1 "$BIN" -v 2>&1)
if printf '%s' "$out" | grep -q '^  lowmem  *0'; then
    pass "-v: MYLANG_NO_LOWMEM=1 refuses the arena"
else
    fail "-v: MYLANG_NO_LOWMEM=1 did not refuse the arena [$out]"
fi

[ $rc = 0 ] && echo "all driver checks passed"
exit $rc
