/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#define _USE_MATH_DEFINES
#include <iostream>
#include <memory>
#include <utility>
#include <cstdint>

/*
 * The following types and funcs are by far too common to be used with the
 * std:: prefix. Therefore, we're selectively enabling them in our global
 * namespace.
 */
using std::cin;
using std::cout;
using std::cerr;
using std::ostream;
using std::endl;

using std::move;
using std::forward;
using std::unique_ptr;
using std::shared_ptr;
using std::make_pair;
using std::make_shared;
using std::make_unique;

/* Custom type defs */
/*
 * The script float type. `double` (not `long double`): it keeps EvalValue small
 * (long double's 16-byte alignment padded EvalValue from 40 to 48 bytes, which
 * dominated array memory traffic and every value copy), matches Python's float
 * exactly, and uses the much faster double libm. If you change this, update the
 * printf/snprintf format strings (currently "%f"/"%.*f") and the math builtins.
 */
typedef double float_type;
typedef intptr_t int_type;

#ifndef _MSC_VER

   /*
    * Using 32-bit unsigned as size/offset type is slightly more efficient
    * than using size_t on 64-bit machines because:
    *
    *      1. Makes the structures smaller (4 bytes vs. 8 bytes)
    *      2. On x86_64, it doesn't require the extra REX.W prefix
    *
    * Overall, we don't really need to handle arrays or strings larger than 4 GB
    * in this simple language. Better take advantage of that.
    */
    typedef uint32_t size_type;

#else

    /*
     * Microsoft's compiler complains too much about integer truncation.
     * While the "problem" is exactly the same with GCC and Clang, and the
     * trade-off is just to support at most 2^32 elements but having smaller
     * EvalValue objects (=> faster to copy), compiling with this compiler
     * WITHOUT warnings, requires a *ton* of extra casts. Therefore, just 
     * use size_t and make the compiler happy.
     */
    typedef size_t size_type;
#endif

/*
 * ML_CHECK / ML_CHECK_MSG - the project's defense-in-depth assertion macros.
 *
 * Use these (NOT bare assert) to state an invariant the code RELIES ON but a
 * caller could violate with an incorrect change: "this can't happen if the
 * code is correct." They are the Swiss-cheese layers - each one is a hole in a
 * different place, so a bad change has many chances to hit a wall. They must be
 * SIDE-EFFECT FREE (the condition is compiled out in a plain release build).
 *
 * Tied to the build's ASSERTS flag, exactly like the C `assert()`: active
 * unless NDEBUG is defined. ASSERTS defaults ON for EVERY build type (debug and
 * release alike); a build with ASSERTS=0 defines NDEBUG, which compiles both
 * the C asserts and these away (the way to measure the assert overhead, e.g.
 * `make OPT=1 ASSERTS=0`). So all CI runs exercise the full net.
 *
 * For conditions that CAN legitimately occur at runtime (bad user input, I/O
 * failure, a real type error) do NOT use these - throw a proper Exception so
 * the error is handled in every build. ML_CHECK is for "impossible" states.
 */
#ifndef NDEBUG
#  include <cassert>
#  define ML_CHECK(cond)          assert(cond)
#  define ML_CHECK_MSG(cond, msg) assert((cond) && (msg))
#else
#  define ML_CHECK(cond)          ((void)0)
#  define ML_CHECK_MSG(cond, msg) ((void)0)
#endif