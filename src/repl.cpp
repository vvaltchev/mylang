/* SPDX-License-Identifier: BSD-2-Clause */

#include "repl.h"
#include "parser.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"
#include "errors.h"
#include "evalvalue.h"
#include "errfmt.h"
#include "lineedit.h"
#include "highlight.h"
#include "structtype.h"
#include "inferencer.h"
#include "resolver.h"
#include "analyzer.h"
#include "replhelp.h"
#include "trace.h"
#include "reflect.h"
#include "coderender.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <iterator>
#include <cctype>
#include <cstdlib>     /* getenv */
#ifndef _WIN32
#include <unistd.h>    /* isatty */
#endif

using std::string;

/* Is stdout a terminal (for color)? The REPL is non-interactive on Windows
 * (see plans/repl.md), so there it is always "not a tty" - no color. */
static bool
repl_out_is_tty()
{
#ifdef _WIN32
    return false;
#else
    return isatty(STDOUT_FILENO);
#endif
}

/*
 * Persistent interpreter state for the REPL. The const context and the runtime
 * global scope are both roots that live for the whole session; the const one
 * auto-loads const_builtins, the runtime one const_builtins + builtins (see the
 * EvalContext root constructor). `retained` keeps every committed input's AST
 * alive so a prior pure func / struct / kept const stays valid for later
 * inputs.
 * `lines` is the ever-growing source, so an error caret points at the right
 * input line - exactly as the file driver uses its own `lines`.
 */
struct ReplEngine::Impl {

    unique_ptr<EvalContext> const_ctx;
    unique_ptr<EvalContext> runtime_ctx;
    ReplInfer infer;               /* faithful per-input type-checking */
    std::vector<unique_ptr<Construct>> retained;
    std::vector<string> lines;

    Impl()
        : const_ctx(new EvalContext(nullptr, /*const_ctx=*/true))
        , runtime_ctx(new EvalContext(nullptr, /*const_ctx=*/false))
    {
        /* The prompt lets you redefine a function or struct (the edit-and-
         * resubmit workflow), unlike a script where a duplicate decl is an
         * error. Set on both scopes: structs / pure funcs are registered in the
         * const context at parse time, vars/funcs in the runtime one. (A plain
         * `var`'s TYPE still sticks - the inferencer's job, not this.) */
        runtime_ctx->allow_redeclare = true;
        const_ctx->allow_redeclare = true;
    }

    string format_error(const Exception &e) const {
        std::ostringstream o;
        format_exception(o, e, lines);
        return o.str();
    }

    /* The interned names currently bound in the global scope (sorted), used to
     * detect what an input's undef() removed. */
    std::vector<const UniqueId *> global_names() const {
        std::vector<std::pair<const UniqueId *, const LValue *>> s;
        runtime_ctx->collect_symbols(s);
        std::vector<const UniqueId *> names;
        names.reserve(s.size());
        for (const auto &kv : s)
            names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        return names;
    }

    string do_eval(const string &src, bool echo);
    string meta_command(const string &src);
    string cmd_tree(const string &code);
    string cmd_analyze(const string &code);
    string cmd_source(const string &path);
    string cmd_trace(const string &arg);
    string cmd_globals();
    string cmd_type(const string &code);
    string cmd_show(const string &arg);
    string cmd_undef(const string &name);

    /* GC a redefined function's now-orphaned template/spec instances */
    void gc_redefined_instances(Block *blk,
                                const std::vector<const UniqueId *> &before);

    /* lex `source` into stable per-line storage (the lexer holds string_views
     * into the lines, so they must outlive the parse). */
    static void lex_stable(const string &source,
                           std::vector<string> &local_lines,
                           std::vector<Tok> &toks) {
        std::stringstream ss(source);
        string line;
        while (std::getline(ss, line))
            local_lines.push_back(line);
        for (size_t i = 0; i < local_lines.size(); i++)
            lexer(local_lines[i], static_cast<int>(i + 1), toks);
    }
};

ReplEngine::ReplEngine() : impl(new Impl) { }
ReplEngine::~ReplEngine() = default;

string
ReplEngine::eval_input(const string &src)
{
    /* A leading ':' is a REPL meta-command (inspection / load / help), never
     * mylang source. */
    {
        const size_t s = src.find_first_not_of(" \t\r\n");
        if (s != string::npos && src[s] == ':')
            return impl->meta_command(src.substr(s));
    }
    return impl->do_eval(src, /*echo=*/true);
}

/* Is `op` a token after which a statement clearly continues (so no `;` should
 * be auto-inserted at a line end)? Binary/assign operators, comma/dot/arrow/
 * colon, an open bracket, or a semicolon already present. */
