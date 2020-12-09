/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "eval.h"
#include "evaltypes.cpp.h"
#include "syntax.h"

class TypeFunc : public SharedType<SharedFuncObjWrapper> {

public:

    TypeFunc() : SharedType<SharedFuncObjWrapper>(Type::t_func) { }

    virtual string to_string(const EvalValue &a) {
        return "<function>";
    }
};

FuncObject::FuncObject(const FuncDeclStmt *func, EvalContext *ctx)
    : func(func)
    , capture_ctx(get_root_ctx(ctx), false, true)
{
    if (!func->captures)
        return;

    for (const auto &capture : func->captures->elems) {
        capture_ctx.symbols.emplace(
            capture->value,             // value means "name" here
            RValue(capture->eval(ctx))
        );
    }
}
