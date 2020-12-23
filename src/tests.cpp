/* SPDX-License-Identifier: BSD-2-Clause */

#include <iostream>
#include <iomanip>
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
            "var a = 2 * -3 + 1 < 0 && 2 >= 1;",
            "var b = (((2 * -3) + 1) < 0) && (2 >= 1);",
            "assert(a == 1);",
            "assert(a == b);",
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
            "assert(len(\"hello\") == 5);",
        },
    },

    {
        "len() works with const",
        {
            "const a = \"hello\";",
            "assert(len(a) == 5);",
        },
    },

    {
        "len() works with variable",
        {
            "var a = \"hello\";",
            "assert(len(a) == 5);",
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
            "assert(\"hello\" + \" world\" == \"hello world\");",
        },
    },

    {
        "var str concat",
        {
            "var a = \"hello\";",
            "assert(a + \" world\" == \"hello world\");",
        },
    },

    {
        "var str += concat",
        {
            "var a = \"hello\";",
            "a += \" world\";",
            "assert(a == \"hello world\");",
        },
    },

    {
        "var str concat with integer",
        {
            "var a = \"hello\";",
            "assert(a + 2 == \"hello2\");",
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
            "a;",
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
            "assert(\"abc\"[1] == \"b\");",
        },
    },

    {
        "Slice operator, regular",
        {
            "const s=\"abc\";",
            "assert(s[1:2] == \"b\");",
        },
    },

    {
        "Slice operator, just start",
        {
            "const s=\"abc\";",
            "assert(s[1:] == \"bc\");",
        },
    },

    {
        "Slice operator, just end",
        {
            "const s=\"abc\";",
            "assert(s[:2] == \"ab\");",
        },
    },

    {
        "Slice operator, neg start, no end",
        {
            "const s=\"abc\";",
            "assert(s[-2:] == \"bc\");",
        },
    },

    {
        "Slice operator, neg start, neg end",
        {
            "const s=\"abc\";",
            "assert(s[-3:-1] == \"ab\");",
        },
    },

    {
        "Slice operator, out of bounds, stop",
        {
            "const s=\"abc\";",
            "assert(s[0:10] == \"abc\");",
        },
    },

    {
        "Slice operator, out of bounds, start",
        {
            "const s=\"abc\";",
            "assert(s[-10:10] == \"abc\");",
        },
    },

    {
        "Slice operator, out of bounds, start and end",
        {
            "const s=\"abc\";",
            "assert(s[10:20] == \"\");",
        },
    },

    {
        "Slice operator, out of bounds, neg start, neg end",
        {
            "const s=\"abc\";",
            "assert(s[-10:-20] == \"\");",
        },
    },

    {
        "Slice operator, out of bounds, start == end",
        {
            "const s=\"abc\";",
            "assert(s[1:1] == \"\");",
        },
    },

    {
        "Slice operator, out of bounds, start > end",
        {
            "const s=\"abc\";",
            "assert(s[2:1] == \"\");",
        },
    },

    {
        "Subscript and slice over slice of string",
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

    {
        "Append to slice of string",
        {
            "var s=\"hello world\";",
            "var sub = s[:5];",
            "assert(sub == \"hello\");",
            "sub += \" john\";",
            "assert(s == \"hello world\");",
            "assert(sub == \"hello john\");",
        },
    },

    {
        "Simple array",
        {
            "var s = [1,2,3,\"a\",\"b\",\"c\"];",
            "assert(s[0] == 1);",
            "assert(s[-1] == \"c\");",
        },
    },

    {
        "Simple array, const",
        {
            "const s = [1,2,3,\"a\",\"b\",\"c\"];",
            "assert(s[0] == 1);",
            "assert(s[-1] == \"c\");",
        },
    },

    {
        "Array slices",
        {
            "var s = [1,2,3,\"a\",\"b\",\"c\"];",
            "assert(s[1:3] == [2,3]);",
            "assert(s[-2:] == [\"b\", \"c\"]);",
        },
    },

    {
        "Slices of array slices",
        {
            "var s = [1,2,3,\"a\",\"b\",\"c\"];",
            "var sub = s[1:5];",
            "assert(sub == [2,3,\"a\",\"b\"]);",
            "assert(sub[0] == 2);",
            "assert(sub[-1] == \"b\");",
            "assert(sub[2:] == [\"a\", \"b\"]);",
            "assert(sub[-3:] == [3,\"a\",\"b\"]);",
            "assert(sub[1:2] == [3]);",
        },
    },

    {
        "Array append",
        {
            "var s = [1,2,3];",
            "s += [4];",
            "assert(s == [1,2,3,4]);",
        },
    },

    {
        "Append in slice of array",
        {
            "var s = [1,2,3];",
            "var sub = s[1:];",
            "sub += [99,100];",
            "assert(sub == [2,3,99,100]);",
            "assert(s == [1,2,3]);",
        },
    },

    {
        "Clone function objects",
        {
            "func genfunc(v) => func [v] { v+=1; return v; };",
            "var f = genfunc(0);",
            "assert(f() == 1);",
            "assert(f() == 2);",
            "assert(f() == 3);",
            "",
            "var g = f;",
            "assert(g() == 4);", // `f` and `g` point to the same func object
            "assert(g() == 5);", // `f` and `g` point to the same func object
            "assert(intptr(g) == intptr(f));",
            "",
            "g = clone(g);",     // now, the counters will diverge
            "assert(intptr(g) != intptr(f));",
            "assert(g() == 6);",
            "assert(g() == 7);",
            "assert(g() == 8);",
            "",
            "assert(f() == 6);", // proof that g != f
        },
    },

    {
        "Multi-dimentional arrays",
        {
            "var arr = [[11, 22], 3, 4];",
            "assert(arr[0] == [11,22]);",
            "assert(arr[0][0] == 11);",
            "assert(arr[0][1] == 22);",
            "assert(len(arr[0]) == 2);",
            "assert(len(arr) == 3);",
        },
    },

    {
        "Subscript of literal arrays",
        {
            "assert([11,22,33][0] == 11);",
            "assert([11,22,33][1] == 22);",
        },
    },

    {
        "Subscript of arrays returned by funcs",
        {
            "func f1 => [11,22,33];",
            "assert(f1() == [11,22,33]);",
            "assert(f1()[0] == 11);",
            "assert(f1()[1] == 22);",
        },
    },

    {
        "Slices of arrays returned by funcs",
        {
            "func f1 => [11,22,33,44];",
            "assert(f1() == [11,22,33,44]);",
            "assert(f1()[2:] == [33,44]);",
        },
    },

    {
        "Call funcs returned by funcs, directly",
        {
            "func get_adder(v) => func [v] => v+1;",
            "assert(get_adder(1)() == 2);",
            "assert(get_adder(25)() == 26);",
            "assert(get_adder(1)() == 2);",
        },
    },

    {
        "Array of functions",
        {
            "var arr = [",
            "   func (v) => v+1,",
            "   func (v) => v+2",
            "];",
            "",
            "assert(arr[0](1) == 2);",
            "assert(arr[1](1) == 3);",
        },
    },

    {
        "Undef builtin",
        {
            "var a = \"hello\";",
            "assert(a == \"hello\");",
            "assert(undef(a));",
            "assert(a == \"hello\");",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "Undef builtin, undefined var",
        {
            "assert(!undef(abc));",
        }
    },

    {
        "undef() builtin, variable shadowing another",
        {
            "var a = 42;",
            "{",
            "   var a = 10;",
            "   assert(a == 10);",
            "   undef(a);",
            "}",
            "assert(a == 42);",
        },
    },

    {
        "split() builtin, simple case",
        {
            "var s = \"a->b->c\";",
            "var arr = split(s, \"->\");",
            "assert(arr == [\"a\",\"b\",\"c\"]);",
        },
    },

    {
        "split() builtin, first elem empty",
        {
            "var s = \"->a->b->c\";",
            "var arr = split(s, \"->\");",
            "assert(arr == [\"\",\"a\",\"b\",\"c\"]);",
        },
    },

    {
        "split() builtin, last elem empty",
        {
            "var s = \"a->b->c->\";",
            "var arr = split(s, \"->\");",
            "assert(arr == [\"a\",\"b\",\"c\",\"\"]);",
        },
    },

    {
        "split() builtin, no delim in string",
        {
            "var s = \"abc\";",
            "var arr = split(s, \"->\");",
            "assert(arr == [\"abc\"]);",
        },
    },

    {
        "split() builtin, const case",
        {
            "const s = \"a->b->c\";",
            "const arr = split(s, \"->\");",
            "assert(arr == [\"a\",\"b\",\"c\"]);",
        },
    },

    {
        "join() builtin, base case",
        {
            "var a = [\"a\", \"b\"];",
            "var s = join(a, \",\");",
            "assert(s == \"a,b\");",
        },
    },

    {
        "join() builtin, const case",
        {
            "const a = [\"a\", \"b\"];",
            "const s = join(a, \",\");",
            "assert(s == \"a,b\");",
        },
    },

    {
        "join() builtin, single elem case",
        {
            "var a = [\"a\"];",
            "var s = join(a, \",\");",
            "assert(s == \"a\");",
        },
    },

    {
        "join() builtin, no elems case",
        {
            "var a = [];",
            "var s = join(a, \",\");",
            "assert(s == \"\");",
        },
    },

    {
        "Array: modify elements of array with slices",
        {
            "var s = [1,2,3,4];",
            "var sub = s[1:3];",
            "assert(sub == [2,3]);",
            "assert(intptr(s) == intptr(sub));", // both vars must point to the same obj
            "s[1] = 20;",                        // change an element of `s`
            "assert(intptr(s) != intptr(sub));", // `s` now must point to a different obj
            "assert(s == [1,20,3,4]);",          // check that now `s` is updated
            "assert(sub == [2,3]);",             // check that `sub` contains the old value
        },
    },

    {
        "Array: modify elements of array WITHOUT slices",
        {
            "var arr = [1,2,3];",
            "var oldptr = intptr(arr);",
            "arr[1] = 99;",
            "assert(arr == [1,99,3]);",
            "assert(intptr(arr) == oldptr);",
        },
    },

    {
        "Array: non-slice assign has reference semantics",
        {
            "var a = [1,2,3];",
            "var b = a;",
            "assert(intptr(a) == intptr(b));",
            "a[0] = 99;",
            "assert(a == [99,2,3]);",
            "assert(b == [99,2,3]);",
        },
    },

    {
        "Array: clone",
        {
            "var a = [1,2,3];",
            "var b = clone(a);",
            "assert(intptr(a) != intptr(b));",
            "a[1] = 99;",
            "assert(a == [1,99,3]);",
            "assert(b == [1,2,3]);",
        },
    },

    {
        "Split string, char by char",
        {
            "assert(split(\"abc\", \"\") == [\"a\",\"b\",\"c\"]);",
        },
    },

    {
        "Builtin splitlines()",
        {
            "assert(splitlines(\"\") == []);",
            "assert(splitlines(\"a\") == [\"a\"]);",
            "assert(splitlines(\"a\\n\") == [\"a\",\"\"]);",
            "assert(splitlines(\"a\\r\") == [\"a\",\"\"]);",
            "assert(splitlines(\"a\\r\\n\") == [\"a\",\"\"]);",
            "assert(splitlines(\"a\\nb\") == [\"a\",\"b\"]);",
            "assert(splitlines(\"a\\rb\") == [\"a\",\"b\"]);",
            "assert(splitlines(\"a\\r\\nb\") == [\"a\",\"b\"]);",
            "assert(splitlines(\"\\nb\") == [\"\",\"b\"]);",
            "assert(splitlines(\"\\rb\") == [\"\",\"b\"]);",
            "assert(splitlines(\"\\r\\nb\") == [\"\",\"b\"]);",
        },
    },

    {
        "Builtins chr() and ord()",
        {
            "assert(ord(\"A\") == 65);",
            "assert(chr(65) == \"A\");",
            "var i = 0;",
            "while (i < 256) {",
            "   assert(ord(chr(i)) == i);",
            "   i += 1;",
            "}",
        },
    },

    {
        "Min and Max builtins",
        {
            "assert(min(1,2) == 1);",
            "assert(max(1,2) == 2);",
            "assert(min(34,52,3) == 3);",
            "assert(max(34,52,3) == 52);",
            "const ar = [34];",
            "assert(min(ar) == 34);",
            "assert(max(ar) == 34);",
            "assert(min([]) == none);",
            "assert(max([]) == none);",
            "const ar2 = [34, 52, 3];",
            "assert(min(ar2) == 3);",
            "assert(max(ar2) == 52);",
        },
    },

    {
        "String as boolean",
        {
            "if (\"\") assert(0); else assert(1);",
            "if (\"a\") assert(1); else assert(0);",
        },
    },

    {
        "Builtin array(N)",
        {
            "assert(array(0) == []);",
            "assert(array(3) == [none,none,none]);",
        },
    },

    {
        "Builtin append() (or push())",
        {
            "var arr = [1,2,3];",
            "assert(arr == [1,2,3]);",
            "append(arr, 99);",
            "assert(arr == [1,2,3,99]);",
            "var s = arr[2:];",
            "assert(s == [3,99]);",
            "assert(append(s, 100) == [3,99,100]);",
            "assert(s == [3,99,100]);",
            "assert(arr == [1,2,3,99]);",
        },
    },

    {
        "Builtin pop(), base case",
        {
            "var arr = [1,2,3];",
            "assert(arr == [1,2,3]);",
            "var ptr = arr;",
            "var e = pop(arr);",
            "assert(e == 3);",
            "assert(arr == [1,2]);",
            "assert(intptr(arr) == intptr(ptr));",
        },
    },

    {
        "Builtin pop(), slices",
        {
            "var arr = [1,2,3];",
            "var s = arr[1:];",
            "assert(intptr(arr) == intptr(s));",
            "assert(pop(s) == 3);",
            "assert(s == [2]);",
            "assert(intptr(arr) == intptr(s));", // The slice still shares the same data
            "assert(arr == [1,2,3]);",
        },
    },

    {
        "Builtin pop(), slices (2)",
        {
            "var arr = [1,2,3];",
            "var s = arr[1:];",
            "assert(s == [2,3]);",
            "assert(intptr(arr) == intptr(s));",
            "assert(pop(arr) == 3);",
            "assert(intptr(arr) != intptr(s));", // The slice has its own copy now
            "assert(arr == [1,2]);",
            "assert(s == [2,3]);",
        },
    },

    {
        "Append() does not work on temp objects",
        {
            "append([1,2,3], 4);",
        },
        &typeid(NotLValueEx),
    },

    {
        "Pop() does not work on temp objects",
        {
            "pop([1,2,3]);",
        },
        &typeid(NotLValueEx),
    },

    {
        "Builtin top()",
        {
            "assert(top([1,2,3]) == 3);",
            "assert(top([1]) == 1);",
        },
    },

    {
        "Exceptions, uncaught",
        {
            "var a=3; append(a, 4);",
        },
        &typeid(TypeErrorEx)
    },

    {
        "Exceptions, single catch TypeErrorEx",
        {
            "var c = 0;",
            "try {",
            "   var t = 3;",
            "   append(t, 4);",
            "   assert(0);",     // We should NEVER get here
            "} catch (TypeErrorEx) {",
            "   c = 1;",
            "}",
            "assert(c == 1);",
        },
    },

    {
        "Exceptions, single catch DivisionByZeroEx",
        {
            "var c = 0;",
            "try {",
            "   var t = 3;",
            "   var d = 0;",
            "   print(t/d);",
            "   assert(0);",     // We should NEVER get here
            "} catch (DivisionByZeroEx) {",
            "   c = 1;",
            "}",
            "assert(c == 1);",
        },
    },

    {
        "Exceptions, single catch other ex type",
        {
            "var c = 0;",
            "try {",
            "   var t = 3;",
            "   var d = 0;",
            "   print(t/d);",
            "   assert(0);",     // We should NEVER get here
            "} catch (TypeErrorEx) {",
            "   c = 1;",         // We should NEVER get here
            "}",
            "assert(0);",
        },
        &typeid(DivisionByZeroEx),
    },

    {
        "Exceptions, multiple catch, ex: DivisionByZeroEx",
        {
            "var c = 0;",
            "try {",
            "   var t = 3;",
            "   var d = 0;",
            "   print(t/d);",
            "   assert(0);",     // We should NEVER get here
            "} catch (TypeErrorEx) {",
            "   assert(0);",     // We should NEVER get here
            "} catch (DivisionByZeroEx) {",
            "   c = 1;",
            "}",
            "assert(c == 1);",
        },
    },

    {
        "Exceptions, multiple catch, ex: TypeErrorEx",
        {
            "var c = 0;",
            "try {",
            "   var t = 3;",
            "   append(t, 34);",
            "   assert(0);",     // We should NEVER get here
            "} catch (TypeErrorEx) {",
            "   c = 1;",
            "} catch (DivisionByZeroEx) {",
            "   assert(0);",     // We should NEVER get here
            "}",
            "assert(c == 1);",
        },
    },

    {
        "Nested try-catch blocks, catch in outer block",
        {
            "var c = 0;",
            "try {",
            "   try {",
            "       var t = 3;",
            "       append(t, 34);",
            "   } catch (DivisionByZeroEx) {",
            "       assert(0);",    // We should NEVER get here
            "   }",
            "} catch (TypeErrorEx) {",
            "   c = 1;",
            "}",
            "assert(c == 1);",
        },
    },

    {
        "Finally, without catch",
        {
            "var c, f = 0;",
            "try {",
            "   try {",
            "       var a=3; append(a, 10);",
            "       assert(0);",     // We should NEVER get here
            "   } finally {",
            "       f = 1;",
            "   }",
            "} catch (TypeErrorEx) {",
            "   c=1;",
            "}",
            "assert(f == 1);",
            "assert(c == 1);",
        },
    },

    {
        "Finally after catch",
        {
            "var c, f = 0;",
            "try {",
            "   var a=3; append(a, 10);",
            "} catch (TypeErrorEx) {",
            "   c = 1;",
            "} finally {",
            "   assert(c == 1);",
            "   f = 1;",
            "}",
            "assert(f == 1);",
        },
    },

    {
        "Finally gets executed in the no-exception case",
        {
            "var f = 0;",
            "try {",
            "   assert(1);",
            "} finally {",
            "   f = 1;",
            "}",
            "assert(f == 1);",
        }
    },

    {
        "Finally gets executed in case of return",
        {
            "var g = 0;",
            "func myfunc {",
            "   try {",
            "       return 42;",
            "   } finally {",
            "       g = 1;",
            "   }",
            "}",
            "var r = myfunc();",
            "assert(r == 42);",
            "assert(g == 1);",
        },
    },

    {
        "Catch anything: TypeErrorEx",
        {
            "var c = 0;",
            "try {",
            "   var a=3; append(a, 4);",
            "} catch {",
            "   c = 1;",
            "}",
            "assert(c == 1);",
        },
    },

    {
        "Catch single ex + anything: catch anything runs",
        {
            "var c1, c2 = 0;",
            "try {",
            "   var a=3; append(a, 4);",
            "} catch (DivisionByZeroEx) {",
            "   c1 = 1;",
            "} catch {",
            "   c2 = 1;",
            "}",
            "assert(c1 == 0);",
            "assert(c2 == 1);",
        },
    },

    {
        "Catch single ex + anything: single catch runs",
        {
            "var c1, c2 = 0;",
            "try {",
            "   var a=3; append(a, 4);",
            "} catch (TypeErrorEx) {",
            "   c1 = 1;",
            "} catch {",
            "   c2 = 1;",
            "}",
            "assert(c1 == 1);",
            "assert(c2 == 0);",
        },
    },

    {
        "Rethrow",
        {
            "var c1, c2 = 0;",
            "try {",
            "   try {",
            "       var a=3; append(a, 4);",
            "   } catch {",
            "       c1 = 1;",
            "       rethrow;",
            "   }",
            "} catch {",
            "   c2 = 1;",
            "}",
            "assert(c1 == 1);",
            "assert(c2 == 1);",
        },
    },

    {
        "Throw (custom) exception",
        {
            "var c = 0;",
            "try {",
            "   throw exception(\"myerr\");",
            "} catch (myerr) {",
            "   c=1;",
            "}",
            "assert(c == 1);",
        },
    },

    {
        "Throw (custom) exception with data",
        {
            "var c = 0;",
            "try {",
            "   throw exception(\"myerr\", 1234);",
            "} catch (myerr as e) {",
            "   c = exdata(e);",
            "}",
            "assert(c == 1234);",
        },
    },

    {
        "Re-throw (custom) exception with data",
        {
            "var c1, c2 = 0;",
            "try {",
            "   try {",
            "       throw ex(\"myerr\", 1234);",
            "   } catch (myerr as e1) {",
            "       c1 = exdata(e1);",
            "       rethrow;",
            "   }",
            "} catch (myerr as e2) {",
            "   c2 = exdata(e2);",
            "}",
            "assert(c1 == 1234);",
            "assert(c2 == 1234);",
        },
    },

    {
        "Erase 1st element, no slice",
        {
            "var a = [1,2,3];",
            "erase(a, 0);",
            "assert(a == [2,3]);",
        },
    },

    {
        "Erase last element, no slice",
        {
            "var a = [1,2,3];",
            "erase(a, len(a)-1);",
            "assert(a == [1,2]);",
        },
    },

    {
        "Erase middle element, no slice",
        {
            "var a = [1,2,3];",
            "erase(a, 1);",
            "assert(a == [1,3]);",
        },
    },

    {
        "Erase 1st elem, slice",
        {
            "var a = [1,2,3,4,5];",
            "var s = a[1:4];",
            "assert(s == [2,3,4]);",
            "assert(intptr(a) == intptr(s));",
            "erase(s, 0);",
            "assert(intptr(a) == intptr(s));",
            "assert(s == [3,4]);",
            "assert(a == [1,2,3,4,5]);",
        },
    },

    {
        "Erase last elem, slice",
        {
            "var a = [1,2,3,4,5];",
            "var s = a[1:4];",
            "assert(s == [2,3,4]);",
            "assert(intptr(a) == intptr(s));",
            "erase(s, len(s) - 1);",
            "assert(intptr(a) == intptr(s));",
            "assert(s == [2,3]);",
            "assert(a == [1,2,3,4,5]);",
        },
    },

    {
        "Erase middle elem, slice",
        {
            "var a = [1,2,3,4,5];",
            "var s = a[1:4];",
            "assert(s == [2,3,4]);",
            "assert(intptr(a) == intptr(s));",
            "erase(s, 1);",
            "assert(intptr(a) != intptr(s));",
            "assert(s == [2,4]);",
            "assert(a == [1,2,3,4,5]);",
        },
    },

    {
        "Builtin range()",
        {
            "assert(range(5) == [0,1,2,3,4]);",
            "assert(range(2,5) == [2,3,4]);",
            "assert(range(2,10,2) == [2,4,6,8]);",
            "assert(range(20,5,-2) == [20,18,16,14,12,10,8,6]);",
        },
    },

    {
        "Builtin find() in array",
        {
            "const arr = [5,8,10];",
            "assert(find(arr, 5) == 0);",
            "assert(find(arr, 8) == 1);",
            "assert(find(arr, 10) == 2);",
            "assert(find(arr, 11) == none);",
        },
    },

    {
        "Builtin find() in string",
        {
            "const s = \"hello world\";",
            "assert(find(s, \"blah\") == none);",
            "assert(find(s, \"hello\") == 0);",
            "assert(find(s, \"wor\") == 6);",
        },
    },

    {
        "Sort, default compare func",
        {
            "var arr = [3,2,1];",
            "var res = sort(arr);", // NOTE: sort() works IN PLACE
            "assert(intptr(arr) == intptr(res));",
            "assert(arr == [1,2,3]);",
        },
    },

    {
        "Sort, default compare func, reverse",
        {
            "var arr = [1,2,3];",
            "var res = rev_sort(arr);", // NOTE: sort() works IN PLACE
            "assert(intptr(arr) == intptr(res));",
            "assert(arr == [3,2,1]);",
        },
    },

    {
        "Sort works with temp arrays as well",
        {
            "var res = sort([3,2,1]);",
            "assert(res == [1,2,3]);",
        },
    },

    {
        "Sort on slice",
        {
            "var arr = [5,4,3,2,1];",
            "var s = arr[1:4];",
            "assert(s == [4,3,2]);",
            "assert(intptr(arr) == intptr(s));",
            "sort(s);",
            "assert(intptr(arr) != intptr(s));",
            "assert(arr == [5,4,3,2,1]);",
            "assert(s == [2,3,4]);",
        },
    },

    {
        "Sort array with slices",
        {
            "var arr = [1,2,3,4,5];",
            "var s = arr[1:4];",
            "assert(intptr(arr) == intptr(s));",
            "rev_sort(arr);",
            "assert(intptr(arr) != intptr(s));",
            "assert(arr == [5,4,3,2,1]);",
            "assert(s == [2,3,4]);",
        },
    },

    {
        "Sort with custom comparison function 1",
        {
            "var arr = [3,2,1];",
            "sort(arr, func (a,b) => a < b);",
            "assert(arr == [1,2,3]);",
        },
    },

    {
        "Sort with custom comparison function 2",
        {
            "var arr = [[3, \"str3\"], [2, \"str2\"], [1, \"str1\"]];",
            "sort(arr, func (a,b) => a[0] < b[0]);",
            "assert(arr == [[1, \"str1\"], [2, \"str2\"], [3, \"str3\"]]);",
        },
    },

    {
        "Sort array of strings",
        {
            "var arr = [\"c\", \"b\", \"a\"];",
            "sort(arr);",
            "assert(arr == [\"a\", \"b\", \"c\"]);",
        },
    },

    {
        "Sort const array",
        {
            "const arr = [3,2,1];",
            "const s = sort(arr);",
            "assert(s == [1,2,3]);",
        },
    },

    {
        "Reverse array",
        {
            "var arr = [1,2,3];",
            "var r = reverse(arr);",
            "assert(intptr(arr) == intptr(r));",
            "assert(arr == [3,2,1]);",
            "assert(r == [3,2,1]);",
        },
    },

    {
        "Reverse slice of array",
        {
            "var arr = [1,2,3,4,5];",
            "var s = arr[1:4];",
            "assert(s == [2,3,4]);",
            "reverse(s);",
            "assert(arr == [1,2,3,4,5]);",
            "assert(s == [4,3,2]);",
        },
    },

    {
        "Reverse array with slices",
        {
            "var arr = [1,2,3,4,5];",
            "var s = arr[1:4];",
            "assert(intptr(arr) == intptr(s));",
            "reverse(arr);",
            "assert(intptr(arr) != intptr(s));",
            "assert(arr == [5,4,3,2,1]);",
            "assert(s == [2,3,4]);",
        },
    },

    {
        "Builtin sum()",
        {
            "const arr = [1,2,3];",
            "const v = sum(arr);",
            "assert(v == 6);",
        },
    },

    {
        "Builtin sum() with key func",
        {
            "const arr = [[1, 323], [2, 123], [3, 999]];",
            "var v = sum(arr, func (e) => e[0]);",
            "assert(v == 6);",
        },
    },

    {
        "Builtin sum() on array of strings",
        {
            "assert(sum([\"a\", \"b\"]) == \"ab\");"
        },
    },

    {
        "Builtin sum() on array of arrays",
        {
            "const arr = [[1,2],[3,4]];",
            "const s = sum(arr);",
            "assert(s == [1,2,3,4]);",
        },
    },

    {
        "Operator + cannot modify strings",
        {
            "var a = \"hello\";",
            "var b = a + \" world\";",
            "assert(b == \"hello world\");",
            "assert(a == \"hello\");",
        }
    },

    {
        "Operator + cannot modify arrays",
        {
            "var a = [1,2,3];",
            "var b = a + [4];",
            "assert(b == [1,2,3,4]);",
            "assert(a == [1,2,3]);",
        },
    },

    {
        "Literal arrays containing non-const variables",
        {
            "var a = 42;",
            "var arr = [a];",
        },
    },

    {
        "Return in while loop works",
        {
            "func f() {",
            "   while (true) {",
            "       return 42;"
            "   }",
            "}",
            "assert(f() == 42);",
        },
    },

    {
        "Subscript with undefined variable",
        {
            "aa[3];",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "Slice with undefined variable",
        {
            "aa[3:5];",
        },
        &typeid(UndefinedVariableEx),
    },

    {
        "Cannot modify const array, append",
        {
            "const arr = [1,2,3];",
            "append(arr, 99);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Cannot modify const array, pop",
        {
            "const arr = [1,2,3];",
            "pop(arr);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Cannot modify const array, erase",
        {
            "const arr = [1,2,3];",
            "erase(arr, 0);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Builtins lpad() and rpad()",
        {
            "assert(lpad(\"a\", 5) == \"    a\");",
            "assert(rpad(\"a\", 5) == \"a    \");",
            "assert(lpad(\"a\", 0) == \"a\");",
            "assert(lpad(\"abcde\", 5) == \"abcde\");",
            "assert(lpad(\"abcdef\", 5) == \"abcdef\");",
            "assert(rpad(\"abcde\", 5) == \"abcde\");",
            "assert(rpad(\"abcdef\", 5) == \"abcdef\");",
            "assert(lpad(\"a\", 5, \"0\") == \"0000a\");",
            "assert(rpad(\"a\", 5, \"0\") == \"a0000\");",
        },
    },

    {
        "Decl multi-id assignments",
        {
            "var a,b = [1,2];",
            "assert(a == 1);",
            "assert(b == 2);",
        },
    },

    {
        "Non-decl multi-id assignments",
        {
            "var a,b;",
            "a,b = [1,2];",
            "assert(a == 1);",
            "assert(b == 2);",
        },
    },

    {
        "Const-decl multi-id assignments",
        {
            "const a,b = [1,2];",
            "assert(a == 1);",
            "assert(b == 2);",
        }
    },

    {
        "Multi-id assignments with more IDs than elems",
        {
            "var a,b,c = [1,2];",
            "assert(a == 1);",
            "assert(b == 2);",
            "assert(c == none);",
        },
    },

    {
        "Multi-id assignments with more elems than IDs",
        {
            "var a,b = [1,2,3];",
            "assert(a == 1);",
            "assert(b == 2);",
        },
    },

    {
        "Decl multi-id assignments with re-defines",
        {
            "var a = 3;",
            "var a,b = [5,6];",
        },
        &typeid(AlreadyDefinedEx),
    },

    {
        "Decl multi-id assignments with re-defines of consts",
        {
            "const a = 3;",
            "var a,b = [5,6];",
        },
        &typeid(CannotRebindConstEx),
    },

    {
        "Decl multi-id assignments to `none`",
        {
            "var a,b;",
            "assert(a == none && b == none);",
        },
    },

    {
        "Decl multi-id assignments to single value",
        {
            "var a,b = \"abc\";",
            "assert(a == \"abc\" && b == \"abc\");",
        },
    },

    {
        "Multi-id assignments with operator +=",
        {
            "var a,b = [1,2];",
            "a,b += [3,10];",
            "assert(a == 4);",
            "assert(b == 12);",
        },
    },

    {
        "Foreach loop",
        {
            "var res = 0;",
            "foreach(var e in [1,2,3]) {",
            "   res += e;",
            "}",
            "assert(res == 6);",
        },
    },

    {
        "Foreach loop, single statement body",
        {
            "var res = 0;",
            "foreach(var e in [1,2,3])",
            "   res += e;",
            "assert(res == 6);",
        },
    },

    {
        "Foreach loop with elems expansion",
        {
            "const arr = [[11, \"hello\"], [22, \"world\"]];",
            "var tmp = [];",
            "foreach (var idx, word in arr) {",
            "   append(tmp, word + \"_\" + str(idx));",
            "}",
            "assert(tmp[0] == \"hello_11\");",
            "assert(tmp[1] == \"world_22\");",
        },
    },

    {
        "Foreach loop with index",
        {
            "var res = [];",
            "foreach (var i, val in indexed [10, 20, 30]) {",
            "   append(res, i * val);",
            "}",
            "assert(res[0] == 0);",
            "assert(res[1] == 20);",
            "assert(res[2] == 60);",
        },
    },

    {
        "Foreach with extern variable",
        {
            "var e;",
            "foreach (e in [1,2,3,4,5]) {",
            "   if (e == 3) continue;",
            "   if (e >= 4) break;",
            "}",
            "assert(e == 4);",
        },
    },

    {
        "Foreach in string",
        {
            "var res = \"\";",
            "var input = \"hello\";",
            "foreach (var i, c in indexed input) {",
            "   res += c;",
            "   if (i < len(input)-1)",
            "       res += \"_\";",
            "}",
            "assert(res == \"h_e_l_l_o\");",
        },
    },

    {
        "Float types work",
        {
            "const myEps = 0.000000001;",
            "const a = 3.4;",
            "const b = 1.2;",
            "const c = a + b;",
            "assert(str(c, 1) == \"4.6\");",
            "assert(str(math_pi, 2) == \"3.14\");",
            "assert(abs((2.0 * 3.0) - 6.0) < myEps);",
            "assert(abs((5.0 / 2) - 2.5) < myEps);",
            "assert(abs((5.0 % 2) - 1.0) < myEps);",
            "assert(abs((5.0 - 2) - 3.0) < myEps);",
            "assert(-1.0 + 1 < myEps);",
            "assert(float(str(1.23)) - 1.23 < myEps);",
            "assert(5.0 > 3.0);",
            "assert(5.0 >= 5.0);",
            "assert(5.0 != 3.0);",
            "assert(2.0 <= 2.0);",
            "assert(2.0 <= 3.0);",
        },
    },

    {
        "Float builtins work",
        {
            "const myEPS = 0.000000001;",
            "assert(sin(0.0) == 0.0);",
            "assert(cos(0.0) == 1.0);",
            "assert(str(sin(math_pi/2),3) == \"1.000\");",
            "assert(str(cos(math_pi/2),3) == \"0.000\");",
            "assert(abs(sin(math_pi/2) - 1.0) < myEPS);",
            "assert(abs(cos(math_pi/2) - 0.0) < myEPS);",
            "assert(abs(round(0.123456789, 0) - 0.0) < myEPS);",
            "assert(abs(round(0.123456789, 1) - 0.1) < myEPS);",
            "assert(abs(round(0.123456789, 2) - 0.12) < myEPS);",
            "assert(abs(round(0.123456789, 3) - 0.123) < myEPS);",
        },
    },

    {
        "Allow sorting of const arrays without side-effect",
        {
            "const a = [3,2,1];",
            "const b = sort(a);",
            "assert(a == [3,2,1]);",
            "assert(b == [1,2,3]);",
        },
    },

    {
        "Named pure funcs",
        {
            "pure func cmp(a,b) => a > b;",
            "const a = [1,2,3];",
            "const b = sort(a, cmp);",
            "assert(a == [1,2,3]);",
            "assert(b == [3,2,1]);",
        },
    },

    {
        "Temporary pure funcs",
        {
            "const a = [1,2,3];",
            "const b = sort(a, pure func (x,y) => x > y);",
            "assert(a == [1,2,3]);",
            "assert(b == [3,2,1]);",
        }
    },

    {
        "Cannot bind temporary pure func to const",
        {
            "const f = pure func(x) => x+1;",
        },
        &typeid(CannotBindPureFuncToConstEx),
    },

    {
        "Named pure funcs cannot see global symbols",
        {
            "var g = 3;",
            "pure func pf() => g+1;",
            "pf();",
        },
        &typeid(UndefinedVariableEx)
    },

    {
        "Temp. pure funcs cannot see non-const global symbols",
        {
            "var g = 3;",
            "print((pure func(x) => g+x)(3));",
        },
        &typeid(UndefinedVariableEx)
    },

    {
        "Builtin map()",
        {
            "const a = [1,2,3];",
            "const b = map(pure func(x) => x*2, a);",
            "assert(b == [2,4,6]);",
        }
    },

    {
        "Builtin filter()",
        {
            "const a = [1,2,3,4,5];",
            "const b = filter(pure func(x) => !(x%2), a);",
            "assert(b == [2,4]);",
        }
    },

    {
        "Dict type",
        {
            "var e = {};",
            "assert(len(e) == 0);",
            "var d = {\"a\": 3, \"b\": 5};",
            "assert(len(d) == 2);",
            "assert(d[\"a\"] == 3);",
            "assert(d[\"b\"] == 5);",
            "d[\"a\"] = 33;",
            "assert(d == {\"a\": 33, \"b\": 5});",
            "var d2 = d;",
            "assert(intptr(d) == intptr(d2));",
            "var r = erase(d, \"aaa\");",
            "assert(!r);",
            "r = erase(d, \"a\");",
            "assert(r);",
            "assert(d == {\"b\": 5});",
        },
    },

    {
        "Dict keys(), values() and kvpairs()",
        {
            "var d = {\"a\": 3, \"b\": 5};",
            "var k = keys(d);",
            "assert(len(k) == 2);",
            "sort(k);",
            "assert(k == [\"a\", \"b\"]);",
            "var v = values(d);",
            "sort(v);",
            "assert(v == [3,5]);",
            "var kv = kvpairs(d);",
            "assert(len(kv) == 2);",
            "sort(kv, func (a, b) => a[0] < b[0]);",
            "assert(kv == [[\"a\", 3], [\"b\", 5]]);",
        },
    },

    {
        "map() on dict",
        {
            "var d = {\"a\": 3, \"b\": 5};",
            "var r = map(func (k,v) => str(k)+str(v), d);",
            "sort(r);",
            "assert(r == [\"a3\", \"b5\"]);",
        },
    },

    {
        "filter() on dict",
        {
            "var d = {\"a\": 3, \"b\": 5};",
            "var r = filter(func (k,v) => v <= 3, d);",
            "assert(r == {\"a\" : 3});",
        },
    },

    {
        "insert() builtin on arrays, begin",
        {
            "var arr = [1,2,3];",
            "insert(arr, 0, 99);",
            "assert(arr == [99,1,2,3]);",
        },
    },

    {
        "insert() builtin on arrays, middle",
        {
            "var arr = [1,2,3];",
            "insert(arr, 1, 99);",
            "assert(arr == [1,99,2,3]);",
        },
    },

    {
        "insert() builtin on arrays, end",
        {
            "var arr = [1,2,3];",
            "insert(arr, 2, 99);",
            "assert(arr == [1,2,99,3]);",
        },
    },

    {
        "insert() builtin on arrays, past-end",
        {
            "var arr = [1,2,3];",
            "insert(arr, 3, 99);",
            "assert(arr == [1,2,3,99]);",
        },
    },

    {
        "insert() builtin on arrays, past-end + 1",
        {
            "var arr = [1,2,3];",
            "insert(arr, 4, 99);",
        },
        &typeid(OutOfBoundsEx),
    },

    {
        "insert() builtin on dict",
        {
            "var d = {\"a\": 3, \"b\": 5};",
            "var r = insert(d, \"a\", 99);",
            "assert(!r);",
            "assert(len(d) == 2);",
            "r = insert(d, \"c\", 99);",
            "assert(len(d) == 3);",
            "var p = kvpairs(d);",
            "sort(p, func(a,b) => a[0] < b[0]);",
            "assert(p == [[\"a\",3],[\"b\",5],[\"c\",99]]);",
        },
    },

    {
        "Object member-access syntax for dict",
        {
            "var d = {\"a\": 42};",
            "assert(d.a == 42);",
            "d.a = 11;",
            "assert(d.a == 11);",
            "d.p2 = \"hello\";",
            "var p = kvpairs(d);",
            "sort(p, func(a,b) => a[0] < b[0]);",
            "assert(p == [[\"a\", 11], [\"p2\", \"hello\"]]);",
        },
    },

    {
        "Object member-access syntax for dict: composition with other ops",
        {
            "var d = {\"a\": [{}, 3, 4]};",
            "assert(d.a[0] == {});",
            "d.a[0].f1 = 3;",
            "d.a[0].f2 = [11,22];",
            "assert(d.a[0].f2[1] == 22);",
        },
    },

    {
        "Builtin dict(): convert array of [k,v] pairs to dict",
        {
            "const orig_a = [[\"a\", 3], [\"b\", 4]];",
            "const d = dict(orig_a);",
            "assert(d == {\"a\":3, \"b\":4});",
            "const gen_a = kvpairs(d);",
            "const sorted_gen_a = sort(gen_a, pure func (a,b) => a[0] < b[0]);",
            "assert(orig_a == sorted_gen_a);",
        },
    },

    {
        "Initialization of multiple vars to single value",
        {
            "var a,b,c = 123;",
            "assert(a == 123);",
            "assert(b == 123);",
            "assert(c == 123);",
        }
    },

    {
        "Dict foreach",
        {
            "var d = {\"a\": 3, \"b\": 4};",
            "var arr = [];",
            "foreach (var k, v, nn in d) {",
            "   assert(nn == none);",
            "   append(arr, [k,v]);",
            "}",
            "assert(dict(arr) == d);",
        },
    },

    {
        "Set item in slice of array",
        {
            "var a = [1,2,3,4,5];",
            "var s = a[1:4];",
            "assert(s == [2,3,4]);",
            "s[1] = 99;",
            "assert(a == [1,2,3,4,5]);",
            "assert(s == [2,99,4]);",
        },
    },

    {
        "Op-assign operators",
        {
            "var a = +10;",
            "a += 1;",
            "assert(a == 11);",
            "a -= 3;",
            "assert(a == 8);",
            "a *= 2;",
            "assert(a == 16);",
            "a /= 3;",
            "assert(a == 5);",
            "a %= 4;",
            "assert(a == 1);",
        },
    },

    {
        "Precedence between && and ||",
        {
            "assert((  1 ||  1  && 0  ) == 1);",
            "assert((  1 || (1  && 0) ) == 1);",
            "assert(( (1 ||  1) && 0  ) == 0);",
        },
    },

    {
        "Clone dict",
        {
            "var d = {\"a\": 3};",
            "assert(d[\"a\"] == 3);",
            "var d2 = clone(d);",
            "d2[\"a\"] = 99;",
            "assert(d[\"a\"] == 3);",
            "assert(d2[\"a\"] == 99);",
        },
    },

    {
        "Dict to string",
        {
            "assert(str({\"a\":3}) == \"{a: 3}\");",
        },
    },

    {
        "Array to string",
        {
            "assert(str([1,2,3]) == \"[1, 2, 3]\");",
        },
    },

    {
        "Accessing a non-existent member of dict",
        {
            "var d = {};",
            "assert(len(d) == 0);",
            "d[\"a\"] = 5;",
            "assert(len(d) == 1);",
            "assert(d[\"a\"] == 5);",
            "assert(d != {});",
        },
    },

    {
        "Compare dict to other type",
        {
            "assert(({} == 3) == 0);",
        },
    },

    {
        "String compare operators",
        {
            "assert(\"a\" < \"b\");",
            "assert(\"a\" <= \"a\");",
            "assert(\"b\" > \"a\");",
            "assert(\"b\" >= \"a\");",
        },
    },

    {
        "Dict with integer keys",
        {
            "var d = {5: 10, 100: 11};",
            "assert(d == {5:10, 100:11});",
            "assert(d[5] == 10);",
            "assert(d[100] == 11);",
        },
    },

    {
        "Array slice without start",
        {
            "const a = [1,2,3];",
            "assert(a[:2] == [1,2]);",
            "assert(a[1:] == [2,3]);"
        },
    },

    {
        "Array slice without end",
        {
            "const a = [1,2,3];",
            "assert(a[1:] == [2,3]);"
        },
    },

    {
        "Array slice without start nor end",
        {
            "const a = [1,2,3];",
            "assert(a[:2] == [1,2]);",
            "assert(a[1:] == [2,3]);"
        },
    },

    {
        "Array and dict to bool",
        {
            "if ([]) {",
            "   assert(0);",
            "} else {",
            "   assert(1);",
            "}",

            "if ([1]) {",
            "   assert(1);",
            "} else {",
            "   assert(0);",
            "}",

            "if ({}) {",
            "   assert(0);",
            "} else {",
            "   assert(1);",
            "}",

            "if ({2: 3}) {",
            "   assert(1);",
            "} else {",
            "   assert(0);",
            "}",
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
check(const test &t, int &err_line, bool dump_syntax_tree)
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
            err_line = e.loc_start.line;

            if (dump_syntax_tree && root) {
                cout << "  Syntax tree:" << endl;
                root->serialize(cout, 2);
                cout << endl;
            }
            return false;
        }

        return true;
    }

    if (t.ex) {

        dump_expected_ex(t.ex, nullptr);

        if (dump_syntax_tree && root) {
            cout << "  Syntax tree:" << endl;
            root->serialize(cout, 2);
            cout << endl;
        }

        return false;
    }

    return true;
}

static void
dump_test_source(const test &t, int err_line)
{
    cout << "  Source: ";

    if (t.source.size() == 1) {

        cout << t.source[0] << endl;

    } else {

        cout << endl;

        for (unsigned i = 0; i < t.source.size(); i++) {
            cout << "    ";
            cout << setfill(' ') << setw(3) << i+1;
            cout << "    " << t.source[i];

            if ((int)i+1 == err_line)
                cout << "   <----- GOT EXCEPTION HERE";

            cout << endl;
        }
    }
}

void run_tests(bool dump_syntax_tree)
{
    size_t pass_count = 0;
    int err_line;

    for (const auto &test : tests) {

        cout << "[ RUN  ] " << test.name << endl;

        err_line = 0;
        if (check(test, err_line, dump_syntax_tree)) {

            cout << "[ PASS ]";
            pass_count++;

        } else {

            dump_test_source(test, err_line);
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

void run_tests(bool)
{
    cout << "Tests NOT compiled in. Build with TESTS=1" << endl;
    exit(1);
}

#endif
