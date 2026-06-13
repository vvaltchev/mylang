/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include <unordered_map>

template <class EvalValueT, class LValueT>
class DictObjectTempl {

public:
    typedef std::unordered_map<EvalValueT, LValueT> inner_type;

private:
    inner_type data;

    /*
     * When set, this dict is read-only: it backs a `const` value, so
     * subscript/member writes (and auto-vivification) and erase are rejected.
     * A clone() is always mutable, so TypeDict::clone clears it on the copy.
     * See make_const_clone() in eval.cpp.
     */
    bool readonly = false;

public:

    DictObjectTempl() = default;
    DictObjectTempl(const DictObjectTempl &) = default;
    DictObjectTempl(DictObjectTempl &&) = default;

    DictObjectTempl(const inner_type &) = delete;
    DictObjectTempl(inner_type &&d)
        : data(move(d))
    { }

    inner_type &get_ref() { return data; }
    const inner_type &get_ref() const { return data; }

    bool is_readonly() const { return readonly; }
    void set_readonly() { readonly = true; }
    void clear_readonly() { readonly = false; }
};
