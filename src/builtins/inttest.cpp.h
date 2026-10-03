/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * This is NOT a header file. It's a C++ file in the form of a header, just
 * because it's faster to compile it this way (see types.cpp).
 *
 * The INTRUSIVE-TEST builtins (#107, plans/intrusive-tests.md) - registered
 * ONLY in an INT_TESTS build, so in any other build `int_hits` is a plain
 * undefined name. They read the event log ML_INT fills (inttest.h).
 */

#ifdef INT_TESTS

#include "../inttest.h"

/* The site an `int_*` builtin names: a string argument that must be a row
 * of intsites.h. A typo is an error, never a silent zero. */
static IntSite int_builtin_site(const ArgLocs *exprList, const EvalValue *args,
                                size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    const ArgLoc *arg = exprList->arg(0);
    if (!args[0].is<SharedStr>())
        throw TypeErrorEx("Expected a site name (str)", arg->start, arg->end);
    const std::string name(args[0].get<SharedStr>().get_view());
    const int i = int_site_by_name(name);
    if (i < 0)
        throw InvalidArgumentEx(arg->start, arg->end);
    /* the census counts a site as CHECKED only through a test's read */
    int_note_query(static_cast<IntSite>(i));
    return static_cast<IntSite>(i);
}

/* int_hits(site): how many events `site` recorded in this process so far -
 * compile-time ones (the inliner ran before the program did) included. */
EvalValue builtin_int_hits(EvalContext *ctx, const ArgLocs *exprList,
                           const EvalValue *args, size_t n)
{
    return static_cast<int_type>(int_hits(int_builtin_site(exprList, args, n)));
}

/* int_events(site): the events themselves, in program order, each in its
 * canonical one-line form (`inline_ast engine="expr" callee="f" ...`). */
EvalValue builtin_int_events(EvalContext *ctx, const ArgLocs *exprList,
                             const EvalValue *args, size_t n)
{
    const IntSite s = int_builtin_site(exprList, args, n);
    SharedArrayObj::vec_type vec;
    for (const std::string &e : int_events(s))
        vec.emplace_back(SharedStr(std::string(e)), false);
    return SharedArrayObj(std::move(vec));
}

#endif  /* INT_TESTS */
