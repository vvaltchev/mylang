# Inline the fused `ord(s[i])` read (30_str_index_iterate, 27x my/cpp)

**Status: STEP 1 LANDED (the self-verifying probe, inert); steps 2-6
next.** Everything needed to execute the rest is here, including the one
hazard that decides the shape.

**What step 1 measured on this toolchain** (gcc 11.4 / libstdc++):
`slice` at +16, `obj` at +0, the char pointer at StrObj+8, and the
self-check **PASSES** - `MYLANG_JITSTATS` reports `str_probe_ok 1`.
Watched failing: adding 8 to the computed offset takes it to
`str_probe_ok 0`, and the program still prints the right answer because
the tier simply does not engage. The emitted code is byte-identical
(modulo ASLR-baked addresses) to before the step, as an inert step must
be.

## Why this one

The census (plans/cpp-gap-ladder.md) ranked the corpus by
STARTUP-CORRECTED my/cpp and classified each worst loop's emitted body.
`30_str_index_iterate` is **the worst at 27.36x**, and it is the
simplest of the eight: its entire inner loop is

    986: mov rsi, r13          ; i - already a cache-aware VALUE arg
    989: lea rdx, r7           ; &dst temp
    996: lea rdi, s2           ; &base slot
   1003: call <jit_ord_char>
   1008: movabs rsi, <int-tag>    ; the call clobbered the pinned
   1018: movabs r8,  <float-tag>  ; type-tag singletons - reload both
   1028: test rax, rax            ; status
   1030: je   +1103
   ...  (the conveyed-exception path)
   1103: mov rax, r12          ; total
   1106: mov rcx, r7           ; reload what the helper stored
   1113: add rax, rcx
   1116: mov r12, rax

**A function call per character, where C++ does one load.** Measured
**6.3 ns/char against C++'s 0.23**.

## The prediction (stated BEFORE building, per the ladder's discipline)

  - REMOVES: a real `call` and its body - a `get_ref<SharedStr>` type
    check, a `string_view` construction with its ML_CHECK, `dst->put`
    (two stores) - plus the two `movabs` singleton reloads, the status
    test, and the result reload. That is genuine work, not free
    instructions, which is the gate #94 step 3 failed.
  - ADDS: ~7 emitted bytes' worth of instructions per `ord(s[i])` SITE.
  - PREDICTED: ~30 loop instructions + a call -> ~10 instructions,
    **6.3 ns -> ~1 ns/char, i.e. 27x -> ~4x** on this benchmark.

Verify with WALL CLOCK. If it is not a wall-clock win it is not a win.

## The emitted sequence

`SharedStr` is `{ intrusive_ptr<StrObj> obj; size_type off; size_type
len; bool slice; }` - so obj at +0, off at +8, len at +12, slice at +16
of the slot payload.

    cmp  byte [base + payload + slice_off], 0
    jne  -> cold            ; a SLICE: decline (see below)
    mov  rcx, [base + payload]              ; the StrObj *
    mov  edx, [base + payload + len_off]    ; len, zero-extended
    cmp  r_idx, rdx
    jae  -> cold            ; ONE unsigned compare catches BOTH a
                            ; negative index and >= len; the helper
                            ; owns the negative-wrap semantics
    mov  rcx, [rcx + strobj_data_off]       ; std::string's data pointer
    movzx eax, byte [rcx + r_idx]
    <store_dst rax>         ; or hand it to lever A in RAX

**Why decline slices rather than handle them:** `SharedStr::offset()` is
`slice ? off : 0`, so a non-slice must IGNORE `off`. Declining removes a
load and a branch from the hot arm, and a slice base is rare. Same
decline-to-the-helper shape as #92/#95's inline tiers.

## ⛔ THE HAZARD THAT DECIDES THE DESIGN: std::string's data pointer

The char data is behind `std::string::_M_p`. In libstdc++ that is at
offset 0 of the string and is ALWAYS a valid data pointer, SSO or not -
so one load works for both short and long strings. **libc++ does not
lay its short form out that way**, and while the JIT is Linux/x86-64
only (`ML_JIT_SUPPORTED`), the project does build with clang and CI has
a libc++ lane, so a hardcoded offset is a silent-wrong-answer waiting
for a toolchain change.

