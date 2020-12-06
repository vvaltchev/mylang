/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once
#include "evalvalue.h"
#include "evaltypes.cpp.h"

class TypeStr : public SharedType<SharedStrWrapper> {

public:

    TypeStr() : SharedType<SharedStrWrapper>(Type::t_str) { }

    virtual string to_string(const EvalValue &a) {
        return a.get<SharedStrWrapper>().get();
    }
};
