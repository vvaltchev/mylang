/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"   /* Chunk */
#include <memory>
#include <vector>

class Construct;
class UniqueId;
struct StructTypeDef;
class FuncDeclStmt;
struct FuncDescriptor;
class EvalContext;
struct Frame;
struct RuntimeException;

/*
 * P8 Inc v2 (cross-frame propagation WITHOUT per-frame C++ unwinding): a
 * VM-body exception that finds no handler in its own frame is converted, at
 * that frame's boundary, into this GLOBAL pending-exception signal instead of a
 * C++ re-throw. do_func_call captures its frame + propagates the signal; each
 * VM call op (CallV/CachedCallV/CallValueV) checks it after a call and either
 * dispatches to a same-frame handler or returns to keep propagating - so a
 * throw crossing N frames pays ONE C++ landing-pad (at its origin), not N. Null
 * except while such an exception is in flight (exception-free code pays zero).
 * vm_execute converts a still-pending signal at the top back into a C++ throw
 * for the mylang.cpp handler.
 */
extern std::unique_ptr<RuntimeException> g_vm_exc_pending;

/*
 * The compiled body chunk for a function (Phase 4); the storage lives in
 * vm.cpp, keyed by DESCRIPTOR, cleared per program. Null only for a
 * never-called template base; EVERY other body compiles (post-teardown the
 * chunk is the only way to run it - the no-fail rule). Filled AOT by
 * vm_precompile_all; this entry point is the never-hit lazy safety net.
 */
/* `jit` (default true): jit-compile the body immediately. The AOT precompile's
 * codegen pass passes FALSE (it jits every body in a later pass, so a caller's
 * native-call gate sees every callee's native_leaf flag - #55 STEP 2). */
const Chunk *vm_func_chunk(const FuncDescriptor *fdesc, bool jit = true);

/* .myv LOAD (plans/myv-serializer.md): install a deserialized chunk as a
 * descriptor's body - the loader's equivalent of what vm_precompile_all
 * does for a fresh compile (own the chunk, stamp vm_chunk/vm_chunk_tried).
 * The AOT native tier is (re-)run here too: only the VM image is stored. */
void vm_install_func_chunk(const FuncDescriptor *fdesc, Chunk &&ck);



/* .myv LOAD: re-resolve a builtin by NAME against the singleton table
 * (a stored image names its builtins; an unknown name is refused). */
bool vm_lookup_builtin(const UniqueId *name, Builtin &out);

/*
 * Drive a chunk against `ctx` (a function body's args context, or main). Runs
 * until Halt or an in-flight `return`; the caller reads ctx->flow->value.
 *
 * #60 (b): this is the per-INVOCATION entry (activation + boundary window +
 * g_current_ctx setup) around the bare dispatch loop (vm_dispatch, file-local
 * in vm.cpp). A builtin CALLBACK loop (VmInvoker / vm_try_invoke) that already
 * owns those re-enters vm_dispatch DIRECTLY, paying no per-element entry setup.
 */
void vm_run_chunk(const Chunk &chunk, EvalContext &ctx);

/*
 * model-flip M2 (plans/model-flip.md): headless self-test for vm_exec_block -
 * the interpreted-ISLAND executor the native container will call. Hand-builds
 * tiny islands (fall-through, an internal boxed branch, a return, an uncaught
 * throw) and asserts the returned status + resulting state. Defined only under
 * TESTS; returns true on success. Called from the -rt table.
 */
bool vm_exec_block_selftest();

/*
 * Allocate / release a callee frame WINDOW on the current activation's
 * segmented slot stack (plans/archived/vm-native-call-stack.md): do_func_call binds a
 * chunked body's params into the returned view Frame instead of constructing
 * a per-call Frame. push returns null when no activation is live (the caller
 * falls back to a plain Frame); throws the catchable StackOverflowEx at the
 * MYLANG_VM_STACK cap. Window addresses are STABLE (segments never move).
 */
Frame *vm_window_push(int_type nslots, const Chunk *ck);
void vm_window_pop();

#ifdef TESTS
/* lever 2 execution proof: VmInvoker's DIRECT fragment entries */
extern unsigned long g_jit_invoke_direct;
/* lever 4 execution proof: per-element runs of the SPECIALIZED dyn-foreach
 * Next bodies (resolved once at ForeachDynInit): 0 int / 1 float / 2 bool /
 * 3 gen / 4 dict */
