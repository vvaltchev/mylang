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

class TypeFunc : public TypeImpl<intrusive_ptr<FuncObject>> {

public:

    TypeFunc() : TypeImpl<intrusive_ptr<FuncObject>>(Type::t_func) { }

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
    if (!b.is<intrusive_ptr<FuncObject>>()) {
        a = false;
        return;
    }

    FuncObject *objp = a.get<intrusive_ptr<FuncObject>>().get();
    a = (objp == b.get<intrusive_ptr<FuncObject>>().get());
}

void TypeFunc::noteq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<intrusive_ptr<FuncObject>>()) {
        a = true;
        return;
    }

    FuncObject *objp = a.get<intrusive_ptr<FuncObject>>().get();
    a = (objp != b.get<intrusive_ptr<FuncObject>>().get());
}

int_type TypeFunc::use_count(const EvalValue &a)
{
    return a.get<intrusive_ptr<FuncObject>>().use_count();
}

EvalValue TypeFunc::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(a.get<intrusive_ptr<FuncObject>>().get());
}

EvalValue TypeFunc::clone(const EvalValue &a)
{
    const FuncObject &func = *a.get<intrusive_ptr<FuncObject>>().get();

    /* A non-capturing function has no per-instance state, so clone()ing it can
     * share the same object; a capturing one is deep-copied so each clone owns
     * independent captured state. */
    if (func.capture_slots.empty())
        return a;

    return intrusive_ptr<FuncObject>(make_intrusive<FuncObject>(func));
}

FuncObject::FuncObject(const FuncObject &rhs)
    : RefCounted()   /* a clone owns a FRESH count - do NOT copy rhs's */
    , func(rhs.func)
    , capture_slots(rhs.capture_slots)
    , capture_root(rhs.capture_root)
{
}

FuncObject::FuncObject(const FuncDescriptor *func, EvalContext *ctx)
    : func(func)
    , capture_root(get_root_ctx(ctx))
{
    if (func->captures.empty())
        return;

    /* Snapshot each captured outer variable into a capture slot, in declaration
     * order (the resolver assigns SymKind::capture indices in the same order).
     * The value is a copy (RValue), mutable unless made in a const context.
     * The source is the descriptor's RESOLVED kind/slot (read_sym) - no
     * capture Identifier is evaluated, so closure creation is AST-free. */
    capture_slots.reserve(func->captures.size());

    for (const auto &cap : func->captures) {
        capture_slots.emplace_back(
            RValue(read_sym(ctx, cap.kind, cap.slot, cap.name)),
            ctx->const_ctx
        );
    }
}