**Do NOT hardcode it. SELF-VERIFY the probe at layout-init time:**
construct a SHORT string and a LONG one, compute the candidate offset
from a real object, load a pointer through it, and check both equal
`s.data()`. If either fails, leave the inline tier DISABLED and every
`ord(s[i])` keeps calling the helper. Ten lines, and it converts a
portability landmine into a tier that quietly does not engage.

This follows the project's existing precedent - `SharedObject`'s
`data_off` already bakes libstdc++'s `vector::_M_start` at +0, and
`CaptureSlots` static_asserts its data pointer's position - but those
are asserted, not verified against a live object, which is what the SSO
case needs.

## The probes to add

Extend `SharedStr::JitProbe` (sharedstr.h - the co-located-probe rule,
so the offsets come from a REAL object and cannot drift):

    struct JitProbe {
        const void *len;          /* already there */
        const void *slice;        /* NEW */
        const void *obj;          /* NEW - assert it is payload + 0 */
        long strobj_data;         /* NEW - &obj->s minus obj.get() */
    };

plus `jit_layout()` fields `str_slice_off`, `str_obj_off`,
`strobj_data_off`, and a `bool str_inline_ok` set by the self-check.

## Steps

1. ~~**The probes + the self-check**, with the tier still disabled.~~
   **DONE.** `SharedStr::JitProbe` gained `slice`/`obj`/`strobj_data`;
   `jit_layout()` gained `str_slice_off`/`str_obj_off`/
   `strobj_data_off`/`str_inline_ok`; the verdict is reported as
   `str_probe_ok`. NOTE the counter's assignment is `#ifdef TESTS` like
   every other JITSTATS bump, but **the TIER's gate must read
   `l.str_inline_ok`**, which is unconditional - a release build has to
   gate correctly too.
2. **The inline arm** in `emit_op`'s `OpCode::OrdCharV` case
   (jit.cpp ~10855), cold-declining to the existing `jit_ord_char`.
3. **`g_jit_ord_inline`**, bumped by the EMITTED code only - the helper's
   own counter cannot prove the fast path ran (the
   `g_jit_store_fast` precedent).
4. **Tests**: fires on a plain string; DECLINES on a slice base, on a
   negative index, and on an out-of-range index - each on its own
   counter, each asserting the VALUE too, and the OOB case asserting the
   CARET is unchanged (the helper throws loc-less and the fragment
   exc-stamps; the inline arm must reach the same stamp).
5. **Sabotage each guard** and watch it fail: the slice test (a slice
   base then reads the parent's bytes), the bounds test (a wild read -
   ASan), and the self-check (force it false; the tier must vanish and
   everything stay green).
6. **Measure wall clock**, interleaved, against the prediction above.

## ⛔ Two emitter traps already paid for once (2026-08-15)

  - **`Emitter::j32` takes the SHORT jcc opcode** and adds 0x10: `je` is
    `j32(0x74)`, not `0x84`. The near second byte assembles `0F 94`, a
    SETE with a bogus modrm.
  - **`Emitter::movabs` is LOW-EIGHT-REGISTERS ONLY** - a bare 0x48 REX
    with no REX.B, so `movabs(R10, x)` emits `ret imm16`. Use
    `movabs_r8` / `movabs_r10`. It now ML_CHECKs `reg < 8`.

## The sibling cases, so the map is complete

`LoadStrChar` (`s[i]` yielding a 1-char STRING, jit.cpp ~10180) is the
same navigation with a different result - it must ALLOCATE a one-char
string, so it is a much weaker candidate and is NOT part of this. The
other six call-dominated benches in the census (63_closures,
76_funcval_dispatch, 11_closure_counter, 75_indexed_unpack,
73_multi_unpack, 35_map_filter) are closure creation, indirect dispatch
and unpack helpers - different work, not this pattern.
