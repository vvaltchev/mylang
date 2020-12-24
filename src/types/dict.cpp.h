/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * NOTE: this is NOT a header file. This is C++ file in the form
 * of a header file, just because it's faster to compile it this
 * way instead.
 */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "eval.h"
#include "evaltypes.cpp.h"

class TypeDict : public SharedType<FlatSharedDictObj> {

public:

    TypeDict() : SharedType<FlatSharedDictObj>(Type::t_dict) { }

    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    EvalValue subscript(const EvalValue &what_lval, const EvalValue &idx_val) override;

    int_type len(const EvalValue &a) override;
    int_type use_count(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;
    string to_string(const EvalValue &a) override;
    bool is_true(const EvalValue &a) override;
};

int_type TypeDict::len(const EvalValue &a)
{
    return a.get<FlatSharedDictObj>().get().get_ref().size();
}

void TypeDict::eq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedDictObj>()) {
        a = false;
        return;
    }

    const DictObject::inner_type &dataA
        = a.get<FlatSharedDictObj>()->get_ref();

    const DictObject::inner_type &dataB
        = b.get<FlatSharedDictObj>()->get_ref();

    a = dataA == dataB;
}

void TypeDict::noteq(EvalValue &a, const EvalValue &b)
{
    if (!b.is<FlatSharedDictObj>()) {
        a = false;
        return;
    }

    const DictObject::inner_type &dataA
        = a.get<FlatSharedDictObj>()->get_ref();

    const DictObject::inner_type &dataB
        = b.get<FlatSharedDictObj>()->get_ref();

    a = dataA != dataB;
}

EvalValue TypeDict::subscript(const EvalValue &what_lval, const EvalValue &key)
{
    const EvalValue &what = RValue(what_lval);
    FlatSharedDictObj &&flatObj = what.get<FlatSharedDictObj>();
    DictObject::inner_type &data = flatObj.get().get_ref();

    const auto &it = data.find(key);

    if (it != data.end())
        return &it->second;

    return &(
        *data.emplace(
            key, LValue(EvalValue(), false)
        ).first
    ).second;
}

string TypeDict::to_string(const EvalValue &a)
{
    const DictObject &obj = a.get<FlatSharedDictObj>().get();
    const DictObject::inner_type &data = obj.get_ref();

    string res;
    size_type i = 0;

    res.reserve(48 * data.size());
    res += "{";

    for (const auto &p: data) {

        res += p.first.to_string();
        res += ": ";
        res += p.second.get().to_string();

        if (i != data.size() - 1)
            res += ", ";

        i++;
    }

    res += "}";
    return res;
}

bool TypeDict::is_true(const EvalValue &a)
{
    return a.get<FlatSharedDictObj>()->get_ref().size() > 0;
}

int_type TypeDict::use_count(const EvalValue &a)
{
    return a.get<FlatSharedFuncObj>().use_count();
}

EvalValue TypeDict::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(
        &a.get<FlatSharedDictObj>().get().get_ref()
    );
}

EvalValue TypeDict::clone(const EvalValue &a)
{
    const FlatSharedDictObj &wrapper = a.get<FlatSharedDictObj>();
    const DictObject &dict = wrapper.get();
    return FlatSharedDictObj(make_shared<DictObject>(dict));
}
