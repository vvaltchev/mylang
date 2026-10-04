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

/* int_live(kind): how many runtime heap objects of `kind` ("str", "arr",
 * "dict", "struct", "func", "exc") are alive right now - the object census
 * (#107 P4), so a test can assert that what it built is freed. */
EvalValue builtin_int_live(EvalContext *ctx, const ArgLocs *exprList,
                           const EvalValue *args, size_t n)
{
    if (n != 1)
        throw InvalidNumberOfArgsEx(exprList->start, exprList->end);
    const ArgLoc *arg = exprList->arg(0);
    if (!args[0].is<SharedStr>())
        throw TypeErrorEx("Expected an object kind (str)", arg->start,
                          arg->end);
    const long long c =
        int_live_count(std::string(args[0].get<SharedStr>().get_view()));
    if (c < 0)
        throw InvalidArgumentEx(arg->start, arg->end);
    return static_cast<int_type>(c);
}

/* int_events(site): the events themselves, each in its canonical one-line
 * form (`inline_ast engine="expr" callee="f" ...`), SORTED. Not program
 * order: some passes walk a pointer-keyed map (the bytecode splice runs
 * over chunks in an unordered_map), so their order would differ from run
 * to run - and every answer this surface gives must be deterministic
 * (plans/intrusive-tests.md, principle 1). */
EvalValue builtin_int_events(EvalContext *ctx, const ArgLocs *exprList,
                             const EvalValue *args, size_t n)
{
    const IntSite s = int_builtin_site(exprList, args, n);
    std::vector<std::string> sorted = int_events(s);
    std::sort(sorted.begin(), sorted.end());
    SharedArrayObj::vec_type vec;
    for (std::string &e : sorted)
        vec.emplace_back(SharedStr(std::move(e)), false);
    return SharedArrayObj(std::move(vec));
}

#endif  /* INT_TESTS */
