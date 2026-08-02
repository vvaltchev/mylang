/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

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
constexpr unsigned MYV_FORMAT_VERSION = 1;

/* Write `prog` to `path`. `src` is embedded (error carets need the TEXT;
 * empty = the strip-source mode). Throws a plain Exception on an I/O error
 * or an unserializable value (loud, never silent). */
void myv_write(const VmProgram &prog, const std::string &path,
               const std::string &src);

/* True iff `path` starts with the MYV magic - the loader is chosen by
 * CONTENT, not by extension. */
bool myv_is_image(const std::string &path);

/* Load `path` into a fresh VmProgram (the caller owns it and must keep it
 * alive for the whole run - exception payloads/backtraces reference its
 * descriptors and struct defs). The embedded source, if any, lands in
 * `out_src` for error rendering. Throws a plain Exception ("corrupt or
 * incompatible .myv") on any structural problem. */
VmProgram myv_read(const std::string &path, std::vector<std::string> &out_src);
