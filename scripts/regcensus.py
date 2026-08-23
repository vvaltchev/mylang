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

    4. A REGISTER SPELLED INTO A modrm BYTE IS INVISIBLE TO EVERY ONE
    OF THE ABOVE. Corrections 1-3 all assume the register appears as a
    NAME somewhere - an operand, a method, an alias. It does not have
    to:

        e.u8(0x48); e.u8(0x8B); e.u8(0x00);   /* mov rax, [rax]     */
        e.u8(0x8B);  e.u8(0x89);              /* mov ecx, [rcx+...] */
        e.u8(0x4A); e.u8(0x8D); e.u8(0x14); e.u8(0xCA);
                                              /* lea rdx,[rdx+r9*8] */

    - the register is in the ENCODING. This is not hypothetical: the
    third of those shipped a SIGFPE (2026-08-19), because `r9` there is
    an ALLOCATED role the allocator moves, and no scan for r9 could see
    the site. Decoding modrm here would be a disassembler, and a
    half-right one is worse than none - so this script does the other
    thing an instrument may do, and SAYS IT DOES NOT KNOW: it counts the
    raw emissions and prints them as an explicit blind spot above the
    table. Convert them to a generic encoder and the number falls.

The output is a lower bound on the work, not an upper one: an
unbracketed site may still turn out to be an ABI setup whose bracket
this scan cannot see, and an accessor's IMPLICIT operands are invisible
to it (`idiv_rdi` also clobbers rax and rdx; `store_elem_int_rdi` reads
rcx and r9 - the encoding says so, the name does not). Read the sites
before trusting a count.

