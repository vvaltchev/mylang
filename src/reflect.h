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
struct FieldDef;
struct FuncDescriptor;
struct StructTypeDef;
struct TypeAnnot;
enum class DeclType : unsigned char;

/* The runtime STRUCTURAL type of a value ("array<int>", "dict<K,V>", a struct
 * name, a function signature, ...) - richer than the bare type() kind. */
std::string reflect_typeof(const EvalValue &e);

/* A function's declared signature ("pure func f(int a, opt b)"). */
std::string reflect_func_sig(const FuncDescriptor *f);

/* A struct type's constructor signature ("Point(int x, int y)"). */
std::string reflect_struct_ctor(const StructTypeDef *def);

/* A struct field's declared type, as its declaration writes it ("int",
 * "opt dict", "array<int>?", "P") - also what the decompiler renders a
 * struct declaration's fields with. */
std::string reflect_field_type(const FieldDef &f);

/* A declaration's ANNOTATION - a variable's or a parameter's - in the
 * form reflect_field_type gives a field: a parameterized container or a
 * box with its element types and a `?` suffix when it is `opt`
 * ("array<int>?", "box<P>"), a struct by its name and a keyword with an
 * `opt ` prefix ("opt P", "opt int"). Empty for DeclType::none. */
std::string reflect_decl_annot(DeclType dt, const StructTypeDef *strct,
                               const TypeAnnot *annot, bool opt);

/* A struct's in-memory layout (POD offsets/size, or boxed slots). */
std::string reflect_layout(const StructTypeDef *def);