static bool
repl_continuation_op(Op op)
{
    switch (op) {
        case Op::plus:  case Op::minus: case Op::times: case Op::div:
        case Op::mod:   case Op::lt:    case Op::gt:    case Op::le:
        case Op::ge:    case Op::eq:    case Op::noteq: case Op::land:
        case Op::lor:   case Op::band:  case Op::bor:   case Op::assign:
        case Op::addeq: case Op::subeq: case Op::muleq: case Op::diveq:
        case Op::modeq: case Op::comma: case Op::dot:   case Op::arrow:
        case Op::colon: case Op::parenL: case Op::braceL: case Op::bracketL:
        case Op::semicolon:
            return true;
        default:
            return false;
    }
}

/*
 * Insert statement-terminating `;` so a REPL user need not type them (like
 * Ruby). Per physical line, a `;` is appended unless: the line is empty /
 * comment-only; the line ends inside an unclosed `(` or `[` (an expression
 * spanning lines, e.g. an array literal); or its last token is a continuation
 * (a binary operator, comma, an open bracket, ...). Inside a `{}` block, each
 * statement line is terminated, so a multi-line func/if body works without
 * typing `;`. A `;` after a `}` is a harmless empty statement (and is needed
 * after a dict literal), so it is added.
 */
static string
repl_auto_terminate(const string &src)
{
    std::vector<string> phys;
    {
        std::stringstream ss(src);
        string line;
        while (std::getline(ss, line))
            phys.push_back(line);
    }

    string out;
    /*
     * Bracket-context stack. `(` and `[` are expression brackets; a `{` is
     * either a block ('B', a statement body where lines need `;`) or a dict
     * literal ('D', an expression where they don't). We tell them apart by
     * whether the previous token expected a value: a `{` right after `=`,
     * `(`, `,`, `:`, `=>`, a binary op, or `return` is a dict; otherwise
     * (after `)`, `;`, `}`, a keyword, or at statement start) it is a block.
     */
    std::vector<char> stack;
    bool expects_value = false;       /* statement position at the start */

    for (const string &line : phys) {

        std::vector<Tok> toks;
        try {
            lexer(line, 1, toks);     /* only token kinds/ops are read */
        } catch (...) {
            /* a bad token: leave the line as-is and let the parser report it */
            out += line;
            out += '\n';
            continue;
        }

        const Tok *last = nullptr;
        for (const auto &t : toks) {
            if (t == Op::braceL) {
                const char kind = expects_value ? 'D' : 'B';
                stack.push_back(kind);
                expects_value = (kind == 'D');
            } else if (t == Op::parenL) {
                stack.push_back('(');
                expects_value = true;
            } else if (t == Op::bracketL) {
                stack.push_back('[');
                expects_value = true;
            } else if (t == Op::parenR || t == Op::bracketR ||
                       t == Op::braceR) {
                if (!stack.empty())
                    stack.pop_back();
                expects_value = false;
            } else if (t == Keyword::kw_return) {
                expects_value = true;
            } else {
                /* an operator that takes a RHS keeps us in value position */
                expects_value =
                    t.op != Op::invalid && repl_continuation_op(t.op);
            }
            last = &t;
        }

        out += line;

        const char ctx = stack.empty() ? 'B' : stack.back();
        const bool stmt_ctx = (ctx == 'B');     /* not 'D'/'('/'[' */
        if (last && stmt_ctx && !repl_continuation_op(last->op))
            out += ';';
        out += '\n';
    }

    return out;
}

