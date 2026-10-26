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
#include <map>
#include <unordered_set>
#include <algorithm>
#include <iterator>
#include <cctype>
#include <cstdlib>
#include "env.h"     /* env_get - cross-platform getenv */
#ifndef _WIN32
#include <unistd.h>    /* isatty */
#endif

using std::string;

/* Is stdout a terminal (for color)? The REPL is non-interactive on Windows
 * (see plans/archived/repl.md), so there it is always "not a tty" - no color. */
static bool
repl_out_is_tty()
{
#ifdef _WIN32
    return false;
#else
    return isatty(STDOUT_FILENO);
#endif
}

/* Syntax-highlight a (possibly multi-line) rendered block (defined below). */
static string show_colorize(const string &s, bool color);

/* Past the bracket that closes the one at toks[i] (end if unbalanced). */
static size_t
repl_skip_bracket(const std::vector<Tok> &toks, size_t i, size_t end)
{
    int depth = 0;
    for (; i < end; i++) {
        const Tok &t = toks[i];
        if (t == Op::parenL || t == Op::bracketL || t == Op::braceL)
            depth++;
        else if (t == Op::parenR || t == Op::bracketR || t == Op::braceR)
            if (--depth == 0)
                return i + 1;
    }
    return end;
}

/*
 * Is the statement WRITTEN as toks[i, end) a plain VALUE LOOKUP - a bare
 * variable, the `none`/`null` literal, or a member/subscript access (`a.b`,
 * `a?.b`, `f()[0]`)? Decided on the tokens, i.e. on what the user typed:
 * the tree has been folded by then, and a call folded to a literal (`w()`
 * of a void `func w() { }`, by the optimizers or by the parser's const
 * evaluation) reads exactly like a written `none`.
 */
static bool
repl_value_lookup(const std::vector<Tok> &toks, size_t i, size_t end)
{
    if (i >= end)
        return false;

    /* a parenthesized whole: what is inside it */
    if (toks[i] == Op::parenL && repl_skip_bracket(toks, i, end) == end)
        return repl_value_lookup(toks, i + 1, end - 1);

    /* the primary: a name, none / null, a bracketed literal, or a
     * parenthesized expression a step follows (`(d)["k"]`) */
    const Tok &first = toks[i];
    size_t k;
    if (first == TokType::id || first == Keyword::kw_none
            || first == Keyword::kw_null)
        k = i + 1;
    else if (first == Op::bracketL || first == Op::braceL
             || first == Op::parenL)
        k = repl_skip_bracket(toks, i, end);
    else
        return false;
    const bool bare = (first == TokType::id || first == Keyword::kw_none
                       || first == Keyword::kw_null);

    /* then postfix steps; the LAST one decides (a call is not a lookup) */
    bool last_is_access = false;
    while (k < end) {
        if ((toks[k] == Op::dot || toks[k] == Op::qmdot) && k + 1 < end
                && toks[k + 1] == TokType::id) {
            k += 2;
            last_is_access = true;
        } else if (toks[k] == Op::bracketL) {
            k = repl_skip_bracket(toks, k, end);
            last_is_access = true;
        } else if (toks[k] == Op::parenL) {
            k = repl_skip_bracket(toks, k, end);
            last_is_access = false;
        } else {
            return false;                /* an operator: not a lookup */
        }
    }
    return k == i + 1 ? bare : last_is_access;
}

/*
 * Should a `none`-valued result be echoed? A `none` is normally suppressed (a
 * func/struct decl, a `print`, an `if`/loop, a void call all yield it - echoing
 * `=> none` for them would be noise). But when the last statement is a plain
 * VALUE LOOKUP (repl_value_lookup) the user asked to SEE that value, so show
 * `=> none`. `last` is the last top-level statement, after the optimizers -
 * only its START is used, to find its tokens.
 */
static bool
repl_echo_none(const Construct *last, const std::vector<Tok> &toks)
{
    /* the node is a lookup or a literal (never a declaration, whose node
     * starts at the declared NAME), and its tokens are a lookup */
    if (!last || !last->start
            || !(last->is_id() || dynamic_cast<const MemberExpr *>(last)
                 || dynamic_cast<const Subscript *>(last)
                 || dynamic_cast<const LiteralNone *>(last)))
        return false;
    const Loc s = last->start;
    size_t i = 0;
    while (i < toks.size()
           && (toks[i].loc.line < s.line
               || (toks[i].loc.line == s.line && toks[i].loc.col < s.col)))
        i++;
    /* a parenthesized statement's node starts INSIDE its parentheses (a
     * `(` before a top-level statement's start can only be its own) */
    while (i > 0 && toks[i - 1] == Op::parenL)
        i--;
    /* the statement ends at the first `;` outside brackets */
    size_t end = i;
    int depth = 0;
    for (; end < toks.size(); end++) {
        const Tok &t = toks[end];
        if (t == Op::parenL || t == Op::bracketL || t == Op::braceL)
            depth++;
        else if (t == Op::parenR || t == Op::bracketR || t == Op::braceR)
            depth--;
        else if (t == Op::semicolon && depth <= 0)
            break;
    }
    return repl_value_lookup(toks, i, end);
}

