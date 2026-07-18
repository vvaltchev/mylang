# Native x86-64 AOT — incremental, on-the-fly, never-complete (design)

Status: N0+N1+N2 LANDED (2026-07-18). N2 = intra-run branches + the
NATIVE BACK EDGE: a run may now contain Jump/JumpUnlessIntCmp/
ForLoopStep/IntAddStep and interior branch targets, so a whole int loop
iterates in machine code (internal branches -> fragment-local jcc/jmp
patched from a per-run label[]; a target outside the run -> exit_pc).
No single-entry constraint is needed: interior ops survive as
interpreted originals, so an external branch/bail just resumes
interpreted. MEASURED (same-binary JIT off vs on, the cleanest control -
the kill switch exists for it): VM-wall geomean 0.895, my/py 4.97x ->
5.47x. 01_while 0.190x, 50_autoconst_dce 0.227x, 02_for 0.333x, 06_if
0.450x, 03_int 0.632x, 68_nested 0.692x. (The cross-binary A/B agreed
on the headline - 5.04x -> 5.47x - but its tiny-magnitude per-bench
deltas at 4-15ms are NOISE; the same-binary control shows no real
regression, incl. 07_nested_loops, which is FASTER same-binary.) NEXT:
N3 DONE (the SSE float tier): FloatBin(add/sub/mul) +
FloatAdd/Sub/MulRR/RI, LoadImmFloat, JumpUnlessFloatCmp (ordering,
NaN-safe via the ucomisd operand-swap trick). Float slot READS
type-dispatch (float->movsd fast, int->cvtsi2sd promote, else bail,
matching read_float_slot); WRITES are the two-store (t_float singleton
in r8 + movsd payload). div/mod stay interpreted (they THROW on 0 /
are a libm call). MEASURED (same-binary JIT off vs on): VM-wall geomean
0.812, my/py 5.00x -> 5.55x; 54_mandelbrot 0.344x, 55_float_sum 0.867x.
N4 DONE (flat array element READS): LoadElemInt/LoadElemFloat lower to
a fragment that navigates slot -> shobj -> kind + data, unsigned-bounds-
checks, and reads the raw scalar; a non-array / SLICE / wrong-kind /
OOB / negative-index base BAILS to the interpreter (byte-identical
throw/caret). The fragile SharedObject layout is baked via a co-located
`jit_probe` accessor (sharedarray.h) that reads real members, so it
can't silently drift. MEASURED (same-binary JIT off vs on): VM-wall
geomean 0.897, my/py 5.05x -> 5.7x; 18_foreach_array 0.650x,
19_foreach_indexed 0.565x (foreach-over-array lowers to a counted loop
with LoadElemInt), sieve/matrix reads 0.92-0.95x. NEXT (optional):
StoreElemInt/Float (unblocks the sieve/matrix WRITE side), MathFnV glue. Maintainer direction: build
it VERY incrementally, accept that it may never cover everything (the
interpreter is the permanent, tested fallback — "never complete" is a
feature, not a debt), never serialize machine code (AOT always runs
on-the-fly AFTER codegen or after a `.myv` load), POSIX-only for now
(x86-64; Windows/arm64 deferred).

## The shape: a baseline JIT over the register machine

The model is V8's Sparkplug / JSC's Baseline tier: a 1:1-ish translator
from bytecode to machine code with NO IR and NO global register
allocation, sharing the interpreter's state layout completely. What
makes it unusually clean HERE: **the VM's registers already ARE memory
slots** (the frame window). Native code and the interpreter share all
state with zero translation at the boundary — a native fragment that
gives up just returns "resume interpreting at pc N". The entire
deoptimization problem of real JITs reduces to `return pc`.

The safety story is the one that carried AST→bytecode: the `-tw`
differential oracle, the three-way fuzzer, the full-suite interleaved
A/B rule — plus a kill switch that turns every phase into a same-binary
A/B lever.

## Decision record

