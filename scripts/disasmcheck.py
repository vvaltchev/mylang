#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
disasmcheck.py - is `-vdj` DECODING CORRECTLY, judged by a second
                 disassembler?

    scripts/disasmcheck.py BINARY [--matrix] [-v]

⛔ WHY THIS EXISTS, AND WHY THE EXISTING NETS CANNOT REPLACE IT.

`-vdj` self-reports `DUMP IS UNRELIABLE` when it meets a byte it cannot
decode, and the `jit: -vdj decodes every emitted form` -rt check
compiles a set of programs and requires that banner never to appear.
Both catch the SAME failure: a byte we know we failed on.

Neither can catch a byte sequence we decode CONFIDENTLY AND WRONGLY -
and that is the failure that actually cost weeks. Until 2026-08-17 the
SIB arm never consumed its displacement, so `mov rdi, [rsp+8]` printed
as `mov rdi, [rsp+rsp*8]`: well-formed, plausible, wrong, and it
desynchronised every instruction after it. A decoder cannot check
itself. Only a SECOND decoder can.

So this runs `MYLANG_VDJ_HEX=1 -vdj`, takes the RAW BYTES the dump now
carries beside each instruction, and hands each fragment to **objdump**
(`-b binary -m i386:x86-64 -M intel`). It then compares, per fragment:

  1. INSTRUCTION BOUNDARIES - the lengths objdump assigns must be
     exactly the lengths we assigned. This is the desync check, and it
     is the one that matters most: a wrong length makes every later
     mnemonic wrong.
  2. MNEMONICS - our text vs objdump's, normalised (see `norm`) because
     the two spell operands differently ON PURPOSE: `-vdj` prints frame
     slots by NAME, baked pointers as `<addr>`/`<int-tag>`/`<helper>`,
     and rel32 targets as absolute offsets. Only the OPCODE and the
     register/memory SHAPE are compared; a difference there is a real
     decode bug.

objdump is a development-time tool invoked by a script, like python3 in
the other scripts here - not a build or test dependency of the
interpreter, and the no-dependency rule is unaffected.

