#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
regcensus.py - how many places HARDCODE each x86-64 scratch register, and
               which of them actually stand between us and pinning it.

    scripts/regcensus.py                # the table
    scripts/regcensus.py RDI            # + every unbracketed site for RDI
    scripts/regcensus.py --all          # + the sites for every register

#96 needs 13 of 16 registers usable by the pin allocator. A register can
only join the pool once nothing emits to it behind the allocator's back,
so "how many sites hardcode it" is the size of the remaining work - and
it is the number to re-measure after every conversion increment.

⛔ TWO CORRECTIONS THIS SCRIPT EXISTS TO MAKE, both of which I got wrong
by grepping, and both of which pointed at the WRONG register:

 1. COMMENTS AND STRINGS ARE NOT CODE. A plain `grep -c RSI src/jit.cpp`
    answered 92, and the prose - including the notes I had just written
    ABOUT rsi, and a stale count in a comment - was a large part of it.
    The real figure is a third of that. So this scrubs block comments,
    line comments and string literals (preserving line numbers) before
    counting, and skips the `enum Reg` declaration itself.

 2. A USE INSIDE A CALL BRACKET IS ALREADY SAFE. `emit_call_prologue`
    SPILLS every caller-saved pin and `emit_call_epilogue` reloads it,
    so the SysV argument setup between them cannot disturb a pin no
    matter which register it writes. Only an UNBRACKETED use is a
    blocker. Counting all uses made rsi/rdi look like the expensive
    registers when they are the cheapest.

    The bracket scan resets at every top-level `}` so a prologue with no
    matching epilogue on some path cannot mark the rest of the file
    "bracketed" - and it must run on the SCRUBBED text, because comments
    mentioning `emit_call_prologue` otherwise open brackets that never
    close (that alone moved RAX from 8 bracketed to 190).

The output is a lower bound on the work, not an upper one: an
unbracketed site may still turn out to be an ABI setup whose bracket
this scan cannot see. Read the sites before trusting a count.
"""

import re
import sys
import os

REGS = ['RAX', 'RCX', 'RDX', 'RSI', 'RDI', 'R8R', 'R9R']


def scrub(s):
    """Blank out comments and string literals, keeping every newline so
    line numbers still refer to the real file."""
    out, i, n = [], 0, len(s)
    while i < n:
        if s.startswith('/*', i):
            j = s.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(c if c == '\n' else ' ' for c in s[i:j]))
            i = j
        elif s.startswith('//', i):
            j = s.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif s[i] == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(''.join(c if c == '\n' else ' ' for c in s[i:j]))
            i = j
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def census(path):
    raw = open(path).read()
    lines = scrub(raw).split('\n')
    rawl = raw.split('\n')

    depth, bracketed = 0, [False] * len(lines)
    for i, l in enumerate(lines):
        if l.startswith('}'):            # end of a top-level definition
            depth = 0
            continue
        if 'emit_call_prologue(' in l:
            depth += 1
        bracketed[i] = depth > 0
        if 'emit_call_epilogue(' in l:
            depth = max(0, depth - 1)

    res = {r: {'br': 0, 'un': 0, 'lines': []} for r in REGS}
    for i, l in enumerate(lines):
        if 'enum Reg' in l:
            continue
        for r in REGS:
            n = len(re.findall(r'\b' + r + r'\b', l))
            if not n:
                continue
            if bracketed[i]:
                res[r]['br'] += n
            else:
                res[r]['un'] += n
                res[r]['lines'].append(i + 1)
    return res, rawl


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, '..', 'src', 'jit.cpp')
    res, rawl = census(path)

    want = [a for a in sys.argv[1:] if not a.startswith('-')]
    show_all = '--all' in sys.argv

    print("hardcoded scratch registers in src/jit.cpp "
          "(code only; comments/strings removed)\n")
    print("%-6s %10s %13s %7s   %s"
          % ("reg", "bracketed", "UNbracketed", "total", "cost to free"))
    tb = tu = 0
    for r in REGS:
        b, u = res[r]['br'], res[r]['un']
        tb += b
        tu += u
        verdict = "CHEAP" if u <= 20 else ("mid" if u <= 80 else "expensive")
        print("%-6s %10d %13d %7d   %s" % (r, b, u, b + u, verdict))
    print("%-6s %10d %13d %7d" % ("TOTAL", tb, tu, tb + tu))
    print("\nonly the UNbracketed column is work: a use between "
          "emit_call_prologue\nand emit_call_epilogue cannot disturb a pin "
          "(the prologue spilled it).")

    for r in REGS:
        if not (show_all or r in want):
            continue
        print("\n=== %s unbracketed sites (%d) ===" % (r, res[r]['un']))
        for n in res[r]['lines']:
            print("%6d: %s" % (n, rawl[n - 1].strip()[:92]))


if __name__ == '__main__':
    main()