1. **Machine code lives in a SIDE BUFFER, not inside `Chunk::code`.**
   The in-stream idea (x86 bytes in 32-byte `Instr` slots) was
   considered and rejected: W^X is PAGE-granular and `Chunk::code` is
   malloc'd vector storage (you'd make interpreter data executable, or
   go RWX — forbidden on hardened macOS); and mixing instruction fetch
   with the interpreter's DATA reads of neighboring slots on the same
   cache lines trips x86's self-modifying-code machinery (pipeline
   flushes). A per-chunk mmap'd buffer + an `EnterNative` opcode gives
   the same incrementality and any-length fragments with none of that.
2. **The unit is the RUN (maximal compilable sequence), not the op.**
   A predicted call+ret costs ~2-4 cycles; one dispatch ~4-8 (more
   mispredicted). Single-op fragments are break-even AT BEST. One
   fragment entry must amortize over N ops (profit from N >= ~4), and
   the real prize is a run containing its own BACK EDGE: the loop
   iterates natively, zero dispatches per iteration.
3. **Native code NEVER throws and never calls anything that can.**
   Fragments are frameless leaf code with no unwind tables — a C++
   exception propagating through them is std::terminate. EVERY
   exceptional condition (a negative shift count, a trapping idiv
   combination) is a BAIL: return the op's pc, the interpreter
   re-executes that op and throws with the proper loc/caret. Error
   behavior stays byte-identical by construction.
4. **AOT is a pure post-load chunk transform.** It runs after
   `codegen_chunk` AND after a `.myv` load, identically — `.myv` stays
   portable and never contains machine code.
5. **Hand-rolled emitter, zero deps.** Instruction encodings are ISA
   facts (no copyright concern; original code). Copy-and-patch-style
   stencils would need LLVM at build time — excluded by the no-deps
   rule; our op set is small enough that a direct emitter is ~500-800
   lines.

## Runtime pieces

- **`Chunk::native`**: an owning handle (mmap length + base; munmap
  deleter; never copied, never serialized) for the chunk's fragment
  buffer. Emitted into `PROT_READ|WRITE`, then flipped to
  `PROT_READ|EXEC` (strict W^X; plain `mprotect` on Linux and macOS
  x86-64 — `MAP_JIT`/`pthread_jit_write_protect_np` is an arm64-era
  concern, deferred with arm64 itself).
- **`OpCode::EnterNative`**: `a` = the byte offset of the fragment
  entry. Handler:
  `pc = ((NativeFrag)(base + off))(&ctx.frame->at(0));` then dispatch.
  (One new opcode total — mind the documented front-end sensitivity:
  the JIT-OFF configuration must be A/B-verified neutral too.)
- **The fragment ABI**: `size_t frag(LValue *slots)` — System V; slots
  base in `rdi`, resume pc returned in `rax`. Fragments use ONLY
  caller-saved registers (rax rcx rdx rsi r8-r11, xmm0-5): no
  prologue/epilogue beyond `ret`, nothing to preserve. v1 fragments
  touch NOTHING but the slot window — no ctx, no chunk (everything
  else is baked as immediates at emit time).

## Slot access contracts (the correctness core)

- **Reads** (`th`-proven int/float operands): the release interpreter
  already does a RAW union load (`getval<int_type>`; the type-tag
  check is VM_HARDENING-only). Native does the same:
  `mov rax, [rdi + slot*sizeof(LValue) + offsetof(val.ival)]`. Bool
  flows as int exactly as the interpreter's readers do (a bool-typed
  slot read via the int path needs the same is<bool> promotion the
  interpreter has — v1 can BAIL on a non-int tag instead, one compare,
  keeping reads exact; measure which).
