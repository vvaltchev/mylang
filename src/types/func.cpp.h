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

class TypeFunc : public TypeImpl<shared_ptr<FuncObject>> {

public:

    TypeFunc() : TypeImpl<shared_ptr<FuncObject>>(Type::t_func) { }

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
    if (!b.is<shared_ptr<FuncObject>>()) {
        a = false;
        return;
    }

    FuncObject *objp = a.get<shared_ptr<FuncObject>>().get();
    a = (objp == b.get<shared_ptr<FuncObject>>().get());
}

void TypeFunc::noteq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<shared_ptr<FuncObject>>()) {
        a = true;
        return;
    }

    FuncObject *objp = a.get<shared_ptr<FuncObject>>().get();
    a = (objp != b.get<shared_ptr<FuncObject>>().get());
}

int_type TypeFunc::use_count(const EvalValue &a)
{
    return a.get<shared_ptr<FuncObject>>().use_count();
}

EvalValue TypeFunc::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(a.get<shared_ptr<FuncObject>>().get());
}

EvalValue TypeFunc::clone(const EvalValue &a)
{
    const FuncObject &func = *a.get<shared_ptr<FuncObject>>().get();

    /* A non-capturing function has no per-instance state, so clone()ing it can
     * share the same object; a capturing one is deep-copied so each clone owns
     * independent captured state. */
    if (func.capture_slots.empty())
        return a;

    return shared_ptr<FuncObject>(make_shared<FuncObject>(func));
}

FuncObject::FuncObject(const FuncObject &rhs)
    : func(rhs.func)
    , capture_slots(rhs.capture_slots)
    , capture_ctx(rhs.capture_ctx.parent,
                  rhs.capture_ctx.const_ctx,
                  rhs.capture_ctx.func_ctx)
{
}

FuncObject::FuncObject(const FuncDeclStmt *func, EvalContext *ctx)
    : func(func)
    , capture_ctx(get_root_ctx(ctx), false, true)
{
    if (!func->captures)
        return;

    /* Snapshot each captured outer variable into a capture slot, in declaration
     * order (the resolver assigns SymKind::capture indices in the same order).
     * The value is a copy (RValue), mutable unless made in a const context. */
    capture_slots.reserve(func->captures->elems.size());

    for (const auto &capture : func->captures->elems) {
        capture_slots.emplace_back(
            RValue(capture->eval(ctx)),
            ctx->const_ctx
        );
    }
}
