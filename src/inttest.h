/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * THE INTRUSIVE-TEST INSTRUMENTATION (#107, plans/intrusive-tests.md).
 *
 * Every abstraction here EXPANDS TO NOTHING without INT_TESTS, so a shipping
 * and a plain TESTS=1 build are byte-identical to a tree without it. With
 * INT_TESTS it records DETERMINISTIC facts at the place they are decided.
 *
 * The contract (plan section 2): an INT build may cost memory and speed and
 * NOTHING else - no output, no exception, no internal DECISION may differ.
 * So an instrumented point only ever READS the state it reports; it never
 * feeds anything back into the code around it.
 *
 *   ML_INT(site, fields...)     record one event at `site` (intsites.h);
 *                               the fields are NOT evaluated without
 *                               INT_TESTS, so they may cost anything
 *   ML_INT_FIELD(type, name, init)
 *                               an extra struct member (`type name = init;`),
 *                               INT builds only - NEVER in a layout emitted
 *                               code or the .myv format reads (LValue,
 *                               EvalValue, Frame, the JitProbe structs): see
 *                               plan section 2b
 *   ML_INT_DEFER(name) / ML_INT_COMMIT(name)
 *                               a scope whose events publish only when
 *                               committed (a pass that may discard and
 *                               redo its work - see IntDefer)
 *   ML_INT_ONLY(stmts...)       statements that exist only in an INT build
 *                               (keeping an INT_FIELD up to date); like
 *                               ML_INT they may only READ the code's state
 */

#pragma once

#ifdef INT_TESTS

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "intsites.h"

enum class IntSite : int {
#define F(t, n)
#define X(name, desc, fields) name,
    ML_INT_SITES(X)
#undef X
#undef F
    count_
};

/* One typed payload per site, filled positionally by ML_INT. */
#define F(t, n) t n;
#define X(name, desc, fields) struct IntPay_##name { fields };
ML_INT_SITES(X)
#undef X
#undef F

/* The value classes (intsites.h, ML_INT_VCLASSES): declared boundaries,
 * counted - never events. */
enum class IntVc : int {
#define V(name, desc) name,
    ML_INT_VCLASSES(V)
#undef V
    count_
};
void int_vc(IntVc c) noexcept;                      /* one more hit */
/* Classify an operation's operands and count every class they sit on.
 * noexcept and allocation-free: each is called (through ML_INT_ONLY) from
 * hot product code, and a hook that could throw would add an exception
 * edge to every caller - a branch the coverage universe counts. */
void int_vc_shift(char op, int64_t v, int64_t n) noexcept; /* 'l' 'r' 'u' */
void int_vc_divmod(bool mod, int64_t a, int64_t b) noexcept;
void int_vc_arith(char op, int64_t a, int64_t b) noexcept; /* + - * n(eg) */
void int_vc_index(bool str, int64_t idx, uint64_t len) noexcept;
void int_vc_size(IntVc empty, IntVc one, uint64_t n) noexcept;
/* a[s:e] / s[s:e]: has_* says whether that bound was given */
void int_vc_slice(bool str, bool has_s, int64_t s, bool has_e, int64_t e,
                  uint64_t len) noexcept;
void int_vc_float_div0(bool mod, double b) noexcept;
void int_vc_float_op(double a, double b, double r) noexcept; /* r = a OP b */
void int_vc_float_neg(double a) noexcept;
void int_vc_float_cmp(double a, double b) noexcept;
void int_vc_float_int(double f) noexcept;                   /* int(f) */
void int_vc_float_text(double v) noexcept;                  /* f as text */
/* range(start, end, step), step != 0, `count` elements */
void int_vc_range(int64_t start, int64_t end, int64_t step,
                  uint64_t count) noexcept;
/* int(s) / float(s): the string, the NumText outcome (numtext.h) and the
 * value read; `at_compile` - float() is being const-evaluated */
void int_vc_str_int(const char *s, size_t n, int outcome,
                    int64_t v) noexcept;
void int_vc_str_float(const char *s, size_t n, int outcome, double v,
                      bool at_compile) noexcept;
void int_vc_str_digits(double v, int64_t digits) noexcept;  /* str(v, d) */

/* The core (inttest.cpp). */
void int_record(IntSite s, std::string &&line);
const char *int_site_name(IntSite s);
const char *int_site_desc(IntSite s);
int int_site_by_name(const std::string &name);      /* -1 when unknown */
uint64_t int_hits(IntSite s);
void int_note_query(IntSite s);                     /* a test READ it */
const std::vector<std::string> &int_events(IntSite s);
void int_reset();                                   /* every site */
/* #107 P4: mark the object census's baseline (main, before any program) */
void int_census_mark();
/* ...and the program is ending through exit(): main's locals stay alive,
 * so the census has nothing to compare */
void int_census_exiting();
/* the live count of one object kind ("str", "arr", "dict", "struct",
 * "func", "exc"), or -1 for an unknown name - `int_live(kind)` */
long long int_live_count(const std::string &kind);
/* #107 P5: JIT probes run (MYLANG_INT_PROBE) - printed as `census probes
 * N` under MYLANG_INT_CENSUS=all, so a runner can prove they ran */
extern unsigned long long g_int_probe_hits;

/*
 * #107 P6: THE CHUNK HOOK. A C++ test registers a function for a pipeline
 * STAGE and gets every compiled chunk there - `fn` is the function's
 * internal name (`main` for the root) - and may EDIT it before the next
 * stage runs: a shape our codegen never emits, reached without writing a
 * second compiler. One hook per stage; int_off clears it, and a test
 * must clear it before returning (IntHookScope does both).
 *   pre_splice  - every chunk codegen produced, before the bytecode
 *                 inliner snapshots any of them (so an edit to a callee
 *                 is what an inlined copy of it contains)
 *   post_splice - every chunk after the bytecode inliner, before the
 *                 JIT: what a test reads to see what the inliner did
 */
struct Chunk;
enum class IntStage { pre_splice, post_splice, N };
typedef std::function<void(const std::string &fn, Chunk &ck)> IntChunkHook;
void int_on(IntStage st, IntChunkHook fn);
void int_off(IntStage st);
void int_stage(IntStage st, const std::string &fn, Chunk &ck);
struct IntHookScope {
    IntStage st;
    IntHookScope(IntStage s, IntChunkHook fn) : st(s) { int_on(s, fn); }
    ~IntHookScope() { int_off(st); }
};

/*
 * THE DECISION ENUMERATOR (plan section 4.2): a heuristic with several
 * LEGAL answers asks here instead of deciding alone. `key` names the
 * decision INSTANCE stably (function + an ordinal, never an address);
 * the answer is `dflt` unless MYLANG_INT_CHOOSE names this key
 * (`key=idx`, several separated by `;` or `,`) with an index below `n`. The
 * runner reads the instances a default run recorded and re-runs the
 * program once per alternative - deterministically, no seeds.
 */
int int_choose(const std::string &key, int n, int dflt);

/* A line about an HONOURED override, written to MYLANG_INT_DUMP at once
 * (appended and flushed, not at exit): a deviation that aborts the
 * process must still say what it applied - those are the runs that need
 * diagnosing. int_choose writes `choose_applied`; a caller may add what
 * the index meant (the allocator: `choose_reg key=... reg=N`). */
void int_applied_note(const std::string &line);

/* The canonical printer's value formats - one per allowed field type. */
void int_put(std::string &o, const char *key, int64_t v);
void int_put(std::string &o, const char *key, bool v);
void int_put(std::string &o, const char *key, const std::string &v);

#define F(t, n) int_put(o, #n, p.n);
#define X(name, desc, fields)                                                 \
    inline void int_event(const IntPay_##name &p)                             \
    {                                                                         \
        std::string o = #name;                                                \
        fields                                                                \
        int_record(IntSite::name, std::move(o));                              \
    }
ML_INT_SITES(X)
#undef X
#undef F

/*
 * A DEFERRAL scope: while one is live, events are BUFFERED; commit()
 * publishes them, and destroying the scope uncommitted DISCARDS them. For
 * a pass that may throw away its own work and redo it - the JIT re-emits a
 * chunk when a bet loses, and the backward goto destroys everything
 * declared after the retry label - so a discarded attempt records nothing.
 * Scopes nest; a commit hands the events to the enclosing one, dropping
 * exact duplicates (one attempt may emit the same op twice - a cold copy).
 */
class IntDefer {
public:
    IntDefer();
    ~IntDefer();
    void commit();
    IntDefer(const IntDefer &) = delete;
    IntDefer &operator=(const IntDefer &) = delete;
private:
    friend void int_record(IntSite s, std::string &&line);
    std::vector<std::pair<IntSite, std::string>> buf;
    IntDefer *prev;
};

#define ML_INT(site, ...) int_event(IntPay_##site{ __VA_ARGS__ })
#define ML_INT_DEFER(name) IntDefer name
#define ML_INT_COMMIT(name) name.commit()
#define ML_INT_FIELD(type, name, init) type name = init;
#define ML_INT_ONLY(...) __VA_ARGS__

#else  /* !INT_TESTS */

#define ML_INT(site, ...) ((void)0)
#define ML_INT_DEFER(name)
#define ML_INT_COMMIT(name) ((void)0)
#define ML_INT_FIELD(type, name, init)
#define ML_INT_ONLY(...)

#endif