- **Int/float slot WRITES are two unconditional stores** — store the
  type-singleton pointer (an imm64 kept in a register per fragment),
  store the payload. Sound because slots are NEVER reused across
  variables (`next_slot` is monotonic — the resolver invariant): a
  `th==i` destination only ever holds none/int/bool over its lifetime,
  all trivial (no release needed), and storing value+type is exactly
  what `LValue::put`'s inline fast path does for a trivial old value.
  This is BRANCHLESS — simpler than the interpreter's own write path.
  (v1 scope: FRAME slots only. Captures/globals keep the interpreter.)
- **Layout dependencies**: `sizeof(LValue)`, the union payload offset,
  and the `AllTypes` singleton addresses are baked as immediates —
  static_asserts at the emitter pin them.

## The v1 op set (the audited never-throw scalar tier)

Exactly the set two prior audits produced (B1/B2 narrow gates,
`op_writes_scalar`):

- `IntAddRR/RI`, Sub, Mul, And, Or, Xor (2-4 instrs each, memory
  operands through rax/rcx)
- `IntShlRR/RI`, `IntShrRR/RI`: inline count checks; an out-of-range /
  negative count BAILS (the interpreter's `bit_shl/bit_shr` throw
  `InvalidValueEx` with the right caret; saturation semantics for
  count >= 64 emitted inline for the non-throw cases)
