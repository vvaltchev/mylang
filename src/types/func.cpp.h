/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "eval.h"
#include "evaltypes.cpp.h"
#include "syntax.h"

class TypeFunc : public SharedType<FlatSharedFuncObj> {

public:

    TypeFunc() : SharedType<FlatSharedFuncObj>(Type::t_func) { }

    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;

    bool is_true(const EvalValue &a) override { return true; }
    int_type use_count(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;

    string to_string(const EvalValue &a) override {
        return "<function>";
    }
};

void TypeFunc::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedFuncObj>()) {
        a = false;
        return;
    }

    FuncObject &obj = a.get<FlatSharedFuncObj>().get();
    a = &obj == &b.get<FlatSharedFuncObj>().get();
}

void TypeFunc::noteq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedFuncObj>()) {
        a = true;
        return;
    }

    FuncObject &obj = a.get<FlatSharedFuncObj>().get();
    a = &obj != &b.get<FlatSharedFuncObj>().get();
}

int_type TypeFunc::use_count(const EvalValue &a)
{
    return a.get<FlatSharedFuncObj>().use_count();
}

EvalValue TypeFunc::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(&a.get<FlatSharedFuncObj>().get());
}

EvalValue TypeFunc::clone(const EvalValue &a)
{
    const FlatSharedFuncObj &wrapper = a.get<FlatSharedFuncObj>();
    const FuncObject &func = wrapper.get();

    if (func.capture_ctx.empty())
        return a;

    return FlatSharedFuncObj(make_shared<FuncObject>(func));
}

FuncObject::FuncObject(const FuncObject &rhs)
    : func(rhs.func)
    , capture_ctx(rhs.capture_ctx.parent,
                  rhs.capture_ctx.const_ctx,
                  rhs.capture_ctx.func_ctx)
{
    capture_ctx.copy_symbols_from(rhs.capture_ctx);
}

FuncObject::FuncObject(const FuncDeclStmt *func, EvalContext *ctx)
    : func(func)
    , capture_ctx(get_root_ctx(ctx), false, true)
{
    if (!func->captures)
        return;

    for (const auto &capture : func->captures->elems) {
        capture_ctx.emplace(
            capture.get(),
            RValue(capture->eval(ctx)),
            ctx->const_ctx
        );
    }
}
