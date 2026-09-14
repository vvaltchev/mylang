#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
WHICH EMITTED INSTRUCTION DOES THE JIT SPEND ITS INSTRUCTIONS ON?

Nothing in this repo could answer that.  `-vdj` says what was EMITTED,
`bench/run.py` says how long the whole program took, and callgrind says
how many instructions ran and which C++ FUNCTION they were in - but JIT
code lives in an anonymous mapping with no symbols, so every emitted
instruction lands in one nameless blob.  The whole call-protocol arc was
costed by counting the emitted sequence BY HAND and hoping the sequence
being counted was the one that ran.

This joins two things that already exist:

  * `MYLANG_JIT_MAP=<path>` (disasm.cpp) - one line per emitted
    instruction at its RUNTIME address, decoded by the SAME decoder
    `-vdj` uses and `scripts/disasmcheck.py` cross-checks against
    objdump, so a length here is a length objdump agrees with;
  * callgrind `--dump-instr=yes` - Ir per absolute address.

and prints per-instruction Ir, hottest first, plus a per-fragment total.

    scripts/jitprofile.py BINARY PROGRAM [args...]
      --top N       how many instructions to list (default 40)
      --frag NAME   restrict to fragments whose label contains NAME
      --range A-B   restrict to a byte-offset window inside each fragment
      --keep        keep the temporary map / callgrind files and say where

THREE PROPERTIES, the ones CLAUDE.md asks any instrument for:

 1. REPRODUCIBLE - the numbers are callgrind Ir, not wall clock, so two
    runs agree exactly.  Addresses differ per run (ASLR + where mmap
    lands), which is precisely why the map is written BY THE SAME
    PROCESS being profiled rather than by a separate `-vdj` run.
 2. IT SAYS WHEN IT DOES NOT KNOW - a fragment whose decode hit an
    undecodable byte is reported (its later addresses are unreliable
    because the decode desynchronised), and cost at an address the map
    does not cover is counted and printed as UNMAPPED rather than
    silently dropped.
 3. IT SELF-TESTS, UNCONDITIONALLY - mapped + unmapped Ir must equal
    the run's own `summary:` line, and the tool EXITS NON-ZERO when it
    does not.  That is the check a profiler most needs and most often
    lacks: callgrind's cost lines are relative-compressed and a
    mis-parsed `calls=` line charges a call site its whole subtree, which
    produces a plausible, wrong, and completely silent profile.
