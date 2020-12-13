/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "sharedval.h"
#include <vector>

using namespace std;

/*
 * This class is a template simply because otherwise this header wouldn't be
 * able to compile if included independently (it requires LValue, which requires
 * EvalValue which requires SharedArray). In this case, it wouldn't be a big
 * deal, but in general it's an anti-pattern to have headers requiring a specific
 * include order.
 */

template <class LValueType>
class SharedArrayTemplate {

    friend class TypeArr;

public:

    typedef vector<LValueType> inner_type;

private:

    /* See SharedStr */
    inner_type &get_ref() const {
        return const_cast<SharedArrayTemplate *>(this)->vec.get();
    }

    /* See SharedStr */
    long use_count() const {
        return const_cast<SharedArrayTemplate *>(this)->vec.use_count();
    }

public:

    SharedVal<inner_type> vec;
    unsigned off;   /* NOTE: cannot be const because we're using this in a union */
    unsigned len;   /* NOTE: cannot be const because we're using this in a union */

    SharedArrayTemplate() = default;
    SharedArrayTemplate(const inner_type &arr) = delete;
    SharedArrayTemplate(inner_type &&arr);

    unsigned size() const {
        return const_cast<SharedArrayTemplate *>(this)->len;
    }
};
