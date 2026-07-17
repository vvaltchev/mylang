# The `.myv` serializer — stored bytecode (design, 2026-07-18)

Status: DESIGNED, not started. This is the endgame artifact the whole
zero-AST campaign built toward (`[[vm-endgame]]`): `mylang -c file.my`
writes a binary the interpreter runs with NO source, NO parse, NO
optimizer passes — load and go. Every prerequisite is proven: the
runtime is machine-checked AST-free (`vm_ast_teardown`), the call model
runs on serializable `FuncDescriptor`s, the no-fail codegen makes every
chunk serializable BY CONSTRUCTION (no fallback op exists), and `-vd`
already dumps 100% of the image — it is the audit surface this file's
format mirrors.

## Decision record: NO in-format compression

The format is RAW, deterministic, fixed-stride records. Rationale:

1. **The no-deps rule decides it structurally**: mylang cannot link
   zstd/zlib, so it could never DECOMPRESS — in-format compression
   would mean hand-rolled varint/LEB decode, i.e. exactly the
   variable-length machinery the 16-byte-instruction experiment
   measured and rejected (see the parking lot's Rejected list; the
   load-time version of that decode is cheaper but still code, bugs,
   and audit surface for ~nothing).
2. **External compression beats varint anyway**: fixed 32-byte records
   with mostly-zero/-1 fields are ideal zstd input; varint encoding
   destroys the regularity a compressor feeds on. A user who cares
   about bytes on the wire runs `zstd file.myv` — the transport layer's
   business, not the format's.
3. **Files are small**: 100k instructions ≈ 3 MB. Not a concern for an
   educational language's compiled scripts.

Corollary: the loader is near-trivial (sized reads into vectors), and
the write side is a mirror of `-vd`'s enumeration.

## Not a security boundary (v1 stance)