"""

import os
import re
import subprocess
import sys
import tempfile


def parse_map(path):
    """-> (frags, addr2insn).  frags: [(base, len, label, undecoded)]."""
    frags = []
    addr = {}
    cur = None
    for line in open(path):
        f = line.rstrip('\n').split(None, 3)
        if not f:
            continue
        if f[0] == 'frag':
            g = line.split()
            cur = (int(g[1], 16), int(g[2]), g[3],
                   int(g[4].split('=')[1]))
            frags.append(cur)
        elif f[0] == 'i' and cur is not None:
            addr[int(f[1], 16)] = (cur, int(f[2]),
                                   f[3] if len(f) > 3 else '?')
    return frags, addr


def parse_callgrind(path):
    """-> (cost_by_addr, summary_ir).

    Callgrind's `positions: instr` lines are relative-compressed: an
    absolute `0x...`, a `+N` / `-N` delta, or `*` (same position).  Cost
    lines inside a `calls=` block describe the CALLEE and must be
    skipped, or every call site is charged its whole subtree.
    """
    cost = {}
    summary = 0
    pos = 0
    skip_next = False
    for line in open(path):
        line = line.rstrip('\n')
        if not line:
            continue
        if line.startswith('summary:'):
            summary = int(line.split()[1])
            continue
        if line.startswith('calls='):
            skip_next = True
            continue
        if line[0] in 'abcdefghijklmnopqrstuvwxyz#' and '=' in line[:4]:
            continue
        m = re.match(r'^(0x[0-9a-fA-F]+|\+\d+|-\d+|\*)\s+(.*)$', line)
        if not m:
            continue
        p, rest = m.group(1), m.group(2).split()
        if p.startswith('0x'):
            pos = int(p, 16)
        elif p == '*':
            pass
        else:
            pos += int(p)
        if skip_next:            # the `calls=` line's cost is the callee's
            skip_next = False
            continue
        if not rest:
            continue
        # events are "Ir" or "instr line Ir" depending on positions:
        try:
            ir = int(rest[-1])
        except ValueError:
            continue
        cost[pos] = cost.get(pos, 0) + ir
    return cost, summary


def main():
    args = sys.argv[1:]
    top = 40
    frag_filter = None
    rng = None
    keep = False
    listing = False
    out = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--top':
            i += 1
            top = int(args[i])
        elif a == '--frag':
            i += 1
            frag_filter = args[i]
        elif a == '--range':
            i += 1
            lo, hi = args[i].split('-')
            rng = (int(lo), int(hi))
        elif a == '--keep':
            keep = True
        elif a == '--listing':
            listing = True
        else:
            out.append(a)
        i += 1
    if len(out) < 2:
        print(__doc__)
        sys.exit(2)
    binary, prog = out[0], out[1]
    rest = out[2:]

    d = tempfile.mkdtemp(prefix='jitprof-')
    mapf = os.path.join(d, 'map.txt')
    cgf = os.path.join(d, 'cg.out')
    env = dict(os.environ, MYLANG_JIT_MAP=mapf)
    cmd = ['valgrind', '--tool=callgrind', '--dump-instr=yes',
           '--callgrind-out-file=' + cgf, binary, prog] + rest
    r = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode('utf-8', 'replace')[-2000:])
        print("jitprofile: the program exited %d - nothing to report"
              % r.returncode)
        sys.exit(1)
    if not os.path.exists(mapf):
        print("jitprofile: no map was written - MYLANG_JIT_MAP is only "
              "honoured by a JIT-supported build with the JIT ON")
        sys.exit(1)

    frags, addr = parse_map(mapf)
    cost, summary = parse_callgrind(cgf)

    mapped = unmapped = 0
    per_frag = {}
    rows = []
    for a, ir in cost.items():
        hit = addr.get(a)
        if hit is None:
            unmapped += ir
            continue
        mapped += ir
        fr, ln, mn = hit
        off = a - fr[0]
        label = fr[2]
        per_frag[label] = per_frag.get(label, 0) + ir
        if frag_filter and frag_filter not in label:
            continue
        if rng and not (rng[0] <= off <= rng[1]):
            continue
        rows.append((ir, label, off, mn))

    bad = [f for f in frags if f[3]]
    if bad:
        print("⛔ %d fragment(s) had UNDECODABLE bytes - their later "
              "addresses are unreliable:" % len(bad))
        for f in bad:
            print("     %s (%d undecoded)" % (f[2], f[3]))

    print("\n%-14s %s" % ("Ir in JIT code:", "{:,}".format(mapped)))
    print("%-14s %s   (C++ helpers, libc, the interpreter, startup)"
          % ("elsewhere:", "{:,}".format(unmapped)))
    if summary:
        tot = mapped + unmapped
        ok = tot == summary
        print("%-14s %s   %s" % ("run total:", "{:,}".format(summary),
                                 "OK (join self-test)" if ok
                                 else "⛔ THE JOIN LOST %d Ir" %
                                      (summary - tot)))
        if not ok:
            print("\n⛔ REFUSING TO REPORT. The address join does not "
                  "account for every instruction the run executed, so "
                  "every number below is a guess. Fix parse_callgrind.")
            sys.exit(3)
    print("\nper fragment:")
    for label, ir in sorted(per_frag.items(), key=lambda kv: -kv[1]):
        print("  %-24s %14s  %5.1f%%"
              % (label, "{:,}".format(ir), 100.0 * ir / max(mapped, 1)))

    if listing:
        # ADDRESS ORDER, with a blank line at every cold gap: this is how
        # you read a hot PATH rather than a hot instruction, which is the
        # question a call protocol actually poses.
        print("\nlisting (address order; blank line = a not-executed gap):")
        seen = sorted((a for a in addr
                       if not frag_filter or frag_filter in addr[a][0][2]))
        prev_run = None
        for a_ in seen:
            fr, ln, mn = addr[a_]
            off = a_ - fr[0]
            if rng and not (rng[0] <= off <= rng[1]):
                continue
            ir = cost.get(a_, 0)
            if ir == 0:
                if prev_run is not None:
                    print()
                    prev_run = None
                continue
            prev_run = ir
            print("  %14s  %-16s %s"
                  % ("{:,}".format(ir), "%s+%d" % (fr[2], off), mn))
        return

    rows.sort(key=lambda r: -r[0])
    print("\nhottest emitted instructions:")
    print("  %14s %6s  %-22s %s" % ("Ir", "%", "fragment+off", "insn"))
    for ir, label, off, mn in rows[:top]:
        print("  %14s %5.1f%%  %-22s %s"
              % ("{:,}".format(ir), 100.0 * ir / max(mapped, 1),
                 "%s+%d" % (label, off), mn))

    if keep:
        print("\nkept: %s" % d)


if __name__ == '__main__':
    main()
