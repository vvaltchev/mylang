# The `.myv` serializer — stored bytecode (design, 2026-07-18)

Status: **IMPLEMENTED (v2, 2026-07-29)** - `-c` writes, a magic-sniffed
file argument loads and runs, `-vd` dumps a loaded image, the round-trip
oracle + determinism + corrupt-file refusal are pinned by `-rt`
(`myv_round_trip`). v2 replaced the embedded source text with a verified
SOURCE REFERENCE (see the section at the end); `MYV_FORMAT_VERSION` is 2
and a v1 file is refused. See CLAUDE.md's ".myv STORED-BYTECODE FORMAT" for the
implementation shape. Deltas from this design: the section TOC was not
needed (a linear reader suffices at these sizes); a BUILTIN-SET
FINGERPRINT was ADDED after finding that builtin slot indices were
pointer-ordered (fixed by sorting the table by name); `slot_names` is
stored unconditionally (it keeps `-vd` faithful and costs little).
Remaining v1 residual: 2 of 83 corpus dumps differ only in a const dict's
printed entry ORDER (unordered by spec).

Original status: DESIGNED, not started. This is the endgame artifact the whole
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

**Source embedding** (⚠ SUPERSEDED IN v2 — see "the SOURCE REFERENCE"
at the end of this file; kept here as the original design): error
rendering prints the offending source line + caret (`dumpLocInError`) —
that TEXT isn't derivable from a `Loc`. Default: `-c` EMBEDS the source
(error quality is core to this project; the cost is the script's own
size). `-c --strip-source` omits it; `errfmt` gains a no-source mode
that prints `line N, col A:B: <Ex>: msg` without the caret block.
Backtraces need no source (names come from descriptors, lines from
Locs). v2 keeps the no-source mode but stores a verified path + CRC32
instead of the text, so the caret works without the image carrying the
program.

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

## SIZE analysis (2026-07-29, measured on samples/)

The v1 format is FAT: an image is 4-6x its source, and even `zstd -3`
leaves it above the source's size. Where the bytes actually go (env
`MYLANG_MYVSTATS=1` prints the per-section accounting the writer now
records) - samples/shopping, 2700-byte source, 12847-byte image:

| section                | bytes | share |
|------------------------|-------|-------|
| code (Instr records)   | 4741  | 37%   |
| header + string table  | 3020  | 24% (SOURCE 2699 - gone in v2) |
| builtin_calls + call_sites | 1642 | 13% |
| mid pools (boxed_ops, literal_objs, catch_types, ...) | 1554 | 12% |
| locs                   |  796  | 6%    |
| consts + ref_slots     |  766  | 6%    |
| descriptors            |  156  | 1%    |
| slot_names (debug)     |   88  | 0.7%  |

RANKED, MEASURED ROI (shopping numbers; the ratios hold across samples):

1. **Compact instruction records - DONE in v3, see below** (4741 ->
   1086 bytes, -77% of the code section).
2. **Delta + narrow `locs` - 796 -> 195 (-75%).** The table is
   pc-ascending and lines/cols are small: pc as a delta, line as a delta,
   col as one byte (with an escape).
3. **Stop storing DERIVED pools - DONE in v4, see below** (947 bytes).
4. **Narrow the Locs inside the caret pools (~10% more).** `ArgLoc` is
   16 bytes (two Locs of two u32s) and every builtin_call / call_site /
   member_key / chain step carries several; the same delta/narrow
   treatment as (2) applies.
5. **Drop `slot_names` unless asked (88 bytes, 0.7%).** Debug-only
   (`-vd` labels); the design already called it the one optional
   section. Cheapest change of the set.

Projected combined (1+2+3+5): 12847 -> **~7.4 KB**; with (4) ~6 KB;
with `--strip-source` on top ~**3.3-4.8 KB** for a 2700-byte source,
which `zstd` then takes to ~1.0-1.3 KB.

THE FRAMING POINT: with the source EMBEDDED (v1's default, because error
carets need the text) an image contains the source PLUS the bytecode, so
it can NEVER be smaller than the source. That framing is what led to the
first change below.

## v2 (2026-07-29): the SOURCE REFERENCE replaces the embedded text

DONE - the 24%-of-the-image string-table entry above is gone. Instead of
the program text an image stores a `MyvSourceRef`: the project ROOT, the
source path RELATIVE to it, the compile-time ABSOLUTE path, and a CRC32 +
byte size of the file (~100 bytes total). At load time `resolve_source`
finds the file and uses it ONLY if the CRC32 still matches, so a caret is
never drawn on text that has changed underneath the image.

Rules (README documents them for users):
- `--source ROOT` looks ONLY under that root (`ROOT/rel`) - an explicit
  root that lacks the file is a mistake worth reporting, not a reason to
  silently fall back to a stale absolute path. Otherwise: the stored
  `abs`, then `root/rel`.
- The ROOT is the compile-time CWD when the source lives under it, else
  the source's own directory - so `rel` is ALWAYS non-empty and an image
  is relocatable. A `rel` full of `..` hops would not survive relocation,
  so one is never built.
- Present + matching -> the error is BYTE-IDENTICAL to the source run's
  (pinned by a byte-compare). Present + CHANGED -> warning, and the file
  is REFUSED (no caret) unless `-f`/`--force`, which uses it and warns
  that carets may be misplaced. ABSENT -> silent (shipping an image alone
  is ordinary); the header still names file/line and the backtrace still
  names each function.
- `--strip-source` now stores NO reference at all, so such an image also
  carries no local paths.

The error header gained the file name (`format_exception`'s new optional
`src_name`): " at FILE, line L, col C". A plain script run passes its
path too, so an image and its source produce the same text; `-e` and the
REPL have no file and are unchanged.

