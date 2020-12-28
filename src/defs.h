/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#define _USE_MATH_DEFINES
#include <iostream>
#include <memory>
#include <utility>

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
typedef long int int_type;

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
typedef unsigned int size_type;
