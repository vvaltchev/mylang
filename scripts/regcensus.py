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

 3. AN ACCESSOR WHOSE NAME ENCODES THE REGISTER IS INVISIBLE TO A SCAN
    FOR THE REGISTER. This is CLAUDE.md's "sixth audit-table shape", and
    this script walked straight into it: it reported RDI at 14
    unbracketed sites, the CHEAPEST register, while

        e.lea_rdi(d);   e.slots_to_arg0();   e.store_elem_byte_dil();

    - 25 more sites - name the register only in the METHOD. The count
    was low by two thirds and pointed at the wrong register.

    The fix is not a hand-written alias list (that is the stale-table
    trap one level up). The accessors are DERIVED: every `void name(`
    in the Emitter whose name, split on `_`, contains a register token
    (rdi/edi/dil/arg0/...) is treated as a use of that register, and its
    call sites are counted. A NEW fixed-pair wrapper is therefore
    counted the day it is written, with no edit here.

The output is a lower bound on the work, not an upper one: an
unbracketed site may still turn out to be an ABI setup whose bracket
this scan cannot see, and an accessor's IMPLICIT operands are invisible
to it (`idiv_rdi` also clobbers rax and rdx; `store_elem_int_rdi` reads
rcx and r9 - the encoding says so, the name does not). Read the sites
before trusting a count.
"""

import re
import sys
import os

REGS = ['RAX', 'RCX', 'RDX', 'RSI', 'RDI', 'R8R', 'R9R']

# The register a NAME TOKEN refers to. Sub-registers and the SysV
# argument-position spellings map to their 64-bit parent, because a
# write to any of them destroys a pin held in the whole register.
TOKEN_REG = {
    'rax': 'RAX', 'eax': 'RAX', 'ax': 'RAX', 'al': 'RAX',
    'rcx': 'RCX', 'ecx': 'RCX', 'cl': 'RCX',
    'rdx': 'RDX', 'edx': 'RDX', 'dl': 'RDX',
    'rsi': 'RSI', 'esi': 'RSI', 'sil': 'RSI', 'arg1': 'RSI',
    'rdi': 'RDI', 'edi': 'RDI', 'dil': 'RDI', 'arg0': 'RDI',
    'r8': 'R8R', 'r8d': 'R8R', 'r8b': 'R8R',
    'r9': 'R9R', 'r9d': 'R9R', 'r9b': 'R9R',
}

METHOD_DECL = re.compile(
    r'^\s{4}(?:static\s+)?(?:ML_\w+\s+)?'
    r'(?:void|int|bool|uint8_t|size_t|int32_t)\s+(\w+)\s*\(')


def derive_accessors(lines):
    """method name -> set of registers its NAME encodes.

    Derived from the source so a new fixed-pair wrapper cannot be
    missed the way lea_rdi/slots_to_arg0/store_elem_byte_dil were."""
    acc = {}
    for l in lines:
        m = METHOD_DECL.match(l)
        if not m:
            continue
        name = m.group(1)
        regs = {TOKEN_REG[t] for t in name.lower().split('_')
                if t in TOKEN_REG}
        if regs:
            acc[name] = regs
    return acc


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

    acc = derive_accessors(lines)
    # An accessor's own DECLARATION is not a use of the register.
    decl_line = {}
    for i, l in enumerate(lines):
        m = METHOD_DECL.match(l)
        if m and m.group(1) in acc:
            decl_line[i] = m.group(1)

    res = {r: {'br': 0, 'un': 0, 'lines': []} for r in REGS}
    for i, l in enumerate(lines):
        if 'enum Reg' in l:
            continue
        hits = {}
        for r in REGS:
            n = len(re.findall(r'\b' + r + r'\b', l))
            if n:
                hits[r] = hits.get(r, 0) + n
        for name, regs in acc.items():
            if decl_line.get(i) == name:
                continue                 # the wrapper's own definition
            n = len(re.findall(r'\b' + re.escape(name) + r'\s*\(', l))
            if not n:
                continue
            for r in regs:
                hits[r] = hits.get(r, 0) + n
        for r, n in hits.items():
            if bracketed[i]:
                res[r]['br'] += n
            else:
                res[r]['un'] += n
                res[r]['lines'].append(i + 1)
    return res, rawl, acc


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, '..', 'src', 'jit.cpp')
    res, rawl, acc = census(path)

    want = [a for a in sys.argv[1:] if not a.startswith('-')]
    show_all = '--all' in sys.argv

    if '--accessors' in sys.argv:
        print("Emitter methods whose NAME encodes a register "
              "(derived, not listed):\n")
        for name in sorted(acc):
            print("  %-24s %s" % (name, ' '.join(sorted(acc[name]))))
        print()

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
