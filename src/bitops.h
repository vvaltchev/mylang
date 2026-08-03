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

/*
 * INT_MIN / -1 (and % -1) overflows: C++ leaves it undefined (-fwrapv
 * covers +,-,* only) and the x86 idiv raises a hardware #DE, which used
 * to SIGFPE every engine INCLUDING the parse-time const-fold (task
 * #103). MyLang THROWS, like division by zero (maintainer ruling) - and
 * NEVER via a signal handler: every emitted division already carries
 * explicit 0/-1 pre-checks that decline to the C++ helpers, so this one
 * check in the helpers covers the JIT too. Callers keep their own zero
 * checks (a different exception); the optional Locs serve the typed
 * paths' operand-precise carets (#76) - the boxed paths throw loc-less
 * and their callers stamp, exactly like DivisionByZeroEx.
 */
static constexpr int_type INT_TYPE_MIN =
    static_cast<int_type>(static_cast<uintptr_t>(1)
                          << (INT_TYPE_BITS - 1));

inline void check_int_div_overflow(int_type a, int_type b,
                                   Loc start = Loc(), Loc end = Loc())
{
    if (b == -1 && a == INT_TYPE_MIN)
        throw InvalidValueEx("integer overflow in division", start, end);
}

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