- `IntModRI`: `idiv` — the imm is nonzero by selection; imm == -1 is
  EXCLUDED at selection (INT64_MIN % -1 traps #DE on x86; the
  interpreter's -fwrapv path defines it)
- `FloatAdd/Sub/MulRR/RI` (`movsd/addsd/subsd/mulsd`), int operand
  promotion via `cvtsi2sd`
- `JumpUnlessIntCmp` / `JumpUnlessFloatCmp` → `cmp`/`ucomisd` + `jcc`
  to a fragment-local label (intra-run) or an exit (`mov eax, pc; ret`)
- `ForLoopStep`, `IntAddStep`, `IntAddModRI` (imm != -1),
  `LoadImmInt/Float`, `MoveV` between two PROVEN-scalar slots only,
  `Jump` (intra-run: `jmp`; out: exit)
- Explicitly NOT in v1: anything boxed, calls, containers
  (`LoadElemInt` is the designated v2 candidate: type+skind+bounds
  checks with a bail), div/mod by register (splits the run), captures,
  globals, `MathFnV` (callable-from-native is v2 glue — `vm_math_fn`
  never throws, but calling C++ needs ABI care).

This covers the inner loops of 01, 02, 03, 05, 06, 07, 43, 45, 53, 56,
57, 59, 60, 61, mandelbrot's kernel, and fib's unrolled body — the
dispatch-bound tier.

## Run discovery + patching (`aot_chunk`, post-codegen/post-load)

1. Compute branch targets over the whole chunk (`visit_pc_fields` —
   the existing audited enumeration).
2. Find maximal runs of v1-compilable ops. v1 constraint: NO external
   branch targets an interior pc (single entry at the head); intra-run
   branches become fragment-local labels — a run containing its own
   back edge iterates natively. Runs shorter than `MIN_RUN` (~4,
   tunable by measurement) are skipped.
3. For each run: emit the fragment (two passes for label fixups —
   emit with rel32 placeholders, patch); every exit and every bail
   site is `mov eax, <resume pc>; ret`.
4. **INSERT** an `EnterNative` at the run head (do NOT overwrite the
   first op: any op can bail on its very first execution — e.g. the
   shift-count case — and the bail contract requires every interior
   pc's original `Instr` intact for resumption). Inserting shifts pcs:
   remap all pc fields + the pc-keyed side tables (locs, inline_ctxs)
   with the compaction pass's existing prefix-sum machinery, run once
   per chunk after all insertions.
5. Flip the buffer RX. Store the handle on the chunk.

Bail semantics recap: a bail returns the pc of the op that could not
complete natively, WITHOUT side effects for that op (checks precede
stores in the emitted sequence). The interpreter re-executes it fully
(and throws if that's the outcome). Re-entry happens naturally the
next time control reaches the run head.

## Gating, config, CI

- `#if defined(__x86_64__) && !defined(_WIN32)` — everything else
  simply never creates fragments; zero behavior change.
- **Kill switch**: `-nj` + `MYLANG_JIT=0` env. This is also the
  measurement lever: the per-phase A/B is THE SAME BINARY, JIT on vs
  off (cleaner than cross-binary), on top of the usual cross-binary
  full-suite rule vs the pre-JIT baseline.
- VM_HARDENING: fragments bypass `Frame::at`/tag checks by
  construction. The hardened lanes still exercise them (the checks
  guard the interpreter's paths); CI adds a `MYLANG_JIT=0` variant of
  one lane so both configurations stay green. RECYCLE/ASan lanes are
  unaffected (no AST interaction); fragments run fine under valgrind.
- `-vd` gains a per-chunk "native runs" section: each run's pc span,
  fragment offset/size, and a hex dump of the bytes (external
  `objdump -D -b binary -m i386:x86-64` disassembles it; an in-tree
  x86 disassembler is NOT in scope).

## Phases (each lands fully validated; stop when returns diminish)

- **N0 — plumbing + emitter core.** The mmap/mprotect handle, the
  byte emitter (the ~15 needed forms: mov r/m64, arithmetic r/r r/imm,
  cmp, jcc/jmp rel32, movsd family, cvtsi2sd, idiv, ret), the
  `EnterNative` op + handler, the kill switch, and ONE hand-written
  smoke fragment behind a hidden test hook. Unit tests for the emitter
  (encode → expected bytes).
- **N1 — straight-line int runs.** No branches inside runs yet
  (`MIN_RUN` gates profit). Differential + fuzzer green with JIT on;
  full-suite A/B (JIT on/off + vs the pre-JIT binary) — expected
  small-positive; the POINT of N1 is proving the contract, not speed.
- **N2 — intra-run branches + the native back edge.** Loops iterate
  natively. This is where the numbers move: expect 2-4x VM-wall on the
  dispatch-bound benches, suite geomean +10-25% (v1, slots in memory).
- **N3 — floats** (the SSE tier + promotion): mandelbrot/54/55-class.
- **N4 — polish tier.** The bool-read promotion decision, shift bail
  coverage tests, `LoadElemInt` (+bounds bail), `MathFnV` via a
  C-call-safe glue, `MIN_RUN` tuning by measurement.
- **N5 — fragment-local register caching (LANDED).** Up to 2 hot
  int-scalar slots are pinned in `r10`/`r11` per fragment: loaded ONCE at
  entry (a back edge to the first op keeps them live across the loop),
  read/written straight from the register, and flushed (type singleton +
  payload, two stores) at EVERY exit/bail. Int loops approach native C
  (`01_while` 0.098x same-binary at the loop-bound extreme, scale 200
  best-of-3; the win is on the accumulator/counter locals). `pick_cached_slots` selects a slot iff
  every use in the run is an int-scalar op AND it is a **resolved LOCAL**
  (`< slot_count`).
  **SOUNDNESS — cache ONLY resolved locals, never TEMPS.** A temp
  (`>= slot_count`) is scratch the VM REUSES for different roles across run
  boundaries: an int scratch inside one JIT run, a `foreach` general-array
  SNAPSHOT / dict-iterator base / slice temp between runs. The eager
  entry-load + exit-flush assumes the register OWNS the slot for the whole
  fragment; that is false for a temp still LIVE as an array across the
  boundary — the flush would overwrite the live array with the int register
  + the `t_int` tag, corrupting the snapshot so a later `LoadElemValue`
  throws `InternalErrorEx`. A resolved local has a stable identity and
  (being counted only via proven-int ops) a stable int type, so it is
  safely owned. This corruption was found by `tests/nested_fuzz.py` (not
  `-rt`; a 30-line nested-loop-plus-2D-foreach program aliased a foreach
  snapshot onto a cached temp) and is pinned by two `jit:` regression
  tests. Two related classifier/emitter bugs were fixed alongside: the
  `IntAddStep` classifier read the wrong operand fields (counting a literal
  rhs VALUE as a slot index — a phantom that could cache/corrupt whatever
  slot it collided with), and the shift-by-register handler read its count
  operand raw (bypassing the cache for a pinned shift-count local). Every
  emitter slot access now goes through the cache-aware `read_slot`/
  `write_slot`/`load_operand`; the raw `slot_addr` reads that remain are on
  `bad()`-disqualified (`LoadElem` base/index) slots only.

- **N6a — native math builtins + THE REF-STORE FIX (LANDED, and the
  turning point).** `MathFnV` (`sqrt`/`sin`/`cos`/`log`/`exp`/`pow`/...)
  became JIT-eligible: `sqrt`/float-cast → SSE (`sqrtsd`/`cvtsi2sd`), the
  transcendentals → a libm call emitted as a bare 5-byte `E8 rel32` (no NOP
  padding), patched post-mmap to libm directly (the anon mmap lands next to
  libm — measured) or to an in-buffer trampoline (`movabs; jmp` — the
  arm64-veneer shape). **But the real discovery was that the JIT was
  BAILING across the suite and running interpreted.** A ref-listed scalar
  store (a reused temp that later holds a string) tested `type == t_int/
  t_float; jne BAIL` — which bailed on a TRIVIAL current value (`none` on
  iteration 1) too, so the fragment bailed at the first store and the whole
  loop ran interpreted (native UNUSED). Every "native builtins are neutral"
  number was interpreter-vs-interpreter. **Fix = approach A**: test
  `type->t >= t_str` (a REAL reference — offset/value probed into
  `JitLayout`); a reference calls a noexcept helper (`jit_put_int`/
  `jit_put_float`: release + store, stay native, cold/once-per-temp), a
  trivial value takes the fast two-store — NEVER bails. Same-binary JIT off
  vs on once fixed: `08_func_call` **0.49x**, `07_nested_loops` **0.58x**,
  `40_math` **0.72x** (my/py 5.6x→**7.7x**), `49`/`51` ~0.7x, broad −3–7%,
  no regressions. This validated **approach A** as the JIT's new contract
  (see below).

## Approach A — the JIT's contract (compile-time fallback, no runtime bail)

The N0–N5 "keep the interpreted originals + bail-to-re-interpret" model is
the AST-fallback anti-pattern: a double copy of every op, and one bail drops
a whole loop to the interpreter (the bench-40/int-store bug). **The correct
model, being rolled out:** the fallback is a COMPILE-TIME decision — a run
we can PROVE is fully handleable in native (fast paths inline + noexcept C++
HELPER CALLS for the hard parts: ref-release, array/dict ops, exception
raise) is compiled and the interpreted originals are DELETED; an op we can't
prove → the VM instruction stays (the only fallback). NO runtime bail.
- **ref-release**: `jit_put_int`/`jit_put_float` (DONE).
- **exceptions (OOB / negative shift)**: the fragment stores a `JitRaiseKind`
  to `g_vm_jit_raise` and exits to the op's pc; `EnterNative` raises the
  matching exception via `vm_raise` — exact caret from the loc table, routes
  through the same frame-walk (catchable), no re-interpret. Only GENUINE
  exceptions convert (the LoadElem non-array/slice/wrong-kind bails are valid
  non-exception cases → stay re-interpret). (DONE, c5ad01b.) NOTE: div0 is
  NOT a current JIT site (IntModRI excludes imm 0/-1 at compile time,
  div-by-reg isn't compiled), so nothing to convert there yet.
- **arrays/dicts/strings**: don't nativize the data structures — CALL the
  same C++ the interpreter calls (a helper per op). (NEXT.)
- **delete the interpreted originals** for a fully-native run (single-entry;
  a rare interior external target splits the run). (IN PROGRESS.)

## Native MyLang calls (`call <offset>`) — the design

The GOAL (the maintainer's definition, not the "Model A" mislabel): a caller
fragment does a real `call <callee fragment offset>`, the callee runs
entirely in machine code and `ret`s — NO dispatch-loop round-trip per call.
This removes the fragment exit + `EnterNative` re-entry and the CallV/ReturnV
dispatch. It does NOT remove the frame-setup MEMORY work (window push +
record + arg bind) — that is inherent (the profile's ~58%); native calls buy
the ~33% dispatch slice plus staying in native across the call.

**Prerequisite: a FULLY-NATIVE callee body** (a single `call`-able entry, no
interpreted ops in the middle) — i.e. approach A / delete-originals applied
to the callee's chunk. This is WHY delete-originals comes first: a native
`call` needs a native blob to jump to.

**Frame setup** (per call): the callee's slots live on the activation's
SEGMENTED slot stack (address-stable — unchanged). The window push + record
fill + arg bind is either emitted natively (a segment-room check + pointer
bump + a store loop; the rare segment-OVERFLOW bails to a C++ helper — it
allocates) or done by a lean noexcept C++ helper `jit_enter_call` (the same
contract-legal helper-call the libm/ref-release paths use). Start with the
helper (simplest, correct); nativize the common fast path later if the
profile says the call setup is hot. `rdi` (the slots base) is repointed to
the new window for the `call`, restored on return.

**Exceptions across a native `call`-chain — the CHECKED-RETURN protocol.**
A hand-emitted `ret`-chain has NO C++ unwind tables, so a C++ `throw` cannot
traverse it — the exception MUST be signalled and unwound cooperatively:
- The callee, on a raise (`jit_raise` or a helper that throws), sets the
  pending signal and RETURNS to its native caller with an exception STATUS
  in a register (`rax`: 0 = normal, non-0 = unwinding). The function RESULT
  is written to the caller's dst SLOT (memory), so `rax` is free to carry
  the status.
- The caller fragment, after EVERY `call`, does `test rax, rax; jnz
  <unwind>` — a load-free, perfectly-predicted, never-taken branch (~1
  cycle; NOT a memory read of a global). On non-0 it does NOT continue: it
  pops its own call-record (`pop_window` frees THIS frame's `ref_slots`
  handles — the objects are on the activation, never the C++ stack) and
  RETURNS `rax` to its caller. Frame by frame, the chain unwinds in machine
  code; `pop_window` does all the ref-count cleanup exactly as the
  interpreted walk does today. A frame WITH a handler catches instead of
  propagating.
- **Overhead (maintainer's concern):** ~1 cycle per call (the `test/jnz` on
  `rax`), vs the ~10–20-cycle frame setup — small. TRY THIS FIRST; do not
  pre-optimize. If it ever shows up: a `longjmp`-to-the-nearest-boundary
  unwind (setjmp at each do_func_call/handler boundary; skips the per-call
  check, but then each intervening frame's record must still be popped by
  the boundary to free slots, and it can't observe an intermediate
  same-frame handler — so it only helps the no-intermediate-handler case),
  or a per-fragment landing-pad table. Measure before adding that
  complexity.

**Depth cap:** real `call`s grow the C++ stack per frame (unlike today's
O(1)-per-activation state-change model), so the `MYLANG_VM_STACK` cap must be
enforced at call setup (a catchable `StackOverflowEx`, as now).

**What "Model A" was (rejected as a native-call story):** the current model —
a call splits the run, C++ `vm_enter_call`/`leave` handles it, fragments are
native only WITHIN a frame. It buys nothing on call overhead; it is the
status quo, not a native call. The real native call is the `call <offset>`
design above, gated on fully-native callee bodies.

## The path to 10x — why it is NOT the N-series (N6/N7 sketch)

MEASURED reality (same-binary JIT off→on): the JIT already delivers
**3–5x on the code it compiles** (01_while 0.235x, autoconst_dce
0.211x = the per-op ceiling, already hit), but the suite geomean is
only ~1.2x. This is **Amdahl, not underperformance**: the non-scalar
benches (dict/string/call/alloc) spend their time in NATIVE C++ the JIT
calls into unchanged — `unordered_map::insert`, `std::string` growth,
`EvalValue` refcount churn, `vm_enter_call`. Dispatch was never their
bottleneck, so removing it is invisible. N1–N5 attack *dispatch*; even
at infinite speedup on the ~35% dispatch-bound fraction the suite caps
near **1.5–1.7x** (≈7–8x CPython). The remaining multiple to 10x lives
in the other 60%, and needs two fundamentally larger arcs:

- **N6 — call inlining / a lean native call.** Today any user call
  splits a run (`vm_enter_call` is never compiled), so fib / recursion
  / closures / higher-order callbacks get ~nothing from the JIT.
  N6a (tractable): a NATIVE fast path for a call to a small, resolved,
  scalar-only user function — a lean window bump + arg copy + `call`
  into the callee's fragment, skipping the ~270-instr `vm_enter_call`
  record machinery on the common no-handler/no-iterator path. N6b
  (harder): true INLINING of a small callee's fragment into the
  caller's (the native analogue of the tree-walker inliner) so the
  call vanishes — bounded depth for recursion, the per-frame PureCache
  still dedups the frontier. Targets 09_fib, 10_recursion, 11/12/63
  closures, 34/35/67 callbacks. Expected: meaningful on the ~15–20% of
  the suite that is call-bound.
- **N7 — allocation elimination / unboxing (the CPython-killer).**
  CPython is slow *because of boxing*; we already win 5x on lighter
  values, but our arrays/dicts/strings/temporaries still heap-allocate
  and refcount. N7 = escape analysis: a temporary array/dict/string
  (or a boxed struct) that does NOT escape its creating scope is
  stack-allocated or elided, and its refcount traffic removed
  (LuaJIT's allocation sinking, PyPy's virtuals — their decisive
  non-numeric win). This is the lever for the dict/string/wordcount
  tier. HONESTLY: it likely needs a different IR than the per-op
  bytecode-fragment model — an SSA-ish trace over a hot region where
  an allocation's lifetime can be proven — i.e. a TRACING tier, not an
  incremental native op. Research-grade scope; the biggest single
  source of the remaining multiple, and the biggest project.

So: **N5 pushes the arithmetic tier toward native-C (≈7–8x suite); 10x
is a real but SEPARATE goal built on N6 (calls) + N7 (allocation), not
on making the scalar JIT more perfect.** Sequence after N5: N6a (lean
call) is the next tractable step; N7 is the big arc, evaluated on its
own merits when the call tier is done.

## Deferred (explicitly out of scope until the above earns it)

- arm64 (macOS Apple Silicon: MAP_JIT + jit-write-protect dance),
  Windows (VirtualAlloc/FlushInstructionCache).
- Native calls (`CallV`/`vm_enter_call`), boxed ops, exceptions,
  containers beyond LoadElemInt, captures/globals.
- Profile-guided run selection (compile-on-Nth-entry); v1 compiles
  eligible runs unconditionally at AOT time — measure whether compile
  time ever matters (it won't for scripts this size).
- Serializing fragments (never: `.myv` stays portable).

## Risks, named

- **Unwinding**: rule 3 is absolute — audit every emitted sequence and
  every future "just call this helper" temptation against it.
- **#DE traps**: idiv INT64_MIN/-1 and the imm==-1 exclusion; a
  division-family op added later must re-check this.
- **Layout drift**: LValue/union/AllTypes immediates — static_asserts
  at the emitter + the differential catches semantic drift.
- **Front-end/layout sensitivity** (the documented WSL2 lesson): the
  JIT-OFF path must measure neutral vs the pre-JIT binary (one new
  opcode + handler); measure, don't assume.
- **The stale-binary trap**: every measurement per the hard rule —
  same session, both binaries rebuilt, interleaved.
