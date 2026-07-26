/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * BENCH-FAIR (plans/bench-fairness.md, class D): the FAT DYNAMIC VALUE for
 * the dyn-bench C++ twins. A MyLang `dyn` variable holds a runtime-tagged
 * value with EvalValue's flexibility (none/int/float/bool/str/array/dict),
 * every operation dispatching on the runtime tag - the C++ twin of a dyn
 * bench must carry the same fat value and the same per-op dispatch, not a
 * statically-typed `long`. Arithmetic mirrors vm_num_binop's rules
 * (int/float promotion; anything else aborts - the benches never hit it).
 * Containers are shared (refcounted) like SharedArrayObj/DictObject.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

struct Value;
struct VDictObj;

using VArr = std::shared_ptr<std::vector<Value>>;
using VDict = std::shared_ptr<VDictObj>;

struct Value {
    std::variant<std::monostate, long, double, bool, std::string,
                 VArr, VDict> v;

    Value() = default;
    Value(long i) : v(i) {}
    Value(double d) : v(d) {}
    Value(bool b) : v(b) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(VArr a) : v(std::move(a)) {}
    Value(VDict d) : v(std::move(d)) {}

    bool operator==(const Value &o) const { return v == o.v; }
};

struct VHash {
    size_t operator()(const Value &x) const
    {
        return std::visit([](const auto &a) -> size_t {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return 0x9e3779b97f4a7c15ULL;
            else if constexpr (std::is_same_v<T, long>)
                return std::hash<long>()(a);
            else if constexpr (std::is_same_v<T, double>)
                return std::hash<double>()(a);
            else if constexpr (std::is_same_v<T, bool>)
                return std::hash<long>()(a ? 1 : 0);
            else if constexpr (std::is_same_v<T, std::string>)
                return std::hash<std::string>()(a);
            else
                return std::hash<const void *>()(a.get());
        }, x.v);
    }
};

struct VDictObj {
    std::unordered_map<Value, Value, VHash> m;
};

[[noreturn]] static inline void value_type_error()
{
    fprintf(stderr, "Value: type error\n");
    abort();
}

/* a + b with vm_num_binop's numeric rules (runtime tag dispatch). */
static inline Value vadd(const Value &a, const Value &b)
{
    if (auto *ai = std::get_if<long>(&a.v)) {
        if (auto *bi = std::get_if<long>(&b.v))
            return Value(*ai + *bi);
        if (auto *bd = std::get_if<double>(&b.v))
            return Value(static_cast<double>(*ai) + *bd);
    } else if (auto *ad = std::get_if<double>(&a.v)) {
        if (auto *bi = std::get_if<long>(&b.v))
            return Value(*ad + static_cast<double>(*bi));
        if (auto *bd = std::get_if<double>(&b.v))
            return Value(*ad + *bd);
    }
    value_type_error();
}

/* a % b (int-only, like TypeInt::mod; the benches use a nonzero rhs). */
static inline Value vmod(const Value &a, const Value &b)
{
    if (auto *ai = std::get_if<long>(&a.v))
        if (auto *bi = std::get_if<long>(&b.v))
            return Value(*ai % *bi);
    value_type_error();
}

static inline long vlong(const Value &a)
{
    if (auto *ai = std::get_if<long>(&a.v))
        return *ai;
    value_type_error();
}
