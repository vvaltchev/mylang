/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <optional>
#include <string>
#include <cstdlib>

/*
 * A cross-platform read-only environment lookup. MSVC deprecates plain `getenv`
 * (C4996) in favor of `_dupenv_s` (which allocates); POSIX `getenv` is fine.
 * Returns nullopt when the variable is UNSET - distinct from set-to-empty,
 * which callers like NO_COLOR treat as "present". Kept in a header (inline) so
 * the REPL and the builtins share one implementation.
 */
inline std::optional<std::string> env_get(const char *name)
{
#ifdef _WIN32
    char *buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf)
        return std::nullopt;
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char *v = std::getenv(name);
    if (!v)
        return std::nullopt;
    return std::string(v);
#endif
}
