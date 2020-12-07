/* SPDX-License-Identifier: BSD-2-Clause */

#include <iostream>
#include <cstdlib>
using namespace std;

#ifdef TESTS

#include "parser.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"

#include <typeinfo>
#include <vector>

struct test {

    const char *name;
    vector<const char *> source;
    const type_info *ex = nullptr;
};

static const vector<test> tests =
{
    {
        "variable decl",
        {
            "var a = 1;",
            "a += 1;",
            "assert(a == 2);",
        },
    },

    {
        "const decl",
        {
            "const a = 1;",
            "var b = a + 1;",
            "assert(b == 2);",
        },
    },

    {
        "len() with wrong type",
        {
            "len(3);",
        },
        &typeid(TypeErrorEx),
    }
};

static void
dump_expected_ex(const type_info *ex, const type_info *got)
{
    cout << "  Expected EX: " << (ex ? ex->name() : "<none>") << endl;
    cout << "  Got EX     : " << (got ? got->name() : "<none>") << endl;
}

static bool
check(const test &t)
{
    vector<Tok> tokens;
    unique_ptr<Construct> root;

    try {

        for (size_t i = 0; i < t.source.size(); i++)
            lexer(t.source[i], i+1, tokens);

        ParseContext pCtx(TokenStream(tokens), true /* const eval */);
        EvalContext eCtx;

        root = pBlock(pCtx);
        root->eval(&eCtx);

    } catch (const Exception &e) {

        if (!t.ex || &typeid(e) != t.ex) {
            dump_expected_ex(t.ex, &typeid(e));
            return false;
        }

        return true;
    }

    if (t.ex) {

        dump_expected_ex(t.ex, nullptr);

        if (root) {
            cout << "  Syntax tree:" << endl;
            root->serialize(cout, 2);
            cout << endl;
        }

        return false;
    }

    return true;
}

static void
dump_test_source(const test &t)
{
    cout << "  Source: ";

    if (t.source.size() == 1) {

        cout << t.source[0] << endl;

    } else {

        cout << endl;

        for (const auto &e : t.source) {
            cout << "    " << e << endl;
        }
    }
}

void run_tests()
{
    int pass_count = 0;

    for (const auto &test : tests) {

        cout << "[ RUN  ] " << test.name << endl;

        if (check(test)) {

            cout << "[ PASS ]";
            pass_count++;

        } else {

            dump_test_source(test);
            cout << "[ FAIL ]";
        }

        cout << endl << endl;
    }

    cout << "SUMMARY" << endl;
    cout << "===========================================" << endl;
    cout << "Tests passed: " << pass_count << "/" << tests.size() << endl;
}

#else

void run_tests()
{
    cout << "Tests NOT compiled in. Build with TESTS=1" << endl;
    exit(1);
}

#endif
