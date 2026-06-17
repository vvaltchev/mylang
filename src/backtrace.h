/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <string>

struct Exception;
struct InlineCtx;

/*
 * Append the virtual frames of an inlined-at chain to e.backtrace, innermost
 * first (each InlineCtx maps to one BacktraceFrame). Called on the error path
 * for an inlined node so its physically-absent call frames still appear. The
 * name/params strings are copied out now, while the chain is still alive.
 */
void flush_inline_frames(const InlineCtx *ic, Exception &e);

/*
 * Render an exception's call-stack backtrace (Exception::backtrace) as a
 * multi-line, console-ready string, or "" when there are no function frames
 * (the error happened at the top level, where the caret alone suffices).
 *
 * Format (most recent call on top, "main" at the bottom):
 *
 *   Backtrace (most recent call first):
 *     [0] inner(x)    at line 12
 *     [1] middle(a,b) at line 7
 *     [2] main()      at line 3
 *
 * The frame number is zero-padded to a common width only when needed (>9
 * frames). The "name(params)" column is right-padded so the "at line N" parts
 * line up; a frame wider than 60 chars has its parameter list truncated to
 * `name(p1, ..., pk, ...)`, then `name(...)` (the function name is never cut).
 */
std::string format_backtrace(const Exception &e);
