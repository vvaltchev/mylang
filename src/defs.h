/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

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

typedef long double float_type;
typedef long int int_type;
