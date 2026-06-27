/* SPDX-License-Identifier: BSD-2-Clause */

#include "replhelp.h"
#include "trace.h"

#include <sstream>
#include <string>
#include <vector>
#include <cstring>

using std::string;

/* ----------------------------- ANSI palette ----------------------------- */

namespace {

struct Pal {
    const char *hdr;     /* a section header */
    const char *sig;     /* a signature / syntax line */
    const char *kw;      /* an inline keyword / name */
    const char *dim;     /* a secondary note */
    const char *rst;
};

Pal palette(bool color)
{
    if (color)
        return Pal{ "\x1b[1;36m", "\x1b[33m", "\x1b[1m", "\x1b[2m",
                    "\x1b[0m" };
    return Pal{ "", "", "", "", "" };
}

/* ------------------------- builtin documentation ------------------------- */

struct BuiltinDoc {
    const char *name;
    const char *cat;       /* a builtin-category id (see builtin_cats) */
    const char *sig;       /* human-friendly signature */
    const char *summary;   /* one line */
    const char *longd;     /* extra detail, or nullptr */
};

struct CatInfo {
    const char *id;
    const char *title;
};

/* Builtin categories, in display order. */
const CatInfo builtin_cats[] = {
    { "convert",    "Type conversion & values" },
    { "reflect",    "Reflection & introspection" },
    { "array",      "Arrays" },
    { "dict",       "Dictionaries" },
    { "string",     "Strings" },
    { "math",       "Math functions" },
    { "floatclass", "Float classification" },
    { "constants",  "Numeric constants" },
    { "random",     "Random numbers" },
    { "io",         "Input / output" },
    { "control",    "Control & exceptions" },
};

const BuiltinDoc builtin_docs[] = {

/* --- convert --- */
{ "len", "convert", "len(x)",
  "Number of elements in an array/dict, or characters in a string.", nullptr },
{ "str", "convert", "str(x, [prec])",
  "Convert x to a string; for a float, prec decimal places.", nullptr },
{ "int", "convert", "int(x)",
  "Convert a bool/float/string to an int (a float truncates toward zero).",
  nullptr },
{ "float", "convert", "float(x)",
  "Convert a bool/int/string to a float.", nullptr },
{ "type", "convert", "type(x)",
  "The bare type name of x as a string (\"int\", \"array\", \"dict\", ...).",
  "Compare typeof(x), which gives the full structural type." },
{ "defined", "convert", "defined(x)",
  "True if x is a defined symbol (not an undefined identifier).", nullptr },
{ "clone", "convert", "clone(x)",
  "A shallow (one-level) copy; nested const objects stay shared/read-only.",
  nullptr },
{ "deepclone", "convert", "deepclone(x)",
  "A fully-mutable deep copy at every level - the way to mutate a const value.",
  nullptr },
{ "hash", "convert", "hash(x)",
  "The integer hash of a hashable value (as used for dict keys).", nullptr },

/* --- reflect --- */
{ "typeof", "reflect", "typeof(x)",
  "The rich structural type of x (\"array<int>\", \"dict<K,V>\", a struct name,"
  " a function signature).",
  "Value-driven, so it works in any script. For a symbol's INFERRED static "
  "type use the REPL :type." },
{ "globals", "reflect", "globals()",
  "Sorted names bound in the global scope (vars/funcs/structs), excluding "
  "builtins.",
  "A const scalar is folded away and absent here (:globals adds it); a script's"
  " top-level vars are slots, so a script lists only map-resident names." },
{ "signature", "reflect", "signature(f)",
  "A function's declared signature, or a struct type's constructor form.",
  nullptr },
{ "layout", "reflect", "layout(S)",
  "A struct's in-memory layout: POD field offsets + total size/align, or the "
  "boxed slot list.", nullptr },
{ "specializations", "reflect", "specializations(f)",
  "The <name>$N / <name>$sN template-instance & specialization clones derived "
  "from f.", nullptr },
{ "show", "reflect", "show(x)",
  "Render x's FINAL optimized AST back into synthetic MyLang code.",
  "A function value renders its whole declaration; any other argument is an "
  "EXPRESSION whose optimized tree is rendered (show(2+3*4) -> 14). Dead code "
  "is gone, folded consts are literals, inlined bodies are spliced in. In the "
  "REPL, :show <name> also renders the <name>$N clones with their inferred "
  "param/return types, syntax-highlighted." },
{ "array_storage", "reflect", "array_storage(a)",
  "The array's storage kind: \"ints\"/\"floats\"/\"bools\"/\"structs\"/"
  "\"general\".",
  "Decided once at creation from the proven static type; never converted "
  "afterward. Flat vs general differ only in speed/memory." },
{ "intptr", "reflect", "intptr(x)",
  "The internal shared-object pointer of x as an int (tests of aliasing).",
  nullptr },
{ "isconst", "reflect", "isconst(x)",
  "True if x is effectively const (literal, const, const-expr, or auto-const).",
  nullptr },
{ "isconstdecl", "reflect", "isconstdecl(x)",
  "True only if x is const by declaration (NOT merely via auto-const).",
  nullptr },
{ "ispure", "reflect", "ispure(f)",
  "True if f is effectively pure (declared pure OR proven pure).", nullptr },
{ "ispuredecl", "reflect", "ispuredecl(f)",
  "True only if f was explicitly declared pure.", nullptr },
{ "trace", "reflect", "trace(category, on)",
  "Enable/disable a diagnostic trace category that narrates the compiler.",
  "category is a single category name or \"all\". In the REPL use :trace; for "
  "a whole script use mylang --trace <cats> file (a script is fully compiled "
  "before it runs, so a runtime trace() call can't show its own "
  "compilation)." },
{ "traceoff", "reflect", "traceoff()",
  "Disable all diagnostic trace categories.", nullptr },
{ "tracing", "reflect", "tracing()",
  "The active trace categories as a sorted array<str>.", nullptr },

/* --- array --- */
{ "array", "array", "array(n, [fill])",
  "A new array of n elements (flat 0/0.0/false, or all = fill).",
  "Non-const, so array(N) is always a runtime call and a huge array isn't "
  "baked into the tree. Storage is type-driven." },
{ "make_array", "array", "make_array(n, gen)",
  "A new array of n elements where element i is gen(i).", nullptr },
{ "range", "array", "range(end) | range(start, end, [step])",
  "An array of ints from start (default 0) to end (exclusive), step (1).",
  nullptr },
{ "top", "array", "top(a)",
  "The last element of a (read-only).", nullptr },
{ "find", "array", "find(c, x, [cmp])",
  "Index of x in an array/string, the key in a dict, or -1 if absent.",
  nullptr },
{ "sort", "array", "sort(a, [cmp])",
  "Sort a in place ascending, or by cmp(x,y); a const array is copied.",
  "The custom-comparator path uses a hand-rolled heapsort that is safe for any "
  "comparator (even a non-ordering one)." },
{ "rev_sort", "array", "rev_sort(a, [cmp])",
  "Sort a in place descending (or by cmp).", nullptr },
{ "reverse", "array", "reverse(a)",
  "Reverse a in place.", nullptr },
{ "sum", "array", "sum(a, [reduce])",
  "Sum the elements of a, or fold them with reduce(acc, x).",
  "An all-int array sums in a tight unboxed loop; sum of an array<bool> counts "
  "the trues." },
{ "map", "array", "map(f, c)",
  "A new array applying f to each element of an array/dict.", nullptr },
{ "filter", "array", "filter(f, c)",
  "A new container of the elements of c for which f(x) is true.", nullptr },
{ "append", "array", "append(a, x)",
  "Append x to array a (mutates a).", nullptr },
{ "push", "array", "push(a, x)",
  "An alias for append() (symmetry with pop()).", nullptr },
{ "pop", "array", "pop(a)",
  "Remove and return the last element of a.", nullptr },
{ "insert", "array", "insert(c, i, x)",
  "Insert x at index/key i in array/dict c.", nullptr },
{ "erase", "array", "erase(c, i)",
  "Erase the element at index/key i from array/dict c.", nullptr },
{ "dynarray", "array", "dynarray(a)",
  "A fresh general (polymorphic, array<dyn>) copy of a - the manual promotion.",
  "There is no automatic runtime promotion; storage is fixed at creation. "
  "clone()/deepclone() preserve the layout." },

/* --- dict --- */
{ "dict", "dict", "dict(pairs) | dict(default)",
  "Build a dict from [k,v] pairs, or a default-dict whose missing keys yield "
  "default.",
  "A plain dict throws KeyNotFoundEx on a missing read; a default-dict returns "
  "its default instead." },
{ "keys", "dict", "keys(d)",
  "An array of the dict's keys.", nullptr },
{ "values", "dict", "values(d)",
  "An array of the dict's values.", nullptr },
{ "kvpairs", "dict", "kvpairs(d)",
  "An array of [key, value] pairs.", nullptr },
{ "get", "dict", "get(d, k)",
  "The value for k, or none if absent (the nullable lookup -> opt V).",
  nullptr },
{ "get!", "dict", "get!(d, k)",
  "The value for k, or throw KeyNotFoundEx if absent (fail-fast -> V).",
  nullptr },

/* --- string --- */
{ "split", "string", "split(s, delim)",
  "Split s by delim into an array (an empty delim splits into characters).",
  nullptr },
{ "join", "string", "join(a, delim)",
  "Join an array of strings with delim.", nullptr },
{ "splitlines", "string", "splitlines(s)",
  "Split s on line endings (\\n or \\r\\n).", nullptr },
{ "ord", "string", "ord(c)",
  "The ASCII code of a one-character string.", nullptr },
{ "chr", "string", "chr(n)",
  "The one-character string for ASCII code n (0..255).", nullptr },
{ "lpad", "string", "lpad(s, w, [c])",
  "Pad s on the left to width w with c (default space).", nullptr },
{ "rpad", "string", "rpad(s, w, [c])",
  "Pad s on the right to width w with c (default space).", nullptr },
{ "lstrip", "string", "lstrip(s)",
  "Remove leading whitespace.", nullptr },
{ "rstrip", "string", "rstrip(s)",
  "Remove trailing whitespace.", nullptr },
{ "strip", "string", "strip(s)",
  "Remove leading and trailing whitespace.", nullptr },
{ "startswith", "string", "startswith(s, p)",
  "True if s starts with prefix p.", nullptr },
{ "endswith", "string", "endswith(s, p)",
  "True if s ends with suffix p.", nullptr },

/* --- math --- */
{ "abs", "math", "abs(x)", "Absolute value (int or float).", nullptr },
{ "min", "math", "min(a) | min(x, y, ...)",
  "Minimum of an array or of the arguments.", nullptr },
{ "max", "math", "max(a) | max(x, y, ...)",
  "Maximum of an array or of the arguments.", nullptr },
{ "pow", "math", "pow(x, y)", "x raised to the power y.", nullptr },
{ "sqrt", "math", "sqrt(x)", "Square root.", nullptr },
{ "cbrt", "math", "cbrt(x)", "Cube root.", nullptr },
{ "exp", "math", "exp(x)", "e raised to x.", nullptr },
{ "exp2", "math", "exp2(x)", "2 raised to x.", nullptr },
{ "log", "math", "log(x)", "Natural logarithm.", nullptr },
{ "log2", "math", "log2(x)", "Base-2 logarithm.", nullptr },
{ "log10", "math", "log10(x)", "Base-10 logarithm.", nullptr },
{ "sin", "math", "sin(x)", "Sine (radians).", nullptr },
{ "cos", "math", "cos(x)", "Cosine (radians).", nullptr },
{ "tan", "math", "tan(x)", "Tangent (radians).", nullptr },
{ "asin", "math", "asin(x)", "Arcsine.", nullptr },
{ "acos", "math", "acos(x)", "Arccosine.", nullptr },
{ "atan", "math", "atan(x)", "Arctangent.", nullptr },
{ "ceil", "math", "ceil(x)", "Round up toward +infinity.", nullptr },
{ "floor", "math", "floor(x)", "Round down toward -infinity.", nullptr },
{ "trunc", "math", "trunc(x)", "Round toward zero.", nullptr },
{ "round", "math", "round(x, [places])",
  "Round to the nearest integer, or to `places` decimal places.", nullptr },

/* --- floatclass --- */
{ "isinf", "floatclass", "isinf(x)", "True if x is +/- infinity.", nullptr },
{ "isfinite", "floatclass", "isfinite(x)", "True if x is finite.", nullptr },
{ "isnormal", "floatclass", "isnormal(x)",
  "True if x is a normal float (not zero/subnormal/inf/nan).", nullptr },
{ "isnan", "floatclass", "isnan(x)", "True if x is NaN.", nullptr },

/* --- constants --- */
{ "math_pi", "constants", "math_pi", "Pi.", nullptr },
{ "math_e", "constants", "math_e", "Euler's number e.", nullptr },
{ "math_pi2", "constants", "math_pi2", "Pi / 2.", nullptr },
{ "math_pi4", "constants", "math_pi4", "Pi / 4.", nullptr },
{ "math_1_pi", "constants", "math_1_pi", "1 / Pi.", nullptr },
{ "math_2_pi", "constants", "math_2_pi", "2 / Pi.", nullptr },
{ "math_2_sqrt_pi", "constants", "math_2_sqrt_pi", "2 / sqrt(Pi).", nullptr },
{ "math_sqrt2", "constants", "math_sqrt2", "sqrt(2).", nullptr },
{ "math_1_sqrt2", "constants", "math_1_sqrt2", "1 / sqrt(2).", nullptr },
{ "math_ln2", "constants", "math_ln2", "ln(2).", nullptr },
{ "math_ln10", "constants", "math_ln10", "ln(10).", nullptr },
{ "math_log2e", "constants", "math_log2e", "log2(e).", nullptr },
{ "math_log10e", "constants", "math_log10e", "log10(e).", nullptr },
{ "nan", "constants", "nan", "Not-a-Number.", nullptr },
{ "inf", "constants", "inf", "Positive infinity.", nullptr },
{ "eps", "constants", "eps", "Machine epsilon for float.", nullptr },

/* --- random --- */
{ "rand", "random", "rand(lo, hi)",
  "A random int in [lo, hi] inclusive (none if lo > hi).", nullptr },
{ "randf", "random", "randf(lo, hi)",
  "A random float in [lo, hi] inclusive (none if lo > hi).", nullptr },

/* --- io --- */
{ "print", "io", "print(...)",
  "Print the arguments space-separated, then a newline.", nullptr },
{ "readln", "io", "readln()",
  "Read one line from stdin (without the trailing newline).", nullptr },
{ "writeln", "io", "writeln(s, [file])",
  "Write s + a newline to stdout, or to file.", nullptr },
{ "write", "io", "write(s, [file])",
  "Write s with no newline.", nullptr },
{ "read", "io", "read([file])",
  "Read all of stdin, or all of file, as one string.", nullptr },
{ "readlines", "io", "readlines([file])",
  "Read all lines as an array of strings.", nullptr },
{ "writelines", "io", "writelines(a, [file])",
  "Write each string of a on its own line.", nullptr },
{ "remove", "io", "remove(file)",
  "Delete file; 1 if removed, 0 if it did not exist.", nullptr },
{ "tmpdir", "io", "tmpdir()",
  "The OS temporary-directory path (no trailing separator).", nullptr },

/* --- control --- */
{ "assert", "control", "assert(x)",
  "Throw AssertionFailureEx if x is false.", nullptr },
{ "exit", "control", "exit(code)",
  "Exit the process with the integer code.", nullptr },
{ "runtime", "control", "runtime(x)",
  "An optimization barrier: returns x, but blocks const-folding of the "
  "enclosing expression.",
  "The argument is still folded, so runtime(1/0) fails at compile time, while "
  "1/runtime(0) throws at runtime." },

};

const BuiltinDoc *find_builtin(const string &name)
{
    for (const BuiltinDoc &d : builtin_docs)
        if (name == d.name)
            return &d;
    return nullptr;
}

const CatInfo *find_builtin_cat(const string &id)
{
    for (const CatInfo &c : builtin_cats)
        if (id == c.id)
            return &c;
    return nullptr;
}

/* ------------------------ language documentation ------------------------- */

struct LangFeature {
    const char *id;
    const char *cat;       /* a language-category id */
    const char *title;     /* display title */
    const char *syntax;    /* a syntax sketch (may contain newlines) */
    const char *body;      /* the explanation */
};

/* Language categories, in display order. */
const CatInfo lang_cats[] = {
    { "basics",        "Values & types" },
    { "variables",     "Variables & constants" },
    { "operators",     "Operators" },
    { "control",       "Control flow" },
    { "functions",     "Functions & closures" },
    { "arrays",        "Arrays & slices" },
    { "dicts",         "Dictionaries" },
    { "strings",       "Strings" },
    { "structs",       "Structs" },
    { "exceptions",    "Exceptions" },
    { "types",         "Type system & inference" },
    { "optimizations", "Optimizations" },
};

const LangFeature lang_features[] = {

/* ---- basics ---- */
{ "values", "basics", "Values & literals",
  "1    3.14    true    \"text\"    none\n[1, 2, 3]    {\"k\": 1}",
  "MyLang has int (64-bit, wraps on overflow), float (64-bit IEEE double), "
  "bool, str, none, arrays, and dicts, plus user structs. Integer division "
  "truncates toward zero; floats match Python's." },
{ "nonenull", "basics", "none / null",
  "var opt x;    x = none;    x = null;",
  "`none` is the absent/unit value; `null` is an exact alias for it. A value "
  "can be `none` only where its type is nullable (see opt)." },
{ "booltype", "basics", "Booleans",
  "true    false",
  "A real scalar type. The numeric promotion chain is bool <= int <= float, so "
  "true + true is the int 2, while comparisons / logical ops / ! yield bool." },

/* ---- variables ---- */
{ "vardecl", "variables", "var",
  "var x = 5;\nvar a, b = [1, 2];    var a, b = 0;",
  "Declare a re-bindable variable. An id-list target with an array rvalue "
  "spreads element-wise; with a scalar rvalue it assigns the same value to "
  "each. A var only makes the NAME rebindable - a value derived from a const "
  "stays read-only." },
{ "constdecl", "variables", "const",
  "const K = 10;    const A = [1, 2, 3];",
  "A compile-time constant, evaluated at parse time (like C++ constexpr). A "
  "const SCALAR is inlined at every use and its declaration dropped; a const "
  "array/dict/func is kept as one runtime symbol but reads of it still fold. A "
  "const is DEEP read-only (immutable through any alias, e.g. a function "
  "parameter)." },
{ "annotations", "variables", "Type annotations",
  "int x = 5;    float f = 3;    str s;    array a = [1, 2];",
  "A primitive type keyword (bool/int/float/str/array/dict) replaces `var` to "
  "PIN a scalar's type: a wrong-typed value or reassignment is a compile "
  "error, while float f = 3 widens. An uninitialized typed decl gets the "
  "type's zero value (0/0.0/false/\"\"/[]/{}), or none when opt. array/dict "
  "annotations check only the kind (the element type stays inferred)." },
{ "optmod", "variables", "opt / nullable ?",
  "opt int x;    int? y;    var opt z;    func f(x?)",
  "`opt` makes a declaration nullable (may hold none). `T? ` is the short "
  "form (int? == opt int); on a parameter, a trailing `?` on the NAME means "
  "opt. A non-opt value is statically guaranteed never none, so the body uses "
  "it without a check (mandatory-opt - see :help mandatoryopt)." },
{ "dynmod", "variables", "dyn",
  "var dyn d = ...;    dyn z;    func f(dyn a)    func f(~a)",
  "`dyn` is a dynamically-typed (variant) declaration: it holds any type and "
  "its type operations are checked at runtime, not by the inferencer. It is "
  "orthogonal to nullability - a bare dyn is non-null, `opt dyn` is nullable. "
  "On a parameter, a leading `~` is the short form of dyn." },

/* ---- operators ---- */
{ "arithmetic", "operators", "Arithmetic",
  "+  -  *  /  %",
  "Over int/float (bool promotes to int). `/` truncates toward zero for ints; "
  "`+` also concatenates strings/arrays. Mixed int/float promotes to float." },
{ "comparison", "operators", "Comparison",
  "<  >  <=  >=  ==  !=",
  "All yield a bool. ==/!= are defined for any pair of values; ordering "
  "(< <= > >=) is numeric or string. 1 and 1.0 compare equal (and hash the "
  "same, so they are one dict key)." },
{ "logical", "operators", "Logical",
  "&&    ||    !",
  "Short-circuit && and ||, and unary !. The result is a bool." },
{ "assignment", "operators", "Assignment",
  "=   +=   -=   *=   /=   %=",
  "Plain and compound assignment. The left side may be a variable, an "
  "array/dict element, a struct field, or an id-list (multiple assignment)." },

/* ---- control ---- */
{ "ifelse", "control", "if / else",
  "if (cond) { ... } else if (c2) { ... } else { ... }",
  "Standard conditional. A const condition is folded at compile time - the "
  "taken branch replaces the whole if and the dead branch is removed." },
{ "whileloop", "control", "while",
  "while (cond) { ... }",
  "A const-false condition drops the loop entirely; break/continue work "
  "inside." },
{ "forloop", "control", "for",
  "for (var i = 0; i < n; i += 1) { ... }",
  "The classic C-style loop. `continue` still runs the increment. (There is no "
  "++ operator; write i += 1.)" },
{ "foreachloop", "control", "foreach",
  "foreach (var e in a) { ... }\nforeach (var k, v in d) { ... }\n"
  "foreach (var i, e in indexed a) { ... }",
  "Iterate an array/dict/string. Tuple-unpacking binds dict key+value (or a "
  "[k,v] pair); the `indexed` keyword also yields the position. The loop var's "
  "type is derived from the container." },
{ "breakcont", "control", "break / continue / return",
  "break;    continue;    return expr;",
  "break/continue only inside a loop, return only inside a function body - "
  "each is a compile error elsewhere. They are signalled internally (not via "
  "C++ exceptions), so they are cheap." },

/* ---- functions ---- */
{ "funcdecl", "functions", "func & lambdas",
  "func f(a, b) { return a + b; }\nfunc g(x) => x + 1;\n"
  "var h = func(x) => x * x;",
  "Named functions, an expression body via `=>`, and anonymous lambdas. "
  "Parameters are passed by value. A function sees only its captures (and, for "
  "a pure func, only consts + params) - never the caller's locals or globals "
  "unless captured." },
{ "params", "functions", "Parameters",
  "func f(const x, opt y, dyn z, int n)",
  "A parameter may be `const` (reassigning it is an error), `opt` (nullable, "
  "and a trailing opt param is skippable at the call site -> none), `dyn` "
  "(variant), or carry a type annotation. An un-annotated plain parameter "
  "makes the function a template (see :help templates)." },
{ "namedargs", "functions", "Named arguments",
  "func f(x, y, z) ...    f(x: 1, z: 3)",
  "Arguments may be passed by name after a leading run of positional ones, in "
  "declaration order; a skipped interior optional is filled with none. A named "
  "call desugars to the positional form and is optimized IDENTICALLY - the "
  "syntax never costs a fold/inline/specialization." },
{ "closures", "functions", "Closures & captures",
  "func make() { var n = 0; return func[n]() => n; }",
  "A function can capture outer variables via a capture list. A non-capturing "
  "function clones to itself (shared); a capturing one is deep-copied per "
  "clone, so each closure has independent captured state." },
{ "purefunc", "functions", "pure func",
  "pure func sq(x) => x * x;    const C = sq(5);",
  "A `pure func` can be invoked during const-evaluation: it may not capture "
  "and sees only consts + its own params. It is the escape hatch that lets a "
  "call run at parse time (sort(a, pure func(x,y) => x<y) folds). See also "
  "auto-pure (:help autopure)." },

/* ---- arrays ---- */
{ "arraylit", "arrays", "Array literals & access",
  "var a = [1, 2, 3];    a[0]    a[1:3]    len(a)    a += [4];",
  "Arrays are 0-indexed, growable, and have value semantics (copy-on-write). "
  "Indexing reads/writes an element; slicing yields an independent (lazily "
  "copied) sub-array." },
{ "slices", "arrays", "Slices (copy-on-write)",
  "var s = a[1:3];",
  "A slice is a cheap VIEW until written: reading it shares the parent's "
  "storage, and a write clones first so logically-distinct arrays never bleed "
  "into each other. intptr(x) reveals when two values share storage." },
{ "flatstorage", "arrays", "Flat (unboxed) arrays",
  "array_storage(a)    dynarray(a)",
  "A homogeneous array<int>/<float>/<bool> (and array<PodStruct>) is stored "
  "UNBOXED - far less memory and much faster bulk ops. The representation is "
  "decided once from the proven static type and never converted; declare `dyn` "
  "or use dynarray() for a polymorphic array. See :help flatarrays / "
  ":trace arrays." },

/* ---- dicts ---- */
{ "dictlit", "dicts", "Dictionaries",
  "var d = {\"k\": 1};    d[\"k\"]    d.k    get(d, k)    get!(d, k)\n"
  "var dd = dict(0);   // a default dict",
  "Keys may be any hashable scalar. A missing-key READ throws KeyNotFoundEx "
  "(the read is non-opt) unless the dict has a default (dict(default_value)); "
  "a plain-assignment to a missing key inserts it. get(d,k) is the nullable "
  "lookup (opt V), get!(d,k) is value-or-throw." },

/* ---- strings ---- */
{ "strtype", "strings", "Strings",
  "var s = \"text\";    s[0]    s[1:3]    s + t    len(s)",
  "Strings are immutable and reference-counted; a slice is a cheap view (never "
  "mutated in place). Characters are 8-bit - there is no Unicode (deliberate). "
  "`+` concatenates; str(x) converts any value." },

/* ---- structs ---- */
{ "structdecl", "structs", "struct",
  "struct Point { int x; int y; const ORIGIN = 0; }\n"
  "var p = Point(3, 4);    p.x    Point.ORIGIN",
  "A user value type with explicitly-typed fields and optional const members. "
  "Construction is a call (Point(...)); fields are accessed with `.`. Structs "
  "have COW value semantics. An all-scalar struct is POD (a native C byte "
  "layout - see layout()); a ref/opt field makes it boxed. A self-reference "
  "must be `opt` (dyn? next) to break the cycle - that is how you write a "
  "list/tree." },

/* ---- exceptions ---- */
{ "trycatch", "exceptions", "Exceptions",
  "struct MyError { str msg; }\ntry { throw MyError(\"boom\"); }\n"
  "catch (MyError as e) { print(e.msg); }\nfinally { ... }    rethrow;",
  "try/catch/finally with catch clauses matched by exception NAME. A user "
  "exception is a struct instance: `throw MyError(...)` is caught by "
  "`catch (MyError)` (the struct type), and `catch (MyError as e)` binds the "
  "instance so `e.field` reads its data. Built-in runtime errors "
  "(DivisionByZeroEx, TypeErrorEx, OutOfBoundsEx, KeyNotFoundEx, ...) are "
  "catchable too. COMPILE-time errors (type/syntax) are NOT catchable. "
  "`rethrow` re-throws the in-flight exception from a catch." },

/* ---- types ---- */
{ "inference", "types", "Static type inference",
  "(automatic; disable with -nti)",
  "A whole-program, compile-time pass gives every variable, parameter and "
  "function return a fixed static type and REJECTS type violations before the "
  "program runs (a static type error is a compile error, not a runtime one). "
  "It runs a fixpoint, then checks every operator/call/assignment/return. "
  ":trace infer narrates it; the REPL :type <expr> shows an inferred type." },
{ "mandatorydyn", "types", "Mandatory dyn",
  "var dyn d = runtime(5);    // a plain `var` here is an error",
  "A plain var/const must infer a CONCRETE type; if its type would be dyn, the "
  "compiler demands you write it explicitly (var dyn). This keeps `dyn` "
  "visible at the declaration. Parameters and foreach loop vars are exempt." },
{ "mandatoryopt", "types", "Mandatory opt & null narrowing",
  "func f(opt x) { if (x == none) return; use(x); }",
  "A parameter that can receive none from some call must be declared `opt`, "
  "else it is a compile error at the declaration - so a non-opt parameter is "
  "PROVEN never none and the body needs no check. Inside a proven branch "
  "(if (x != none) ..., a guard clause) a nullable reads as non-opt." },
{ "templates", "types", "Function templates",
  "func f(x) { var t = x + 1; return t; }    f(1); f(\"s\");",
  "A named function with an un-annotated, non-dyn, non-opt parameter is a "
  "TEMPLATE: it is not checked in isolation but instantiated per call-site "
  "signature as a typed clone (f$0, f$1, ...). So f(1) and f(\"s\") make two "
  "instances, not a type conflict. `dyn` is the explicit one-instance-any-type "
  "parameter. See :trace template / specializations()." },

/* ---- optimizations ---- */
{ "constfold", "optimizations", "Const-evaluation",
  "const K = 2 + 3 * 4;    // baked to 14 at parse time",
  "Constants are computed at PARSE time (constexpr-like), inside the parser. A "
  "pure func with const args runs then too. A const error fails the build (it "
  "is not deferred to runtime). :trace fold shows folds; :tree / :analyze show "
  "the result." },
{ "autoconst", "optimizations", "Auto-const",
  "var n = 10;    // write-once scalar -> a compile-time constant",
  "A post-parse pass does for plain `var`s what const does: a write-once "
  "variable with a constant scalar initializer is promoted to a compile-time "
  "constant, its declaration dropped and every use folded - cascading in "
  "declaration order. :trace autoconst narrates it." },
{ "autopure", "optimizations", "Auto-pure",
  "func area(r) => math_pi * r * r;    // proven pure",
  "A capture-free function that reads only consts/params (and nests no "
  "function) is promoted to effectively-pure, so its const-argument calls fold "
  "even though it was not written `pure`. ispure() reports it; :trace autopure "
  "narrates it." },
{ "inlining", "optimizations", "Inlining",
  "func inc(x) => x + 1;    inc(y)  ->  y + 1    (-it N, -ni to disable)",
  "A small expression-bodied or tail call is spliced in place: it removes call "
  "overhead and exposes the body to the caller's const-folder. The body must "
  "be under a node threshold (-it, default 24). Backtraces are preserved via "
  "inlined-at frames. :trace inline shows each decision." },
{ "specialization", "optimizations", "Specialization",
  "func g(n) { ...big... }    g(3)  ->  g$s0  (n bound to 3, folded)",
  "A const argument is propagated into a (possibly non-pure) function and "
  "folded; if that shrinks the body enough, a shared clone <name>$sN is made "
  "and the call redirected (a scalar OR a deep read-only array/dict arg can "
  "seed one). The half whole-call folding misses. :trace specialize." },
{ "templinst", "optimizations", "Template instantiation",
  "f(1); f(2.5)  ->  f$0 (int), f$1 (float)",
  "A template (see :help templates) is cloned per distinct call-site signature "
  "as a concrete, typed instance <name>$N; same-signature calls share one "
  "clone. Past 64 instances a template's further calls run dynamically. "
  ":trace template / specializations() list them." },
{ "m8scalar", "optimizations", "Typed scalar specialization (M8)",
  "(automatic for proven int/float scalar expressions)",
  "After inference proves a scalar expression is a non-null int/float, it is "
  "rewritten to an unboxed TypedScalarExpr evaluated with no promotion "
  "dispatch and no intermediate boxing - ~2-3x on numeric loops. :analyze "
  "marks specialized call sites." },
{ "flatarrays", "optimizations", "Flat arrays",
  "array_storage(a) == \"ints\"",
  "A homogeneous int/float/bool array (and array<PodStruct>) is stored unboxed "
  "(8 bytes per int/float, ONE byte per bool), decided from the proven static "
  "type and never converted. See :help flatstorage / :trace arrays." },
{ "cse", "optimizations", "Const de-duplication (CSE)",
  "const A = [1,2,3]; const B = [1,2,3];   // share one baked value",
  "Two identical const expressions share one baked read-only value instead of "
  "duplicating it - a compile-time + memory win (folding already made each use "
  "a literal). intptr() shows the shared identity." },
{ "cow", "optimizations", "Copy-on-write",
  "var b = a;    // aliases until one is written",
  "Arrays, dicts, strings and structs are reference-counted with value "
  "semantics preserved via copy-on-write: assignment/slicing share storage "
  "until a mutation, which clones first. Read-only (const) values are never "
  "cloned. clone() is shallow, deepclone() deep." },

};

bool lang_db_empty() { return false; }

const LangFeature *find_feature(const string &id)
{
    for (const LangFeature &f : lang_features)
        if (id == f.id)
            return &f;
    return nullptr;
}

const CatInfo *find_lang_cat(const string &id)
{
    for (const CatInfo &c : lang_cats)
        if (id == c.id)
            return &c;
    return nullptr;
}

/* ------------------------ REPL command documentation --------------------- */

struct CommandDoc {
    const char *name;      /* the command word, WITHOUT the leading ':' */
    const char *syntax;
    const char *summary;
    const char *longd;     /* or nullptr */
};

const CommandDoc command_docs[] = {
    { "help", ":help [topic]",
      "Open this reference.",
      "topic is a builtin, a language category/feature, or a command. "
      ":help builtins and :help language list those; :help commands lists "
      "the commands. A leading ':' is optional (:help :trace = :help trace)." },
    { "trace", ":trace [<cat>...] on|off",
      "Toggle the diagnostic tracer that narrates the compiler's reasoning.",
      "With no argument it shows the active categories; ':trace off' disables "
      "all. The trace prints just above the result as the next input compiles. "
      "(Script equivalent: mylang --trace <cats> file; or the "
      "trace()/traceoff()/tracing() builtins.)" },
    { "globals", ":globals",
      "A table of every global (vars, consts, funcs, structs) with its type.",
      "Merges the runtime scope with the const context, so folded const "
      "SCALARS appear too. See also the globals() builtin." },
    { "type", ":type <expr>",
      "Show a type without committing anything.",
      "For a bare committed global, the inferencer's inferred/declared static "
      "type; for any other expression, its runtime structural type (evaluated "
      "in a throwaway scope). See also the typeof() builtin." },
    { "show", ":show <function-or-expression>",
      "Render a function (or expression) optimized AST back into MyLang code.",
      "Folded consts as literals, inlined bodies spliced in, dead code gone. A "
      "function's <name>$N clones are rendered too, each with its inferred "
      "parameter and return types (e.g. `int func dot$0(int x, int y)`), "
      "syntax-highlighted. An expression argument shows how it optimizes "
      "(:show 2 + 3 * 4 -> 14). See the show() builtin." },
    { "tree", ":tree <code>",
      "Print the const-folded syntax tree of <code> (non-committing).",
      "Shows what survived parse-time folding, e.g. :tree 2 + 3 * 4 -> "
      "Int(14)." },
    { "analyze", ":analyze <code>",
      "Reprint <code> colored by which optimizations fired (non-committing).",
      "The -a/--analyze view: auto-const, flat vs dyn arrays, inlined, "
      "specialized, folded, dead code." },
    { "source", ":source <file>",
      "Evaluate a file as if it were typed at the prompt, one unit at a time.",
      nullptr },
    { "undef", ":undef <name>",
      "Remove a global symbol so it can be redeclared (even with a new type).",
      "A REPL-only convenience: a script's symbols are fixed slots at compile "
      "time, so there is no `undef` builtin - a script re-defines a name." },
    { "quit", ":quit",
      "Exit the REPL (also Ctrl-D at the prompt).", nullptr },
};

const CommandDoc *find_command(const string &name)
{
    for (const CommandDoc &c : command_docs)
        if (name == c.name)
            return &c;
    return nullptr;
}

/* ------------------------------ rendering -------------------------------- */

/* Wrap a comma-separated name list to ~72 columns under a 2-space indent. */
void wrap_names(std::ostream &o, const std::vector<string> &names)
{
    size_t col = 0;
    o << "  ";
    col = 2;
    for (size_t i = 0; i < names.size(); i++) {
        const string &n = names[i];
        const size_t add = n.size() + (i + 1 < names.size() ? 2 : 0);
        if (col + add > 74 && col > 2) {
            o << "\n  ";
            col = 2;
        }
        o << n;
        if (i + 1 < names.size())
            o << ", ";
        col += add;
    }
    o << "\n";
}

const char *builtin_kind_note(const BuiltinDoc &d)
{
    /* the const-foldable categories vs the runtime-only ones */
    if (!std::strcmp(d.cat, "reflect") || !std::strcmp(d.cat, "random") ||
        !std::strcmp(d.cat, "io") || !std::strcmp(d.cat, "control") ||
        !std::strcmp(d.name, "array") || !std::strcmp(d.name, "append") ||
        !std::strcmp(d.name, "push") || !std::strcmp(d.name, "pop") ||
        !std::strcmp(d.name, "insert") || !std::strcmp(d.name, "erase") ||
        !std::strcmp(d.name, "dynarray") || !std::strcmp(d.name, "deepclone"))
        return "runtime (never const-folded)";
    return "const (folds with const arguments)";
}

void render_builtin_entry(std::ostream &o, const BuiltinDoc &d, bool color)
{
    const Pal p = palette(color);
    o << p.sig << d.sig << p.rst << "\n";
    o << "    " << d.summary << "\n";
    if (d.longd)
        o << "    " << d.longd << "\n";
    if (!std::strcmp(d.name, "trace")) {       /* the category bullet list */
        o << "    categories:\n";
        o << trace_categories_help("      ");
    }
    const CatInfo *c = find_builtin_cat(d.cat);
    o << p.dim << "    [" << (c ? c->title : d.cat) << "; "
      << builtin_kind_note(d) << "]" << p.rst << "\n";
}

void render_builtins_index(std::ostream &o, bool color)
{
    const Pal p = palette(color);
    o << "Builtins by category "
      << p.dim << "(:help <name> for one, :help builtins <cat> for a group)"
      << p.rst << "\n";
    for (const CatInfo &c : builtin_cats) {
        std::vector<string> names;
        for (const BuiltinDoc &d : builtin_docs)
            if (!std::strcmp(d.cat, c.id))
                names.push_back(d.name);
        o << "\n" << p.hdr << c.title << p.rst
          << p.dim << "  [" << c.id << "]" << p.rst << "\n";
        wrap_names(o, names);
    }
}

void render_builtins_category(std::ostream &o, const CatInfo &c, bool color)
{
    const Pal p = palette(color);
    o << p.hdr << c.title << p.rst << "\n\n";
    for (const BuiltinDoc &d : builtin_docs)
        if (!std::strcmp(d.cat, c.id)) {
            o << p.sig << "  " << d.sig << p.rst << "\n";
            o << "      " << d.summary << "\n";
        }
}

void render_language_index(std::ostream &o, bool color)
{
    const Pal p = palette(color);
    if (lang_db_empty()) {
        o << "Language reference is being assembled "
          << p.dim << "(see plans/repl-introspection.md)" << p.rst << "\n";
        return;
    }
    o << "Language features by category "
      << p.dim << "(:help <category> to expand, :help <feature> for one)"
      << p.rst << "\n";
    for (const CatInfo &c : lang_cats)
        o << "\n" << p.hdr << c.title << p.rst
          << p.dim << "  [" << c.id << "]" << p.rst << "\n";
}

void render_lang_category(std::ostream &o, const CatInfo &c, bool color)
{
    const Pal p = palette(color);
    o << p.hdr << c.title << p.rst << "\n\n";
    for (const LangFeature &f : lang_features)
        if (!std::strcmp(f.cat, c.id))
            o << p.kw << "  " << f.title << p.rst
              << p.dim << "  [" << f.id << "]" << p.rst << "\n";
}

void render_feature(std::ostream &o, const LangFeature &f, bool color)
{
    const Pal p = palette(color);
    o << p.hdr << f.title << p.rst << "\n\n";
    if (f.syntax && *f.syntax) {
        o << p.sig << f.syntax << p.rst << "\n\n";
    }
    o << f.body << "\n";
}

void render_command_entry(std::ostream &o, const CommandDoc &c, bool color)
{
    const Pal p = palette(color);
    o << p.sig << c.syntax << p.rst << "\n";
    o << "    " << c.summary << "\n";
    if (c.longd)
        o << "    " << c.longd << "\n";
    if (!std::strcmp(c.name, "trace")) {       /* the category bullet list */
        o << "    categories:\n";
        o << trace_categories_help("      ");
    }
    o << p.dim << "    [REPL command]" << p.rst << "\n";
}

void render_commands_index(std::ostream &o, bool color)
{
    const Pal p = palette(color);
    o << p.hdr << "REPL commands" << p.rst
      << p.dim << "  (:help <command> for one; the leading ':' is optional)"
      << p.rst << "\n\n";
    for (const CommandDoc &c : command_docs) {
        o << p.sig << "  " << c.syntax << p.rst << "\n";
        o << "      " << c.summary << "\n";
    }
}

void render_overview(std::ostream &o, bool color)
{
    const Pal p = palette(color);
    o << p.hdr << "MyLang REPL help" << p.rst << "\n";
    o << "Documentation:\n";
    o << "  " << p.kw << ":help builtins" << p.rst
      << "          all builtins, grouped by category\n";
    o << "  " << p.kw << ":help builtins <cat>" << p.rst
      << "    one builtin category\n";
    o << "  " << p.kw << ":help <builtin>" << p.rst
      << "         one builtin (signature + description)\n";
    o << "  " << p.kw << ":help language" << p.rst
      << "          language features, grouped by category\n";
    o << "  " << p.kw << ":help <category>" << p.rst
      << "        one language category\n";
    o << "  " << p.kw << ":help <feature>" << p.rst
      << "         one language feature (incl. optimizations)\n";
    o << "  " << p.kw << ":help commands" << p.rst
      << "          the REPL commands (:help <command> for one)\n";
    o << "Inspection:\n";
    o << "  " << p.kw << ":globals" << p.rst
      << "  :type <expr>  :show <func>  :tree <code>  :analyze <code>\n";
    o << "  " << p.kw << ":trace <cat> on|off" << p.rst
      << "     narrate the compiler's reasoning (:trace help)\n";
    o << "Session: " << p.kw << ":source <file>" << p.rst
      << "  :quit\n";
}

}  /* namespace */

