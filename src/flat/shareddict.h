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
};
