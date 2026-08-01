#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Validate docs/myv-format.txt by parsing real images with ONLY the document
as the specification.

This tests the DOCUMENT, not the implementation: it was written from
docs/myv-format.txt and deliberately NOT from src/serialize.cpp, so if it
consumes an image to exactly EOF then the document is complete and correct.
A leftover byte, or a failure, means the doc and the format have drifted -
which is exactly what a reader of the doc would hit.

Usage:
    mylang -c samples/gcd -o /tmp/gcd.myv
    tests/myv_doc_check.py /tmp/*.myv

Stdlib only (the no-dependencies rule). Exits nonzero if any image fails.
Keep it in step with docs/myv-format.txt, NOT with serialize.cpp - a change
made by reading the C++ would defeat the purpose.
"""
import struct, sys

class R:
    def __init__(self, b): self.b = b; self.p = 0
    def u8(self):  v = self.b[self.p]; self.p += 1; return v
    def _n(self, fmt, n):
        v = struct.unpack_from(fmt, self.b, self.p)[0]
        self.p += n
        return v
    def u16(self): return self._n('<H', 2)
    def u32(self): return self._n('<I', 4)
    def i32(self): return self._n('<i', 4)
    def i64(self): return self._n('<q', 8)
    def f64(self): return self._n('<d', 8)
    def boolv(self): return self.u8() != 0
    def raw(self):
        n = self.u32(); v = self.b[self.p:self.p+n]; self.p += n; return v
    def sid(self): return self.u32()
    def uid(self): return self.u32()
    def sref(self): return self.u32()
    def dref(self): return self.u32()
    def loc(self):
        line = self.u16()
        if line == 0xffff: line = self.i32()
        col = self.u8()
        if col == 0xff: col = self.i32()
        return (line, col)
    def arglocs(self):
        return [(self.loc(), self.loc()) for _ in range(self.u32())]
    def nx(self, f):
        return [f() for _ in range(self.u32())]

# ---- section 11: the value codec ----------------------------------------
def value(r):
    t = r.u8()
    if t == 0: return None
    if t == 1: return r.boolv()
    if t == 2: return r.i64()
    if t == 3: return r.f64()
    if t == 4: return r.raw()
    if t == 5:                                    # arr
        kind = r.u8(); r.boolv(); n = r.u32()
        if kind == 1: return [r.i64() for _ in range(n)]
        if kind == 2: return [r.f64() for _ in range(n)]
        if kind == 3: return [r.boolv() for _ in range(n)]
        if kind == 5: return [r.raw() for _ in range(n)]
        if kind == 4:                             # structs: def, stride, bytes
            r.sref(); stride = r.u32(); r.p += n * stride; return '<structarr>'
        return [value(r) for _ in range(n)]       # 0 general
    if t == 6:                                    # dict
        r.boolv()
        if r.boolv(): value(r)
        return [(value(r), value(r)) for _ in range(r.u32())]
    if t == 7:                                    # strct
        r.sref(); r.boolv(); pod = r.boolv()
        if pod:
            n = r.u32(); r.p += n
        else:
            for _ in range(r.u32()): value(r)
        return '<struct>'
    if t == 8: return ('structtype', r.sref())
    if t == 9: return ('func', r.dref())
    raise ValueError('bad value tag %d at %d' % (t, r.p))

# ---- section 9: a chunk -------------------------------------------------
def chunk(r):
    ncode = r.u32()
    for _ in range(ncode):                        # 9.1 compact instructions
        r.u8()                                    # op
        flags = r.u16()
        assert (flags & 0xf000) == 0, 'reserved instr flag bits set'
        wa, wb = flags & 7, (flags >> 3) & 7
        wt, wt2 = (flags >> 6) & 3, (flags >> 8) & 3
        if flags & (1 << 10): r.u8()              # aop
        if flags & (1 << 11): r.u8()              # opflags
        for w in (wt, wt2, wa, wb):
            if w: r.p += 1 << (w - 1)
    for _ in range(5): r.u32()                    # slot_count..n_trys
    r.nx(r.u32)                                   # ref_slots
    r.nx(lambda: value(r))                        # consts
    for _ in range(r.u32()):                      # 9.2 delta loc table
        if r.u8() == 0xff: r.i32()                # pcd   (u8, 255 escapes)
        if r.u8() == 0x80: r.i32()                # lined (i8, -128 escapes)
        if r.u8() == 0xff: r.i32()                # col   (u8, 255 escapes)
        if r.u8() == 0xff: r.i32(); r.i32()       # ecol escape: full end Loc
    for _ in range(r.u32()):                      # 9.3 inline_frames
        r.sid(); r.nx(r.sid); r.loc(); r.u32()
    for _ in range(r.u32()): r.u32(); r.u32()     # inline_ctxs
    for _ in range(r.u32()):                      # 9.4 member_keys
        value(r); r.uid(); r.boolv()
        r.loc(); r.loc(); r.loc(); r.loc()
        r.sref(); r.u32()
    for _ in range(r.u32()):                      # 9.5 ctor_plans
        for _ in range(r.u32()): r.u32(); r.u32(); r.u8()
    for _ in range(r.u32()):                      # 9.6 incdec_sites
        r.loc(); r.loc(); r.loc(); r.loc(); value(r); r.uid()
    def steps():
        for _ in range(r.u32()):
            r.boolv(); r.u32(); r.loc(); r.loc()
    for _ in range(r.u32()): steps()              # 9.7 chain_steps
    for _ in range(r.u32()):                      # 9.7 incdec_chains
        steps()
        for _ in range(4): r.boolv()
        r.loc(); r.loc(); r.loc(); r.loc()
    for _ in range(r.u32()):                      # 9.8 chain_locs
        for _ in range(r.u32()): r.loc(); r.loc()
    for _ in range(r.u32()): r.nx(r.sid)          # 9.9 catch_types
    for _ in range(r.u32()):                      # 9.10 literal_objs
        value(r); r.boolv(); r.u8(); r.sref()
    r.nx(r.dref)                                  # closure_defs
    r.nx(r.sref)                                  # struct_defs
    for _ in range(r.u32()): r.sref(); r.arglocs()          # 9.11 boxed_ctors
    for _ in range(r.u32()):                                # 9.12 emplace
        r.sref(); r.uid(); r.loc(); r.loc(); r.arglocs()
    for _ in range(r.u32()): r.u8(); r.loc(); r.loc(); r.uid()   # 9.13 throws
    for _ in range(r.u32()): r.nx(r.u32)          # 9.14 unpack_targets
    for _ in range(r.u32()): r.nx(r.u8)           # 9.14 unpack_coerce
    for _ in range(r.u32()):                      # 9.15 builtin_calls
        r.uid(); r.loc(); r.loc(); r.u8(); r.arglocs(); r.uid()
    for _ in range(r.u32()):                      # 9.16 call_sites
        r.loc(); r.loc(); r.arglocs()
        r.u8(); r.u8(); r.u32(); r.u32(); r.u32(); r.uid()
    r.nx(r.sid)                                   # 9.17 slot_names

def main(path):
    r = R(open(path, 'rb').read())
    assert r.b[:4] == b'MYLV', 'bad magic'
    r.p = 4
    ver, endian = r.u32(), r.u32()
    assert endian == 0x01020304, 'bad endian mark'
    r.i64()                                       # builtin fingerprint
    root, rel, abs_, crc, size = (r.raw(), r.raw(), r.raw(), r.u32(), r.i64())
    strs = r.nx(r.raw)                            # section 6
    nstructs = r.u32()                            # section 7
    for _ in range(nstructs):
        r.uid()
        for _ in range(r.u32()):                  # fields
            r.uid(); r.u8(); r.uid(); r.sref(); r.boolv(); r.u32()
        for _ in range(r.u32()): r.uid(); value(r)     # consts
    ndesc = r.u32()                               # section 8
    has_chunk = []
    for _ in range(ndesc):
        r.uid(); r.sid()
        for _ in range(r.u32()):                  # params
            r.uid(); r.boolv(); r.boolv(); r.boolv(); r.u8()
        for _ in range(r.u32()): r.uid(); r.u8(); r.u32()   # captures
        r.boolv(); r.u32(); r.u32()
        for _ in range(6): r.boolv()   # pure/effective/cache/pure_ctx/tmpl/fast
        has_chunk.append(r.boolv())
    chunk(r)                                      # section 9: root
    nfunc = 0
    for hc in has_chunk:
        if hc: chunk(r); nfunc += 1
    r.nx(r.u8)                                    # section 10
    r.u32(); r.nx(r.uid)

    left = len(r.b) - r.p
    print("%-22s v%d  %6d bytes  strings %3d  structs %d  descs %d "
          "(chunks %d)  src=%s" % (path.split('/')[-1], ver, len(r.b),
          len(strs), nstructs, ndesc, 1 + nfunc,
          rel.decode() or '(stripped)'))
    if left:
        print("   *** %d BYTES LEFT OVER - the doc is INCOMPLETE ***" % left)
    return left

if __name__ == '__main__':
    bad = 0
    for p in sys.argv[1:]:
        try:
            bad += 1 if main(p) else 0
        except Exception as e:
            print("%-22s FAILED: %s" % (p.split('/')[-1], e)); bad += 1
    sys.exit(1 if bad else 0)