MEASURED (image / source): shopping 12847 -> **10243** (4.76x -> 3.79x),
gcd 4920 -> 3197 (2.72x -> 1.77x), fib 1125 -> 971, primes2 4073 -> 3339,
strloop 727 -> 695. Every image shrank by its source size less ~100
bytes. Items 1-5 above are unaffected and still stand; with the source
gone, the projected combined result for shopping is ~4.8 KB raw.

## v3 (2026-07-29): the COMPACT instruction encoding

DONE - item 1 of the ROI list. The fixed 27-byte record
(`op` + `aop` + `opflags` + `target` + `target2` + `pa` + `pb`, written
field-wise) became

```
op:u8  flags:u16  [the present fields, in order]

  bits 0-2   pa       width code 0..4  (0 / 1 / 2 / 4 / 8 bytes)
  bits 3-5   pb       width code 0..4
  bits 6-7   target   width code 0..3  (0 / 1 / 2 / 4 bytes)
  bits 8-9   target2  width code 0..3
  bit  10    aop      stored (else Op::invalid)
  bit  11    opflags  stored (else 0)
  bits 12-15 RESERVED, must be zero (the reader REFUSES a nonzero, so a
             later version can spend them without a v3 reader silently
             misreading the file)
```

Width code 0 means "the field is at its DEFAULT and is not stored".

CHOSEN FROM A CENSUS, not a guess (a temporary tally over bench/ +
samples/, 3483 instructions): `target` is present in 95% of instructions
and fits in ONE byte in 97% of those (92 need two, none need four);
`target2` is at its default in 48% and one byte in 50% (35 need four);
`pa` default in 32%, one byte in 66% (27 are 8-byte float payloads);
`pb` default in 54%, one byte in 40%. So the dominant instruction is
`op + flags + 1-3 payload bytes` - about 5-6 bytes instead of 27.

Two design notes worth keeping:
- **Self-describing, NOT table-driven.** An obvious alternative was a
  per-opcode table of "which fields does this op use", which would save
  the flags word entirely. Rejected: this codebase has a history of
  per-opcode tables silently going stale when an op is added
  (`visit_use_def`, `op_writes_scalar`, `visit_pc_fields`), and here a
  stale entry would mean SILENT DATA LOSS in a stored image. The flags
  word costs ~2 bytes per instruction and cannot drift.
- **The default-vs-value subtlety.** `pa == -1` is both "unset slot" and
  the literal -1. That is fine and NOT ambiguous on disk: the field is
  simply not stored, and the reader's default restores exactly -1;
  `opflags` independently records whether the operand is a literal. The
  round-trip test pins it (a `-1` literal, wide ints of every width, and
  8-byte float payloads).
- Still NOT in-format compression: no dictionary, no entropy coding, no
  bit-level packing across fields. Every field is a plain little-endian
  integer at a byte boundary, decoded in O(1) with no state, and two
  compiles stay byte-identical. The decision record above stands.

MEASURED: shopping's code section 4741 -> **1086** bytes, the whole image
**10243 -> 6588** (-36%). Per sample (image/source): gcd 3197 -> 2067
(**1.14x** the source), shopping 6588 (2.44x), primes2 2059 (2.49x),
rand_sort 684, fib 655, strloop 506. Cumulative from v1: shopping
12847 -> 6588 (-49%), gcd 4920 -> 2067 (-58%).

## v4 (2026-07-29): the DERIVED `boxed_ops` pool is not stored

DONE - item 3 of the ROI list, and the cheapest of the set: pure
deletion. `boxed_ops` (the JIT-bakeable operand data for BinOpV / CmpV /
CompoundV / LogV / UnaryV + a compound global/capture store) is a PURE
FUNCTION of the final code plus the loc side table, so the image stores
none of its 49-byte entries and `read_chunk` calls **`build_boxed_ops`**
- exported from codegen.h for exactly this - once a chunk is read (last,
since it needs `locs`). Same shape as `catch_uids`, which was already
rebuilt from `catch_types` at load.

THE POINT IS SINGLE-SOURCE-OF-TRUTH, not only bytes: the loader calls the
function CODEGEN uses. A hand-written reader would be a second
implementation free to drift, and a drifted pool would feed the JIT wrong
operands or a wrong caret.

Fell out of it: the `Operand` read/write codec in serialize.cpp is GONE -
the derived pool was its only user (an `Instr`'s own operands ride the v3
compact encoding, not that fat 14-byte form). `-Werror=unused-function`
pointed this out, which is the warnings-as-errors rule paying for itself.

VERIFICATION worth noting, because the obvious oracle is INSUFFICIENT:
`-vd` prints only each entry's `target` + `aop`, so byte-identical dumps
do NOT prove the rebuilt operands or carets. The round-trip test therefore
compares the pools FIELD-FOR-FIELD (target, aop, both Operands - the live
union member per `lit_kind` - and both Locs) across the root chunk and
every function chunk, and it counts the entries so that an edit to the
test program cannot make the comparison vacuously pass on two empty
pools (the program yields 8). Separately confirmed end to end: a `dyn`
div-by-zero inside a JIT'd loop - whose caret the JIT stamps STRAIGHT
from `boxed_ops[i].start/end` - renders byte-identically from an image
and from source.

MEASURED: shopping 6588 -> **5641** (-14%), gcd 2067 -> **1957** = 1.08x
its source, strloop 506 -> 404, phonebook 8863 -> 8284. Cumulative from
v1: shopping 12847 -> 5641 (**-56%**), gcd 4920 -> 1957 (-60%).
