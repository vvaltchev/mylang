# The AST-free VM runtime (Tier 4): FuncDescriptor, VmProgram, AST teardown

Maintainer directive (2026-07-15): before any serialization work, remove the
LAST runtime dependence on the AST — the call model (`closure_defs` holding
`FuncDeclStmt*`, `do_func_call` binding params via AST `Identifier`s,
`StructTypeDef` owned by `StructDeclStmt`, chunks cached ON decl nodes) — and
PROVE zero dependence by, in debug builds, recursively memset(0)-ing and
freeing every AST object right before the VM engine starts. Everything the VM
needs must be self-contained in new, serialization-shaped data structures.

## The complete Tier-4 inventory (what the runtime still reads off the AST)

1. `FuncObject::func` is a `const FuncDeclStmt *`. Readers at runtime:
   - `do_func_call`: `resolved`, `frame_size`, `params->elems` (bind, incl.
     `opt_mod`/`const_param`/`decl_type` per param), `min_args_cache`,
     `vm_chunk`/`vm_chunk_tried`, `func_expr_body(func)` + `body->eval`
     (tree-walk), and `is_const`/`display_name`/`id`/param names in
     `vm_capture_frame` (backtrace).
   - `cached_call`: `PureCacheKey{func, args}` (identity only).
   - builtins: `ispure`/`ispuredecl` (`effective_pure`/`explicit_pure`),
     `signature`/`typestr`-of-func (`reflect_func_sig` → name + params),
     `specializations` (name/display_name), REPL `show` (`render_func_code`).
   - `FuncObject`'s ctor evals the decl's CAPTURE-list `Identifier`s.
2. `Chunk::closure_defs` holds `const FuncDeclStmt *` (MakeClosureV).
3. Function chunks are stored on the DECL (`FuncDeclStmt::vm_chunk`, backed by
   a `g_func_chunks` map keyed by `FuncDeclStmt*`).
4. `StructTypeDef` is OWNED by `StructDeclStmt::def`; referenced from struct
   instances, `t_structtype` const-pool values, `struct_defs`/`boxed_ctors`/
   `emplace_sites`/`literal_objs.arr_hint_struct`, exception payloads.
5. `codegen_func_body` declines a non-scope-free body and an all-control-op
   body → those bodies TREE-WALK under `-vm` (Tier-3 residual).
6. Root-block data the VM run needs: `slot_count`, `global_func_names`.

## New data structures (serialization-shaped)

### `FuncDescriptor` (funcdesc.h) — the function's RUNTIME identity

Owned by its `FuncDeclStmt` (`desc_owner`, created in the ctor; `desc` is the
stable raw alias) until `vm_compile` MOVES ownership into the `VmProgram`.
`FuncObject::func` becomes `const FuncDescriptor *` — both engines, one call
model. Fields:

- `const UniqueId *name` (null = lambda) + `std::string display_name`
- `params`: `vector<ParamDesc{const UniqueId *name; bool opt; bool cnst;
  bool dyn_mod; DeclType decl_type;}>` — binding + `signature()`
- `captures`: `vector<CaptureDesc{const UniqueId *name; SymKind kind;
  int slot;}>` — closure creation snapshots WITHOUT the capture Identifiers
- `resolved`, `frame_size`, `min_args_cache` — MOVED from FuncDeclStmt
  (single storage: the resolver/inliner write the descriptor directly, so a
  compile-time fold that calls the function mid-pipeline reads current data)
- `explicit_pure`, `effective_pure`, `cache_results`, `pure_ctx`
  (= the decl's parse-time `is_const`, the pure-func error tag),
  `is_template_base` — MOVED/copied
- `vm_chunk`/`vm_chunk_tried` — MOVED (still filled AOT by precompile)
- `const FuncDeclStmt *decl` — the COMPILE-TIME/tree-walker back-pointer
  (body eval, REPL `show`); NULLED at AST teardown; never serialized.

Param/name data is synced from the decl ONCE at parse end (`sync_params`,
called by pAcceptFuncDecl and clone()); captures are synced by the resolver
when it resolves the capture list. Everything else lives ONLY in the
descriptor — there is no second copy to drift.

### `VmProgram` (vm.h) — the whole runtime image

- root `Chunk`, root `slot_count`, `global_func_names` (copied strings/uids)
- `vector<unique_ptr<FuncDescriptor>>` (moved from the decls)
- `vector<unique_ptr<StructTypeDef>>` (moved from the StructDeclStmts;
  the decl keeps a raw alias for compile-time reads)
- the per-function chunk storage (was `g_func_chunks`), keyed by descriptor.

`vm_execute(root)` splits into `vm_compile(root) -> VmProgram` (codegen +
AOT precompile + ownership transfer) and `vm_run(program)`. The `-rt`
harness keeps calling `vm_execute` (compile+run, AST retained); the script
driver calls the two halves so the teardown can run between them.

## Chunks for EVERY callable body

`codegen_func_body` keeps only the `is_template_base` skip (proven never
called — enforced by an ML_CHECK in do_func_call when both chunk and decl are
absent). The "has at least one real op" gate is DROPPED (an empty body chunk
is a Halt returning none — correct and negligible), and a NON-scope-free body
(only reachable via the pathological >64-param unslottable function) becomes
a compile-time `NotLoweredEx` instead of a silent tree-walk.

## The debug-build teardown proof (the point of it all)

- `Construct::operator new/delete` exist in ALL non-NDEBUG builds (today:
  RECYCLE only). Debug delete `memset(0)`s the node's bytes before freeing
  (sized class delete); a global live-node counter is ++/-- maintained.
- After `vm_compile` the script driver (mylang.cpp, `-vm`) destroys the whole
  AST (`root.reset()` — the normal recursive unique_ptr teardown) and
  `ML_CHECK(Construct::live_count == 0)`: nothing survived, and any residual
  pointer now reads zeroed, freed memory (ASan/RECYCLE lanes make a stale
  deref loud). Then `vm_run(program)` executes — conclusively AST-free.
- Scope: script mode only. The REPL and the `-rt` harness retain their ASTs
  by design (decompilation, differential testing); release builds skip the
  teardown (the user asked for it in debug builds).

## Execution order

1. FuncDescriptor + field migration + FuncObject/do_func_call/backtrace/
   builtins/reflect/disasm over the descriptor (no behavior change).
2. `closure_defs` → descriptors; chunk storage keyed by descriptor.
3. VmProgram + StructTypeDef ownership transfer + vm_compile/vm_run split.
4. codegen_func_body gate changes (chunk for every callable body).
5. Debug teardown + live-count proof + tests (script-mode e2e, fuzzer,
   bench/samples under a debug -vm build).
6. Docs (CLAUDE.md ⛔ list, this plan, memory) + full validation matrix + CI.

Each step lands `-rt`-green (headline + differential) before the next.