/* The global names a top-level statement binds when it runs: a function or
 * struct declaration's name, a `var`/`const` declaration's targets. */
static void
repl_declared_names(const Construct *c,
                    std::unordered_set<const UniqueId *> &out)
{
    if (const auto *fd = dynamic_cast<const FuncDeclStmt *>(c)) {
        if (fd->id)
            out.insert(fd->id->uid);
    } else if (const auto *sd = dynamic_cast<const StructDeclStmt *>(c)) {
        if (sd->id)
            out.insert(sd->id->uid);
    } else if (const auto *e = dynamic_cast<const Expr14 *>(c)) {
        if (!(e->fl & pFlags::pInDecl))
            return;
        if (const auto *id = dynamic_cast<const Identifier *>(
                e->lvalue.get()))
            out.insert(id->uid);
        else if (const auto *il = dynamic_cast<const IdList *>(
                     e->lvalue.get()))
            for (const auto &x : il->elems)
                out.insert(x->uid);
    }
}

/* Width budget for the `=>` echo's pretty-printer: a value whose single-line
 * form is wider than this is expanded across lines. */
static const int REPL_PRETTY_WIDTH = 80;

/*
 * The persistent CONST context as it was before an input was parsed. The
 * parser commits to it while it parses - a const's value, a struct's
 * descriptor, a `pure func` - so an input REFUSED before it runs (a
 * syntax, type or optimizer error) must give those back, or the refused
 * declaration stays in force: a refused `pure func pf() => 2` was what a
 * later `pf()` folded to, a refused struct redefinition what a later
 * `S1 p` resolved as its type. A name the input bound is erased, and
 * every saved binding gets its saved value back IN PLACE, rebound or
 * not. Telling a rebound slot by its ADDRESS (the first version) failed
 * in an optimized build: a redefinition erases and re-binds the name,
 * and the allocator handed the freed map node straight back, so the
 * refused struct looked untouched ("'sp1' is declared 'S1' but is
 * assigned 'S1'" - rel-hard only; the debug build's ASan quarantine
 * never reuses an address that soon).
 */
struct ConstSnapshot {
    std::map<const UniqueId *, LValue> entries;

    explicit ConstSnapshot(const EvalContext &c) {
        std::vector<std::pair<const UniqueId *, const LValue *>> syms;
        c.collect_symbols(syms);
        for (const auto &kv : syms)
            entries.emplace(kv.first, *kv.second);
    }

    void restore(EvalContext &c) const {
        std::vector<std::pair<const UniqueId *, const LValue *>> syms;
        c.collect_symbols(syms);
        for (const auto &kv : syms)
            if (!entries.count(kv.first)) {
                Identifier id(kv.first->val);   /* bound by the input */
                c.erase(&id);
            }
        for (const auto &kv : entries) {
            if (LValue *cur = c.lookup(kv.first))
                *cur = kv.second;
            else
                c.emplace(kv.first, EvalValue(kv.second.get()),
                          kv.second.is_const_var());
        }
    }
};

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
    bool color = false;            /* set by ReplEngine::set_color */

    Impl()
        : const_ctx(new EvalContext(nullptr, /*const_ctx=*/true))
        , runtime_ctx(new EvalContext(nullptr, /*const_ctx=*/false,
                                      /*func_ctx=*/false, /*repl=*/true))
    {
        /* The prompt lets you redefine a function or struct (the edit-and-
         * resubmit workflow), unlike a script where a duplicate decl is an
         * error. Set on both scopes: structs / pure funcs are registered in the
         * const context at parse time, vars/funcs in the runtime one. (A plain
         * `var`'s TYPE still sticks - the inferencer's job, not this.) */
        runtime_ctx->allow_redeclare = true;
        const_ctx->allow_redeclare = true;
        /* a pure call the parser runs is type-inferred first (question 5) */
        const_ctx->parse_typed = true;
    }

    string format_error(const Exception &e) const {
        std::ostringstream o;
        format_exception(o, e, lines);
        return o.str();
    }

    /* Point an input's parser at the persistent const scope, and tell it
     * which const builtins' names an EARLIER input gave the program's own
     * function (or struct): the parser learns that from `func NAME` in its
     * token stream (ParseContext::shadowed, #133), which a later input's
     * stream does not hold - after `func abs(x) => 42`, a later `abs(-3)`
     * folded to the builtin's 3 while `abs(runtime(-3))` gave 42. */
    void bind_parser(ParseContext &pc) const {
        pc.const_ctx = const_ctx.get();
        std::vector<std::pair<const UniqueId *, const LValue *>> s;
        runtime_ctx->collect_symbols(s);
        /* the program's own binding is a function or a struct (a const
         * builtin cannot be rebound to anything else); a builtin's own
         * entry is a Builtin or a value - `inf`, `math_pi` */
        for (const auto &kv : s) {
            const EvalValue &v = kv.second->get();
            if (EvalContext::const_builtins.count(kv.first)
                    && (v.is<intrusive_ptr<FuncObject>>()
                        || v.is<StructTypeDef *>()))
                pc.shadow_add(kv.first);
        }
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

    /* Lex `source` in one pass (so a string / block comment can span lines);
     * token string_views point into `source`, which the caller keeps alive
     * through the parse. `local_lines` is filled too, for error carets. */
    static void lex_stable(const string &source,
                           std::vector<string> &local_lines,
                           std::vector<Tok> &toks) {
        std::stringstream ss(source);
        string line;
        while (std::getline(ss, line))
            local_lines.push_back(line);
        lexer(source, 1, toks);
    }
};

