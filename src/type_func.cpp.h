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

class TypeFunc : public SharedType<FlatSharedFuncObj> {

public:

    TypeFunc() : SharedType<FlatSharedFuncObj>(Type::t_func) { }

    virtual long use_count(const EvalValue &a);
    virtual EvalValue clone(const EvalValue &a);
    virtual EvalValue intptr(const EvalValue &a);

    virtual string to_string(const EvalValue &a) {
        return "<function>";
    }
};

long TypeFunc::use_count(const EvalValue &a)
{
    return a.get<FlatSharedFuncObj>().use_count();
}

EvalValue TypeFunc::intptr(const EvalValue &a)
{
    return reinterpret_cast<long>(&a.get<FlatSharedFuncObj>().get());
}

EvalValue TypeFunc::clone(const EvalValue &a)
{
    const FlatSharedFuncObj &wrapper = a.get<FlatSharedFuncObj>();
    const FuncObject &func = wrapper.get();

    if (!func.capture_ctx.symbols.size())
        return a;

    return FlatSharedFuncObj(make_shared<FuncObject>(func));
}

FuncObject::FuncObject(const FuncObject &rhs)
    : func(rhs.func)
    , capture_ctx(rhs.capture_ctx.parent,
                  rhs.capture_ctx.const_ctx,
                  rhs.capture_ctx.func_ctx)
{
    capture_ctx.symbols = rhs.capture_ctx.symbols;
}

FuncObject::FuncObject(const FuncDeclStmt *func, EvalContext *ctx)
    : func(func)
    , capture_ctx(get_root_ctx(ctx), false, true)
{
    if (!func->captures)
        return;

    for (const auto &capture : func->captures->elems) {
        capture_ctx.symbols.emplace(
            capture->value,             // value means "name" here
            LValue(RValue(capture->eval(ctx)), ctx->const_ctx)
        );
    }
}
