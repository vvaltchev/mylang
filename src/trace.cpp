/* SPDX-License-Identifier: BSD-2-Clause */

#include "trace.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

unsigned g_trace_mask = 0;

namespace {

struct CatName {
    const char *name;
    TraceCat cat;
    const char *color;     /* ANSI for the category tag */
    const char *desc;      /* one-line description (for :trace help / :help) */
};

/* The user-facing category names, tag colors, and descriptions - the single
 * source of truth shared by `:trace help`, `:help :trace`, and `--trace`. */
const CatName cat_names[] = {
    { "infer",      TraceCat::infer,      "\x1b[36m",     /* cyan */
      "type inference: how each type is inferred" },
    { "inline",     TraceCat::inlining,   "\x1b[34m",     /* blue */
      "calls spliced in place" },
    { "specialize", TraceCat::specialize, "\x1b[35m",     /* magenta */
      "const-arg calls redirected to a $specN clone" },
    { "template",   TraceCat::templ,      "\x1b[32m",     /* green */
      "a template monomorphized to $tmplN per signature" },
    { "autoconst",  TraceCat::autoconst,  "\x1b[33m",     /* yellow */
      "a write-once scalar var folded to a constant" },
    { "autopure",   TraceCat::autopure,   "\x1b[93m",     /* bright yellow */
      "a function proven effectively pure" },
    { "arrays",     TraceCat::arrays,     "\x1b[92m",     /* bright green */
      "flat (unboxed) vs general array storage" },
    { "fold",       TraceCat::fold,       "\x1b[90m",     /* gray */
      "const expressions / calls folded to literals" },
};

const CatName *lookup(TraceCat c)
{
    for (const CatName &cn : cat_names)
        if (cn.cat == c)
            return &cn;
    return nullptr;
}

bool g_trace_color = false;
std::ostream *g_trace_sink = &std::cerr;

}  /* namespace */

void
trace_emit(TraceCat c, int indent, const std::string &msg)
{
    std::ostream &o = *g_trace_sink;
    for (int i = 0; i < indent; i++)
        o << "  ";

    const CatName *cn = lookup(c);
    const char *tag = cn ? cn->name : "trace";
    const int tagw = 10;
    const int pad = tagw - static_cast<int>(std::strlen(tag));

    if (g_trace_color && cn)
        o << cn->color << tag << "\x1b[0m";
    else
        o << tag;
    for (int i = 0; i < pad; i++)
        o << ' ';
    o << ' ' << msg << "\n";
}

bool
trace_set(const std::string &name, bool on)
{
    if (name == "all") {
        unsigned all = 0;
        for (const CatName &cn : cat_names)
            all |= static_cast<unsigned>(cn.cat);
        if (on)
            g_trace_mask |= all;
        else
            g_trace_mask &= ~all;
        return true;
    }
    for (const CatName &cn : cat_names) {
        if (name == cn.name) {
            if (on)
                g_trace_mask |= static_cast<unsigned>(cn.cat);
            else
                g_trace_mask &= ~static_cast<unsigned>(cn.cat);
            return true;
        }
    }
    return false;
}

void
trace_clear_all()
{
    g_trace_mask = 0;
}

std::vector<std::string>
trace_active()
{
    std::vector<std::string> out;
    for (const CatName &cn : cat_names)
        if (g_trace_mask & static_cast<unsigned>(cn.cat))
            out.push_back(cn.name);
    std::sort(out.begin(), out.end());
    return out;
}

std::string
trace_categories_help(const std::string &indent)
{
    size_t w = 3;                        /* "all" */
    for (const CatName &cn : cat_names)
        w = std::max(w, std::strlen(cn.name));

    std::string s;
    auto line = [&](const char *name, const char *desc) {
        s += indent + "- " + name;
        for (size_t i = std::strlen(name); i < w; i++)
            s += ' ';
        s += "  ";
        s += desc;
        s += "\n";
    };
    for (const CatName &cn : cat_names)
        line(cn.name, cn.desc);
    line("all", "every category at once");
    return s;
}

std::string
trace_state_str()
{
    const std::vector<std::string> on = trace_active();
    if (on.empty())
        return "tracing: off";
    std::string s = "tracing:";
    for (const std::string &c : on)
        s += " " + c;
    return s;
}

void trace_set_color(bool on) { g_trace_color = on; }

void
trace_set_sink(std::ostream *os)
{
    g_trace_sink = os ? os : &std::cerr;
}

std::ostream *trace_sink() { return g_trace_sink; }