THE JUSTIFICATION RULE (maintainer-set, 2026-08-22). An emitter site
may demand a SPECIFIC register only with a HARD reason; everything
else must ask alloc_scratch(caps). A hard reason is declared ON THE
LINE with a tag inside a comment:

    reg:isa     the INSTRUCTION requires it (cl shift counts,
                rax:rdx for cqo/idiv/mul, movabs-A forms, ...)
    reg:abi     a calling convention outside a prologue bracket
                (the fragment's own entry/exit protocol: the resume
                pc returned in eax, EnterNative's status, ret values)
    reg:conv    a documented fragment-WIDE convention that functions
                as an internal ABI (the t_int/t_float singletons in
                rsi/r8 that helpers rely on)

A tag with any other reason word is an ERROR; a tag on a line with no
hardcoded register is reported STALE (the norec-coverage exemption
pattern - a marker that cannot rot silently).

THE FUNCTION-SCOPE FORM `reg:conv(fn)` / `reg:abi(fn)` sits on a
FUNCTION DEFINITION line and justifies every site until the next
top-level `}` - for the call-protocol emitters
(emit_sync_push_native / emit_sync_call_inline / emit_ret_native),
where EVERY register is protocol: a MyLang call's clobber mask denies
the whole caller-saved pool in any run containing one, so no pin can
exist where they emit, and tagging their ~150 lines one by one would
say less than the one sentence above. The DELIBERATE TRADE: a future
unjustified site added inside such a function escapes the ratchet -
which is why the form is reserved for functions that are wholly
protocol, and why a region with ZERO register sites is reported
STALE like a line tag. The table splits the
unbracketed column into justified / UNJUSTIFIED, and UNJUSTIFIED is
the work. `--gate` compares each register's UNJUSTIFIED count against
scripts/regcensus_floor.txt and FAILS on any increase - the ratchet:
a conversion batch lowers the floor in the same commit, a regression
cannot land silently.
"""

import re
import sys
import os

# R8/R9 have TWO spellings in jit.cpp: the `Reg` enum members (R8, R9)
# and the function-local `const uint8_t R8R/R9R` the older emitters
# declare. Both must be counted, or a conversion that switches from
# one to the other looks like the uses vanished - which happened:
# rewriting cmp_r9_rdx() as cmp_rr(9, RDX) briefly dropped R9 from
# 87 to 23 while changing nothing, because a bare `9` is as invisible
# as the wrapper name was. jit.cpp now names them; this counts both.
REGS = ['RAX', 'RCX', 'RDX', 'RSI', 'RDI', 'R8', 'R9', 'R10', 'R11',
        'R8R', 'R9R']
REPORT = ['RAX', 'RCX', 'RDX', 'RSI', 'RDI', 'R8', 'R9', 'R10', 'R11']
# report R8/R8R (and R9/R9R, R10/R11) as one register each
MERGE = {'R8R': 'R8', 'R9R': 'R9'}

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

# The register NUMBER each REPORTable name denotes, so an alias constant
# can be resolved to one.
REG_NUM = {0: 'RAX', 1: 'RCX', 2: 'RDX', 6: 'RSI', 7: 'RDI',
           8: 'R8R', 9: 'R9R', 10: 'R10', 11: 'R11'}

ALIAS_DECL = re.compile(
    r'^\s*static\s+constexpr\s+uint8_t\s+(\w+)\s*=\s*(\d+)\s*;')

# A function-local multi-declarator alias line (`const uint8_t R10 = 10,
# R11 = 11;`) DECLARES names, it does not use registers - skip it like
# the enum's member list (each declared NAME's uses are still counted,
# R8R/R9R being in REGS).
MULTI_ALIAS_DECL = re.compile(
    r'^\s*(?:static\s+)?const(?:expr)?\s+uint8_t\s+\w+\s*=\s*\d+'
    r'(?:\s*,\s*\w+\s*=\s*\d+)*\s*;')


def derive_aliases(lines):
    """alias constant -> register, DERIVED from its declaration.

    ⛔ THE SIXTH AUDIT-TABLE SHAPE, ONE LEVEL DEEPER THAN CORRECTION 3.
    That one caught `lea_rdi` - the register in the METHOD NAME. This
    catches the register behind a named CONSTANT:

        static constexpr uint8_t REG_ARG0 = 7;   /* rdi */
        ...
        push_reg(REG_ARG0);        mov_rr(REG_SLOTS_BASE, REG_ARG0);

    Three real code uses of rdi that `\bRDI\b` cannot match and that no
    accessor name encodes. The census reported RDI at 15 sites while the
    truth was 18, and RDI is exactly the register #96 step 2 was about to
    admit to the pin pool on the strength of that count.

    Same failure as r9 (admitted while ~80 sites were invisible, a
    shipping wrong answer for a day), same fix as correction 3: DERIVE
    the mapping from the declaration rather than hand-listing it, so a
    new alias is counted the day it is written."""
    out = {}
    for l in lines:
        m = ALIAS_DECL.match(l)
        if m and int(m.group(2)) in REG_NUM:
            out[m.group(1)] = REG_NUM[int(m.group(2))]
    return out


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


RAW_INSN = re.compile(r'\b\w+\.?u8\(0x[0-9A-Fa-f]+\)\s*;\s*'
                      r'\w*\.?u8\(0x[0-9A-Fa-f]+\)')


def raw_encodings(lines):
    """Lines that emit two or more literal bytes in a row - a
    hand-encoded instruction, whose register operands live in the
    modrm/SIB bytes and are therefore invisible to every count below."""
    return [i + 1 for i, l in enumerate(lines) if RAW_INSN.search(l)]


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
    aliases = derive_aliases(lines)
    # An accessor's own DECLARATION is not a use of the register.
    decl_line = {}
    for i, l in enumerate(lines):
        m = METHOD_DECL.match(l)
        if m and m.group(1) in acc:
            decl_line[i] = m.group(1)

    res = {MERGE.get(r, r): {'br': 0, 'un': 0, 'ju': 0, 'lines': []}
           for r in REGS}
    res['__raw__'] = raw_encodings(lines)
    TAG = re.compile(r'reg:(\w+)(\(fn\))?')
    tag_errors, stale_tags = [], []
    fn_region = False            # inside a reg:xxx(fn) function body
    fn_region_line = 0
    fn_region_hits = 0
    ALLOC_API = re.compile(r'\balloc_scratch\s*\(|\.take\s*\(|'
                           r'\btake\s*\(|\bfree_scratch\s*\(')
    in_reg_enum = False
    for i, l in enumerate(lines):
        if 'enum Reg' in l:
            in_reg_enum = True
        if in_reg_enum:
            # the enum's own member list, INCLUDING continuation lines
            # (the single-line test missed `RDI = 7, R8 = 8, ... };`)
            if '};' in l:
                in_reg_enum = False
            continue
        # A register mention on an ALLOCATOR-API line is the model's
        # INPUT - a prefer/exclude mask (`alloc_scratch(caps, 1u << RDX)`
        # keeps the legacy emission byte-identical while the register is
        # free) - not a bypass. Counting it would punish exactly the
        # conversion the mandate demands.
        if ALLOC_API.search(l):
            continue
        if MULTI_ALIAS_DECL.match(l):
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
        # alias constants (REG_ARG0 == rdi, ...), skipping the line that
        # DECLARES the alias - that is not a use.
        if not ALIAS_DECL.match(l):
            for name, r in aliases.items():
                n = len(re.findall(r'\b' + re.escape(name) + r'\b', l))
                if n:
                    hits[r] = hits.get(r, 0) + n
        # the justification tag lives in a COMMENT, so it is read from
        # the RAW line (the scrub removed it from `l`)
        if fn_region and l.startswith('}'):
            if fn_region_hits == 0:
                stale_tags.append(fn_region_line)
            fn_region = False
        m = TAG.search(rawl[i])
        tag = m.group(1) if m else None
        if tag is not None and tag not in ('isa', 'abi', 'conv'):
            tag_errors.append((i + 1, tag))
            tag = None
        if tag is not None and m.group(2):     # the (fn) region form
            fn_region = True
            fn_region_line = i + 1
            fn_region_hits = 0
        if fn_region and hits:
            fn_region_hits += len(hits)
            tag = tag or 'conv'                # region-justified
        if tag is not None and not hits and not fn_region:
            stale_tags.append(i + 1)
        for r0, n in hits.items():
            r = MERGE.get(r0, r0)
            if bracketed[i]:
                res[r]['br'] += n
            elif tag is not None:
                res[r]['ju'] += n
            else:
                res[r]['un'] += n
                res[r]['lines'].append(i + 1)
    res['__tag_errors__'] = tag_errors
    res['__stale_tags__'] = stale_tags
    return res, rawl, acc


def read_floor(path):
    floor = {}
    for l in open(path):
        l = l.split('#', 1)[0].strip()
        if l:
            k, v = l.split()
            floor[k] = int(v)
    return floor


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

    raw = res.pop('__raw__')
    tag_errors = res.pop('__tag_errors__')
    stale_tags = res.pop('__stale_tags__')
    rc = 0
    if tag_errors:
        rc = 1
        for n, t in tag_errors:
            print("⛔ UNKNOWN reg: tag '%s' at line %d "
                  "(isa/abi/conv only)" % (t, n))
    if stale_tags:
        rc = 1
        for n in stale_tags:
            print("⛔ STALE reg: tag at line %d - no hardcoded register "
                  "on the line; delete or fix it" % n)
    if raw:
        print("⛔ BLIND SPOT: %d line(s) emit a hand-encoded instruction "
              "(two or more\n   literal bytes in a row). Their register "
              "operands are in the modrm/SIB\n   bytes, so EVERY count "
              "below misses them - the table is a LOWER BOUND.\n"
              "   `scripts/regcensus.py --raw` lists them.\n" % len(raw))

    print("hardcoded scratch registers in src/jit.cpp "
          "(code only; comments/strings removed)\n")
    print("%-6s %10s %10s %12s %7s"
          % ("reg", "bracketed", "justified", "UNJUSTIFIED", "total"))
    tb = tj = tu = 0
    for r in REPORT:
        b, j, u = res[r]['br'], res[r]['ju'], res[r]['un']
        tb += b
        tj += j
        tu += u
        print("%-6s %10d %10d %12d %7d" % (r, b, j, u, b + j + u))
    print("%-6s %10d %10d %12d %7d" % ("TOTAL", tb, tj, tu, tb + tj + tu))
    print("\nbracketed = inside a call prologue/epilogue (the spill "
          "covers it);\njustified = tagged reg:isa / reg:abi / reg:conv "
          "on the line.\nUNJUSTIFIED is the work: it must reach ZERO "
          "(maintainer mandate,\n2026-08-22) - every such site converts "
          "to alloc_scratch(caps).")

    if '--gate' in sys.argv:
        here2 = os.path.dirname(os.path.abspath(__file__))
        floor = read_floor(os.path.join(here2, 'regcensus_floor.txt'))
        for r in REPORT + ['TOTAL', 'RAWENC']:
            got = (tu if r == 'TOTAL'
                   else len(raw) if r == 'RAWENC'
                   else res[r]['un'])
            want_ = floor.get(r)
            if want_ is None:
                print("gate: no floor for %s" % r)
                rc = 1
            elif got > want_:
                print("⛔ GATE: %s UNJUSTIFIED %d > floor %d - a new "
                      "hardcoded site landed" % (r, got, want_))
                rc = 1
            elif got < want_:
                print("gate: %s improved (%d < floor %d) - LOWER the "
                      "floor in scripts/regcensus_floor.txt in this "
                      "commit" % (r, got, want_))
                rc = 1
        if rc == 0:
            print("gate: every register at its floor.")
        sys.exit(rc)
    if rc:
        sys.exit(rc)

    if '--raw' in sys.argv:
        print("\n=== hand-encoded instructions (%d) ===" % len(raw))
        for n_ in raw:
            print("%6d: %s" % (n_, rawl[n_ - 1].strip()[:92]))

    for r in REPORT:
        if not (show_all or r in want):
            continue
        print("\n=== %s unbracketed sites (%d) ===" % (r, res[r]['un']))
        for n in res[r]['lines']:
            print("%6d: %s" % (n, rawl[n - 1].strip()[:92]))


if __name__ == '__main__':
    main()