string
ReplEngine::Impl::do_eval(const string &src, bool echo)
{
    std::ostringstream out;

    /* A REPL input is not semicolon-terminated the way a script is (you don't
     * type `;` at a prompt, like Ruby): auto-insert them per line. */
    if (src.find_first_not_of(" \t\r\n") == string::npos)
        return "";                           /* blank input */
    const string source = repl_auto_terminate(src);

    /* Point the diagnostic-trace sink at this input's capture stream for the
     * whole pipeline (inference/resolve/specialize run before the cout swap),
     * so an enabled :trace narrates into the REPL output, just above the
     * result. Restored on every return path. */
    struct TraceSinkGuard {
        std::ostream *prev;
        explicit TraceSinkGuard(std::ostream *os) : prev(trace_sink()) {
            trace_set_sink(os);
        }
        ~TraceSinkGuard() { trace_set_sink(prev); }
    } trace_guard(&out);

    /* 1. Append all physical lines to the persistent source FIRST (so the
     *    vector finishes any reallocation), THEN lex from the now-stable
     *    buffers - the lexer stores string_views into them, so lexing while
     *    the vector still grows would dangle earlier lines' tokens. The line
     *    number of lines[i] is i + 1 (1-based, continuing across inputs). */
    const size_t base = lines.size();
    {
        std::stringstream ss(source);
        string line;
        while (std::getline(ss, line))
            lines.push_back(line);
    }

    std::vector<Tok> tokens;
    for (size_t i = base; i < lines.size(); i++)
        lexer(lines[i], static_cast<int>(i + 1), tokens);

    if (tokens.empty())
        return "";

    /* 2. Parse against the PERSISTENT const context (no pushed top-level scope,
     *    so new consts/pure-funcs/structs survive to the next input). */
    unique_ptr<Construct> root;
    try {
        ParseContext pc(TokenStream(tokens), true);
        pc.const_ctx = const_ctx.get();
        root = pBlock(pc, 0, /*push_const_scope=*/false);
        if (!pc.eoi())
            throw SyntaxErrorEx(Loc(pc.get_tok().loc),
                                "Unexpected token at the end", &pc.get_tok());
    } catch (const Exception &e) {
        return format_error(e);
    }

    /* 2b. Type-check this input against the committed globals (faithful
     *     incremental inference). A type error rejects just this input - report
     *     it and commit nothing (no eval, no retain). On success the inferencer
     *     has pinned this input's new globals for the next input, and stamped
     *     ArrHints/TypeHints on the tree the evaluator below reads. */
    try {
        infer.check_input(root.get());
    } catch (const Exception &e) {
        retained.push_back(move(root));   /* keep the AST alive (id_sym refs) */
        /* keep any trace lines emitted before the error (the reasoning that
         * led up to it), then the error itself. */
        return out.str() + format_error(e);
    }

    /* 2c. Run the real optimizers so the REPL is the true interpreter (and the
     *     optimizations are inspectable). repl_mode keeps top-level decls as
     *     persistent map globals (never slotted / auto-const-promoted), while
     *     nested locals slot, inline, and specialize as in a script. */
    /* The SAME optimizer pipeline the script driver runs (run_optimizers), so
     * the REPL's tree transformation is on par with a script's - just with
     * repl_mode (top-level decls stay map globals) and the prior-input scope
     * seeded for cross-input fold / inline / specialize. */
    run_optimizers(root.get(), /*enable_inline=*/true, /*inline_threshold=*/24,
                   /*enable_specialize=*/true, /*repl_mode=*/true,
                   /*prior_scope=*/runtime_ctx.get());

    /* 3. Evaluate each top-level statement directly in the persistent global
     *    scope, capturing print() output and the last statement's value. */
    Block *blk = dynamic_cast<Block *>(root.get());
    EvalValue last;
    const std::vector<const UniqueId *> names_before = global_names();
    std::streambuf *old_cout = std::cout.rdbuf(out.rdbuf());

    try {
        if (blk) {
            for (const auto &e : blk->elems) {
                EvalValue v = e->eval(runtime_ctx.get());
                if (v.is<UndefinedId>())
                    throw UndefinedVariableEx(
                        v.get<UndefinedId>().id, e->start, e->end);
                last = move(v);
            }
        }
    } catch (const Exception &e) {
        std::cout.rdbuf(old_cout);
        retained.push_back(move(root));   /* keep partial defs alive */
        out << format_error(e);
        return out.str();
    }

    std::cout.rdbuf(old_cout);
    retained.push_back(move(root));

    /* (Removing a global from the type environment is the `:undef` command's
     * job now - there is no runtime undef() builtin to detect here.) */

    /* Drop any template/spec instances orphaned by a redefinition in this
     * input (e.g. f$0 from a throwaway `f(1,2)` after `func f` is redefined),
     * unless another function still consumes them. */
    gc_redefined_instances(blk, names_before);

    /* 4. The `=> value` echo (RValue collapses an lvalue result). A `none`
     *    result - a func/struct decl, a `print`, an `if`/loop, a void call -
     *    is not echoed, so the prompt stays uncluttered. */
    if (echo) {
        const EvalValue r = RValue(last);
        if (!r.is<NoneVal>())
            out << "=> " << r.get_type()->to_string(r) << "\n";
    }
    return out.str();
}

/* ------------------------------------------------------------------ */
/* Meta-commands (`:name args`) - inspection / load / help.            */
/* ------------------------------------------------------------------ */

string
ReplEngine::Impl::meta_command(const string &src)
{
    /* Split into the command word and the rest (the argument). */
    string cmd, arg;
    {
        size_t i = 1;                       /* skip the ':' */
        while (i < src.size() && !isspace(static_cast<unsigned char>(src[i])))
            cmd += src[i++];
        while (i < src.size() && isspace(static_cast<unsigned char>(src[i])))
            i++;
        arg = src.substr(i);
        const size_t e = arg.find_last_not_of(" \t\r\n");
        arg.erase(e == string::npos ? 0 : e + 1);
    }

    if (cmd == "tree" || cmd == "s")
        return cmd_tree(arg);
    if (cmd == "analyze" || cmd == "a")
        return cmd_analyze(arg);
    if (cmd == "source" || cmd == "load")
        return cmd_source(arg);
    if (cmd == "help" || cmd == "h") {
        const bool color = repl_out_is_tty() && !std::getenv("NO_COLOR");
        return repl_help(arg, color);
    }
    if (cmd == "trace")
        return cmd_trace(arg);
    if (cmd == "globals" || cmd == "g")
        return cmd_globals();
    if (cmd == "type" || cmd == "t")
        return cmd_type(arg);
    if (cmd == "show")
        return cmd_show(arg);
    if (cmd == "undef")
        return cmd_undef(arg);
    if (cmd == "quit" || cmd == "q")
        return "";              /* the loop handles the actual exit */

    return "Unknown command ':" + cmd + "' (try :help)\n";
}

