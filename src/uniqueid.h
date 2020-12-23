/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include <set>
#include <string>
#include <string_view>

class UniqueId final {

    struct Comparator {

        struct is_transparent { };

        bool operator()(const UniqueId &a, const UniqueId &b) const {
            return a.val < b.val;
        }

        bool operator()(const UniqueId &a, const std::string_view &b) const {
            return a.val < b;
        }
    };

    static std::set<UniqueId, Comparator> unique_set;

public:

    const std::string val;

    UniqueId(const std::string_view &str) : val(str) { }

    static const UniqueId *get(const std::string_view &str) {
        return &(*unique_set.emplace(str).first);
    }
};
