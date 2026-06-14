/* SPDX-License-Identifier: BSD-2-Clause */

#include "backtrace.h"
#include "errors.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using std::string;
using std::vector;

/*
 * Target width for a frame's "name(params)" text. A frame wider than this has
 * its parameter list truncated; the function name itself is never truncated, so
 * the result can still exceed it for a very long name (then alignment grows).
 */
static constexpr size_t MAX_FRAME_WIDTH = 60;

/* Build "name(p1, p2, ...)", truncating the param list to fit (see above). */
static string
frame_name(const string &name, const vector<string> &params)
{
    if (params.empty())
        return name + "()";

    /* Try the full form first. */
    string full = name + "(";
    for (size_t i = 0; i < params.size(); i++)
        full += (i ? ", " : "") + params[i];
    full += ")";

    if (full.size() <= MAX_FRAME_WIDTH)
        return full;

    /* Drop trailing params (replaced by "...") until it fits. */
    for (size_t k = params.size() - 1; k >= 1; k--) {
        string cand = name + "(";
        for (size_t i = 0; i < k; i++)
            cand += (i ? ", " : "") + params[i];
        cand += ", ...)";
        if (cand.size() <= MAX_FRAME_WIDTH)
            return cand;
    }

    /* Not even one param fits (or the name alone is too long). */
    return name + "(...)";
}

static int
num_digits(size_t n)
{
    int d = 1;
    while (n >= 10) { n /= 10; d++; }
    return d;
}

void
flush_inline_frames(const InlineCtx *ic, Exception &e)
{
    for (; ic; ic = ic->parent)
        e.backtrace.push_back({ ic->callee_name, ic->params, ic->call_site });
}

string
format_backtrace(const Exception &e)
{
    const auto &bt = e.backtrace;

    if (bt.empty())
        return "";

    /*
     * Build display frames. The innermost recorded frame's line is the error
     * site (the exception's own loc); every other frame's line is the call site
     * where it invoked the next-deeper frame. The bottom frame is "main".
     */
    struct DispFrame { string name; int line; };
    vector<DispFrame> frames;

    for (size_t i = 0; i < bt.size(); i++) {
        const int line = (i == 0) ? e.loc_start.line
                                  : bt[i - 1].call_site.line;
        frames.push_back({ frame_name(bt[i].name, bt[i].params), line });
    }
    frames.push_back({ "main()", bt.back().call_site.line });

    /* Pass 1: find the name column's max width. */
    size_t name_w = 0;

    for (const auto &f : frames)
        name_w = std::max(name_w, f.name.size());

    const int num_w = num_digits(frames.size());

    /* Pass 2: emit, right-padding the name column so "at line N" aligns. */
    std::ostringstream os;
    os << "\nBacktrace (most recent call first):\n";

    for (size_t i = 0; i < frames.size(); i++) {

        string num = std::to_string(i);
        num.insert(0, num_w - static_cast<int>(num.size()), '0');

        string nm = frames[i].name;
        nm.append(name_w - nm.size(), ' ');

        os << "  [" << num << "] " << nm
           << " at line " << frames[i].line << "\n";
    }

    return os.str();
}