extern unsigned long g_dyn_foreach_fast[5];
/* H3 (#159) execution proof: unpack binds that took the direct SharedStr
 * copy-assign (no type-erased dispatch) instead of put() */
extern unsigned long g_unpack_fast_binds;
#endif

/*
 * Phase D: run a builtin's USER-CALLBACK (eval_func's funnel) as a boundary
 * frame on the current activation - no per-element do_func_call/
 * EvalContext. False = not available (no activation / const-eval / no
 * chunk); the caller falls back to do_func_call. May throw exactly what
 * do_func_call would (arity, bind coercion, the callee's exceptions with
 * this frame's backtrace entry appended).
 */
class FuncObject;
class CaptureSlots;   /* a closure's captured values (eval.h) */
bool vm_try_invoke(EvalContext *caller_ctx, FuncObject &obj,
                   const EvalValue *argv, size_t n, EvalValue &out);

/*
 * Phase D, the LOOP form (roadmap C2): a builtin that invokes the SAME
 * callback per element (map/filter/sort's comparator/make_dict/find/
 * make_array) prepares the call ONCE - one boundary window push, one
 * captures switch - and each invoke() just REBINDS the param slots and
 * re-enters the dispatch loop: no per-element do_func_call, EvalContext,
 * window push/pop, or record churn. Reference slots are reset between
 * elements (per-call frame-death semantics preserved - COW/use_count
 * behavior stays byte-identical to fresh frames). RAII: the dtor pops the
 * window and restores captures even when a callback throws. When !ready()
 * (tree-walk engine, const-eval, no activation, chunk-less callee) the
 * builtin falls back to eval_func per element.
 */
struct VmActivation;
class VmInvoker {
public:
    VmInvoker(EvalContext *ctx, FuncObject &obj);
    ~VmInvoker();
    VmInvoker(const VmInvoker &) = delete;
    bool ready() const { return ready_; }
    EvalValue invoke(const EvalValue *argv, size_t n);

private:
    bool ready_ = false;
    bool fast_bind_ = false;
    VmActivation *act_ = nullptr;
    EvalContext *c_ = nullptr;
    const Chunk *cck_ = nullptr;
    const FuncDescriptor *desc_ = nullptr;
    Frame *w_ = nullptr;
    CaptureSlots *saved_caps_ = nullptr;
    /* #60: g_current_ctx is owned for the whole loop (set once in the ctor,
     * restored in the dtor) so invoke() re-enters vm_dispatch with no per-
     * element CtxGuard store. */
    EvalContext *saved_gctx_ = nullptr;
    /* Loop-fixed arity fields (hoisted from desc_ - read once, not per call). */
    size_t nparams_ = 0;
    size_t min_args_ = 0;
    /* Lever 2: the callee fragment's DIRECT entry (body starts native),
     * cached once per loop; null = the vm_dispatch fallback. */
    const char *entry_ = nullptr;
};
void vm_window_pop();

/*
 * The COMPLETE compiled program image (plans/archived/vm-ast-free-runtime.md): the root
 * chunk, the root-context data the run needs (slot count + the global table's
 * names), and - crucially - OWNERSHIP of every FuncDescriptor and
 * StructTypeDef, MOVED here from the AST by vm_compile. After that transfer
 * the whole AST is droppable: closures build from descriptor pools, struct
 * instances/type values point at program-owned defs, and every function body
 * is a precompiled chunk. This is the in-memory shape of the future `.myv`
 * file (per-function chunks are keyed by descriptor in vm.cpp's per-run
 * storage; they serialize alongside the descriptors).
 */
struct VmProgram {
    Chunk root;
    int root_slot_count = 0;
    std::vector<const UniqueId *> global_func_names;
    /* the root block's per-global-slot "may be reassigned" flags - the
     * native-call gate reads them, so a .myv carries them (else a loaded
     * image could not rebuild the same native tier). */
    std::vector<char> global_slot_reassigned;
    std::vector<std::unique_ptr<FuncDescriptor>> funcs;
    std::vector<std::unique_ptr<StructTypeDef>> structs;
};

