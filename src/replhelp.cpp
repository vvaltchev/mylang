/* SPDX-License-Identifier: BSD-2-Clause */

#include "replhelp.h"

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
  "The $specN / $tmplN specialization & template-instantiation clones derived "
  "from f.", nullptr },
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
{ "undef", "control", "undef(name)",
  "Remove name from the current scope (true if it existed).", nullptr },
{ "exception", "control", "exception(name, [data])",
  "Create an exception value with the given name and optional payload.",
  nullptr },
{ "ex", "control", "ex(name, [data])",
  "A shortcut for exception().", nullptr },
{ "exdata", "control", "exdata(e)",
  "The payload data of an exception value.", nullptr },

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
/* Populated by Pillar 2b; the dispatch handles an empty database gracefully. */

struct LangFeature {
    const char *id;
    const char *cat;       /* a language-category id */
    const char *title;     /* display title */
    const char *syntax;    /* a syntax sketch (may contain newlines) */
    const char *body;      /* the explanation */
};

const CatInfo lang_cats[] = {
    { "__none__", "" },    /* placeholder; replaced in 2b */
};

const LangFeature lang_features[] = {
    { "__none__", "__none__", "", "", "" },
};

bool lang_db_empty() { return true; }    /* flipped on in 2b */

const LangFeature *find_feature(const string &) { return nullptr; }
const CatInfo *find_lang_cat(const string &) { return nullptr; }

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
    o << "Inspection:\n";
    o << "  " << p.kw << ":globals" << p.rst
      << "  :type <expr>  :tree <code>  :analyze <code>\n";
    o << "  " << p.kw << ":trace <cat> on|off" << p.rst
      << "     narrate the compiler's reasoning\n";
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

    /* a single topic: builtin, then language category, then language feature */
    if (const BuiltinDoc *d = find_builtin(t)) {
        render_builtin_entry(o, *d, color);
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

    o << "No help for '" << t << "'. Try :help, :help builtins, or "
      << ":help language\n";
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
    for (const CatInfo &c : builtin_cats)
        if (pre(c.id))
            out.push_back(c.id);
    for (const BuiltinDoc &d : builtin_docs)
        if (pre(d.name))
            out.push_back(d.name);
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
