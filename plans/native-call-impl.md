# #55 native calls — the EXECUTION guide (self-contained, survives a compact)

This is the concrete, do-this-next implementation guide for the native-call
machine-code, written so it can be executed from a FRESH context with no
conversation memory. Read `plans/native-aot.md` "### #55 — the STAGED
implementation plan" for the design + the F1/F2/F3 findings; this file is the
step-by-step. HARD constraint (maintainer): at compile time we know EXACTLY
what to emit — NO runtime-decided silent fallback. Prioritize the SMALLEST
progress step, not the highest-value one (CLAUDE.md).

## Current state (all committed; HEAD = e3e111c "extract vm_frame_leave")

FOUNDATIONS DONE (all -rt 1395x2 green, behavior-identical):
- `vm_frame_setup` (vm.cpp): the ENTER core. Signature
  `static ML_NOINLINE Frame *vm_frame_setup(VmActivation &act, EvalContext
  &ctx, const Chunk *ret_chunk, size_t ret_pc, FuncObject &fo, const Chunk
  *cck, int_type argbase, size_t nargs, int_type dst,
  std::unique_ptr<PureCacheKey> ckey)`. Does arity check, `push_window`,
  record fill (`ret_chunk`, `ret_pc+1`, `dst`, `desc`, `caller_captures`,
  `cache_key`), arg bind, `ctx.captures = &fo.capture_slots`. Returns the
  callee window `Frame*`. Does NOT switch chunk/pc. `vm_enter_call` = it +
  `chunk = cck; pc = 0;`.
- `vm_frame_leave` (vm.cpp): the LEAVE core.
  `static ML_NOINLINE void vm_frame_leave(VmActivation &act, EvalContext &ctx,
  EvalValue res)`. Sets `g_vm_resume_chunk`/`g_vm_resume_pc` from the dead
  record's `ret_chunk`/`ret_pc`, sets `ctx.captures`, `pop_window()`,
  maintains the caller pure cache (scalar result only), writes the parent
  `dst` (`if (dst >= 0)`). `vm_leave_call` = it + `chunk = g_vm_resume_chunk;
  pc = g_vm_resume_pc;`. NEW globals (vm.cpp, near `g_vm_jit_raise`):
  `static const Chunk *g_vm_resume_chunk = nullptr; static size_t
  g_vm_resume_pc = 0;`.
- Write-once tracking: `Block::global_slot_reassigned` (`std::vector<char>`,
  by global-table slot; 1 == REASSIGNED == NOT write-once). Filled by the
  resolver's `count_write` (records any write to a `SymKind::global` slot into
  `Resolver::reassigned_globals`, published at the `global_func_names`
  publish site in resolver.cpp). A callee slot with
  `root->global_slot_reassigned[slot] == 0` is WRITE-ONCE (fixed identity).

## The fragment ABI + emitter primitives (jit.cpp — VERIFIED)

- A fragment is `size_t frag(void *slots)` called by `jit_enter(frag, slots)`.
  Returns a RESUME PC. `EnterNative` (vm.cpp ~2664): `pc = jit_enter(chunk->
  native.base + in->a_lit(), ctx.frame->slots);` then checks `g_vm_jit_raise`
  / `g_vm_jit_exc` and re-raises via `vm_raise` (loc from the side table).
- Registers: `RDI`=slots base (never clobbered by ops), `RSI`=t_int singleton,
  `RAX`=accumulator / returned resume pc, `RCX`=2nd operand, `RDX`=idiv rem,
  `R8`=t_float, `R9`=t_arr, `R10`/`R11`=N5 cache (up to 2 hot INT LOCAL slots;
  `Emitter::cache` = list of `CacheEnt{slot, payload, type, reg}`).
- Frameless: no prologue/epilogue; all caller-saved. `sizeof(LValue)==48`
  (static_assert). `slot_addr(slot)` → `SlotAddr{payload, type}` byte offsets
  from RDI (payload at slot*48+off_payload, type at +off_type; see
  `jit_layout()`).
- `e.flush_cache()`: writes every cached slot back to memory (rsi=t_int to
  .type, reg to .payload). `e.exit_pc(pc)`: `flush_cache; mov eax,pc; ret`.
- `e.movabs(reg, imm64)`, `e.mov_rr(dst,src)`, `e.lea_rdi(disp)`, `e.store(reg,
  disp)` (`mov [rdi+disp], reg`), `e.load(reg, disp)`, `e.u8/u32/u64`,
  `e.j8(op)`/`e.patch8`, `e.push_reg`/`e.pop_reg`, `e.call_relocs` (a list of
  `{site, fn}` patched to `call rel32` after mmap; used as
  `e.call_relocs.push_back({e.pos(), fn}); e.u8(0xE8); e.u32(0);`).
