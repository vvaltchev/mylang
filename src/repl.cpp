/* SPDX-License-Identifier: BSD-2-Clause */

#include "repl.h"
#include "parser.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"
#include "errors.h"
#include "evalvalue.h"
#include "errfmt.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
};

ReplEngine::ReplEngine() : impl(new Impl) { }
ReplEngine::~ReplEngine() = default;

string
ReplEngine::eval_input(const string &src)
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
            impl->lines.push_back(line);
            lexer(impl->lines.back(), impl->next_line++, tokens);
        }
    }

    if (tokens.empty())
        return "";

    /* 2. Parse against the PERSISTENT const context (no pushed top-level scope,
     *    so new consts/pure-funcs/structs survive to the next input). */
    unique_ptr<Construct> root;
    try {
        ParseContext pc(TokenStream(tokens), true);
        pc.const_ctx = impl->const_ctx.get();
        root = pBlock(pc, 0, /*push_const_scope=*/false);
        if (!pc.eoi())
            throw SyntaxErrorEx(Loc(pc.get_tok().loc),
                                "Unexpected token at the end", &pc.get_tok());
    } catch (const Exception &e) {
        return impl->format_error(e);
    }

    /* 3. Evaluate each top-level statement directly in the persistent global
     *    scope, capturing print() output and the last statement's value. */
    Block *blk = dynamic_cast<Block *>(root.get());
    EvalValue last;
    std::streambuf *old_cout = std::cout.rdbuf(out.rdbuf());

    try {
        if (blk) {
            for (const auto &e : blk->elems) {
                EvalValue v = e->eval(impl->runtime_ctx.get());
                if (v.is<UndefinedId>())
                    throw UndefinedVariableEx(
                        v.get<UndefinedId>().id, e->start, e->end);
                last = move(v);
            }
        }
    } catch (const Exception &e) {
        std::cout.rdbuf(old_cout);
        impl->retained.push_back(move(root));   /* keep partial defs alive */
        out << impl->format_error(e);
        return out.str();
    }

    std::cout.rdbuf(old_cout);
    impl->retained.push_back(move(root));

    /* 4. The `=> value` echo (RValue collapses an lvalue result). */
    const EvalValue r = RValue(last);
    out << "=> " << r.get_type()->to_string(r) << "\n";
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

    std::cout << "MyLang REPL. Type :quit (or Ctrl-D) to exit.\n";

    string input;
    bool continuing = false;

    while (true) {

        std::cout << (continuing ? ".. " : ">> ") << std::flush;

        string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;                              /* EOF / Ctrl-D */
        }

        if (!continuing && (line == ":quit" || line == ":q"))
            break;

        if (!input.empty())
            input += "\n";
        input += line;

        if (ReplEngine::is_incomplete(input)) {
            continuing = true;
            continue;
        }

        const string out = engine.eval_input(input);
        std::cout << out;

        input.clear();
        continuing = false;
    }

    return 0;
}