/*
 * :tree - parse `code` and print its syntax tree WITHOUT committing it. We pass
 * push_const_scope=true so prior consts are visible for folding but any new
 * const/func/struct binds in the pushed scope the parser pops (so persistent
 * state is untouched), and we never evaluate or retain the result.
 */
string
ReplEngine::Impl::cmd_tree(const string &code)
{
    if (code.empty())
        return "usage: :tree <code>\n";

    string source = code;
    const size_t e = source.find_last_not_of(" \t\r\n");
    source.erase(e + 1);
    if (!source.empty() && source.back() != ';')
        source += ';';

    /* Keep the source lines in a stable vector: the lexer stores string_views
     * into them, so they must outlive the parse (a local getline buffer would
     * dangle). The vector also backs an error caret. */
    std::vector<string> local_lines;
    {
        std::stringstream ss(source);
        string line;
        while (std::getline(ss, line))
            local_lines.push_back(line);
    }

    std::vector<Tok> toks;
    for (size_t i = 0; i < local_lines.size(); i++)
        lexer(local_lines[i], static_cast<int>(i + 1), toks);

    try {
        ParseContext pc(TokenStream(toks), true);
        pc.const_ctx = const_ctx.get();
        unique_ptr<Construct> root = pBlock(pc, 0, /*push_const_scope=*/true);
        std::ostringstream o;
        o << *root << "\n";
        return o.str();
    } catch (const Exception &ex) {
        std::ostringstream o;
        format_exception(o, ex, local_lines);
        return o.str();
    }
}

/*
 * :analyze - the REPL's `-a`: parse `code` and reprint it colored by which
 * compile-time optimizations fired (auto-const/pure, flat vs dyn array,
 * inlined, specialized, folded, dead code). Non-committing like :tree (parsed
 * against a pushed scope, never evaluated), and analyzed in repl_mode so the
 * top-level mirrors how the REPL actually resolves.
 */
string
ReplEngine::Impl::cmd_analyze(const string &code)
{
    if (code.empty())
        return "usage: :analyze <code>\n";

    string source = code;
    const size_t e = source.find_last_not_of(" \t\r\n");
    source.erase(e + 1);
    if (!source.empty() && source.back() != ';')
        source += ';';

    std::vector<string> local_lines;
    std::vector<Tok> toks;
    lex_stable(source, local_lines, toks);

    try {
        AnalysisInfo info;
        ParseContext pc(TokenStream(toks), true);
        pc.const_ctx = const_ctx.get();
        pc.analysis = &info;        /* record parse-time folds / dead code */
        unique_ptr<Construct> root = pBlock(pc, 0, /*push_const_scope=*/true);

        const bool color =
            repl_out_is_tty() && !std::getenv("NO_COLOR");
        std::ostringstream o;
        analyze_and_render(o, root.get(), info, local_lines, color,
                           /*repl_mode=*/true);
        return o.str();
    } catch (const Exception &ex) {
        std::ostringstream o;
        format_exception(o, ex, local_lines);
        return o.str();
    }
}

/*
 * :source - read a file and feed it to the engine EXACTLY as if typed: split it
 * into complete top-level units (the same completeness rule the loop uses) and
 * eval each as one input, in order, so cross-input semantics and per-unit error
 * recovery apply. The `=>` echo is suppressed for sourced units (print() output
 * and errors still show).
 */
string
ReplEngine::Impl::cmd_source(const string &path)
{
    if (path.empty())
        return "usage: :source <file>\n";

    std::ifstream f(path);
    if (!f.is_open())
        return "Failed to open file '" + path + "'\n";

    std::ostringstream out;
    string unit, line;

    while (std::getline(f, line)) {
        if (!unit.empty())
            unit += "\n";
        unit += line;
        if (ReplEngine::is_incomplete(unit))
            continue;
        out << do_eval(unit, /*echo=*/false);
        unit.clear();
    }

    if (!unit.empty())                 /* a trailing incomplete unit: try it */
        out << do_eval(unit, /*echo=*/false);

    return out.str();
}

/*
 * :trace - toggle the diagnostic trace categories (see trace.h).
 *   :trace                       show the active categories
 *   :trace off | :trace none     disable all
 *   :trace <cat> [<cat>...]       enable those categories
 *   :trace <cat> [<cat>...] on|off  enable/disable those categories
 * A category narrates the compiler's reasoning (inference/inlining/...) just
 * before the next input runs.
 */
