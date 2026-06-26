/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"
#include "errors.h"

/*
 * Shared integer shift helpers, so the boxed TypeInt path (int.cpp.h) and the
 * unboxed M8 path (TypedScalarExpr::eval_int) compute identically. `<<`/`>>>`
 * shift through the unsigned representation to avoid signed-shift UB; `>>` is a
 * SIGNED (arithmetic, sign-extending) right shift, `>>>` the UNSIGNED
 * (zero-filling) one (JavaScript semantics). The shift count must be >= 0; a
 * count past the int width yields 0 for `<<`/`>>>` and a full sign-fill for
 * `>>`, instead of C's undefined behavior.
 */

static constexpr int_type INT_TYPE_BITS =
    static_cast<int_type>(8 * sizeof(int_type));

inline int_type bit_shl(int_type v, int_type n)
{
    if (n < 0)
        throw InvalidValueEx("negative shift count");
    return (n >= INT_TYPE_BITS)
               ? 0
               : static_cast<int_type>(static_cast<uintptr_t>(v) << n);
}

inline int_type bit_shr(int_type v, int_type n)   /* signed / arithmetic */
{
    if (n < 0)
        throw InvalidValueEx("negative shift count");
    return (n >= INT_TYPE_BITS) ? (v < 0 ? -1 : 0) : (v >> n);
}

inline int_type bit_ushr(int_type v, int_type n)  /* unsigned / logical */
{
    if (n < 0)
        throw InvalidValueEx("negative shift count");
    return (n >= INT_TYPE_BITS)
               ? 0
               : static_cast<int_type>(static_cast<uintptr_t>(v) >> n);
}
