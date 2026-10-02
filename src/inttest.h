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
 *   ML_INT_ONLY(stmts...)       statements that exist only in an INT build
 *                               (keeping an INT_FIELD up to date); like
 *                               ML_INT they may only READ the code's state
 */

#pragma once

#ifdef INT_TESTS

#include <cstdint>
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

/* The core (inttest.cpp). */
void int_record(IntSite s, std::string &&line);
const char *int_site_name(IntSite s);
const char *int_site_desc(IntSite s);
int int_site_by_name(const std::string &name);      /* -1 when unknown */
uint64_t int_hits(IntSite s);
const std::vector<std::string> &int_events(IntSite s);
void int_reset();                                   /* every site */

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

#define ML_INT(site, ...) int_event(IntPay_##site{ __VA_ARGS__ })
#define ML_INT_FIELD(type, name, init) type name = init;
#define ML_INT_ONLY(...) __VA_ARGS__

#else  /* !INT_TESTS */

#define ML_INT(site, ...) ((void)0)
#define ML_INT_FIELD(type, name, init)
#define ML_INT_ONLY(...)

#endif
