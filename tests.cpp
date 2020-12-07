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
            "var a=1;",
            "a += 1;",
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

        if (typeid(e) != *t.ex) {

            cout << "TEST FAIL\n";
            cout << "Expected exception: " << t.ex->name()
                 << ", got: " << typeid(e).name() << endl;

            return false;
        }

        return true;
    }

    if (t.ex) {

        cout << "TEST FAIL\n";
        cout << "Expected exception: " << t.ex->name() << ", got nothing" << endl;

        if (root) {
            cout << "Syntax tree" << endl;
            cout << "-------------------------------" << endl;
            cout << *root << endl;
        }

        return false;
    }

    return true;
}

void run_tests()
{
    for (const auto &test : tests) {

        cout << "Test: '" << test.name << "'" << endl;
        cout << "---------------------------------" << endl;

        cout << "Source: ";

        if (test.source.size() == 1) {

            cout << test.source[0] << endl;

        } else {

            cout << endl;

            for (const auto &e : test.source) {
                cout << "   " << e << endl;
            }
        }

        if (test.ex)
            cout << "Expected Ex: " << test.ex->name() << endl;

        check(test);
        cout << "---------------------------------" << endl;
        cout << "TEST PASS\n\n";
    }
}

#else

void run_tests()
{
    cout << "Tests NOT compiled in. Build with TESTS=1" << endl;
    exit(1);
}

#endif
