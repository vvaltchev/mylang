#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
Differential fuzzer for MyLang's deep-nesting bytecode codegen.

For a recursive tree-walker, "N levels of nesting work" is nearly self-evident.
For the FLAT bytecode VM it is not: nested loops/ifs are compiled into one
linear instruction stream with jump backpatching and reused scratch registers,
so deep nesting can expose codegen bugs (wrong backpatch target, temp-slot
collision across levels, a break/continue targeting the wrong loop, array COW
under mutation) that never occur in the tree-walker. This tool hammers exactly
that.

It generates random, DEEPLY-NESTED MyLang programs full of side effects (fixed
+ growing arrays, a dict, many scalar accumulators, per-level temp variables,
break/continue) together with their EXACT Python twins, then checks

        tree-walker result  ==  VM (-vm) result  ==  CPython result

for every program. The generator stays strictly inside the documented
MyLang/Python equivalence subset (see bench/README.md), so a mismatch is a real
interpreter bug, not a known semantic difference:
  * every value is kept in [0, MOD) - operands are non-negative before every `%`
    (MyLang truncates toward zero, Python floors: they'd differ on a negative),
  * no integer overflow past 64-bit (products of two [0,MOD) values fit),
  * NO division (`/` truncates in MyLang, is float in Python),
  * dicts use only fixed integer keys read by index - never iterated (MyLang is
    unordered, Python insertion-ordered),
  * break/continue only where the C-for <-> Python-for/while translation is
    exact.

Usage:
    python3 tests/nested_fuzz.py [--count 1000] [--max-depth 15]
        [--mylang build/mylang] [--seed 1] [--engines tw,vm,py]
        [--keep-failures DIR] [--check-fallbacks] [-v]

Exit code is non-zero if any program diverges (or, with --check-fallbacks, if a
program compiled to an AST fallback op).
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

# A SMALL modulus keeps every value 2 digits, so the generated code is readable
# and the variety is visibly in the control flow / operation KINDS, not in the
# size of numbers (the CPU and MyLang don't care about magnitude within a
# register). It still bounds accumulators and keeps everything non-negative.
MOD = 100
ALEN = 7          # size of the fixed array A
GCAP = 20         # cap on the growing array G (so len(G) stays small too)
MDIM = 3          # M is an MDIM x MDIM nested array (2-D structure)


class Gen:
    """Generate one random program (MyLang + Python) for a given seed/depth."""

    def __init__(self, seed, depth):
        self.rng = random.Random(seed)
        self.depth = depth
        self.my = []
        self.py = []
        # names of scalar accumulators always in scope
        self.scalars = ["s0", "s1", "s2", "s3", "s4", "s5", "s6"]
        # a stack of loop variables currently in scope (for use in expressions)
        self.loopvars = []
        self.tempcount = 0

    # ---- expression builders (every result is a non-negative int < MOD).
    # NOTE: expr()/operand() return a string used VERBATIM for BOTH engines -
    # every operator here (+ - * % & | ^ < > <= >= == !=, A[], len, max, min)
    # is syntactically AND semantically identical in MyLang and Python on
    # non-negative ints (a MyLang bool from a comparison promotes to 0/1 in
    # arithmetic, exactly like a Python bool). ----

    def leaf(self):
        choices = list(self.scalars) + ["rep"]
        if self.loopvars:
            choices += self.loopvars
        return self.rng.choice(choices)

    def operand(self, d=0):
        """A non-negative int-valued leaf or small subexpression."""
        r = self.rng.random()
        if d < 2 and r < 0.10:
            # nested array index: A[<operand> % 7]
            return "A[(%s) %% %d]" % (self.operand(d + 1), ALEN)
        if r < 0.28:
            return "A[%s %% %d]" % (self.leaf(), ALEN)
        if r < 0.36:
            return str(self.rng.randint(0, 9))
        if r < 0.42:
            return "len(G)"
        if d < 2 and r < 0.52:
            # a comparison used as a 0/1 value (MyLang bool + arith == Python).
            # Parenthesized: MyLang binds comparison ABOVE bitwise, Python below
            # (the `a & b == c` trap) - explicit parens keep the two identical.
            op = self.rng.choice(["<", ">", "<=", ">=", "==", "!="])
            return "(%s %s %s)" % (self.leaf(), op, self.leaf())
        if d < 2 and r < 0.60:
            return "%s(%s, %s)" % (self.rng.choice(["max", "min"]),
                                   self.leaf(), self.leaf())
        if d < 2 and r < 0.66:
            # a 2-D array read M[i][j] (multi-level subscript load)
            return "M[%s %% %d][%s %% %d]" % (self.leaf(), MDIM,
                                              self.leaf(), MDIM)
        if d < 2 and r < 0.72:
            # sum of a NON-EMPTY slice a<b (COW sub-view). Two subtleties:
            # (1) an EMPTY slice diverges (MyLang sum([]) is none, Python 0) - so
            # keep a<b strictly; (2) the sum can reach (b-a)*99 >> MOD, and EVERY
            # operand must stay < MOD or the `(x + MOD - operand)` subtraction
            # form goes negative (a mixed truncate/floor `%` divergence + an
            # out-of-bounds erase index) - so mod it.
            a = self.rng.randint(0, ALEN - 2)
            b = self.rng.randint(a + 1, ALEN)
            return "(sum(A[%d:%d])) %% %d" % (a, b, MOD)
        return self.leaf()

    def expr(self, d=0):
        """A short non-negative expression `(...) % MOD`, MyLang/Python-identical.
        1-2 operations - the variety is in the OPERAND kinds and operators, not
        in length (`1+1+1` is not more complex than `1+1`, just more work)."""
        a = self.operand(d)
        for _ in range(self.rng.randint(0, 1)):
            op = self.rng.choice(["+", "-", "*", "&", "|", "^"])
            b = self.operand(d)
            if op == "-":
                a = "(%s + %d - %s)" % (a, MOD, b)   # non-negative
            else:
                a = "(%s %s %s)" % (a, op, b)
        return "(%s) %% %d" % (a, MOD)

    # ---- statements ----

    def emit(self, m, p):
        self.my.append(m)
        self.py.append(p)

    # A weighted menu of side-effect kinds - (method, weight). New structural
    # constructs are added here (see plans/fuzz-variability.md).
    def se_menu(self):
        return [
            (self.se_scalar,  30),
            (self.se_array,   15),
            (self.se_dict,    12),
            (self.se_append,  10),
            (self.se_temp,    10),
            (self.se_foreach, 10),
            (self.se_ternary, 10),
            (self.se_matrix,  12),
            (self.se_pop,      6),
            (self.se_insert,   6),
            (self.se_erase,    6),
            (self.se_clone,    5),
            (self.se_swap,     8),
            (self.se_multidecl, 8),
        ]

    def side_effect(self, im, ip):
        """One random side effect, emitted as a (MyLang, Python) pair."""
        menu = self.se_menu()
        fn = self.rng.choices([m for m, _ in menu],
                              weights=[w for _, w in menu])[0]
        fn(im, ip)

    def se_scalar(self, im, ip):
        tgt = self.rng.choice(self.scalars)
        e = "%s = %s" % (tgt, self.expr())
        self.emit(im + e + ";", ip + e)

    def se_array(self, im, ip):
        # the INDEX is often a full expression too (A[<large expr>] = v)
        if self.rng.random() < 0.5:
            idx = "%s %% %d" % (self.leaf(), ALEN)
        else:
            idx = "(%s) %% %d" % (self.expr(), ALEN)
        e = "A[%s] = %s" % (idx, self.expr())
        self.emit(im + e + ";", ip + e)

    def se_dict(self, im, ip):
        key = self.rng.randint(0, 4)
        e = "D[%d] = (D[%d] + %s) %% %d" % (key, key, self.operand(), MOD)
        self.emit(im + e + ";", ip + e)

    def se_append(self, im, ip):
        e = self.expr()   # ONCE - the same string must go to both engines
        self.emit(im + "if (len(G) < %d) append(G, %s);" % (GCAP, e),
                  ip + "if len(G) < %d: G.append(%s)" % (GCAP, e))

    def se_foreach(self, im, ip):
        # a FLAT foreach over the fixed array, folding it into a scalar
        fe = "e%d" % self.tempcount
        self.tempcount += 1
        tgt = self.rng.choice(self.scalars)
        body = "%s = (%s + %s) %% %d" % (tgt, tgt, fe, MOD)
        self.emit(im + "foreach (var %s in A) %s;" % (fe, body),
                  ip + "for %s in A: %s" % (fe, body))

    def se_temp(self, im, ip):
        # a per-level TEMP variable: declare + fold into a scalar (stresses the
        # VM's scratch-slot allocation at depth)
        t = "t%d" % self.tempcount
        self.tempcount += 1
        tgt = self.rng.choice(self.scalars)
        ev = self.expr()   # ONCE
        self.emit(im + "var %s = %s;" % (t, ev), ip + "%s = %s" % (t, ev))
        e = "%s = (%s + %s) %% %d" % (tgt, tgt, t, MOD)
        self.emit(im + e + ";", ip + e)

    def se_ternary(self, im, ip):
        # a conditional VALUE (not a branch statement): s = cond ? a : b.
        # MyLang `c ? a : b` vs Python `a if c else b` - the ONLY difference is
        # the wrapper; the condition + arms are identical-syntax strings.
        tgt = self.rng.choice(self.scalars)
        cond = "(%s %s %s)" % (self.leaf(),
                               self.rng.choice(["<", ">", "<=", ">=",
                                                "==", "!="]), self.leaf())
        a, b = self.expr(), self.expr()
        self.emit(im + "%s = (%s ? %s : %s);" % (tgt, cond, a, b),
                  ip + "%s = (%s if %s else %s)" % (tgt, a, cond, b))

    def se_matrix(self, im, ip):
        # a 2-D array store M[i][j] = v (multi-level subscript store, inner COW).
        # Identical syntax in both engines.
        i = "%s %% %d" % (self.leaf(), MDIM)
        j = "%s %% %d" % (self.leaf(), MDIM)
        e = "M[%s][%s] = %s" % (i, j, self.expr())
        self.emit(im + e + ";", ip + e)

    # data-structure mutations on the growing array G (guarded so they never
    # under/overflow); builtins in MyLang vs methods in Python -> (my, py) pairs
    def se_pop(self, im, ip):
        self.emit(im + "if (len(G) > 0) pop(G);",
                  ip + "if len(G) > 0: G.pop()")

    def se_insert(self, im, ip):
        e = self.expr()   # ONCE
        self.emit(im + "if (len(G) < %d) insert(G, 0, %s);" % (GCAP, e),
                  ip + "if len(G) < %d: G.insert(0, %s)" % (GCAP, e))

    def se_erase(self, im, ip):
        # erase at a valid data-dependent index; `del` is Python's erase
        g = self.rng.choice(self.scalars)
        self.emit(im + "if (len(G) > 0) erase(G, %s %% len(G));" % g,
                  ip + "if len(G) > 0: del G[%s %% len(G)]" % g)

    def se_clone(self, im, ip):
        # clone the fixed array + fold its sum (exercises the clone builtin);
        # MyLang clone() vs Python list()
        tgt = self.rng.choice(self.scalars)
        self.emit(im + "%s = (%s + sum(clone(A))) %% %d;" % (tgt, tgt, MOD),
                  ip + "%s = (%s + sum(list(A))) %% %d" % (tgt, tgt, MOD))

    def se_swap(self, im, ip):
        # rotate N distinct scalars via multi-assign - snapshot-before-bind
        # (`a, b = [b, a]` swaps). MyLang array-destructure vs Python tuple.
        n = self.rng.randint(2, 3)
        tgts = self.rng.sample(self.scalars, n)
        src = tgts[1:] + tgts[:1]          # rotate left
        lhs = ", ".join(tgts)
        self.emit(im + "%s = [%s];" % (lhs, ", ".join(src)),
                  ip + "%s = %s" % (lhs, ", ".join(src)))

    def se_multidecl(self, im, ip):
        # a destructuring DECL `var a, b = [e0, e1]` (STRICT arity), then fold
        a, b = "t%d" % self.tempcount, "t%d" % (self.tempcount + 1)
        self.tempcount += 2
        e0, e1 = self.expr(), self.expr()
        tgt = self.rng.choice(self.scalars)
        self.emit(im + "var %s, %s = [%s, %s];" % (a, b, e0, e1),
                  ip + "%s, %s = %s, %s" % (a, b, e0, e1))
        fold = "%s = (%s + %s + %s) %% %d" % (tgt, tgt, a, b, MOD)
        self.emit(im + fold + ";", ip + fold)

    def build(self, level, im, ip):
        # a side effect at this level, then (if not innermost) a nested construct
        self.side_effect(im, ip)
        if level == self.depth:
            return
        # 1-2 extra side effects sometimes, for variety
        for _ in range(self.rng.randint(0, 1)):
            self.side_effect(im, ip)

        kind = self.rng.choice(["if", "while", "for_opt", "for_gen",
                                "for_down", "while_data"])
        v = "v%d" % level

        if kind == "if":
            # BOTH branches recurse: true nesting, both paths run, deep always
            # reached. Distinct side effect per branch.
            key = self.rng.choice(self.scalars)
            m = self.rng.randint(2, 4)
            self.emit(im + "if (%s %% %d != 0) {" % (key, m),
                      ip + "if %s %% %d != 0:" % (key, m))
            self.side_effect(im + "  ", ip + "    ")
            self.build(level + 1, im + "  ", ip + "    ")
            self.emit(im + "} else {", ip + "else:")
            self.side_effect(im + "  ", ip + "    ")
            self.build(level + 1, im + "  ", ip + "    ")
            self.emit(im + "}", "")

        elif kind == "while":
            n = self.rng.randint(2, 3)
            self.emit(im + "var %s = 0;" % v, ip + "%s = 0" % v)
            self.emit(im + "while (%s < %d) {" % (v, n),
                      ip + "while %s < %d:" % (v, n))
            # increment FIRST so any later continue is translation-safe
            self.emit(im + "  %s += 1;" % v, ip + "    %s += 1" % v)
            self.loopvars.append(v)
            self.build(level + 1, im + "  ", ip + "    ")
            self.loopvars.pop()
            self.emit(im + "}", "")

        elif kind == "for_opt":
            n = self.rng.randint(2, 3)
            self.emit(im + "for (var %s = 0; %s < %d; %s++) {" % (v, v, n, v),
                      ip + "for %s in range(%d):" % (v, n))
            # rare continue (before deep) + rare break (after deep): a MyLang
            # `for` and a Python `for v in range` advance/exit identically.
            g1 = self.rng.choice(self.scalars)
            self.emit(im + "  if (%s %% 11 == 0) continue;" % g1,
                      ip + "    if %s %% 11 == 0: continue" % g1)
            self.loopvars.append(v)
            self.build(level + 1, im + "  ", ip + "    ")
            self.loopvars.pop()
            g2 = self.rng.choice(self.scalars)
            self.emit(im + "  if (%s %% 13 == 0) break;" % g2,
                      ip + "    if %s %% 13 == 0: break" % g2)
            self.emit(im + "}", "")

        elif kind == "for_down":
            # DOWN-counting: v = N, N-1, ..., 1 (reverse iteration).
            n = self.rng.randint(2, 3)
            self.emit(im + "for (var %s = %d; %s > 0; %s--) {" % (v, n, v, v),
                      ip + "for %s in range(%d, 0, -1):" % (v, n))
            self.loopvars.append(v)
            self.build(level + 1, im + "  ", ip + "    ")
            self.loopvars.pop()
            g = self.rng.choice(self.scalars)
            self.emit(im + "  if (%s %% 13 == 0) break;" % g,
                      ip + "    if %s %% 13 == 0: break" % g)
            self.emit(im + "}", "")

        elif kind == "while_data":
            # DATA-DEPENDENT bound: the `<scalar> < LIM` part can flip during the
            # loop (the body mutates scalars); the `v < N` counter guarantees
            # termination. `&&` vs `and` -> an explicit (my, py) header pair.
            n = self.rng.randint(2, 4)
            lim = self.rng.randint(40, 90)
            g = self.rng.choice(self.scalars)
            self.emit(im + "var %s = 0;" % v, ip + "%s = 0" % v)
            self.emit(im + "while (%s < %d && %s < %d) {" % (v, n, g, lim),
                      ip + "while %s < %d and %s < %d:" % (v, n, g, lim))
            self.emit(im + "  %s += 1;" % v, ip + "    %s += 1" % v)
            self.loopvars.append(v)
            self.build(level + 1, im + "  ", ip + "    ")
            self.loopvars.pop()
            self.emit(im + "}", "")

        else:  # for_gen: multiplicative step -> Python `while` (no continue:
               # a Python continue would skip `v*=2` -> infinite loop)
            top = self.rng.choice([4, 8])
            self.emit(im + "for (var %s = 1; %s < %d; %s = %s * 2) {"
                      % (v, v, top, v, v),
                      ip + "%s = 1" % v)
            self.emit("", ip + "while %s < %d:" % (v, top))
            self.loopvars.append(v)
            self.build(level + 1, im + "  ", ip + "    ")
            self.loopvars.pop()
            self.emit(im + "}", ip + "    %s = %s * 2" % (v, v))

    def program(self, reps):
        self.emit("var A = [1, 2, 3, 4, 5, 6, 7];", "A = [1, 2, 3, 4, 5, 6, 7]")
        # G is seeded with [0] (not []): pins its element type to int (an untyped
        # empty `[]` infers array<none> -> `acc + x` is a MyLang nullability
        # error, while Python's empty foreach is a no-op) AND stays a native
        # array literal (a typed empty-array decl would be an AST fallback). The
        # seed 0 is harmless + identical in both engines.
        self.emit("var G = [0];", "G = [0]")
        self.emit("var D = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0};",
                  "D = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0}")
        self.emit("var M = [[1, 2, 3], [4, 5, 6], [7, 8, 9]];",
                  "M = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]")
        self.emit("var s0 = 0; var s1 = 1; var s2 = 2; var s3 = 3; "
                  "var s4 = 4; var s5 = 5; var s6 = 6;",
                  "s0 = 0; s1 = 1; s2 = 2; s3 = 3; s4 = 4; s5 = 5; s6 = 6")
        self.emit("for (var rep = 0; rep < %d; rep++) {" % reps,
                  "for rep in range(%d):" % reps)
        self.build(0, "  ", "    ")
        self.emit("}", "")
        # aggregation - covers every piece of state (MOD embedded literally to
        # avoid %-format escaping of the MyLang modulo operator)
        m = str(MOD)
        agg = [
            ("var acc = 0;", "acc = 0"),
            ("foreach (var x in A) acc = (acc + x) % " + m + ";",
             "for x in A: acc = (acc + x) % " + m),
            ("foreach (var x in G) acc = (acc + x) % " + m + ";",
             "for x in G: acc = (acc + x) % " + m),
            ("acc = (acc + len(G)) % " + m + ";",
             "acc = (acc + len(G)) % " + m),
            ("foreach (var row in M) foreach (var x in row)"
             " acc = (acc + x) % " + m + ";",
             "for row in M:"),
            ("", "    for x in row: acc = (acc + x) % " + m),
            ("acc = (acc + s0 + s1 + s2 + s3 + s4 + s5 + s6) % " + m + ";",
             "acc = (acc + s0 + s1 + s2 + s3 + s4 + s5 + s6) % " + m),
            ("acc = (acc + D[0] + D[1] + D[2] + D[3] + D[4]) % " + m + ";",
             "acc = (acc + D[0] + D[1] + D[2] + D[3] + D[4]) % " + m),
            ('print("result:", acc);', 'print("result:", acc)'),
        ]
        for m, p in agg:
            self.emit(m, p)
        my = "\n".join(l for l in self.my if l.strip()) + "\n"
        py = "\n".join(l for l in self.py if l.strip()) + "\n"
        return my, py


def run(cmd, src_path):
    try:
        out = subprocess.run(cmd + [src_path], capture_output=True, text=True,
                             timeout=30)
    except subprocess.TimeoutExpired:
        return "<timeout>"
    if out.returncode != 0:
        return "<error:%d> %s" % (out.returncode,
                                  (out.stderr or out.stdout).strip()[:200])
    return out.stdout.strip()


def has_fallback(mylang, my_path):
    out = subprocess.run([mylang, "-vd", my_path], capture_output=True,
                         text=True)
    txt = out.stdout + out.stderr
    return ("eval.stmt" in txt) or ("eval.expr" in txt) or ("EvalToSlot" in txt)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=1000,
                    help="number of random programs (default 1000)")
    ap.add_argument("--max-depth", type=int, default=15,
                    help="max nesting depth; each program picks 1..max (def 15)")
    ap.add_argument("--min-depth", type=int, default=1)
    ap.add_argument("--reps", type=int, default=2,
                    help="rep-loop iterations per program (default 2)")
    ap.add_argument("--mylang", default="build/mylang",
                    help="path to the mylang binary (default build/mylang)")
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--seed", type=int, default=1,
                    help="base seed (program i uses seed+i); DETERMINISTIC by "
                         "design, so a reported failure reproduces exactly")
    ap.add_argument("--random", action="store_true",
                    help="pick a fresh random base seed each run (for casual "
                         "--show inspection); the chosen seed is printed so you "
                         "can reproduce it with --seed")
    ap.add_argument("--engines", default="tw,vm,py",
                    help="comma list of engines to compare: tw,vm,py")
    ap.add_argument("--keep-failures", default=None,
                    help="directory to save diverging .my/.py for debugging")
    ap.add_argument("--check-fallbacks", action="store_true",
                    help="also fail a program if it compiles to an AST fallback")
    ap.add_argument("--show", type=int, default=0, metavar="N",
                    help="print the first N generated programs (MyLang + Python "
                         "+ their result) and exit, for inspection")
    ap.add_argument("--show-depth", type=int, default=None,
                    help="with --show, force this exact depth (default random)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.random:
        args.seed = random.randrange(1, 2 ** 31)
        print("# --random: base seed = %d "
              "(reproduce with --seed %d)\n" % (args.seed, args.seed))

    def pick_depth(seed):
        if args.show and args.show_depth is not None:
            return args.show_depth
        return random.Random(seed * 2654435761 & 0xffffffff).randint(
            args.min_depth, args.max_depth)

    # --show: dump programs for a human to judge complexity, then exit
    if args.show:
        for i in range(args.show):
            seed = args.seed + i
            depth = pick_depth(seed)
            my_src, py_src = Gen(seed, depth).program(args.reps)
            tmp = tempfile.mkdtemp(prefix="mylang_show_")
            mp = os.path.join(tmp, "p.my")
            open(mp, "w").write(my_src)
            res = run([args.mylang, "-vm"], mp)
            nlines = my_src.count("\n")
            print("=" * 72)
            print("# PROGRAM %d   seed=%d   depth=%d   (%d lines)   %s"
                  % (i, seed, depth, nlines, res))
            print("=" * 72)
            print("------------------------------- MyLang "
                  "-------------------------------")
            print(my_src)
            print("------------------------------- Python "
                  "-------------------------------")
            print(py_src)
        return 0

    engines = args.engines.split(",")
    if args.keep_failures:
        os.makedirs(args.keep_failures, exist_ok=True)

    failures = 0
    fallbacks = 0
    tmp = tempfile.mkdtemp(prefix="mylang_fuzz_")
    my_path = os.path.join(tmp, "p.my")
    py_path = os.path.join(tmp, "p.py")

    for i in range(args.count):
        seed = args.seed + i
        depth = random.Random(seed * 2654435761 & 0xffffffff).randint(
            args.min_depth, args.max_depth)
        my_src, py_src = Gen(seed, depth).program(args.reps)
        open(my_path, "w").write(my_src)
        open(py_path, "w").write(py_src)

        results = {}
        if "tw" in engines:
            results["tw"] = run([args.mylang], my_path)
        if "vm" in engines:
            results["vm"] = run([args.mylang, "-vm"], my_path)
        if "py" in engines:
            results["py"] = run([args.python], py_path)

        vals = list(results.values())
        ok = all(v == vals[0] for v in vals) and not vals[0].startswith("<")
        fb = args.check_fallbacks and has_fallback(args.mylang, my_path)
        if fb:
            fallbacks += 1

        if not ok or fb:
            failures += 0 if ok else 1
            tag = "DIVERGE" if not ok else "FALLBACK"
            print("[%s] program %d (seed=%d depth=%d): %s"
                  % (tag, i, seed, depth,
                     "  ".join("%s=%s" % (k, v) for k, v in results.items())))
            if args.keep_failures:
                base = os.path.join(args.keep_failures,
                                    "fail_%d_seed%d_d%d" % (i, seed, depth))
                open(base + ".my", "w").write(my_src)
                open(base + ".py", "w").write(py_src)
                print("        saved %s.{my,py}" % base)
        elif args.verbose:
            print("[ok]   program %d (seed=%d depth=%d) = %s"
                  % (i, seed, depth, vals[0]))
        elif (i + 1) % 100 == 0:
            print("... %d/%d ok" % (i + 1, args.count))

    print("\n%d programs, depth <= %d, engines=%s: %d diverged%s"
          % (args.count, args.max_depth, args.engines, failures,
             (", %d fallbacks" % fallbacks) if args.check_fallbacks else ""))
    bad = failures + (fallbacks if args.check_fallbacks else 0)
    if bad == 0:
        print("ALL AGREE - tree-walker == VM == CPython on every program.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