ReplEngine::ReplEngine() : impl(new Impl)
{
    /* The REPL is a DEV environment: dev-only builtins (show()) that need the
     * AST are allowed here (the REPL retains its input ASTs). A script leaves
     * this false, so a script call to show() is a compile-time error. Covers
     * both the interactive REPL and the headless repl: tests (both build a
     * ReplEngine). */
    g_dev_builtins_allowed = true;
}
ReplEngine::~ReplEngine() = default;

void
ReplEngine::set_color(bool on)
{
    impl->color = on;
}

/*
 * The command word of a `:` meta-command input (`quit` for ":quit  "), or
 * "" when the input is not one. The ONE reading of it: run_repl's exit
 * test and meta_command's dispatch both ask, so an input cannot run as
 * `:quit` and fail the exit test - the loop compared the whole input with
 * ":quit", so `:quit ` (a trailing space) neither exited nor did anything.
 */
static string
repl_meta_word(const string &input)
{
    const size_t s = input.find_first_not_of(" \t\r\n");
    if (s == string::npos || input[s] != ':')
        return "";
    string word;
    for (size_t i = s + 1;
         i < input.size() && !isspace(static_cast<unsigned char>(input[i]));
         i++)
        word += input[i];
    return word;
}

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
        case Op::lor:   case Op::band:  case Op::bor:   case Op::bxor:
        case Op::shl:   case Op::shr:   case Op::ushr:  case Op::assign:
        case Op::comma: case Op::dot:   case Op::arrow:
        case Op::colon: case Op::parenL: case Op::braceL: case Op::bracketL:
        case Op::semicolon:
            return true;
        default:
            /* every compound assignment, via the single map (operators.h) */
            return compound_assign_base(op) != Op::invalid;
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
 * after a dict literal), so it is added. The `;` goes where the line's CODE
 * ends - before a trailing `#` comment - and a block closed on the line of
 * its last statement (`if (c) { print(3) }`) gets that statement's `;`
 * before its `}`.
 */
/*
 * Find the "code" span [begin, end) of one physical line, given the running
 * in_string / in_comment state (updated in place). A string or block comment
 * may span lines: any part of the line inside one is excluded so the caller
 * lexes only real code (and never an unterminated token). Returns begin ==
 * npos when the whole line sits inside a string/comment. On return, in_string /
 * in_comment reflect the state AT END OF LINE (true => the line opens one that
 * does not close here). `#` starts a line comment (code ends there).
 */
