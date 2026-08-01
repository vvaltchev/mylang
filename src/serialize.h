/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct VmProgram;

/*
 * The `.myv` STORED-BYTECODE format (plans/myv-serializer.md) - the endgame
 * artifact of the zero-AST campaign: `mylang -c file.my` writes a binary the
 * interpreter runs with NO source, NO parse, NO optimizer passes.
 *
 * ONLY the bytecode VM image is stored - never native code: the AOT tier
 * re-runs at LOAD time (jit_compile_chunk over the loaded chunks), so a
 * `.myv` is portable across machines and the JIT's own evolution. Serializing
 * fragments would need a relocating linker (library + own-text addresses
 * differ per run) - a deliberate later increment.
 *
 * The write and read of each record live ADJACENT in serialize.cpp so the
 * pair cannot drift; ANY format change must bump MYV_FORMAT_VERSION in the
 * same commit.
 */
constexpr unsigned MYV_FORMAT_VERSION = 5;

/*
 * The stored SOURCE REFERENCE (v2). An image does NOT embed the source text -
 * it records WHERE the source was, plus a CRC32 + byte size that PROVE the
 * file found there is still the one that was compiled. So error carets cost
 * ~a hundred bytes instead of the whole program text, and an image whose
 * source is absent (shipped alone) still reports file/line/backtrace.
 *
 * `rel` is always non-empty when a reference exists, which is what makes an
 * image RELOCATABLE via `--source ROOT`: the root is the compile-time CWD
 * when the source lives under it, else the source's own directory.
 */
struct MyvSourceRef {
    std::string root;    /* the project root, absolute */
    std::string rel;     /* the source path relative to `root` */
    std::string abs;     /* the source's absolute path when compiled */
    uint32_t crc = 0;    /* CRC32 of the source bytes */
    uint64_t size = 0;   /* the source's byte size (a free pre-check) */
};

/* Build the reference for `path` (reads the file to CRC it). A missing /
 * unreadable file yields an EMPTY reference (`abs` empty) - the caller has
 * already read the source, so this cannot normally fail. */
MyvSourceRef myv_source_ref(const std::string &path);

/* What the loader resolved for error rendering. */
struct MyvSource {
    std::vector<std::string> lines;  /* the TEXT; empty => plain errors */
    std::string name;                /* the path to NAME in errors ("" none) */
    std::string warning;             /* non-empty => print it to stderr */
};

/* How to look for the source of an image being loaded. */
struct MyvLoadOpts {
    std::string root;    /* --source ROOT: resolve `rel` under THIS root
                          * instead of the stored root / absolute path */
    bool force = false;  /* -f/--force: use the file even when the CRC32
                          * says it changed (the warning is printed anyway) */
};

/* Write `prog` to `path`. An EMPTY `src` (no `abs`) stores no reference at
 * all - the --strip-source mode, which also keeps local paths out of the
 * image. Throws a plain Exception on an I/O error or an unserializable
 * value (loud, never silent). */
void myv_write(const VmProgram &prog, const std::string &path,
               const MyvSourceRef &src);

/* True iff `path` starts with the MYV magic - the loader is chosen by
 * CONTENT, not by extension. */
bool myv_is_image(const std::string &path);

/* Load `path` into a fresh VmProgram (the caller owns it and must keep it
 * alive for the whole run - exception payloads/backtraces reference its
 * descriptors and struct defs). `out_src` gets the source the reference
 * resolved to (see MyvSource / MyvLoadOpts). Throws a plain Exception
 * ("corrupt or incompatible .myv") on any structural problem. */
VmProgram myv_read(const std::string &path, MyvSource &out_src,
                   const MyvLoadOpts &opts = MyvLoadOpts());
