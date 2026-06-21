/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include <functional>

/*
 * The boolean type: `true` and `false` are its only two values. bool is a
 * trivial scalar (stored in EvalValue's `bval`), distinct from int so the
 * inferencer can tell them apart (a comparison yields bool, `var b = true` is
 * bool). Arithmetic and comparison promote bool to int (bool <= int <= float),
 * so bool never reaches the int/float binary ops here - num_bin_op() promotes a
 * bool operand first. Hence TypeBool only needs truthiness, printing, and a
 * hash that matches int 0/1 (so `true` and `1`, which compare equal, are the
 * same dict key). Logical `&&`/`||`/`!` are handled truthy-style in eval.cpp.
 */
class TypeBool : public Type {

public:

    TypeBool() : Type(Type::t_bool) { }

    bool is_true(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    size_t hash(const EvalValue &a) override;
};

bool TypeBool::is_true(const EvalValue &a)
{
    return a.get<bool>();
}

string TypeBool::to_string(const EvalValue &a)
{
    return a.get<bool>() ? "true" : "false";
}

size_t TypeBool::hash(const EvalValue &a)
{
    /* Hash as int 0/1 so a `true` key collides with a `1` key - they compare
     * equal (bool promotes to int), so they must hash equal. */
    return std::hash<int_type>()(a.get<bool>() ? 1 : 0);
}
