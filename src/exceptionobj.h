/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "uniqueid.h"
#include "poolalloc.h"
#include "flatval.h"
#include "errors.h"
#include "intrusiveptr.h"   /* RefCounted: held in the value model as t_ex */
#include <string>

/* A user/runtime exception object is BOTH thrown (as a RuntimeException) and
 * held in the value model (t_ex) - the latter via a non-atomic intrusive_ptr,
 * so it also inherits RefCounted. The refcount is irrelevant while thrown. */
template <class EvalValueT>
class ExceptionObjectTempl : public RuntimeException, public RefCounted {

    std::string dyn_name;
    const UniqueId *dyn_uid;     /* interned dyn_name (#74 inc 3) */
    EvalValueT data;

public:

    ML_POOL_NEW_DELETE

    /* `uid`: the already-interned type name when the caller has it (a
     * thrown struct's def->name - the hot path); else the ctor interns
     * (the cold built-in wrapper path). Always non-null after. */
    ExceptionObjectTempl(const std::string &name,
                         const EvalValueT &data = EvalValueT(),
                         const UniqueId *uid = nullptr)
        : RuntimeException("DynamicExceptionEx", nullptr)
        , dyn_name(name)
        , dyn_uid(uid ? uid : UniqueId::get(name))
        , data(data)
    { }

    std::string_view get_name() const {
        return dyn_name;
    };

    std::string_view match_name() const override {
        return dyn_name;
    }

    bool is_exception_object() const override {
        return true;
    }

    const UniqueId *match_uid() const override {
        return dyn_uid;
    }

    const EvalValueT &get_data() const {
        return data;
    };

    ExceptionObjectTempl *clone() const override {
        return new ExceptionObjectTempl(*this);
    }

    void rethrow() const override {
        throw *this;
    }
};
