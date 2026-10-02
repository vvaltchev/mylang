/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * THE INTRUSIVE-TEST SITE REGISTRY (#107, plans/intrusive-tests.md).
 *
 * The ONE list of every instrumented point. Each row is
 *
 *     X(name, "what an event here means", F(type, field) F(type, field)...)
 *
 * and inttest.h expands it into the site enum, a TYPED payload struct per
 * site (`IntPay_<name>`, filled positionally by `ML_INT(name, ...)`, so a
 * wrong field count or type is a compile error) and the canonical printer.
 *
 * Field types are restricted to `int64_t`, `bool` and `std::string`: a
 * printed event must be a deterministic function of the program, so no
 * pointer, address or hash ever goes in a payload.
 *
 * A row nobody's tests reach is a failure (tests/int_run.py's site census):
 * add a site together with the test that exercises it.
 */

#pragma once

#define ML_INT_SITES(X)                                                       \
    /* The AST inliner spliced a call: `engine` is expr / block / tail,    */ \
    /* `caller` the function whose body holds the call (`main` at the top  */ \
    /* level), `callee` the spliced function, line:col the call site.      */ \
    /* Display names, so a template instance reads as its base.            */ \
    X(inline_ast, "the AST inliner spliced a call",                           \
      F(std::string, engine) F(std::string, caller)                           \
      F(std::string, callee) F(int64_t, line) F(int64_t, col))
