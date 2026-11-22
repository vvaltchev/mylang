/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <optional>
#include <string>
#include <cstdlib>
#include <cstdio>

/*
 * The small cross-platform wrappers for C-runtime calls MSVC DEPRECATES
 * (C4996) and this project builds with warnings-as-errors on all three
 * compilers - so a plain `getenv`/`fopen` fails the Windows lane and
 * nothing else. Suppressing with _CRT_SECURE_NO_WARNINGS is explicitly
 * not the fix (see CLAUDE.md, *Compiler warnings must be ADDRESSED*);
 * one inline wrapper per call is.
 */

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

/*
 * fopen. MSVC deprecates it in favour of `fopen_s`, which differs in
 * signature (it returns an errno_t and takes the FILE** out first);
 * POSIX `fopen` is fine. Returns null on failure either way, so a
 * caller checks exactly one thing.
 */
inline std::FILE *file_open(const char *path, const char *mode)
{
#ifdef _WIN32
    std::FILE *f = nullptr;
    if (fopen_s(&f, path, mode) != 0)
        return nullptr;
    return f;
#else
    return std::fopen(path, mode);
#endif
}