`--matrix` sweeps the axes that change WHICH forms get emitted - both
arena configurations, every pin-pool rotation, and a range of pin
budgets - because a register only reachable at a high budget is
precisely the one whose REX-prefixed encoding nothing has ever decoded
(the r9 lesson, one level down).
"""

import os
import re
import subprocess
import sys

CORPUS_DIRS = [("bench/my", ".my"), ("samples", ""),
               ("tests/functional", ".my")]

INSN = re.compile(r'^\s+\.\s+\+\s*(\d+):\s+\{([0-9a-f]+)\}\s+(.*?)\s*$')
OBJD = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*?)\s*$')


def corpus():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    out = []
    for d, ext in CORPUS_DIRS:
        p = os.path.join(root, d)
        if not os.path.isdir(p):
            continue
        for f in sorted(os.listdir(p)):
            if ext and not f.endswith(ext):
                continue
            fp = os.path.join(p, f)
            if os.path.isfile(fp):
                out.append(fp)
    return out


def frags(binary, path, env):
    """[(start_off, [(off, bytes, mnemonic)])] - one entry per fragment.

    A fragment ENDS at the `end native` marker; instructions are only
    those lines carrying a {hex} block, so the surrounding bytecode
    listing is ignored."""
    e = dict(os.environ)
    e.update(env)
    e['MYLANG_VDJ_HEX'] = '1'
    r = subprocess.run([binary, '-vdj', path], capture_output=True,
                       text=True, env=e)
    if r.returncode != 0:
        return None
    out, cur = [], []
    for line in r.stdout.split('\n'):
        m = INSN.match(line)
        if m:
            cur.append((int(m.group(1)), m.group(2), m.group(3)))
            continue
        if 'end native' in line and cur:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    return out


def objdump_lens(blob):
    """{offset: (length, mnemonic)} from objdump over a raw blob."""
    import tempfile
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(blob)
        name = f.name
    try:
        r = subprocess.run(['objdump', '-D', '-b', 'binary',
                            '-m', 'i386:x86-64', '-M', 'intel',
                            '--insn-width=16', name],
                           capture_output=True, text=True)
    finally:
        os.unlink(name)
    #
    # ⛔ LENGTHS COME FROM CONSECUTIVE OFFSETS, NOT FROM COUNTING THE
    # BYTES ON THE LINE. objdump WRAPS a long instruction across lines,
    # so a 10-byte `movabs rax, imm64` shows 7 bytes on its first line
    # and 3 on a continuation. Counting them reported a length error on
    # EVERY fragment's entry sequence - 206 false positives, and the
    # first version of this script printed them as decoder bugs. An
    # instrument's first run should be assumed to be measuring itself.
    #
    starts = []
    for line in r.stdout.split('\n'):
        m = OBJD.match(line)
        #
        # ⛔ AND A CONTINUATION LINE IS NOT AN INSTRUCTION. When objdump
        # wraps, the overflow bytes get their OWN `offset:` line with an
        # EMPTY mnemonic - which the offset-difference fix above duly
        # counted as an instruction start, so a 10-byte movabs still
        # read as 7. Second false alarm from the same wrap, and both
        # accused the decoder. `--insn-width=16` above stops the
        # wrapping; this stays as the belt to that braces.
        #
        if not m or not m.group(3).strip():
            continue
        starts.append((int(m.group(1), 16), m.group(3)))
    res = {}
    for i, (off, mn) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(blob)
        res[off] = (end - off, mn)
    return res


# Operand spellings the two tools deliberately disagree on. We compare
# the OPCODE and the register/memory SHAPE; everything numeric or named
# is erased, because -vdj prints slots by name and pointers as <addr>.
def norm(s):
    s = s.split(';')[0].strip().lower()
    s = re.sub(r'\b(qword|dword|word|byte)\s+ptr\b', '', s)
    s = re.sub(r'0x[0-9a-f]+', 'K', s)
    s = re.sub(r'<[^>]*>', 'K', s)
    s = re.sub(r'\b\d+\b', 'K', s)
    s = re.sub(r'[\s,]+', ' ', s)
    return s.strip()


def check(binary, env, files, verbose):
    bad_len = bad_mn = insns = frag_n = 0
    for path in files:
        fs = frags(binary, path, env)
        if fs is None:
            continue
        for ins in fs:
            frag_n += 1
            base = ins[0][0]
            blob = b''.join(bytes.fromhex(b) for _, b, _ in ins)
            od = objdump_lens(blob)
            for i, (off, hx, mn) in enumerate(ins):
                insns += 1
                rel = off - base
                if rel not in od:
                    bad_len += 1
                    print("BOUNDARY %s +%d: objdump has no instruction "
                          "starting here (we said %d bytes: %s)"
                          % (path, off, len(hx) // 2, mn))
                    break
                olen, omn = od[rel]
                if olen != len(hx) // 2:
                    bad_len += 1
                    print("LENGTH %s +%d: we %d bytes (%s), objdump %d (%s)"
                          % (path, off, len(hx) // 2, mn, olen, omn))
                    break
                a, b = norm(mn), norm(omn)
                if a.split(' ')[0] != b.split(' ')[0]:
                    bad_mn += 1
                    if verbose or bad_mn <= 20:
                        print("MNEMONIC %s +%d: {%s} we %-28r objdump %r"
                              % (path, off, hx, mn, omn))
    return bad_len, bad_mn, insns, frag_n


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    if not args:
        print(__doc__.strip().split('\n')[2], file=sys.stderr)
        print("usage: disasmcheck.py BINARY [--matrix] [-v]",
              file=sys.stderr)
        return 2
    binary = args[0]
    verbose = '-v' in sys.argv
    files = corpus()

    envs = [({}, 'default')]
    if '--matrix' in sys.argv:
        envs.append(({'MYLANG_NO_LOWMEM': '1'}, 'no-lowmem'))
        for r in range(7):
            envs.append(({'MYLANG_JIT_XROT': str(r)}, 'xrot=%d' % r))
        for p in (4, 6, 8, 10, 11):
            envs.append(({'MYLANG_JIT_MAXPINS': str(p)}, 'maxpins=%d' % p))

    tl = tm = ti = tf = 0
    for env, name in envs:
        bl, bm, n, fn = check(binary, env, files, verbose)
        print("%-12s %6d insns in %4d frags   boundary-errors %d   "
              "mnemonic-errors %d" % (name, n, fn, bl, bm))
        tl += bl
        tm += bm
        ti += n
        tf += fn

    print("\nTOTAL %d instructions, %d fragments" % (ti, tf))
    print("  boundary errors: %d   mnemonic errors: %d" % (tl, tm))
    if tl or tm:
        print("\n⛔ the disassembler DISAGREES with objdump. A boundary "
              "error means\n   every mnemonic after it in that fragment "
              "is read at the wrong\n   offset - fix decode_one.",
              file=sys.stderr)
        return 1
    if ti == 0:
        print("\n⛔ VACUOUS: no instructions were compared. Is "
              "MYLANG_VDJ_HEX honoured?", file=sys.stderr)
        return 2
    print("  every instruction agrees with objdump.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