string
ReplEngine::Impl::cmd_trace(const string &arg)
{
    std::vector<string> toks;
    {
        std::stringstream ss(arg);
        string w;
        while (ss >> w)
            toks.push_back(w);
    }

    if (toks.empty())
        return trace_state_str() + "\n";

    if (toks[0] == "help" || toks[0] == "?" || toks[0] == "list") {
        string out = "trace categories:\n";
        out += trace_categories_help("  ");
        out += "usage: :trace <cat>... on|off   :trace off   "
               ":trace   (show active)\n";
        out += trace_state_str() + "\n";
        return out;
    }

    if (toks.size() == 1 && (toks[0] == "off" || toks[0] == "none")) {
        trace_clear_all();
        return trace_state_str() + "\n";
    }

    /* a trailing on/off sets the direction; otherwise the categories go on */
    bool on = true;
    if (toks.back() == "on" || toks.back() == "off") {
        on = (toks.back() == "on");
        toks.pop_back();
    }

    string unknown;
    for (const string &c : toks)
        if (!trace_set(c, on))
            unknown += (unknown.empty() ? "" : ", ") + c;

    string out = trace_state_str() + "\n";
    if (!unknown.empty())
        out = "Unknown trace category: " + unknown +
              " (try :help optimizations)\n" + out;
    return out;
}

/*
 * :globals - a table of every global the session holds: variables, functions,
 * structs, and consts (including const SCALARS, which are folded out of the
 * runtime scope but live in the persistent const context). Each row shows the
 * name, its type (the inferencer's INFERRED/declared static type for a var; a
 * signature for a function; a constructor for a struct type), and the kind.
 */
string
ReplEngine::Impl::cmd_globals()
{
    struct Row { string name; string type; const char *kind; };
    std::vector<Row> rows;
    std::set<const UniqueId *> seen;

    auto classify = [&](const UniqueId *uid, const EvalValue &v,
                        bool is_const) -> Row {
        Row r;
        r.name = string(uid->val);
        if (v.is<shared_ptr<FuncObject>>()) {
            r.kind = "func";
            r.type = reflect_func_sig(v.get<shared_ptr<FuncObject>>()->func);
        } else if (v.is<StructTypeDef *>()) {
            r.kind = "struct type";
            r.type = reflect_struct_ctor(v.get<StructTypeDef *>());
        } else if (v.is<intrusive_ptr<StructObject>>()) {
            r.kind = is_const ? "const" : "var";
            r.type = string(
                v.get<intrusive_ptr<StructObject>>()->def->name->val);
        } else {
            r.kind = is_const ? "const" : "var";
            /* the inferred/declared static type (richer than the runtime kind:
             * opt, dyn, array<dyn>, ...); fall back to the runtime structural
             * type for a const scalar the inferencer never saw as a symbol. */
            const string st = infer.global_type(uid);
            r.type = !st.empty() ? st : reflect_typeof(v);
        }
        return r;
    };

    std::vector<std::pair<const UniqueId *, const LValue *>> rsyms;
    runtime_ctx->collect_symbols(rsyms);
    for (const auto &kv : rsyms) {
        if (EvalContext::const_builtins.count(kv.first) ||
            EvalContext::builtins.count(kv.first))
            continue;
        seen.insert(kv.first);
        rows.push_back(classify(kv.first, kv.second->get(),
                                kv.second->is_const_var()));
    }

    /* const scalars: present in the const context, folded out of runtime */
    std::vector<std::pair<const UniqueId *, const LValue *>> csyms;
    const_ctx->collect_symbols(csyms);
    for (const auto &kv : csyms) {
        if (EvalContext::const_builtins.count(kv.first) ||
            seen.count(kv.first))
            continue;
        rows.push_back(classify(kv.first, kv.second->get(), true));
    }

    if (rows.empty())
        return "(no globals defined yet)\n";

    std::sort(rows.begin(), rows.end(),
              [](const Row &a, const Row &b) { return a.name < b.name; });

    size_t w = 0;
    for (const Row &r : rows)
        w = std::max(w, r.name.size());

    std::ostringstream o;
    for (const Row &r : rows) {
        o << r.name;
        for (size_t i = r.name.size(); i < w; i++)
            o << ' ';
        o << " : " << r.type << "   [" << r.kind << "]\n";
    }
    return o.str();
}

/*
 * :undef <name> - remove a GLOBAL symbol from the session: the runtime scope,
 * the const context, and the type environment (so a later input may re-declare
 * it, even with a new type). This is a REPL-only convenience: a script's
 * symbols are fixed slots at compile time, so there is no `undef` builtin -
 * a script just re-defines a name instead. Mirrors gc_redefined_instances'
 * removal (a fresh Identifier resolves via the map, where REPL globals live).
 */
