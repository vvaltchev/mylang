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
        "break in while stmt",
        {
            "var i = 0;",
            "while (i < 10) {",
            "   if (i == 5) break;",
            "   i += 1;",
            "}",
            "assert(i == 5);",
        },
    },

    {
        "continue in while stmt",
        {
            "var i = 0;",
            "while (i < 10) {",
            "   if (i == 5) {",
            "       i += 1;",
            "       continue;",
            "       assert(0);",
            "   }",
            "   i += 1;",
            "}",
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

    {
        "assign builtins to vars",
        {
            "var a = len;\n",
            "assert(a(\"hello\") == 5);",
        },
    },

    {
        "assign builtins to consts",
        {
            "const a = len;\n",
            "assert(a(\"hello\") == 5);",
        },
    },

    {
        "rebind builtins is not allowed",
        {
            "len = 5;",
        },
        &typeid(CannotRebindBuiltinEx),
    },

    {
        "vars shadowing builtins are not allowed",
        {
            "{ var len = 5; assert(len == 5); }",
        },
        &typeid(CannotRebindBuiltinEx),
    },

    {
        "consts shadowing builtins are not allowed",
        {
            "{ const len = 5; assert(len == 5); }",
        },
        &typeid(CannotRebindBuiltinEx),
    },

    {
        "undefined variable",
        {
            "assert(a == 1);",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "undefined as ID in CallExpr",
        {
            "a(1);",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "defined() builtin",
        {
            "assert(defined(a) == 0);",
            "assert(defined(len) == 1);",
            "assert(defined(\"blah\") == 1);",
            "assert(defined(defined) == 1);",
            "assert(defined(none) == 1);",
            "",
            "var a;",
            "assert(defined(a) == 1);",
            "var b = 0;",
            "assert(defined(b) == 1);",
            "const c1 = \"val\";",
            "assert(defined(c1) == 1);",
        },
    },

    {
        "simple func",
        {
            "func add(a, b) {",
            "   return a+b;",
            "}",
            "",
            "assert(add(3,4) == 7);",
        },
    },

    {
        "simple func (short syntax)",
        {
            "func add(a, b) => a+b;",
            "assert(add(3,4) == 7);",
        },
    },

    {
        "func accessing globals",
        {
            "var g1 = 34;",
            "func add(a) => g1 + a;",
            "assert(add(1) == 35);",
        },
    },

    {
        "func accessing global consts",
        {
            "const g1 = 34;",
            "func add(a) => g1 + a;",
            "assert(add(1) == 35);",
        },
    },

    {
        "function objects",
        {
            "var f = func (a,b) => a+b;",
            "assert(f(1,2) == 3);",
        },
    },

    {
        "function objects with capture",
        {
            "{",
            "   var local=1;",
            "   var f = func [local] { local+=1; return local; };",
            "   assert(f() == 2);",
            "   assert(f() == 3);",
            "   assert(f() == 4);",
            "   assert(local == 1);",
            "}",
        },
    },

    {
        "functions don't see outer scope except global (short expr)",
        {
            "{",
            "   var g = 1;",
            "   var f = func () => g;",
            "   f();",  // We must fail here
            "}",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "functions don't see outer scope except global (direct return)",
        {
            "{",
            "   var g = 1;",
            "   var f = func () { return g; };",
            "   f();",  // We must fail here
            "}",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "functions don't see outer scope except global (generic return)",
        {
            "{",
            "   var g = 1;",
            "   var f = func () { { return g; } };",
            "   f();",  // We must fail here
            "}",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "function return (direct)",
        {
            "{",
            "   var f = func () { return 123; };",
            "   assert(f() == 123);",
            "}",
        },
    },

    {
        "function return (generic)",
        {
            "{",
            "   var f = func () { { return 123; } };",
            "   assert(f() == 123);",
            "}",
        },
    },

    {
        "Undefined (and unused) variable",
        {
            "a;"
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "Use expressions as callable objects",
        {
            "assert((func (a) => a+1)(2) == 3);",
        },
    },

    {
        "Call function returned by other function in expr",
        {
            "func getfunc(v) {",
            "   var f = func [v] (a) => a+v;",
            "   return f;",
            "}",
            "",
            "assert((getfunc(3))(2) == 5);",
        },
    },

    {
        "String subscript operator",
        {
            "var s=\"abc\";",
            "assert(s[0] == \"a\");",
            "assert(s[1] == \"b\");",
            "assert(s[2] == \"c\");",
        }
    },

    {
        "String subscript operator (neg index)",
        {
            "var s=\"abc\";",
            "assert(s[-1] == \"c\");",
            "assert(s[-2] == \"b\");",
            "assert(s[-3] == \"a\");",
        }
    },

    {
        "String subscript operator, out of bounds",
        {
            "var s=\"abc\";",
            "s[3];",
        },
        &typeid(OutOfBoundsEx),
    },

    {
        "String subscript operator, out of bounds, neg",
        {
            "var s=\"abc\";",
            "s[-4];",
        },
        &typeid(OutOfBoundsEx),
    },

    {
        "String subscript operator, literal string",
        {
            "assert(\"abc\"[1] == \"b\");"
        },
    },

    {
        "Slice operator, regular",
        {
            "const s=\"abc\";",
            "assert(s[1:2] == \"b\");"
        },
    },

    {
        "Slice operator, just start",
        {
            "const s=\"abc\";",
            "assert(s[1:] == \"bc\");"
        },
    },

    {
        "Slice operator, just end",
        {
            "const s=\"abc\";",
            "assert(s[:2] == \"ab\");"
        },
    },

    {
        "Slice operator, neg start, no end",
        {
            "const s=\"abc\";",
            "assert(s[-2:] == \"bc\");"
        },
    },

    {
        "Slice operator, neg start, neg end",
        {
            "const s=\"abc\";",
            "assert(s[-3:-1] == \"ab\");"
        },
    },

    {
        "Slice operator, out of bounds, stop",
        {
            "const s=\"abc\";",
            "assert(s[0:10] == \"abc\");"
        },
    },

    {
        "Slice operator, out of bounds, start",
        {
            "const s=\"abc\";",
            "assert(s[-10:10] == \"abc\");"
        },
    },

    {
        "Slice operator, out of bounds, start and end",
        {
            "const s=\"abc\";",
            "assert(s[10:20] == \"\");"
        },
    },

    {
        "Slice operator, out of bounds, neg start, neg end",
        {
            "const s=\"abc\";",
            "assert(s[-10:-20] == \"\");"
        },
    },

    {
        "Slice operator, out of bounds, start == end",
        {
            "const s=\"abc\";",
            "assert(s[1:1] == \"\");"
        },
    },

    {
        "Slice operator, out of bounds, start > end",
        {
            "const s=\"abc\";",
            "assert(s[2:1] == \"\");"
        },
    },

    {
        "Subscript and splice over slice of string",
        {
            "const s=\"hello world, john!\";",
            "const sub=s[6:11];",
            "assert(sub == \"world\");",
            "assert(len(sub) == 5);",
            "assert(sub[0] == \"w\");",
            "assert(sub[4] == \"d\");",
            "assert(sub[-1] == \"d\");",
            "assert(sub[-100:] == \"world\");",
            "assert(sub[1:] == \"orld\");",
            "assert(sub[:2] == \"wo\");",
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

        root = pBlock(pCtx);
        root->eval(nullptr);

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
    size_t pass_count = 0;

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
    cout << "Tests passed: " << pass_count << "/" << tests.size() << " ";


    if (pass_count != tests.size()) {
        cout << "[ FAIL ]" << endl;
        exit(1);
    }

    cout << "[ PASS ]" << endl;
    exit(0);
}

#else

void run_tests()
{
    cout << "Tests NOT compiled in. Build with TESTS=1" << endl;
    exit(1);
}

#endif
