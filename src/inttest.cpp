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
 * it through int_hits/int_events; then one `vc <class> <hits>` line per
 * declared value class. tests/int_run sums these over its
 * runs for the site census - a site no test CHECKS fails, since reaching
 * a site without asserting on it verifies nothing.
 *
 * MYLANG_INT_DUMP=<path>: at exit, every event the process recorded,
 * sorted - the decision instances a run reached, for tests/int_enum -
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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

#include "inttest.h"
#include "numtext.h"
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
 * kind whose count did not come back (tests/int_run fails on it), or
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

/* The value classes: plain counters in static storage (zero-initialized,
 * no constructor), so a recording point is one increment. */
static constexpr int N_VCLASSES = static_cast<int>(IntVc::count_);
static uint64_t g_int_vc_hits[N_VCLASSES];
static const char *const g_vc_names[] = {
#define V(name, desc) #name,
    ML_INT_VCLASSES(V)
#undef V
};
static_assert(sizeof(g_vc_names) / sizeof(g_vc_names[0]) == N_VCLASSES,
              "intsites.h: value-class names out of step with the enum");
/* the classes name 64-bit boundaries (shl_63, shl_64, INT_MIN) */
static_assert(sizeof(intptr_t) == 8, "value classes assume a 64-bit int");

void int_vc(IntVc c) noexcept
{
    g_int_vc_hits[static_cast<int>(c)]++;
}

static void int_vc_count(int64_t n, IntVc neg, IntVc zero, IntVc w63,
                         IntVc w64, IntVc over) noexcept
{
    if (n < 0)
        int_vc(neg);
    else if (n == 0)
        int_vc(zero);
    else if (n == 63)
        int_vc(w63);
    else if (n == 64)
        int_vc(w64);
    else if (n > 64)
        int_vc(over);
}

void int_vc_shift(char op, int64_t v, int64_t n) noexcept
{
    switch (op) {
    case 'l':
        int_vc_count(n, IntVc::shl_neg, IntVc::shl_zero, IntVc::shl_63,
                     IntVc::shl_64, IntVc::shl_over);
        break;
    case 'r':
        int_vc_count(n, IntVc::shr_neg, IntVc::shr_zero, IntVc::shr_63,
                     IntVc::shr_64, IntVc::shr_over);
        if (v < 0 && n >= 64)
            int_vc(IntVc::shr_fill);
        break;
    default:
        int_vc_count(n, IntVc::ushr_neg, IntVc::ushr_zero, IntVc::ushr_63,
                     IntVc::ushr_64, IntVc::ushr_over);
        if (v < 0 && n > 0 && n < 64)
            int_vc(IntVc::ushr_negval);
        break;
    }
}

void int_vc_divmod(bool mod, int64_t a, int64_t b) noexcept
{
    if (b == 0) {
        int_vc(mod ? IntVc::mod_zero : IntVc::div_zero);
        return;
    }
    if (b == -1 && a == INT64_MIN) {
        int_vc(mod ? IntVc::mod_min_neg1 : IntVc::div_min_neg1);
        return;
    }
    if (a % b == 0)
        return;
    if (!mod) {
        if ((a < 0) != (b < 0))
            int_vc(IntVc::div_neg_trunc);
    } else if (a < 0) {
        int_vc(IntVc::mod_neg);
    } else if (b < 0) {
        int_vc(IntVc::mod_neg_divisor);
    }
}

void int_vc_arith(char op, int64_t a, int64_t b) noexcept
{
    int64_t r;
    switch (op) {
    case '+':
        if (__builtin_add_overflow(a, b, &r))
            int_vc(IntVc::add_wrap);
        break;
    case '-':
        if (__builtin_sub_overflow(a, b, &r))
            int_vc(IntVc::sub_wrap);
        break;
    case '*':
        if (__builtin_mul_overflow(a, b, &r))
            int_vc(IntVc::mul_wrap);
        break;
    default:
        if (a == INT64_MIN)
            int_vc(IntVc::neg_min);
        break;
    }
}

