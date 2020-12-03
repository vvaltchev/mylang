/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "evalvalue.h"
#include <variant>
#include <map>

class LValue {

    variant<long> value;

public:

    template <class T>
    LValue(T &&arg) : value(forward<T>(arg)) { }

    template <class T>
    T get() const {
        return std::get<T>(value);
    }

    void put(EvalValue v) {

        if (v.is<long>())
            value = v.get<long>();
        else
            throw TypeErrorEx();
    }

    EvalValue eval() const {
        return EvalValue(std::get<long>(value));
    }
};

ostream &operator<<(ostream &s, const EvalValue &c);

class EvalContext {

public:

    map<string, LValue, less<>> vars;
};
