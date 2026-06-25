/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <vector>
#include <iosfwd>

/*
 * Diagnostic tracing - "MyLang's mind" (see plans/repl-introspection.md).
 *
 * A per-category, toggleable narration of the compiler's reasoning: type
 * inference, inlining, specialization, template instantiation, auto-const,
 * auto-pure, array-storage decisions, const-folding. It is OFF by default and
 * built so that when a category is off the only cost is one bitmask test
 * (`trace_enabled`) - the guarded `TRACE(...)` emits can therefore sit on hot
 * compile paths without harming a normal run.
 *
 * Control surface (both, by the builtins-first rule): the `trace()` /
 * `traceoff()` / `tracing()` builtins and the REPL `:trace` meta-command drive
 * `trace_set` / `trace_clear_all` / `trace_active`.
 */

enum class TraceCat : unsigned {
    infer      = 1u << 0,
    inlining   = 1u << 1,
    specialize = 1u << 2,
    templ      = 1u << 3,
    autoconst  = 1u << 4,
    autopure   = 1u << 5,
    arrays     = 1u << 6,
    fold       = 1u << 7,
};

/* The enabled-category bitmask; the hot guard reads it directly. */
extern unsigned g_trace_mask;

inline bool trace_enabled(TraceCat c)
{
    return (g_trace_mask & static_cast<unsigned>(c)) != 0;
}

/*
 * Emit one trace line for category `c` at nesting `indent` (2 spaces each).
 * Call only inside a `trace_enabled(c)` guard (the TRACE macro does this) - it
 * does NOT re-check the mask, and the message is built by the caller, so a
 * disabled category never even constructs the string.
 */
void trace_emit(TraceCat c, int indent, const std::string &msg);

/*
 * The guarded emit. `msg` is evaluated ONLY when the category is enabled, so a
 * `TRACE(infer, 1, "x -> " + sty_to_string(t))` costs just a mask test when
 * tracing is off. `catname` is the unqualified enumerator (e.g. `infer`).
 */
#define TRACE(catname, indent, msg)                                       \
    do {                                                                  \
        if (trace_enabled(TraceCat::catname))                             \
            trace_emit(TraceCat::catname, (indent), (msg));               \
    } while (0)

/* Enable/disable a category by NAME ("infer", "inline", "specialize",
 * "template", "autoconst", "autopure", "arrays", "fold", or "all"). Returns
 * false on an unknown name. */
bool trace_set(const std::string &name, bool on);
void trace_clear_all();

/* The active category names (sorted), and a one-line human summary. */
std::vector<std::string> trace_active();
std::string trace_state_str();

/* The categories as an aligned bullet list (name + description, plus an `all`
 * row), each line prefixed by `indent` - the single source of truth, shared by
 * `:trace help` and the `trace` / `:trace` help entries so the list is
 * identical everywhere. */
std::string trace_categories_help(const std::string &indent);

/* Output controls. Color wraps the category tag in ANSI; the sink defaults to
 * &std::cerr. The REPL points the sink at its per-input capture stream so trace
 * lines appear in (and are testable from) the REPL output; a script leaves it
 * at cerr so trace never corrupts the program's stdout. */
void trace_set_color(bool on);
void trace_set_sink(std::ostream *os);
std::ostream *trace_sink();