string
ReplEngine::Impl::cmd_undef(const string &name)
{
    if (name.empty())
        return "usage: :undef <name>\n";

    Identifier id(name);
    const bool in_rt = runtime_ctx->erase(&id);
    const bool in_ct = const_ctx->erase(&id);

    if (in_rt || in_ct) {
        infer.undef_global(id.uid);
        return "undefined '" + name + "'\n";
    }
    return "no global named '" + name + "'\n";
}

/*
 * :type <expr> - show a type without committing anything. For a bare committed
 * global it reports the inferencer's INFERRED/declared static type (opt, dyn,
 * array<dyn>, a function/struct shape); for any other expression it evaluates
 * it in a throwaway child scope and reports the runtime structural type. The
 * expression is parsed against (but does not modify) the persistent state.
 */
string
ReplEngine::Impl::cmd_type(const string &code)
{
    if (code.empty())
        return "usage: :type <expr>\n";

    string source = code;
    const size_t e = source.find_last_not_of(" \t\r\n");
    source.erase(e == string::npos ? 0 : e + 1);
    if (!source.empty() && source.back() != ';')
        source += ';';

    std::vector<string> local_lines;
    std::vector<Tok> toks;
    lex_stable(source, local_lines, toks);

    try {
        ParseContext pc(TokenStream(toks), true);
        pc.const_ctx = const_ctx.get();
        unique_ptr<Construct> root = pBlock(pc, 0, /*push_const_scope=*/true);
        Block *blk = dynamic_cast<Block *>(root.get());
        if (!blk || blk->elems.empty())
            return "(empty)\n";

        /* a single bare identifier that is a committed global -> its inferred
         * static type (no evaluation needed, richer than the value kind). */
        if (blk->elems.size() == 1) {
            if (auto *id = dynamic_cast<Identifier *>(blk->elems[0].get())) {
                const string st = infer.global_type(id->uid);
                if (!st.empty())
                    return string(id->get_str()) + " : " + st +
                           "   (inferred static type)\n";
            }
        }

        /* otherwise evaluate in a throwaway child of the global scope (a
         * declaration in the expr does NOT persist), read the runtime type. */
        EvalContext child(runtime_ctx.get());
        EvalValue last;
        for (const auto &el : blk->elems)
            last = el->eval(&child);
        const EvalValue r = RValue(last);
        return reflect_typeof(r) + "   (runtime type)\n";

    } catch (const Exception &ex) {
        std::ostringstream o;
        format_exception(o, ex, local_lines);
        return o.str();
    }
}

/* Syntax-highlight a (possibly multi-line) rendered block when color is on,
 * line by line (highlight_line is per-line). */
