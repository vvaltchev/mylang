/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include "intrusiveptr.h"
#include <unordered_map>
#include "poolalloc.h"

template <class EvalValueT, class LValueT>
class DictObjectTempl : public RefCounted {

public:

    ML_POOL_NEW_DELETE
    /* H2 v2: the NODE-POOLED map (poolalloc.h) - a chained unordered_map
     * heap-allocated a ~96-byte node per insert; the pool serves those
     * from program-lifetime free lists. Node-pointer stability (what the
     * held-LValue* runtime paths rely on) is untouched - rehash moves
     * only the bucket array. */
    typedef std::unordered_map<
        EvalValueT, LValueT,
        std::hash<EvalValueT>, std::equal_to<EvalValueT>,
        PoolAlloc<std::pair<const EvalValueT, LValueT>>> inner_type;

private:
    inner_type data;

    /*
     * When set, this dict is read-only: it backs a `const` value, so
     * subscript/member writes (and auto-vivification) and erase are rejected.
     * A clone() is always mutable, so TypeDict::clone clears it on the copy.
     * See make_const_clone() in eval.cpp.
     */
    bool readonly = false;

    /*
     * Default value for a missing key (a "default dict", from
     * dict(default_value)). When `has_default` is set, reading a missing key
     * returns (and inserts) `default_val` instead of throwing - so `d[k] += 1`
     * works without a none-check. Copied by the default copy/move ctors.
     */
    bool has_default = false;
    EvalValueT default_val;

public:

    DictObjectTempl() = default;
    DictObjectTempl(const DictObjectTempl &) = default;
    DictObjectTempl(DictObjectTempl &&) = default;

    DictObjectTempl(const inner_type &) = delete;
    DictObjectTempl(inner_type &&d)
        : data(std::move(d))
    { }

    inner_type &get_ref() { return data; }
    const inner_type &get_ref() const { return data; }

    bool is_readonly() const { return readonly; }
    void set_readonly() { readonly = true; }
    void clear_readonly() { readonly = false; }

    bool get_has_default() const { return has_default; }
    const EvalValueT &get_default() const { return default_val; }
    void set_default(const EvalValueT &v)
        { has_default = true; default_val = v; }
};
