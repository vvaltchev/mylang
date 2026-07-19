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

/*
 * Drive a chunk against `ctx` (a function body's args context, or main). Runs
 * until Halt or an in-flight `return`; the caller reads ctx->flow->value.
 *
 * `reentrant` (#60): the caller is a builtin CALLBACK loop (VmInvoker /
 * vm_try_invoke) that has ALREADY set up the activation + boundary window and
 * announced g_current_ctx. In that case this skips the per-invocation entry
 * setup (vm_enter_invocation_fast + the CtxGuard store) - the window/activation
 * are the caller's and g_current_ctx is already `&ctx` - which is pure
 * per-element overhead the callback loop would otherwise re-pay each element.
 */
void vm_run_chunk(const Chunk &chunk, EvalContext &ctx, bool reentrant = false);

/*
 * Allocate / release a callee frame WINDOW on the current activation's
 * segmented slot stack (plans/vm-native-call-stack.md): do_func_call binds a
 * chunked body's params into the returned view Frame instead of constructing
 * a per-call Frame. push returns null when no activation is live (the caller
 * falls back to a plain Frame); throws the catchable StackOverflowEx at the
 * MYLANG_VM_STACK cap. Window addresses are STABLE (segments never move).
 */
Frame *vm_window_push(int_type nslots, const Chunk *ck);
void vm_window_pop();

/*
 * Phase D: run a builtin's USER-CALLBACK (eval_func's funnel) as a boundary
 * frame on the current activation - no per-element do_func_call/
 * EvalContext. False = not available (no activation / const-eval / no
 * chunk); the caller falls back to do_func_call. May throw exactly what
 * do_func_call would (arity, bind coercion, the callee's exceptions with
 * this frame's backtrace entry appended).
 */
class FuncObject;
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
    std::vector<LValue> *saved_caps_ = nullptr;
    /* #60: g_current_ctx is owned for the whole loop (set once in the ctor,
     * restored in the dtor) so a reentrant invoke() need not re-store it. */
    EvalContext *saved_gctx_ = nullptr;
    /* Loop-fixed arity fields (hoisted from desc_ - read once, not per call). */
    size_t nparams_ = 0;
    size_t min_args_ = 0;
};
void vm_window_pop();

/*
 * The COMPLETE compiled program image (plans/vm-ast-free-runtime.md): the root
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
    std::vector<std::unique_ptr<FuncDescriptor>> funcs;
    std::vector<std::unique_ptr<StructTypeDef>> structs;
};

/*
 * The runtime bytecode VM - the -vm execution engine (plans/bytecode-vm.md).
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
VmProgram vm_compile(const Construct *root);
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
