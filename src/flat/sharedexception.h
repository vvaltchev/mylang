/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include "errors.h"
#include <string>

template <class EvalValueT>
class ExceptionObjectTempl : public RuntimeException {

    std::string dyn_name;
    EvalValueT data;

public:

    ExceptionObjectTempl(const std::string &name,
                         const EvalValueT &data = EvalValueT())
        : RuntimeException("DynamicExceptionEx", nullptr)
        , dyn_name(name)
        , data(data)
    { }

    std::string_view get_name() const {
        return dyn_name;
    };

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