void int_vc_index(bool str, int64_t idx, uint64_t len) noexcept
{
    const int64_t n = static_cast<int64_t>(len);
    if (n == 0) {
        int_vc(str ? IntVc::str_idx_empty : IntVc::arr_idx_empty);
        return;
    }
    /* each boundary the index sits on - at len 1, 0 is first AND last */
    if (idx == -n - 1)
        int_vc(str ? IntVc::str_idx_below : IntVc::arr_idx_below);
    if (idx == -n)
        int_vc(str ? IntVc::str_idx_neg_first : IntVc::arr_idx_neg_first);
    if (idx == -1)
        int_vc(str ? IntVc::str_idx_neg_last : IntVc::arr_idx_neg_last);
    if (idx == 0)
        int_vc(str ? IntVc::str_idx_first : IntVc::arr_idx_first);
    if (idx == n - 1)
        int_vc(str ? IntVc::str_idx_last : IntVc::arr_idx_last);
    if (idx == n)
        int_vc(str ? IntVc::str_idx_len : IntVc::arr_idx_len);
}

void int_vc_size(IntVc empty, IntVc one, uint64_t n) noexcept
{
    if (n == 0)
        int_vc(empty);
    else if (n == 1)
        int_vc(one);
}

void int_vc_slice(bool str, bool has_s, int64_t s, bool has_e, int64_t e,
                  uint64_t len) noexcept
{
#define VC(name) int_vc(str ? IntVc::str_slice_##name : IntVc::arr_slice_##name)
    const int64_t n = static_cast<int64_t>(len);
    if (n == 0) {
        VC(of_empty);
        return;
    }
    if (!has_s && !has_e)
        VC(open);
    /* Python's rules, as TypeArr::slice applies them */
    int64_t ns = 0, ne = n;
    if (has_s) {
        if (s < -n)
            VC(start_below);
        else if (s < 0)
            VC(start_neg);
        else if (s == n)
            VC(start_len);
        ns = s < 0 ? std::max<int64_t>(s + n, 0) : s;
    }
    if (has_e) {
        if (e < -n)
            VC(end_below);
        else if (e < 0)
            VC(end_neg);
        else if (e == n)
            VC(end_len);
        else if (e > n)
            VC(end_past);
        ne = e < 0 ? e + n : std::min(e, n);
    }
    if (ns < n) {
        if (ne == ns)
            VC(empty_range);
        else if (ne < ns)
            VC(reversed);
    }
#undef VC
}

void int_vc_float_div0(bool mod, double b) noexcept
{
    if (b == 0.0)
        int_vc(mod ? IntVc::fmod_zero : IntVc::fdiv_zero);
}

void int_vc_float_op(double a, double b, double r) noexcept
{
    if (std::isnan(a) || std::isnan(b))
        int_vc(IntVc::f_nan_operand);
    else if (std::isnan(r))
        int_vc(IntVc::f_nan_made);
    else if (std::isinf(r) && std::isfinite(a) && std::isfinite(b))
        int_vc(IntVc::f_overflow);
    if (r == 0.0 && std::signbit(r))
        int_vc(IntVc::f_neg_zero);
}

void int_vc_float_neg(double a) noexcept
{
    if (std::isnan(a))
        int_vc(IntVc::f_nan_operand);
    else if (a == 0.0 && !std::signbit(a))
        int_vc(IntVc::f_neg_zero);           /* the result: -0.0 */
}

void int_vc_float_cmp(double a, double b) noexcept
{
    if (std::isnan(a) || std::isnan(b))
        int_vc(IntVc::fcmp_nan);
    else if (a == 0.0 && b == 0.0 && std::signbit(a) != std::signbit(b))
        int_vc(IntVc::fcmp_zero_signs);
}

void int_vc_float_int(double f) noexcept
{
    const double lim = 9223372036854775808.0;      /* 2^63 */
    if (std::isnan(f))
        int_vc(IntVc::fint_nan);
    else if (std::isinf(f))
        int_vc(IntVc::fint_inf);
    else if (!(f >= -lim && f < lim))
        int_vc(IntVc::fint_range);
    else if (f == -lim)
        int_vc(IntVc::fint_min);
    else if (f == 0.0 && std::signbit(f))
        int_vc(IntVc::fint_neg_zero);
    else if (f < 0.0 && f != std::trunc(f))
        int_vc(IntVc::fint_neg_frac);
}

void int_vc_float_text(double v) noexcept
{
    if (std::isnan(v))
        int_vc(IntVc::fstr_nan);
    else if (std::isinf(v))
        int_vc(IntVc::fstr_inf);
    else if (v == 0.0 && std::signbit(v))
        int_vc(IntVc::fstr_neg_zero);
}

