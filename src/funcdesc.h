/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "uniqueid.h"

#include <string>
#include <vector>

/*
 * The SERIALIZABLE runtime function descriptor (plans/vm-ast-free-runtime.md).
 *
 * FuncDescriptor is the RUNTIME identity of a function: everything
 * do_func_call, closure creation, backtraces, the per-frame pure cache, and
 * the reflection builtins need at run time, held OUTSIDE the AST. A
 * FuncObject points at a FuncDescriptor - never at a FuncDeclStmt - so after
 * codegen a `-vm` script can FREE the whole AST (the debug-build teardown
 * proof in mylang.cpp) and the VM still runs: closures build from
 * `Chunk::closure_defs` (descriptor pointers), params bind from `params`,
 * captures snapshot via `captures` (resolved kind/slot, no capture
 * Identifiers), and the compiled body is `vm_chunk`.
 *
 * Ownership: created by the FuncDeclStmt ctor (`desc_owner`, with `desc` the
 * stable raw alias) and MOVED into the VmProgram by vm_compile, so the
 * descriptor outlives the AST under `-vm`; under the tree-walker engine it
 * simply lives and dies with its decl. `decl` is the compile-time /
 * tree-walker back-pointer (body eval, REPL :show) - NULLED by the AST
 * teardown and never serialized.
 *
 * Field policy (single storage, no sync drift): the mutable compile results
 * (`resolved`, `frame_size`, purity, `display_name`, `cache_results`,
 * `vm_chunk`) live ONLY here - the resolver/inliner write the descriptor
 * directly, so a compile-time fold that calls the function mid-pipeline
 * always reads current data. Param/capture metadata is a snapshot of parse/
 * resolve results that cannot change afterwards (`sync_params` at parse end +
 * clone; captures stamped by the resolver when it resolves the capture list).
 */

/*
 * Explicit type annotation on a declaration / parameter (e.g. `int x = 5;`,
 * `func f(str s)`). `none` = no annotation (plain `var`/inferred). The scalar
 * kinds pin the symbol's static type and check assignability (with bool<=int<=
 * float coercion); `arr`/`dict` are generic kind constraints whose element/key/
 * value types are still inferred. Set by the parser onto the decl/param
 * Identifier; read by the inferencer (typing/checking) and the runtime
 * (coercion + zero-value default-init). See the README "explicit types".
 */
enum class DeclType : unsigned char {
    none, b, i, f, s, arr, dict,
    strct,   /* a user struct type; the exact type is in `decl_struct` */
    dyn,     /* `dyn` as a type (used inside a TypeAnnot, e.g. `array<dyn>`) */
};

/*
 * Result of the name-resolution pass (resolver.cpp) for an Identifier.
 *
 * `local` means the identifier was resolved to a fixed slot in the current
 * function call's Frame (see eval.h), so it's an O(1) array index instead of a
 * scope-chain map lookup. `unresolved` (the default) means "fall back to the
 * runtime EvalContext map walk" - used for the few things the resolver doesn't
 * handle: builtins, REPL globals, and genuinely-undefined names.
 */
enum class SymKind : unsigned char {
    unresolved,
    local,      /* slot in the CURRENT call's Frame (ctx->frame) */
    global,     /* slot in the program-wide global table (ctx->gfuncs) - a
                 * top-level function/variable, reachable from any call depth */
    capture,    /* slot in the closure's per-instance capture vector
                 * (ctx->captures) - a captured outer variable, snapshot at
                 * closure creation, persists across calls to that closure */
    builtin,    /* slot in the program-wide builtin table (builtin_slot) - a
                 * builtin not shadowed by a user symbol; a global constant */
};

struct ResolvedSym {
    SymKind kind = SymKind::unresolved;
    int slot = -1;          /* index into Frame::slots when kind == local */
};

class FuncDeclStmt;

struct FuncDescriptor {

    /* One parameter's binding + signature metadata (a snapshot of the param
     * Identifier's parse-time fields; see sync_params). */
    struct ParamDesc {
        const UniqueId *name;
        bool opt;                    /* trailing-skippable, binds none */
        bool cnst;                   /* `const x` - binds as a const LValue */
        bool dyn_mod;                /* `dyn`/`~` (signature rendering only) */
        DeclType decl_type;          /* i/f trigger the bind-time coercion */
    };

    /* One capture-list entry, RESOLVED: closure creation snapshots the value
     * by kind/slot (no capture Identifier eval). `unresolved` (REPL globals)
     * falls back to a name lookup in the creating context's map chain. */
    struct CaptureDesc {
        const UniqueId *name;
        SymKind kind;
        int slot;
    };

    const UniqueId *name = nullptr;  /* null for a lambda */
    std::string display_name;        /* backtrace override (spec clones) */
    std::vector<ParamDesc> params;
    std::vector<CaptureDesc> captures;

    /* Compile results (see the field policy above). */
    bool resolved = false;
    int frame_size = 0;

    /* 1 + index of the last non-opt param; computed by sync_params (no lazy
     * per-call recompute). */
    int min_args = 0;

    bool explicit_pure = false;
    bool effective_pure = false;
    bool cache_results = false;
    /* The decl's parse-time const flag (`pure func`): an UndefinedVariableEx
     * escaping the body is tagged as a pure-func restriction error. */
    bool pure_ctx = false;
    /* A never-called monomorphization source: proven callee-less, so it has
     * neither a chunk nor (post-teardown) a body - calling it is a bug. */
    bool is_template_base = false;

    /*
     * The bytecode VM's compiled body chunk (an opaque `const Chunk *`; this
     * header stays VM-agnostic). Filled UPFRONT under -vm by
     * vm_precompile_all; `vm_chunk_tried` guards the one-time compile.
     */
    mutable const void *vm_chunk = nullptr;
    mutable bool vm_chunk_tried = false;

    /*
     * COMPILE-TIME / tree-walker back-pointer to the owning decl (body eval,
     * func_expr_body fast path, REPL :show). NULLED by the debug AST teardown
     * (mylang.cpp) - after it, only chunk execution remains. Never serialized.
     */
    const FuncDeclStmt *decl = nullptr;
};
