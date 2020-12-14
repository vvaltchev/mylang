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

public:
    typedef vector<LValueType> inner_type;

private:

    SharedVal<inner_type> shval;
    unsigned off = 0;   /* NOTE: cannot be const because we're using this in a union */
    unsigned len = 0;   /* NOTE: cannot be const because we're using this in a union */
    bool slice = false;

public:
    SharedArrayTemplate() = default;
    SharedArrayTemplate(const inner_type &arr) = delete;
    SharedArrayTemplate(inner_type &&arr);

    inner_type &get_ref() { return shval.get(); }
    const inner_type &get_ref() const { return shval.get(); }
    SharedVal<inner_type> &get_shval() { return shval; }
    const SharedVal<inner_type> &get_shval() const { return shval; }
    long use_count() const { return shval.use_count(); }

    void set_slice(unsigned off_val, unsigned len_val) {
        off = off_val;
        len = len_val;
        slice = true;
    }

    bool is_slice() const { return slice; }
    unsigned offset() const { return slice ? off : 0; }
    unsigned size() const { return slice ? len : get_ref().size(); }
};
