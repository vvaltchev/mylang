#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
disasmcheck.py - is `-vdj` DECODING CORRECTLY, judged by a second
                 disassembler?

    scripts/disasmcheck.py BINARY [--matrix] [--env K=V[,K=V...]]...
                           [--only FILE|DIR]... [--shard I/N] [-v]

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

`--env K=V[,K=V...]` adds ONE more configuration with that environment
(repeatable). It is how a run reaches what no matrix axis does - an
intrusive-test build's forced picks (`MYLANG_INT_CHOOSE`), which put a
float stage in xmm8-15 and so emit the REX-prefixed SSE forms no default
run produces (`pxor xmm10, xmm10` was undecoded for exactly that
reason). `--only FILE|DIR` restricts the corpus (repeatable), since a
forced pick names a site in ONE program (a DIR selects every corpus file
under it). A value may hold commas (`MYLANG_INT_CHOOSE=k1=a,k2=b`): a
comma starts a new variable only before `NAME=`.

`--shard I/N` (0 <= I < N) keeps every Nth configuration, starting at
the Ith, so N CI jobs split one `--matrix` run between them (the
configurations are dealt round-robin, so neighbouring xrot values land
in different shards). Each shard checks its own result in full -
including the VACUOUS guards - so a shard left with no work fails
rather than passing.

