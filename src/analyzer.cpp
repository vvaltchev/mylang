/* SPDX-License-Identifier: BSD-2-Clause */

#include "analyzer.h"
#include "resolver.h"   /* resolve_names, collect_resolver_analysis */

#include <ostream>
#include <string>
#include <vector>

using std::string;
using std::ostream;

/* ANSI color escape for an annotation kind (see analyzer.h's legend). */
static const char *
anno_code(AnnoKind k)
{
    switch (k) {
        case AnnoKind::auto_const:  return "\033[33m";   /* yellow  */
        case AnnoKind::flat_array:  return "\033[32m";   /* green   */
        case AnnoKind::dyn_array:   return "\033[31m";   /* red     */
        case AnnoKind::inlined:     return "\033[94m";   /* blue    */
        case AnnoKind::specialized: return "\033[36m";   /* cyan    */
        case AnnoKind::folded:      return "\033[35m";   /* magenta */
        default:                    return "";
    }
}

void
analyze_and_render(ostream &o, Construct *root, AnalysisInfo &info,
                   const std::vector<string> &src, bool color,
                   bool repl_mode, bool enable_inline, int inline_threshold)
{
    collect_array_analysis(root, info);
    resolve_names(root, enable_inline, inline_threshold, &info, repl_mode);
    collect_resolver_analysis(root, info);
    render_analysis(o, src, info, color);
}

void
render_analysis(ostream &o, const std::vector<string> &src,
                const AnalysisInfo &info, bool color)
{
    static const char *const RESET = "\033[0m";
    static const char *const DIM = "\033[2m";

    if (color) {
        o << "Legend: "
          << "\033[33mauto-const/pure\033[0m  "
          << "\033[32mflat array\033[0m  "
          << "\033[31mdyn array\033[0m  "
          << "\033[94minlined\033[0m  "
          << "\033[36mspecialized\033[0m  "
          << "\033[35mfolded call\033[0m  "
          << DIM << "dead code" << RESET << "\n";
        o << "--------------------------\n";
    }

    for (size_t li = 0; li < src.size(); li++) {

        const int line = static_cast<int>(li) + 1;
        const string &text = src[li];
        const int n = static_cast<int>(text.length());

        if (!color) {
            o << text << "\n";
            continue;
        }

        /* per-column state: -1 default, -2 dim, else (int)AnnoKind color. */
        std::vector<int> attr(n, -1);

        for (const auto &d : info.dead) {
            if (line < d.l1 || line > d.l2)
                continue;
            const int from = (line == d.l1) ? d.c1 : 1;
            const int to   = (line == d.l2) ? d.c2 : n;
            for (int c = from; c <= to && c <= n; c++)
                if (c >= 1)
                    attr[c - 1] = -2;
        }

        /* color spans override dim (an annotated identifier still shows) */
        auto lo = info.spans.lower_bound({line, 0});
        auto hi = info.spans.lower_bound({line + 1, 0});
        for (auto it = lo; it != hi; ++it) {
            const int col = it->first.second;
            const int len = it->second.len;
            for (int c = col; c < col + len && c <= n; c++)
                if (c >= 1)
                    attr[c - 1] = static_cast<int>(it->second.kind);
        }

        string out;
        int cur = -1;   /* currently-open attribute (-1 == default/none open) */

        for (int c = 0; c < n; c++) {
            if (attr[c] != cur) {
                if (cur != -1)
                    out += RESET;
                if (attr[c] == -2)
                    out += DIM;
                else if (attr[c] != -1)
                    out += anno_code(static_cast<AnnoKind>(attr[c]));
                cur = attr[c];
            }
            out += text[c];
        }
        if (cur != -1)
            out += RESET;

        o << out << "\n";
    }
}
