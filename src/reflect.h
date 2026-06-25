/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>

/*
 * The shared value-/type-rendering helpers behind the runtime reflection
 * builtins (builtins/reflect.cpp.h, where they are defined) - exposed here so
 * the REPL's :globals / :type inspection commands reuse the exact same strings.
 * Definitions live in reflect.cpp.h, which is #included once into types.cpp.
 */

class EvalValue;
class FuncDeclStmt;
struct StructTypeDef;

/* The runtime STRUCTURAL type of a value ("array<int>", "dict<K,V>", a struct
 * name, a function signature, ...) - richer than the bare type() kind. */
std::string reflect_typeof(const EvalValue &e);

/* A function's declared signature ("pure func f(int a, opt b)"). */
std::string reflect_func_sig(const FuncDeclStmt *f);

/* A struct type's constructor signature ("Point(int x, int y)"). */
std::string reflect_struct_ctor(const StructTypeDef *def);

/* A struct's in-memory layout (POD offsets/size, or boxed slots). */
std::string reflect_layout(const StructTypeDef *def);