- HELPER CALL pattern (a call clobbers caller-saved incl. r10/r11):
  `emit_call_prologue(e)` (push rdi + each cache reg + a pad for 16-align),
  set args (System V: rdi,rsi,rdx,rcx,r8,r9), the `call rel32` via
  call_relocs, `emit_call_epilogue(e)` (pop, re-materialise rsi=t_int,
  r8=t_float). See `emit_put_int_call` (jit.cpp ~731) for the exact shape.
- Compile driver: `jit_compile_chunk` (jit.cpp ~1522) finds maximal runs of
  `jit_op_eligible` ops (MIN_RUN=4), marks a run `deletable` iff every op is
  `op_fully_native` AND single-entry, inserts `EnterNative` at each run head,
  remaps pcs. `emit_op` (~1132) emits per-op; `emit_branch` (~1358) the
  branch ops; `op_is_branch` (~1446); `branch_pc_target` (~1502);
  `op_fully_native` (~1482); `jit_op_eligible` (~561).

## STEP 1 — native `ReturnV` (the next step; the smaller machine-code slice)

GOAL: a fully-native LEAF body's `ReturnV` runs IN the fragment (no interpreted
ReturnV). Tested by a leaf function called NORMALLY through the interpreter
(vm_enter_call → the callee chunk's EnterNative → the fragment → native
ReturnV). NO native CallV yet. This ALSO makes the callee a `call`-able blob
for STEP 2.

Trace (interpreter-entry): interp CallV → `vm_enter_call` pushes callee
window/record + switches to the callee chunk → loop hits the callee chunk's
`EnterNative` (pc 0) → `jit_enter(fragment)` → fragment runs body → native
`ReturnV`: reads the result slot, calls `jit_ret` (which reads the slot +
`vm_frame_leave` → pop callee window, write parent dst, set resume globals),
returns a SENTINEL in rax → `jit_enter` returns sentinel → `EnterNative` sees
the sentinel → `chunk=g_vm_resume_chunk; pc=g_vm_resume_pc; code=chunk->code.
data();` continue in the parent. This REPLACES the interpreted vm_leave_call
for a fully-native leaf (its ReturnV is now in the fragment).

Substeps:
1. **Globals for the fragment to reach act/ctx** (vm.cpp): `g_current_act`
   (`VmActivation*`) and `g_current_ctx` (`EvalContext*`), set at `vm_run`
   entry and SAVED+RESTORED around it (a builtin callback re-enters `vm_run`
   → nested activation; must restore the outer on return). Find `vm_run` (the
   activation entry that constructs `VmActivation` + calls `vm_run_chunk`) and
   add `auto sa=g_current_act, sc=g_current_ctx; g_current_act=&act;
   g_current_ctx=&ctx; ... ; g_current_act=sa; g_current_ctx=sc;` (RAII or
   try/finally-safe — vm_run already has a catch; put the restore on all exit
   paths).
2. **`jit_ret` helper** (vm.cpp, `extern "C"`, declared in jit.h): 
   `extern "C" size_t jit_ret(int_type res_slot) noexcept { EvalValue res =
   g_current_ctx->frame->at(res_slot).get(); vm_frame_leave(*g_current_act,
   *g_current_ctx, std::move(res)); return JIT_RET_SENTINEL; }`. It reads the
   result from the CALLEE window (still current — pop happens inside
   vm_frame_leave), pops, writes parent dst, sets resume globals, returns the
   sentinel. Define `JIT_RET_SENTINEL` = `SIZE_MAX` (a pc no chunk has;
   ML_CHECK it's never a real remap target). noexcept: a fully-native leaf
   body is throw-free, so vm_frame_leave can't throw here (the cache emplace
   is scalar-only; put is a plain slot write). Declared in jit.h so jit.cpp
   can bake `&jit_ret`.
3. **`op_fully_native(ReturnV) → true`** (jit.cpp ~1482) AND
   **`jit_op_eligible(ReturnV) → true`** (~561). NOTE ReturnV's operand: the
   result value slot is `in->a_slot()` (see the interp ReturnV handler,
   vm.cpp ~4064). ReturnV is a run TERMINATOR (rets; no fall-through), so it
   is NOT in `op_is_branch`/`branch_pc_target`.
4. **`emit_op` case for `ReturnV`** (jit.cpp ~1132): 
   ```
   // result slot value must be in MEMORY for jit_ret to read it:
   e.flush_cache();                 // write r10/r11 back (uses rdi=slots base)
   // arg1 = res_slot (System V rdi). rdi is the slots base; we ret after, so
   // clobbering it is fine (jit_ret uses g_current_ctx->frame, not rdi).
   e.movabs(RDI, (uint64_t)(int64_t)in.a_slot());
   e.call_relocs.push_back({e.pos(), (const void*)jit_ret});
   e.u8(0xE8); e.u32(0);            // call jit_ret (returns sentinel in rax)
   e.u8(0xC3);                      // ret (rax = JIT_RET_SENTINEL)
   ```
   NO emit_call_prologue/epilogue (we ret right after — nothing to preserve;
   the cache is dead post-pop; flush_cache already wrote the result to memory).
   IMPORTANT: emit `flush_cache` BEFORE overwriting rdi (flush uses rdi).
