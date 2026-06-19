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

#include <typeinfo>
#include <vector>

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
        "Read a const dict's members at runtime: existing and missing",
        {
            "func ga(p) { return p.a; }",
            "func gz(p) { return p.zzz; }",
            "const d = {\"a\": 1};",
            "assert(ga(d) == 1);",
            "assert(gz(d) == none);",            /* missing: none, no vivify */
            "assert(len(d) == 1);",
        },
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
    { "hash() of an array is unsupported",
      { "hash([1,2]);" }, &typeid(TypeErrorEx) },
    { "hash() of none is unsupported",
      { "hash(none);" }, &typeid(TypeErrorEx) },

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
    { "ti reject: dict read passed to a non-opt param forces opt",
      { "func f(x) => x + 1; var d = {\"a\": 1}; f(d.a);" },
      &typeid(OptRequiredEx) },
    { "ti accept: opt param takes a dict read, narrowed in body",
      { "func f(opt x) { if (x == none) return 0; return x + 1; }",
        "var d = {\"a\": 5}; assert(f(d.a) == 6); assert(f(d.b) == 0);" } },
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
    ok = ok && s_on.find("$spec0") != std::string::npos;   /* specialized */
    ok = ok && s_on.find("$spec1") != std::string::npos;   /* both funcs */
    ok = ok && s_on.find("Int(60)") != std::string::npos;  /* a[..] folded */
    ok = ok && s_on.find("Int(300)") != std::string::npos; /* m[..] folded */
    /* off never folds these: no specialization without inlining. */
    ok = ok && s_off.find("$spec0") == std::string::npos;
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
    ok = ok && s.find("$spec0") != std::string::npos;    /* specialized */
    ok = ok && s.find("$spec1") == std::string::npos;    /* deduped to one */

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
    ok = ok && bt_on.find("$spec") == std::string::npos;    /* no synthetic */

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

static const std::vector<extra_check> extra_checks =
{
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

    const size_t total = tests.size() + extra_checks.size();

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