A `.myv` is TRUSTED input, like a `.pyc`: the loader does structural
validation (magic/version, section sizes, every index/pc bounds-checked
in ONE pass at load — a corrupt file gets a clean "corrupt or
incompatible .myv" error, never UB), but a maliciously CRAFTED file is
out of scope and documented as such. No cross-version compatibility:
`format_version` must match exactly, else "recompile from source".

## What executes from a `.myv` (the image inventory)

Everything `vm_run(prog)` touches, i.e. the `VmProgram` closure:

- **VmProgram**: the root chunk, `root_slot_count`,
  `global_func_names` (the global table's slot→name list, also feeds
  `globals()` reflection), `prog.funcs` (every `FuncDescriptor`),
  `prog.structs` (every `StructTypeDef`).
- **FuncDescriptor**: name / `display_name`, `ParamDesc[]` (uid, opt,
  const, `decl_type`), the resolved capture list (kind/slot pairs),
  `frame_size`, `min_args`, purity flags, `fast_bind`,
  `is_template_base`, `cache_results`, `pure_ctx`, and its CHUNK (by
  index). `decl` stays null in-file (compile-only back-pointer).
- **StructTypeDef**: name + `FieldDef[]` (name, kind, struct
  reference BY INDEX, opt, annot if runtime-read — audit in S3) +
  folded `consts` (values). Layout (size/align/offsets) is RECOMPUTED
  at load via `compute_layout` — deterministic from the field kinds,
  so serializing it would only add drift surface.
- **Chunk**: `code` (`Instr[]`), `n_temps`, `n_dict_iters`,
  `n_dyn_iters`, `slot_count`, `ref_slots` (SERIALIZED, not recomputed
  — it derives from the pre-slice fat stream, which no longer exists at
  load), and every pool exactly as `-vd` enumerates them: `consts`,
  `member_keys`, `catch_types`, `literal_objs`, `builtin_calls`,
  `emplace_sites`, `call_sites`, `incdec_sites`, `incdec_chains`,
  `chain_steps`, `chain_locs`, `throws`, `unpack_targets`,
  `unpack_coerce`, `boxed_ctors`, `closure_defs` (descriptor indices),
  `struct_defs` (struct indices), `locs`, `inline_ctxs` +
  `inline_frames`. `slot_names` is the one OPTIONAL (debug) section —
  `-vd` only; `locs`/`inline_frames` are NOT optional (error carets
  and backtraces are language behavior).
- **Values** (the recursive `EvalValue` codec — consts, literal_objs,
  struct consts, dict defaults): tag byte + payload for none / bool /
  int / float / str / array (WITH its storage kind — ints / floats /
  bools / strs / structs / general — and the deep-readonly flag; flat
  storage round-trips AS flat, `array_storage()` is observable) / dict
  (pairs + `has_default`/`default_val` + readonly) / struct instance
  (def index + POD bytes or boxed fields) / struct DESCRIPTOR
  (`t_structtype` → def index) / FuncObject (descriptor index;
  capture-free by construction in a const pool — assert). The writer
  ABORTS on any unlisted type tag (loud, like `throw_not_lowered`) —
  S0 empirically enumerates what actually occurs by sweeping
  bench/ + samples/ + the test table.
- **Cross-references**: every `UniqueId *` becomes a string-table
  index (the loader re-interns); every `Builtin` becomes its NAME
  (the loader re-resolves against the builtin table and refuses a
  file naming an unknown builtin); struct/descriptor pointers become
  table indices (writers emit in dependency order: strings, structs
  topologically — inline fields only reference earlier defs, a
  parser invariant — then descriptors, then chunks).

## The file format

All integers little-endian. Big-endian hosts are unsupported in v1
(refused via the endianness marker; MyLang targets x86-64/arm64).
Fields are written FIELD-WISE, never by struct memcpy — `Instr` has
padding bytes whose content is indeterminate; the on-disk instruction
is the 27 meaningful bytes (op, aop, opflags, target, target2, pa, pb)
at a fixed stride, read back into a default-constructed `Instr`.

```
[Header]
  magic          "MYLV"            (detected by CONTENT, not extension)
  format_version u32               (exact match required)
  endian_mark    u32 = 0x01020304
  lang_version   str               (diagnostic only)
  flags          u32               (bit0: source embedded)
  section TOC    (id, offset, size) — forward-seekable, skippable
[Strings]   count + (len, bytes)*          — deduped; all names herein
[Source]    the original source text       — OPTIONAL (see below)
[Structs]   count + defs (dependency order)
[Descs]     count + descriptors (chunk by index)
[Chunks]    count + chunk records (root = index 0)
[Globals]   root_slot_count, global_func_names (string indices)
[Debug]     slot_names per chunk           — OPTIONAL, strippable
```

**Source embedding**: error rendering prints the offending source line
+ caret (`dumpLocInError`) — that TEXT isn't derivable from a `Loc`.
Default: `-c` EMBEDS the source (error quality is core to this
project; the cost is the script's own size). `-c --strip-source`
omits it; `errfmt` gains a no-source mode that prints
`line N, col A:B: <Ex>: msg` without the caret block. Backtraces need
no source (names come from descriptors, lines from Locs).

**Determinism goal**: compiling the same source twice yields
byte-identical `.myv` (codegen is single-threaded and pool orders are
deterministic; no timestamps in the header). Pinned by a
compile-twice-and-cmp test — it makes the format diffable and the
round-trip oracle exact.

## CLI

- `mylang -c file.my [-o out.myv] [--strip-source]` — full pipeline
  (parse → infer → optimize → vm_compile), then serialize and EXIT
  (no execution; `-c` implies `-vm` semantics).
- `mylang file.myv [args]` — magic-sniffed (first 4 bytes), loaded,
  `vm_run`. Extension is convention only.
- `mylang -vd file.myv` — disassemble a LOADED image: the round-trip
  audit tool and the everyday "what's in this file" answer.
- Version mismatch / corrupt file / unknown builtin → a clean
  `Exception` ("recompile from source"), exit 1.

## Loader steps

1. Header: magic, version (exact), endianness; TOC.
2. Strings → intern each → `vector<const UniqueId *>`.
3. Structs: build `StructTypeDef`s (fields wired by index — always
   backward), run `compute_layout` + `check`s; the program owns them
   (`prog.structs`).
4. Descriptors pass 1: construct all (names/params/captures/flags).
5. Chunks: code (field-wise reads), pools, values (recursive codec),
   builtin names → `Builtin` (refuse unknown), struct/desc indices →
   pointers; the ONE-PASS structural verifier (every pc field within
   code bounds via `visit_pc_fields`, every pool index in range,
   `ref_slots`/targets within the frame) — cheap, once, then the VM
   trusts the image exactly as it trusts a fresh compile.
6. Descriptors pass 2: stamp `vm_chunk` (+ `g_func_chunks`), matching
   `vm_precompile_all`'s end state.
7. `vm_run(prog)` — the identical entry the script driver uses today;
   `prog` owned outside the try (exception payloads/backtraces
   reference its defs/descriptors — the lazy-BacktraceFrame rule).

## Serializer steps

A `Serializer` over the finished `VmProgram` (post-`vm_compile`; the
AST may already be torn down — the image is closed). Writer/reader
primitives (`u8/u32/i64/f64/loc/str_idx/value`) with the write and
read of each record kept ADJACENT in one file (`serialize.cpp`, a new
TU) so the pair can't drift; any format change bumps
`format_version` in the same commit (a checklist comment at the top).

## Implementation phases (each lands validated; no big bang)

- **S0 — the value codec.** `EvalValue` write/read + unit tests:
  round-trip every storage kind (flat i/f/b/strs/structs + general),
  readonly deep-flags, dict defaults, POD + boxed structs, nested,
  `use_count()==1` freshness after load. Plus the pool-type SWEEP: a
  scan asserting which tags actually occur across bench/ + samples/ +
  the test table (the writer's abort-on-unknown net thereafter).
- **S1 — chunk codec + the ORACLE.** code + scalar pools + locs;
  `-c`/load for straight-line programs; and FIRST the round-trip gate:
  `-vd` of the in-memory compile vs `-vd` of the loaded file must be
  BYTE-IDENTICAL (the dump-diff technique that caught pp_thread). This
  oracle gates every later phase.
- **S2 — descriptors.** funcs/closures/captures/`g_func_chunks`;
  recursion + closure + call benches round-trip; backtrace parity
  from a loaded image (the lazy frames' descriptor lifetimes).
- **S3 — structs.** defs (+ the FieldDef runtime-field audit),
  emplace_sites/boxed_ctors/struct pools; struct benches round-trip.
- **S4 — the long tail.** call_sites, incdec_*, chains, throws,
  unpack_*, inline_frames; then the FULL sweep: all of bench/ +
  samples/ round-trip with byte-identical `-vd` AND output-identical
  runs vs `-vm`; error/caret/backtrace cases pinned.
- **S5 — productization.** magic sniffing, `-o`, `--strip-source` +
  the no-source errfmt mode, the corrupt-file verifier, determinism
  (compile-twice cmp), `nested_fuzz.py --myv` (compile→file→load→run
  as a fourth agreement lane), a CI round-trip step, README (user
  docs) + CLAUDE.md (implementation) in the same commits.

## Testing obligations (the optimizations-must-generalize bar)

- The `-vd` byte-identity oracle over bench/ + samples/ (S1 onward).
- Output-identical execution vs `-vm` for every bench/sample (S4).
- Error parity: carets (with source), the no-source mode, backtraces
  incl. inlined virtual frames, `StackOverflowEx`, caught-user-throw
  round trips.
- REPL is OUT of scope (it retains ASTs; `.myv` is the script path).
- Determinism cmp; corrupt-file refusals (truncation, bad magic, bad
  index — each a clean error, fuzzed by bit-flipping in S5).

## Deferred (explicitly not v1)

- Cross-version compatibility / migration.
- Signing/verification, sandbox hardening.
- A combined archive (multiple scripts per file).
- mmap-zero-copy loading (the sized-read loader is plenty; revisit
  only if load time ever measures as a problem).
