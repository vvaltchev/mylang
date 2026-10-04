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
 *
 * MYLANG_INT_DUMP=<path>: at exit, every event the process recorded,
 * sorted - the decision instances a run reached, for tests/int_enum.py -
 * plus one `choose_applied key=... pick=...` line per MYLANG_INT_CHOOSE
 * override int_choose actually honoured, written the moment it is
 * honoured (int_applied_note), so the file is APPENDED to and a crashing
 * run still names its deviation. That record bypasses IntDefer on
 * purpose: a deviation can make the JIT DISCARD the attempt it was taken
 * in (a lost bet, a register conflict) and re-emit, and the re-emission
 * numbers its decisions afresh - so the committed events may never show
 * the deviated pick although the deviation is what changed the code.
 */

#ifdef INT_TESTS

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "inttest.h"
#include "poolalloc.h"

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
    std::vector<std::string> applied;   /* honoured overrides, undeferred */
    uint64_t hits[N_SITES] = {};
    uint64_t queries[N_SITES] = {};
    std::vector<std::string> events[N_SITES];
};

Log &log()
{
    static Log l;
    return l;
}

}  // namespace

/* #107 P4: the object census (poolalloc.h) - live heap objects per kind,
 * and the counts at the mark taken when the program starts */
long long g_int_live[IOK_N] = {};
static long long g_int_live_base[IOK_N] = {};
static bool g_int_census_marked = false;

static const char *const g_iok_names[IOK_N] = {
    "str", "arr", "dict", "struct", "func", "exc" };

/* runs after main's locals are destroyed (a return from main) and BEFORE
 * any static destructor - registered from main, so it is the LAST handler
 * registered and the first to run - which makes "end == base" exactly
 * "everything the program created is freed" */
static bool g_int_census_exiting = false;

void int_census_exiting()
{
    g_int_census_exiting = true;
}

long long int_live_count(const std::string &kind)
{
    for (int k = 0; k < IOK_N; k++)
        if (kind == g_iok_names[k])
            return g_int_live[k];
    return -1;
}

/* MYLANG_INT_CENSUS=1: one `census LEAK <kind> base B end E` line per
 * kind whose count did not come back (tests/int_run.py fails on it), or
 * `census skipped (exit)`; MYLANG_INT_CENSUS=all also prints the
 * balanced kinds. stderr, so a program's own output is untouched. */
unsigned long long g_int_probe_hits = 0;

static IntChunkHook g_int_hooks[static_cast<int>(IntStage::N)];

void int_on(IntStage st, IntChunkHook fn)
{
    g_int_hooks[static_cast<int>(st)] = std::move(fn);
}
void int_off(IntStage st) { g_int_hooks[static_cast<int>(st)] = nullptr; }
void int_stage(IntStage st, const std::string &fn, Chunk &ck)
{
    if (const IntChunkHook &h = g_int_hooks[static_cast<int>(st)])
        h(fn, ck);
}

static void census_at_exit()
{
    const char *mode = std::getenv("MYLANG_INT_CENSUS");
    if (!mode || !*mode)
        return;
    if (g_int_census_exiting) {
        std::fprintf(stderr, "census skipped (exit)\n");
        return;
    }
    const bool all = std::string(mode) == "all";
    for (int k = 0; k < IOK_N; k++)
        if (all || g_int_live[k] != g_int_live_base[k])
            std::fprintf(stderr, "census %s%s base %lld end %lld\n",
                         g_int_live[k] != g_int_live_base[k] ? "LEAK "
                                                             : "",
                         g_iok_names[k], g_int_live_base[k],
                         g_int_live[k]);
    if (all)
        std::fprintf(stderr, "census probes %llu\n", g_int_probe_hits);
}

void int_census_mark()
{
    for (int k = 0; k < IOK_N; k++)
        g_int_live_base[k] = g_int_live[k];
    if (!g_int_census_marked)
        std::atexit(census_at_exit);
    g_int_census_marked = true;
}

namespace {

/* Write the per-site counts at exit, when asked to. Registered from a
 * static initializer so a script run needs no explicit call. */
void dump_at_exit()
{
    /* MYLANG_INT_DUMP=<path>: every recorded event, one per line, in
     * canonical (sorted) order - what tests/int_enum.py reads to learn
     * which decision instances a run reached */
    if (const char *dp = std::getenv("MYLANG_INT_DUMP")) {
        if (*dp) {
            std::vector<std::string> all;
            for (int i = 0; i < N_SITES; i++)
                all.insert(all.end(), log().events[i].begin(),
                           log().events[i].end());
            std::sort(all.begin(), all.end());
            if (FILE *f = std::fopen(dp, "a")) {
                for (const std::string &e : all)
                    std::fprintf(f, "%s\n", e.c_str());
                std::fclose(f);
            }
        }
    }
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

int int_choose(const std::string &key, int n, int dflt)
{
    /* parsed once: MYLANG_INT_CHOOSE is fixed for the process */
    static const std::vector<std::pair<std::string, int>> overrides = [] {
        std::vector<std::pair<std::string, int>> out;
        const char *env = std::getenv("MYLANG_INT_CHOOSE");
        std::string spec = env ? env : "";
        size_t pos = 0;
        while (pos < spec.size()) {
            /* `;` or `,`: a test header's INT-CONFIGS already spends
             * `;` on separating configurations */
            size_t end = spec.find_first_of(";,", pos);
            if (end == std::string::npos)
                end = spec.size();
            const std::string item = spec.substr(pos, end - pos);
            const size_t eq = item.rfind('=');
            if (eq != std::string::npos && eq > 0)
                out.emplace_back(item.substr(0, eq),
                                 std::atoi(item.c_str() + eq + 1));
            pos = end + 1;
        }
        return out;
    }();
    for (const auto &o : overrides)
        if (o.first == key && o.second >= 0 && o.second < n) {
            std::string line = "choose_applied";
            int_put(line, "key", key);
            int_put(line, "pick", static_cast<int64_t>(o.second));
            if (std::find(log().applied.begin(), log().applied.end(),
                          line) == log().applied.end()) {
                int_applied_note(line);
                log().applied.push_back(std::move(line));
            }
            return o.second;
        }
    return dflt;
}

void int_applied_note(const std::string &line)
{
    const char *dp = std::getenv("MYLANG_INT_DUMP");
    if (!dp || !*dp)
        return;
    if (FILE *f = std::fopen(dp, "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
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
