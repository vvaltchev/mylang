/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "bytecode.h"

class Block;

/*
 * Lower a Block's statements to a bytecode Chunk. `slot_count` is the frame's
 * resolved-local count (register-machine temps grow above it, at
 * [slot_count, slot_count + chunk.n_temps)). Used for both the root/main block
 * and a function body (Phase 4). See plans/bytecode-vm.md.
 */
Chunk codegen_chunk(const Block *block, int slot_count);

/* The root/main wrapper: codegen_chunk(root, root->slot_count). */
Chunk codegen_program(const Block *root);
