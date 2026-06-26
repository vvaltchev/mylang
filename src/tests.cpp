/* SPDX-License-Identifier: BSD-2-Clause */

#include "defs.h"
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <sstream>

#ifdef TESTS

#include "parser.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"
#include "resolver.h"
#include "backtrace.h"
#include "stype.h"
#include "inferencer.h"
#include "repl.h"
#include "lineedit.h"
#include "highlight.h"
#include "replhelp.h"
#include "trace.h"
#include "coderender.h"

#include <typeinfo>
#include <vector>
#include <algorithm>

using std::setw;
using std::setfill;

struct test {

    const char *name;
    std::vector<const char *> source;
    const std::type_info *ex = nullptr;

    /*
     * Expected source location of the thrown exception. Each field is checked
     * only when non-zero, so tests can pin as much of the caret span as they
     * care about. ex_col/ex_line are loc_start; ex_col_end/ex_line_end are
     * loc_end (the column one past the last marked char + 1).
     */
    int ex_col = 0;
    int ex_line = 0;
    int ex_col_end = 0;
    int ex_line_end = 0;
};

static const std::vector<test> tests =
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
        "cannot rebind consts existing at runtime 1",
        {
            "const arr = [1,2,3];",
            "arr = [9];",   /* same type: still a const-rebind, not a type error */
        },
        &typeid(CannotRebindConstEx),
    },

    {
        "cannot rebind consts existing at runtime 2",
        {
            "const x = pure func (x, y) => x < y;",
            "x = 42;",
        },
        &typeid(CannotRebindConstEx),
    },

    {
        "cannot rebind consts existing at runtime 3",
        {
            "const x = { 3 : 10, 4: 20 };",
            "x = {};",   /* same type: still a const-rebind, not a type error */
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
            "var dyn a = len;\n",   /* a builtin handle is dynamically typed */
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
        "rebind of const builtins is not allowed",
        {
            "len = 5;",
        },
        &typeid(CannotRebindBuiltinEx),
    },

    {
        "rebind of non-const builtins is not allowed",
        {
            "print = 5;",
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
        "Array: clone of an array grown in place (+=)",
        {
            /* clone() must copy the array's *current* length, not the stale
             * len from construction (a non-slice grows via += in place). */
            "var a = [1,2];",
            "a += [3,4];",
            "var b = clone(a);",
            "assert(b == [1,2,3,4]);",
            "a[0] = 99;",
            "assert(b == [1,2,3,4]);",
        },
    },

    {
        "Const slice result is materialized and usable",
        {
            "const x = [10,20,30,40,50];",
            "const y = x[1:4];",
            "assert(y == [20,30,40]);",
            "assert(y[0] == 20);",
            "assert(len(y) == 3);",
        },
    },

    {
        "Const-ness propagates: a const slice bound to a var stays read-only",
        {
            /* Binding a const-derived value to a `var` keeps it read-only -
             * `var` only grants rebindability of the name, not mutability of
             * the value. clone() is the way to get a mutable copy. */
            "const x = [1,2,3,4];",
            "var z = x[0:3];",
            "assert(z == [1,2,3]);",
            "z[0] = 99;",
        },
        &typeid(NotLValueEx),
    },

    {
        "clone() of a const slice yields a mutable, independent copy",
        {
            "const x = [1,2,3,4];",
            "var z = clone(x[0:3]);",
            "z[0] = 99;",
            "assert(z == [99,2,3]);",
            "assert(x == [1,2,3,4]);",
        },
    },

    {
        "Var bound to a const-foldable array literal is deeply mutable",
        {
            "var a = [[1,2],[3,4]];",
            "a[0][0] = 9;",
            "assert(a == [[9,2],[3,4]]);",
        },
    },

    {
        "Materialized const value is fresh on each evaluation",
        {
            /* A function body bound to a const-foldable literal must get an
             * independent array each call (no leak across invocations). */
            "func f() {",
            "   var a = [1,2,3];",
            "   a[0] = a[0] + 1;",
            "   return a;",
            "}",
            "assert(f() == [2,2,3]);",
            "assert(f() == [2,2,3]);",
        },
    },

    {
        "Const dict bound to a var stays read-only",
        {
            "const d = {\"a\": 1, \"b\": 2};",
            "var e = d;",
            "assert(e[\"a\"] == 1);",   /* reads are fine */
            "e[\"a\"] = 9;",            /* writes are not */
        },
        &typeid(NotLValueEx),
    },

    {
        "clone() of a const dict yields a mutable, independent copy",
        {
            "const d = {\"a\": 1, \"b\": 2};",
            "var e = clone(d);",
            "e[\"a\"] = 9;",
            "assert(e[\"a\"] == 9);",
            "assert(d[\"a\"] == 1);",
        },
    },

    /* Deep-const read-only: a const array/dict cannot be mutated even when
     * aliased through a non-const function parameter. */
    {
        "Const array: element write through a param is rejected",
        {
            "func g(p) { p[0] = 9; }",
            "const y = [1,2,3];",
            "g(y);",
        },
        &typeid(NotLValueEx),
    },

    {
        "Const array: += through a param is rejected",
        {
            "func g(p) { p += [4]; }",
            "const y = [1,2,3];",
            "g(y);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Const array: append through a param is rejected",
        {
            "func g(p) { append(p, 4); }",
            "const y = [1,2,3];",
            "g(y);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Const array: nested element write through a param is rejected",
        {
            "func g(p) { p[0][0] = 9; }",
            "const y = [[1,2],[3,4]];",
            "g(y);",
        },
        &typeid(NotLValueEx),
    },

    {
        "Const dict: element write through a param is rejected",
        {
            "func g(p) { p[\"a\"] = 9; }",
            "const d = {\"a\": 1};",
            "g(d);",
        },
        &typeid(NotLValueEx),
    },

    {
        "Const dict: member write through a param is rejected",
        {
            "func g(p) { p.a = 9; }",
            "const d = {\"a\": 1};",
            "g(d);",
        },
        &typeid(NotLValueEx),
    },

    {
        "Const dict: erase through a param is rejected",
        {
            "func g(p) { erase(p, \"a\"); }",
            "const d = {\"a\": 1, \"b\": 2};",
            "g(d);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Const param is rebindable, only mutation is rejected",
        {
            /* g rebinds its own param to a fresh array and mutates that; the
             * const argument is left untouched. */
            "func g(p) { p = [9,9]; p[0] = 1; return p; }",
            "const y = [1,2,3];",
            "assert(g(y) == [1,9]);",
            "assert(y == [1,2,3]);",
        },
    },

    {
        "clone() of a const yields a mutable, independent copy",
        {
            "const y = [1,2,3];",
            "var z = clone(y);",
            "z[0] = 9;",
            "append(z, 4);",
            "assert(z == [9,2,3,4]);",
            "assert(y == [1,2,3]);",
        },
    },

    {
        "Reading a const through a function works",
        {
            "func sumit(p) { var t = 0; foreach (var e in p) { t += e; }",
            "                return t; }",
            "const y = [1,2,3];",
            "assert(sumit(y) == 6);",
        },
    },

    {
        "sort() of a const returns a sorted copy, original untouched",
        {
            "func g(p) { return sort(p); }",
            "const y = [3,1,2];",
            "assert(g(y) == [1,2,3]);",
            "assert(y == [3,1,2]);",
        },
    },

    {
        "sum() does not mutate its argument",
        {
            "var arr = [[1,2],[3,4]];",
            "var s = sum(arr);",
            "assert(s == [1,2,3,4]);",
            "assert(arr == [[1,2],[3,4]]);",
        },
    },

    {
        "deepclone() of a const yields a fully-mutable deep copy",
        {
            "const y = [[1,2],[3,4]];",
            "var c = deepclone(y);",
            "c[0][0] = 99;",            /* nested mutation works */
            "append(c, [5,6]);",
            "assert(c == [[99,2],[3,4],[5,6]]);",
            "assert(y == [[1,2],[3,4]]);",  /* original untouched */
        },
    },

    {
        "deepclone() of a const dict is fully mutable and independent",
        {
            "const d = {\"a\": [1,2]};",
            /* dyn: a dict read is opt now, so mutating a nested value needs the
             * dyn escape (this test is about deepclone, not nullability). */
            "var dyn e = deepclone(d);",
            "e[\"a\"][0] = 99;",
            "assert(e[\"a\"] == [99,2]);",
            "assert(d[\"a\"] == [1,2]);",
        },
    },

    {
        "clone() is shallow: a nested const stays read-only",
        {
            /* clone() copies only the top level; nested objects are shared, and
             * a shared const stays read-only. Use deepclone() to mutate deeply.
             */
            "const y = [[1,2],[3,4]];",
            "var c = clone(y);",
            "c[0] = [9,9];",   /* OK: top level is mutable */
            "c[1][0] = 9;",    /* error: c[1] is a shared const */
        },
        &typeid(NotLValueEx),
    },

    {
        "Const-ness propagates into a fresh literal element",
        {
            /* `a` is a fresh, mutable array, but its element is the const `y`,
             * which stays read-only. */
            "const y = [1,2];",
            "var a = [y, 3];",
            "assert(a[0] == [1,2]);",
            "a[0][0] = 9;",
        },
        &typeid(NotLValueEx),
    },

    {
        "deepclone() of a scalar / string is a no-op",
        {
            "assert(deepclone(5) == 5);",
            "assert(deepclone(\"hi\") == \"hi\");",
        },
    },

    /* More read-only / clone / deepclone coverage. */
    {
        "Const array: pop through a param is rejected",
        {
            "func g(p) { pop(p); }",
            "const y = [1,2,3];",
            "g(y);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Const array: erase through a param is rejected",
        {
            "func g(p) { erase(p, 0); }",
            "const y = [1,2,3];",
            "g(y);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Const array: insert through a param is rejected",
        {
            "func g(p) { insert(p, 0, 9); }",
            "const y = [1,2,3];",
            "g(y);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Const dict: insert through a param is rejected",
        {
            "func g(p) { insert(p, \"k\", 9); }",
            "const d = {\"a\": 1};",
            "g(d);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Const empty array is read-only",
        {
            "func g(p) { append(p, 1); }",
            "const e = [];",
            "g(e);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "Builtin get() / get!() on a const dict: existing and missing",
        {
            "const d = {\"a\": 1};",
            "assert(get(d, \"a\") == 1);",       /* present -> value */
            "assert(get(d, \"zzz\") == none);",  /* missing -> none */
            "assert(get!(d, \"a\") == 1);",      /* present -> value (non-opt) */
            "assert(len(d) == 1);",              /* nothing was inserted */
        },
    },
    {
        "Builtin get!() on a missing key throws",
        { "const d = {\"a\": 1}; get!(d, \"zzz\");" },
        &typeid(KeyNotFoundEx),
    },

    {
        "Read a const dict's members at runtime: existing (member) + missing "
        "(get)",
        {
            "func ga(p) { return p.a; }",
            "const d = {\"a\": 1};",
            "assert(ga(d) == 1);",                /* present member -> value */
            "assert(get(d, \"zzz\") == none);",   /* missing -> none */
            "assert(len(d) == 1);",
        },
    },
    {
        "A missing dict member throws (member access is non-opt)",
        { "var d = {\"a\": 1}; print(d.zzz);" },
        &typeid(KeyNotFoundEx),
    },

    {
        "Const dict nested in a fresh array stays read-only",
        {
            "const d = {\"a\": 1};",
            "var a = [d];",
            "assert(a[0][\"a\"] == 1);",
            "a[0][\"a\"] = 9;",
        },
        &typeid(NotLValueEx),
    },

    {
        "deepclone() of a deeply nested const is fully mutable",
        {
            "const y = [[[1]]];",
            "var c = deepclone(y);",
            "c[0][0][0] = 9;",
            "assert(c == [[[9]]]);",
            "assert(y == [[[1]]]);",
        },
    },

    {
        "clone() of a string returns it unchanged",
        {
            "assert(clone(\"hi\") == \"hi\");",
        },
    },

    {
        "deepclone() with the wrong number of arguments is rejected",
        {
            "deepclone(1, 2);",
        },
        &typeid(InvalidNumberOfArgsEx),
    },

    {
        "Array + non-array is a type error",
        {
            "func f(a) { return a + 5; }",
            "f([1,2]);",
        },
        &typeid(TypeMismatchEx),    /* compile-time now: a is array, a + 5 */
    },

    {
        "Array equality: equal, length mismatch, type mismatch",
        {
            /* eq2 compares values of differing types, so its params are dyn. */
            "func eq2(dyn a, dyn b) { return a == b; }",
            "assert(eq2([1,2], [1,2]));",
            "assert(eq2([1,2], [1,2,3]) == false);",
            "assert(eq2([1,2], 5) == false);",
        },
    },

    {
        "Array subscript with a non-integer is a type error",
        {
            "func f(a) { return a[\"x\"]; }",
            "f([1,2,3]);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "Array slice edge cases",
        {
            "func sl(a) {",
            "   assert(a[1:] == [2,3]);",         /* open end */
            "   assert(a[:2] == [1,2]);",         /* open start */
            "   assert(a[-10:] == [1,2,3]);",     /* start clamped to 0 */
            "   assert(a[:-1] == [1,2]);",        /* negative end */
            "   assert(a[:100] == [1,2,3]);",     /* end clamped to size */
            "   assert(a[5:] == []);",            /* start past end */
            "   assert(a[2:1] == []);",           /* end <= start */
            "}",
            "sl([1,2,3]);",
        },
    },

    {
        "Array slice with a non-integer start is a type error",
        {
            "func sl(a) { return a[\"x\":]; }",
            "sl([1,2,3]);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "Array slice with a non-integer end is a type error",
        {
            "func sl(a) { return a[1:\"x\"]; }",
            "sl([1,2,3]);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "Writing outside a live slice doesn't detach it",
        {
            /* The write at index 3 is outside s's [0:2] range, so the COW pass
             * skips s (it keeps sharing) and the in-place write is visible. */
            "var a = [1,2,3,4];",
            "var s = a[0:2];",
            "a[3] = 99;",
            "assert(a == [1,2,3,99]);",
            "assert(s == [1,2]);",
        },
    },

    {
        "str() of a multi-element dict",
        {
            "assert(str({\"a\":1, \"b\":2}) != \"\");",
        },
    },

    {
        "clone() with the wrong number of arguments is rejected",
        {
            "clone(1, 2);",
        },
        &typeid(InvalidNumberOfArgsEx),
    },

    /* erase()/insert() preserve slice independence: a slice taken before the
     * mutation must keep acting like an independent copy (it is detached). */
    { "erase: a FRONT erase does not disturb a live slice (flat int)",
      { "var a = [10,20,30,40,50]; var s = a[1:4];",   /* s = [20,30,40] */
        "erase(a, 0);",
        "assert(s == [20,30,40]);",
        "assert(a == [20,30,40,50]);" } },
    { "erase: a MIDDLE erase does not disturb a live slice",
      { "var a = [10,20,30,40,50]; var s = a[0:3];",   /* s = [10,20,30] */
        "erase(a, 3);",
        "assert(s == [10,20,30]);",
        "assert(a == [10,20,30,50]);" } },
    { "erase: erasing the LAST element leaves a non-reaching slice intact",
      { "var a = [10,20,30,40,50]; var s = a[0:2];",   /* s = [10,20] */
        "erase(a, 4);",
        "assert(s == [10,20]);",
        "assert(a == [10,20,30,40]);" } },
    { "erase: front erase preserves a slice of a GENERAL (string) array",
      { "var a = [\"a\",\"b\",\"c\",\"d\"]; var s = a[1:3];",   /* [b,c] */
        "erase(a, 0);",
        "assert(s == [\"b\",\"c\"]);",
        "assert(a == [\"b\",\"c\",\"d\"]);" } },
    { "insert: a MIDDLE insert does not disturb a live slice",
      { "var a = [1,2,3,4,5]; var s = a[1:4];",         /* s = [2,3,4] */
        "insert(a, 2, 99);",
        "assert(s == [2,3,4]);",
        "assert(a == [1,2,99,3,4,5]);" } },

    /* erase()/insert() error paths. */
    {
        "erase() with the wrong number of arguments is rejected",
        {
            "var a = [1,2,3];",
            "erase(a);",
        },
        &typeid(InvalidNumberOfArgsEx),
    },

    {
        "erase() on a non-lvalue is rejected",
        {
            "erase([1,2,3], 0);",
        },
        &typeid(NotLValueEx),
    },

    {
        "erase() on a const-declared variable is rejected",
        {
            "const y = [1,2,3];",
            "erase(y, 0);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "erase() on an array with a non-integer index is a type error",
        {
            "var a = [1,2,3];",
            "erase(a, \"x\");",
        },
        &typeid(TypeErrorEx),
    },

    {
        "erase() on an unsupported container type is a type error",
        {
            "var x = 5;",
            "erase(x, 0);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "insert() with the wrong number of arguments is rejected",
        {
            "var a = [1,2,3];",
            "insert(a, 0);",
        },
        &typeid(InvalidNumberOfArgsEx),
    },

    {
        "insert() on a non-lvalue is rejected",
        {
            "insert([1,2,3], 0, 9);",
        },
        &typeid(NotLValueEx),
    },

    {
        "insert() on a const-declared variable is rejected",
        {
            "const y = [1,2,3];",
            "insert(y, 0, 9);",
        },
        &typeid(CannotChangeConstEx),
    },

    {
        "insert() on an array with a non-integer index is a type error",
        {
            "var a = [1,2,3];",
            "insert(a, \"x\", 9);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "insert() on an unsupported container type is a type error",
        {
            "var x = 5;",
            "insert(x, 0, 9);",
        },
        &typeid(TypeErrorEx),
    },

    /* ---- lexer (lexer.cpp): number / string / comment paths ---- */
    {
        "lexer: valid float forms (exponent, leading dot)",
        {
            "assert(1.5e3 == 1500.0);",
            "assert(.5 == 0.5);",
            "assert(2e2 == 200.0);",
        },
    },
    {
        "lexer: line comments are skipped",
        {
            "# this is a comment",
            "var x = 1; # trailing comment",
            "assert(x == 1);",
        },
    },
    { "lexer: malformed float exponent is an invalid token",
      { "var x = 1e;" }, &typeid(InvalidTokenEx) },
    { "lexer: a second exponent in a float is an invalid token",
      { "var x = 1e2e3;" }, &typeid(InvalidTokenEx) },
    { "lexer: a hex-looking literal is an invalid token",
      { "var x = 0x;" }, &typeid(InvalidTokenEx) },
    { "lexer: an unterminated string is an invalid token",
      { "var x = \"abc;" }, &typeid(InvalidTokenEx) },
    { "lexer: an unknown character is a syntax error",
      { "var x = @;" }, &typeid(SyntaxErrorEx) },
    { "lexer: a malformed number is a syntax error",
      { "var x = 1.2.3;" }, &typeid(SyntaxErrorEx) },

    /* parse-time dead-code folding must yield a Nop (not a NULL statement),
     * else the FOLLOWING statement is a spurious syntax error. */
    { "dead-code: const-false if (no else) then a statement",
      { "func d1() { if (false) { return 99; } return 7; }",
        "assert(d1() == 7);" } },
    { "dead-code: const-true if (empty body) then a statement",
      { "func d2() { if (true) { } return 5; }",
        "assert(d2() == 5);" } },
    { "dead-code: while(false) then a statement",
      { "func d3() { while (false) { return 9; } return 3; }",
        "assert(d3() == 3);" } },
    { "dead-code: foreach over an empty literal then a statement",
      { "func d4() { foreach (var e in []) { return e; } return 4; }",
        "assert(d4() == 4);" } },
    { "dead-code: const-false if at top level then a statement",
      { "if (false) { assert(false); } var ok = 1; assert(ok == 1);" } },

    /* short-circuit folding: a const operand that determines a logical op's
     * result (the rest short-circuits, so dropping it is sound). */
    { "fold: short-circuit `true || X` folds to true",
      { "func sc_or(x) => true || (x > 0);",
        "assert(startswith(show(sc_or),"
        " \"/* pure */ func sc_or(x) => true\"));" } },
    { "fold: short-circuit `false && X` folds to false",
      { "func sc_and(x) => false && (x > 0);",
        "assert(startswith(show(sc_and),"
        " \"/* pure */ func sc_and(x) => false\"));" } },
    { "fold: short-circuit drops the dead operand's side effects",
      { "var n = 0;",
        "func bump() { n = n + 1; return true; }",
        "func sc_se() { if (false && bump()) { } return n; }",
        "assert(sc_se() == 0);" } },
    { "fold: a const-false `&&` flag eliminates the guarded branch (DCE)",
      { "const FLAG = false;",
        "func guarded(x) { if (FLAG && x > 1000) { return 1; } return 2; }",
        "assert(guarded(9) == 2);" } },
    { "fold: short-circuit `true ||` drops an undefined dead operand",
      { "func sc_undef() => 7 || undefined_name_xyz;",
        "assert(sc_undef() == true);" } },
    /* a NON-determining leading const (`false ||`, `true &&`) drops - but only
     * collapses to a lone operand when that operand is ALREADY bool, since
     * mylang's ||/&& yield bool (so `false || 5` is `true`, not `5`). */
    { "fold: `false || (cmp)` collapses to the comparison (already bool)",
      { "func bc1(x) => false || (x > 0);",
        "assert(startswith(show(bc1),"
        " \"/* pure */ func bc1(x) => x > 0\"));",
        "assert(bc1(5) == true && bc1(-1) == false);" } },
    { "fold: `true && (cmp)` collapses to the comparison",
      { "func bc2(x) => true && (x > 0);",
        "assert(startswith(show(bc2),"
        " \"/* pure */ func bc2(x) => x > 0\"));" } },
    { "fold: `false || x` (non-bool x) does NOT collapse; yields bool(x)",
      { "func bc3(x) => false || x;",
        "assert(bc3(5) == true);",   /* bool true, NOT the int 5 */
        "assert(bc3(0) == false);" } },
    { "fold: a leading const-false drops from a || chain",
      { "func bc4(a, b) => false || (a > 0) || (b > 0);",
        "assert(endswith(show(bc4), \"a > 0 || b > 0;\\n\"));" } },

    /* ---- resolver (resolver.cpp): auto-const DCE & auto-pure analysis ---- */
    {
        "auto-const DCE: dead branches dropped after promotion",
        {
            /* c/d/w are write-once -> auto-promoted, so the conditions fold and
               the dead branch is dropped by the resolver's DCE. */
            "var c = 0; var r = 0;  if (c) { r = 1; } else { r = 2; }",
            "assert(r == 2);",
            "var d = 1; var r2 = 0; if (d) { r2 = 1; } else { r2 = 2; }",
            "assert(r2 == 1);",
            "var w = 0; var cnt = 0; while (w) { cnt += 1; }",
            "assert(cnt == 0);",
        },
    },
    {
        /* Regression: a promoted write-once var used ONLY in a `return` must be
         * folded there too. ReturnStmt is a plain Construct (not a
         * SingleChildConstruct), so without an explicit fold_child case its
         * decl was dropped but the use left dangling ("undefined variable"). */
        "auto-const: a write-once var used only in a return is folded",
        {
            "func f() { var x = 5; return x; }",
            "func g() { var y = 6; return y + 1; }",
            "assert(f() == 5);",
            "assert(g() == 7);",
        },
    },
    {
        /* Regression: a promoted write-once var used as a subscript/member
         * INDEX in an assignment LVALUE must be folded there too. fold_block
         * folded only the rvalue, so the index's decl was dropped but its use
         * was left
         * dangling -> "undefined variable" on `a[i] = v`, `a[i] += v`,
         * `d[k] = v`, `m[i][j] = v`. (A reassigned index like a loop's `j` is
         * never promoted, which is why the sieve never hit this.) */
        "auto-const: write-once var as an assignment subscript index is folded",
        {
            "func t() {",
            "    var a = [10, 20, 30];",
            "    var i = 0;",            /* write-once -> promoted */
            "    a[i] = 5;",             /* index in a plain-assign lvalue */
            "    var j = 1;",
            "    a[j] += 100;",          /* index in a compound-assign lvalue */
            "    var dyn d = {};",       /* dyn: a dict read is opt otherwise */
            "    var k = 7;",
            "    d[k] = 99;",            /* dict key in an assign lvalue */
            "    return a[0] + a[1] + d[7];",
            "}",
            "assert(t() == 5 + 120 + 99);",
        },
    },
    {
        "auto-pure analysis descends into for-loop and try/catch bodies",
        {
            "func f() { var s = 0;",
            "           for (var i = 0; i < 3; i += 1) { s += i; }",
            "           return s; }",
            "func g() { try { return 1; } catch (Foo) { return 2; } }",
            "assert(ispure(f));",
            "assert(ispure(g));",
            "assert(f() == 3);",
            "assert(g() == 1);",
        },
    },
    {
        "runtime if/else (non-const condition) takes the else branch",
        {
            "var c = rand(0, 0);",   /* runtime 0: condition is not folded */
            "var r = 0;",
            "if (c) { r = 1; } else { r = 2; }",
            "assert(r == 2);",
        },
    },

    /* ---- exit / exception / exdata / type builtins (types.cpp) ---- */
    {
        "exception() / exdata() / type() value forms",
        {
            "var e = ex(\"MyErr\", 42);",
            "assert(exdata(e) == 42);",
            "assert(type(5) == \"int\");",
            "assert(type(2.5) == \"float\");",
            "assert(type(\"s\") == \"str\");",
            "assert(type([1]) == \"arr\");",
            "assert(type({1:2}) == \"dict\");",
        },
    },
    { "exit() with no args is rejected",
      { "exit();" }, &typeid(InvalidNumberOfArgsEx) },
    { "exit() with a non-integer is a type error",
      { "exit(\"x\");" }, &typeid(TypeErrorEx) },
    { "ex() with no args is rejected",
      { "ex();" }, &typeid(InvalidNumberOfArgsEx) },
    { "ex() with a non-string name is a type error",
      { "ex(123);" }, &typeid(TypeErrorEx) },
    { "ex() with a digit-leading name is an invalid value",
      { "ex(\"1bad\");" }, &typeid(InvalidValueEx) },
    { "ex() with a non-identifier name is an invalid value",
      { "ex(\"a-b\");" }, &typeid(InvalidValueEx) },
    { "exdata() of a non-exception is a type error",
      { "exdata(5);" }, &typeid(TypeErrorEx) },
    { "exdata() with no args is rejected",
      { "exdata();" }, &typeid(InvalidNumberOfArgsEx) },

    /* ---- ++ / -- (pre/postfix increment/decrement, int/float only) ---- */
    { "++/--: postfix yields the OLD value, prefix the NEW (int)",
      { "var x = 5; var a = x++; assert(a == 5 && x == 6);",
        "var y = 5; var b = ++y; assert(b == 6 && y == 6);",
        "var z = 5; var c = z--; assert(c == 5 && z == 4);",
        "var w = 5; var d = --w; assert(d == 4 && w == 4);" } },
    { "++/--: float",
      { "var x = 2.5; x++; assert(x == 3.5);",
        "x--; x--; assert(x == 1.5);",
        "var a = x++; assert(a == 1.5 && x == 2.5);" } },
    { "++/--: negative values",
      { "var x = -3; x++; assert(x == -2);",
        "var y = -3; --y; assert(y == -4);" } },
    { "++/--: repeated, as statements",
      { "var x = 0; x++; x++; x++; assert(x == 3);",
        "x--; assert(x == 2);" } },
    { "++/--: used inside a larger expression",
      { "var x = 3; var r = (x++) + 10; assert(r == 13 && x == 4);",
        "var y = 3; var s = (++y) * 2; assert(s == 8 && y == 4);" } },
    { "++/--: as a for-loop counter",
      { "var s = 0; for (var i = 0; i < 5; i++) { s += i; }",
        "assert(s == 10);",
        "var t = 0; for (var i = 10; i > 0; i--) { t += 1; }",
        "assert(t == 10);" } },
    { "++/--: on an array element (general + flat storage)",
      { "var a = [1, 2, 3]; var o = a[1]++; assert(o == 2 && a[1] == 3);",
        "++a[0]; assert(a[0] == 2);",
        "assert(array_storage(a) == \"ints\");",   /* stays flat */
        "var b = [[1, 2], [3, 4]]; b[0][1]++; assert(b[0][1] == 3);" } },
    { "++/--: on a struct field",
      { "struct P { int x; int y; }",
        "var p = P(1, 2); var o = p.x++; assert(o == 1 && p.x == 2);",
        "--p.y; assert(p.y == 1);" } },
    { "++/--: on a default-dict element (pins the value type, like += 1)",
      { "var d = dict(0);",
        "foreach (var k in [\"a\", \"b\", \"a\"]) { d[k]++; }",
        "assert(d[\"a\"] == 2 && d[\"b\"] == 1);" } },
    { "++/--: on a function's scalar param (local copy)",
      { "func f(n) { n++; return n; }",
        "assert(f(5) == 6);",
        "var a = 5; f(a); assert(a == 5);" } },   /* caller untouched */
    { "++/--: a dyn holding an int works",
      { "var dyn d = 5; d++; assert(d == 6); --d; assert(d == 5);" } },
    { "++/--: a dyn holding a non-number throws at runtime",
      { "var dyn d = \"s\"; d++;" }, &typeid(TypeErrorEx) },

    /* error cases (compile-time unless noted) */
    { "++ on a bool is a type error",
      { "var b = true; b++;" }, &typeid(TypeMismatchEx) },
    { "++ on a string is a type error",
      { "var s = \"a\"; s++;" }, &typeid(TypeMismatchEx) },
    { "++ on an array is a type error",
      { "var a = [1, 2]; a++;" }, &typeid(TypeMismatchEx) },
    { "++ on a non-lvalue expression is rejected",
      { "var x = 5; var y = (x + 1)++;" }, &typeid(TypeMismatchEx) },
    { "++ on a const is rejected",
      { "const c = 5; c++;" }, &typeid(TypeMismatchEx) },

    /* optimization interactions */
    { "++/--: a ++'d var is NOT auto-const-promoted",
      { "var x = 5; x++; assert(isconst(x) == false);" } },
    { "++/--: a pure function with a ++ loop folds at a const call",
      { "pure func sumto(n) { var s = 0; var i = 0;"
        " while (i < n) { s += i; i++; } return s; }",
        "var r = sumto(5);",
        "assert(isconst(r) && r == 10);" } },
    { "++/--: a scalar-param-mutating func is NOT inlined (sound)",
      { "func f(x) => x++;",   /* x++ on the param: must stay a call */
        "var a = 5; var r = f(a); assert(a == 5 && r == 5);" } },
    { "++/--: a through-param mutation inlines to the same effect",
      { "func g(arr) => arr[0]++;",
        "var a = [10, 20]; var r = g(a);",
        "assert(r == 10 && a[0] == 11);" } },   /* ref semantics, like a call */

    /* ---- bitwise & shift operators (~ & | ^ << >> >>>), int only ---- */
    { "bitwise: & | ^ ~ basics",
      { "assert((5 & 3) == 1);",
        "assert((5 | 2) == 7);",
        "assert((5 ^ 1) == 4);",
        "assert(~5 == -6 && ~0 == -1 && ~(-1) == 0);" } },
    { "bitwise: AND/OR/XOR chains",
      { "assert((1 | 2 | 4) == 7);",
        "assert((7 & 6 & 4) == 4);",
        "assert((1 ^ 2 ^ 3) == 0);" } },
    { "shift: << and the two right shifts",
      { "assert((1 << 4) == 16 && (5 << 1) == 10);",
        "assert((16 >> 2) == 4 && (16 >>> 2) == 4);" } },
    { "shift: >> sign-extends, >>> zero-fills (negative)",
      { "assert((-8 >> 1) == -4);",            /* arithmetic */
        "assert((-8 >>> 60) == 15);",          /* logical */
        "assert((-1 >>> 63) == 1);" } },
    { "shift: count past the width saturates (no UB)",
      { "assert((1 << 64) == 0 && (1 << 100) == 0);",
        "assert((5 >>> 100) == 0);",
        "assert((-1 >> 64) == -1 && (4 >> 64) == 0);" } },
    { "shift: a negative count throws at runtime",
      { "var n = -1; var x = 1 << n;" }, &typeid(InvalidValueEx) },
    { "bitwise: bool operands promote to int",
      { "assert((true & true) == 1);",
        "assert((true | false) == 1);",
        "assert(~true == -2 && (true << 2) == 4);" } },
    { "bitwise: C precedence (== over &, & over ^ over |, shift over <)",
      { "assert((5 & 3 == 1) == 0);",      /* 5 & (3==1) = 5 & 0 = 0 */
        "assert((1 | 2 & 3) == 3);",       /* 1 | (2&3) */
        "assert((1 ^ 1 & 0) == 1);",       /* 1 ^ (1&0) */
        "assert((1 + 2 << 1) == 6);",      /* (1+2) << 1 */
        "assert((2 < 1 << 2) == true);" } },  /* 2 < (1<<2) */
    { "bitwise: ~ binds tightest (unary)",
      { "assert((~5 + 1) == -5);",         /* (~5) + 1 */
        "assert((~5 & 3) == 2);" } },      /* (~5) & 3 */
    { "bitwise: classic set/clear/test/toggle bit idioms",
      { "var x = 0;",
        "x = x | (1 << 3); assert(x == 8);",           /* set bit 3 */
        "assert(((x >> 3) & 1) == 1);",                /* test bit 3 */
        "x = x ^ (1 << 3); assert(x == 0);",           /* toggle off */
        "var y = 15; y = y & ~(1 << 1); assert(y == 13);" } },  /* clear */
    { "bitwise: works on non-const values (M8-specialized path)",
      { "func mix(a, b) => (a & b) | (a << 1) ^ (b >>> 1);",
        "assert(mix(5, 3) == 11);" } },
    /* type errors: bitwise is int-only */
    { "bitwise: & on a float is a type error",
      { "func f(float x) => x & 1;" }, &typeid(TypeMismatchEx) },
    { "bitwise: << with a float is a type error",
      { "func f(float x) => 1 << x;" }, &typeid(TypeMismatchEx) },
    { "bitwise: ~ on a float is a type error",
      { "func f(float x) => ~x;" }, &typeid(TypeMismatchEx) },
    { "bitwise: | on a string is a type error (const-folded)",
      { "var x = \"a\" | 2;" }, &typeid(TypeErrorEx) },

    /* ---- for-range loop specialization (ForRangeStmt) ---- */
    { "for-range: ascending (i++) is correct",
      { "var s = 0; for (var i = 0; i < 5; i++) { s += i; }",
        "assert(s == 10);" } },
    { "for-range: descending (i--) is correct",
      { "var s = 0; for (var i = 10; i >= 0; i--) { s += i; }",
        "assert(s == 55);" } },
    { "for-range: step (i += k / i -= k)",
      { "var s = 0; for (var i = 0; i < 10; i += 2) { s += i; }",
        "assert(s == 20);",                              /* 0+2+4+6+8 */
        "var t = 0; for (var i = 9; i >= 0; i -= 3) { t += i; }",
        "assert(t == 18);" } },                          /* 9+6+3+0 */
    { "for-range: break stops the loop",
      { "var a = 0;",
        "for (var i = 0; i < 100; i++) { if (i == 5) { break; } a += i; }",
        "assert(a == 10);" } },
    { "for-range: continue still runs the step",
      { "var b = 0;",
        "for (var i = 0; i < 10; i++) {"
        " if (i % 2 == 1) { continue; } b += i; }",
        "assert(b == 20);" } },
    { "for-range: return out of the loop",
      { "func f() { for (var i = 0; i < 100; i++) { if (i == 7) { return i; } }"
        " return -1; }",
        "assert(f() == 7);" } },
    { "for-range: a body that reassigns the loop var is respected",
      { "var s = 0; for (var i = 0; i < 10; i++) { s += i; i += 1; }",
        "assert(s == 20);" } },                          /* i = 0,2,4,6,8 */
    { "for-range: nested counted loops",
      { "var s = 0;",
        "for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) {"
        " s += i * j; } }",
        "assert(s == 9);" } },
    { "for-range: zero-iteration loops",
      { "var s = 0; for (var i = 5; i < 5; i++) { s += 1; } assert(s == 0);",
        "for (var i = 0; i >= 5; i--) { s += 1; } assert(s == 0);" } },
    { "for-range: bound is an immutable expression, evaluated once",
      { "var n = 5; var s = 0; for (var i = 0; i < n * 2; i++) { s += 1; }",
        "assert(s == 10);" } },
    { "for-range: a body that MUTATES the bound stays correct (general loop)",
      { "var n = 5; var c = 0;",
        "for (var i = 0; i < n; i++) { c += 1; if (i == 2) { n = 10; } }",
        "assert(c == 10);" } },     /* bound re-read each iter -> 10 iters */
    { "for-range: <= and > (inclusive / exclusive) forms",
      { "var s = 0; for (var i = 0; i <= 5; i++) { s += i; } assert(s == 15);",
        "var t = 0; for (var i = 5; i > 0; i--) { t += i; }",
        "assert(t == 15);" } },
    { "for-range: a len(arr) bound (pure builtin, arr not mutated)",
      { "var a = [10, 20, 30, 40]; var s = 0;",
        "for (var i = 0; i < len(a); i++) { s += a[i]; }",
        "assert(s == 100);" } },
    { "for-range: a fill `arr[i]=v` with len(arr) bound stays correct",
      { "var a = [0, 0, 0, 0, 0];",
        "for (var i = 0; i < len(a); i++) { a[i] = i * i; }",
        "assert(a[4] == 16 && a[2] == 4);" } },
    { "for-range: a body that GROWS the array stays correct (general loop)",
      { "var a = [1, 2, 3]; var c = 0;",
        "for (var i = 0; i < len(a); i++) { c += 1;"
        " if (i == 0) { append(a, 9); } }",
        "assert(c == 4);" } },   /* len re-read: grew to 4 -> 4 iterations */
    { "for-range: a pure user-function bound with a scalar arg",
      { "func lim(int x) { var t = x * 2; return t; }",
        "var s = 0; for (var i = 0; i < lim(4); i++) { s += i; }",
        "assert(s == 28);" } },   /* lim(4) = 8 -> sum 0..7 */
    { "for-range: a pure func that MUTATES a container bound stays correct",
      { "func shrink(array a) { a[0] = a[0] - 1; return a[0]; }",
        "var x = [10, 0, 0]; var c = 0;",
        "for (var i = 0; i < shrink(x); i++) { c += 1; }",
        "assert(c == 5);" } },  /* must re-eval (5), not cache shrink(x)=9 */

    /* ---- evaluator (eval.cpp) ---- */
    {
        "string escape sequences (\\t \\v \\a \\b and unknown)",
        {
            "var s = \"\\t\\v\\a\\b\";",
            "assert(len(s) == 4);",
            "assert(s[0] == \"\\t\");",
            "var u = \"\\q\";",          /* unknown escape keeps both chars */
            "assert(len(u) == 2);",
        },
    },
    {
        "runtime recursion (non-folded function calls)",
        {
            "func fib(n) {",
            "   if (n < 2) { return n; }",
            "   return fib(n - 1) + fib(n - 2);",
            "}",
            "var k = 10;",            /* runtime arg, so fib() isn't folded */
            "assert(fib(k) == 55);",
        },
    },
    {
        "arrow-body function at runtime",
        {
            "func sq(a) => a * a;",
            "var k = 7;",
            "assert(sq(k) == 49);",
        },
    },
    {
        "closure capturing state at runtime",
        {
            "func make_counter() {",
            "   var n = 0;",
            "   return func [n]() { n += 1; return n; };",
            "}",
            "var c = make_counter();",
            "assert(c() == 1);",
            "assert(c() == 2);",
        },
    },
    { "calling a function with too many args is rejected",
      { "func f(a) { return a; } var k=1; f(k, 2);" },
      &typeid(WrongArgCountEx) },
    { "calling a function with too few args is rejected",
      { "func f(a, b) { return a; } var k=1; f(k);" },
      &typeid(WrongArgCountEx) },
    { "assigning to an undefined variable is an error",
      { "nonexistent = 5;" }, &typeid(UndefinedVariableEx) },
    { "foreach over a non-iterable is a type error",
      { "var n = 5; foreach (var x in n) { }" }, &typeid(TypeErrorEx) },
    { "member access on a non-dict is a type error",
      { "var x = 5; var y = x.foo;" }, &typeid(TypeMismatchEx) },
    { "duplicate declaration in the same block is rejected",
      { "var x = 1; var x = 2;" }, &typeid(AlreadyDefinedEx) },
    {
        "if/else with the else branch taken at runtime",
        {
            "var c = 0; var r = 0;",
            "if (c) { r = 1; } else { r = 2; }",
            "assert(r == 2);",
        },
    },
    {
        "block scoping: an inner var shadows an outer one",
        {
            "var x = 1;",
            "{ var x = 2; assert(x == 2); }",
            "assert(x == 1);",
        },
    },
    {
        "multiple-assignment spreads a scalar to each target",
        {
            "var a, b, c = 7;",
            "assert(a == 7 && b == 7 && c == 7);",
        },
    },

    /* ---- parser (parser.cpp) syntax-error paths ---- */
    { "binary + with no rhs",  { "var x = 1 +;" }, &typeid(SyntaxErrorEx) },
    { "binary * with no rhs",  { "var x = 1 *;" }, &typeid(SyntaxErrorEx) },
    { "binary < with no rhs",  { "var x = 1 <;" }, &typeid(SyntaxErrorEx) },
    { "binary == with no rhs", { "var x = 1 ==;" }, &typeid(SyntaxErrorEx) },
    { "binary && with no rhs", { "var x = 1 &&;" }, &typeid(SyntaxErrorEx) },
    { "binary || with no rhs", { "var x = 1 ||;" }, &typeid(SyntaxErrorEx) },
    { "unary - with no operand", { "var x = -;" }, &typeid(SyntaxErrorEx) },
    { "unary ! with no operand", { "var x = !;" }, &typeid(SyntaxErrorEx) },
    { "open paren with no expr",  { "var x = (;" }, &typeid(SyntaxErrorEx) },
    { "member access of a non-identifier",
      { "var a = 0; var y = a.5;" }, &typeid(SyntaxErrorEx) },
    { "empty subscript",
      { "var a = [1]; var y = a[];" }, &typeid(SyntaxErrorEx) },
    { "var with no identifier", { "var ;" }, &typeid(SyntaxErrorEx) },
    { "var with no rvalue", { "var a = ;" }, &typeid(SyntaxErrorEx) },
    { "id-list target without '='", { "var a, b 5;" }, &typeid(SyntaxErrorEx) },
    { "const initialized with a non-const expression",
      { "const x = rand(1, 2);" }, &typeid(ExpressionIsNotConstEx) },
    { "func declaration without a block",
      { "func f()" }, &typeid(SyntaxErrorEx) },
    { "foreach with no loop variable",
      { "foreach () { }" }, &typeid(SyntaxErrorEx) },
    { "foreach without the `in` keyword",
      { "foreach (var x y) { }" }, &typeid(SyntaxErrorEx) },
    { "try with no block", { "try x;" }, &typeid(SyntaxErrorEx) },
    { "try block with no catch/finally", { "try { }" }, &typeid(SyntaxErrorEx) },
    { "catch with a non-identifier exception",
      { "try { } catch (5) { }" }, &typeid(SyntaxErrorEx) },
    { "catch `as` a non-identifier",
      { "try { } catch (Foo as 5) { }" }, &typeid(SyntaxErrorEx) },
    { "catch with no block",
      { "try { } catch (Foo) x;" }, &typeid(SyntaxErrorEx) },

    /* ---- function value type (func.cpp.h) ---- */
    {
        "function value: to_string / equality / truthiness / clone",
        {
            "var f = func() { };",
            "var g = func() { };",
            "assert(str(f) == \"<function>\");",
            "assert(f == f);",
            "assert((f == g) == false);",
            "assert(f != g);",
            "assert((f == 5) == false);",
            "assert((f != 5) == true);",
            "if (f) { assert(true); } else { assert(false); }",  /* is_true */
            "var h = clone(f);",
        },
    },

    /* ---- string operators (str.cpp.h) type errors ---- */
    { "string * non-integer is a type error",
      { "\"ab\" * \"x\";" }, &typeid(TypeErrorEx) },
    { "string < non-string is a type error",
      { "\"a\" < 5;" }, &typeid(TypeErrorEx) },
    { "string > non-string is a type error",
      { "\"a\" > 5;" }, &typeid(TypeErrorEx) },
    { "string <= non-string is a type error",
      { "\"a\" <= 5;" }, &typeid(TypeErrorEx) },
    { "string >= non-string is a type error",
      { "\"a\" >= 5;" }, &typeid(TypeErrorEx) },
    { "string subscript with non-integer is a type error",
      { "\"abc\"[\"x\"];" }, &typeid(TypeErrorEx) },
    { "string slice with non-integer start is a type error",
      { "\"abc\"[\"x\":];" }, &typeid(TypeErrorEx) },
    { "string slice with non-integer end is a type error",
      { "\"abc\"[1:\"y\"];" }, &typeid(TypeErrorEx) },

    /* ---- int operators (int.cpp.h) type errors / div-by-zero ---- */
    { "int + non-int is a type error",  { "5 + \"x\";" }, &typeid(TypeErrorEx) },
    { "int - non-int is a type error",  { "5 - \"x\";" }, &typeid(TypeErrorEx) },
    { "int * non-int is a type error",  { "5 * \"x\";" }, &typeid(TypeErrorEx) },
    { "int / non-int is a type error",  { "5 / \"x\";" }, &typeid(TypeErrorEx) },
    { "int % non-int is a type error",  { "5 % \"x\";" }, &typeid(TypeErrorEx) },
    { "int < non-int is a type error",  { "5 < \"x\";" }, &typeid(TypeErrorEx) },
    { "int > non-int is a type error",  { "5 > \"x\";" }, &typeid(TypeErrorEx) },
    { "int <= non-int is a type error", { "5 <= \"x\";" }, &typeid(TypeErrorEx) },
    { "int >= non-int is a type error", { "5 >= \"x\";" }, &typeid(TypeErrorEx) },
    { "int division by zero",           { "5 / 0;" }, &typeid(DivisionByZeroEx) },
    { "int modulo by zero",             { "5 % 0;" }, &typeid(DivisionByZeroEx) },

    /* ---- base Type virtuals (type.h): unsupported-operation errors ---- */
    /* With type inference, `none <op> x` on a none-typed value is caught at
     * compile time as a NullabilityEx (was a runtime TypeErrorEx). */
    { "none + x is unsupported", { "var n=none; var y=n+1;" }, &typeid(NullabilityEx) },
    { "none - x is unsupported", { "var n=none; var y=n-1;" }, &typeid(NullabilityEx) },
    { "none * x is unsupported", { "var n=none; var y=n*1;" }, &typeid(NullabilityEx) },
    { "none / x is unsupported", { "var n=none; var y=n/1;" }, &typeid(NullabilityEx) },
    { "none % x is unsupported", { "var n=none; var y=n%1;" }, &typeid(NullabilityEx) },
    { "none < x is unsupported", { "var n=none; var y=n<1;" }, &typeid(NullabilityEx) },
    { "none > x is unsupported", { "var n=none; var y=n>1;" }, &typeid(NullabilityEx) },
    { "none <= x is unsupported",{ "var n=none; var y=n<=1;" }, &typeid(NullabilityEx) },
    { "none >= x is unsupported",{ "var n=none; var y=n>=1;" }, &typeid(NullabilityEx) },
    { "unary - on none is unsupported",
      { "var n=none; var y=-n;" }, &typeid(NullabilityEx) },
    /* subscripting/slicing a non-container is now a compile-time TypeMismatch. */
    { "subscript on a non-container is unsupported",
      { "var x=5; var y=x[0];" }, &typeid(TypeMismatchEx) },
    { "slice on a non-container is unsupported",
      { "var x=5; var y=x[0:1];" }, &typeid(TypeMismatchEx) },
    /* deep, by-value hash of containers/structs (universal hashing) */
    { "hash: an array hashes deeply and is order-dependent",
      { "assert(hash([1,2,3]) == hash([1,2,3]));",
        "assert(hash([1,2]) != hash([2,1]));",
        "assert(hash([[1,2],[3,4]]) == hash([[1,2],[3,4]]));" } },
    { "hash: none is hashable (a stable value)",
      { "assert(hash(none) == hash(none));" } },
    { "hash: a dict hashes regardless of insertion order",
      { "var a = {}; a[\"x\"] = 1; a[\"y\"] = 2;",
        "var b = {}; b[\"y\"] = 2; b[\"x\"] = 1;",
        "assert(hash(a) == hash(b));",
        "assert(a == b);" } },
    { "hash: a struct hashes deeply, order-dependent on fields",
      { "struct P { int x; int y; }",
        "assert(hash(P(1,2)) == hash(P(1,2)));",
        "assert(hash(P(1,2)) != hash(P(2,1)));" } },
    { "hash: equal values hash equal (1 and 1.0)",
      { "assert(hash(1) == hash(1.0));" } },

    /* any-type dict keys (enabled by universal hashing) + key-freeze safety */
    { "dict key: an array can be a key",
      { "var d = {}; d[[1,2]] = 5; d[[3,4]] = 6;",
        "assert(d[[1,2]] == 5);",
        "assert(d[[3,4]] == 6);",
        "assert(len(d) == 2);" } },
    { "dict key: a struct can be a key",
      { "struct P { int x; int y; }",
        "var d = {}; d[P(1,2)] = \"a\"; d[P(3,4)] = \"b\";",
        "assert(d[P(1,2)] == \"a\");",
        "assert(d[P(3,4)] == \"b\");" } },
    { "dict key: none can be a key",
      { "var d = {}; d[none] = 7;",
        "assert(d[none] == 7);" } },
    { "dict key: mutating the ORIGINAL after insert does NOT corrupt the dict",
      { "var k = [1, 2]; var d = {}; d[k] = 5;",
        "k[0] = 9;",                       /* mutate the original key */
        "assert(d[[1,2]] == 5);",          /* stored key is frozen, intact */
        "assert(len(d) == 1);" } },
    { "dict key: a struct key mutated after insert stays intact",
      { "struct P { int x; int y; }",
        "var p = P(1, 2); var d = {}; d[p] = 8;",
        "p.x = 9;",
        "assert(d[P(1,2)] == 8);" } },
    { "dict key: an array key in a {} literal is frozen",
      { "var k = [1]; var d = { k: 7 };",   /* key is k's VALUE, [1] */
        "k[0] = 2;",
        "assert(d[[1]] == 7);" } },
    { "dict key: dict() from pairs freezes array keys",
      { "var k = [1]; var d = dict([[k, 9]]);",
        "k[0] = 2;",
        "assert(d[[1]] == 9);" } },

    /* incremental hash cache for FLAT-scalar arrays: the cached hash must stay
     * correct across every mutation (maintained on append, invalidated
     * otherwise). `hash(deepclone(a))` recomputes from scratch, so equality
     * proves the cache is not stale. */
    { "hash cache: append maintains the hash (and it actually changes)",
      { "var a = [1,2,3]; var h0 = hash(a);",
        "append(a, 4);",
        "assert(hash(a) == hash(deepclone(a)));",
        "assert(hash(a) != h0);" } },
    { "hash cache: pop invalidates correctly",
      { "var a = [1,2,3]; hash(a); pop(a);",
        "assert(hash(a) == hash(deepclone(a)));" } },
    { "hash cache: insert invalidates correctly",
      { "var a = [1,2,3]; hash(a); insert(a, 1, 9);",
        "assert(hash(a) == hash(deepclone(a)));" } },
    { "hash cache: erase invalidates correctly",
      { "var a = [1,2,3]; hash(a); erase(a, 0);",
        "assert(hash(a) == hash(deepclone(a)));" } },
    { "hash cache: element write a[i]=v invalidates",
      { "var a = [1,2,3]; hash(a); a[1] = 99;",
        "assert(hash(a) == hash(deepclone(a)));" } },
    { "hash cache: a[i]+=v and a[i]++ invalidate",
      { "var a = [1,2,3]; hash(a); a[1] += 5; a[2]++;",
        "assert(hash(a) == hash(deepclone(a)));" } },
    { "hash cache: sort and reverse invalidate",
      { "var a = [3,1,2]; hash(a); sort(a);",
        "assert(hash(a) == hash(deepclone(a)));",
        "var b = [1,2,3]; hash(b); reverse(b);",
        "assert(hash(b) == hash(deepclone(b)));" } },
    { "hash cache: += (concat) invalidates",
      { "var a = [1,2,3]; hash(a); a += [4,5];",
        "assert(hash(a) == hash(deepclone(a)));" } },
    { "hash cache: float and bool flat arrays too",
      { "var f = [1.0, 2.0]; hash(f); append(f, 3.0); f[0] = 9.0;",
        "assert(hash(f) == hash(deepclone(f)));",
        "var b = [true, false]; hash(b); b[0] = false; append(b, true);",
        "assert(hash(b) == hash(deepclone(b)));" } },
    { "hash cache: incremental append over many elements matches recompute",
      { "var a = []; var i = 0;",
        "while (i < 64) { append(a, i * i - 3); i++; }",
        "assert(hash(a) == hash(deepclone(a)));" } },
    /* general/struct/nested arrays are NOT cached (no back-pointer to
     * invalidate an enclosing container) - they recompute, so a nested mutation
     * is reflected. */
    { "hash cache: a nested array mutation is reflected (not cached stale)",
      { "var a = [[1,2],[3,4]]; var h0 = hash(a); a[0][1] = 99;",
        "assert(hash(a) == hash(deepclone(a)));",
        "assert(hash(a) != h0);" } },
    { "hash cache: a struct element replace is reflected",
      { "struct P { int x; int y; }",
        "var a = [P(1,2), P(3,4)]; var h0 = hash(a); a[0] = P(9,9);",
        "assert(hash(a) == hash(deepclone(a)));",
        "assert(hash(a) != h0);" } },
    { "hash cache: a const (read-only) flat array hashes stably",
      { "const c = [1,2,3];",
        "assert(hash(c) == hash(c));",
        "assert(hash(c) == hash([1,2,3]));" } },

    /* ---- float operators (float.cpp.h) ---- */
    { "float + non-numeric is a type error",
      { "2.5 + \"x\";" }, &typeid(TypeErrorEx) },
    { "float / 0.0 is division by zero",
      { "2.5 / 0.0;" }, &typeid(DivisionByZeroEx) },
    {
        "float equality / hashing with non-numerics",
        {
            "assert((2.5 == \"x\") == false);",
            "assert((2.5 != \"x\") == true);",
            "assert(hash(2.5) == hash(2.5));",
        },
    },

    /* ---- generic builtins (generic.cpp.h) error & runtime paths ---- */
    { "len() with no args is rejected",
      { "len();" }, &typeid(InvalidNumberOfArgsEx) },
    { "defined() with no args is rejected",
      { "defined();" }, &typeid(InvalidNumberOfArgsEx) },
    { "str() with no args is rejected",
      { "str();" }, &typeid(InvalidNumberOfArgsEx) },
    { "str() of a float with negative precision is a type error",
      { "str(3.5, -1);" }, &typeid(TypeErrorEx) },
    { "str() of a float with too-high precision is a type error",
      { "str(3.5, 99);" }, &typeid(TypeErrorEx) },
    { "runtime() with no args is rejected",
      { "runtime();" }, &typeid(InvalidNumberOfArgsEx) },
    {
        "isconst() / isconstdecl() on params (auto-const vs reassigned)",
        {
            /* A never-reassigned param is auto-const: isconst true (per the
             * README), isconstdecl false (not const by declaration). The fold
             * is consistent in a `return` as anywhere else. */
            "func f(x) { return isconst(x); }",
            "func g(x) { return isconstdecl(x); }",
            /* A reassigned param is genuinely non-const: isconst false. */
            "func h(x) { x = x + 1; return isconst(x); }",
            "assert(f(5) == true);",
            "assert(g(5) == false);",
            "assert(h(5) == false);",
        },
    },
    {
        "ispure() / ispuredecl() report on a function value",
        {
            "pure func p() => 1;",
            "func q() => 1;",
            "assert(ispuredecl(p));",
            "assert(ispuredecl(q) == false);",
            "assert(ispure(p));",
        },
    },
    /* auto-pure propagates: a function calling an earlier auto-pure helper is
     * itself recognized pure, so the whole pure chain const-folds (-O3-like) */
    { "auto-pure: a func calling an auto-pure helper is recognized pure",
      { "func add(a, b) => a + b;",
        "func f(x, y) => add(x, y);",
        "assert(ispure(add) && ispure(f));" } },
    { "auto-pure: a pure chain with literal args const-folds",
      { "func add(a, b) => a + b;",
        "func f(x, y) => add(x, 2 * y);",
        "var r = f(1, 2);",
        "assert(isconst(r) && r == 5);" } },
    { "auto-pure: a 3-level pure chain folds",
      { "func a(x) => x + 1; func b(x) => a(x) * 2;",
        "func c(x) => b(x) + a(x);",
        "var r = c(5); assert(isconst(r) && r == 18);" } },
    { "auto-pure: an impure callee keeps the caller impure",
      { "func imp(x) { print(x); return x; }",
        "func u(x) => imp(x) + 1;",
        "assert(ispure(imp) == false && ispure(u) == false);" } },
    { "auto-pure: a self-recursive function stays conservative (not pure)",
      { "func fac(n) { if (n < 2) { return 1; } return n * fac(n - 1); }",
        "assert(ispure(fac) == false);" } },
    /* a pure call inside an EXPRESSION-bodied function folds (the body is a
     * bare expression, which AutoConst used to skip) */
    { "auto-const: a pure call in an expression-bodied function folds",
      { "func aa(a, b) => a + b;",
        "func g() => aa(1, 2);",
        "assert(g() == 3);",
        "assert(startswith(show(g), \"/* pure */ func g() => 3\"));" } },
    /* pure forbids mutating a reference (array/dict/struct) parameter - that
     * is an observable side effect; scalar-param and fresh-local mutation are
     * fine. (see func_mutates_input in the resolver) */
    { "pure: mutating an array param is NOT pure",
      { "func f(a) { a[0] = 9; return a[0]; }",
        "assert(ispure(f) == false);" } },
    { "pure: mutating an array param via append is NOT pure",
      { "func f(a) { append(a, 9); return len(a); }",
        "assert(ispure(f) == false);" } },
    { "pure: a fresh-local builder that writes its OWN local stays pure",
      { "func mk(n) { var r = [0, 0]; r[0] = n; return r; }",
        "assert(ispure(mk));" } },
    { "pure: reading an array param stays pure",
      { "func d(a) => a[0] + a[1];",
        "assert(ispure(d));" } },
    { "pure: an alias of a param then mutate is NOT pure",
      { "func g(a) { var b = a; b[0] = 9; return b[0]; }",
        "assert(ispure(g) == false);" } },
    { "pure: a param stored in a literal then deep-mutated is NOT pure",
      { "func dp(a) { var r = [a]; r[0][0] = 9; return r[0][0]; }",
        "assert(ispure(dp) == false);" } },
    { "pure: mutating a SCALAR param (by copy) stays pure",
      { "func h(x) { x = x + 1; return x * 2; }",
        "assert(ispure(h));" } },
    { "pure: mutating a struct param field is NOT pure",
      { "struct P { int x; int y; }",
        "func sf(p) { p.x = 5; return p.x; }",
        "assert(ispure(sf) == false);" } },
    { "pure: an explicit 'pure func' that mutates input - ispuredecl still "
      "true, ispure false",
      { "pure func pf(a) { a[0] = 1; return a[0]; }",
        "assert(ispuredecl(pf));",
        "assert(ispure(pf) == false);" } },
    { "ispure() of a non-function is a type error",
      { "ispure(5);" }, &typeid(TypeErrorEx) },
    { "ispure() with no args is rejected",
      { "ispure();" }, &typeid(InvalidNumberOfArgsEx) },
    { "ispuredecl() of a non-function is a type error",
      { "ispuredecl(5);" }, &typeid(TypeErrorEx) },
    { "intptr() with no args is rejected",
      { "intptr();" }, &typeid(InvalidNumberOfArgsEx) },
    { "intptr() of a non-lvalue is rejected",
      { "intptr(5);" }, &typeid(NotLValueEx) },
    { "undef() with no args is rejected",
      { "undef();" }, &typeid(InvalidNumberOfArgsEx) },
    { "undef() of a non-identifier is a type error",
      { "undef(5);" }, &typeid(TypeErrorEx) },
    { "assert(false) fails",
      { "assert(false);" }, &typeid(AssertionFailureEx) },
    { "assert() with no args is rejected",
      { "assert();" }, &typeid(InvalidNumberOfArgsEx) },
    {
        "find() / hash() value forms",
        {
            "assert(find([1,2,3], 2) == 1);",
            "assert(find([1,2,3], 9) == none);",
            "assert(find(\"abcd\", \"cd\") == 2);",
            "assert(hash(5) == hash(5));",
            "assert(hash(\"x\") == hash(\"x\"));",
        },
    },
    { "find() with no args is rejected",
      { "find();" }, &typeid(InvalidNumberOfArgsEx) },
    { "find() with a non-function key is a type error",
      { "find([1,2], 1, 5);" }, &typeid(TypeErrorEx) },
    { "find() in a string with a non-string needle is a type error",
      { "find(\"abc\", 5);" }, &typeid(TypeErrorEx) },
    { "find() in an unsupported container is a type error",
      { "find(5, 1);" }, &typeid(TypeErrorEx) },
    { "hash() with no args is rejected",
      { "hash();" }, &typeid(InvalidNumberOfArgsEx) },

    /* ---- runtime-exception clone()/rethrow() (errors.h) via try/catch ---- */
    {
        "NotLValueEx can be caught (as), rethrown, and re-caught",
        {
            "var hit = 0;",
            "try {",
            "   try { append([1,2], 3); }",
            "   catch (NotLValueEx as e) { hit += 1; rethrow; }",
            "} catch (NotLValueEx) { hit += 1; }",
            "assert(hit == 2);",
        },
    },
    {
        "NotCallableEx can be caught (as), rethrown, and re-caught",
        {
            "var hit = 0;",
            "try {",
            /* `dyn` so the not-callable is a runtime (catchable) error, not a
             * compile-time type error. */
            "   try { var dyn x = 5; x(); }",
            "   catch (NotCallableEx as e) { hit += 1; rethrow; }",
            "} catch (NotCallableEx) { hit += 1; }",
            "assert(hit == 2);",
        },
    },
    {
        /* Each must be triggered at RUNTIME (rand keeps the operand unfolded),
           else it would throw at parse and escape the try. */
        "clone/rethrow for the catchable runtime exceptions",
        {
            "var n = 0; var z = rand(0, 0);",
            "try { try { var q = 5 / z; }",
            "  catch (DivisionByZeroEx as e) { n += 1; rethrow; } }",
            "catch (DivisionByZeroEx) { n += 1; }",
            "assert(n == 2);",

            "n = 0; var a = [1, 2]; var i = 10;",
            "try { try { var q = a[i]; }",
            "  catch (OutOfBoundsEx as e) { n += 1; rethrow; } }",
            "catch (OutOfBoundsEx) { n += 1; }",
            "assert(n == 2);",

            "n = 0; var dyn s = str(rand(0, 0));",
            "try { try { var dyn q = 5 + s; }",
            "  catch (TypeErrorEx as e) { n += 1; rethrow; } }",
            "catch (TypeErrorEx) { n += 1; }",
            "assert(n == 2);",

            "n = 0; var f = false;",
            "try { try { assert(f); }",
            "  catch (AssertionFailureEx as e) { n += 1; rethrow; } }",
            "catch (AssertionFailureEx) { n += 1; }",
            "assert(n == 2);",

            "n = 0;",
            "try { try { var q = range(0, 5, rand(0, 0)); }",
            "  catch (InvalidValueEx as e) { n += 1; rethrow; } }",
            "catch (InvalidValueEx) { n += 1; }",
            "assert(n == 2);",

            "n = 0;",
            "try { try { read(\"no_such_file_clone.tmp\"); }",
            "  catch (CannotOpenFileEx as e) { n += 1; rethrow; } }",
            "catch (CannotOpenFileEx) { n += 1; }",
            "assert(n == 2);",
        },
    },

    /* ---- numeric / math builtins (num.cpp.h) ---- */
    {
        "int() / float() / abs() conversions",
        {
            "assert(int(3.9) == 3);",
            "assert(int(\"42\") == 42);",
            "assert(int(7) == 7);",
            "assert(float(3) == 3.0);",
            "assert(float(\"3.5\") == 3.5);",
            "assert(float(2.5) == 2.5);",
            "assert(abs(-5) == 5);",
            "assert(abs(5) == 5);",
            "assert(abs(-2.5) == 2.5);",
        },
    },
    { "int() of a non-numeric string is a type error",
      { "int(\"abc\");" }, &typeid(TypeErrorEx) },
    { "int() of an unsupported type is a type error",
      { "int([1]);" }, &typeid(TypeErrorEx) },
    { "int() with no args is rejected",
      { "int();" }, &typeid(InvalidNumberOfArgsEx) },
    { "float() of a non-numeric string is a type error",
      { "float(\"xyz\");" }, &typeid(TypeErrorEx) },
    { "float() of an unsupported type is a type error",
      { "float([1]);" }, &typeid(TypeErrorEx) },
    { "abs() of an unsupported type is a type error",
      { "abs(\"x\");" }, &typeid(TypeErrorEx) },
    { "float() with no args is rejected",
      { "float();" }, &typeid(InvalidNumberOfArgsEx) },
    { "abs() with no args is rejected",
      { "abs();" }, &typeid(InvalidNumberOfArgsEx) },

    {
        "min() / max() over arrays and varargs",
        {
            "assert(min([3,1,2]) == 1);",
            "assert(max([3,1,2]) == 3);",
            "assert(min(3,1,2) == 1);",
            "assert(max(3,1,2) == 3);",
        },
    },
    { "min() with no args is rejected",
      { "min();" }, &typeid(InvalidNumberOfArgsEx) },
    { "max() with no args is rejected",
      { "max();" }, &typeid(InvalidNumberOfArgsEx) },
    { "min() of a single non-array is a type error",
      { "min(5);" }, &typeid(TypeErrorEx) },
    { "max() of a single non-array is a type error",
      { "max(5);" }, &typeid(TypeErrorEx) },

    {
        "math float builtins (int args accepted)",
        {
            /* Transcendental results are compared with an epsilon: a libm is
               not required to round them to the mathematically exact value, so
               `log10(1000.0) == 3.0` etc. are not portable (they're const-
               folded with whatever libm compiles the test). Only sqrt of a
               perfect square and ceil/floor/trunc are IEEE-exact. */
            "assert(sqrt(16.0) == 4.0);",
            "assert(sqrt(16) == 4.0);",           /* int arg is promoted */
            "assert(ceil(2.1) == 3.0);",
            "assert(floor(2.9) == 2.0);",
            "assert(trunc(2.9) == 2.0);",
            "assert(abs(cbrt(27.0) - 3.0) < 1e-9);",
            "assert(abs(exp(0.0) - 1.0) < 1e-9);",
            "assert(abs(exp2(10.0) - 1024.0) < 1e-9);",
            "assert(abs(log(1.0) - 0.0) < 1e-9);",
            "assert(abs(log2(8.0) - 3.0) < 1e-9);",
            "assert(abs(log10(1000.0) - 3.0) < 1e-9);",
            "assert(abs(pow(2.0, 10.0) - 1024.0) < 1e-9);",
            "assert(abs(sin(0.0) - 0.0) < 1e-9);",
            "assert(abs(cos(0.0) - 1.0) < 1e-9);",
            "assert(abs(tan(0.0) - 0.0) < 1e-9);",
            "assert(abs(asin(0.0) - 0.0) < 1e-9);",
            "assert(abs(acos(1.0) - 0.0) < 1e-9);",
            "assert(abs(atan(0.0) - 0.0) < 1e-9);",
        },
    },
    { "a math builtin on a non-numeric is a type error",
      { "sqrt(\"x\");" }, &typeid(TypeErrorEx) },
    { "a math builtin with the wrong arity is rejected",
      { "sqrt();" }, &typeid(InvalidArgumentEx) },
    { "pow() with one argument is rejected",
      { "pow(2.0);" }, &typeid(InvalidArgumentEx) },

    {
        "isnan / isinf / isfinite / isnormal",
        {
            "assert(isfinite(1.0));",
            "assert(isnan(sqrt(-1.0)));",
            "assert(isinf(log(0.0)));",
            "assert(isnan(1.0) == false);",
            "assert(isinf(1.0) == false);",
            "assert(isnormal(1.0));",
        },
    },

    {
        "round() value forms",
        {
            "assert(round(2.5) == 3.0);",     /* integer results are exact */
            "assert(round(2.4) == 2.0);",
            "assert(round(-2.5) == -3.0);",
            "assert(round(5) == 5.0);",
            /* the digits form divides by a power of 10 -> not exact */
            "assert(abs(round(2.567, 1) - 2.6) < 1e-9);",
        },
    },
    { "round() with no args is rejected",
      { "round();" }, &typeid(InvalidArgumentEx) },
    { "round() of a non-numeric is a type error",
      { "round(\"x\");" }, &typeid(TypeErrorEx) },
    { "round() with negative digits is a type error",
      { "round(2.5, -1);" }, &typeid(TypeErrorEx) },
    { "round() with too many args is rejected",
      { "round(1.0, 2, 3);" }, &typeid(InvalidArgumentEx) },

    {
        "rand() / randf() basics",
        {
            "assert(rand(5, 5) == 5);",       /* lo == hi */
            "assert(rand(9, 1) == none);",    /* lo > hi  */
            "var r = rand(1, 3);",
            "assert(r >= 1 && r <= 3);",
            "assert(randf(2.0, 2.0) == 2.0);",
            "assert(randf(5.0, 1.0) == none);",
            "var f = randf(0.0, 1.0);",
            "assert(f >= 0.0 && f <= 1.0);",
        },
    },
    { "rand() with a non-integer is a type error",
      { "rand(\"x\", 5);" }, &typeid(TypeErrorEx) },
    { "rand() with the wrong arity is rejected",
      { "rand(1);" }, &typeid(InvalidNumberOfArgsEx) },
    { "randf() with a non-float is a type error",
      { "randf(1.0, \"y\");" }, &typeid(TypeErrorEx) },

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

    /*
     * Flat (unboxed) typed-array storage (plans/typed-arrays.md).
     * array_storage() exposes the representation so these pin it.
     */
    {
        "Typed arrays: range()/array(N,v)/make_array pick flat storage",
        {
            "assert(array_storage(range(5)) == \"ints\");",
            "assert(array_storage(range(0, 10, 2)) == \"ints\");",
            "assert(array_storage(array(4, 0)) == \"ints\");",
            "assert(array_storage(array(4, 1.5)) == \"floats\");",
            "assert(array_storage(array(4, \"x\")) == \"general\");",
            "assert(array_storage(array(4)) == \"general\");",
            "assert(array_storage([1,2,3]) == \"ints\");",
            "assert(array_storage([1.0,2.0]) == \"floats\");",
            "assert(array_storage([1,\"x\"]) == \"general\");",
            "assert(array(4, 0) == [0,0,0,0]);",
            "assert(array(3, 2) == [2,2,2]);",
            "assert(array_storage(make_array(5, func(i) => i*i)) == \"ints\");",
            "assert(make_array(5, func(i) => i*i) == [0,1,4,9,16]);",
            "assert(array_storage(make_array(3, func(i) => i*1.0))"
                " == \"floats\");",
            "assert(array_storage(make_array(2, func(i) => \"v\"))"
                " == \"general\");",
            "assert(make_array(0, func(i) => i) == []);",
        },
    },
    {
        /* `array += array` concatenation for each flat storage kind, plus
         * clone/intptr - the flat float/bool paths the struct flat-array work
         * (plans/structs.md phase 7) mirrors. */
        "Typed arrays: flat += concatenation, clone, intptr (float/bool)",
        {
            "var fa = [1.0, 2.0]; fa += [3.0, 4.0];",
            "assert(fa == [1.0, 2.0, 3.0, 4.0]);",
            "assert(array_storage(fa) == \"floats\");",
            "var ba = [true, false]; ba += [true, true];",
            "assert(ba == [true, false, true, true]);",
            "assert(array_storage(ba) == \"bools\");",
            "var ia = [1, 2]; ia += [3, 4]; assert(ia == [1, 2, 3, 4]);",
            "assert(array_storage(ia) == \"ints\");",
            /* concatenation onto a slice clones (the slice branch); slice a
             * variable so it is a live slice view, not a materialized copy */
            "var fbase = [1.0, 2.0, 3.0]; var fs = fbase[0:2]; fs += [9.0];",
            "assert(fs == [1.0, 2.0, 9.0]); assert(fbase == [1.0, 2.0, 3.0]);",
            "var bbase = [true, false, true]; var bs = bbase[0:2];",
            "bs += [true]; assert(bs == [true, false, true]);",
            /* clone keeps flat storage and is independent */
            "var fc = clone([1.0, 2.0]); append(fc, 3.0);",
            "assert(array_storage(fc) == \"floats\"); assert(len(fc) == 3);",
            "var bc = clone([true]); assert(array_storage(bc) == \"bools\");",
            /* intptr identity on flat float/bool arrays */
            "var fp = [1.0]; var fq = fp; assert(intptr(fp) == intptr(fq));",
            "var bp = [true]; var bp2 = clone(bp);",
            "assert(intptr(bp) != intptr(bp2));",
            /* a general array concatenating a flat one (arr_elem_at path) */
            "var dyn ga = [1, \"x\"]; ga += [2, 3];",
            "assert(ga == [1, \"x\", 2, 3]);",
        },
    },
    { "arrays: += with a non-array right side is a type error (runtime)",
      { "var dyn a = [1.0]; a += 5;" }, &typeid(TypeErrorEx) },
    {
        /* the flat float/bool builtin paths (append/pop/insert/erase/sort/
         * reverse/min/max/sum/find/map/filter) that the struct flat-array work
         * mirrors - each must keep the storage flat and stay correct. */
        "Typed arrays: flat float/bool builtin operations",
        {
            "var f = [3.0, 1.0, 2.0];",
            "append(f, 4.0); assert(f == [3.0, 1.0, 2.0, 4.0]);",
            "assert(pop(f) == 4.0);",
            "insert(f, 0, 0.0); assert(f == [0.0, 3.0, 1.0, 2.0]);",
            "erase(f, 1); assert(f == [0.0, 1.0, 2.0]);",
            "sort(f); assert(f == [0.0, 1.0, 2.0]);",
            "reverse(f); assert(f == [2.0, 1.0, 0.0]);",
            "assert(min(f) == 0.0); assert(max(f) == 2.0);",
            "assert(sum(f) == 3.0); assert(find(f, 1.0) == 1);",
            "assert(map(func(x) => x * 2.0, f) == [4.0, 2.0, 0.0]);",
            "assert(filter(func(x) => x > 0.0, f) == [2.0, 1.0]);",
            "assert(array_storage(f) == \"floats\");",
            "var b = [false, true, false];",
            "append(b, true); assert(array_storage(b) == \"bools\");",
            "assert(sum(b) == 2);",
            "reverse(b); assert(b == [true, false, true, false]);",
            "assert(find(b, true) == 0);",
            "sort(b); assert(b == [false, false, true, true]);",
        },
    },
    {
        "Typed arrays: flat subscript store + compound keep storage flat",
        {
            "var a = range(5);",
            "a[2] = 99; a[-1] = 7;",
            "assert(a == [0,1,99,3,7]);",
            "assert(array_storage(a) == \"ints\");",
            "a[0] += 100;",
            "assert(a[0] == 100 && array_storage(a) == \"ints\");",
            "var f = array(3, 0.0);",
            "f[1] = 2.5; f[0] += 1;",
            "assert(array_storage(f) == \"floats\");",
        },
    },
    {
        "Typed arrays: a value used polymorphically is general from creation "
        "(type-driven, no promotion)",
        {
            /* a[1]=\"hello\" makes `a` array<dyn>, so array(3,0) is built
             * general from the start - never flat-then-promoted. */
            "var a = array(3, 0);",
            "a[1] = \"hello\";",
            "assert(a == [0, \"hello\", 0]);",
            "assert(array_storage(a) == \"general\");",
            /* likewise `b`: append(b,\"x\") makes it array<dyn>, so range(3)
             * is general from the start. */
            "var b = range(3);",
            "append(b, 9);",
            "append(b, \"x\");",
            "assert(b == [0,1,2,9,\"x\"]);",
            "assert(array_storage(b) == \"general\");",
        },
    },
    {
        "Typed arrays: flat reads (sum/min/max/reverse/sort/foreach)",
        {
            "assert(sum(range(1, 6)) == 15);",
            "assert(min(range(3, 9)) == 3 && max(range(3, 9)) == 8);",
            "assert(reverse(range(1, 6)) == [5,4,3,2,1]);",
            "var b = range(5);",
            "reverse(b);",
            "assert(array_storage(b) == \"ints\" && b == [4,3,2,1,0]);",
            "sort(b);",
            "assert(array_storage(b) == \"ints\" && b == [0,1,2,3,4]);",
            "var t = 0;",
            "foreach (var x in range(5)) t += x;",
            "assert(t == 10);",
        },
    },
    {
        "Typed arrays: slices share flat storage, COW on write, clones flat",
        {
            "var a = range(5);",
            "var s = a[1:4];",
            "assert(array_storage(s) == \"ints\" && s == [1,2,3]);",
            "a[1] = 100;",
            "assert(s == [1,2,3]);",
            "assert(a == [0,100,2,3,4]);",
            "var c = clone(range(4));",
            "assert(array_storage(c) == \"ints\");",
            "var d = deepclone(range(4));",
            "assert(array_storage(d) == \"ints\");",
        },
    },
    {
        /* `a` is array<int> (its only use is `var dyn d = a`), so it stays
         * flat. d aliases a's flat storage; appending a str would need an
         * in-place flat->general promotion, which mylang does not do - it
         * errors instead (declare the array dyn for a polymorphic array). */
        "Typed arrays: a non-fitting mutation of a dyn-laundered flat array "
        "errors (no promotion)",
        {
            "var a = range(3);",
            "assert(array_storage(a) == \"ints\");",
            "var dyn d = a;",
            "append(d, \"hi\");",
        },
        &typeid(TypeErrorEx),
    },
    {
        "array_storage() rejects a non-array",
        { "array_storage(42);" },
        &typeid(TypeErrorEx),
    },
    {
        /* `var dyn` declares a polymorphic array, so it is built general from
         * the start - a later mixed write does not hit the flat-array error. */
        "Typed arrays: var dyn builds a general (polymorphic) array",
        {
            "var dyn d = [1,2,3];",
            "assert(array_storage(d) == \"general\");",
            "d[0] = \"x\";",
            "assert(d == [\"x\", 2, 3]);",
            "var dyn e = array(3, 0);",
            "e[1] = \"y\";",
            "assert(e == [0, \"y\", 0]);",
        },
    },
    {
        /* dynarray(a) is the manual promotion: a fresh general copy of a flat
         * array. The original stays flat/typed; the copy is independent and
         * polymorphic, typed array<dyn> so a plain `var` is fine. */
        "Typed arrays: dynarray() promotes a flat array to a general copy",
        {
            "var a = range(3);",
            "var d = dynarray(a);",
            "assert(array_storage(a) == \"ints\");",
            "assert(array_storage(d) == \"general\");",
            "d[0] = \"x\";",
            "append(d, 9.5);",
            "assert(d == [\"x\", 1, 2, 9.5]);",
            "assert(a == [0, 1, 2]);",      /* original untouched */
            "var f = dynarray([1.0, 2.0]);", /* also works on float arrays */
            "assert(array_storage(f) == \"general\");",
        },
    },
    {
        "dynarray() rejects a non-array",
        { "dynarray(42);" },
        &typeid(TypeErrorEx),
    },

    /* ----------------------- bool type ----------------------- */

    {
        "bool: true/false are the bool type, distinct from int",
        {
            "assert(type(true) == \"bool\");",
            "assert(type(false) == \"bool\");",
            "assert(type(1 < 2) == \"bool\");",
            "assert(type(1 == 1) == \"bool\");",
            "assert(type(!0) == \"bool\");",
            "assert(type(true && false) == \"bool\");",
            "assert(type(1) == \"int\");",         /* 1 stays int */
            "var b = true; assert(type(b) == \"bool\");",
        },
    },
    {
        "bool: printing and str()",
        {
            "assert(str(true) == \"true\");",
            "assert(str(false) == \"false\");",
            "assert(str(1 > 0) == \"true\");",
        },
    },
    {
        "bool: promotion chain bool <= int <= float",
        {
            "assert(true + 1 == 2);",
            "assert(false + 7 == 7);",
            "assert(true * 10 == 10);",
            "assert(true + 2.5 == 3.5);",
            "assert(type(true + 1) == \"int\");",
            "assert(type(true + 2.5) == \"float\");",
            "assert(-true == -1);",
            "assert(type(-true) == \"int\");",
        },
    },
    {
        "bool: equality with int/float (true==1, false==0)",
        {
            "assert(true == 1);",
            "assert(false == 0);",
            "assert(true == 1.0);",
            "assert(true != 0);",
            "assert(false != true);",
            "assert((1 == 1) == true);",
            "assert((1 == 2) == false);",
        },
    },
    {
        "bool: int()/float() conversions",
        {
            "assert(int(true) == 1);",
            "assert(int(false) == 0);",
            "assert(float(true) == 1.0);",
            "assert(float(false) == 0.0);",
        },
    },
    {
        "bool: logical operators yield bool",
        {
            "assert((true && true) == true);",
            "assert((true && false) == false);",
            "assert((false || true) == true);",
            "assert(!true == false);",
            "assert(!!5 == true);",
            "assert(type(true || false) == \"bool\");",
            "assert(type(!5) == \"bool\");",
        },
    },
    {
        "bool: truthy rules unchanged (0/none/[]/{} are false)",
        {
            "var n = 0;",
            "if (0) { n += 1; } if (none) { n += 1; }",
            "if ([]) { n += 1; } if ({}) { n += 1; }",
            "if (\"\") { n += 1; }",
            "assert(n == 0);",
            "if (1) { n += 1; } if (true) { n += 1; }",
            "if ([0]) { n += 1; } if (\"x\") { n += 1; }",
            "assert(n == 4);",
        },
    },
    {
        "bool: a bool dict key collides with the equal int key",
        {
            "var d = {1: \"a\"};",
            "d[true] = \"b\";",         /* true == 1, same key */
            "assert(len(d) == 1);",
            "assert(d[1] == \"b\");",
            "var e = {true: \"yes\", false: \"no\"};",
            "assert(e[true] == \"yes\");",
            "assert(e[false] == \"no\");",
            "assert(e[1] == \"yes\");",
        },
    },
    {
        "bool: hashes match the equal int (true~1, false~0)",
        {
            "assert(hash(true) == hash(1));",
            "assert(hash(false) == hash(0));",
        },
    },
    {
        "bool: const-folding of a bool var/const",
        {
            "const C = 3 > 2;",
            "assert(C == true);",
            "assert(type(C) == \"bool\");",
            "var f = false;",          /* auto-const: folds, decl dropped */
            "assert(f == false);",
            "assert(!f == true);",
        },
    },

    /* ----------------------- array<bool> ----------------------- */

    {
        "array<bool>: flat (bools) storage from a literal",
        {
            "var b = [true, false, true];",
            "assert(array_storage(b) == \"bools\");",
            "assert(len(b) == 3);",
            "assert(b == [true, false, true]);",
            "assert(b[0] == true);",
            "assert(b[1] == false);",
            "assert(str(b) == \"[true, false, true]\");",
        },
    },
    {
        "array<bool>: array(N, false) and make_array build flat bools",
        {
            "var g = array(4, false);",
            "assert(array_storage(g) == \"bools\");",
            "assert(g == [false, false, false, false]);",
            "var m = make_array(5, func(i) => i % 2 == 0);",
            "assert(array_storage(m) == \"bools\");",
            "assert(m == [true, false, true, false, true]);",
        },
    },
    {
        "array<bool>: subscript store, append, pop, insert, erase",
        {
            "var b = [true, false, false];",
            "b[1] = true;",
            "assert(b == [true, true, false]);",
            "append(b, true);",
            "assert(array_storage(b) == \"bools\");",
            "assert(b == [true, true, false, true]);",
            "assert(pop(b) == true);",
            "insert(b, 0, false);",
            "assert(b == [false, true, true, false]);",
            "erase(b, 0);",
            "assert(b == [true, true, false]);",
        },
    },
    {
        "array<bool>: sum counts trues (as int), min/max",
        {
            "var b = [true, false, true, true];",
            "assert(sum(b) == 3);",
            "assert(type(sum(b)) == \"int\");",
            "assert(max(b) == true);",
            "assert(min(b) == false);",
            "assert(max([false, false]) == false);",
            "assert(min([true, true]) == true);",
        },
    },
    {
        "array<bool>: reverse, sort, foreach",
        {
            "var b = [true, false, true];",
            "reverse(b);",
            "assert(b == [true, false, true]);",
            "var s = sort(clone([true, false, true, false]));",
            "assert(s == [false, false, true, true]);",
            "var cnt = 0;",
            "foreach (var x in [true, false, true, true]) {",
            "  if (x) { cnt += 1; }",
            "}",
            "assert(cnt == 3);",
        },
    },
    {
        "array<bool>: slices share flat storage, COW on write",
        {
            "var b = [true, false, true, false];",
            "var sl = b[1:3];",
            "assert(array_storage(sl) == \"bools\");",
            "assert(sl == [false, true]);",
            "assert(intptr(b) == intptr(sl));",
            "sl[0] = true;",                 /* COW: b untouched */
            "assert(sl == [true, true]);",
            "assert(b == [true, false, true, false]);",
        },
    },
    {
        "array<bool>: clone keeps flat, const is deep read-only",
        {
            "var b = [true, false];",
            "var c = clone(b);",
            "assert(array_storage(c) == \"bools\");",
            "c[0] = false;",
            "assert(b == [true, false]);",
            "const K = [true, false, true];",
            "assert(array_storage(K) == \"bools\");",
        },
    },
    {
        "array<bool>: mutating a const bool array throws",
        {
            "const K = [true, false];",
            "K[0] = false;",
        },
        &typeid(NotLValueEx),
    },
    {
        "array<bool>: dyn-launder mutation with a non-bool throws",
        {
            "var b = [true, false];",
            "var dyn d = b;",
            "d[0] = 5;",                     /* int into a flat bool array */
        },
        &typeid(TypeErrorEx),
    },
    {
        "array<bool>: dynarray() promotes to a general copy",
        {
            "var b = [true, false];",
            "var g = dynarray(b);",
            "assert(array_storage(g) == \"general\");",
            "assert(g == [true, false]);",
            "g[0] = 5;",                     /* now polymorphic */
            "assert(g == [5, false]);",
            "assert(b == [true, false]);",   /* original untouched */
        },
    },
    {
        "array<bool>: concatenation keeps bools",
        {
            "var a = [true, false];",
            "var b = [false, true];",
            "a += b;",
            "assert(a == [true, false, false, true]);",
            "assert(array_storage(a) == \"bools\");",
        },
    },
    {
        "array<bool>: var dyn builds a general (polymorphic) bool array",
        {
            "var dyn d = [true, false];",
            "assert(array_storage(d) == \"general\");",
            "d[0] = 99;",                    /* allowed: it's polymorphic */
            "assert(d == [99, false]);",
        },
    },
    {
        "array<bool>: map/filter over a bool array",
        {
            "var b = [true, false, true, true];",
            "var inv = map(func(x) => !x, b);",
            "assert(inv == [false, true, false, false]);",
            "var keep = filter(func(x) => x, b);",
            "assert(keep == [true, true, true]);",
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
            "   var d = 1; d = 0;",   // reassigned -> runtime value, so
            "   print(t/d);",         // t/d is a runtime (catchable) error
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
            "   var d = 1; d = 0;",   // runtime value (see above)
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
            "   var d = 1; d = 0;",   // runtime value (see above)
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
        // auto-const promotes write-once scalar `var`s to constants, so a/b is
        // fully known at "compile" time: 6/0 always fails. A program that can
        // never run correctly is rejected before execution (like the parser's
        // const-folding), rather than deferred to a runtime exception.
        "auto-const: provably-constant div-by-zero fails at compile time",
        {
            "var a = 6;",
            "var b = 0;",
            "var c = a / b;",
        },
        &typeid(DivisionByZeroEx),
    },

    {
        // try/catch is for RUNTIME exceptions; a provable compile-time error
        // still aborts before the script runs and is NOT caught here.
        "auto-const: compile-time error is not caught by try/catch",
        {
            "var a = 6;",
            "var b = 0;",
            "try { var c = a / b; } catch (DivisionByZeroEx) { }",
        },
        &typeid(DivisionByZeroEx),
    },

    {
        // An always-wrong constant in a LIVE branch fails the build. (`on` is a
        // promoted var, so the parser keeps the if; auto-const folds the live
        // branch and hits 6/0.)
        "auto-const: compile-time error in a live branch",
        {
            "var a = 6;",
            "var b = 0;",
            "var on = 1;",
            "if (on) { var c = a / b; }",
        },
        &typeid(DivisionByZeroEx),
    },

    {
        // Dual of the above: a branch auto-const proves DEAD is eliminated, not
        // analyzed - so the (unreachable) 6/0 inside it is never folded and the
        // program runs cleanly. (off is a promoted var, so the parser can't
        // drop the branch first; auto-const's own DCE does.)
        "auto-const: a provably-dead branch is eliminated, not folded",
        {
            "var a = 6;",
            "var b = 0;",
            "var off = 0;",
            "if (off) { var c = a / b; }",
            "assert(1);",
        },
    },

    {
        // Safety: `a` is a direct call argument, so it stays an lvalue (append
        // may mutate it) and is NOT folded to a literal. append() on an int is
        // therefore a normal runtime TypeErrorEx, not a folded compile error.
        "auto-const: write-once var passed to a mutating builtin is not folded",
        {
            "var a = 3;",
            "append(a, 10);",
        },
        &typeid(TypeErrorEx),
    },

    {
        // runtime() barrier: same div-by-zero, but the divisor is wrapped so
        // the expression can't be folded. It is therefore a RUNTIME error that
        // try/catch CAN handle - the dual of the compile-time test above.
        "auto-const: runtime() forces a catchable runtime div-by-zero",
        {
            "var a = 6;",
            "var b = 0;",
            "var ok = 0;",
            "try { var dyn c = a / runtime(b); assert(0); }",
            "catch (DivisionByZeroEx) { ok = 1; }",
            "assert(ok == 1);",
        },
    },

    {
        // runtime() only "runtime-izes" its result: an error INSIDE the
        // argument is folded first, so this still fails at compile time.
        "auto-const: error inside runtime()'s arg fails at compile time",
        {
            "var a = 6;",
            "var b = 0;",
            "var dyn c = runtime(a / b);",
        },
        &typeid(DivisionByZeroEx),
    },

    {
        // runtime() returns its argument unchanged at run time (identity).
        "auto-const: runtime() is the identity at run time",
        {
            "assert(runtime(5) == 5);",
            "var dyn a = runtime(3) + 4;",
            "assert(a == 7);",
        },
    },

    {
        "const param: basic, value is usable",
        {
            "func f(const x, y) { return x + y; }",
            "assert(f(3, 4) == 7);",
        },
    },

    {
        "const param: reassigning it is a compile-time error",
        {
            "func f(const x) { x = 9; return x; }",
            "print(f(1));",
        },
        &typeid(CannotRebindConstEx),
    },

    {
        "const param: a non-const param next to it stays mutable",
        {
            "func f(const x, y) { y = y + x; return y; }",
            "assert(f(10, 5) == 15);",
        },
    },

    {
        "const param: a pure func may reassign its own (non-const) param",
        {
            "pure func f(x) { x = x * 2; return x + 1; }",
            "assert(f(10) == 21);",
        },
    },

    {
        "isconst/isconstdecl: explicit const, auto-const var, runtime barrier",
        {
            "const c = 5;",
            "var v = 7;",              // write-once -> auto-const
            "var dyn r = runtime(9);", // barrier -> not const (and dynamic)
            "assert(isconst(c) && isconstdecl(c));",
            "assert(isconst(v) && !isconstdecl(v));",
            "assert(!isconst(r) && !isconstdecl(r));",
            "assert(isconst(2 + 3) && isconstdecl(2 + 3));",
        },
    },

    {
        "isconst/isconstdecl: const param vs auto-const param vs mutable param",
        {
            "func f(const a, b, c) {",
            "  b = b + 1;",
            "  assert(isconst(a) && isconstdecl(a));",   // const param
            "  assert(!isconst(b) && !isconstdecl(b));", // reassigned
            "  assert(isconst(c) && !isconstdecl(c));",  // auto-const param
            "  return a + b + c;",
            "}",
            "assert(f(1, 2, 3) == 7);",
        },
    },

    {
        "ispure/ispuredecl: explicit pure",
        {
            "pure func p(x) => x + 1;",
            "assert(ispure(p) && ispuredecl(p));",
        },
    },

    {
        "ispure: an effectively-pure func is auto-promoted (not declared pure)",
        {
            "func q(x) => x * 2;",
            "assert(ispure(q) && !ispuredecl(q));",
        },
    },

    {
        "ispure: a func reading a const global / const builtin is pure",
        {
            "const k = 10;",
            "func usesk(x) => x + k;",
            "func mylen(a) => len(a);",
            "assert(ispure(usesk) && ispure(mylen));",
        },
    },

    {
        "ispure: reading a non-const global or calling print is impure",
        {
            "var g = 10;",
            "func usesg(x) => x + g;",
            "func loud(x) { print(x); return x; }",
            "assert(!ispure(usesg) && !ispure(loud));",
        },
    },

    /*
     * Error-reporting / caret precision. The ex_col/ex_col_end fields pin the
     * exact source span the error points at (the "^^^" run); ex_line/
     * ex_line_end pin multi-line spans. See the `test` struct.
     */
    {
        "err loc: undefined var marks the variable, not the `=`",
        { "var y = foobar;" },
        &typeid(UndefinedVariableEx), 9, 0, 16, 0,
    },
    {
        "err loc: undefined operand inside a binary expression",
        { "var z = 10 + missing + 20;" },
        &typeid(UndefinedVariableEx), 14, 0, 22, 0,
    },
    {
        "err loc: type error marks the offending operand",
        /* `c` is dynamic (a runtime() result), so it must be declared `dyn`;
         * that shifts the caret columns right by 4. */
        { "var s = 5; var dyn c = s + runtime(\"x\");" },
        &typeid(TypeErrorEx), 28, 0, 41, 0,
    },
    {
        "err loc: division by zero marks the divisor",
        { "var s = 100; var dyn c = s / runtime(0);" },
        &typeid(DivisionByZeroEx), 30, 0, 41, 0,
    },
    {
        "err loc: calling a defined non-function is a type error",
        { "var x = 5; x(1, 2);" },
        &typeid(TypeMismatchEx), 12, 0, 14, 0,
    },
    {
        "err loc: undefined callee marks the callee, not the whole call",
        { "var z = undefined_fn(1, 2);" },
        &typeid(UndefinedVariableEx), 9, 0, 22, 0,
    },
    {
        "err loc: out-of-bounds marks the subscript, not the assignment",
        { "var a = [1, 2, 3]; var x = a[runtime(5)];" },
        &typeid(OutOfBoundsEx), 28, 0, 42, 0,
    },
    {
        "err loc: a var passed to append stays an lvalue (TypeError there)",
        { "var a = 3; append(a, 10);" },
        &typeid(TypeErrorEx), 19, 0, 21, 0,
    },
    {
        "err loc: uncaught user exception points at the throw",
        { "throw ex(\"Boom\", 42);" },
        &typeid(ExceptionObject), 1, 0, 21, 0,
    },
    {
        "err loc: multi-line - undefined var on a continuation line",
        {
            "var total =",
            "    10 +",
            "    missing_var +",
            "    20;",
        },
        &typeid(UndefinedVariableEx), 5, 3, 17, 3,
    },
    {
        "err loc: multi-line - an operand spanning lines",
        {
            "var c = 5 + [10,",
            "             20];",
        },
        &typeid(TypeErrorEx), 13, 1, 18, 2,
    },
    {
        "err loc: break outside a loop is a clear parse error",
        { "break;" },
        &typeid(SyntaxErrorEx), 1, 0, 0, 0,
    },
    {
        "err loc: return outside a function is a clear parse error",
        { "return 5;" },
        &typeid(SyntaxErrorEx), 1, 0, 0, 0,
    },
    {
        "err loc: rethrow outside a catch is a clear parse error",
        { "rethrow;" },
        &typeid(SyntaxErrorEx), 1, 0, 0, 0,
    },

    {
        "Exceptions, catch multiple exceptions, ex: TypeErrorEx",
        {
            "var c = 0;",
            "try {",
            "   var t = 3;",
            "   append(t, 34);",
            "   assert(0);",     // We should NEVER get here
            "} catch (DivisionByZeroEx, TypeErrorEx) {",
            "   c = 1;",
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
            "var dyn c = 0;",   /* exdata() yields a dynamic payload */
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
            "var dyn c1, c2 = 0;",   /* exdata() payloads are dynamic */
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
        /*
         * A user comparator may not be a valid strict weak ordering. It must
         * still be MEMORY-SAFE: sort uses heapsort for custom comparators,
         * which stays in bounds whatever the comparator returns (introsort's
         * unguarded partition would read off the buffer here). Big enough to
         * leave insertion-sort territory. A crash here would take down the
         * whole -rt run, so this passing is the regression guard.
         */
        "Sort with an invalid (non-ordering) comparator stays memory-safe",
        {
            "var arr = range(2000);",
            "var r = sort(arr, func (a, b) => a != b);",  // not an ordering
            "assert(len(r) == 2000);",
            "assert(sum(r) == 1999000);",                 // elements preserved
        },
    },

    {
        "Sort with an always-true comparator stays memory-safe",
        {
            "var arr = range(1000);",
            "var r = sort(arr, func (a, b) => true);",
            "assert(len(r) == 1000);",
            "assert(sum(r) == 499500);",
        },
    },

    {
        /* Same hazard reachable at parse time via a pure comparator. */
        "Const sort with an invalid comparator stays memory-safe",
        {
            "const r = sort(range(2000), pure func (a, b) => a != b);",
            "assert(len(r) == 2000);",
            "assert(sum(r) == 1999000);",
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
        "Reverse sort",
        {
            "const arr = [1,2,3];",
            "const s = rev_sort(arr);",
            "assert(s == [3,2,1]);",
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
        "Foreach loop without elems expansion",
        {
            "const arr = [[11, \"hello\"], [22, \"world\"]];",
            "var tmp = [];",
            "foreach (var e in arr) {",
            "   append(tmp, str(e));",
            "}",
            "assert(tmp[0] == \"[11, hello]\");",
            "assert(tmp[1] == \"[22, world]\");",
        }
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
        "Foreach loop with index, but without elems expansion",
        {
            "const arr = [[11, \"hello\"], [22, \"world\"]];",
            "var tmp = [];",
            "foreach (var i, e in indexed arr) {",
            "   append(tmp, str(i)+str(e));",
            "}",
            "assert(tmp[0] == \"0[11, hello]\");",
            "assert(tmp[1] == \"1[22, world]\");",
        }
    },

    {
        "Foreach with extern variable",
        {
            "var e = 0;",   /* non-null: used in comparisons below */
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
            "assert(isinf(inf) == 1);",
            "assert(isinf(-inf) == 1);",
            "assert(isnan(nan) == 1);",
            "assert(floor(0.9) == 0);",
            "assert(ceil(0.1) == 1.0);",
            "assert(.1 == 0.1);",
        },
    },

    {
        "Float comparison with integers",
        {
            "assert(3.0 == 3);",
            "assert(3.0 != 2);",
        },
    },

    {
        "Float to bool",
        {
            "if (3.0) { assert(1); } else { assert(0); }",
            "if (0.0) { assert(0); } else { assert(1); }",
        },
    },

    {
        "int() builtin works",
        {
            "assert(int(\"123\") == 123);",
            "assert(int(3.4) == 3);",
            "assert(int(3.99) == 3);",
        },
    },

    {
        "int() builtin throws in case of invalid string",
        {
            "int(\"abc\");",
        },
        &typeid(TypeErrorEx),
    },

    {
        "int() builtin trucates in case of float string",
        {
            "assert(int(\"4.5\") == 4);",
        },
    },

    {
        "float() builtin throws in case of invalid string",
        {
            "float(\"abc\");",
        },
        &typeid(TypeErrorEx),
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
        "Const-bound pure funcs",
        {
            "const cmp = pure func(a, b) => a < b;",
            "assert(cmp(1,2) == 1);",
            "assert(cmp(2,1) == 0);",
            "var a, b = [1,2];",
            "assert(cmp(a,b) == 1);",
            "assert(cmp(b,a) == 0);",
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
        "find() on dict",
        {
            "var d = {\"a\": 3, \"b\": 5};",
            "assert(find(d, \"a\") == 3);",
            "assert(find(d, \"x\") == none);",
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
        "insert() builtin on slices of arrays",
        {
            "var arr = [1,2,3,4,5];",
            "var s = arr[1:4];",
            "assert(s == [2,3,4]);",
            "insert(s, 0, 99);",
            "assert(s == [99,2,3,4]);",
            "s = arr[1:4];",
            "insert(s, 1, 99);",
            "assert(s == [2,99,3,4]);",
            "s = arr[1:4];",
            "insert(s, 2, 99);",
            "assert(s == [2,3,99,4]);",
            "s = arr[1:4];",
            "insert(s, 3, 99);",
            "assert(s == [2,3,4,99]);",
        },
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
            /* dyn: a dict read is opt now; this exercises nested member /
             * subscript writes, not nullability. */
            "var dyn d = {\"a\": [{}, 3, 4]};",
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

    {
        "Clone() of empty dictionaries work as expected",
        {
            "var d = {};",
            "var d2 = clone(d);",
            "d2.a = 42;",
            "assert(len(d2) == 1);",
            "assert(len(d) == 0);",
        },
    },

    {
        "The find() builtin supports a key func param for arrays",
        {
            "var a = [[\"hello\", 42], [\"world\", 11]];",
            "pure func deref0(x) => x[0];",
            "assert(find(a, \"hello\", deref0) == 0);",
            "assert(find(a, \"world\", deref0) == 1);",
        },
    },

    {
        "lstrip(), rstrip() and strip()",
        {
            "var orig = \"something  abc   other\";",
            "var s = orig[9:17];",
            "assert(s == \"  abc   \");",
            "assert(lstrip(s) == \"abc   \");",
            "assert(rstrip(s) == \"  abc\");",
            "assert(strip(s) == \"abc\");",
        },
    },

    {
        "startswith(), endswith() builtins",
        {
            "var a = \"123abc456\";",
            "var s = a[3:6];",
            "assert(s == \"abc\");",
            "assert(startswith(\"\", \"\"));",
            "assert(startswith(s, \"\"));",
            "assert(startswith(s, \"a\"));",
            "assert(startswith(s, \"ab\"));",
            "assert(startswith(s, \"abc\"));",
            "assert(startswith(s, \"abcd\") == false);",
            "assert(startswith(s, \"x\") == false);",
            "assert(endswith(\"\", \"\"));",
            "assert(endswith(s, \"\"));",
            "assert(endswith(s, \"c\"));",
            "assert(endswith(s, \"bc\"));",
            "assert(endswith(s, \"abc\"));",
            "assert(endswith(s, \"abcd\") == false);",
            "assert(endswith(s, \"x\") == false);",
        },
    },

    {
        "Exception to string",
        {
            "assert(str(exception(\"abc\")) == \"<Exception(abc)>\");",
        },
    },

    {
        "str() of builtin",
        {
            "assert(str(print) == \"<Builtin(print)>\");",
        },
    },

    {
        "str() of none",
        {
            "assert(str(none) == \"<none>\");",
        },
    },

    {
        "The empty_arr optimization works as expected",
        {
            "var a=[1];",
            "var b=a[5:];",
            "b+=[99];",
            "assert(b == [99]);",
            "var c = a[5:];",
            "assert(c == []);",
            "",
            "var e1 = []; var e2 = [];",
            "assert(intptr(e1) == intptr(e2));",
            "assert(intptr(e1) == intptr(c));",
        },
    },

    {
        "The empty_str optimization works as expected",
        {
            "var a = \"h\";",
            "var b = a[5:];",
            "b += \"blah\";",
            "assert(a == \"h\");",
            "assert(b == \"blah\");",
            "var c = a[5:];",
            "assert(c == \"\");",
            "var e1 = \"\"; var e2 = \"\";",
            "assert(intptr(e1) == intptr(e2));",
            "assert(intptr(e1) == intptr(c));",
        },
    },

    {
        "Classic for-loop",
        {
            "var ext = \"\";",
            "",
            "for (var i = 0; i < 3; i += 1)",
            "   ext += str(i);",
            "",
            "assert(ext == \"012\");",
            "",
            "ext = \"\";",
            "for (var i = 0; i < 10; i += 1) {",
            "",
            "   if (i == 2)",
            "       continue;",
            "",
            "   if (i == 5)",
            "       break;",
            "",
            "   ext += str(i);",
            "}",
            "assert(ext == \"0134\");",
        },
    },

    {
        "rand() builtin",
        {
            "for (var i = 0; i < 1000; i += 1) {",
            "   var val = rand(0, 100);",
            "   assert(0 <= val && val <= 100);",
            "}",
            "for (var i = 0; i < 1000; i += 1) {",
            "   var val = rand(-20, 20);",
            "   assert(-20 <= val && val <= 20);",
            "}",
        },
    },

    {
        "randf() builtin",
        {
            "for (var i = 0; i < 1000; i += 1) {",
            "   var val = randf(-20.0, 100.0);",
            "   assert(-20.0 <= val && val <= 100.0);",
            "}",
        },
    },

    {
        "Insert() is slice-safe on arrays (1)",
        {
            "var a = [1,2,3];",
            "var slices = [a[0:1], a[1:2], a[2:3]];",
            "assert(slices == [[1],[2],[3]]);",
            "assert(intptr(slices[0]) == intptr(a));",
            "assert(intptr(slices[1]) == intptr(a));",
            "assert(intptr(slices[2]) == intptr(a));",
            "insert(a, 1, 99);",
            "assert(slices == [[1],[2],[3]]);",
            "assert(a == [1, 99, 2, 3]);",
        },
    },

    {
        "Insert() is slice-safe on arrays (2)",
        {
            "var a = [1,2,3];",
            "var slices = [a[0:1], a[1:2], a[2:3]];",
            "assert(slices == [[1],[2],[3]]);",
            "assert(intptr(slices[0]) == intptr(a));",
            "assert(intptr(slices[1]) == intptr(a));",
            "assert(intptr(slices[2]) == intptr(a));",
            "insert(a, len(a), 99);",
            "assert(intptr(slices[0]) == intptr(a));",
            "assert(intptr(slices[1]) == intptr(a));",
            "assert(intptr(slices[2]) == intptr(a));",
            "assert(slices == [[1],[2],[3]]);",
            "assert(a == [1, 2, 3, 99]);",
        },
    },

    {
        "value-less return statement returns none",
        {
            "func f() { return; }",
            "assert(f() == none);",
            "func g(x) { if (x) return 10; return; }",
            "assert(g(1) == 10);",
            "assert(g(0) == none);",
        },
    },

    {
        "sum() of an empty array returns none",
        {
            "assert(sum([]) == none);",
            "var a = [1, 2, 3];",
            "var e = a[1:1];",
            "assert(sum(e) == none);",
            "assert(sum([], pure func(x) => x) == none);",
        },
    },

    {
        "integer literal out of range is a syntax error",
        {
            "var x = 99999999999999999999999999;",
        },
        &typeid(SyntaxErrorEx),
    },

    {
        "float literal out of range is a syntax error",
        {
            "var x = 1e999999;",
        },
        &typeid(SyntaxErrorEx),
    },

    {
        "dict != non-dict is true (and == is false)",
        {
            "var d = {\"a\": 1};",
            "assert(d != 5);",
            "assert((d == 5) == 0);",
            "assert(d != [1, 2]);",
            "assert(d != \"x\");",
            "assert((d != {\"a\": 1}) == 0);",
            "assert(d != {\"a\": 2});",
        },
    },

    {
        "writelines() respects array slice bounds",
        {
            "var a = [\"aa\", \"bb\", \"cc\", \"dd\", \"ee\"];",
            "var s = a[1:3];",
            "var f = tmpdir() + \"/mylang_test_io_writelines.tmp\";",
            "writelines(s, f);",
            "var lines = readlines(f);",
            "assert(remove(f));",   /* clean up the temp file */
            "assert(len(lines) == 2);",
            "assert(rstrip(lines[0]) == \"bb\");",
            "assert(rstrip(lines[1]) == \"cc\");",
        },
    },

    /* ---- file I/O builtins (io.cpp.h). stdin forms (read()/readln()/
       readlines() with no file) need external input, so they aren't covered. */
    {
        "I/O: write / writeln / read a file round-trip",
        {
            "var d = tmpdir();",
            "var f1 = d + \"/mylang_test_io_rw.tmp\";",
            "var f2 = d + \"/mylang_test_io_rw2.tmp\";",
            "write(\"hello\\n\", f1);",
            /* writeln's newline goes to stdout, not the file, so the file
               holds just the value. */
            "writeln(\"world\", f2);",
            "assert(read(f1) == \"hello\\n\");",
            "assert(read(f2) == \"world\");",
            "var L = readlines(f1);",
            "assert(remove(f1)); assert(remove(f2));",   /* clean up */
            "assert(len(L) == 1 && L[0] == \"hello\");",
        },
    },
    {
        "I/O: print / writeln to stdout",
        {
            "print(\"io-coverage\", 42);",
            "writeln(\"io-writeln\");",
        },
    },
    {
        "I/O: remove() deletes a file and reports success/failure",
        {
            "var f = tmpdir() + \"/mylang_test_remove.tmp\";",
            "write(\"x\", f);",
            "assert(remove(f) == true);",   /* existed -> removed */
            "assert(remove(f) == false);",  /* gone now -> false, no throw */
        },
    },
    { "remove() with a non-string arg is a type error",
      { "remove(123);" }, &typeid(TypeErrorEx) },
    { "remove() with wrong arity is an error",
      { "remove();" }, &typeid(InvalidNumberOfArgsEx) },
    { "tmpdir() returns a non-empty path",
      { "var d = tmpdir(); assert(len(d) > 0);" } },
    { "tmpdir() with an argument is an error",
      { "tmpdir(1);" }, &typeid(InvalidNumberOfArgsEx) },
    { "write() with a non-string value is a type error",
      { "write(123);" }, &typeid(TypeErrorEx) },
    { "write() with a non-string filename is a type error",
      { "write(\"x\", 123);" }, &typeid(TypeErrorEx) },
    { "write() with no args is rejected",
      { "write();" }, &typeid(InvalidNumberOfArgsEx) },
    { "write() with too many args is rejected",
      { "write(\"a\", \"b\", \"c\");" }, &typeid(InvalidNumberOfArgsEx) },
    { "write() to an unopenable path fails",
      { "write(\"x\", \"/no_such_dir_xyz123/f.tmp\");" },
      &typeid(CannotOpenFileEx) },
    { "read() of a non-string filename is a type error",
      { "read(123);" }, &typeid(TypeErrorEx) },
    { "read() of a missing file fails",
      { "read(\"no_such_file_xyz123.tmp\");" }, &typeid(CannotOpenFileEx) },
    { "read() with too many args is rejected",
      { "read(\"a\", \"b\");" }, &typeid(InvalidNumberOfArgsEx) },
    { "readlines() of a non-string filename is a type error",
      { "readlines(123);" }, &typeid(TypeErrorEx) },
    { "readlines() of a missing file fails",
      { "readlines(\"no_such_file_xyz456.tmp\");" }, &typeid(CannotOpenFileEx) },
    { "readlines() with too many args is rejected",
      { "readlines(\"a\", \"b\");" }, &typeid(InvalidNumberOfArgsEx) },
    { "writelines() of a non-array is a type error",
      { "writelines(123, \"f.tmp\");" }, &typeid(TypeErrorEx) },
    { "writelines() with a non-string filename is a type error",
      { "writelines([\"a\"], 123);" }, &typeid(TypeErrorEx) },
    { "writelines() with no args is rejected",
      { "writelines();" }, &typeid(InvalidNumberOfArgsEx) },
    { "writelines() to an unopenable path fails",
      { "writelines([\"a\"], \"/no_such_dir_xyz123/f.tmp\");" },
      &typeid(CannotOpenFileEx) },

    {
        /* The C++ literals below encode mylang source with backslash escapes. */
        "escaped backslash in string literals",
        {
            "assert(len(\"\\\\\") == 1);",        /* mylang: len("\\") == 1  */
            "assert(len(\"a\\\\b\") == 3);",      /* mylang: len("a\\b") == 3 */
            "assert(len(\"\\\"\") == 1);",        /* mylang: len("\"") == 1  */
            "assert(\"\\n\" != \"n\");",          /* mylang: "\n" != "n"     */
        },
    },

    {
        "scientific notation with signed exponent",
        {
            "assert(1e3 == 1000.0);",
            "assert(1e+3 == 1000.0);",
            "assert(5e-1 == 0.5);",       /* negative exponent; 0.5 is exact */
            "assert(25e-2 == 0.25);",
            "assert(2.5e2 == 250.0);",
            "assert(5e2 - 3 == 497.0);",  /* digit after exponent: '-' subtracts */
        },
    },

    {
        "float exponent with no digits is invalid",
        {
            "var x = 1e;",
        },
        &typeid(InvalidTokenEx),
    },

    {
        "float exponent with a sign but no digits is invalid",
        {
            "var x = 1e-;",
        },
        &typeid(InvalidTokenEx),
    },

    {
        "int and float compare equal symmetrically",
        {
            "assert(1 == 1.0);",
            "assert(1.0 == 1);",
            "assert((1 != 1.0) == 0);",
            "assert((1.0 != 1) == 0);",
            "assert(2 == 2.0);",
            "assert(1 != 1.5);",
            "assert(3.0 != 4);",
            "assert((1 == 2.0) == 0);",
        },
    },

    {
        "int and float dict keys are unified",
        {
            "var d = {};",
            "d[1] = 10;",
            "d[1.0] = 20;",         /* same key as 1 */
            "assert(len(d) == 1);",
            "assert(d[1] == 20);",
            "assert(d[1.0] == 20);",
            "d[2.0] = 99;",
            "d[2] = 7;",            /* same key as 2.0 */
            "assert(len(d) == 2);",
            "assert(d[2.0] == 7);",
        },
    },

    {
        "equal-content slices of one array compare equal",
        {
            "var a = [7, 7, 7];",
            "var s1 = a[0:2];",
            "var s2 = a[1:3];",
            /* Both slices share a's backing vector but have different offsets. */
            "assert(intptr(s1) == intptr(a));",
            "assert(intptr(s2) == intptr(a));",
            "assert(s1 == s2);",
            "assert((s1 != s2) == 0);",
            /* Same backing vector, same offset: still equal. */
            "assert(a[0:2] == a[0:2]);",
            /* Different content at different offsets: not equal. */
            "var b = [1, 2, 3];",
            "assert(b[0:2] != b[1:3]);",
            "assert((b[0:2] == b[1:3]) == 0);",
        },
    },

    {
        "round() accepts integer arguments",
        {
            "assert(round(5) == 5.0);",
            "assert(round(5, 2) == 5.0);",
            "assert(round(2.5) == 3.0);",
            "assert(round(2.4) == 2.0);",
            "var n = 7;",
            "assert(round(n) == 7.0);",
        },
    },

    {
        /* The error must point at the first char of the `==` operator
           (col 7), not its second char. */
        "two-char operator reports its first column",
        {
            "var x == 5;",
        },
        &typeid(SyntaxErrorEx),
        7,
    },

    {
        "str() of a float is not truncated at high precision",
        {
            /* 1e30 needs 31 integer digits + '.' + 64 fractional = 96 chars,
               which a fixed 80-byte buffer would silently truncate. */
            "var s = str(1e30, 64);",
            "assert(len(s) == 96);",
            "assert(str(3.5, 2) == \"3.50\");",
            "assert(str(2.0, 0) == \"2\");",
        },
    },

    {
        /* The function arg must be validated before the container is
           evaluated: with a bad func and an undefined container, the
           "Expected function" TypeErrorEx must win over UndefinedVariable. */
        "map() validates its function argument first",
        {
            "map(5, undefined_var);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "filter() validates its function argument first",
        {
            "filter(5, undefined_var);",
        },
        &typeid(TypeErrorEx),
    },

    {
        "chr() round-trips across the [0, 255] range",
        {
            "assert(chr(65) == \"A\");",
            "assert(ord(chr(0)) == 0);",
            "assert(ord(chr(255)) == 255);",
        },
    },

    {
        "chr() rejects values above 255",
        {
            "chr(256);",
        },
        &typeid(InvalidValueEx),
    },

    {
        "chr() rejects negative values",
        {
            "chr(-1);",
        },
        &typeid(InvalidValueEx),
    },

    {
        "dict() keeps the last value for duplicate keys",
        {
            "var d = dict([[1, \"a\"], [1, \"b\"]]);",
            "assert(len(d) == 1);",
            "assert(d[1] == \"b\");",
            "var d2 = dict([[\"x\", 1], [\"x\", 2], [\"y\", 3]]);",
            "assert(len(d2) == 2);",
            "assert(d2.x == 2);",
            "assert(d2.y == 3);",
        },
    },

    {
        "mixed int/float arithmetic and comparison are symmetric",
        {
            /* arithmetic: int-OP-float matches float-OP-int and yields float */
            "assert(1 + 2.0 == 3.0);",
            "assert(2.0 + 1 == 3.0);",
            "assert(5 - 1.5 == 3.5);",
            "assert(2 * 1.5 == 3.0);",
            "assert(5 / 2.0 == 2.5);",
            "assert(5 % 2.0 == 1.0);",
            "assert(type(1 + 2.0) == \"float\");",
            /* relational works in both orders */
            "assert(1 < 2.0);",
            "assert(2.0 > 1);",
            "assert(2 <= 2.0);",
            "assert(2.0 >= 2);",
            "assert((3 < 2.0) == 0);",
            /* int/int division stays integer */
            "assert(5 / 2 == 2);",
        },
    },

    {
        "int divided by float zero throws",
        {
            "var x = 1 / 0.0;",
        },
        &typeid(DivisionByZeroEx),
    },

    {
        /* Exercises mixed int/float promotion via EvalValue's comparison
           operators (sort/min/max), not just the expression evaluator. */
        "mixed int/float ordering via sort/min/max",
        {
            "var a = [3, 1.5, 2, 0];",
            "assert(min(a) == 0);",
            "assert(max(a) == 3);",
            "assert(sort(clone(a)) == [0, 1.5, 2, 3]);",
        },
    },

    /*
     * Parse-time common-subexpression de-duplication (CSE). Identical const
     * array/dict expressions are evaluated once at parse time and the
     * resulting deep read-only value is shared, asserted here via intptr().
     */

    {
        "CSE: identical const slices share storage",
        {
            "const big = range(1000);",
            "const a = big[0:500];",
            "const b = big[0:500];",
            "assert(intptr(a) == intptr(b));",
            "assert(a == b);",
            "assert(len(a) == 500);",
            "assert(a[0] == 0 && a[499] == 499);",
        },
    },

    {
        "CSE: identical const builtin call (sort) shares",
        {
            "const base = [3, 1, 2, 5, 4];",
            "const s1 = sort(base);",
            "const s2 = sort(base);",
            "assert(intptr(s1) == intptr(s2));",
            "assert(s1 == [1, 2, 3, 4, 5]);",
        },
    },

    {
        "CSE: identical const range() call shares",
        {
            "const r1 = range(100);",
            "const r2 = range(100);",
            "assert(intptr(r1) == intptr(r2));",
            "assert(len(r1) == 100);",
        },
    },

    {
        "CSE: identical keys() of a const dict shares",
        {
            "const d = {\"a\": 1, \"b\": 2, \"c\": 3};",
            "const k1 = keys(d);",
            "const k2 = keys(d);",
            "assert(intptr(k1) == intptr(k2));",
            "assert(sort(k1) == [\"a\", \"b\", \"c\"]);",
        },
    },

    {
        "CSE: identical user pure-func call shares",
        {
            "pure func head2(a) => a[0:2];",
            "const base = range(50);",
            "const p1 = head2(base);",
            "const p2 = head2(base);",
            "assert(intptr(p1) == intptr(p2));",
            "assert(p1 == [0, 1]);",
        },
    },

    {
        "CSE: different slice bounds do NOT share",
        {
            "const big = range(1000);",
            "const a = big[0:500];",
            "const c = big[0:400];",
            "assert(intptr(a) != intptr(c));",
            "assert(len(a) == 500 && len(c) == 400);",
        },
    },

    {
        "CSE: subscript returning a sub-array dedups",
        {
            "const grid = [[1, 2], [3, 4], [5, 6]];",
            "const r1 = grid[1];",
            "const r2 = grid[1];",
            "assert(intptr(r1) == intptr(r2));",
            "assert(r1 == [3, 4]);",
        },
    },

    {
        "CSE: nested block reuses an outer-scope cache entry",
        {
            "const big = range(100);",
            "const a = big[10:20];",
            "{",
            "    const b = big[10:20];",
            "    assert(intptr(a) == intptr(b));",
            "    assert(b == a);",
            "}",
        },
    },

    {
        /* The crux of the per-block cache: a sibling block reuses the freed
         * block's stack addresses, so a flat cache keyed on LValue pointers
         * would false-hit. The cache is popped with its block, so it can't. */
        "CSE: sibling blocks with reused local const stay correct",
        {
            "{",
            "    const local = [1, 2, 3, 4];",
            "    const x = local[0:2];",
            "    assert(x == [1, 2]);",
            "}",
            "{",
            "    const local = [7, 8, 9, 10];",
            "    const x = local[0:2];",
            "    assert(x == [7, 8]);",
            "}",
        },
    },

    {
        "CSE: var-bound const-derived slices share (const-propagation)",
        {
            "const big = range(10);",
            "var s = big[0:3];",
            "var t = big[0:3];",
            "assert(intptr(s) == intptr(t));",
            "assert(s == [0, 1, 2]);",
        },
    },

    {
        "CSE: clone() of a dedup'd const is independent and mutable",
        {
            "const big = range(10);",
            "const a = big[0:3];",
            "const b = big[0:3];",
            "assert(intptr(a) == intptr(b));",
            "var c = clone(a);",
            "assert(intptr(c) != intptr(a));",
            "c[0] = 99;",
            "assert(c == [99, 1, 2]);",
            "assert(a == [0, 1, 2]);",
        },
    },

    {
        "CSE: mutable var literals are NOT shared",
        {
            "var a = [1, 2, 3];",
            "var b = [1, 2, 3];",
            "assert(intptr(a) != intptr(b));",
            "a[0] = 99;",
            "assert(a == [99, 2, 3]);",
            "assert(b == [1, 2, 3]);",
        },
    },

    {
        "CSE: subscript-assign through a shared const slice is rejected",
        {
            "const big = range(10);",
            "const a = big[0:3];",
            "const b = big[0:3];",
            "a[0] = 99;",
        },
        &typeid(NotLValueEx),
    },

    {
        /* CSE must not defer or swallow a const-eval error: an out-of-bounds
         * read in a const subexpression still fails at parse time. */
        "CSE: out-of-bounds in a const subexpr fails at parse",
        {
            "const big = range(10);",
            "const a = big[5] + big[50000];",
        },
        &typeid(OutOfBoundsEx),
    },

    {
        /* A const compound expression (array concatenation) whose operands
         * are const-array identifiers is keyable, so it de-dups. */
        "CSE: identical const array concatenation shares",
        {
            "const base = range(5);",
            "const c = base + base;",
            "const d = base + base;",
            "assert(intptr(c) == intptr(d));",
            "assert(c == [0, 1, 2, 3, 4, 0, 1, 2, 3, 4]);",
        },
    },

    {
        /* Concatenation of already-materialized slices: the operands are
         * LiteralObj (not keyable), so the result is correct but not shared.
         * Exercises the key builder's bail-out on an unkeyable operand. */
        "CSE: concat of materialized slices stays correct",
        {
            "const big = range(10);",
            "const c = big[0:3] + big[5:8];",
            "assert(c == [0, 1, 2, 5, 6, 7]);",
        },
    },

    {
        /* An unkeyable subnode (a bare literal array) makes the whole
         * expression unkeyable; it still materializes correctly. These cover
         * the key builder's bail-out paths for subscript/slice/call bases. */
        "CSE: unkeyable subnodes still materialize correctly",
        {
            "const x1 = [[1, 2], [3, 4]][0];",
            "assert(x1 == [1, 2]);",
            "const x2 = [[1, 2], [3, 4]][0:1];",
            "assert(x2 == [[1, 2]]);",
            "const s = sort([3, 1, 2]);",
            "assert(s == [1, 2, 3]);",
        },
    },

    {
        /* A key that would exceed the size cap is abandoned (the expression
         * just isn't de-duplicated); the result is still correct. */
        "CSE: over-cap key is abandoned, result still correct",
        {
            "const base = range(2);",
            "const c = base + base + base + base + base + base + base +",
            "          base + base + base + base + base + base + base +",
            "          base + base + base + base + base + base;",
            "assert(len(c) == 40);",
            "assert(c[0] == 0 && c[39] == 1);",
        },
    },

    /* =========================================================== *
     *  Type inference + checking (plans/type-inference.md)         *
     * =========================================================== */

    /* ---- MUST ACCEPT: well-typed programs run without error ---- */
    { "ti: scalar inference", { "var x = 5; var y = x + 3; assert(y == 8);" } },
    { "ti: int->float promote in a var",
      { "var x = 1; x = 2.5; assert(x > 2.0);" } },
    { "ti: int->float promote across calls",
      { "func f(x) => x + 1; assert(f(2) == 3); assert(f(2.5) == 3.5);" } },
    { "ti: str concat with int/float/array",
      { "var s = \"a\"; assert(s + 1 == \"a1\");",
        "assert(s + 2.5 == \"a2.500000\"); assert(s + [1] == \"a[1]\");" } },
    { "ti: param inferred from a single call",
      { "func sq(n) => n * n; assert(sq(4) == 16);" } },
    { "ti: recursion (factorial)",
      { "func fact(n) { if (n <= 1) return 1; return n * fact(n-1); }",
        "assert(fact(5) == 120);" } },
    { "ti: mutual recursion",
      { "func ev(n) { if (n==0) return 1; return od(n-1); }",
        "func od(n) { if (n==0) return 0; return ev(n-1); }",
        "assert(ev(10) == 1); assert(od(7) == 1);" } },
    { "ti: function returning a function (closure typed)",
      { "func adder(n) => func [n] (x) => x + n;",
        "var add10 = adder(10); assert(add10(5) == 15);" } },
    { "ti: map with a named function",
      { "func dbl(x) => x * 2; assert(map(dbl, [1,2,3]) == [2,4,6]);" } },
    { "ti: map with an inline lambda",
      { "assert(map(func(x) => x + 1, [1,2,3]) == [2,3,4]);" } },
    { "ti: filter typed", { "assert(filter(func(x) => x > 2, [1,2,3,4]) == [3,4]);" } },
    { "ti: sort with named comparator",
      { "func cmp(a,b) => a < b; var arr = [3,1,2]; sort(arr, cmp);",
        "assert(arr == [1,2,3]);" } },
    { "ti: sort with inline comparator",
      { "var arr = [3,1,2]; sort(arr, func(a,b) => a < b); assert(arr==[1,2,3]);" } },
    { "ti: sum over an int array", { "assert(sum([1,2,3,4]) == 10);" } },
    { "ti: array element inference + append same type",
      { "var a = [1,2]; append(a, 3); assert(a == [1,2,3]);" } },
    { "ti: append of a mixed type degrades to dyn (no error)",
      { "var dyn x = 1; var a = [1,2]; append(a, \"s\"); assert(len(a) == 3);" } },
    { "ti: dict typed and subscripted (read is opt)",
      /* a dict read is opt int now; == is opt-safe (no narrowing needed) */
      { "var d = {\"a\": 1, \"b\": 2};",
        "assert(d[\"a\"] == 1 && d[\"b\"] == 2);" } },
    { "ti: dict read narrowed then used as non-opt",
      { "var d = {\"a\": 5}; var opt v = d[\"a\"];",
        "if (v != none) assert(v + 1 == 6);" } },
    { "ti: dict member access typed",
      { "var d = {\"a\": 1}; assert(d.a == 1);" } },
    { "ti: foreach over an array",
      { "var s = 0; foreach (var x in [1,2,3]) s += x; assert(s == 6);" } },
    { "ti: foreach over a dict (k, v)",
      { "var s = 0; foreach (var k, v in {\"a\":1,\"b\":2}) s += v; assert(s==3);" } },
    { "ti: foreach indexed (index first)",
      { "var t = 0; foreach (var i, x in indexed [10,20,30]) t += i;",
        "assert(t == 3);" } },
    { "ti: foreach over a string",
      { "var n = 0; foreach (var c in \"abc\") n += 1; assert(n == 3);" } },
    { "ti: nested arrays and subscript chains",
      { "var a = [[1,2],[3,4]]; assert(a[1][0] == 3);" } },
    { "ti: == / != across types is allowed (returns int)",
      { "var a = 5; assert((a == \"x\") == false); assert((a != \"x\") == true);" } },
    { "ti: opt param accepts none and a value",
      { "func f(opt x) => 1; assert(f(none) == 1); assert(f(5) == 1);" } },

    /* ----- trailing opt params are skippable (default to none) ----- */
    { "opt params: trailing opt args may be omitted (-> none)",
      { "func foo(x, opt y, opt z) { if (y == none) y = -1;",
        "  if (z == none) z = -2; return [x, y, z]; }",
        "assert(foo(1) == [1, -1, -2]);",
        "assert(foo(1, 2) == [1, 2, -2]);",
        "assert(foo(1, 2, 3) == [1, 2, 3]);" } },
    { "opt params: a single trailing opt may be omitted",
      { "func g(a, opt b) { if (b == none) return a; return a + b; }",
        "assert(g(5) == 5); assert(g(5, 3) == 8);" } },
    { "opt params: omitting a required (non-opt) param is an arity error",
      { "func f(x, opt y) => x; f();" }, &typeid(WrongArgCountEx) },
    { "opt params: passing too many is still an arity error",
      { "func f(x, opt y) => x; f(1, 2, 3);" }, &typeid(WrongArgCountEx) },
    { "opt params: a non-opt param after an opt one can't be skipped",
      /* min args is 3 here: z is required, so f(1,2) is too few. */
      { "func f(x, opt y, z) => z; f(1, 2);" }, &typeid(WrongArgCountEx) },
    { "opt params: f(x, opt y, z) still callable with all three",
      { "func f(x, opt y, z) => [x, y, z]; assert(f(1, 2, 3) == [1, 2, 3]);" } },
    { "opt params: all-opt function callable with no args",
      { "func f(opt a, opt b) { if (a == none) return 0; return a; }",
        "assert(f() == 0); assert(f(7) == 7); assert(f(7, 8) == 7);" } },
    { "opt params: a lambda callback with a trailing opt (map passes 1 arg)",
      { "var r = map(func(x, opt u) => x * 2, [1, 2, 3]);",
        "assert(r == [2, 4, 6]);" } },
    { "opt params: a function value omitting a trailing opt",
      { "var f = func(a, opt b) { if (b == none) return a; return a + b; };",
        "assert(f(7) == 7); assert(f(7, 3) == 10);" } },
    { "opt params: pure func folds with an omitted opt at compile time",
      { "pure func p(x, opt y) { if (y == none) return x; return x + y; }",
        "const C = p(5); const D = p(5, 100);",
        "assert(C == 5); assert(D == 105);" } },
    { "opt params: opt dyn trailing param may be omitted",
      { "func q(x, opt dyn d) { if (d == none) return x; return d; }",
        "assert(q(1) == 1); assert(q(1, \"hi\") == \"hi\");",
        "assert(q(1, 99) == 99);" } },

    /* ------------------------- named arguments ------------------------- */
    /* `f(name: value)` - sugar lowered to a positional call by the inferencer
     * (lower_named_args). Names must follow parameter-declaration order; an
     * interior skipped opt param is filled with `none`. Usable for both
     * required and optional params. */
    { "named args: all params named (required + opt)",
      { "func f(x, y?, z?) => [x, y, z];",
        "assert(f(x: 1, y: 2, z: 3) == [1, 2, 3]);" } },
    { "named args: skip an interior optional param (-> none)",
      { "func f(x, y?, z?) => [x, y, z];",
        "assert(f(x: 1, z: 3) == [1, none, 3]);" } },
    { "named args: name a single required param",
      { "func f(x) => x * 10; assert(f(x: 4) == 40);" } },
    { "named args: name two required params",
      { "func f(a, b) => a - b; assert(f(a: 10, b: 3) == 7);" } },
    { "named args: leading positional then named",
      { "func f(x, y?, z?) => [x, y, z];",
        "assert(f(1, z: 3) == [1, none, 3]);",
        "assert(f(1, 2, z: 3) == [1, 2, 3]);",
        "assert(f(1, y: 2) == [1, 2, none]);" } },
    { "named args: only a trailing opt named (earlier opts -> none)",
      { "func f(a?, b?, c?) => [a, b, c];",
        "assert(f(c: 9) == [none, none, 9]);",
        "assert(f(b: 5) == [none, 5, none]);" } },
    { "named args: equivalent to the positional call",
      { "func f(x, y?, z?) => [x, y, z];",
        "assert(f(x: 1) == f(1));",
        "assert(f(x: 1, y: 2) == f(1, 2));",
        "assert(f(x: 1, y: 2, z: 3) == f(1, 2, 3));" } },
    { "named args: non-scalar values (string / array / bool)",
      { "func f(s, a?, b?) => [s, a, b];",
        "var r = f(s: \"hi\", a: [1, 2], b: true);",
        "assert(r == [\"hi\", [1, 2], true]);" } },
    { "named args: value may be any expression",
      { "func f(x, y?) { if (y == none) return x; return x + y; }",
        "func g() => 5;",
        "assert(f(x: 2 * 3) == 6);",
        "assert(f(x: 1, y: g() + 1) == 7);" } },
    { "named args: required param after an opt one, opt skipped",
      { "func f(x, opt y, z) => [x, y, z];",
        "assert(f(x: 1, z: 3) == [1, none, 3]);",
        "assert(f(1, z: 3) == [1, none, 3]);" } },
    { "named args: nested named calls",
      { "func g(a, b?) { if (b == none) return a; return a + b; }",
        "func f(x) => x * 10;",
        "assert(f(x: g(a: 5, b: 2)) == 70);",
        "assert(f(x: g(a: 5)) == 50);" } },
    { "named args: left-to-right evaluation order is preserved",
      { "var seen = [];",
        "func note(v) { append(seen, v); return v; }",
        "func f(a?, b?, c?) => [a, b, c];",
        "var r = f(a: note(1), c: note(3));",
        "assert(seen == [1, 3]);",       /* the none filler has no effect */
        "assert(r == [1, none, 3]);" } },
    { "named args: through a func-valued var bound to a lambda",
      { "var h = func(p, q?) { if (q == none) return p; return p + q; };",
        "assert(h(p: 1, q: 9) == 10);",
        "assert(h(p: 5) == 5);" } },
    { "named args: a typed param coerces (float <- int) when named",
      { "func f(float a, int b) => a;",
        "assert(type(f(a: 3, b: 2)) == \"float\");",
        "assert(f(a: 3, b: 2) == 3.0);" } },
    { "named args: a pure call with names folds (var + const)",
      /* A named call to an already-declared pure func is desugared to
       * positional at parse time, so it const-folds like the positional form -
       * usable in a `var` and a `const` alike. */
      { "pure func add(a, b) => a + b;",
        "var c = add(a: 3, b: 4); assert(c == 7);",
        "const D = add(a: 10, b: 100); assert(D == 110);" } },
    { "named args: a named pure call folds in a const initializer",
      /* skips the interior opt `y` -> none, all at compile time */
      { "pure func f(x, y?, z?) => [x, y, z];",
        "const C = f(x: 1, z: 5); assert(C == [1, none, 5]);" } },
    { "named args: all-opt function, name just the second",
      { "func f(opt a, opt b) { if (a == none) return b; return a; }",
        "assert(f(b: 9) == 9); assert(f(a: 1) == 1);" } },

    /* --- named-argument errors (all compile-time) --- */
    { "named args err: out-of-order names are rejected",
      { "func f(x, y?, z?) => x; f(z: 3, x: 1);" },
      &typeid(TypeMismatchEx) },
    { "named args err: a name duplicating a positional arg",
      { "func f(x, y?) => x; f(1, x: 2);" },
      &typeid(TypeMismatchEx) },
    { "named args err: unknown parameter name",
      { "func f(x, y?) => x; f(x: 1, w: 9);" },
      &typeid(TypeMismatchEx) },
    { "named args err: duplicate named argument (syntactic)",
      { "func f(x, y?) => x; f(x: 1, x: 2);" },
      &typeid(SyntaxErrorEx) },
    { "named args err: a positional arg after a named one (syntactic)",
      { "func f(x, y?) => x; f(x: 1, 2);" },
      &typeid(SyntaxErrorEx) },
    { "named args err: missing a required interior param",
      { "func f(x, y, z?) => x; f(z: 3);" },
      &typeid(WrongArgCountEx) },
    { "named args err: missing a required trailing param",
      { "func f(x, y) => x; f(x: 1);" },
      &typeid(WrongArgCountEx) },
    { "named args err: too many positional args before a name",
      /* 1,2,3 fill slots 0,1 then 2 >= nparams(2); the trailing name makes it
       * a named call so lower_named_args sees and rejects it. */
      { "func f(x, y?) => x; f(1, 2, 3, z: 4);" },
      &typeid(WrongArgCountEx) },
    { "named args err: wrong type to a named param",
      { "func f(int x, int y) => x; f(x: 1, y: \"s\");" },
      &typeid(TypeMismatchEx) },
    { "named args err: names require a directly-named function (dyn callee)",
      { "var dyn g = 5; g(x: 1);" },
      &typeid(TypeMismatchEx) },
    { "named args err: names not allowed on a builtin call",
      { "print(x: 1);" },
      &typeid(TypeMismatchEx) },
    { "named args err: names not allowed through a function param",
      { "func apply(g, v) => g(a: v); apply(func(a) => a, 3);" },
      &typeid(TypeMismatchEx) },

    /* err loc: a named-arg error points at the offending label */
    { "err loc: out-of-order named argument caret",
      { "func f(x, y?, z?) => x;",
        "f(z: 3, x: 1);" },
      &typeid(TypeMismatchEx), 9, 2 },    /* the `x` label on line 2 */

    /* ------------------------- struct declarations ------------------------ */
    { "struct: a decl binds a type descriptor",
      { "struct Point { int x; int y; }",
        "assert(defined(Point));" } },
    { "struct: explicit field types incl dyn / array? / struct / const",
      { "struct A { int a; }",
        "struct B { dyn d; array? arr; A nested; float f; const K = 3; }",
        "assert(defined(B));" } },
    { "struct: a decl inside a function is scoped to it",
      { "func f() { struct Local { int v; } return defined(Local); }",
        "assert(f() == true);" } },
    { "struct err: a var field is rejected",
      { "struct S { var x; }" }, &typeid(SyntaxErrorEx) },
    { "struct err: an opt scalar field is rejected",
      { "struct S { opt int x; }" }, &typeid(SyntaxErrorEx) },
    { "struct err: an opt str field is rejected (v1)",
      { "struct S { opt str x; }" }, &typeid(SyntaxErrorEx) },
    { "struct err: opt is allowed on dyn/array/dict",
      { "struct S { opt dyn d; array? a; dict? m; }",
        "assert(defined(S));" } },
    { "struct err: duplicate field name",
      { "struct S { int x; int x; }" }, &typeid(SyntaxErrorEx) },
    { "struct err: field vs const name collision",
      { "struct S { int x; const x = 1; }" }, &typeid(SyntaxErrorEx) },
    { "struct err: redeclaring a struct name",
      { "struct P { int x; } struct P { int y; }" },
      &typeid(AlreadyDefinedEx) },
    { "struct err: a duplicate const member",
      { "struct S { const K = 1; const K = 2; }" },
      &typeid(SyntaxErrorEx) },
    { "struct err: a const member colliding with a later field",
      { "struct S { const x = 1; int x; }" }, &typeid(SyntaxErrorEx) },
    { "struct err: an anonymous struct (missing name)",
      { "struct { int x; }" }, &typeid(SyntaxErrorEx) },
    { "struct err: a non-const const member is rejected",
      { "var g = 5; struct S { const K = g; }" },
      &typeid(ExpressionIsNotConstEx) },

    /* ----------------------- struct construction ------------------------- */
    { "struct: positional construction + structural equality",
      { "struct Point { int x; int y; }",
        "assert(Point(1, 2) == Point(1, 2));",
        "assert(Point(1, 2) != Point(1, 3));" } },
    { "struct: named and mixed construction equal the positional form",
      { "struct Point { int x; int y; int z; }",
        "assert(Point(x: 1, y: 2, z: 3) == Point(1, 2, 3));",
        "assert(Point(1, y: 2, z: 3) == Point(1, 2, 3));" } },
    { "struct: a skipped interior opt field becomes none",
      { "struct R { int x; dyn? y; int z; }",
        "assert(R(x: 1, z: 3) == R(1, none, 3));" } },
    { "struct: a var holds the struct type (not dyn)",
      { "struct Point { int x; int y; }",
        "var p = Point(1, 2); assert(type(p) == \"struct\");" } },
    { "struct: float field coerces an int argument",
      { "struct V { float a; int b; }",
        "var v = V(3, 2); assert(v == V(3.0, 2));" } },
    { "struct: inferred array<Struct> of homogeneous elements",
      { "struct Point { int x; int y; }",
        "var a = [Point(1, 2), Point(3, 4)];",
        "assert(len(a) == 2); assert(a[0] == Point(1, 2));" } },
    { "struct: different struct types are never equal",
      { "struct A { int x; } struct B { int x; }",
        "assert((A(1) == B(1)) == false);" } },
    { "struct err: wrong field type (compile-time)",
      { "struct Point { int x; int y; } var p = Point(1, \"s\");" },
      &typeid(TypeMismatchEx) },
    { "struct err: too few field values",
      { "struct Point { int x; int y; } var p = Point(1);" },
      &typeid(WrongArgCountEx) },
    { "struct err: too many field values",
      { "struct Point { int x; int y; } var p = Point(1, 2, 3);" },
      &typeid(WrongArgCountEx) },
    { "struct err: reordered named fields",
      { "struct Point { int x; int y; } var p = Point(y: 2, x: 1);" },
      &typeid(TypeMismatchEx) },
    { "struct err: unknown field name",
      { "struct Point { int x; int y; } var p = Point(x: 1, w: 9);" },
      &typeid(TypeMismatchEx) },

    /* ----------------- struct field & const access ----------------------- */
    { "struct: field read",
      { "struct Point { int x; int y; } var p = Point(1, 2);",
        "assert(p.x == 1); assert(p.y == 2);" } },
    { "struct: field write mutates in place",
      { "struct Point { int x; int y; } var p = Point(1, 2);",
        "p.x = 9; p.y = p.y + 1; assert(p == Point(9, 3));" } },
    { "struct: assignment aliases (mutation is shared, Python-like)",
      { "struct Point { int x; int y; } var p = Point(1, 2); var q = p;",
        "p.x = 9; assert(q.x == 9);" } },
    { "struct: clone() makes an independent copy",
      { "struct Point { int x; int y; } var p = Point(1, 2);",
        "var q = clone(p); p.x = 9; assert(q.x == 1);" } },
    { "struct: const member via instance and via type",
      { "struct C { int x; const K = 42; const NAME = \"c\"; }",
        "var c = C(1); assert(c.K == 42); assert(C.K == 42);",
        "assert(C.NAME == \"c\");" } },
    { "struct: const member folds at parse time via the type",
      { "struct C { int x; const K = 10; } assert(C.K == 10);" } },
    { "struct: a nested struct field",
      { "struct P { int x; int y; } struct Line { P a; P b; }",
        "var l = Line(P(0, 0), P(3, 4));",
        "assert(l.a.x == 0); assert(l.b.y == 4);",
        "l.a = P(9, 9); assert(l.a.x == 9);" } },
    { "struct: an array field",
      { "struct Bag { array items; }",
        "var b = Bag([1, 2, 3]); assert(b.items == [1, 2, 3]);",
        "assert(len(b.items) == 3);" } },
    { "struct: a struct flowing through a dyn var",
      { "struct Point { int x; int y; } var dyn d = Point(5, 6);",
        "assert(d.x == 5); assert(type(d) == \"struct\");" } },
    { "struct: const construction folds and is deep read-only",
      { "struct Point { int x; int y; } const P = Point(1, 2);",
        "assert(P.x == 1); assert(P == Point(1, 2));" } },
    { "struct: a const array of structs",
      { "struct Point { int x; int y; }",
        "const A = [Point(1, 2), Point(3, 4)];",
        "assert(A[0].x == 1); assert(A[1].y == 4);" } },
    { "struct err: writing a const struct's field fails",
      { "struct Point { int x; } const P = Point(1); P.x = 9;" },
      &typeid(NotLValueEx) },
    { "struct err: unknown member access (compile-time)",
      { "struct Point { int x; } var p = Point(1); var z = p.bogus;" },
      &typeid(TypeMismatchEx) },
    { "struct err: a dyn-laundered wrong-typed field value (runtime)",
      { "struct Point { int x; int y; } var dyn d = \"s\";",
        "var p = Point(d, 2);" },
      &typeid(TypeErrorEx) },

    /* ----------------- struct: field kinds & opt & misc ------------------ */
    { "struct: bool / str / array / dict field kinds",
      { "struct All { bool b; str s; array a; dict m; }",
        "var v = All(true, \"hi\", [1, 2], {\"k\": 1});",
        "assert(v.b == true); assert(v.s == \"hi\");",
        "assert(v.a == [1, 2]); assert(v.m[\"k\"] == 1);" } },
    { "struct: an opt field holds a value or none",
      { "struct R { int x; dyn? d; }",
        "assert(R(1, 5).d == 5); assert(R(1, \"s\").d == \"s\");",
        "assert(R(1).d == none);" } },
    { "struct: all-opt struct constructed with no args",
      { "struct O { dyn? a; array? b; }",
        "var o = O(); assert(o.a == none); assert(o.b == none);" } },
    { "struct: != between same-type instances",
      { "struct Point { int x; int y; }",
        "assert((Point(1, 2) != Point(1, 3)) == true);",
        "assert((Point(1, 2) != Point(1, 2)) == false);" } },
    { "struct: nested struct equality recurses",
      { "struct P { int x; int y; } struct L { P a; P b; }",
        "assert(L(P(0, 0), P(1, 1)) == L(P(0, 0), P(1, 1)));",
        "assert(L(P(0, 0), P(1, 1)) != L(P(0, 0), P(2, 2)));" } },
    { "struct: deepclone makes a fully independent copy",
      { "struct Bag { array items; }",
        "var b = Bag([1, 2, 3]); var c = deepclone(b);",
        "append(b.items, 4); assert(len(b.items) == 4);",
        "assert(len(c.items) == 3);" } },
    { "struct: foreach over an array of structs",
      { "struct Point { int x; int y; }",
        "var a = [Point(1, 2), Point(3, 4)]; var sx = 0;",
        "foreach (var p in a) { sx = sx + p.x; }",
        "assert(sx == 4);" } },
    { "struct: pass to and return from a function",
      { "struct Point { int x; int y; }",
        "func swap(p) => Point(p.y, p.x);",
        "var r = swap(Point(1, 2)); assert(r == Point(2, 1));" } },
    { "struct: an (untyped) param infers its struct type from the call site",
      { "struct Point { int x; int y; }",
        "func mag2(p) => p.x * p.x + p.y * p.y;",
        "assert(mag2(Point(3, 4)) == 25);" } },
    { "struct: reassigning a struct variable",
      { "struct Point { int x; int y; }",
        "var p = Point(1, 2); p = Point(5, 6); assert(p.x == 5);" } },
    { "struct: a struct as a dict value and array element",
      { "struct Point { int x; int y; }",
        "var d = {\"o\": Point(0, 0)}; assert(d[\"o\"] == Point(0, 0));",
        "var a = [Point(1, 1)]; assert(a[0] == Point(1, 1));" } },
    { "struct: print shows Type(field: value, ...)",
      { "struct Point { int x; int y; }",
        "assert(str(Point(1, 2)) == \"Point(x: 1, y: 2)\");" } },
    { "struct: an empty-named const member chain via type",
      { "struct Cfg { int n; const MAX = 100; const MIN = 0; }",
        "assert(Cfg.MAX - Cfg.MIN == 100);" } },
    { "struct: a function-local struct used in an array",
      { "func build() {",
        "  struct Pair { int a; int b; }",
        "  return [Pair(1, 2), Pair(3, 4)];",
        "}",
        "var ps = build(); assert(ps[0].a == 1); assert(ps[1].b == 4);" } },
    /* ----------------- POD structs (C byte layout) ---------------------- */
    { "struct POD: mixed scalar kinds (bool/int/float) construct + read",
      { "struct V { bool b; int i; float f; }",
        "var v = V(true, 7, 1.5);",
        "assert(v.b == true); assert(v.i == 7); assert(v.f == 1.5);",
        "assert(v == V(true, 7, 1.5));" } },
    { "struct POD: field writes and compound assigns",
      { "struct V { int i; float f; bool b; }",
        "var v = V(1, 2.0, false);",
        "v.i = 10; v.i += 5; v.i -= 2; v.i *= 2;",
        "v.f += 0.5; v.b = true;",
        "assert(v.i == 26); assert(v.f == 2.5); assert(v.b == true);" } },
    { "struct POD: equality is byte-exact (negative / zero values)",
      { "struct P { int x; int y; }",
        "assert(P(-1, 0) == P(-1, 0));",
        "assert(P(-1, 0) != P(1, 0));",
        "assert(P(0, 0) != P(0, -1));" } },
    { "struct POD: alias shares, clone is independent, const immutable",
      { "struct P { int x; int y; }",
        "var p = P(1, 2); var q = p; p.x = 9; assert(q.x == 9);",
        "var r = clone(p); p.x = 100; assert(r.x == 9);",
        "const C = P(5, 6); assert(C.x == 5);" } },
    { "struct POD err: writing a const POD field fails",
      { "struct P { int x; } const C = P(1); C.x = 9;" },
      &typeid(NotLValueEx) },
    { "struct POD: an array of POD structs reads back correctly",
      { "struct P { int x; int y; }",
        "var a = [P(1, 2), P(3, 4), P(5, 6)];",
        "assert(a[0].x == 1); assert(a[1].y == 4); assert(a[2].x == 5);",
        "var s = 0; foreach (var p in a) { s += p.x + p.y; }",
        "assert(s == 21);" } },
    { "struct POD: nested POD struct field is boxed for now (still works)",
      { "struct P { int x; int y; } struct L { P a; P b; }",
        "var l = L(P(1, 2), P(3, 4));",
        "assert(l.a.x == 1); l.a = P(9, 9); assert(l.a.x == 9);" } },
    { "struct boxed: a ref-field struct stays boxed and mutable",
      { "struct B { dyn d; array a; }",
        "var b = B(5, [1, 2]); assert(b.d == 5);",
        "b.d = \"x\"; append(b.a, 3);",
        "assert(b.d == \"x\"); assert(b.a == [1, 2, 3]);" } },

    /* ------------------ nested POD structs (phase 6) -------------------- */
    { "struct nested POD: a POD-struct field is embedded inline",
      { "struct P { int x; int y; } struct L { P a; P b; }",
        "var l = L(P(1, 2), P(3, 4));",
        "assert(l.a.x == 1); assert(l.b.y == 4);",
        "assert(l == L(P(1, 2), P(3, 4)));",
        "assert(l != L(P(1, 2), P(9, 9)));" } },
    { "struct nested POD: a nested field write (whole + composed)",
      { "struct P { int x; } struct L { P a; int n; }",
        "var l = L(P(1), 5); l.a = P(9); l.n += 1;",
        "assert(l.a.x == 9); assert(l.n == 6);" } },
    { "struct nested POD: an array of nested-POD structs is flat",
      { "struct P { int x; int y; } struct L { P a; P b; }",
        "var arr = [L(P(1, 2), P(3, 4)), L(P(5, 6), P(7, 8))];",
        "assert(array_storage(arr) == \"structs\");",
        "assert(arr[1].a.x == 5); assert(arr[1].b.y == 8);" } },
    { "struct nested POD: const works and is read-only",
      { "struct P { int x; } struct L { P a; int n; }",
        "const C = L(P(7), 3); assert(C.a.x == 7); assert(C.n == 3);" } },
    { "struct nested: a struct with a non-POD (str) field stays boxed",
      { "struct T { int x; str s; } struct W { T t; }",
        "var w = W(T(1, \"hi\")); assert(w.t.x == 1);",
        "assert(w.t.s == \"hi\"); w.t = T(2, \"bye\");",
        "assert(w.t.s == \"bye\");" } },
    { "struct nested: a forward-referenced nested type is boxed but works",
      { "struct L { P a; } struct P { int x; }",
        "var l = L(P(5)); assert(l.a.x == 5);" } },
    { "struct nested: a self-referential struct lays out (no infinite size)",
      { "struct N { int v; dyn? next; }",
        "var n = N(1, none); assert(n.v == 1); assert(n.next == none);" } },

    /* ---------- infinitely-recursive struct rejection (compile) ---------- */
    { "struct recursion: a non-opt self-referential field is rejected",
      { "struct N { int v; N next; }" },
      &typeid(SyntaxErrorEx) },
    { "struct recursion: mutual (A->B->A) non-opt is rejected",
      { "struct A { int v; B b; }",
        "struct B { int v; A a; }" },
      &typeid(SyntaxErrorEx) },
    { "struct recursion: a 3-cycle (A->B->C->A) is rejected",
      { "struct A { B b; } struct B { C c; } struct C { A a; }" },
      &typeid(SyntaxErrorEx) },
    { "struct recursion: an opt (dyn?) back-edge is allowed and constructs",
      { "struct N { int v; dyn? next; }",
        "var n = N(1, N(2, none));",
        "assert(n.v == 1); assert(n.next != none);" } },
    { "struct recursion: a non-cyclic forward chain is allowed",
      { "struct C { int x; } struct B { C c; } struct A { B b; }",
        "var a = A(B(C(7))); assert(a.b.c.x == 7);" } },
    { "struct recursion: a non-opt forward ref (no cycle) is allowed",
      { "struct A { int v; B b; } struct B { int x; }",
        "var a = A(1, B(2)); assert(a.v == 1); assert(a.b.x == 2);" } },
    { "struct recursion: an array/dict field is not a recursive struct field",
      { "struct N { int v; array kids; }",
        "var n = N(1, []); assert(n.v == 1); assert(len(n.kids) == 0);" } },

    /* ------------- flat array<POD struct> storage (phase 7) -------------- */
    { "struct array: a POD-struct array literal is stored flat",
      { "struct P { int x; int y; }",
        "var a = [P(1, 2), P(3, 4)];",
        "assert(array_storage(a) == \"structs\");",
        "assert(a[0].x == 1); assert(a[1].y == 4);",
        "assert(a == [P(1, 2), P(3, 4)]);" } },
    { "struct array: empty + append stays flat (build-up pattern)",
      { "struct P { int x; int y; }",
        "var a = []; var i = 0;",
        "while (i < 4) { append(a, P(i, i * 2)); i = i + 1; }",
        "assert(array_storage(a) == \"structs\"); assert(len(a) == 4);",
        "assert(a[3].x == 3); assert(a[3].y == 6);" } },
    { "struct array: a const POD-struct array is flat and read-only",
      { "struct P { int x; int y; }",
        "const A = [P(1, 2), P(3, 4)];",
        "assert(array_storage(A) == \"structs\");",
        "assert(A[0].x == 1); assert(A[1].y == 4);" } },
    { "struct array: foreach reads each element (COW reuse correctness)",
      { "struct P { int x; int y; }",
        "var a = [P(1, 2), P(3, 4), P(5, 6)]; var s = 0;",
        "foreach (var p in a) { s += p.x * 10 + p.y; }",
        "assert(s == 9 * 10 + 12);" } },     /* (1+3+5)*10 + (2+4+6) */
    { "struct array: foreach element captured keeps its own value (COW)",
      { "struct P { int x; }",
        "var a = [P(1), P(2), P(3)]; var saved = [];",
        "foreach (var p in a) { append(saved, p); }",
        "assert(saved[0].x == 1); assert(saved[1].x == 2);",
        "assert(saved[2].x == 3);" } },
    { "struct array: a mixed literal falls back to general",
      { "struct P { int x; }",
        "var dyn a = [P(1), 5];",
        "assert(array_storage(a) == \"general\"); assert(a[1] == 5);" } },
    { "struct array: a boxed-struct array stays general",
      { "struct B { dyn d; }",
        "var a = [B(1), B(2)];",
        "assert(array_storage(a) == \"general\"); assert(a[0].d == 1);" } },
    { "struct array: cold ops promote to general (insert/map/filter/sort)",
      { "struct P { int x; }",
        "var a = [P(3), P(1), P(2)];",
        "insert(a, 0, P(0)); assert(a[0].x == 0); assert(len(a) == 4);",
        "var xs = map(func(p) => p.x, a); assert(xs == [0, 3, 1, 2]);",
        "var big = filter(func(p) => p.x > 1, a); assert(len(big) == 2);" } },
    { "struct array: appending a different type widens it to general",
      { "struct P { int x; } struct Q { int x; }",
        "var a = [P(1)]; append(a, Q(2));",
        "assert(array_storage(a) == \"general\"); assert(len(a) == 2);" } },
    { "struct array: pop / erase / slice on a flat struct array",
      { "struct P { int x; }",
        "var a = [P(1), P(2), P(3)];",
        "assert(pop(a).x == 3); assert(len(a) == 2);",
        "var s = a[0:2]; assert(s[1].x == 2);",
        "erase(a, 0); assert(a[0].x == 2);" } },
    { "struct array: a[i] = struct stores in place (stays flat)",
      { "struct P { int x; int y; }",
        "var a = [P(1, 2), P(3, 4)]; a[1] = P(9, 8);",
        "assert(array_storage(a) == \"structs\");",
        "assert(a[1].x == 9); assert(a[1].y == 8);" } },
    { "struct array: writing a slice element clones (COW), parent unchanged",
      { "struct P { int x; }",
        "var a = [P(1), P(2), P(3)]; var s = a[0:2]; s[0] = P(9);",
        "assert(s[0].x == 9); assert(a[0].x == 1);" } },
    { "struct array: deepclone is independent",
      { "struct P { int x; }",
        "var a = [P(1), P(2)]; var b = deepclone(a); a[0] = P(9);",
        "assert(a[0].x == 9); assert(b[0].x == 1);" } },
    { "struct array: a bare mixed literal (struct then non-struct) spills",
      { "struct P { int x; }",
        "assert(array_storage([P(1), P(2), 5]) == \"general\");" } },
    { "struct array: spreading a flat struct array into an id-list",
      { "struct P { int x; }",
        "var a = [P(1), P(2)]; var p, q = a;",
        "assert(p.x == 1); assert(q.x == 2);" } },
    { "struct array: built flat inside a function (returned)",
      { "struct P { int x; }",
        "func mk() { var a = []; append(a, P(1)); append(a, P(2));",
        "  return a; }",
        "var r = mk(); assert(array_storage(r) == \"structs\");",
        "assert(r[1].x == 2);" } },

    /* ----------- direct (unboxed) POD field access (phase 8) ----------- */
    { "struct fast: typed int/float field reads compute unboxed",
      { "struct P { int x; int y; float f; }",
        "var p = P(3, 4, 1.5);",
        "assert(p.x + p.y == 7); assert(p.x * 10 - 1 == 29);",
        "assert(p.f * 2.0 == 3.0); assert(p.f + p.x == 4.5);" } },
    { "struct fast: a[i].field reads straight from the flat array",
      { "struct P { int x; int y; float f; }",
        "var a = [P(1, 2, 1.0), P(3, 4, 2.5)];",
        "assert(a[0].x + a[1].y == 5); assert(a[1].x * a[1].y == 12);",
        "assert(a[1].f * 2.0 == 5.0); assert(a[0].f + a[1].x == 4.0);" } },
    { "struct fast: a flat-struct reduction (compound-assign unboxed rhs)",
      { "struct P { int x; int y; }",
        "var a = []; var i = 0;",
        "while (i < 50) { append(a, P(i, i * 2)); i = i + 1; }",
        "var sx = 0; var sy = 0;",
        "for (var k = 0; k < len(a); k += 1) { sx += a[k].x; sy += a[k].y; }",
        "assert(sx == 1225); assert(sy == 2450);" } },
    { "struct fast: a foreach reduction over a flat struct array",
      { "struct P { int x; int y; }",
        "var a = [P(10, 1), P(20, 2), P(30, 3)]; var sx = 0;",
        "foreach (var p in a) { sx += p.x * 100 + p.y; }",
        "assert(sx == 6006);" } },
    { "struct fast: a[i].b.x through a nested-POD element",
      { "struct P { int x; int y; } struct L { P a; P b; }",
        "var ls = [L(P(1, 2), P(3, 4)), L(P(5, 6), P(7, 8))];",
        "assert(ls[0].b.x == 3); assert(ls[1].a.y + ls[1].b.x == 13);" } },
    { "struct fast: a[i].x on a general (boxed) struct array still works",
      { "struct B { int x; dyn d; }",
        "var a = [B(1, \"a\"), B(2, \"b\")];",
        "assert(a[0].x + a[1].x == 3);" } },
    { "struct fast: a bool field via a[i] reads 0/1 in an int context",
      { "struct P { int x; bool b; }",
        "var a = [P(1, true), P(2, false), P(3, true)]; var c = 0;",
        "for (var i = 0; i < 3; i += 1) c += a[i].b;",
        "assert(c == 2);" } },
    { "struct fast: a[i].x with a negative index reads from the end",
      { "struct P { int x; }",
        "var a = [P(1), P(2), P(3)]; var s = 0; s += a[-1].x;",
        "assert(s == 3);" } },
    { "struct fast: a[i].x out of bounds throws (typed read path)",
      { "struct P { int x; }",
        "var a = [P(1)]; var s = 0; s += a[5].x;" },
      &typeid(OutOfBoundsEx) },

    /* ------- construct-in-place: append(flat_arr, Ctor(..)) (phase 8) ----- */
    { "struct build: append(a, S(..)) constructs in place, stays flat",
      { "struct P { int x; int y; }",
        "var a = []; var i = 0;",
        "while (i < 6) { append(a, P(i, i * 10)); i = i + 1; }",
        "assert(array_storage(a) == \"structs\"); assert(len(a) == 6);",
        "assert(a[5].x == 5); assert(a[5].y == 50); assert(a[0].x == 0);" } },
    { "struct build: a named/mixed-arg constructor appends in place",
      { "struct P { int x; int y; }",
        "var a = []; append(a, P(x: 1, y: 2)); append(a, P(3, y: 4));",
        "assert(a[0].x == 1); assert(a[0].y == 2);",
        "assert(a[1].x == 3); assert(a[1].y == 4);",
        "assert(array_storage(a) == \"structs\");" } },
    { "struct build: a nested-POD constructor appends in place",
      { "struct P { int x; int y; } struct L { P a; P b; }",
        "var ls = []; append(ls, L(P(1, 2), P(3, 4)));",
        "assert(ls[0].a.x == 1); assert(ls[0].b.y == 4);",
        "assert(array_storage(ls) == \"structs\");" } },
    { "struct build: a throw mid-construct leaves the array unchanged",
      { "struct P { int x; int y; }",
        "var a = [P(9, 9)]; var dyn z = runtime(0); var caught = false;",
        "try { append(a, P(1, 5 / z)); } catch { caught = true; }",
        "assert(caught); assert(len(a) == 1); assert(a[0].x == 9);",
        "assert(array_storage(a) == \"structs\");" } },
    { "struct build: appending a plain struct value still works",
      { "struct P { int x; }",
        "var a = []; var p = P(7); append(a, p);",
        "assert(a[0].x == 7); assert(array_storage(a) == \"structs\");" } },

    { "struct: an instance is truthy",
      { "struct Point { int x; int y; } var p = Point(0, 0);",
        "var hit = 0; if (p) { hit = 1; } assert(hit == 1);" } },
    { "struct: the type descriptor stringifies to its name",
      { "struct Point { int x; int y; } assert(str(Point) == \"Point\");" } },
    { "struct: type descriptors compare by identity",
      { "struct A { int x; } struct B { int x; }",
        "assert((A == A) == true); assert((A == B) == false);",
        "assert((A != B) == true);" } },
    { "struct: an instance compared to a non-struct is not equal",
      { "struct Point { int x; } var p = Point(1);",
        "assert((p == 5) == false); assert((p != 5) == true);",
        "assert((p == [1]) == false);" } },
    { "struct: intptr tracks instance sharing (alias vs clone)",
      { "struct Point { int x; } var p = Point(1); var q = p;",
        "assert(intptr(p) == intptr(q));",
        "var r = clone(p); assert(intptr(p) != intptr(r));" } },

    /* --------------- explicit type annotations on declarations --------- */
    { "types: scalar annotations declare and check",
      { "int x = 32; float f = 1.5; str s = \"hi\"; bool b = true;",
        "assert(x == 32); assert(type(x) == \"int\");",
        "assert(f == 1.5); assert(type(f) == \"float\");",
        "assert(s == \"hi\"); assert(type(s) == \"str\");",
        "assert(b == true); assert(type(b) == \"bool\");" } },
    { "types: array/dict annotations (generic, element inferred)",
      { "array a = [1,2,3]; dict d = {\"k\": 1};",
        "assert(a == [1,2,3]); assert(type(a) == \"arr\");",
        "assert(array_storage(a) == \"ints\");",   /* element type kept */
        "assert(d[\"k\"] == 1); assert(type(d) == \"dict\");" } },
    { "types: float annotation coerces an int initializer to float",
      { "float f = 3; assert(f == 3.0); assert(type(f) == \"float\");" } },
    { "types: float coercion on reassignment too",
      { "float f = 1.0; f = 5; assert(f == 5.0);",
        "assert(type(f) == \"float\");" } },
    { "types: int annotation coerces a bool to int",
      { "int x = true; assert(x == 1); assert(type(x) == \"int\");" } },
    { "types: uninitialized typed decl gets the zero value",
      { "int i; float f; str s; bool b; array a; dict d;",
        "assert(i == 0); assert(f == 0.0); assert(s == \"\");",
        "assert(b == false); assert(a == []); assert(len(d) == 0);" } },
    { "types: opt scalar annotation is nullable, defaults to none",
      { "opt int x; assert(x == none);",
        "opt int y = 5; assert(y == 5);",
        "opt int z = none; assert(z == none);" } },
    { "types: const scalar annotation (coerced + inlined)",
      { "const float c = 3; assert(c == 3.0); assert(type(c) == \"float\");",
        "const int k = 7; assert(k == 7);" } },
    { "types: a typed for-loop variable",
      { "var s = 0; for (int i = 0; i < 5; i += 1) s += i; assert(s == 10);" } },
    { "types: explicit-typed reassignment keeps the type checked",
      { "int x = 5; x = 10; assert(x == 10); int y = x + 1; assert(y == 11);" } },
    { "types reject: str into int", { "int x = \"hi\";" },
      &typeid(TypeMismatchEx) },
    { "types reject: float into int", { "int x = 3.5;" },
      &typeid(TypeMismatchEx) },
    { "types reject: float into int on reassignment",
      { "int x = 5; x = 2.5;" }, &typeid(TypeMismatchEx) },
    { "types reject: int into bool (not assignable)",
      { "bool b = 1;" }, &typeid(TypeMismatchEx) },
    { "types reject: non-array into array",
      { "array a = 5;" }, &typeid(TypeMismatchEx) },
    { "types reject: array into dict",
      { "dict d = [1, 2];" }, &typeid(TypeMismatchEx) },
    { "types reject: const scalar type mismatch",
      { "const int z = 3.5;" }, &typeid(TypeMismatchEx) },

    /* --------------- explicit type annotations on parameters ----------- */
    { "types: typed parameters declare and check",
      { "func add(int a, float b) => a + b;",
        "assert(add(2, 3) == 5.0); assert(type(add(2, 3)) == \"float\");" } },
    { "types: typed param coerces a widening argument",
      { "func f(float x) => x; assert(f(3) == 3.0); assert(type(f(3)) ==",
        "\"float\");" } },
    { "types: opt typed parameter",
      { "func f(int a, opt int b) { if (b == none) return a; return a + b; }",
        "assert(f(5) == 5); assert(f(5, 2) == 7);" } },
    { "types: a typed param's body type-checks (uncalled shape)",
      { "func sq(int n) => n * n; assert(sq(6) == 36);" } },
    { "types reject: wrong-typed argument to a typed param",
      { "func f(int p) => p; f(\"x\");" }, &typeid(TypeMismatchEx) },
    { "types reject: str argument to a float param",
      { "func g(float x) => x; g(\"no\");" }, &typeid(TypeMismatchEx) },

    /* ----------- null alias + the `?` nullable short syntax ----------- */
    { "null: alias for none",
      { "var x = null; assert(x == none); assert(none == null);",
        "assert(null == null);" } },
    { "null: usable as an opt value",
      { "opt int x = null; assert(x == none);",
        "x = 5; assert(x == 5);" } },
    { "syntax: ? is the canonical nullable suffix (var?/int?/dyn?/array?)",
      { "int? a = 5; assert(a == 5); assert(type(a) == \"int\");",
        "var? b; assert(b == none);",
        "dyn? c; assert(c == none);",
        "array? d; assert(d == none);",          /* opt -> none default */
        "array e; assert(e == []);",             /* non-opt -> zero value */
        "int? f; assert(f == none);" } },        /* opt int uninit -> none */
    { "syntax: ? equals the opt keyword",
      { "int? a = none; opt int b = none;",
        "assert(a == none); assert(b == none);" } },
    { "syntax: dyn is a standalone declaration starter",
      { "dyn z = 5; assert(z == 5); z = \"hi\"; assert(z == \"hi\");",
        "dyn? k; assert(k == none); k = 1; assert(k == 1);" } },
    { "syntax: ~ and trailing ? short forms on parameters",
      { "func f(x, y?, ~z?) { if (y == none) y = -1;",
        "  if (z == none) z = -2; return [x, y, z]; }",
        "assert(f(1) == [1, -1, -2]);",
        "assert(f(1, 2) == [1, 2, -2]);",
        "assert(f(1, 2, 3) == [1, 2, 3]);" } },
    { "syntax: canonical ? on parameters (var?/dyn?/int?)",
      { "func f(var x, var? y, dyn z, int? w) => [x, y, z, w];",
        "assert(f(1, 2, 3, 4) == [1, 2, 3, 4]);",
        "assert(f(1, 2, 3) == [1, 2, 3, none]);" } },   /* w (int?) omitted */
    { "syntax: ~ param is dynamic",
      { "func f(~x) => x; assert(f(1) == 1); assert(f(\"s\") == \"s\");" } },
    { "~ is the bitwise-NOT operator in an expression",
      { "assert(~5 == -6 && ~0 == -1);" } },
    { "syntax reject: trailing name-? is rejected in a body declaration",
      { "int x?;" }, &typeid(SyntaxErrorEx) },

    { "ti: dyn param accepts differing types",
      { "func id(dyn x) => x; assert(id(3) == 3); assert(id(\"s\") == \"s\");" } },
    { "ti: dyn var allows reassignment to a different type",
      { "var dyn d = 5; d = \"hi\"; d = [1,2]; assert(len(d) == 2);" } },
    { "ti: const array used in an expression",
      { "const a = [1,2,3]; assert(sum(a) == 6);" } },
    { "ti: if/else branches join to a common type",
      { "var c = 1; var r = 0; if (c) r = 1; else r = 2; assert(r + 1 == 2);" } },
    { "ti: global visible inside a function",
      { "var g = 10; func f() => g + 1; assert(f() == 11);" } },
    { "ti: forward reference to a function",
      { "func a() => b() + 1; func b() => 41; assert(a() == 42);" } },
    { "ti: unary minus on int / float",
      { "var i = 5; var f = 2.5; assert(-i == -5); assert(-f < 0.0);" } },
    { "ti: compound assignment (int, float, str)",
      { "var i = 1; i += 2; var f = 1.0; f += 1; var s = \"a\"; s += \"b\";",
        "assert(i == 3); assert(f == 2.0); assert(s == \"ab\");" } },
    { "ti: while/for loops with typed vars",
      { "var s = 0; for (var i = 0; i < 5; i += 1) s += i; assert(s == 10);" } },

    /* ---- MUST REJECT: type violations fail at compile time ---- */
    { "ti reject: type change of a var",
      { "var x = 5; x = \"s\";" }, &typeid(TypeMismatchEx) },
    { "ti reject: type change in a loop",
      { "var x = 0; while (x < 1) { x = \"s\"; }" }, &typeid(TypeMismatchEx) },
    { "ti reject: conflicting param types across calls",
      { "func f(x) => x + 1; f(3); f([1,2]);" }, &typeid(TypeMismatchEx) },
    { "ti reject: too many arguments",
      { "func f(a) => a; f(1, 2);" }, &typeid(WrongArgCountEx) },
    { "ti reject: too few arguments",
      { "func f(a, b) => a + b; f(1);" }, &typeid(WrongArgCountEx) },
    { "ti reject: none passed to a non-opt param (forces opt at decl)",
      /* a possibly-none arg to a non-opt param is now reported at the param
       * declaration as OptRequiredEx (the mandatory-`opt` rule). */
      { "func f(x) => 1; var n = none; f(n);" }, &typeid(OptRequiredEx) },
    { "ti accept: a dict read is non-opt (usable as a non-opt arg)",
      { "func f(x) => x + 1; var d = {\"a\": 1}; assert(f(d.a) == 2);" } },
    { "ti accept: get() is opt, narrowed in body",
      { "func f(opt x) { if (x == none) return 0; return x + 1; }",
        "var d = {\"a\": 5};",
        "assert(f(get(d, \"a\")) == 6); assert(f(get(d, \"b\")) == 0);" } },
    /* ---- Phase B: nullability is orthogonal to `dyn` ---- */
    { "ti dyn: a plain dyn is non-null, usable without a check",
      { "func f(dyn x) => x + x;",
        "assert(f(3) == 6); assert(f(\"a\") == \"aa\");" } },
    { "ti dyn: a dyn var holds different types (variant)",
      { "var dyn d = 5; d = \"hi\"; d = [1,2]; assert(len(d) == 2);" } },
    { "ti dyn: a dyn-element array (variant) reads as non-null dyn",
      { "var a = [1, \"two\", 3.0];",
        "assert(a[0] == 1); assert(a[1] == \"two\");",
        "func use(dyn v) => v; assert(use(a[2]) == 3.0);" } },
    { "ti dyn accept: opt dyn parameter takes none, narrowed",
      { "func f(opt dyn x) { if (x == none) return 0; return x + 1; }",
        "assert(f(none) == 0); assert(f(5) == 6);" } },
    { "ti dyn accept: opt dyn var (parses), narrowed",
      { "var opt dyn d = 5; d = none;",
        "if (d != none) assert(d + 1 == 6); assert(1 == 1);" } },
    { "ti dyn accept: dyn var infers opt from none-flow, narrowed",
      { "var dyn d = 5; d = none;",
        "if (d != none) assert(false); assert(1 == 1);" } },
    { "ti dyn accept: non-null dyn arg to a concrete param (type-escape)",
      { "func f(x) => x + 1; var dyn d = 5; assert(f(d) == 6);" } },
    { "ti dyn reject: none to a non-opt dyn param forces opt dyn",
      { "func f(dyn x) => x; f(none);" }, &typeid(OptRequiredEx) },
    { "ti dyn reject: none flowing through a dyn param is caught",
      { "func f(x) => x + 1; func g(dyn d) { return f(d); } g(none);" },
      &typeid(OptRequiredEx) },
    { "ti dyn reject: an opt dyn used in arithmetic (no narrow)",
      { "func f(opt dyn x) => x + 1; f(3);" }, &typeid(NullabilityEx) },
    { "ti dyn reject: a dyn var that got none, used raw",
      { "var dyn d = 5; d = none; var y = d + 1;" }, &typeid(NullabilityEx) },
    { "ti dyn reject: an opt value to a non-opt dyn param forces opt dyn",
      { "func f(dyn x) => x; var d = {\"a\": 5}; f(get(d, \"a\"));" },
      &typeid(OptRequiredEx) },

    { "ti reject: opt value used in arithmetic",
      { "func f(opt x) => x + 1; f(3);" }, &typeid(NullabilityEx) },
    { "ti reject: bare-declared (none) local in arithmetic",
      { "var n; var y = n + 1;" }, &typeid(NullabilityEx) },
    { "ti reject: maybe-none (inferred opt) local used as non-opt",
      { "var x; var c = 1; if (c) x = 5; var y = x + 1;" },
      &typeid(NullabilityEx) },
    { "ti reject: ordering across types",
      { "var s = \"a\"; var b = 1 < s;" }, &typeid(TypeMismatchEx) },
    { "ti reject: calling a non-function",
      { "var x = 5; x();" }, &typeid(TypeMismatchEx) },
    { "ti reject: subscripting a non-container",
      { "var x = 5; var y = x[0];" }, &typeid(TypeMismatchEx) },
    { "ti reject: int + str (through a param)",
      { "func f(x) => 1 + x; f(\"a\");" }, &typeid(TypeMismatchEx) },
    { "ti reject: array + non-array",
      { "var a = [1,2]; var b = a + 5;" }, &typeid(TypeMismatchEx) },
    { "ti reject: invalid compound assignment (str -= int)",
      { "var s = \"x\"; s -= 1;" }, &typeid(TypeMismatchEx) },
    { "ti reject: throwing a non-exception",
      { "var dyn d = 5; throw 5;" }, &typeid(TypeMismatchEx) },
    { "ti reject: assigning an incompatible type to an inferred var",
      { "var x = [1,2]; x = 5;" }, &typeid(TypeMismatchEx) },

    /* ---- flow-sensitive null narrowing (accept) ---- */
    { "ti narrow: if (x != none) then-branch",
      { "func f(opt x) { if (x != none) return x + 1; return 0; }",
        "assert(f(5) == 6); assert(f(none) == 0);" } },
    { "ti narrow: if (x) truthy then-branch",
      { "func f(opt x) { if (x) return x * 2; return -1; }",
        "assert(f(5) == 10);" } },
    { "ti narrow: if (x == none) else-branch",
      { "func f(opt x) { if (x == none) return 0; else return x + 1; }",
        "assert(f(7) == 8); assert(f(none) == 0);" } },
    { "ti narrow: guard clause `if (x == none) return`",
      { "func f(opt x) { if (x == none) return -1; return x * 10; }",
        "assert(f(4) == 40); assert(f(none) == -1);" } },

    /* ---- precise const-container element types (accept) ---- */
    { "ti: heterogeneous const array is array<dyn> (no error)",
      { "const a = [1, \"x\", 3.0]; var s = \"\"; foreach (var e in a) s = s+e;",
        "assert(len(s) > 0);" } },
    { "ti: array(N) then fill is array<int> (foreach sums)",
      { "var a = array(5); for (var i=0;i<5;i+=1) a[i]=i;",
        "var s=0; foreach (var x in a) s += x; assert(s == 10);" } },
    { "ti reject: typed-int use of an un-narrowed opt is still caught",
      { "func f(opt x) => x + 1; f(3);" }, &typeid(NullabilityEx) },

    /* ---- function templates (monomorphization) ---- */
    { "template: a generic helper, no dyn needed (the user's case)",
      { "func f(x, y){ var t = x + y; t += 2; return t; }",
        "assert(f(3, 4) == 9); assert(f(1.5, 2.5) == 6.0);" } },
    { "template: int and str calls don't conflict (separate instances)",
      { "func id(x){ return x; }",
        "assert(id(5) == 5); assert(id(\"hi\") == \"hi\");" } },
    { "template: a never-called template is not checked (no error)",
      { "func unused(a){ var r = a + 1; r += 2; return r; }",
        "assert(true);" } },
    { "template: a derived local needs no `var dyn`",
      { "func g(a){ var r = a + 1; return r; }",
        "assert(g(41) == 42); assert(g(2.5) == 3.5);" } },
    { "template: recursion instantiates once (self-redirect)",
      { "func fib(n){ if (n < 2) return n; return fib(n-1) + fib(n-2); }",
        "assert(fib(10) == 55);" } },
    { "template: a template calling another template",
      { "func dbl(x){ return x * 2; } func quad(x){ return dbl(dbl(x)); }",
        "assert(quad(5) == 20);" } },
    { "template: a wrong-arity call is still an arity error",
      { "func g(a){ return a; } g(1, 2);" }, &typeid(WrongArgCountEx) },
    { "template: a per-instantiation type error is caught",
      { "func g(a){ return a * a; } g(\"x\");" }, &typeid(TypeMismatchEx) },
    { "template: a non-opt param is generic alongside an opt param",
      { "func f(a, opt b){ if (b == none) return a; return a + b; }",
        "assert(f(1) == 1); assert(f(10, 5) == 15); assert(f(\"x\") == \"x\");" } },
    { "template: the non-opt param can take different types (no conflict)",
      { "func g(a, opt tag){ return a; }",
        "assert(g(1) == 1); assert(g(\"s\") == \"s\"); assert(g(2.5) == 2.5);" } },
    { "template: a function with only an opt param stays join (narrows)",
      { "func f(opt x){ if (x != none) return x + 1; return 0; }",
        "assert(f(5) == 6); assert(f(none) == 0);" } },
    { "template: a write-once, calls-only var-bound lambda is a template",
      { "var id = func(x) => x;",
        "assert(id(1) == 1); assert(id(\"s\") == \"s\"); assert(id(2.5)==2.5);" } },
    { "template: a var-bound lambda's typed result is concrete",
      { "var sq = func(n) => n*n; var r = sq(6);",
        "assert(r + 1 == 37);" } },
    { "template: a value-used lambda stays the join model (map)",
      { "var f = func(x) => x*2;",
        "assert(sum(map(f, [1,2,3])) == 12);" } },
    { "template: a capturing lambda stays the join model",
      { "var b = 10; var add = func(n) => n + b;",
        "assert(add(5) == 15);" } },

    /* ---- mandatory `dyn`: a plain var/const must infer a concrete type -- */
    { "ti: mandatory dyn - a plain var inferred dyn is rejected",
      { "var x = runtime(5);" }, &typeid(DynRequiredEx) },
    { "ti: mandatory dyn - declaring it `dyn` is accepted",
      { "var dyn x = runtime(5); assert(x == 5);" } },
    { "ti: mandatory dyn - a var reassigned to a conflicting type is rejected",
      /* the join conflict surfaces as a TypeMismatchEx, not DynRequiredEx */
      { "var x = 1; x = \"s\";" }, &typeid(TypeMismatchEx) },
    { "ti: mandatory dyn - `dyn` allows the conflicting reassignment",
      { "var dyn x = 1; x = \"s\"; assert(x == \"s\");" } },
    { "ti: mandatory dyn - an int accumulator stays concrete (no dyn needed)",
      { "var s = 0; var a = range(5);",
        "foreach (var e in a) s += e; assert(s == 10);" } },
    { "ti: mandatory dyn - array<dyn> is tolerated under plain var (Phase A)",
      { "var a = [1, \"x\"]; assert(len(a) == 2);" } },
    { "ti: mandatory dyn - a foreach loop var is exempt (type is derived)",
      { "var dyn d = [1, \"x\"]; var n = 0;",
        "foreach (var e in d) n += 1; assert(n == 2);" } },

    /* ---- runtime reflection builtins (builtins/reflect.cpp.h) ---- */
    { "reflect: typeof flat int array",
      { "assert(typeof([1, 2, 3]) == \"array<int>\");" } },
    { "reflect: typeof flat float array",
      { "assert(typeof([1.0, 2.0]) == \"array<float>\");" } },
    { "reflect: typeof flat bool array",
      { "assert(typeof([true, false]) == \"array<bool>\");" } },
    { "reflect: typeof str array",
      { "assert(typeof([\"a\", \"b\"]) == \"array<str>\");" } },
    { "reflect: typeof nested array",
      { "assert(typeof([[1, 2], [3, 4]]) == \"array<array<int>>\");" } },
    { "reflect: typeof a heterogeneous (general) array is array<dyn>",
      { "var dyn a = [1, \"x\", 2.0];",
        "assert(typeof(a) == \"array<dyn>\");" } },
    { "reflect: typeof dict",
      { "assert(typeof({1: \"a\"}) == \"dict<int,str>\");" } },
    { "reflect: typeof scalars",
      { "assert(typeof(1) == \"int\");",
        "assert(typeof(2.5) == \"float\");",
        "assert(typeof(true) == \"bool\");",
        "assert(typeof(\"s\") == \"str\");" } },
    { "reflect: typeof none",
      { "var dyn n = none; assert(typeof(n) == \"none\");" } },
    { "reflect: typeof a struct instance is its name",
      { "struct Pt { int x; int y; }",
        "var p = Pt(1, 2); assert(typeof(p) == \"Pt\");" } },
    { "reflect: typeof a flat struct array",
      { "struct Pt { int x; int y; }",
        "assert(typeof([Pt(1, 2), Pt(3, 4)]) == \"array<Pt>\");" } },
    { "reflect: signature of a function",
      { "func f(int a, float b) => a + b;",
        "assert(signature(f) == \"func f(int a, float b)\");" } },
    { "reflect: signature shows pure / const / opt modifiers",
      { "pure func g(const x, opt y) => x;",
        "assert(signature(g) == \"pure func g(const x, opt y)\");" } },
    { "reflect: signature of a struct constructor",
      { "struct Pt { int x; int y; }",
        "assert(signature(Pt) == \"Pt(int x, int y)\");" } },
    { "reflect: signature of a struct instance (its ctor)",
      { "struct Pt { int x; int y; }",
        "var p = Pt(1, 2); assert(signature(p) == \"Pt(int x, int y)\");" } },
    { "reflect: signature rejects a non-function",
      { "signature(42);" }, &typeid(TypeErrorEx) },
    { "reflect: layout of a POD struct",
      { "struct V { int a; float b; }",
        "assert(startswith(layout(V), \"V - POD,\"));" } },
    { "reflect: layout of a boxed struct",
      { "struct N { int v; dyn? next; }",
        "assert(startswith(layout(N), \"N - boxed, 2 slot(s)\"));" } },
    { "reflect: layout rejects a non-struct",
      { "layout(42);" }, &typeid(TypeErrorEx) },
    { "reflect: specializations of a plain function is empty",
      { "func f(int a) => a + 1; assert(len(specializations(f)) == 0);" } },
    { "reflect: specializations rejects a non-function",
      { "specializations(42);" }, &typeid(TypeErrorEx) },
    { "reflect: show() renders a function as code",
      /* sf reads only its param, so it is auto-pure (rendered with a pure
       * marker), then the signature */
      { "func sf(int x) => x + 1;",
        "assert(startswith(show(sf), \"/* pure */ func sf(int x)\"));" } },
    { "reflect: show() renders an expression (folded)",
      { "assert(show(2 + 3 * 4) == \"14\");" } },
    { "reflect: show() renders an expression tree",
      { "assert(show([1, 2, 3]) == \"[1, 2, 3]\");" } },

    /* ---- diagnostic tracing builtins (trace.h) ---- */
    { "trace: tracing() is empty by default",
      { "assert(len(tracing()) == 0);" } },
    { "trace: unknown category throws",
      { "trace(\"bogus\", true);" }, &typeid(InvalidValueEx) },
    { "trace: trace() bad arity",
      { "trace(\"infer\");" }, &typeid(InvalidNumberOfArgsEx) },
    { "trace: traceoff() takes no args",
      { "traceoff(1);" }, &typeid(InvalidNumberOfArgsEx) },
};

static void
dump_expected_ex(const std::type_info *ex, const std::type_info *got)
{
    cout << "  Expected EX: " << (ex ? ex->name() : "<none>") << endl;
    cout << "  Got EX     : " << (got ? got->name() : "<none>") << endl;
}

static bool
check(const test &t, int &err_line, bool dump_syntax_tree)
{
    std::vector<Tok> tokens;
    unique_ptr<Construct> root;

    try {

        for (size_t i = 0; i < t.source.size(); i++)
            lexer(t.source[i], static_cast<int>(i+1), tokens);

        ParseContext pCtx(TokenStream(tokens), true /* const eval */);

        root = pBlock(pCtx);
        infer_types(root.get());
        resolve_names(root.get());
        specialize_types(root.get());
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

        const struct {
            const char *what; int expected; int got;
        } locChecks[] = {
            { "loc_start.col",  t.ex_col,      e.loc_start.col  },
            { "loc_start.line", t.ex_line,     e.loc_start.line },
            { "loc_end.col",    t.ex_col_end,  e.loc_end.col    },
            { "loc_end.line",   t.ex_line_end, e.loc_end.line   },
        };

        for (const auto &lc : locChecks) {
            if (lc.expected && lc.got != lc.expected) {
                cout << "  Expected " << lc.what << ": " << lc.expected
                     << ", got: " << lc.got << endl;
                err_line = e.loc_start.line;
                return false;
            }
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

        for (size_type i = 0; i < t.source.size(); i++) {
            cout << "    ";
            cout << setfill(' ') << setw(3) << i+1;
            cout << "    " << t.source[i];

            if ((int)i+1 == err_line)
                cout << "   <----- GOT EXCEPTION HERE";

            cout << endl;
        }
    }
}

/*
 * Some properties cannot be exercised through a source snippet in the table
 * above (e.g. they involve C++-level APIs). They are checked here instead.
 */
struct extra_check {
    const char *name;
    bool (*fn)();
};

/*
 * Regression check: a Construct must serialize into the stream it is given,
 * not into cout. FuncDeclStmt and TryCatchStmt used to write to cout, so
 * their output would be missing from a different target stream.
 */
static bool
serialize_writes_to_given_stream()
{
    std::vector<Tok> tokens;
    static const char *src[] = {
        "func f(x) => x + 1;",
        "try { f(1); } catch { }",
    };

    for (size_t i = 0; i < sizeof(src) / sizeof(src[0]); i++)
        lexer(src[i], static_cast<int>(i + 1), tokens);

    ParseContext pctx(TokenStream(tokens), true /* const eval */);
    unique_ptr<Construct> root = pBlock(pctx);

    std::ostringstream ss;
    root->serialize(ss, 0);
    const std::string out = ss.str();

    return out.find("FuncDeclStmt") != std::string::npos &&
           out.find("TryCatchStmt") != std::string::npos;
}

/* Build a formatted backtrace from synthetic frames (innermost first). */
static std::string
fmt_bt(int err_line, std::vector<BacktraceFrame> frames)
{
    DivisionByZeroEx ex;
    ex.loc_start = Loc(err_line, 1);
    ex.backtrace = std::move(frames);
    return format_backtrace(ex);
}

static std::vector<std::string>
bt_frame_lines(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream is(s);

    for (std::string ln; std::getline(is, ln); )
        if (ln.size() > 2 && ln[0] == ' ' && ln[2] == '[')
            out.push_back(ln);

    return out;
}

/*
 * Backtrace formatting: param names shown, most-recent on top with "main" at
 * the bottom, single-digit [N] frame numbers, line attribution (error site for
 * the top frame, call site otherwise), and the "at line" column aligned.
 */
static bool
backtrace_format_basic()
{
    const std::string s = fmt_bt(15, {
        { "inner", { "x" }, Loc(12, 1) },
        { "middle", { "a", "b" }, Loc(7, 1) },
    });

    const auto npos = std::string::npos;
    bool ok = true;

    ok = ok && s.find("Backtrace (most recent call first):") != npos;
    ok = ok && s.find("[0] inner(x)") != npos;
    ok = ok && s.find("[1] middle(a, b)") != npos;
    ok = ok && s.find("[2] main()") != npos;
    ok = ok && s.find("at line 15") != npos;   /* inner: error site */
    ok = ok && s.find("at line 12") != npos;   /* middle: call to inner */
    ok = ok && s.find("at line 7") != npos;    /* main: call to middle */
    ok = ok && s.find("[0] inner") < s.find("[1] middle");
    ok = ok && s.find("[1] middle") < s.find("[2] main");

    const auto fr = bt_frame_lines(s);
    ok = ok && fr.size() == 3;

    if (!fr.empty()) {
        const size_t col = fr[0].find(" at line ");
        for (const auto &ln : fr)
            ok = ok && ln.find(" at line ") == col;   /* aligned */
    }

    if (!ok)
        cout << "  got:\n" << s;

    return ok;
}

/* >9 frames: numbers are zero-padded to a common width ([00]..[10]). */
static bool
backtrace_zero_padding()
{
    std::vector<BacktraceFrame> frames;

    for (int i = 0; i < 10; i++)   /* 10 recorded + main == 11 frames */
        frames.push_back({ "f" + std::to_string(i), {}, Loc(i + 1, 1) });

    const std::string s = fmt_bt(99, std::move(frames));
    const auto npos = std::string::npos;
    bool ok = true;

    ok = ok && s.find("[00] f0()") != npos;
    ok = ok && s.find("[09] f9()") != npos;
    ok = ok && s.find("[10] main()") != npos;
    ok = ok && s.find("[0] ") == npos;   /* never a 1-digit number here */

    if (!ok)
        cout << "  got:\n" << s;

    return ok;
}

/* Long frames truncate the param list to ~60 chars; the name is never cut. */
static bool
backtrace_truncation()
{
    const auto npos = std::string::npos;
    bool ok = true;

    std::vector<std::string> many;
    for (int i = 0; i < 30; i++)
        many.push_back("param" + std::to_string(i));

    const std::string s1 = fmt_bt(1, { { "f", many, Loc(2, 1) } });
    ok = ok && s1.find("f(param0") != npos;   /* leading params kept */
    ok = ok && s1.find(", ...)") != npos;     /* and a "..." tail */

    const auto fr1 = bt_frame_lines(s1);
    if (!fr1.empty()) {
        const size_t at = fr1[0].find(" at line ");
        const size_t nameStart = fr1[0].find("] ") + 2;
        ok = ok && (at - nameStart) <= 60;    /* name column within budget */
    }

    /* a single oversized param collapses to "f(...)" */
    const std::string s2 = fmt_bt(1, { { "f", { std::string(80, 'z') },
                                         Loc(2, 1) } });
    ok = ok && s2.find("f(...)") != npos;

    /* an oversized function name is kept (alignment just grows) */
    const std::string longname(70, 'g');
    const std::string s3 = fmt_bt(1, { { longname, { "x" }, Loc(2, 1) } });
    ok = ok && s3.find(longname + "(...)") != npos;

    if (!ok)
        cout << "  got:\n" << s1 << s2 << s3;

    return ok;
}

/* End to end: a real error through a -> b -> main yields the right frames. */
static bool
backtrace_end_to_end()
{
    static const char *src[] = {
        "func a(x) { return x + oops; }",
        "func b(x) { return a(x); }",
        "print(b(10));",
    };

    std::vector<Tok> tokens;
    for (size_t i = 0; i < sizeof(src) / sizeof(src[0]); i++)
        lexer(src[i], static_cast<int>(i + 1), tokens);

    ParseContext pctx(TokenStream(tokens), true);
    unique_ptr<Construct> root = pBlock(pctx);
    resolve_names(root.get());

    std::string bt;

    try {
        root->eval(nullptr);
        return false;   /* should have thrown */
    } catch (const Exception &e) {
        bt = format_backtrace(e);
    }

    const auto npos = std::string::npos;
    bool ok = true;

    ok = ok && bt.find("[0] a(x)") != npos;     /* error in a, line 1 */
    ok = ok && bt.find("[1] b(x)") != npos;     /* b called a, line 2 */
    ok = ok && bt.find("[2] main()") != npos;   /* main called b, line 3 */
    ok = ok && bt.find("at line 1") != npos;
    ok = ok && bt.find("at line 2") != npos;
    ok = ok && bt.find("at line 3") != npos;

    if (!ok)
        cout << "  got:\n" << bt;

    return ok;
}

/*
 * Inlined-frame backtrace reconstruction (flush_inline_frames). No inliner
 * emits InlineCtx yet, so this drives the flush helper directly, in the two
 * positions the real hooks use: flushed into an empty backtrace (error inside
 * inlined code), and flushed after a physical frame (a real call made from
 * inlined code).
 */
static bool
backtrace_inline_frames()
{
    const auto npos = std::string::npos;
    bool ok = true;

    /* (1) error inside g, inlined into f (both virtual). Empty bt -> flush. */
    {
        InlineCtx f_ctx{ "f", { "b" }, Loc(30, 1), nullptr };
        InlineCtx g_ctx{ "g", { "a" }, Loc(12, 1), &f_ctx };

        DivisionByZeroEx ex;
        ex.loc_start = Loc(5, 1);
        flush_inline_frames(&g_ctx, ex);
        const std::string s = format_backtrace(ex);

        ok = ok && s.find("[0] g(a)") != npos;     /* error site, line 5 */
        ok = ok && s.find("[1] f(b)") != npos;     /* f called g, line 12 */
        ok = ok && s.find("[2] main()") != npos;   /* main called f, line 30 */
        ok = ok && s.find("at line 5") != npos;
        ok = ok && s.find("at line 12") != npos;
        ok = ok && s.find("at line 30") != npos;
        ok = ok && s.find("[0] g") < s.find("[1] f");
        if (!ok) cout << "  (1) got:\n" << s;
    }

    /* (2) physical frame P called from inlined g: push P, then flush g. */
    {
        InlineCtx g_ctx{ "g", { "a" }, Loc(15, 1), nullptr };

        DivisionByZeroEx ex;
        ex.loc_start = Loc(5, 1);
        ex.backtrace.push_back({ "P", {}, Loc(8, 1) });   /* physical frame */
        flush_inline_frames(&g_ctx, ex);
        const std::string s = format_backtrace(ex);

        ok = ok && s.find("[0] P()") != npos;      /* error site, line 5 */
        ok = ok && s.find("[1] g(a)") != npos;     /* g called P, line 8 */
        ok = ok && s.find("[2] main()") != npos;   /* main called g, line 15 */
        ok = ok && s.find("at line 8") != npos;
        ok = ok && s.find("at line 15") != npos;
        if (!ok) cout << "  (2) got:\n" << s;
    }

    return ok;
}

/*
 * AST deep-clone (Construct::clone): cloning a parsed + resolved program must
 * produce a structurally identical tree (same serialization) that is a separate
 * object graph and still evaluates correctly on its own (its internal assert
 * holds). Exercises many node types: funcs, pure funcs, arrays, dicts, foreach,
 * for, if/else, try/catch, while.
 */
static bool
ast_clone_roundtrip()
{
    static const char *src[] = {
        "var total = 0;",
        "func add(a, b) { return a + b; }",
        "pure func sq(x) => x * x;",
        "const arr = [1, 2, 3, 4, 5];",
        "const d = {\"a\": 1, \"b\": 2};",
        "foreach (var e in arr) total = total + e;",          /* 15 */
        "for (var i = 0; i < 3; i += 1) {",
        "    if (i % 2 == 0) total = total + sq(i);",         /* +0, +4 */
        "    else total = total - i;",                        /* -1 */
        "}",
        "try { total = total + add(d.a, d.b); }",             /* +3 -> 21 */
        "catch (Exception as ex) { total = -1; }",
        "while (total > 1000) total = total - 1;",
        "assert(total == 21);",
    };

    std::vector<Tok> tokens;
    for (size_t i = 0; i < sizeof(src) / sizeof(src[0]); i++)
        lexer(src[i], static_cast<int>(i + 1), tokens);

    ParseContext pctx(TokenStream(tokens), true);
    unique_ptr<Construct> root = pBlock(pctx);
    resolve_names(root.get());

    unique_ptr<Construct> cloned = root->clone();
    bool ok = true;

    /* (1) separate object graph */
    ok = ok && cloned.get() != root.get();

    /* (2) structurally identical: same serialization */
    std::ostringstream a, b;
    root->serialize(a, 0);
    cloned->serialize(b, 0);
    if (a.str() != b.str()) {
        ok = false;
        cout << "  serialize mismatch\n  orig:\n" << a.str()
             << "\n  clone:\n" << b.str();
    }

    /* (3) the clone evaluates on its own; its assert(total == 21) must hold */
    try {
        cloned->eval(nullptr);
    } catch (const Exception &e) {
        ok = false;
        cout << "  clone eval threw: " << e.name << "\n";
    }

    return ok;
}

/* Parse `lines` into a fresh tree (caller resolves/evals it). */
static unique_ptr<Construct>
parse_lines(const std::vector<const char *> &lines)
{
    std::vector<Tok> tokens;
    for (size_t i = 0; i < lines.size(); i++)
        lexer(lines[i], static_cast<int>(i + 1), tokens);

    ParseContext pctx(TokenStream(tokens), true);
    return pBlock(pctx);
}

static size_t
count_substr(const std::string &s, const std::string &sub)
{
    size_t n = 0, pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) {
        n++;
        pos += sub.size();
    }
    return n;
}

static std::string
serialize_tree(const Construct *c)
{
    std::ostringstream ss;
    c->serialize(ss, 0);
    return ss.str();
}

/*
 * The inliner actually splices: resolving the SAME parsed program with inlining
 * on vs off differs only by the inliner, so a difference proves it ran. Here
 * `f(n)` in g's body is inlined away (one fewer CallExpr), and both versions
 * still evaluate correctly.
 */
static bool
inliner_splices_call()
{
    const std::vector<const char *> src = {
        "func f(x) => x + 1;",
        "func g(n) { return f(n); }",
        "assert(g(5) == 6);",
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);    /* inline */
    resolve_names(off.get(), false);  /* no inline */

    const std::string s_on = serialize_tree(on.get());
    const std::string s_off = serialize_tree(off.get());

    bool ok = true;
    ok = ok && s_on != s_off;                            /* inliner ran */
    ok = ok && count_substr(s_on, "CallExpr")
                   < count_substr(s_off, "CallExpr");    /* a call removed */

    /* both still evaluate (their assert holds) */
    try { on->eval(nullptr); } catch (const Exception &) { ok = false; }
    try { off->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  s_on:\n" << s_on << "\n  s_off:\n" << s_off;
    return ok;
}

/*
 * The invariant that makes inlining acceptable: a runtime error inside an
 * inlined call produces a backtrace IDENTICAL to the non-inlined one (the
 * InlineCtx flush rebuilds the virtual frame).
 */
static bool
inliner_backtrace_identical()
{
    /*
     * The divisor is a runtime, reassigned (so non-const, non-folded) variable
     * that becomes 0, so the error happens at RUN time through the inlined call
     * - not at parse time via const folding.
     */
    const std::vector<const char *> src = {
        "func bad(x) => 100 / x;",
        "var n = 1;",
        "n = 0;",
        "var z = bad(n);",
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);
    resolve_names(off.get(), false);

    std::string bt_on, bt_off;
    try { on->eval(nullptr); }
    catch (const Exception &e) { bt_on = format_backtrace(e); }
    try { off->eval(nullptr); }
    catch (const Exception &e) { bt_off = format_backtrace(e); }

    bool ok = true;
    ok = ok && !bt_on.empty();
    ok = ok && bt_on == bt_off;                  /* identical with/without */
    ok = ok && bt_on.find("[0] bad(x)") != std::string::npos;
    ok = ok && bt_on.find("[1] main()") != std::string::npos;

    if (!ok) cout << "  bt_on:\n" << bt_on << "\n  bt_off:\n" << bt_off;
    return ok;
}

/*
 * Regression: an error thrown by a BUILTIN inside an inlined body must still
 * carry the inlined frames in the backtrace. Builtins bypass do_func_call (the
 * usual flush point) and stamp their own error loc, so the flush can't be keyed
 * off the loc once-guard - it's keyed off inline_origin_emitted instead. Also
 * covers the multi-level case (b inlined into a): both virtual frames appear,
 * exactly once each, identical to the non-inlined run.
 */
static bool
inliner_builtin_error_backtrace_identical()
{
    /* append() on a const array throws CannotChangeConstEx at RUN time; it is
     * a non-const builtin, so it is never folded away, and tbl's first-arg
     * lvalue position keeps it out of re-folding too. */
    const std::vector<const char *> src = {
        "const tbl = [1, 2, 3];",
        "func b() => append(tbl, 9);",
        "func a() => b();",
        "a();",
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);
    resolve_names(off.get(), false);

    std::string bt_on, bt_off;
    try { on->eval(nullptr); }
    catch (const Exception &e) { bt_on = format_backtrace(e); }
    try { off->eval(nullptr); }
    catch (const Exception &e) { bt_off = format_backtrace(e); }

    bool ok = true;
    ok = ok && !bt_on.empty();
    ok = ok && bt_on == bt_off;                  /* identical with/without */
    ok = ok && bt_on.find("[0] b()") != std::string::npos;
    ok = ok && bt_on.find("[1] a()") != std::string::npos;
    ok = ok && bt_on.find("[2] main()") != std::string::npos;
    /* the inlined frame must appear exactly once (no double flush). */
    ok = ok && count_substr(bt_on, "[0] b()") == 1;

    if (!ok) cout << "  bt_on:\n" << bt_on << "\n  bt_off:\n" << bt_off;
    return ok;
}

/*
 * The -it threshold gates inlining: the same program resolved with a tiny
 * threshold keeps the call (body too big), while a large threshold inlines it.
 */
static bool
inliner_threshold_gates()
{
    const std::vector<const char *> src = {
        "func big(x) => x + 1 + 2 + 3;",   /* body is ~5 nodes */
        "func g(n) { return big(n); }",
        "assert(g(0) == 6);",
    };

    unique_ptr<Construct> big = parse_lines(src);
    unique_ptr<Construct> small = big->clone();

    resolve_names(big.get(), true, 24);    /* big enough: inline big() */
    resolve_names(small.get(), true, 2);   /* too small: keep big() call */

    bool ok = count_substr(serialize_tree(small.get()), "CallExpr")
                  > count_substr(serialize_tree(big.get()), "CallExpr");

    try { big->eval(nullptr); }   catch (const Exception &) { ok = false; }
    try { small->eval(nullptr); } catch (const Exception &) { ok = false; }

    return ok;
}

/*
 * Cross-boundary re-fold: a const argument substituted into a NON-pure function
 * (so AutoConst did not fold the whole call) makes a body subexpression all-
 * const, which the inliner's re-fold collapses to a literal. `f(3)` with
 * `f(x) => x * 10 + gv` (gv runtime) splices to `3 * 10 + gv`, and `3 * 10`
 * folds to `30`.
 */
static bool
inliner_refolds_const_subexpr()
{
    const std::vector<const char *> src = {
        "var gv = 1;",
        "gv = gv + 1;",                  /* gv reassigned -> runtime, == 2 */
        "func f(x) => x * 10 + gv;",     /* not pure (reads gv) */
        "var r = f(3);",
        "assert(r == 32);",
    };

    unique_ptr<Construct> root = parse_lines(src);
    resolve_names(root.get(), true, 24);

    const std::string s = serialize_tree(root.get());

    /*
     * The inlined `var r` body holds Int(30) (3 * 10 folded). f's own
     * declaration is kept and still reads `x * 10`, so don't assert on that.
     */
    bool ok = s.find("Int(30)") != std::string::npos;

    try { root->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  resolved tree:\n" << s;
    return ok;
}

/*
 * Const ARRAY/DICT arg specialization: a block-bodied function called with a
 * const array (and a const dict) arg has its element/member reads folded in the
 * clone. The const value is read-only, so this is sound: reads fold, the
 * runtime result is unchanged. Compared against the non-inlined run to prove
 * the fold happened (the constant 60/30 only appears when specialization ran).
 */
static bool
inliner_specializes_array_const_arg()
{
    const std::vector<const char *> src = {
        "const tbl = [10, 20, 30];",
        "const dct = {1: 100, 2: 200};",
        "func pick(a, k) {",
        "    var t = a[0] + a[1] + a[2];",     /* folds to 60 in the clone */
        "    return t + k;",
        "}",
        "func myget(m, k) {",
        "    var s = m[1] + m[2];",            /* dict reads fold to 300 */
        "    return s + k;",
        "}",
        "var n = 1;",
        "n = 5;",                              /* k is runtime */
        "var r = pick(tbl, n) + myget(dct, n);",
        "assert(r == 65 + 305);",
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true, 24);
    resolve_names(off.get(), false);

    const std::string s_on = serialize_tree(on.get());
    const std::string s_off = serialize_tree(off.get());

    bool ok = true;
    ok = ok && s_on.find("$s0") != std::string::npos;   /* specialized */
    ok = ok && s_on.find("$s1") != std::string::npos;   /* both funcs */
    ok = ok && s_on.find("Int(60)") != std::string::npos;  /* a[..] folded */
    ok = ok && s_on.find("Int(300)") != std::string::npos; /* m[..] folded */
    /* off never folds these: no specialization without inlining. */
    ok = ok && s_off.find("$s0") == std::string::npos;
    ok = ok && s_off.find("Int(60)") == std::string::npos;

    try { on->eval(nullptr); } catch (const Exception &) { ok = false; }
    try { off->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  resolved tree (on):\n" << s_on;
    return ok;
}

/*
 * Const-arg specialization: a block-bodied function called with a const arg
 * that gates control flow is cloned and folded (DCE) for that constant, and the
 * call is redirected to the shared clone. Two calls with the SAME const share
 * one clone (dedup). Behavior is unchanged.
 */
static bool
inliner_specializes_block_func()
{
    const std::vector<const char *> src = {
        "func f(mode, x) {",
        "    if (mode == 0) return x;",
        "    else return 0 - x;",
        "}",
        "var a = 1;",
        "a = 7;",                            /* a is runtime (reassigned) */
        "var r = f(0, a) + f(0, a);",        /* same const 0 twice -> 1 clone */
        "assert(r == 14);",
    };

    unique_ptr<Construct> root = parse_lines(src);
    resolve_names(root.get(), true, 24);

    const std::string s = serialize_tree(root.get());

    bool ok = true;
    ok = ok && s.find("$s0") != std::string::npos;    /* specialized */
    ok = ok && s.find("$s1") == std::string::npos;    /* deduped to one */

    try { root->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  resolved tree:\n" << s;
    return ok;
}

/*
 * A runtime error inside a specialized clone must report the ORIGINAL function
 * name (via display_name), not the synthetic clone name - so the backtrace is
 * identical with specialization on vs off.
 */
static bool
inliner_spec_backtrace_identical()
{
    const std::vector<const char *> src = {
        "func f(mode, x) {",
        "    if (mode == 0) return 100 / x;",
        "    else return x;",
        "}",
        "var n = 1;",
        "n = 0;",
        "var z = f(0, n);",     /* specialized; x==0 -> div by zero at run */
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);
    resolve_names(off.get(), false);

    std::string bt_on, bt_off;
    try { on->eval(nullptr); }
    catch (const Exception &e) { bt_on = format_backtrace(e); }
    try { off->eval(nullptr); }
    catch (const Exception &e) { bt_off = format_backtrace(e); }

    bool ok = true;
    ok = ok && !bt_on.empty();
    ok = ok && bt_on == bt_off;                              /* identical */
    ok = ok && bt_on.find("[0] f(mode, x)") != std::string::npos;
    ok = ok && bt_on.find("$") == std::string::npos;    /* no synthetic name */

    if (!ok) cout << "  bt_on:\n" << bt_on << "\n  bt_off:\n" << bt_off;
    return ok;
}

/*
 * Fixpoint: a g-into-f-into-h chain collapses fully in one pass even when
 * declaration order defeats the bottom-up walk (h is declared before the
 * functions it transitively calls, so when h's body is inlined the inner calls
 * weren't inlined yet). Re-scanning each splice inlines them. The funcs are
 * non-pure (read a runtime global) so they CAN'T fold - only inlining removes
 * the calls. After the pass every chain call is gone; single-level inlining
 * would leave the innermost g() behind (so == 1 below, the assert call only,
 * proves the fixpoint, not just one level).
 */
static bool
inliner_fixpoint_collapses_chain()
{
    const std::vector<const char *> src = {
        "var c = 0;",
        "c = 1;",                          /* c is runtime -> g is non-pure */
        "func h(z) => f(z) - 3;",
        "func f(y) => g(y) * 2;",
        "func g(x) => x + c;",
        "var n = 1;",
        "n = 10;",
        "assert(h(n) == 19);",             /* ((10+1)*2)-3 = 19 */
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);
    resolve_names(off.get(), false);

    const std::string s_on = serialize_tree(on.get());
    const std::string s_off = serialize_tree(off.get());

    bool ok = true;
    /* ON: chain fully collapsed -> only the assert() call remains. */
    ok = ok && count_substr(s_on, "CallExpr") == 1;
    /* OFF: nothing inlined -> assert + h(n) + f(z) + g(y) = 4 calls. */
    ok = ok && count_substr(s_off, "CallExpr") > 1;

    try { on->eval(nullptr); } catch (const Exception &) { ok = false; }
    try { off->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  on CallExpr=" << count_substr(s_on, "CallExpr")
                  << " off=" << count_substr(s_off, "CallExpr")
                  << "\n  resolved tree (on):\n" << s_on;
    return ok;
}

/*
 * Fixpoint backtraces: an error in the DEEPEST link of a chain that only
 * collapses via re-scanning must still show every virtual frame, identical to
 * the non-inlined run. This exercises rebase's parent-stacking (the spliced
 * call site already carries an inline_ctx when its inner call is inlined).
 */
static bool
inliner_fixpoint_deep_backtrace()
{
    const std::vector<const char *> src = {
        "var d = 1;",
        "d = 0;",                          /* runtime 0 -> g divides by it */
        "func h(a) => f(a);",
        "func f(b) => g(b);",
        "func g(c) => 100 / c;",
        "var r = h(d);",
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);
    resolve_names(off.get(), false);

    std::string bt_on, bt_off;
    try { on->eval(nullptr); }
    catch (const Exception &e) { bt_on = format_backtrace(e); }
    try { off->eval(nullptr); }
    catch (const Exception &e) { bt_off = format_backtrace(e); }

    bool ok = true;
    ok = ok && !bt_on.empty();
    ok = ok && bt_on == bt_off;                  /* identical with/without */
    ok = ok && bt_on.find("[0] g(c)") != std::string::npos;
    ok = ok && bt_on.find("[1] f(b)") != std::string::npos;
    ok = ok && bt_on.find("[2] h(a)") != std::string::npos;
    ok = ok && bt_on.find("[3] main()") != std::string::npos;

    if (!ok) cout << "  bt_on:\n" << bt_on << "\n  bt_off:\n" << bt_off;
    return ok;
}

/*
 * Tail-call inlining of a BLOCK-bodied function with locals: `return helper(.)`
 * splices helper's body in place, RE-RESOLVING helper's locals to a fresh range
 * at the top of the caller's frame. The scenario is sharp: helper writes a
 * local (`t`) and then reads arg `b` (= caller local `c2`); if the local
 * weren't remapped above the caller's slots it would clobber `c2` and the
 * result would change. Args are runtime (no specialization), so only tail
 * inlining removes the call.
 */
static bool
inliner_tail_inlines_block_body()
{
    const std::vector<const char *> src = {
        "func helper(a, b) {",
        "    var t = a + 100;",       /* writes a fresh local */
        "    return t + b;",          /* reads arg b (caller's c2) afterward */
        "}",
        "func caller(p) {",
        "    var c1 = p;",            /* caller locals: must not be clobbered */
        "    var c2 = p * 1000;",
        "    return helper(c1, c2);", /* tail call -> spliced */
        "}",
        "var n = 1;",
        "n = 3;",                     /* runtime: no specialization */
        "assert(caller(n) == 3103);", /* (3+100) + 3000 */
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);
    resolve_names(off.get(), false);

    const std::string s_on = serialize_tree(on.get());
    const std::string s_off = serialize_tree(off.get());

    bool ok = true;
    /* helper's call is gone from caller's body (spliced in). */
    ok = ok && count_substr(s_on, "CallExpr") < count_substr(s_off, "CallExpr");

    /* correctness (the assert fails -> throws -> ok=false if slots aliased). */
    try { on->eval(nullptr); } catch (const Exception &) { ok = false; }
    try { off->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  on CallExpr=" << count_substr(s_on, "CallExpr")
                  << " off=" << count_substr(s_off, "CallExpr")
                  << "\n  resolved tree (on):\n" << s_on;
    return ok;
}

/*
 * A runtime error inside a tail-inlined BLOCK body still backtraces identically
 * to the non-inlined run: the spliced body carries helper's InlineCtx, so the
 * virtual `helper` frame shows above the real `caller` frame.
 */
static bool
inliner_tail_block_backtrace()
{
    const std::vector<const char *> src = {
        "func helper(a) { var t = 100 / a; return t; }",
        "func caller(x) { return helper(x); }",    /* tail -> spliced */
        "var n = 1;",
        "n = 0;",                                  /* helper(0) -> 100 / 0 */
        "var r = caller(n);",                      /* caller(n) is NOT tail */
    };

    unique_ptr<Construct> on = parse_lines(src);
    unique_ptr<Construct> off = on->clone();

    resolve_names(on.get(), true);
    resolve_names(off.get(), false);

    std::string bt_on, bt_off;
    try { on->eval(nullptr); }
    catch (const Exception &e) { bt_on = format_backtrace(e); }
    try { off->eval(nullptr); }
    catch (const Exception &e) { bt_off = format_backtrace(e); }

    bool ok = true;
    ok = ok && !bt_on.empty();
    ok = ok && bt_on == bt_off;
    ok = ok && bt_on.find("[0] helper(a)") != std::string::npos;
    ok = ok && bt_on.find("[1] caller(x)") != std::string::npos;
    ok = ok && bt_on.find("[2] main()") != std::string::npos;

    if (!ok) cout << "  bt_on:\n" << bt_on << "\n  bt_off:\n" << bt_off;
    return ok;
}

/*
 * Re-fold of NON-MultiOp const ops in a spliced body: a substituted array arg
 * makes `a[0]` and `len(a)` self-contained constants, which fold to literals
 * (subscript and const-builtin call), not just arithmetic. The function is
 * non-pure (reads a runtime global) so AutoConst's whole-call folding didn't
 * already handle it.
 */
static bool
inliner_refolds_non_multiop()
{
    const std::vector<const char *> src = {
        "var g = 1;",
        "g = 2;",                              /* g runtime */
        "func f(a) => a[0] + len(a) + g;",     /* not pure (reads g) */
        "var r = f([10, 20, 30]);",            /* -> 10 + 3 + g */
        "assert(r == 15);",
    };

    unique_ptr<Construct> root = parse_lines(src);
    resolve_names(root.get(), true, 24);

    const std::string s = serialize_tree(root.get());

    bool ok = true;
    ok = ok && s.find("Int(10)") != std::string::npos;   /* a[0] folded */
    ok = ok && s.find("Int(3)") != std::string::npos;     /* len(a) folded */

    try { root->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  resolved tree:\n" << s;
    return ok;
}

/*
 * Const-global folding: a read of a const array/dict GLOBAL with a const index
 * folds in a spliced body. `tbl[0]` (tbl a const global) folds to `100` after a
 * non-pure `pick(0)` is inlined - the global's value is seeded into the fold
 * context from its top-level decl.
 */
static bool
inliner_folds_const_global()
{
    const std::vector<const char *> src = {
        "const tbl = [100, 200, 300];",
        "var g = 1;",
        "g = 2;",
        "func pick(i) => tbl[i] + g;",       /* not pure (reads g) */
        "var r = pick(0);",                  /* -> tbl[0] + g -> 100 + g */
        "assert(r == 102);",
    };

    unique_ptr<Construct> root = parse_lines(src);
    resolve_names(root.get(), true, 24);

    const std::string s = serialize_tree(root.get());

    bool ok = s.find("Int(100)") != std::string::npos;   /* tbl[0] folded */

    try { root->eval(nullptr); } catch (const Exception &) { ok = false; }

    if (!ok) cout << "  resolved tree:\n" << s;
    return ok;
}

/*
 * M0 of the type-inference feature (plans/type-inference.md): unit-check the
 * static-type lattice (stype.h / stype.cpp) directly in C++, since it has no
 * AST wiring yet. These exercise the rules the inferencer will rely on.
 */
static bool stype_ground_caching()
{
    STyArena a;

    if (a.int_ty() != a.int_ty())                 return false;  /* cached */
    if (a.int_ty() == a.int_ty(true))             return false;  /* opt != */
    if (a.with_opt(a.int_ty(), true) != a.int_ty(true)) return false;
    if (a.with_opt(a.int_ty(true), false) != a.int_ty())         return false;
    if (a.fresh_var() == a.fresh_var())           return false;  /* fresh */
    return true;
}

static bool stype_assignable_rules()
{
    STyArena a;
    STyRef i = a.int_ty(), f = a.float_ty(), s = a.str_ty();
    STyRef d = a.dyn_ty(), n = a.none_ty();

    if (!sty_assignable(i, i))            return false;
    if (!sty_assignable(i, f))            return false;  /* int -> float */
    if ( sty_assignable(f, i))            return false;  /* float -/-> int */
    if (!sty_assignable(i, d))            return false;  /* any -> dyn */
    if ( sty_assignable(d, i))            return false;  /* dyn -/-> int */
    if ( sty_assignable(s, i))            return false;  /* str -/-> int */
    if ( sty_assignable(n, i))            return false;  /* none -/-> int */
    if (!sty_assignable(n, a.int_ty(true)))          return false; /* ->opt */
    if ( sty_assignable(a.int_ty(true), i))          return false; /* opt-/->*/
    if (!sty_assignable(i, a.int_ty(true)))          return false;
    if (!sty_assignable(i, a.float_ty(true)))        return false; /* promote*/
    return true;
}

static bool stype_join_rules()
{
    STyArena a;
    STyRef i = a.int_ty(), f = a.float_ty(), s = a.str_ty();
    STyRef n = a.none_ty(), d = a.dyn_ty();

    if (a.join(i, i) != i)                  return false;
    if (a.join(i, f) != f)                  return false;  /* promote */
    if (a.join(n, i) != a.int_ty(true))     return false;  /* none|int=opt */
    if (a.join(i, s) != nullptr)            return false;  /* conflict */
    if (a.join(i, d) != d)                  return false;  /* dyn absorbs */

    STyRef ai = a.array_of(i), as = a.array_of(s);
    if (!sty_equal(a.join(ai, ai), ai))     return false;

    STyRef mixed = a.join(ai, as);                         /* -> array<dyn> */
    if (!mixed || mixed->kind != STyKind::Array)           return false;
    if (sty_resolve(mixed->elem)->kind != STyKind::Dyn)    return false;
    return true;
}

static bool stype_unify_vars()
{
    STyArena a;

    STyRef v = a.fresh_var();
    if (!sty_unify(v, a.int_ty()))          return false;
    if (sty_resolve(v) != a.int_ty())       return false;  /* v bound int */
    if (sty_unify(v, a.str_ty()))           return false;  /* int != str */

    /* occurs-check: w := array<w> is an infinite type and must be rejected */
    STyRef w = a.fresh_var();
    if (sty_unify(w, a.array_of(w)))        return false;

    /* structural unify binds the inner variable */
    STyRef x = a.fresh_var();
    if (!sty_unify(a.array_of(x), a.array_of(a.int_ty()))) return false;
    if (sty_resolve(x) != a.int_ty())       return false;
    return true;
}

static bool stype_to_string_basic()
{
    STyArena a;

    if (sty_to_string(a.int_ty()) != "int")                       return false;
    if (sty_to_string(a.int_ty(true)) != "opt int")              return false;
    if (sty_to_string(a.array_of(a.str_ty())) != "array<str>")   return false;
    if (sty_to_string(a.none_ty()) != "none")                    return false;
    if (sty_to_string(a.dyn_ty()) != "dyn")                      return false;

    STyRef fn = a.func_of({ a.int_ty(), a.str_ty() },
                          { false, true }, a.float_ty());
    if (sty_to_string(fn) != "func(int,opt str)->float")         return false;
    return true;
}

/*
 * REPL tests: a sequence of (input, expected-output-substring) steps driving a
 * SINGLE ReplEngine, so the persisted global context is exercised across steps
 * (input -> output -> input -> output ...). A step passes when the engine's
 * output for that input CONTAINS the expected substring; the whole test passes
 * iff every step does. This is the headless mock of an interactive session - no
 * TTY needed (the line-editor layer is tested separately).
 */
struct repl_step {
    const char *input;
    const char *expect;     /* substring that must appear in the output */
};

struct repl_test {
    const char *name;
    std::vector<repl_step> steps;
};

static const std::vector<repl_test> repl_tests =
{
    { "a var persists across inputs",
      { { "var x = 5", "=> 5" },
        { "x + 1", "=> 6" },
        { "x * x", "=> 25" } } },

    { "a function defined in one input is callable in the next",
      { { "func f(a) => a * 2", "" },
        { "f(21)", "=> 42" },
        { "f(f(3))", "=> 12" } } },

    { "a const folds in a later input",
      { { "const C = 10", "" },
        { "C * 3", "=> 30" },
        { "C + C", "=> 20" } } },

    { "a struct type persists and constructs later",
      { { "struct P { int x; int y; }", "" },
        { "var p = P(3, 4)", "P(x: 3, y: 4)" },
        { "p.x + p.y", "=> 7" } } },

    { "redefining a global rebinds it",
      { { "var x = 5", "=> 5" },
        { "var x = 99", "=> 99" },
        { "x", "=> 99" } } },

    { "an undefined-variable error is recoverable",
      { { "z + 1", "Undefined variable 'z'" },
        { "var z = 3", "=> 3" },
        { "z + 1", "=> 4" } } },

    { "a runtime error does not kill the session",
      { { "var b = 0", "=> 0" },
        { "10 / b", "Division by zero" },
        { "b + 7", "=> 7" } } },

    { "print() output is shown, then the => echo",
      { { "var x = 5", "=> 5" },
        { "print(\"hi\", x)", "hi 5" } } },

    { "multiple statements in one input; last value echoes",
      { { "var a = 1; var b = 2; a + b", "=> 3" } } },

    { "a flat array and a builtin over it persist",
      { { "var arr = [10, 20, 30]", "" },
        { "sum(arr)", "=> 60" },
        { "len(arr)", "=> 3" } } },

    { "a closure captures the persistent global scope",
      { { "var base = 100", "=> 100" },
        { "func addbase(n) => n + base", "" },
        { "addbase(5)", "=> 105" },
        { "var base = 200", "=> 200" },
        { "addbase(5)", "=> 205" } } },

    { "a syntax error in one input is recoverable",
      { { "var q = (1 + ", "SyntaxError" },
        { "var q = 7", "=> 7" },
        { "q", "=> 7" } } },

    { ":tree shows the const-folded syntax tree",
      { { ":tree 2 + 3 * 4", "Int(14)" } } },

    { ":tree does not commit its declarations",
      { { ":tree var wq = 5", "Int(5)" },
        { "wq", "Undefined variable 'wq'" } } },

    { ":help and an unknown command",
      { { ":help", ":source" },
        { ":bogus", "Unknown command" } } },

    { ":analyze reprints the code (optimization view)",
      { { ":analyze var a = [1, 2, 3]", "var a = [1, 2, 3]" } } },

    { "a multi-line func body needs no typed semicolons",
      { { "func g(a) {\n  var r = a + 1\n  return r\n}", "" },
        { "g(41)", "=> 42" } } },

    { "a multi-line dict literal is not statement-terminated",
      { { "var dd = {\n  \"k\": 7,\n  \"m\": 8\n}", "k: 7" },
        { "dd.k + dd.m", "=> 15" } } },

    { "a multi-line array literal persists",
      { { "var aa = [\n  1, 2,\n  3, 4\n]", "[1, 2, 3, 4]" },
        { "sum(aa)", "=> 10" } } },

    { "an if/else block evaluates without typed semicolons",
      { { "var yy = 5", "=> 5" },
        { "if (yy > 0) {\n  yy = 100\n} else {\n  yy = -1\n}", "" },
        { "yy", "=> 100" } } },

    /* ---- faithful cross-input type commitment (incremental inference) ---- */
    { "a committed global's inferred type is pinned across inputs",
      { { "var ci = 3", "=> 3" },
        { "ci = \"hello\"", "has type 'int' but is assigned 'str'" },
        { "ci", "=> 3" } } },             /* the bad input was rejected */

    { "a conforming reassignment across inputs is fine",
      { { "var cj = 3", "=> 3" },
        { "cj = 40", "=> 40" },
        { "cj", "=> 40" } } },

    { "an annotated global says 'is declared', inferred says 'has type'",
      { { "int ck = 3", "=> 3" },
        { "ck = \"x\"", "is declared 'int'" },
        { "var cl = 3", "=> 3" },
        { "cl = \"x\"", "has type 'int'" } } },

    { "a wrong-typed arg to a prior function is caught across inputs",
      { { "func cf(int n) => n + 1", "" },
        { "cf(10)", "=> 11" },
        { "cf(\"x\")", "TypeMismatch" } } },

    { "a flat array's element type is enforced across inputs",
      { { "var ca = [1, 2, 3]", "=> [1, 2, 3]" },
        { "ca[0] + 100", "=> 101" },
        { "ca = [\"a\", \"b\"]", "is assigned" } } },

    { "undef() resets a global's type so it can be redeclared",
      { { "var cu = 3", "=> 3" },
        { "var cu = \"s\"", "is assigned 'str'" },   /* redeclare: rejected */
        { "undef(cu)", "" },                          /* now reset it */
        { "var cu = \"s\"", "=> s" },                 /* fresh type: ok */
        { "cu", "=> s" } } },

    { "the real optimizers run per input (flat array storage)",
      { { "var fa = [1, 2, 3]", "=> [1, 2, 3]" },
        { "array_storage(fa)", "=> ints" },
        { "var fb = [1.5, 2.5]", "" },
        { "array_storage(fb)", "=> floats" } } },

    { "a function can be redefined (edit + resubmit)",
      { { "func rf(n) => n * 2", "" },
        { "rf(5)", "=> 10" },
        { "func rf(n) => n * 3", "" },
        { "rf(5)", "=> 15" } } },

    { "a struct can be redefined",
      { { "struct RS { int x; }", "" },
        { "struct RS { int x; int y; }", "" },
        { "var rsv = RS(1, 2)", "RS(x: 1, y: 2)" },
        { "rsv.x + rsv.y", "=> 3" } } },

    { "a multi-line input is one history/eval unit",
      { { "func ml(int a) {\n  var t = a + 1\n  return t * 10\n}", "" },
        { "ml(4)", "=> 50" } } },

    { "a template defined then called across inputs instantiates per type",
      { { "func tg(a){ var r = a + 1; return r; }", "" },
        { "tg(41)", "=> 42" },
        { "tg(2.5)", "=> 3.5" },
        { "tg(\"x\")", "=> x1" } } },

    /* ---- runtime reflection over the persistent global scope ---- */
    { "reflect: globals() lists user globals, excludes builtins",
      { { "var gx = 1", "=> 1" },
        { "func gf(a) => a", "" },
        { "globals()", "[gf, gx]" } } },

    { "reflect: specializations() surfaces a template instance",
      { { "func tf(a) => a + 1", "" },
        { "tf(3)", "=> 4" },
        { "specializations(tf)", "tf$0" } } },

    /* ---- cross-input template instantiation: reuse, not duplicate ---- */
    { "template: the same signature across inputs reuses one instance",
      { { "func tr(x, y) { var s = x + y; return s + 1; }", "" },
        { "tr(2, 3)", "=> 6" },
        { "specializations(tr)", "[tr$0]" },
        { "tr(10, 20)", "=> 31" },            /* same (int,int): reuse */
        { "specializations(tr)", "[tr$0]" } } },   /* STILL one instance */

    { "template: a distinct signature across inputs makes a new instance",
      { { "func tq(x, y) { var s = x + y; return s; }", "" },
        { "tq(1, 2)", "=> 3" },
        { "tq(1.0, 2.0)", "=> 3.0" },
        { "specializations(tq)", "[tq$0, tq$1]" } } },

    { "template: an instance is inspectable by its name (typeof f$0)",
      { { "func ti(x, y) => x + y", "" },
        { "ti(2, 3)", "=> 5" },
        { "typeof(ti$0)", "func ti(" } } },

    { ":show renders a function with inferred param + return types",
      { { "func sm(x, y) { var t = x + y; return t; }", "" },
        { "sm(2, 3)", "=> 5" },
        /* the instance shows inferred param types AND the return type */
        { ":show sm", "int func sm$0(int x, int y)" } } },

    { ":show of an expression renders its optimized (folded) form",
      { { ":show 2 + 3 * 4", "14" } } },

    /* a counted for-loop specializes to a ForRangeStmt (shown "counted") */
    { ":show shows a counted for-loop specialized (ForRangeStmt)",
      { { "func cf(int n) { var s = 0; for (var i = 0; i < n; i++) {"
          " s += i; } return s; }", "" },
        { ":show cf", "counted" } } },
    { ":show: a loop whose bound is mutated is NOT specialized",
      { { "func nf(int n) { var s = 0; for (var i = 0; i < n; i++) {"
          " s += i; n = n - 1; } return s; }", "" },
        { ":show nf", "for (" } } },     /* a plain for, no "counted" marker */
    /* a CROSS-INPUT pure user func (block body) as a loop bound specializes:
       the bound calls a prior-input pure func with a scalar arg. */
    { ":show: a block-bodied pure func bound specializes (counted)",
      { { "func lm(int x) { var t = x + 3; return t; }", "" },
        { "func us(int n) { var s = 0; for (var i = 0; i < lm(n); i++) {"
          " s += i; } return s; }", "" },
        { ":show us", "counted" } } },
    /* a pure CONTAINER-arg bound specializes now that `pure` forbids input
       mutation (so caching the call once is sound). */
    { ":show: a pure container-arg func bound specializes (counted)",
      { { "func cnt(array a) { var n = len(a); return n - 1; }", "" },
        { "func uc(array x) { var s = 0; for (var i = 0; i < cnt(x); i++) {"
          " s += x[i]; } return s; }", "" },
        { ":show uc", "counted" } } },

    /* cross-input inlining: a pure function from an EARLIER input is inlined +
     * folded into a function defined in a LATER one (caller$0 from a prior
     * input becomes `x + 6` here - the c2 case). */
    { ":show inlines+folds a PRIOR-input pure call into a later function",
      { { "func xf(x, y) => x + y", "" },
        { "func xcaller(a, b) => xf(a, 2 * b)", "" },
        { "xcaller(2, 3)", "=> 8" },                /* creates xcaller$0 */
        { "func xc2(x) => xcaller(x, 3)", "" },     /* xcaller$0 is prior */
        { "xc2(10)", "=> 16" },                     /* instantiates xc2$0 */
        { ":show xc2$0", "x + " },                  /* caller inlined away */
        { ":show xc2$0", "6" } } },                 /* 2*3 folded to 6 */

    /* cross-input inline must substitute a param used TWICE even inside an M8
     * TypedScalarExpr node (the prior body is post-specialization): regression
     * for `func mk(a)=>[a, a*2]` inlined cross-input dropping the 2nd `a`. */
    { ":show cross-input inline substitutes a param used twice (M8 body)",
      { { "func mk2(a) => [a, a * 2, a + a]", "" },
        { "mk2(3)", "" },                           /* creates mk2$0 */
        { "func um2() => mk2(5)", "" },             /* cross-input inline */
        { "um2()", "=> [5, 10, 10]" },              /* correct at runtime */
        { ":show um2", "[5, 10, 10]" } } },         /* all `a` subst + folded */

    /* a template instance appending to a PINNED global array must not trip the
     * global's assignability check before the arg type settles (defer-on-
     * Unknown for array<?> -> the element type). */
    { "template: append to a pinned global array from a function works",
      { { "var ag = [1, 2, 3]", "" },
        { "func af(x) { append(ag, x); }", "" },
        { "af(4)", "" },
        { "ag", "[1, 2, 3, 4]" } } },

    /* redefining a function GCs its now-orphaned instances (created by a
     * throwaway top-level call), but keeps any a function still consumes. */
    { "template: redefining a function drops its orphaned instances",
      { { "func rf(x, y) => x + y", "" },
        { "rf(1, 2)", "=> 3" },
        { "specializations(rf)", "[rf$0]" },
        { "func rf(x, y) => x * y", "" },          /* redefine */
        { "specializations(rf)", "[]" } } },       /* orphan dropped */

    { "auto-pure: a pure call to a PRIOR-input function folds (cross-input)",
      { { "func pa(a, b) => a + b", "" },
        { "func pf(x) => pa(x, 10)", "" },
        { "pf(5)", "=> 15" },                  /* creates pf$0 */
        { "func pg() => pf(3)", "" },          /* pf$0 is from a prior input */
        { ":show pg", "=> 13" } } },           /* folded: pa(3,10) = 13 */

    { "template: a consumed instance survives a redefinition",
      { { "func cf(x, y) => x + y", "" },
        { "cf(1, 2)", "=> 3" },
        { "func cg() { return cf(1, 2); }", "" },  /* cg consumes cf$0 */
        { "func cf(x, y) => x * y", "" },          /* redefine cf */
        { "specializations(cf)", "[cf$0]" },       /* kept */
        { "cg()", "=> 3" } } },                     /* still callable */

    { "trace: an uninstantiated template is reported as a template, not dyn",
      { { ":trace infer on", "tracing: infer" },
        { "func tt(a, b) { var u = a + b; return u; }",
          "template (instantiated per call)" },
        { ":trace off", "tracing: off" } } },

    /* ---- diagnostic tracing narrates the compiler's reasoning ---- */
    { "trace: :trace infer narrates inference into the REPL output",
      { { ":trace infer on", "tracing: infer" },
        { "var tx = 5", "tx : int" },
        { ":trace off", "tracing: off" } } },

    { "trace: an unknown :trace category is reported",
      { { ":trace bogus on", "Unknown trace category" },
        { ":trace off", "tracing: off" } } },

    { "trace: :trace help lists the categories as a bullet list",
      { { ":trace help", "- autopure " } } },

    { "help: :help :trace and :help trace both reach the tracer",
      { { ":help :trace", "[REPL command]" },
        { ":help trace", "also a REPL command" },
        { ":help commands", ":source <file>" } } },

    /* ---- :globals / :type rich inspection views ---- */
    { ":globals lists a function with its signature",
      { { "var gv = 7", "=> 7" },
        { "func gfn(int a) => a", "" },
        { ":globals", "gfn : func gfn(int a)" } } },

    { ":globals shows a const scalar (folded, from the const ctx)",
      { { "const KC = 42", "" },
        { ":globals", "KC : int   [const]" } } },

    { ":type of a global shows its inferred static type",
      { { "var ta = [1, 2, 3]", "" },
        { ":type ta", "array<int>" } } },

    { ":type of an expression shows the runtime type",
      { { ":type [1.5, 2.5]", "array<float>" } } },
};

static bool run_one_repl_test(const repl_test &t)
{
    ReplEngine engine;

    for (const auto &s : t.steps) {

        const std::string out = engine.eval_input(s.input);

        if (out.find(s.expect) == std::string::npos) {
            cout << "  step input : " << s.input << "\n";
            cout << "  expected   : " << s.expect << "\n";
            cout << "  got        : " << out << "\n";
            return false;
        }
    }

    return true;
}

/*
 * LineEditor core tests (the pure, headless part of the line editor). Each
 * feeds a byte script - including raw control codes (Ctrl-x is byte x) and ESC
 * sequences for the arrows - and asserts the resulting buffer/cursor. No TTY.
 */
static void le_feed(LineEditor &ed, const std::string &bytes)
{
    for (char c : bytes)
        ed.feed(static_cast<unsigned char>(c));
}

static bool lineedit_typing_and_backspace()
{
    LineEditor ed;
    le_feed(ed, "abc");
    if (ed.buffer() != "abc" || ed.cursor() != 3) return false;
    ed.feed(127);                                  /* Backspace */
    return ed.buffer() == "ab" && ed.cursor() == 2;
}

static bool lineedit_cursor_move_and_insert()
{
    LineEditor ed;
    le_feed(ed, "abc");
    ed.feed(2); ed.feed(2);                        /* Ctrl-B x2 -> cursor 1 */
    ed.feed('X');
    return ed.buffer() == "aXbc" && ed.cursor() == 2;
}

static bool lineedit_home_end_kill()
{
    LineEditor ed;
    le_feed(ed, "hello");
    ed.feed(1);                                    /* Ctrl-A (home) */
    ed.feed('Z');
    if (ed.buffer() != "Zhello") return false;
    ed.feed(11);                                   /* Ctrl-K (kill to end) */
    if (ed.buffer() != "Z") return false;
    ed.feed(5);                                    /* Ctrl-E (end) */
    return ed.cursor() == 1;
}

static bool lineedit_kill_word()
{
    LineEditor ed;
    le_feed(ed, "foo bar");
    ed.feed(23);                                   /* Ctrl-W */
    return ed.buffer() == "foo " && ed.cursor() == 4;
}

static bool lineedit_arrow_keys()
{
    LineEditor ed;
    le_feed(ed, "ab");
    le_feed(ed, "\033[D");                         /* left arrow -> cursor 1 */
    ed.feed('X');
    if (ed.buffer() != "aXb" || ed.cursor() != 2) return false;
    le_feed(ed, "\033[C");                         /* right arrow -> cursor 3 */
    return ed.cursor() == 3;
}

static bool lineedit_delete_key()
{
    LineEditor ed;
    le_feed(ed, "abc");
    ed.feed(1);                                    /* home */
    le_feed(ed, "\033[3~");                        /* Delete -> remove 'a' */
    return ed.buffer() == "bc" && ed.cursor() == 0;
}

static bool lineedit_submit_action()
{
    LineEditor ed;
    le_feed(ed, "abc");
    return ed.feed(13) == LineEditor::Action::submit && ed.buffer() == "abc";
}

static bool lineedit_ctrl_c_and_d()
{
    LineEditor ed;
    if (ed.feed(3) != LineEditor::Action::cancel) return false;   /* Ctrl-C */
    LineEditor ed2;
    if (ed2.feed(4) != LineEditor::Action::eof) return false;     /* C-D empty */
    LineEditor ed3;
    le_feed(ed3, "ab");
    ed3.feed(1);                                   /* home */
    if (ed3.feed(4) != LineEditor::Action::none)   /* C-D non-empty: del fwd */
        return false;
    return ed3.buffer() == "b";
}

/* a simple completeness test for the multi-line editor tests: brackets balance */
static bool le_balanced(const std::string &s)
{
    int d = 0;
    for (char c : s) {
        if (c == '{' || c == '(' || c == '[') d++;
        else if (c == '}' || c == ')' || c == ']') d--;
    }
    return d <= 0;
}

static bool lineedit_multiline_enter_inserts_newline()
{
    LineEditor ed;
    ed.set_submitter(le_balanced);
    le_feed(ed, "func f() {");
    /* unbalanced -> Enter inserts a newline + auto-indent (2 spaces) */
    if (ed.feed(13) != LineEditor::Action::none) return false;
    if (ed.buffer() != "func f() {\n  ") return false;
    le_feed(ed, "return 1;");
    ed.feed(13);                       /* still one open brace -> newline */
    le_feed(ed, "}");
    /* balanced now -> Enter submits */
    return ed.feed(13) == LineEditor::Action::submit;
}

static bool lineedit_multiline_cursor_and_nav()
{
    LineEditor ed;
    ed.set_buffer("ab\ncde\nf");       /* 3 lines, cursor at end */
    if (ed.cursor_row() != 2 || ed.cursor_col() != 1) return false;
    ed.feed(16);                       /* Ctrl-P: up to line 1, col 1 */
    if (ed.cursor_row() != 1 || ed.cursor_col() != 1) return false;
    ed.feed(16);                       /* up to line 0, col 1 */
    if (ed.cursor_row() != 0 || ed.cursor_col() != 1) return false;
    ed.feed(14);                       /* Ctrl-N: back down to line 1 */
    return ed.cursor_row() == 1;
}

static bool lineedit_multiline_home_end_kill()
{
    LineEditor ed;
    ed.set_buffer("first\nsecond");    /* cursor at end of line 1 */
    ed.feed(1);                        /* Ctrl-A: start of CURRENT line */
    if (ed.cursor_col() != 0) return false;
    ed.feed(11);                       /* Ctrl-K: kill to end of LINE */
    if (ed.buffer() != "first\n") return false;
    return ed.cursor_row() == 1 && ed.cursor_col() == 0;
}

static bool lineedit_multiline_tilde_home_end()
{
    /* the ESC[1~ / ESC[4~ Home/End variants must be line-relative too */
    LineEditor ed;
    ed.set_buffer("first\nsecond");    /* cursor at end of line 1 */
    ed.feed(27); ed.feed('['); ed.feed('1'); ed.feed('~');   /* Home */
    if (ed.cursor_row() != 1 || ed.cursor_col() != 0) return false;
    ed.feed(27); ed.feed('['); ed.feed('4'); ed.feed('~');   /* End */
    return ed.cursor_row() == 1 && ed.cursor_col() == 6;     /* "second" */
}

static bool lineedit_history_nav()
{
    std::vector<std::string> h = { "first", "second" };
    LineEditor ed;
    ed.set_history(&h);
    le_feed(ed, "live");                           /* the live line */
    le_feed(ed, "\033[A");                         /* up -> "second" */
    if (ed.buffer() != "second") return false;
    le_feed(ed, "\033[A");                         /* up -> "first" */
    if (ed.buffer() != "first") return false;
    le_feed(ed, "\033[B");                         /* down -> "second" */
    if (ed.buffer() != "second") return false;
    le_feed(ed, "\033[B");                         /* down -> back to live */
    return ed.buffer() == "live";
}

/* Strip ANSI escape sequences (ESC '[' ... final-letter) from `s`. */
static std::string strip_ansi(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\033') {
            i++;
            if (i < s.size() && s[i] == '[')
                while (i < s.size() && !isalpha((unsigned char)s[i]))
                    i++;
            continue;                          /* skip the final letter too */
        }
        out += s[i];
    }
    return out;
}

static bool highlight_inserts_color()
{
    set_highlight_enabled(true);
    const std::string h = highlight_line("var x = 5  # c");
    /* keyword, number and comment should each introduce an escape */
    return h.find("\033[") != std::string::npos &&
           h != "var x = 5  # c";
}

static bool highlight_preserves_visible_text()
{
    set_highlight_enabled(true);
    const std::string src = "func f(n) => n * 2  # cmt \"s\"";
    /* the colored text, with escapes removed, must equal the input exactly -
     * this is what guarantees the editor's cursor columns stay correct */
    return strip_ansi(highlight_line(src)) == src;
}

static bool highlight_disabled_is_identity()
{
    set_highlight_enabled(false);
    const std::string s = "var x = 5  # c";
    const bool ok = highlight_line(s) == s;
    set_highlight_enabled(true);
    return ok;
}

static bool highlight_tolerates_unterminated_string()
{
    set_highlight_enabled(true);
    /* mid-edit: an unclosed string must not throw or drop characters */
    const std::string src = "var s = \"hello";
    return strip_ansi(highlight_line(src)) == src;
}

static bool has_cand(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

static bool repl_completion_globals_builtins_keywords()
{
    ReplEngine e;
    e.eval_input("var counter = 1");
    e.eval_input("var country = 2");
    /* a global prefix completes to both globals */
    const auto g = e.completions("coun", 4);
    if (!has_cand(g, "counter") || !has_cand(g, "country")) return false;
    /* a builtin and a keyword complete too */
    if (!has_cand(e.completions("le", 2), "len")) return false;
    if (!has_cand(e.completions("retu", 4), "return")) return false;
    /* an unrelated prefix yields nothing */
    return e.completions("zzzq", 4).empty();
}

static bool repl_completion_struct_members()
{
    ReplEngine e;
    e.eval_input("struct P { int alpha; int beta; }");
    e.eval_input("var p = P(1, 2)");
    /* `p.` lists the fields */
    const auto all = e.completions("p.", 2);
    if (!has_cand(all, "alpha") || !has_cand(all, "beta")) return false;
    /* `p.al` uniquely completes to alpha */
    const auto one = e.completions("p.al", 4);
    return one.size() == 1 && one[0] == "alpha";
}

static bool lineedit_tab_unique_completion()
{
    LineEditor ed;
    ed.set_completer([](const std::string &, size_t) {
        return std::vector<std::string>{ "counter" };
    });
    le_feed(ed, "coun");
    ed.feed(9);                                    /* Tab */
    return ed.buffer() == "counter" && ed.cursor() == 7;
}

static bool lineedit_tab_common_prefix()
{
    LineEditor ed;
    ed.set_completer([](const std::string &, size_t) {
        return std::vector<std::string>{ "counter", "country" };
    });
    le_feed(ed, "cou");
    ed.feed(9);                                    /* Tab */
    /* extends to the longest common prefix "count" and offers the list */
    if (ed.buffer() != "count") return false;
    const auto offered = ed.take_completions();
    return offered.size() == 2;
}

static bool repl_incomplete_detection()
{
    if (!ReplEngine::is_incomplete("func f() {")) return false;   /* open { */
    if (!ReplEngine::is_incomplete("var a = [1,")) return false;  /* open [  */
    if (!ReplEngine::is_incomplete("1 +")) return false;          /* trailing op */
    if (!ReplEngine::is_incomplete("foo(1, 2")) return false;     /* open ( */
    if (ReplEngine::is_incomplete("var x = 5")) return false;     /* complete */
    if (ReplEngine::is_incomplete("func f() { return 1; }"))      /* complete */
        return false;
    if (ReplEngine::is_incomplete("[1, 2, 3]")) return false;     /* complete */
    return true;
}

/* A substring test helper for the headless :help renderer. */
static bool help_has(const std::string &topic, const char *needle)
{
    const std::string s = repl_help(topic, /*color=*/false);
    return s.find(needle) != std::string::npos;
}

static bool replhelp_overview_and_builtins()
{
    if (!help_has("", "MyLang REPL help"))           return false;
    if (!help_has("", ":help builtins"))             return false;
    if (!help_has("builtins", "Arrays"))             return false;
    if (!help_has("builtins", "Reflection"))         return false;
    if (!help_has("builtins math", "sqrt"))          return false;
    return true;
}

static bool replhelp_builtin_entries()
{
    /* a builtin signature + its description, including the bang name */
    if (!help_has("typeof", "typeof(x)"))            return false;
    if (!help_has("typeof", "structural type"))      return false;
    if (!help_has("get!", "KeyNotFoundEx"))          return false;
    if (!help_has("runtime", "optimization barrier")) return false;
    /* the const-vs-runtime kind note */
    if (!help_has("len", "const ("))                 return false;
    if (!help_has("print", "runtime ("))             return false;
    return true;
}

static bool replhelp_language_reference()
{
    /* the language index + a category expansion */
    if (!help_has("language", "Optimizations"))      return false;
    if (!help_has("language", "Type system"))        return false;
    if (!help_has("optimizations", "Inlining"))      return false;
    if (!help_has("optimizations", "[inlining]"))    return false;
    /* an individual feature: syntax + body, incl. an optimization */
    if (!help_has("inlining", "spliced in place"))   return false;
    if (!help_has("templates", "$0"))                return false;
    if (!help_has("foreachloop", "indexed"))         return false;
    if (!help_has("namedargs", "IDENTICALLY"))       return false;
    return true;
}

static bool trace_module_basics()
{
    /* save + restore the global trace state so the suite stays clean */
    const unsigned saved_mask = g_trace_mask;
    std::ostream *saved_sink = trace_sink();

    g_trace_mask = 0;
    std::ostringstream cap;
    trace_set_sink(&cap);

    bool ok = true;
    ok = ok && trace_set("infer", true);
    ok = ok && trace_enabled(TraceCat::infer);
    ok = ok && !trace_enabled(TraceCat::inlining);
    ok = ok && !trace_set("nope", true);          /* unknown -> false */

    trace_emit(TraceCat::infer, 1, "hello world");
    const std::string s = cap.str();
    ok = ok && s.find("infer") != std::string::npos;
    ok = ok && s.find("hello world") != std::string::npos;

    trace_set("all", true);
    ok = ok && trace_active().size() == 8;
    ok = ok && trace_state_str().find("infer") != std::string::npos;
    trace_clear_all();
    ok = ok && trace_active().empty();
    ok = ok && trace_state_str() == "tracing: off";

    trace_set_sink(saved_sink);
    g_trace_mask = saved_mask;
    return ok;
}

/*
 * Drive the real compile pipeline with every trace category on and a captured
 * sink, asserting each category narrates at least one decision. The program is
 * crafted to exercise all of them: a template (compute/helper -> <name>$N), a
 * flat array (arrays), an auto-pure func (helper), a write-once local
 * (autoconst k/t), an inlined call (helper spliced), constant folds, and a
 * const-arg specialization (compute$0 -> compute$0$s0).
 */
static bool trace_pipeline_categories()
{
    const unsigned saved_mask = g_trace_mask;
    std::ostream *saved_sink = trace_sink();

    g_trace_mask = 0;
    std::ostringstream cap;
    trace_set_sink(&cap);
    trace_set("all", true);

    bool ok = true;
    try {
        /* helper is pure; compute has a side effect (print) so it SPECIALIZES
         * on a const arg rather than folding away; helper(n) inside compute
         * INLINES (n is a non-const param); helper(1) FOLDS (const pure call). */
        const char *src[] = {
            "func helper(x) => x + 1;",
            "func compute(n) { var k = 5; print(helper(n) * k); "
            "return n; }",
            "var a = [1, 2, 3];",
            "var fo = helper(1);",
            "var sv = compute(10);",
        };
        const size_t nsrc = sizeof(src) / sizeof(src[0]);
        std::vector<Tok> toks;
        for (size_t i = 0; i < nsrc; i++)
            lexer(src[i], static_cast<int>(i + 1), toks);
        ParseContext pc(TokenStream(toks), true);
        unique_ptr<Construct> root = pBlock(pc);
        infer_types(root.get());
        resolve_names(root.get());
        specialize_types(root.get());
    } catch (...) {
        ok = false;
    }

    const std::string s = cap.str();
    const char *cats[] = { "infer", "template", "arrays", "autopure",
                           "autoconst", "inline", "fold", "specialize" };
    for (const char *c : cats)
        if (s.find(c) == std::string::npos)
            ok = false;

    trace_set_sink(saved_sink);
    g_trace_mask = saved_mask;
    return ok;
}

static bool replhelp_commands()
{
    /* the commands index + an explicit-colon command lookup */
    if (!help_has("commands", ":trace"))             return false;
    if (!help_has("commands", ":globals"))           return false;
    if (!help_has(":trace", "[REPL command]"))       return false;
    if (!help_has(":globals", "const context"))      return false;
    /* a no-colon name that is a command but not a builtin */
    if (!help_has("source", "[REPL command]"))       return false;
    /* a name that is BOTH a builtin and a command: the builtin entry, plus a
     * pointer to the command (the user's ":help trace" case) */
    if (!help_has("trace", "trace(category, on)"))   return false;
    if (!help_has("trace", "also a REPL command"))   return false;
    /* and the trace entries list the categories as a bullet list (one per
     * line) - the original complaint */
    if (!help_has("trace", "categories:"))           return false;
    if (!help_has("trace", "- infer "))              return false;
    if (!help_has("trace", "- autopure "))           return false;
    if (!help_has(":trace", "- all "))               return false;
    return true;
}

/* find a top-level function decl by name in a parsed/optimized root block */
static const FuncDeclStmt *find_top_func(Construct *root, const char *name)
{
    auto *blk = dynamic_cast<Block *>(root);
    if (!blk)
        return nullptr;
    for (auto &e : blk->elems)
        if (auto *fd = dynamic_cast<FuncDeclStmt *>(e.get()))
            if (fd->id && fd->id->get_str() == name)
                return fd;
    return nullptr;
}

/*
 * The :show / show() code renderer: f inlined + const-folded into g becomes
 * `print(3)`; inlined with non-const args becomes `print(x + y)` annotated
 * `inlined f`; an instantiated template surfaces its array element type.
 */
static bool coderender_inline_fold_types()
{
    const char *src[] = {
        "func f(x, y) => x + y;",
        "func g() { print(f(1, 2)); }",
        "func h(x, y) { print(f(x, y)); }",
        "func mk(x) { var a = [x, x]; return a; }",
        "var z = mk(5);",
    };
    std::vector<Tok> toks;
    for (size_t i = 0; i < 5; i++)
        lexer(src[i], static_cast<int>(i + 1), toks);

    unique_ptr<Construct> root;
    try {
        ParseContext pc(TokenStream(toks), true);
        root = pBlock(pc);
        infer_types(root.get());
        resolve_names(root.get());
        specialize_types(root.get());
    } catch (...) {
        return false;
    }

    const FuncDeclStmt *g = find_top_func(root.get(), "g");
    const FuncDeclStmt *h = find_top_func(root.get(), "h");
    if (!g || !h)
        return false;

    const std::string sg = render_func_code(g);
    const std::string sh = render_func_code(h);

    bool ok = true;
    ok = ok && sg.find("print(3)") != std::string::npos;     /* inlined+folded */
    ok = ok && sh.find("inlined f") != std::string::npos;    /* annotated */
    ok = ok && sh.find("x + y") != std::string::npos;        /* non-const args */

    /* the instantiated mk$0 (int) renders its flat array element type */
    const FuncDeclStmt *mk0 = find_top_func(root.get(), "mk$0");
    if (mk0)
        ok = ok && render_func_code(mk0).find("array<int>") !=
                       std::string::npos;

    /* an arbitrary expression renders too */
    if (Block *blk = dynamic_cast<Block *>(root.get())) {
        if (!blk->elems.empty())
            ok = ok && !render_construct_code(blk->elems[0].get()).empty();
    }
    return ok;
}

/* The :show highlighter extensions: a C-style block comment is colored, and a
 * synthetic `f$0` name stays one identifier (the '$' is an id char now). */
static bool coderender_highlight()
{
    set_highlight_enabled(true);
    const std::string code = "/* pure */ int func f$0(int x) => x + 1;";
    const std::string hl = highlight_line(code);
    set_highlight_enabled(true);   /* (the suite default) */

    bool ok = true;
    ok = ok && hl != code;                              /* colored */
    ok = ok && hl.find("\033[38;5;244m/*") != std::string::npos;  /* comment */
    ok = ok && hl.find("f$0") != std::string::npos;     /* contiguous id */
    return ok;
}

static bool replhelp_unknown_and_topics()
{
    if (!help_has("no_such_thing", "No help for"))   return false;
    if (!help_has("builtins boguscat", "Unknown builtin category"))
        return false;
    /* completion topics */
    const auto t = repl_help_topics("ar");
    bool has_array = false, has_array_storage = false;
    for (const auto &s : t) {
        if (s == "array")         has_array = true;
        if (s == "array_storage") has_array_storage = true;
    }
    return has_array && has_array_storage;
}

static const std::vector<extra_check> extra_checks =
{
    { "repl: multi-line completeness detection", repl_incomplete_detection },
    { "replhelp: overview + builtins index", replhelp_overview_and_builtins },
    { "replhelp: builtin entries + kind note", replhelp_builtin_entries },
    { "replhelp: language reference", replhelp_language_reference },
    { "replhelp: REPL commands + colon lookup", replhelp_commands },
    { "replhelp: unknown topic + completion", replhelp_unknown_and_topics },
    { "trace: module set/emit/active basics", trace_module_basics },
    { "trace: every category narrates a decision", trace_pipeline_categories },
    { "coderender: inlining, folding, and instance types",
      coderender_inline_fold_types },
    { "coderender: :show highlighting (block comment + $ id)",
      coderender_highlight },
    { "repl: completion (globals/builtins/keywords)",
      repl_completion_globals_builtins_keywords },
    { "repl: completion (struct members)", repl_completion_struct_members },
    { "lineedit: Tab completes a unique candidate",
      lineedit_tab_unique_completion },
    { "lineedit: Tab extends to the common prefix",
      lineedit_tab_common_prefix },
    { "highlight: inserts color escapes", highlight_inserts_color },
    { "highlight: stripping escapes restores the input",
      highlight_preserves_visible_text },
    { "highlight: disabled is identity", highlight_disabled_is_identity },
    { "highlight: tolerates an unterminated string",
      highlight_tolerates_unterminated_string },
    { "lineedit: typing and backspace", lineedit_typing_and_backspace },
    { "lineedit: cursor move and insert", lineedit_cursor_move_and_insert },
    { "lineedit: home/end and kill-to-end", lineedit_home_end_kill },
    { "lineedit: kill word (Ctrl-W)", lineedit_kill_word },
    { "lineedit: arrow keys move the cursor", lineedit_arrow_keys },
    { "lineedit: the Delete key", lineedit_delete_key },
    { "lineedit: Enter submits", lineedit_submit_action },
    { "lineedit: Ctrl-C cancels, Ctrl-D eof/delete", lineedit_ctrl_c_and_d },
    { "lineedit: Up/Down history navigation", lineedit_history_nav },
    { "lineedit: Enter inserts a newline until complete (multi-line)",
      lineedit_multiline_enter_inserts_newline },
    { "lineedit: 2-D cursor + within-block Up/Down",
      lineedit_multiline_cursor_and_nav },
    { "lineedit: line-relative Home/End/kill in a block",
      lineedit_multiline_home_end_kill },
    { "lineedit: ESC[1~/ESC[4~ Home/End are line-relative",
      lineedit_multiline_tilde_home_end },
    { "serialize() writes to the given stream", serialize_writes_to_given_stream },
    { "AST deep-clone round-trips", ast_clone_roundtrip },
    { "inliner splices an expr-func call", inliner_splices_call },
    { "inlined-call backtrace == non-inlined", inliner_backtrace_identical },
    { "inlined builtin-error backtrace == non-inlined",
      inliner_builtin_error_backtrace_identical },
    { "inline threshold (-it) gates inlining", inliner_threshold_gates },
    { "inliner re-folds a const subexpression", inliner_refolds_const_subexpr },
    { "inliner re-folds subscript/len in a splice",
      inliner_refolds_non_multiop },
    { "inliner folds a const-global subscript", inliner_folds_const_global },
    { "inliner specializes a block func (deduped)",
      inliner_specializes_block_func },
    { "inliner specializes on a const array/dict arg",
      inliner_specializes_array_const_arg },
    { "specialized-clone backtrace == non-spec",
      inliner_spec_backtrace_identical },
    { "inliner fixpoint collapses a forward-decl chain",
      inliner_fixpoint_collapses_chain },
    { "inliner fixpoint deep backtrace == non-inlined",
      inliner_fixpoint_deep_backtrace },
    { "inliner tail-inlines a block body (re-resolved locals)",
      inliner_tail_inlines_block_body },
    { "tail-inlined block backtrace == non-inlined",
      inliner_tail_block_backtrace },
    { "backtrace: basic format & alignment", backtrace_format_basic },
    { "backtrace: zero-padding for >9 frames", backtrace_zero_padding },
    { "backtrace: long-frame truncation", backtrace_truncation },
    { "backtrace: end-to-end call chain", backtrace_end_to_end },
    { "backtrace: inlined virtual frames", backtrace_inline_frames },
    { "stype: ground caching & with_opt", stype_ground_caching },
    { "stype: assignable rules", stype_assignable_rules },
    { "stype: join (LUB) rules", stype_join_rules },
    { "stype: unify & occurs-check", stype_unify_vars },
    { "stype: to_string", stype_to_string_basic },
};

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

    for (const auto &ec : extra_checks) {

        cout << "[ RUN  ] " << ec.name << endl;

        if (ec.fn()) {

            cout << "[ PASS ]";
            pass_count++;

        } else {

            cout << "[ FAIL ]";
        }

        cout << endl << endl;
    }

    for (const auto &rt : repl_tests) {

        cout << "[ RUN  ] repl: " << rt.name << endl;

        if (run_one_repl_test(rt)) {

            cout << "[ PASS ]";
            pass_count++;

        } else {

            cout << "[ FAIL ]";
        }

        cout << endl << endl;
    }

    const size_t total =
        tests.size() + extra_checks.size() + repl_tests.size();

    cout << "SUMMARY" << endl;
    cout << "===========================================" << endl;
    cout << "Tests passed: " << pass_count << "/" << total << " ";


    if (pass_count != total) {
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