/*
 * The runtime bytecode VM - the -vm execution engine (plans/archived/bytecode-vm.md).
 * `root` is the OPTIMIZED program AST (post infer / resolve_names /
 * specialize_types), exactly what the tree-walker's root->eval(nullptr) runs.
 * vm_execute lowers it to bytecode and drives it through the SAME root context
 * the tree-walker builds, so observable behavior is identical.
 *
 * It is two phases the script driver may call separately:
 *   vm_compile - codegen the root, AOT-precompile every function body, and
 *                TRANSFER descriptor/struct-def ownership into the returned
 *                VmProgram (the AST keeps raw aliases; nothing the runtime
 *                reads lives in the tree anymore);
 *   vm_run     - build the root EvalContext/Frame/GlobalFuncTable from the
 *                program image alone and execute.
 * Between the two, a debug-build script run DESTROYS the whole AST (the
 * teardown proof in mylang.cpp). vm_execute = compile + run (the -rt
 * harness's path, AST retained for the differential oracle).
 */
/* `jit` false: skip the native AOT tier - the .myv WRITER stores PRE-jit
 * bytecode (the JIT rewrites code in place: EnterNative ops whose fragments
 * are NOT serialized), and the LOADER re-runs the tier itself. */
/* .myv LOAD: run the AOT native tier over a fully-read image - the loader's
 * equivalent of vm_precompile_all's two passes (set every native_leaf flag,
 * then jit each body with a JitCtx rebuilt from the image), so a loaded
 * program gets the IDENTICAL native tier a fresh compile does. */
void vm_jit_loaded_image(VmProgram &prog);

/*
 * #137: REFUSE a structurally impossible image, BEFORE the JIT or the
 * interpreter indexes anything in it. Runs verify_chunk (codegen.h) over the
 * root and every function body.
 *
 * It lives here, not in the loader, because the limits ARE VM facts: a
 * chunk's frame is `frame_size + n_temps` slots and its captures come from
 * the descriptor, so the bounds must be computed from the same expressions
 * vm_run/do_func_call size the frame with - a second copy would be free to
 * drift and would then either reject a valid image or accept a fatal one.
 *
 * THROWS (a plain "MyvError" Exception) - it guards hostile input, so it is
 * on in a release build too. The `.myv` loader calls it unconditionally;
 * vm_compile calls it under ASSERTS, where it is the net that proves the
 * per-opcode table agrees with what codegen actually emits.
 */
void vm_verify_program(const VmProgram &prog);

/*
 * #137 tier 1: can `aop` be dispatched by vm_num_binop?
 *
 * A boxed arith/compare op's `aop` is ONE RAW BYTE in a `.myv`, cast straight
 * into `enum class Op`. A corrupt one finds no method - `binop_pmf` and
 * `cmp_pmf` both return null - and the call then goes through a NULL
 * pointer-to-member. That was a real SIGSEGV in the DEFAULT release build
 * (the ML_VM_CHECK that catches it belongs to the VM_HARDENING tier, which a
 * release turns off).
 *
 * Exported so verify_chunk can refuse it AT LOAD, for free, in every build -
 * and exported rather than duplicated so the verifier asks the SAME two
 * tables the dispatch uses. A new operator joins them in one place.
 */
bool vm_aop_dispatchable(Op aop);

VmProgram vm_compile(const Construct *root, bool jit = true);
void vm_run(VmProgram &prog);
void vm_execute(const Construct *root);

/*
 * THE ZERO-AST PROOF (ASSERTS builds; a no-op under ASSERTS=0): called by the
 * script driver between vm_compile and vm_run. NULLs every descriptor's
 * compile-time `decl` back-pointer, DESTROYS the whole AST (the normal
 * recursive unique_ptr teardown - each freed node is memset(0) by the class
 * operator delete), and ML_CHECKs that the process-wide live Construct count
 * is ZERO. The VM then runs with the AST provably gone: everything it needs
 * is self-contained in the VmProgram (+ the per-descriptor chunks). The REPL
 * and the -rt harness never call this (they retain their ASTs by design).
 */
void vm_ast_teardown(std::unique_ptr<Construct> &root, VmProgram &prog);

/*
 * Which engine the test harness (tests.cpp check()) runs a program with, so the
 * SAME functional suite runs under BOTH the tree-walker (the oracle) and the VM
 * and must match - the differential-testing pillar. The script / -e path
 * selects the engine directly in mylang.cpp; this global is only the harness's
 * switch.
 */
enum class ExecEngine { TreeWalk, Vm };
extern ExecEngine g_exec_engine;