5. **`EnterNative` sentinel handling** (vm.cpp ~2664, right after `pc =
   jit_enter(...)`): `if (pc == JIT_RET_SENTINEL) { chunk = g_vm_resume_chunk;
   pc = g_vm_resume_pc; code = chunk->code.data(); VM_NEXT; }` — BEFORE the
   g_vm_jit_raise/exc checks (a native ret is not a raise). Must interleave
   with the CGOTO dispatch correctly (a temporary, no live destructor at the
   goto). Verify the boundary case: for a BOUNDARY callee (do_func_call entry,
   `cur_rec().boundary`), the native ReturnV must NOT pop-to-parent; it must
   set flow + return from vm_run_chunk. TWO options: (a) keep the boundary
   callee's chunk INTERPRETED (don't native-call it, and its ReturnV stays
   interpreted) — but the fragment IS the body... so instead (b) `jit_ret`
   branches: `if (g_current_act->back_rec().boundary) { ctx->flow->value=res;
   ctx->flow->type=ret; return JIT_RET_BOUNDARY_SENTINEL; }` and EnterNative
   on that sentinel does `return;` (leave vm_run_chunk, the do_func_call
   contract). Add a test: a fully-native function entered as a BOUNDARY
   (e.g. via a builtin callback / the top-level call) returns correctly.
6. **`Chunk::native_leaf` (bool) + `native_entry_off` (size_t)** (bytecode.h):
   set in `jit_compile_chunk` after the run analysis: `native_leaf = true` iff
   there is exactly ONE run, it is `deletable`, it covers `[0, code.size())`
   of the ORIGINAL body (whole body), AND the body's last op is `ReturnV`
   (or every exit is a ReturnV). `native_entry_off = frag_off[0]` (the run's
   fragment offset in `chunk->native`). Show both in `-vd`
   (disasm.cpp) for testability. (STEP 1 doesn't CONSUME native_leaf, but
   compute it here so STEP 2 has it + it's -vd-testable now.)
7. **ASSERTs**: `ML_CHECK(res_slot valid)` in jit_ret; `ML_VM_CHECK` that
   JIT_RET_SENTINEL is never a real chunk pc; in EnterNative, `ML_CHECK` the
   resume chunk/pc are valid when the sentinel is seen.

TEST STEP 1: build `make -j TESTS=1 OPT=0 BUILD_DIR=build-dbg`; `./build-dbg/
mylang -rt` (1395x2). Add a `jit:` test: a leaf `func add(a,b)=>a+b` called in
a loop returns correct sums (its ReturnV now native). `-vdj` shows the
fragment ending in `call jit_ret; ret`. `python3 tests/nested_fuzz.py`
(tw==vm==cpython). Same-binary JIT off/on A/B is neutral-to-tiny here (the
call is still interpreter-driven; the win is STEP 2). If green, COMMIT.

## STEP 2 — native `CallV` (caller side; the bigger win)

GATE (all COMPILE-TIME; emit interpreted CallV if any fails — NOT a runtime
bail): (1) `dc->vm_direct_func && dc->direct_func_slot >= 0`; (2) op is
`CallV` (not `CachedCallV` in v1); (3) `root->global_slot_reassigned[slot]==0`
(write-once); (4) the callee resolves at compile time to a `FuncDescriptor`
whose `vm_chunk->native_leaf` is true (LEAF: its body makes NO calls — so it
is C-stack-bounded, F2; check the callee chunk has no CallV/CallValueV/etc.);
(5) arity fixed+correct (the inferencer already proved it). Need a
slot→FuncDescriptor map at codegen (build from `collect_funcs` / the global
slot assignments; the resolver knows `id->sym.slot` per FuncDeclStmt).

The caller fragment reaches the callee via an INDIRECT call: bake the callee
`FuncDescriptor*` (program-lifetime, stable); at runtime load `desc->vm_chunk`
→ `->native.base + native_entry_off`, `call rax`.

`ret_chunk` for the record: the caller fragment BAKES its OWN function's
`FuncDescriptor*` (known at `codegen_func_body` time) so the setup helper reads
`caller_desc->vm_chunk`; `ret_pc` = the CallV pc (a baked immediate).

Helper `jit_call_setup` (vm.cpp, extern "C", noexcept):
`LValue *jit_call_setup(FuncDescriptor *callee_desc, int_type argbase,
size_t nargs, int_type dst, FuncDescriptor *caller_desc, size_t callv_pc)` —
resolve the FuncObject from the callee's global slot (`g_current_ctx->gfuncs`),
`ML_CHECK` its `->func == callee_desc` (write-once invariant — loud, never a
silent fallback), call `vm_frame_setup(*g_current_act, *g_current_ctx,
caller_desc->vm_chunk, callv_pc, *fo, callee_desc->vm_chunk, argbase, nargs,
dst, nullptr)`, return the window's `slots` ptr; on `StackOverflowEx` (from
push_window) catch → `g_vm_jit_exc` → return nullptr.

