/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "evaltypes.cpp.h"

class TypeException : public TypeImpl<shared_ptr<ExceptionObject>> {

public:

    TypeException() : TypeImpl<shared_ptr<ExceptionObject>>(Type::t_ex) { }

    string to_string(const EvalValue &a) override {

        const ExceptionObject &ex = *a.get<shared_ptr<ExceptionObject>>().get();
        string res = "<Exception(";
        res += ex.get_name();
        res += ")>";
        return res;
    }
};