static string
show_colorize(const string &s, bool color)
{
    if (!color)
        return s;
    string out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t nl = s.find('\n', start);
        const size_t end = (nl == string::npos) ? s.size() : nl;
        out += highlight_line(s.substr(start, end - start));
        if (nl == string::npos)
            break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

/* Render `fn` (and, recursively at the call site, its clones) with its inferred
 * parameter + return types. */
string
ReplEngine::Impl::cmd_show(const string &arg)
{
    if (arg.empty())
        return "usage: :show <function-or-expression>\n";

    const bool color = repl_out_is_tty() && !std::getenv("NO_COLOR");

    std::vector<std::pair<const UniqueId *, const LValue *>> syms;
    runtime_ctx->collect_symbols(syms);

    auto func_of = [](const LValue *lv) -> const FuncDeclStmt * {
        const EvalValue &v = lv->get();
        return v.is<shared_ptr<FuncObject>>()
                   ? v.get<shared_ptr<FuncObject>>()->func
                   : nullptr;
    };
    auto render = [&](const FuncDeclStmt *f) {
        return render_func_code(f, infer.func_param_types(f),
                                infer.func_return_type(f));
    };

    /* if `arg` is exactly a global FUNCTION name, render it + its clones */
    const FuncDeclStmt *base = nullptr;
    for (const auto &kv : syms)
        if (kv.first->val == arg)
            base = func_of(kv.second);

    if (base) {
        std::ostringstream o;
        o << render(base);
        std::vector<std::pair<string, const FuncDeclStmt *>> clones;
        for (const auto &kv : syms) {
            const FuncDeclStmt *g = func_of(kv.second);
            if (g && g != base && g->display_name == arg)
                clones.emplace_back(string(kv.first->val), g);
        }
        std::sort(clones.begin(), clones.end(),
                  [](const auto &x, const auto &y) {
                      return x.first < y.first;
                  });
        for (const auto &c : clones)
            o << "\n" << render(c.second);
        return show_colorize(o.str(), color);
    }

    /* otherwise treat `arg` as an EXPRESSION: parse it (non-committing) and run
     * the optimizers, then render the result - so :show 2 + 3 * 4 shows `14`
     * and :show f(x) shows how that call folds/inlines. */
    string source = arg;
    const size_t e = source.find_last_not_of(" \t\r\n");
    source.erase(e == string::npos ? 0 : e + 1);
    if (!source.empty() && source.back() != ';')
        source += ';';

    std::vector<string> local_lines;
    std::vector<Tok> toks;
    lex_stable(source, local_lines, toks);

    try {
        ParseContext pc(TokenStream(toks), true);
        pc.const_ctx = const_ctx.get();
        unique_ptr<Construct> root = pBlock(pc, 0, /*push_const_scope=*/true);
        resolve_names(root.get(), /*inline=*/true, 24, nullptr, /*repl=*/true);

        Block *blk = dynamic_cast<Block *>(root.get());
        string r;
        if (blk) {
            for (const auto &el : blk->elems)
                r += render_construct_code(el.get()) + "\n";
        } else {
            r = render_construct_code(root.get()) + "\n";
        }
        if (r.empty())
            return "(nothing to show)\n";
        return show_colorize(r, color);

    } catch (const Exception &ex) {
        std::ostringstream o;
        format_exception(o, ex, local_lines);
        return o.str();
    }
}

void
ReplEngine::Impl::gc_redefined_instances(
    Block *blk, const std::vector<const UniqueId *> &before_vec)
{
    if (!blk)
        return;

    const std::set<const UniqueId *> before(before_vec.begin(),
                                            before_vec.end());

    /* function names this input REDEFINED: a top-level user FuncDeclStmt
     * (display_name empty - not an inserted clone) whose name already existed.
     */
    std::set<string> redefined;
    for (const auto &e : blk->elems)
        if (auto *fd = dynamic_cast<FuncDeclStmt *>(e.get()))
            if (fd->id && fd->display_name.empty() &&
                before.count(fd->id->uid))
                redefined.insert(string(fd->id->get_str()));
    if (redefined.empty())
        return;

    /* OLD instance globals (present before this input) whose base was redefined
     * and which have no live function consumer -> remove from both scopes and
     * the inferencer. (New instances created this input aren't in `before`.) */
    std::vector<std::pair<const UniqueId *, const LValue *>> syms;
    runtime_ctx->collect_symbols(syms);

    std::vector<const UniqueId *> to_remove;
    for (const auto &kv : syms) {
        if (!before.count(kv.first))
            continue;
        const EvalValue &v = kv.second->get();
        if (!v.is<shared_ptr<FuncObject>>())
            continue;
        const FuncDeclStmt *fd = v.get<shared_ptr<FuncObject>>()->func;
        if (fd->display_name.empty() || !redefined.count(fd->display_name))
            continue;                       /* not a redefined func's clone */
        if (infer.instance_has_consumer(fd))
            continue;                       /* still used by a function */
        to_remove.push_back(kv.first);
    }

    for (const UniqueId *n : to_remove) {
        Identifier id(n->val);
        runtime_ctx->erase(&id);
        const_ctx->erase(&id);
        infer.undef_global(n);
    }
}

/* Net unclosed (){}/[] depth of `src` (>=0), lexer-based so strings/comments
 * don't count. Drives both is_incomplete and the continuation auto-indent. */
static int
repl_bracket_depth(const string &src)
{
    std::vector<Tok> toks;
    {
        std::stringstream ss(src);
        string line;
        int n = 1;
        while (std::getline(ss, line))
            lexer(line, n++, toks);
    }

    int depth = 0;
    for (const auto &t : toks) {
        if (t == Op::parenL || t == Op::braceL || t == Op::bracketL)
            depth++;
        else if (t == Op::parenR || t == Op::braceR || t == Op::bracketR)
            depth--;
    }
    return depth < 0 ? 0 : depth;
}

std::vector<string>
ReplEngine::completions(const string &buf, size_t cursor) const
{
    if (cursor > buf.size())
        cursor = buf.size();

    /* the identifier prefix ending at the cursor */
    size_t start = cursor;
    while (start > 0 &&
           (isalnum(static_cast<unsigned char>(buf[start - 1])) ||
            buf[start - 1] == '_'))
        start--;
    const string prefix = buf.substr(start, cursor - start);

    auto starts_with = [&](const string &s) {
        return s.size() >= prefix.size() &&
               s.compare(0, prefix.size(), prefix) == 0;
    };

    std::vector<string> out;

    if (start > 0 && buf[start - 1] == '.') {

        /* member access `base.<prefix>`: complete base's fields / consts */
        size_t bend = start - 1, bstart = bend;
        while (bstart > 0 &&
               (isalnum(static_cast<unsigned char>(buf[bstart - 1])) ||
                buf[bstart - 1] == '_'))
            bstart--;
        const string base = buf.substr(bstart, bend - bstart);

        if (!base.empty()) {
            std::vector<std::pair<const UniqueId *, const LValue *>> syms;
            impl->runtime_ctx->collect_symbols(syms);
            for (const auto &kv : syms) {
                if (kv.first->val != base)
                    continue;
                const EvalValue &v = kv.second->get();
                const StructTypeDef *def = nullptr;
                const bool inst = v.is<intrusive_ptr<StructObject>>();
                if (inst)
                    def = v.get<intrusive_ptr<StructObject>>()->def;
                else if (v.is<StructTypeDef *>())
                    def = v.get<StructTypeDef *>();
                if (def) {
                    if (inst)
                        for (const auto &f : def->fields)
                            if (starts_with(f.name->val))
                                out.push_back(f.name->val);
                    for (const auto &c : def->consts)
                        if (starts_with(c.first->val))
                            out.push_back(c.first->val);
                }
                break;
            }
        }

    } else {

        for (int i = 1; i < static_cast<int>(Keyword::kw_count); i++)
            if (starts_with(KwString[i]))
                out.push_back(KwString[i]);

        std::vector<std::pair<const UniqueId *, const LValue *>> syms;
        impl->runtime_ctx->collect_symbols(syms);
        for (const auto &kv : syms)
            if (starts_with(kv.first->val))
                out.push_back(kv.first->val);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool
ReplEngine::is_incomplete(const string &src)
{
    std::vector<Tok> toks;
    {
        std::stringstream ss(src);
        string line;
        int n = 1;
        while (std::getline(ss, line))
            lexer(line, n++, toks);
    }

    if (toks.empty())
        return false;

    if (repl_bracket_depth(src) > 0)
        return true;

    /* A line ending on a binary/continuation operator wants more. */
    switch (toks.back().op) {
        case Op::plus:    case Op::minus:   case Op::times:
        case Op::div:     case Op::mod:     case Op::lt:
        case Op::gt:      case Op::le:      case Op::ge:
        case Op::eq:      case Op::noteq:   case Op::land:
        case Op::lor:     case Op::assign:  case Op::comma:
        case Op::dot:     case Op::arrow:
            return true;
        default:
            return false;
    }
}

/* ------------------------------------------------------------------ */
/* The interactive loop (raw-mode line editor; see lineedit.cpp).       */
/* ------------------------------------------------------------------ */

/* The persisted history file path ($HOME/.mylang_history), or "" if no HOME. */
static string
history_path()
{
    const char *home = std::getenv("HOME");
    return home ? string(home) + "/.mylang_history" : string();
}

static const size_t HISTORY_CAP = 2000;

static void
load_history(std::vector<string> &history)
{
    const string path = history_path();
    if (path.empty())
        return;
    std::ifstream f(path);
    string line;
    while (std::getline(f, line))
        if (!line.empty())
            history.push_back(line);
    if (history.size() > HISTORY_CAP)
        history.erase(history.begin(), history.end() - HISTORY_CAP);
}

static void
save_history(const std::vector<string> &history)
{
    const string path = history_path();
    if (path.empty())
        return;
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return;
    const size_t from =
        history.size() > HISTORY_CAP ? history.size() - HISTORY_CAP : 0;
    for (size_t i = from; i < history.size(); i++)
        f << history[i] << "\n";
}

int
run_repl()
{
    ReplEngine engine;
    std::vector<string> history;
    load_history(history);

    /* Colors on a TTY unless NO_COLOR is set (https://no-color.org). */
    const bool color = repl_out_is_tty() && !std::getenv("NO_COLOR");
    set_highlight_enabled(color);
    trace_set_color(color);
    auto *hl = color ? highlight_line : nullptr;

    LineEditor::Completer completer =
        [&engine](const string &b, size_t cur) {
            return engine.completions(b, cur);
        };

    /* The editor keeps a block open until it parses complete (a meta-command
     * `:...` line is always complete - it never spans rows). */
    LineEditor::Submitter submitter = [](const string &s) {
        const size_t i = s.find_first_not_of(" \t\r\n");
        if (i != string::npos && s[i] == ':')
            return true;
        return !ReplEngine::is_incomplete(s);
    };

    std::cout << "MyLang REPL. :quit (or Ctrl-D) to exit, :help for help.\n";

    while (true) {

        ReadLineResult r =
            read_line(">> ", ".. ", history, hl, completer, submitter);

        if (r.eof)
            break;                          /* Ctrl-D at the prompt exits */
        if (r.cancelled)
            continue;                       /* Ctrl-C drops the input */

        const string &input = r.line;       /* may span multiple lines */
        if (input.find_first_not_of(" \t\r\n") == string::npos)
            continue;                       /* blank */

        /* A leading ':' meta-command (`:quit` exits; others run via eval_input,
         * which dispatches them). The whole input is one history entry. */
        const size_t s = input.find_first_not_of(" \t");
        const bool is_meta = (s != string::npos && input[s] == ':');
        if (is_meta && (input.substr(s) == ":quit" || input.substr(s) == ":q"))
            break;

        if (input != ":quit" && input != ":q")
            history.push_back(input);

        std::cout << engine.eval_input(input);
    }

    save_history(history);
    return 0;
}