Emission (jit.cpp `emit_op` CallV case, only when the gate passes):
```
e.flush_cache();                     // args + live locals to memory
emit_call_prologue-like: push rdi (caller window) + cache regs, align
set System V args: rdi=callee_desc, rsi=argbase, rdx=nargs, rcx=dst,
                   r8=caller_desc, r9=callv_pc  (all baked immediates/movabs)
call jit_call_setup                   // -> rax = callee window (or null)
test rax,rax; jz <exit to g_vm_jit_exc path: exit_pc(callv_pc)>  // StackOverflow
mov rdi, rax                          // rdi = callee window for the callee frag
// indirect call the callee fragment:
movabs rax, callee_desc; mov rax,[rax + off(vm_chunk)]; mov rcx,[rax +
  off(native.base)]; add rcx, native_entry_off; call rcx
// callee's native ReturnV already wrote our dst + popped its window + set
// resume globals; we ignore the sentinel in rax (leaf = throw-free post-setup)
restore rdi (caller window) + cache regs (pop), re-materialise rsi/r8
reload the N5 cache (load each cached slot from memory)
// continue with the next op
```
The dst is written by the callee's `jit_ret`. NO checked-return in v1 (leaf =
throw-free; the only exception is StackOverflow at setup, handled by the
null-return → g_vm_jit_exc exit BEFORE the call). `CallV` is NOT
`op_fully_native` (it can exit on StackOverflow), so its run is non-deletable
(the original CallV survives, dead — like the container stores). Keep it out of
`op_is_branch`.

COVERAGE (maintainer rule "prove the code ran"): add a counter
`g_jit_native_calls` bumped in jit_call_setup; a `jit:` test asserts it > 0 on
a call-in-loop bench; measure same-binary JIT off/on A/B on `08_func_call` /
`12_higher_order` (`build/mylang -nj` vs default). Backtrace test: an exception
from a native-called leaf shows byte-identical frames (the record's ret_chunk/
ret_pc from the baked caller_desc). `-rt` + `nested_fuzz.py` green. COMMIT.

## KEY GOTCHAS
- A fragment holds NO chunk pointer (loc-less design; the chunk is stack-built
  in codegen_chunk then std::move'd out — baking `&chunk` DANGLES, an ASan
  SEGV). Use baked `FuncDescriptor*` (stable) + `->vm_chunk`.
- Nested activations (builtin callbacks re-enter vm_run) → SAVE+RESTORE
  g_current_act/ctx around vm_run.
- `flush_cache` uses rdi (slots base) — flush BEFORE clobbering rdi for a
  helper arg.
- `ML_VM_HARDENING` uses `#if` not `#ifdef` (a past trap).
- Rebuild before profiling; measure same-binary JIT off/on (never cross-binary
  — a past false-neutral trap). callgrind for deterministic I-counts.
- Build/test: `make -j TESTS=1 OPT=0 BUILD_DIR=build-dbg` (debug+ASan);
  `./build-dbg/mylang -rt`; `python3 tests/nested_fuzz.py`;
  `make -j OPT=1 ASSERTS=0 BUILD_DIR=build-rel` (perf); `-vdj FILE` (native
  disasm); `-nj` / `MYLANG_JIT=0` (kill switch, the A/B lever).
- ONLY x86-64 + !_WIN32 (the JIT is `#if ML_JIT_SUPPORTED`); off-platform the
  whole thing is a no-op, so keep the interpreter path correct regardless.

## v2/v3 (later, per maintainer guidance — see native-aot.md)
- v2: RECURSION (a self/mutual call that grows the C-stack — a growable native
  stack via realloc, the maintainer's intent) + THROWING native functions (the
  checked-return unwind: status in rax, `test/jnz` after each call,
  frame-by-frame native pop) + the self-recursion fully-native fixpoint.
- v3: a REASSIGNED (dyn) callee lowers natively via a runtime TYPE CHECK
  (matching-signature FuncObject → call; non-function → raise NotCallableEx
  via g_vm_jit_exc; unknown signature → a bytecode ISLAND).