string
repl_help(const string &topic, bool color)
{
    /* trim */
    string t = topic;
    {
        const size_t a = t.find_first_not_of(" \t\r\n");
        const size_t b = t.find_last_not_of(" \t\r\n");
        t = (a == string::npos) ? string() : t.substr(a, b - a + 1);
    }

    std::ostringstream o;

    if (t.empty()) {
        render_overview(o, color);
        return o.str();
    }

    /* A leading ':' means the user is naming a REPL command explicitly
     * (:help :trace == :help trace); strip it and remember. */
    bool had_colon = false;
    if (t[0] == ':') {
        had_colon = true;
        t = t.substr(1);
        const size_t a = t.find_first_not_of(" \t");
        t = (a == string::npos) ? string() : t.substr(a);
        if (t.empty()) {                 /* ":help :" -> the commands index */
            render_commands_index(o, color);
            return o.str();
        }
    }

    /* first word + remainder */
    string w0 = t, rest;
    {
        const size_t sp = t.find_first_of(" \t");
        if (sp != string::npos) {
            w0 = t.substr(0, sp);
            const size_t r = t.find_first_not_of(" \t", sp);
            if (r != string::npos)
                rest = t.substr(r);
        }
    }

    if (w0 == "commands") {
        render_commands_index(o, color);
        return o.str();
    }

    if (w0 == "builtins") {
        if (rest.empty()) {
            render_builtins_index(o, color);
        } else if (const CatInfo *c = find_builtin_cat(rest)) {
            render_builtins_category(o, *c, color);
        } else {
            o << "Unknown builtin category '" << rest
              << "'. Try :help builtins\n";
        }
        return o.str();
    }

    if (w0 == "language") {
        render_language_index(o, color);
        return o.str();
    }

    /*
     * A single topic. With an explicit ':' the user means the COMMAND, so try
     * that first. Otherwise resolve builtin -> command -> language category ->
     * language feature. When a builtin and a same-named command coexist (trace,
     * type, globals), show the builtin and point at the command for discovery.
     */
    const CommandDoc *cmd = find_command(t);
    const BuiltinDoc *bi = find_builtin(t);
    const Pal p = palette(color);

    if (had_colon && cmd) {
        render_command_entry(o, *cmd, color);
        return o.str();
    }
    if (bi) {
        render_builtin_entry(o, *bi, color);
        if (cmd)
            o << p.dim << "    (also a REPL command - :help :" << t << ")"
              << p.rst << "\n";
        return o.str();
    }
    if (cmd) {
        render_command_entry(o, *cmd, color);
        return o.str();
    }
    if (const CatInfo *c = find_lang_cat(t)) {
        render_lang_category(o, *c, color);
        return o.str();
    }
    if (const LangFeature *f = find_feature(t)) {
        render_feature(o, *f, color);
        return o.str();
    }

    o << "No help for '" << (had_colon ? ":" : "") << t
      << "'. Try :help, :help builtins, :help language, or :help commands\n";
    return o.str();
}

std::vector<string>
repl_help_topics(const string &prefix)
{
    auto pre = [&](const char *s) {
        return std::strncmp(s, prefix.c_str(), prefix.size()) == 0;
    };

    std::vector<string> out;
    if (pre("builtins")) out.push_back("builtins");
    if (pre("language")) out.push_back("language");
    if (pre("commands")) out.push_back("commands");
    for (const CatInfo &c : builtin_cats)
        if (pre(c.id))
            out.push_back(c.id);
    for (const BuiltinDoc &d : builtin_docs)
        if (pre(d.name))
            out.push_back(d.name);
    for (const CommandDoc &c : command_docs)
        if (pre(c.name))
            out.push_back(c.name);
    if (!lang_db_empty()) {
        for (const CatInfo &c : lang_cats)
            if (pre(c.id))
                out.push_back(c.id);
        for (const LangFeature &f : lang_features)
            if (pre(f.id))
                out.push_back(f.id);
    }
    return out;
}
