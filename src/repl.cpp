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

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cctype>
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
    ReplInfer infer;               /* faithful per-input type-checking */
    std::vector<unique_ptr<Construct>> retained;
    std::vector<string> lines;

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
        return format_error(e);
    }

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

    /* A global that undef() removed at runtime is also dropped from the type
     * environment, so a LATER input may re-declare it with a new type (the
     * documented way to change a global's type). */
    {
        const std::vector<const UniqueId *> names_after = global_names();
        std::vector<const UniqueId *> removed;
        std::set_difference(names_before.begin(), names_before.end(),
                            names_after.begin(), names_after.end(),
                            std::back_inserter(removed));
        for (const UniqueId *n : removed)
            infer.undef_global(n);
    }

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
    const bool color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
    set_highlight_enabled(color);
    auto *hl = color ? highlight_line : nullptr;

    LineEditor::Completer completer =
        [&engine](const string &b, size_t cur) {
            return engine.completions(b, cur);
        };

    std::cout << "MyLang REPL. :quit (or Ctrl-D) to exit, :help for help.\n";

    string input;
    bool continuing = false;

    while (true) {

        /* Auto-indent a continuation line by the current bracket depth. */
        const string indent =
            continuing ? string(repl_bracket_depth(input) * 2, ' ') : "";

        ReadLineResult r = read_line(continuing ? ".. " : ">> ", history, hl,
                                     indent, completer);

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
        if (!line.empty() && line != ":quit" && line != ":q")
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

    save_history(history);
    return 0;
}
