/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <string_view>

using namespace std;

struct InvalidTokenEx { string_view val; };
struct SyntaxErrorEx { };
struct DivisionByZeroEx { };
struct TypeErrorEx { };
struct UndefinedVariableEx { string var; };
