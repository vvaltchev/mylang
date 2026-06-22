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

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>     /* getenv */
#include <unistd.h>    /* isatty */

using std::string;

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
    std::vector<unique_ptr<Construct>> retained;
    std::vector<string> lines;
    int next_line = 1;

    Impl()
        : const_ctx(new EvalContext(nullptr, /*const_ctx=*/true))
        , runtime_ctx(new EvalContext(nullptr, /*const_ctx=*/false))
    {
        /* The prompt lets you redefine a global (re-declare to change its
         * value/type), unlike a script where a duplicate decl is an error. */
        runtime_ctx->allow_redeclare = true;
    }

    string format_error(const Exception &e) const {
        std::ostringstream o;
        format_exception(o, e, lines);
        return o.str();
    }

    string do_eval(const string &src, bool echo);
    string meta_command(const string &src);
    string cmd_tree(const string &code);
    string cmd_source(const string &path);
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

string
ReplEngine::Impl::do_eval(const string &src, bool echo)
{
    std::ostringstream out;

    /* A REPL input is not semicolon-terminated the way a script statement is
     * (you don't type `;` at a prompt, like Ruby), so auto-terminate it. A
     * trailing `;` (or `;` after a closing `}`) is just an empty statement, so
     * appending one is always safe. */
    string source = src;
    {
        const size_t e = source.find_last_not_of(" \t\r\n");
        if (e == string::npos)
            return "";                       /* blank input */
        source.erase(e + 1);
        if (source.back() != ';')
            source += ';';
    }

    /* 1. Split into physical lines, append to the persistent source, and lex
     *    each into this input's token vector (line numbers continue). */
    std::vector<Tok> tokens;
    {
        std::stringstream ss(source);
        string line;
        while (std::getline(ss, line)) {
            lines.push_back(line);
            lexer(lines.back(), next_line++, tokens);
        }
    }

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

    /* 3. Evaluate each top-level statement directly in the persistent global
     *    scope, capturing print() output and the last statement's value. */
    Block *blk = dynamic_cast<Block *>(root.get());
    EvalValue last;
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

    /* 4. The `=> value` echo (RValue collapses an lvalue result). */
    if (echo) {
        const EvalValue r = RValue(last);
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
    if (cmd == "source" || cmd == "load")
        return cmd_source(arg);
    if (cmd == "help" || cmd == "h") {
        return
            ":help            this message\n"
            ":tree <code>     show the parsed (const-folded) syntax tree\n"
            ":source <file>   evaluate a file as if typed at the prompt\n"
            ":quit            exit the REPL\n";
    }
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

    int depth = 0;
    for (const auto &t : toks) {
        if (t == Op::parenL || t == Op::braceL || t == Op::bracketL)
            depth++;
        else if (t == Op::parenR || t == Op::braceR || t == Op::bracketR)
            depth--;
    }
    if (depth > 0)
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
/* The interactive loop (cooked mode for now - a hand-rolled raw line  */
/* editor replaces std::getline in Phase 1).                           */
/* ------------------------------------------------------------------ */

int
run_repl()
{
    ReplEngine engine;
    std::vector<string> history;

    /* Colors on a TTY unless NO_COLOR is set (https://no-color.org). */
    const bool color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
    set_highlight_enabled(color);
    auto *hl = color ? highlight_line : nullptr;

    std::cout << "MyLang REPL. :quit (or Ctrl-D) to exit, :help for help.\n";

    string input;
    bool continuing = false;

    while (true) {

        ReadLineResult r = read_line(continuing ? ".. " : ">> ", history, hl);

        if (r.eof) {
            /* Ctrl-D mid-block abandons it; at a fresh prompt it exits. */
            if (continuing) {
                input.clear();
                continuing = false;
                continue;
            }
            break;
        }

        if (r.cancelled) {                  /* Ctrl-C: drop the current input */
            input.clear();
            continuing = false;
            continue;
        }

        const string &line = r.line;
        if (!line.empty())
            history.push_back(line);

        /* A meta-command (`:name ...`) at a fresh prompt is a complete one-line
         * input - never accumulated for multi-line continuation. */
        if (!continuing) {
            const size_t s = line.find_first_not_of(" \t");
            if (s != string::npos && line[s] == ':') {
                const string c = line.substr(s);
                if (c == ":quit" || c == ":q")
                    break;
                std::cout << engine.eval_input(line);
                continue;
            }
        }

        if (!input.empty())
            input += "\n";
        input += line;

        if (ReplEngine::is_incomplete(input)) {
            continuing = true;
            continue;
        }

        std::cout << engine.eval_input(input);

        input.clear();
        continuing = false;
    }

    return 0;
}
