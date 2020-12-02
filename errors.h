/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <string_view>

using namespace std;

struct InvalidTokenEx { string_view val; };
struct DivisionByZeroEx { };
struct TypeErrorEx { };
struct UndefinedVariableEx { string_view name; };
struct NotLValueEx { };
struct InternalErrorEx { };

struct SyntaxErrorEx {

    const char *msg;

    SyntaxErrorEx(const char *msg) : msg(msg) { }
};
