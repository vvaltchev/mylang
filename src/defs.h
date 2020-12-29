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
typedef long double float_type; /* Note: replace %Lf if you change this! */
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