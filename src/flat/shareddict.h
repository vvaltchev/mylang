/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include "flatval.h"
#include <vector>
#include <unordered_map>
#include <cassert>

using namespace std;

template <class EvalValueT, class LValueT>
class DictObjectTempl {

public:
    typedef unordered_map<EvalValueT, LValueT> inner_type;

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
