/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "errors.h"   /* Loc */
#include <map>
#include <vector>
#include <utility>

class Construct;

/*
 * Optimization annotations collected by the `-a`/`--analyze` pass and rendered
 * as ANSI colors over the source (see mylang.cpp's renderer and the help text
 * for the legend). The inferencer and the resolver passes record their
 * decisions here, keyed by the *original source location* of the identifier or
 * call-site; the renderer then re-prints the file verbatim with one color per
 * annotated span. Inactive in a normal run (the passes only record when an
 * AnalysisInfo is threaded in), so it costs nothing unless analysis is asked
 * for.
 */
enum class AnnoKind : unsigned char {
    none,
    auto_const,    /* yellow:  auto-const var/param, auto-pure function */
    flat_array,    /* green:   array<int>/array<float> (unboxed storage) */
    dyn_array,     /* red:     array<dyn> (dynamic, unoptimizable) */
    inlined,       /* blue:    call spliced inline at its call-site */
    specialized,   /* cyan:    call redirected to a $specN clone */
    folded,        /* magenta: call evaluated at compile time -> literal */
};

struct AnalysisInfo {

    struct Span {
        int len;            /* identifier width in columns */
        AnnoKind kind;
    };

    /* (line, col) -> span; col is 1-based, matching Loc. */
    std::map<std::pair<int, int>, Span> spans;

    /* Dead (DCE'd) source ranges, inclusive [l1:c1 .. l2:c2], dimmed. */
    struct DeadRange { int l1, c1, l2, c2; };
    std::vector<DeadRange> dead;

    /*
     * Precedence when two records land on the same location: the more
     * "interesting" optimization wins (e.g. an array parameter shows its
     * storage color, not auto-const yellow; a folded/inlined call-site beats
     * everything).
     */
    static int prio(AnnoKind k) {
        switch (k) {
            case AnnoKind::inlined:
            case AnnoKind::specialized:
            case AnnoKind::folded:      return 3;   /* call-site decisions */
            case AnnoKind::dyn_array:
            case AnnoKind::flat_array:  return 2;   /* array storage */
            case AnnoKind::auto_const:  return 1;
            default:                    return 0;
        }
    }

    void mark(Loc l, int len, AnnoKind k) {
        if (!l.line || !l.col || len <= 0)
            return;
        const auto key = std::make_pair(l.line, l.col);
        auto it = spans.find(key);
        if (it == spans.end() || prio(k) > prio(it->second.kind))
            spans[key] = Span{len, k};
    }

    void mark_dead(Loc a, Loc b) {
        if (!a.line || !b.line)
            return;
        dead.push_back(DeadRange{a.line, a.col, b.line, b.col});
    }
};

/*
 * Collect array-storage annotations (green for flat int/float, red for
 * array<dyn>) by running type inference over the clean tree and inspecting each
 * resolved identifier's static type. Mirrors dump_type_info; non-mutating from
 * the renderer's point of view (the tree's source Locs are what is recorded).
 */
void collect_array_analysis(Construct *root, AnalysisInfo &out);