Beyond the mnemonic, EVERY OPERAND is compared (3), from a second dump
taken with `MYLANG_VDJ_RAW=1`, which renders operands the way objdump
does: registers by their real names (a frame slot is `[rbx+0x30]`, not
`n`; a 32-bit register `r8d`), memory as base+index*scale+disp, baked
addresses and tags as numbers, branch and call targets as fragment
offsets. The comparison is per operand: register names exactly (GP and
xmm - a decoder that drops REX.R/REX.B/REX.X decodes the right mnemonic
on the WRONG register, `rax` for `r8`, which (2) passes), memory base,
index, scale and displacement, a `byte` size both ways, immediates (by
value, at objdump's operand width) and targets.

  3. OPERANDS - the RAW dump vs objdump, operand by operand. A
     rendering difference that is not a decode error belongs in the RAW
     mode (disasm.cpp `render_op_raw`), never in a normalisation here:
     this script compares, the tool renders.
"""

import os
import re
import subprocess
import sys

CORPUS_DIRS = [("bench/my", ".my"), ("samples", ""),
               ("tests/functional", ".my")]

INSN = re.compile(r'^\s+\.\s+\+\s*(\d+):\s+\{([0-9a-f]+)\}\s+(.*?)\s*$')
XMM = re.compile(r'\bxmm\d+\b')
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


def frags(binary, path, env, raw=False):
    """[(start_off, [(off, bytes, mnemonic)])] - one entry per fragment.

    A fragment ENDS at the `end native` marker; instructions are only
    those lines carrying a {hex} block, so the surrounding bytecode
    listing is ignored."""
    e = dict(os.environ)
    e.update(env)
    e['MYLANG_VDJ_HEX'] = '1'
    if raw:
        e['MYLANG_VDJ_RAW'] = '1'
    else:
        e.pop('MYLANG_VDJ_RAW', None)
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
                            '-w', '--insn-width=16', name],
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
    #
    # ⛔ WRAPPING IS PREVENTED, AND THEN CHECKED FOR. `-w` (--wide) plus
    # --insn-width=16 keep every instruction on one line. Without them
    # objdump splits a long one and gives the overflow bytes their OWN
    # `offset:` line with an EMPTY mnemonic - which cost this script two
    # rounds of false positives, both of which ACCUSED THE DECODER (a
    # 10-byte movabs read as 7, on 206 fragments).
    #
    # So a continuation line is not skipped, it is COUNTED and reported.
    # Silently tolerating it would leave the script working by accident
    # on an objdump whose wrapping the flags failed to suppress - the
    # workaround-in-the-consumer trap, one level up. If this ever fires
    # the ORACLE is misconfigured, which is a different verdict from
    # "the decoder is wrong", and the exit code says so.
    #
    starts, wrapped = [], 0
    for line in r.stdout.split('\n'):
        m = OBJD.match(line)
        if not m:
            continue
        if not m.group(3).strip():
            wrapped += 1
            continue
        starts.append((int(m.group(1), 16), m.group(3)))
    res = {}
    for i, (off, mn) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(blob)
        res[off] = (end - off, mn)
    return res, wrapped


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


SIZE = re.compile(r'\b(byte|word|dword|qword|xmmword|tbyte|oword|ymmword)'
                  r'(?:\s+ptr)?\s+')
REG = re.compile(r'^(?:[re]?[abcd]x|[abcd]l|[abcd]h|[re]?(?:si|di|sp|bp)'
                 r'|(?:si|di|sp|bp)l|r(?:[89]|1[0-5])[dwb]?|xmm\d+|rip'
                 r'|[cdefgs]s)$')
BRANCH = re.compile(r'^(j[a-z]+|call|loop[a-z]*|jmp)$')


def split_ops(s):
    """mnemonic, [operand text] - Intel syntax has no comma inside a
    memory operand, so a plain split is exact."""
    s = s.split(';')[0].split('#')[0].strip().lower()
    parts = s.split(None, 1)
    if not parts:
        return '', []
    mn = parts[0]
    rest = parts[1] if len(parts) > 1 else ''
    return mn, [o.strip() for o in rest.split(',') if o.strip()]


def num(t):
    return int(t, 16) if t.lstrip('-').startswith('0x') else int(t)


def parse_op(t):
    """An operand as a comparable tuple:
       ('reg', name) | ('mem', base, index, scale, disp, byte) |
       ('num', value) | ('?', text)."""
    m = SIZE.match(t)
    size = m.group(1) if m else None
    if m:
        t = t[m.end():]
    if t.startswith('ds:'):
        return ('mem', None, None, 1, num(t[3:]), size == 'byte')
    if t.startswith('[') and t.endswith(']'):
        base = index = None
        scale, disp = 1, 0
        for sign, term in re.findall(r'([+-]?)([^+-]+)', t[1:-1]):
            term = term.strip()
            if '*' in term:
                r, sc = term.split('*')
                index, scale = r.strip(), int(sc)
            elif REG.match(term):
                if base is None:
                    base = term
                else:
                    index = term
            else:
                v = num(term)
                disp += -v if sign == '-' else v
        return ('mem', base, index, scale, disp, size == 'byte')
    if REG.match(t):
        return ('reg', t)
    try:
        return ('num', num(t))
    except ValueError:
        return ('?', t)


def num_eq(ours, od):
    """an immediate: objdump prints it unsigned at the OPERAND's width,
    which the text does not carry - so equal at some width it fits."""
    for bits in (8, 16, 32, 64):
        if 0 <= od < (1 << bits) and ours % (1 << bits) == od:
            return True
    return ours == od


OPS_COMPARED = [0]


def ops_mismatch(raw, omn, base, off):
    """None when every operand agrees, else a short reason."""
    mn, a = split_ops(raw)
    _, b = split_ops(omn)
    if len(a) != len(b):
        return "operand count %d vs %d" % (len(a), len(b))
    for i, (x, y) in enumerate(zip(a, b)):
        OPS_COMPARED[0] += 1
        p, q = parse_op(x), parse_op(y)
        if p[0] != q[0]:
            return "operand %d kind %s vs %s" % (i, x, y)
        if p[0] == 'reg' and p[1] != q[1]:
            return "operand %d register %s vs %s" % (i, x, y)
        if p[0] == 'mem':
            if p[1:4] != q[1:4]:
                return "operand %d address %s vs %s" % (i, x, y)
            if (p[4] - q[4]) % (1 << 32):
                return "operand %d displacement %s vs %s" % (i, x, y)
            if p[5] != q[5]:
                return "operand %d byte size %s vs %s" % (i, x, y)
        if p[0] == 'num':
            if BRANCH.match(mn):
                if (p[1] - base - q[1]) % (1 << 64):
                    return "operand %d target %s vs %s" % (i, x, y)
            elif not num_eq(p[1], q[1]):
                return "operand %d immediate %s vs %s" % (i, x, y)
        if p[0] == '?' and x != y:
            return "operand %d unparsed %s vs %s" % (i, x, y)
    return None


def check(binary, env, files, verbose, mon=None):
    bad_len = bad_mn = bad_op = insns = frag_n = wraps = 0
    for path in files:
        if mon:
            mon.set_current(os.path.basename(path))
            mon.advance()
        fs = frags(binary, path, env)
        if fs is None:
            continue
        rs = frags(binary, path, env, raw=True)
        if rs is None or [[(o, len(h)) for o, h, _ in f] for f in rs] != \
                [[(o, len(h)) for o, h, _ in f] for f in fs]:
            #
            # The RAW dump must be the SAME instructions as the plain
            # one - it differs in rendering only. If it is not, the
            # operand check below would compare a different stream.
            # Offsets and LENGTHS, not bytes: the bytes hold baked
            # addresses, which differ between the two processes.
            #
            # And objdump is handed the RAW run's bytes, so the
            # addresses it prints are the ones the RAW dump printed.
            #
            bad_op += 1
            print("RAW %s: MYLANG_VDJ_RAW changed the decoded stream"
                  % path)
            rs = None
        for fi, ins in enumerate(fs):
            frag_n += 1
            base = ins[0][0]
            src = rs[fi] if rs is not None else ins
            blob = b''.join(bytes.fromhex(b) for _, b, _ in src)
            od, wr = objdump_lens(blob)
            wraps += wr
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
                if a.split(' ')[0] != b.split(' ')[0] \
                        or XMM.findall(a) != XMM.findall(b):
                    bad_mn += 1
                    if verbose or bad_mn <= 20:
                        print("MNEMONIC %s +%d: {%s} we %-28r objdump %r"
                              % (path, off, hx, mn, omn))
                    continue
                if rs is None:
                    continue
                raw = rs[fi][i][2]
                why = ops_mismatch(raw, omn, base, rel)
                if why:
                    bad_op += 1
                    if verbose or bad_op <= 20:
                        print("OPERAND %s +%d: {%s} %s: we %r objdump %r"
                              % (path, off, hx, why, raw, omn))
    return bad_len, bad_mn, bad_op, insns, frag_n, wraps


ENV_NAME = re.compile(r'^[A-Z_][A-Z0-9_]*=')


def parse_env(spec):
    """`K=V[,K=V...]` -> dict. A comma starts a new variable only when
    what follows is `NAME=`: a value may itself hold commas -
    `MYLANG_INT_CHOOSE=main@0/fp#0=11,main@21/fp#0=11` is ONE variable
    with two keys, and splitting it at every comma used to turn the
    second key into an environment variable of its own, silently
    dropping the pick."""
    out, cur = {}, None
    for part in spec.split(','):
        if ENV_NAME.match(part) or cur is None:
            k, _, v = part.partition('=')
            out[k] = v
            cur = k
        else:
            out[cur] += ',' + part
    return out


def main():
    argv = sys.argv[1:]
    extra, only, args = [], [], []
    shard = (0, 1)
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--env', '--only', '--shard') and i + 1 < len(argv):
            if a == '--shard':
                m = re.fullmatch(r'(\d+)/(\d+)', argv[i + 1])
                if not m or not 0 <= int(m.group(1)) < int(m.group(2)):
                    print("error: --shard wants I/N with 0 <= I < N",
                          file=sys.stderr)
                    return 2
                shard = (int(m.group(1)), int(m.group(2)))
            elif a == '--env':
                extra.append((parse_env(argv[i + 1]), argv[i + 1]))
            else:
                only.append(os.path.abspath(argv[i + 1]))
            i += 2
            continue
        if not a.startswith('-'):
            args.append(a)
        i += 1
    if not args:
        print(__doc__.strip().split('\n')[2], file=sys.stderr)
        print("usage: disasmcheck.py BINARY [--matrix] "
              "[--env K=V[,K=V...]]... [--only FILE|DIR]... [-v]",
              file=sys.stderr)
        return 2
    binary = args[0]
    verbose = '-v' in sys.argv
    try:
        subprocess.run(['objdump', '--version'], capture_output=True)
    except FileNotFoundError:
        print("error: objdump not found. This script's whole point is to "
              "compare\n       our decoder against an INDEPENDENT one; "
              "without it there is no\n       check to run. Install "
              "binutils.", file=sys.stderr)
        return 2
    files = corpus()
    if only:
        # a FILE, or a DIRECTORY meaning every corpus file under it
        files = [f for f in files
                 if any(os.path.abspath(f) == o
                        or os.path.abspath(f).startswith(o + os.sep)
                        for o in only)]
        if not files:
            print("error: --only matched no corpus file", file=sys.stderr)
            return 2

    envs = [({}, 'default')]
    if '--matrix' in sys.argv:
        envs.append(({'MYLANG_NO_LOWMEM': '1'}, 'no-lowmem'))
        for r in range(16):
            envs.append(({'MYLANG_JIT_XROT': str(r)}, 'xrot=%d' % r))
        for p in (4, 6, 8, 10, 11):
            envs.append(({'MYLANG_JIT_MAXPINS': str(p)}, 'maxpins=%d' % p))
    envs.extend(extra)
    envs = envs[shard[0]::shard[1]]
    if shard[1] > 1:
        print("shard %d/%d: %s" % (shard[0], shard[1],
                                   ', '.join(n for _, n in envs)))

    tl = tm = to = ti = tf = tw = 0
    # progress: tests/testctl.py lists this run with its percentage
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(
        __file__)), "..", "tests"))
    from testrun import Monitor
    mon = Monitor("disasmcheck", total=len(envs) * len(files),
                  phase="configs").__enter__()
    for env, name in envs:
        mon.set_extra(config=name)
        bl, bm, bo, n, fn, wr = check(binary, env, files, verbose, mon)
        print("%-12s %6d insns in %4d frags   boundary-errors %d   "
              "mnemonic-errors %d   operand-errors %d%s"
              % (name, n, fn, bl, bm, bo,
                 "   ⛔ %d WRAPPED objdump lines" % wr if wr else ""))
        tl += bl
        tm += bm
        to += bo
        ti += n
        tf += fn
        tw += wr

    mon.__exit__(None, None, None)
    print("\nTOTAL %d instructions, %d fragments" % (ti, tf))
    print("  boundary errors: %d   mnemonic errors: %d   "
          "operand errors: %d (of %d operands compared)"
          % (tl, tm, to, OPS_COMPARED[0]))
    if tw:
        print("\n⛔ objdump WRAPPED %d instruction(s) across lines despite "
              "-w and\n   --insn-width=16. The comparison below cannot be "
              "trusted: a wrapped\n   line parses as a second, empty "
              "instruction. THE ORACLE is misconfigured,\n   not the "
              "decoder - fix the objdump invocation." % tw, file=sys.stderr)
        return 2
    if tl or tm or to:
        print("\n⛔ the disassembler DISAGREES with objdump. A boundary "
              "error means\n   every mnemonic after it in that fragment "
              "is read at the wrong\n   offset - fix decode_one.",
              file=sys.stderr)
        return 1
    if ti == 0:
        print("\n⛔ VACUOUS: no instructions were compared. Is "
              "MYLANG_VDJ_HEX honoured?", file=sys.stderr)
        return 2
    if OPS_COMPARED[0] == 0:
        print("\n⛔ VACUOUS: no operands were compared. Is "
              "MYLANG_VDJ_RAW honoured?", file=sys.stderr)
        return 2
    print("  every instruction agrees with objdump.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
