/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * The intrusive-test core (#107, plans/intrusive-tests.md): the event log
 * and per-site hit counts behind ML_INT. Compiled empty without INT_TESTS.
 *
 * Determinism: events are recorded in program order and printed from their
 * typed payloads only (intsites.h forbids pointers in a payload), so the log
 * is the same on every run of the same (source, build, flags).
 *
 * MYLANG_INT_OUT=<path>: at exit, the process appends one
 * `site hits queries` line per site to <path> (every site, zeros
 * included): how many events it recorded, and how many times a test READ
 * it through int_hits/int_events. tests/int_run.py sums these over its
 * runs for the site census - a site no test CHECKS fails, since reaching
 * a site without asserting on it verifies nothing.
 */

#ifdef INT_TESTS

#include <cstdio>
#include <cstdlib>

#include "inttest.h"

namespace {

constexpr int N_SITES = static_cast<int>(IntSite::count_);

const char *const site_names[] = {
#define X(name, desc, fields) #name,
    ML_INT_SITES(X)
#undef X
};

const char *const site_descs[] = {
#define X(name, desc, fields) desc,
    ML_INT_SITES(X)
#undef X
};

static_assert(sizeof(site_names) / sizeof(site_names[0]) == N_SITES,
              "intsites.h: name table out of step with the enum");

struct Log {
    uint64_t hits[N_SITES] = {};
    uint64_t queries[N_SITES] = {};
    std::vector<std::string> events[N_SITES];
};

Log &log()
{
    static Log l;
    return l;
}

/* Write the per-site counts at exit, when asked to. Registered from a
 * static initializer so a script run needs no explicit call. */
void dump_at_exit()
{
    const char *path = std::getenv("MYLANG_INT_OUT");
    if (!path || !*path)
        return;
    FILE *f = std::fopen(path, "a");
    if (!f)
        return;
    for (int i = 0; i < N_SITES; i++)
        std::fprintf(f, "%s %llu %llu\n", site_names[i],
                     static_cast<unsigned long long>(log().hits[i]),
                     static_cast<unsigned long long>(log().queries[i]));
    std::fclose(f);
}

struct AtExit {
    AtExit() { log(); std::atexit(dump_at_exit); }
} at_exit_registration;

}  // namespace

static IntDefer *g_defer = nullptr;     /* the innermost live scope */

void int_record(IntSite s, std::string &&line)
{
    if (g_defer) {
        g_defer->buf.emplace_back(s, std::move(line));
        return;
    }
    const int i = static_cast<int>(s);
    log().hits[i]++;
    log().events[i].push_back(std::move(line));
}

IntDefer::IntDefer() : prev(g_defer)
{
    g_defer = this;
}

IntDefer::~IntDefer()
{
    /* uncommitted: the buffered events are dropped with the scope */
    g_defer = prev;
}

void IntDefer::commit()
{
    /* an identical event twice in one committed attempt is ONE fact: the
     * JIT emits an op more than once inside a single emission (a C1 cold
     * copy re-emits the loop), and its site states a property of the op,
     * not a count of how many copies of it exist */
    std::vector<std::pair<IntSite, std::string>> out;
    for (auto &ev : buf) {
        bool dup = false;
        for (const auto &o : out)
            dup = dup || (o.first == ev.first && o.second == ev.second);
        if (!dup)
            out.push_back(std::move(ev));
    }
    buf.clear();
    IntDefer *const self = g_defer;
    g_defer = prev;                 /* publish to the enclosing scope */
    for (auto &ev : out)
        int_record(ev.first, std::move(ev.second));
    g_defer = self;
}

const char *int_site_name(IntSite s)
{
    return site_names[static_cast<int>(s)];
}

const char *int_site_desc(IntSite s)
{
    return site_descs[static_cast<int>(s)];
}

int int_site_by_name(const std::string &name)
{
    for (int i = 0; i < N_SITES; i++)
        if (name == site_names[i])
            return i;
    return -1;
}

void int_note_query(IntSite s)
{
    log().queries[static_cast<int>(s)]++;
}

uint64_t int_hits(IntSite s)
{
    return log().hits[static_cast<int>(s)];
}

const std::vector<std::string> &int_events(IntSite s)
{
    return log().events[static_cast<int>(s)];
}

void int_reset()
{
    for (int i = 0; i < N_SITES; i++)
        log().events[i].clear();
    /* the hit counts are NOT reset: they feed the exit census, which must
     * see every event the process recorded */
}

void int_put(std::string &o, const char *key, int64_t v)
{
    o += ' ';
    o += key;
    o += '=';
    o += std::to_string(v);
}

void int_put(std::string &o, const char *key, bool v)
{
    o += ' ';
    o += key;
    o += v ? "=true" : "=false";
}

void int_put(std::string &o, const char *key, const std::string &v)
{
    o += ' ';
    o += key;
    o += "=\"";
    for (char c : v) {
        if (c == '"' || c == '\\')
            o += '\\';
        o += c;
    }
    o += '"';
}

#endif  /* INT_TESTS */
