/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "evalvalue.h"
#include <map>

using namespace std;

class EvalContext {

public:

    typedef map<string, LValue, less<>> SymbolsType;
    EvalContext *const parent;
    const bool const_ctx;
    const bool func_ctx;
    SymbolsType symbols;

    EvalContext(const EvalContext &rhs) = delete;
    EvalContext(EvalContext &&rhs) = delete;

    EvalContext(EvalContext *parent = nullptr,
                bool const_ctx = false,
                bool func_ctx = false);

    static const SymbolsType builtins;
    static const SymbolsType const_builtins;
};

inline EvalContext *
get_root_ctx(EvalContext *ctx)
{
    while (ctx->parent)
        ctx = ctx->parent;

    return ctx;
}

class FuncObject {

public:

    const FuncDeclStmt *const func;
    EvalContext capture_ctx;

    FuncObject(const FuncDeclStmt *func, EvalContext *ctx);
    FuncObject(const FuncObject &rhs);
};
