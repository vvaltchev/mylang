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
            "assert(a == 1);",
        },
    },

    {
        "const decl",
        {
            "const a = 1;",
            "assert(a == 1);",
        },
    },

    {
        "expr with priority",
        {
            "var a = 2 * -3 + 1 < 0 && 2 >= 1;"
            "var b = (((2 * -3) + 1) < 0) && (2 >= 1);"
            "assert(a == 1);"
            "assert(a == b);"
        },
    },

    {
        "if stmt",
        {
            "var a = 1;",
            "if (a >= 1) {",
            "   assert(1);",
            "} else {",
            "   assert(0);",
            "}",
        },
    },

    {
        "if stmt with single stmts",
        {
            "var a = 1;",
            "if (a >= 1)",
            "   assert(1);",
            "else",
            "   assert(0);",
        },
    },

    {
        "if stmt with empty stmts",
        {
            "var a = 1;",
            "if (a) {",
            "} else {",
            "}",
        },
    },

    {
        "if stmt with empty stmts 2",
        {
            "var a = 1;",
            "if (a)",
            "else {",
            "}",
        },
    },

    {
        "if stmt with empty stmts 3",
        {
            "var a = 1;",
            "if (a)",
            "else",
            "",
        },
    },

    {
        "if stmt with no stmts",
        {
            "var a = 1;",
            "if (a);",
        },
    },

    {
        "assign as expr",
        {
            "var a = 1;",
            "assert((a = 3) == 3);",
            "assert(a == 3);",
        },
    },

    {
        "plus-assign as expr",
        {
            "var a = 1;",
            "assert((a += 3) == 4);",
            "assert(a == 4);",
        },
    },

    {
        "while stmt",
        {
            "var i = 0;",
            "while (i < 10) {",
            "   i += 1;",
            "}",
            "assert(i == 10);",
        },
    },

    {
        "while stmt, no brackets",
        {
            "var i = 0;",
            "while (i < 10)",
            "   i += 1;",
            "assert(i == 10);",
        },
    },

    {
        "while stmt, no body",
        {
            "var i = 0;",
            "while ((i += 1) < 10);",
            "assert(i == 10);",
        },
    },

    {
        "scope of variables",
        {
            "var a = 1;",
            "assert(a == 1);",
            "{",
            "   assert(a == 1);",
            "   var a = 2;",        // shadowing
            "   assert(a == 2);",
            "}",
            "assert(a == 1);",
        },
    },

    {
        "const shadowing a variable",
        {
            "var a = 1;",
            "assert(a == 1);",
            "{",
            "   const a = 2;",
            "   assert(a == 2);",
            "}",
            "assert(a == 1);",
        },
    },

    {
        "variable re-decl in the same scope fails",
        {
            "var a = 1; var a = 2;",
        },
        &typeid(AlreadyDefinedEx),
    },

    {
        "const re-decl in the same scope fails",
        {
            "const a = 1; const a = 2;",
        },
        &typeid(CannotRebindConstEx),
    },

    {
        "const re-decl in nested scope fails",
        {
            "const a = 1; { const a = 2; }",
        },
        &typeid(CannotRebindConstEx),
    },

    {
        "variable shadowing a const fails",
        {
            "const a = 1;",
            "assert(a == 1);",
            "{",
            "   var a = 2;",
            "   assert(a == 2);",
            "}",
            "assert(a == 1);",
        },
        &typeid(CannotRebindConstEx),
    },

    {
        "len() works with literal",
        {
            "assert(len(\"hello\") == 5);"
        },
    },

    {
        "len() works with const",
        {
            "const a = \"hello\";",
            "assert(len(a) == 5);"
        },
    },

    {
        "len() works with variable",
        {
            "var a = \"hello\";",
            "assert(len(a) == 5);"
        },
    },

    {
        "len() with wrong type fails",
        {
            "len(3);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "literal str concat",
        {
            "assert(\"hello\" + \" world\" == \"hello world\");"
        },
    },

    {
        "var str concat",
        {
            "var a = \"hello\";",
            "assert(a + \" world\" == \"hello world\");"
        },
    },

    {
        "var str += concat",
        {
            "var a = \"hello\";",
            "a += \" world\";",
            "assert(a == \"hello world\");"
        },
    },

    {
        "var str concat with integer",
        {
            "var a = \"hello\";",
            "assert(a + 2 == \"hello2\");"
        },
    },

    {
        "str() builtin",
        {
            "var a = str(3);",
            "assert(a == \"3\");",
        },
    },

    {
        "string repeat",
        {
            "assert(\"a\" * 3 == \"aaa\");",
        },
    },

    {
        "invalid string operators",
        {
            "\"a\" - 3;",
        },
        &typeid(TypeErrorEx),
    },

    {
        "var decl without init",
        {
            "var a;",
        },
    },

    {
        "var decl without init is none",
        {
            "var a;",
            "assert(a == none);",
        },
    },

    {
        "assign none to variable",
        {
            "var a = 3;",
            "assert(a == 3);",
            "assert(a != none);",
            "a = none;",
            "assert(a == none);",
        },
    },

    {
        "compare different types with !=",
        {
            "assert(\"1\" != 1);",
            "assert(\"1\" != 0);",
            "assert(\"1\" != none);",
            "assert(1 != \"1\");",
            "assert(1 != \"0\");",
            "assert(1 != none);",
        },
    },

    {
        "compare different types with ==",
        {
            "assert(!(\"1\" == 1));",
            "assert(!(\"1\" == 0));",
            "assert(!(\"1\" == none));",
            "assert(!(1 == \"1\"));",
            "assert(!(1 == \"0\"));",
            "assert(!(1 == none));",
        },
    },

    {
        "none is none",
        {
            "assert(none == none);",
            "assert(!(none != none));",
        },
    },
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