void int_vc_range(int64_t start, int64_t end, int64_t step,
                  uint64_t count) noexcept
{
    if (count == 0) {
        int_vc(IntVc::range_empty);
        return;
    }
    if (count == 1)
        int_vc(IntVc::range_one);
    if (step < 0)
        int_vc(IntVc::range_neg_step);

    const uint64_t span = step > 0
        ? static_cast<uint64_t>(end) - static_cast<uint64_t>(start)
        : static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
    const uint64_t mag = step > 0
        ? static_cast<uint64_t>(step)
        : uint64_t(0) - static_cast<uint64_t>(step);
    int_vc(span % mag == 0 ? IntVc::range_end_hit : IntVc::range_end_miss);

    const int64_t last = static_cast<int64_t>(
        static_cast<uint64_t>(start) +
        (count - 1) * static_cast<uint64_t>(step));
    const int64_t mx = std::numeric_limits<int64_t>::max();
    const int64_t mn = std::numeric_limits<int64_t>::min();
    if (step > 0 ? last > mx - step : last < mn - step)
        int_vc(IntVc::range_limit);
}

void int_vc_str_int(const char *s, size_t n, int outcome,
                    int64_t v) noexcept
{
    const std::string_view sv(s, n);
    const std::string_view t = numtext_trim(sv);

    switch (static_cast<NumText>(outcome)) {
        case NumText::ok:
            if (t.size() != sv.size())
                int_vc(IntVc::sint_space);
            if (t[0] == '+' || t[0] == '-')
                int_vc(IntVc::sint_sign);
            if (v == std::numeric_limits<int64_t>::max())
                int_vc(IntVc::sint_max);
            else if (v == std::numeric_limits<int64_t>::min())
                int_vc(IntVc::sint_min);
            break;
        case NumText::overflow:
            int_vc(IntVc::sint_range);
            break;
        default:
            int_vc(t.empty() ? IntVc::sint_empty : IntVc::sint_junk);
            break;
    }
}

void int_vc_str_float(const char *s, size_t n, int outcome, double v,
                      bool at_compile) noexcept
{
    const std::string_view sv(s, n);
    const std::string_view t = numtext_trim(sv);
    const NumText o = static_cast<NumText>(outcome);

    if (o == NumText::bad) {
        int_vc(IntVc::sflt_junk);
        return;
    }
    if (o != NumText::ok) {
        int_vc(at_compile ? IntVc::sflt_const_range
               : o == NumText::overflow ? IntVc::sflt_overflow
               : IntVc::sflt_underflow);
        return;
    }
    if (t.size() != sv.size())
        int_vc(IntVc::sflt_space);
    if (std::isinf(v) || std::isnan(v)) {
        int_vc(IntVc::sflt_word);           /* only a word reads as one */
        return;
    }
    if (t.find_first_of("eE") != std::string_view::npos)
        int_vc(IntVc::sflt_exp);
    if (std::fpclassify(v) == FP_SUBNORMAL)
        int_vc(IntVc::sflt_subnormal);
}

void int_vc_str_digits(double v, int64_t digits) noexcept
{
    if (digits < 0 || digits > 64) {
        int_vc(IntVc::sdig_range);
        return;
    }
    if (digits == 0)
        int_vc(IntVc::sdig_zero);
    if (digits == 64)
        int_vc(IntVc::sdig_max);

    /* Exactly halfway at `digits` places: v * 10^d + 1/2 an integer, i.e.
     * 2 * v * 10^d odd. A double is a / 2^k with a odd, so that is
     * a * 5^d * 2^(d+1-k) odd - k == d + 1, v * 2^(d+1) an odd integer. */
    const double t = std::ldexp(v, static_cast<int>(digits) + 1);
    if (std::isfinite(t) && t == std::trunc(t) && std::fmod(t, 2.0) != 0)
        int_vc(IntVc::sdig_tie);
}

namespace {

/* Write the per-site counts at exit, when asked to. Registered from a
 * static initializer so a script run needs no explicit call. */
void dump_at_exit()
{
    /* MYLANG_INT_DUMP=<path>: every recorded event, one per line, in
     * canonical (sorted) order - what tests/int_enum reads to learn
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
    /* `vc <class> <hits>`, every declared class, zeros included */
    for (int i = 0; i < N_VCLASSES; i++)
        std::fprintf(f, "vc %s %llu\n", g_vc_names[i],
                     static_cast<unsigned long long>(g_int_vc_hits[i]));
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