static size_t
repl_scan_line(const string &line, bool &in_string, bool &in_comment,
               size_t &code_end)
{
    const size_t n = line.size();
    size_t i = 0, begin;

    if (in_comment) {
        while (i + 1 < n && !(line[i] == '*' && line[i + 1] == '/'))
            i++;
        if (i + 1 < n) { in_comment = false; i += 2; begin = i; }
        else { code_end = n; return string::npos; }   /* still in comment */
    } else if (in_string) {
        while (i < n) {
            if (line[i] == '\\') { i += 2; continue; }
            if (line[i] == '"') { in_string = false; i++; break; }
            i++;
        }
        if (in_string) { code_end = n; return string::npos; }  /* still open */
        begin = i;
    } else {
        begin = 0;
    }

    code_end = n;
    while (i < n) {
        const char c = line[i];
        if (c == '#') { code_end = i; break; }     /* line comment */
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(line[j] == '*' && line[j + 1] == '/'))
                j++;
            if (j + 1 < n) { i = j + 2; continue; }            /* closed */
            in_comment = true; code_end = i; break;            /* opens */
        }
        if (c == '"') {
            size_t j = i + 1;
            while (j < n) {
                if (line[j] == '\\') { j += 2; continue; }
                if (line[j] == '"') break;
                j++;
            }
            if (j < n) { i = j + 1; continue; }                /* closed */
            in_string = true; code_end = i; break;             /* opens */
        }
        i++;
    }
    return begin;
}

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
    /* the last code token ENDS a value no `;` terminates yet - so a block's
     * `}` right after it needs one (across lines too: a value on a line
     * that got no `;`, e.g. the end of a call spanning lines) */
    bool open_value = false;
    bool in_string = false;           /* a string spanning physical lines */
    bool in_comment = false;          /* a block comment spanning lines */

    /* the next line holding code starts with `else` / `catch` / `finally`:
     * the block a line closes is then not the statement's end (`}` on one
     * line, `else {` on the next, as a script may have it) */
    const auto next_continues = [&](size_t i) {
        for (size_t j = i + 1; j < phys.size(); j++) {
            std::vector<Tok> t;
            try {
                lexer(phys[j], 1, t);
            } catch (...) {
                return false;
            }
            if (!t.empty())
                return t[0] == Keyword::kw_else || t[0] == Keyword::kw_catch
                    || t[0] == Keyword::kw_finally;
        }
        return false;
    };

    for (size_t li = 0; li < phys.size(); li++) {
        const string &line = phys[li];

        /* A line wholly inside a multi-line string / comment is passed through
         * verbatim - never lex it (it is not code) or terminate it. */
        const bool was_in_string = in_string;
        size_t code_end = 0;
        const size_t code_begin =
            repl_scan_line(line, in_string, in_comment, code_end);
        if (code_begin == string::npos) {
            out += line;
            out += '\n';
            continue;
        }

        /* A multi-line string that CLOSED at the start of this line is itself a
         * value, so the statement can end here with no code token after. */
        const bool closed_string = was_in_string && !in_string;

        /* Lex only the code part (complete strings/comments, never an open one
         * - so the lexer can't throw on an unterminated token here). */
        const string code = line.substr(code_begin, code_end - code_begin);
        const bool open_at_end = in_string || in_comment;

        std::vector<Tok> toks;
        try {
            lexer(code, 1, toks);     /* only token kinds/ops are read */
        } catch (...) {
            /* a bad token: leave the line as-is and let the parser report it */
            out += line;
            out += '\n';
            continue;
        }

        /* Where a `;` goes in front of position `pos` of the line: right
         * after the last code character before it, so a `#` comment or the
         * spaces that follow the statement stay after its `;`. */
        const auto semi_before = [&](size_t pos) {
            while (pos > code_begin
                   && isspace(static_cast<unsigned char>(line[pos - 1])))
                pos--;
            return pos;
        };
        std::vector<size_t> semis;        /* `;` positions, left to right */

        if (closed_string)
            open_value = true;

        const Tok *last = nullptr;
        bool closed_block = false;        /* the last `}` closed a block */
        for (const auto &t : toks) {
            if (t == Op::braceL) {
                const char kind = expects_value ? 'D' : 'B';
                stack.push_back(kind);
                expects_value = (kind == 'D');
                open_value = false;
            } else if (t == Op::parenL) {
                stack.push_back('(');
                expects_value = true;
                open_value = false;
            } else if (t == Op::bracketL) {
                stack.push_back('[');
                expects_value = true;
                open_value = false;
            } else if (t == Op::braceR) {
                /* a block closing right after a statement on the SAME line
                 * (`if (c) { print(3) }`): that statement's `;` goes before
                 * the `}`, since no line end follows it */
                const char kind = stack.empty() ? 'B' : stack.back();
                if (kind == 'B' && open_value)
                    semis.push_back(semi_before(
                        code_begin + static_cast<size_t>(t.loc.col - 1)));
                if (!stack.empty())
                    stack.pop_back();
                expects_value = false;
                open_value = (kind == 'D');   /* a dict literal is a value */
                closed_block = (kind == 'B');
            } else if (t == Op::parenR || t == Op::bracketR) {
                if (!stack.empty())
                    stack.pop_back();
                expects_value = false;
                open_value = true;
            } else if (t == Keyword::kw_return) {
                expects_value = true;
                open_value = true;            /* a bare `return` ends too */
            } else {
                /* an operator that takes a RHS keeps us in value position */
                expects_value =
                    t.op != Op::invalid && repl_continuation_op(t.op);
                open_value = !expects_value;
            }
            last = &t;
        }

        const char ctx = stack.empty() ? 'B' : stack.back();
        const bool stmt_ctx = (ctx == 'B');     /* not 'D'/'('/'[' */
        /* The line ends in a value (so a `;` terminates the statement) when its
         * last code token isn't a continuation, or - with no code token - when
         * a multi-line string just closed here. No `;` when the line instead
         * opens a string/comment that continues onto the next line. The `;`
         * goes at the end of the CODE, never after a trailing `#` comment
         * (where it was part of the comment, and the statement went on into
         * the next line). */
        const bool ends_in_value =
            last ? !repl_continuation_op(last->op) : closed_string;
        const bool chain_goes_on = last && *last == Op::braceR
            && closed_block && next_continues(li);
        if (!open_at_end && stmt_ctx && ends_in_value && !chain_goes_on) {
            semis.push_back(semi_before(code_end));
            open_value = false;
        }

        size_t from = 0;
        for (const size_t at : semis) {
            out.append(line, from, at - from);
            out += ';';
            from = at;
        }
        out.append(line, from, string::npos);
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

    /* 2. Lex the whole input in one pass (so a string / block comment can span
     *    lines), then parse against the PERSISTENT const context (no pushed
     *    top-level scope, so new consts/pure-funcs/structs survive to the next
     *    input). The lexer can throw (a bad token like `2_`, or an unterminated
     *    string), so it runs inside the try: a lex error is reported like any
     *    other and rejects only this input, never the session. `source` (the
     *    auto-terminated input, which already holds these lines joined by '\n')
     *    outlives parsing, so a token / error string_view into it stays valid;
     *    the caret line itself comes from the persistent `lines`. The first
     *    line is number base + 1, continuing across inputs. */
    std::vector<Tok> tokens;
    unique_ptr<Construct> root;
    const ConstSnapshot const_before(*const_ctx);
    try {
        lexer(source, static_cast<int>(base + 1), tokens);

        if (tokens.empty())
            return "";

        ParseContext pc(TokenStream(tokens), true);
        bind_parser(pc);
        root = pBlock(pc, 0, /*push_const_scope=*/false);
        if (!pc.eoi())
            throw SyntaxErrorEx(Loc(pc.get_tok().loc),
                                "Unexpected token at the end", &pc.get_tok());
    } catch (const SyntaxErrorEx &caught) {
        const_before.restore(*const_ctx);
        /* An error at the END of the input carries the EOF sentinel, which
         * has no location: point it past the last token, as main() does
         * for a script. (It rendered "SyntaxError" with no line at all.) */
        SyntaxErrorEx e = caught;
        if (e.tok == &invalid_tok && !tokens.empty()) {
            e.loc_start = tokens.back().loc + 1;
            e.loc_end = tokens.back().loc + 2;
        }
        return format_error(e);
    } catch (const Exception &e) {
        const_before.restore(*const_ctx);
        return format_error(e);
    }

    /* 2a-bis. Implicit top-level `var`: a bare `name = expr` to a name not
     *     already a committed global (or builtin) is a declaration. Seed the
     *     "known" set with this REPL's prior globals (runtime + const scopes)
     *     so a later `a = 2` re-targets the existing global instead of
     *     re-declaring it. */
    {
        std::unordered_set<const UniqueId *> known;
        std::vector<std::pair<const UniqueId *, const LValue *>> prior;
        runtime_ctx->collect_symbols(prior);
        for (auto &kv : prior)
            known.insert(kv.first);
        prior.clear();
        const_ctx->collect_symbols(prior);
        for (auto &kv : prior)
            known.insert(kv.first);
        mark_implicit_globals(root.get(), known);
    }

    /* 2b. Type-check this input against the committed globals (faithful
     *     incremental inference). A type error rejects just this input - report
     *     it and commit nothing (no eval, no retain). On success the inferencer
     *     has pinned this input's new globals for the next input, and stamped
     *     ArrHints/TypeHints on the tree the evaluator below reads. */
    try {
        infer.check_input(root.get());
    } catch (const Exception &e) {
        const_before.restore(*const_ctx);
        retained.push_back(std::move(root));   /* keep the AST alive (id_sym refs) */
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
     * seeded for cross-input fold / inline / specialize.
     *
     * It can RAISE: a constant call is evaluated here (`h(0)` with
     * `func h(x) => 10 / x` folds to a division by zero - a script's
     * compile error). That refuses the input like a type error does:
     * report it, and take back what the check above committed (its
     * globals, its template instances), since nothing of it ran. Uncaught,
     * it ended the session - and, for a thrown struct, was rendered by
     * main() after ~ReplEngine had freed the struct's definition (a
     * use-after-free). */
    try {
        run_optimizers(root.get(), /*enable_inline=*/true,
                       /*inline_threshold=*/24, /*enable_specialize=*/true,
                       /*repl_mode=*/true,
                       /*prior_scope=*/runtime_ctx.get());
    } catch (const Exception &e) {
        infer.reject_input();
        const_before.restore(*const_ctx);
        retained.push_back(std::move(root));   /* keep the AST alive */
        return out.str() + format_error(e);
    }

    /* 3. Evaluate each top-level statement directly in the persistent global
     *    scope, capturing print() output and the last statement's value. */
    Block *blk = dynamic_cast<Block *>(root.get());
    EvalValue last;
    const Construct *last_elem = nullptr;   /* the last top-level statement */
    size_t ran = 0;                         /* statements that completed */
    const std::vector<const UniqueId *> names_before = global_names();
    std::streambuf *old_cout = std::cout.rdbuf(out.rdbuf());

    /*
     * The input's "main" FRAME. The REPL's own globals live in the map, so
     * the top level declares no slot - but the inliner may give it some: a
     * call inlined into a top-level statement takes its callee's locals and
     * its argument temps (`$a0 = k` for `rt(k)`) as new slots of main's
     * frame (root->slot_count), exactly as in a script, whose root
     * Block::do_eval builds that frame. The statements below run straight in
     * the persistent scope, so build it here, for this input's run only.
     * Without it such a call read "Undefined variable '$a0'".
     */
    std::unique_ptr<Frame> frame;
    Frame *const prev_frame = runtime_ctx->frame;
    if (blk && blk->slot_count) {
        frame = std::make_unique<Frame>();
        frame->init(blk->slot_count);
        runtime_ctx->frame = frame.get();
    }

    try {
        if (blk) {
            for (const auto &e : blk->elems) {
                EvalValue v = e->eval(runtime_ctx.get());
                if (v.is<UndefinedId>())
                    throw UndefinedVariableEx(
                        v.get<UndefinedId>().id, e->start, e->end);
                last = std::move(v);
                last_elem = e.get();
                ran++;
            }
        }
    } catch (const Exception &e) {
        runtime_ctx->frame = prev_frame;
        std::cout.rdbuf(old_cout);
        /* the statements that completed keep what they defined; the
         * declarations the run never reached bound nothing, so their
         * committed types go back */
        std::unordered_set<const UniqueId *> bound;
        for (size_t i = 0; i < ran; i++)
            repl_declared_names(blk->elems[i].get(), bound);
        infer.forget_unbound([&bound](const UniqueId *n) {
            return bound.count(n) != 0;
        });
        retained.push_back(std::move(root));   /* keep partial defs alive */
        out << format_error(e);
        return out.str();
    }

    runtime_ctx->frame = prev_frame;
    std::cout.rdbuf(old_cout);
    retained.push_back(std::move(root));

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
        if (!r.is<NoneVal>() || repl_echo_none(last_elem, tokens)) {
            /* Pretty-print: a container too wide for one line expands across
             * lines, indented (structs/dicts/arrays, recursively). Colored
             * line-by-line via the highlighter when color is on. */
            const string p = r.pretty(3, REPL_PRETTY_WIDTH);
            out << "=> " << show_colorize(p, color) << "\n";
        }
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
    const string cmd = repl_meta_word(src);
    string arg;
    {
        size_t i = 1 + cmd.size();          /* past the ':' and the word */
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
    if (cmd == "help" || cmd == "h")
        return repl_help(arg, color);
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

    try {
        lexer(source, 1, toks);   /* one pass; views point into `source` */
        ParseContext pc(TokenStream(toks), true);
        bind_parser(pc);
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
    try {
        lex_stable(source, local_lines, toks);
        AnalysisInfo info;
        ParseContext pc(TokenStream(toks), true);
        bind_parser(pc);
        pc.analysis = &info;        /* record parse-time folds / dead code */
        unique_ptr<Construct> root = pBlock(pc, 0, /*push_const_scope=*/true);

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

    /*
     * A function's row: its declared signature - except for a CLONE, whose
     * row says what it is FOR (README: `:globals` shows what each clone
     * specializes on): a TEMPLATE INSTANCE (`f$0`) each parameter with its
     * inferred type, and the return type; a const-argument SPECIALIZATION
     * (`f$s0`) the values it was built for (`with mode = 1`). Both printed
     * the base's generic signature, `f$0 : func f(a, int b)`.
     */
    auto func_row = [&](const FuncDescriptor *d) -> string {
        const FuncDeclStmt *decl = d->decl;   /* the REPL retains its ASTs */
        if (decl && !decl->spec_args.empty())
            return reflect_func_sig(d) + decl->spec_args_str();
        const std::vector<string> pt =
            !d->display_name.empty() && decl ? infer.func_param_types(decl)
                                             : std::vector<string>();
        if (pt.size() != d->params.size() || pt.empty())
            return reflect_func_sig(d);
        string s = d->explicit_pure ? "pure func " : "func ";
        s += d->display_name + "(";
        for (size_t i = 0; i < pt.size(); i++) {
            if (i)
                s += ", ";
            if (d->params[i].cnst)
                s += "const ";
            s += pt[i] + " " + string(d->params[i].name->val);
        }
        s += ")";
        const string ret = infer.func_return_type(decl);
        if (!ret.empty())
            s += " -> " + ret;
        return s;
    };

    auto classify = [&](const UniqueId *uid, const EvalValue &v,
                        bool is_const) -> Row {
        Row r;
        r.name = string(uid->val);
        if (v.is<intrusive_ptr<FuncObject>>()) {
            r.kind = "func";
            r.type = func_row(v.get<intrusive_ptr<FuncObject>>()->func);
        } else if (v.is<StructTypeDef *>()) {
            r.kind = v.get<StructTypeDef *>()->is_class ? "class type"
                                                        : "struct type";
            r.type = reflect_struct_ctor(v.get<StructTypeDef *>());
        } else if (v.is<intrusive_ptr<StructObject>>()) {
            r.kind = is_const ? "const" : "var";
            /* the struct's or class's name - or box<P> for a box */
            r.type = reflect_typeof(v);
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

    /* the const context first: a const scalar is folded out of the
     * run-time scope, and one a closure captured has a run-time `var` of
     * its name there (the capture's source) - the name is still a const */
    std::vector<std::pair<const UniqueId *, const LValue *>> csyms;
    const_ctx->collect_symbols(csyms);
    for (const auto &kv : csyms) {
        if (EvalContext::const_builtins.count(kv.first))
            continue;
        seen.insert(kv.first);
        rows.push_back(classify(kv.first, kv.second->get(), true));
    }

    std::vector<std::pair<const UniqueId *, const LValue *>> rsyms;
    runtime_ctx->collect_symbols(rsyms);
    for (const auto &kv : rsyms) {
        if (EvalContext::const_builtins.count(kv.first) ||
            EvalContext::builtins.count(kv.first) || seen.count(kv.first))
            continue;
        rows.push_back(classify(kv.first, kv.second->get(),
                                kv.second->is_const_var()));
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
    try {
        lex_stable(source, local_lines, toks);
        ParseContext pc(TokenStream(toks), true);
        bind_parser(pc);
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

    std::vector<std::pair<const UniqueId *, const LValue *>> syms;
    runtime_ctx->collect_symbols(syms);

    auto func_of = [](const LValue *lv) -> const FuncDeclStmt * {
        const EvalValue &v = lv->get();
        /* desc->decl: the REPL retains its input ASTs, so a committed
         * function's decl is always alive here. */
        return v.is<intrusive_ptr<FuncObject>>()
                   ? v.get<intrusive_ptr<FuncObject>>()->func->decl
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
            if (g && g != base && g->desc->display_name == arg)
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
    try {
        lex_stable(source, local_lines, toks);
        ParseContext pc(TokenStream(toks), true);
        bind_parser(pc);
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
            if (fd->id && fd->desc->display_name.empty() &&
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
        if (!v.is<intrusive_ptr<FuncObject>>())
            continue;
        const FuncDeclStmt *fd =
            v.get<intrusive_ptr<FuncObject>>()->func->decl;
        if (fd->desc->display_name.empty()
                || !redefined.count(fd->desc->display_name))
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
    try {
        lexer(src, 1, toks);          /* whole-buffer: strings/comments span */
    } catch (const InvalidTokenEx &e) {
        /* An UNTERMINATED string / block comment means the input is still being
         * written - keep the editor open for more lines. Any other bad token
         * (e.g. `2_`) is a definitive error: report it complete so the editor
         * submits it and do_eval surfaces the error. */
        return e.unterminated;
    } catch (const Exception &) {
        return false;
    }

    if (toks.empty())
        return false;

    /* Net unclosed (){}/[] depth (>0 wants more). A string's / comment's own
     * brackets aren't here - they're inside a single string token / skipped. */
    int depth = 0;
    for (const auto &t : toks) {
        if (t == Op::parenL || t == Op::braceL || t == Op::bracketL)
            depth++;
        else if (t == Op::parenR || t == Op::braceR || t == Op::bracketR)
            depth--;
    }
    if (depth > 0)
        return true;

    /* A line ending on a binary/continuation operator wants more. The
     * compound assignments answer through compound_assign_base (the single
     * map, operators.h) so a new one joins here with no edit - this switch
     * was a second hand-copy of the set and had silently missed even `+=`
     * (a trailing `x +=` used to submit and error). */
    if (compound_assign_base(toks.back().op) != Op::invalid)
        return true;
    switch (toks.back().op) {
        case Op::plus:    case Op::minus:   case Op::times:
        case Op::div:     case Op::mod:     case Op::lt:
        case Op::gt:      case Op::le:      case Op::ge:
        case Op::eq:      case Op::noteq:   case Op::land:
        case Op::lor:     case Op::band:    case Op::bor:
        case Op::bxor:    case Op::shl:     case Op::shr:
        case Op::ushr:    case Op::assign:  case Op::comma:
        case Op::dot:     case Op::arrow:
            return true;
        default:
            return false;
    }
}

bool
ReplEngine::continues_chain(const string &line)
{
    std::vector<Tok> toks;
    try {
        lexer(line, 1, toks);
    } catch (const Exception &) {
        return false;
    }
    return !toks.empty()
        && (toks[0] == Keyword::kw_else || toks[0] == Keyword::kw_catch
            || toks[0] == Keyword::kw_finally);
}

bool
ReplEngine::may_continue(const string &src, bool multiline_only)
{
    std::vector<Tok> toks;
    try {
        lexer(src, 1, toks);
    } catch (const Exception &) {
        return false;
    }
    if (toks.empty() || !(toks.back() == Op::braceR))
        return false;

    /* the `{` the final `}` closes, and what stands before it */
    const auto open_of = [&](size_t close, Op l, Op r) -> size_t {
        int depth = 0;
        for (size_t i = close + 1; i-- > 0;) {
            if (toks[i] == r)
                depth++;
            else if (toks[i] == l && --depth == 0)
                return i;
        }
        return string::npos;
    };
    const size_t ob = open_of(toks.size() - 1, Op::braceL, Op::braceR);
    if (ob == string::npos || ob == 0)
        return false;
    if (multiline_only && toks[ob].loc.line == toks.back().loc.line)
        return false;

    const Tok &before = toks[ob - 1];
    if (before == Keyword::kw_try || before == Keyword::kw_catch)
        return true;                        /* `try {` / `catch {` */
    if (!(before == Op::parenR))
        return false;                       /* `else {`, `finally {`, ... */
    const size_t op = open_of(ob - 1, Op::parenL, Op::parenR);
    if (op == string::npos || op == 0)
        return false;
    return toks[op - 1] == Keyword::kw_if          /* if / else if */
        || toks[op - 1] == Keyword::kw_catch;      /* catch (T as e) */
}

/* ------------------------------------------------------------------ */
/* The interactive loop (raw-mode line editor; see lineedit.cpp).       */
/* ------------------------------------------------------------------ */

/* The persisted history file path ($HOME/.mylang_history), or "" if no HOME. */
static string
history_path()
{
    const std::optional<string> home = env_get("HOME");
    return home ? *home + "/.mylang_history" : string();
}

static const size_t HISTORY_CAP = 2000;

/*
 * An entry is one INPUT, which may span lines (a block, a paste): written
 * one per line, its newlines as `\n`, its carriage returns as `\r` and its
 * backslashes as `\\`. Written raw, a block came back after a restart as
 * one entry per line (Up recalled a lone `}`), and an empty line inside it
 * was lost. The file is written in BINARY, so it holds the same bytes on
 * every platform (Windows' text mode wrote each line end as CRLF), and a
 * line read back drops a trailing CR, so a file an editor saved with CRLF
 * line ends still loads - no entry ends in a raw CR, since it is escaped.
 */
static const char *const HISTORY_HEADER = "#mylang-history 2";

static string
history_encode(const string &e)
{
    string out;
    for (const char c : e) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += c;
    }
    return out;
}

static string
history_decode(const string &l)
{
    string out;
    for (size_t i = 0; i < l.size(); i++) {
        if (l[i] == '\\' && i + 1 < l.size()) {
            i++;
            out += l[i] == 'n' ? '\n' : l[i] == 'r' ? '\r' : l[i];
        } else {
            out += l[i];
        }
    }
    return out;
}

void
repl_load_history(const string &path, std::vector<string> &history)
{
    std::ifstream f(path, std::ios::binary);
    string line;
    const auto next_line = [&]() {
        if (!std::getline(f, line))
            return false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return true;
    };
    /* the first line names the format: a file written before entries were
     * escaped (repl_save_history) has one entry per line, as written */
    bool escaped = false;
    if (next_line()) {
        if (line == HISTORY_HEADER)
            escaped = true;
        else if (!line.empty())
            history.push_back(line);
    }
    while (next_line())
        if (!line.empty())
            history.push_back(escaped ? history_decode(line) : line);
    if (history.size() > HISTORY_CAP)
        history.erase(history.begin(), history.end() - HISTORY_CAP);
}

void
repl_save_history(const string &path, const std::vector<string> &history)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return;
    const size_t from =
        history.size() > HISTORY_CAP ? history.size() - HISTORY_CAP : 0;
    f << HISTORY_HEADER << "\n";
    for (size_t i = from; i < history.size(); i++)
        f << history_encode(history[i]) << "\n";
}

int
run_repl()
{
    ReplEngine engine;
    std::vector<string> history;
    const string hpath = history_path();
    if (!hpath.empty())
        repl_load_history(hpath, history);

    /* Colors on a TTY unless NO_COLOR is set (https://no-color.org). */
    const bool color = repl_out_is_tty() && !env_get("NO_COLOR");
    engine.set_color(color);
    set_highlight_enabled(color);
    trace_set_color(color);
    /* the cast selects the cross-line (stateful) highlight overload */
    string (*hl)(const string &, int &) =
        color ? static_cast<string (*)(const string &, int &)>(highlight_line)
              : nullptr;

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

    /* An `if` / `try` chain whose last block closes over several lines
     * stays open for an `else` / `catch` / `finally` on the next line, as
     * in a script: an empty line (Enter) runs it, and so does any other
     * line, which then runs with it (Python's REPL asks the same blank
     * line after a block). A chain closed on one line runs at once. */
    LineEditor::Joiner joiner = [](const string &s, const string *next) {
        const size_t i = s.find_first_not_of(" \t\r\n");
        if (i != string::npos && s[i] == ':')
            return false;
        if (!next)
            return ReplEngine::may_continue(s, true);
        return ReplEngine::continues_chain(*next);
    };

    std::cout << "MyLang REPL. :quit (or Ctrl-D) to exit, :help for help.\n";

    while (true) {

        ReadLineResult r = read_line(">> ", ".. ", history, hl, completer,
                                     submitter, joiner);

        if (r.eof)
            break;                          /* Ctrl-D at the prompt exits */
        if (r.cancelled)
            continue;                       /* Ctrl-C drops the input */

        const string &input = r.line;       /* may span multiple lines */
        if (input.find_first_not_of(" \t\r\n") == string::npos)
            continue;                       /* blank */

        /* A leading ':' meta-command (`:quit` exits; others run via eval_input,
         * which dispatches them). The whole input is one history entry. */
        const string word = repl_meta_word(input);
        if (word == "quit" || word == "q")
            break;

        history.push_back(input);

        std::cout << engine.eval_input(input);
    }

    if (!hpath.empty())
        repl_save_history(hpath, history);
    return 0;
}
