/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"
#include "resolver.h"
#include "backtrace.h"
#include "inferencer.h"
#include "analyzer.h"
#include "repl.h"
#include "errfmt.h"

#include <initializer_list>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <unistd.h>   /* isatty */

using std::string;

static bool opt_show_tokens;
static bool opt_show_syntax_tree;
static bool opt_no_const_eval;
static bool opt_no_inline;
static int opt_inline_threshold = 24;  /* max inlined body size (nodes) */
static bool opt_no_run;
static bool opt_no_type_infer;
static bool opt_debug_ti;
static bool opt_analyze;
static bool opt_no_color;
static bool opt_repl;

static std::vector<string> lines;
static std::vector<Tok> tokens;

void run_tests(bool dump_syntax_tree);

void help()
{
    cout << "Syntax:" << endl;
    cout << "   mylang [-t] [-s] [-nc] FILE | -e EXPR" << endl;
    cout << endl;
    cout << "   -t      Show all tokens" << endl;
    cout << "   -s      Dump the syntax tree" << endl;
    cout << "  -nc      No const eval (debug)" << endl;
    cout << "  -ni      No function inlining (debug)" << endl;
    cout << "  -it N    Inline threshold: max inlined body size (default 24)"
         << endl;
    cout << "  -nr      Don't run, just validate" << endl;
    cout << " -nti      No type inference / checking (debug)" << endl;
    cout << " --debug-ti  Dump inferred types of all identifiers, then exit"
         << endl;
    cout << "  -a       Analyze: reprint the source with colors showing which"
         << endl;
    cout << "           optimizations fired (--analyze; --no-color for plain)"
         << endl;

#ifdef TESTS
    cout << "  -rt      Run unit tests" << endl;
#endif
}

void
read_script(const char *filename)
{
    {
        string line;
        std::ifstream filestream(filename);

        if (filestream.is_open()) {

            while (getline(filestream, line)) {
                lines.push_back(move(line));
                line.clear(); /* Put the string is a known state */
            }

        } else {

            cout << "Failed to open file '" << filename << "'\n";
            exit(1);
        }
    }

    for (size_t i = 0; i < lines.size(); i++)
        lexer(lines[i], static_cast<int>(i+1), tokens);
}

void
parse_args(int argc, char **argv)
{
    string inline_text;
    bool in_tokens = false;

    if (!argc) {

        /* That should *never* happen */
        cout << "Unexpected (system) error: zero arguments" << endl;
        exit(1);

    } else if (argc == 1) {

        /* No FILE / -e: drop into the interactive REPL on a terminal (Ruby-
         * like); otherwise print help. */
        if (isatty(STDIN_FILENO)) {
            opt_repl = true;
            return;
        }

        help();
        exit(0);
    }

    for (argc--, argv++; argc > 0; argc--, argv++) {

        char *arg = argv[0];

        if (in_tokens) {
            /*
             * Join multiple arguments after -e with a space, so that an
             * unquoted expression split by the shell (e.g. -e var x = 5)
             * is not glued back into a single mangled token (varx=5).
             */
            if (!inline_text.empty())
                inline_text += ' ';
            inline_text += arg;
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {

            help(); exit(0);

        } else if (!strcmp(arg, "-rt")) {

            run_tests(opt_show_syntax_tree); exit(0);

        } else if (!strcmp(arg, "-t")) {

            opt_show_tokens = true;

        } else if (!strcmp(arg, "-s")) {

            opt_show_syntax_tree = true;

        } else if (!strcmp(arg, "-nc")) {

            opt_no_const_eval = true;

        } else if (!strcmp(arg, "-ni")) {

            opt_no_inline = true;

        } else if (!strcmp(arg, "-it")) {

            if (argc < 2) {
                cout << "error: -it requires a value (max inlined body size)"
                     << endl;
                exit(1);
            }

            opt_inline_threshold = atoi(argv[1]);
            argc--; argv++;   /* consume the value; the loop skips it too */

        } else if (!strcmp(arg, "-nr")) {

            opt_no_run = true;

        } else if (!strcmp(arg, "-nti")) {

            opt_no_type_infer = true;

        } else if (!strcmp(arg, "--debug-ti")) {

            opt_debug_ti = true;

        } else if (!strcmp(arg, "-a") || !strcmp(arg, "--analyze")) {

            opt_analyze = true;

        } else if (!strcmp(arg, "--no-color")) {

            opt_no_color = true;

        } else if (!strcmp(arg, "--repl")) {

            opt_repl = true;   /* force the REPL even off a TTY (for testing) */
            return;

        } else if (!strcmp(arg, "-e")) {

            if (argc > 1) {
                in_tokens = true;
                continue;
            }

            help(); exit(1);

        } else {

            read_script(arg);

            SharedArrayObj::vec_type vec;

            for (int i = 1; i < argc; i++)
                vec.emplace_back(SharedStr(string(argv[i])), false);

            EvalContext::builtins.emplace(

                make_pair(
                    UniqueId::get("argv"),
                    LValue(
                        EvalValue(SharedArrayObj(move(vec))),
                        false
                    )
                )
            );

            break;
        }
    }

    if (in_tokens) {
        lines.emplace_back(move(inline_text));
        lexer(lines[0], 1, tokens);
    }
}

/* ANSI color escape for an annotation kind (see analyzer.h's legend). */
static const char *
anno_code(AnnoKind k)
{
    switch (k) {
        case AnnoKind::auto_const:  return "\033[33m";   /* yellow  */
        case AnnoKind::flat_array:  return "\033[32m";   /* green   */
        case AnnoKind::dyn_array:   return "\033[31m";   /* red     */
        case AnnoKind::inlined:     return "\033[94m";   /* blue    */
        case AnnoKind::specialized: return "\033[36m";   /* cyan    */
        case AnnoKind::folded:      return "\033[35m";   /* magenta */
        default:                    return "";
    }
}

/*
 * -a/--analyze: reprint the source verbatim, coloring each annotated identifier
 * / call-site and dimming dead (DCE'd) code. Rendering is column-based over the
 * raw source line so spacing and comments are preserved exactly; a dead range
 * dims its columns (and wins over a color span, since dead code won't run).
 */
static void
render_analysis(const std::vector<string> &src, const AnalysisInfo &info,
                bool color)
{
    static const char *const RESET = "\033[0m";
    static const char *const DIM = "\033[2m";

    if (color) {
        cout << "Legend: "
             << "\033[33mauto-const/pure\033[0m  "
             << "\033[32mflat array\033[0m  "
             << "\033[31mdyn array\033[0m  "
             << "\033[94minlined\033[0m  "
             << "\033[36mspecialized\033[0m  "
             << "\033[35mfolded call\033[0m  "
             << DIM << "dead code" << RESET << "\n";
        cout << "--------------------------\n";
    }

    for (size_t li = 0; li < src.size(); li++) {

        const int line = static_cast<int>(li) + 1;
        const string &text = src[li];
        const int n = static_cast<int>(text.length());

        if (!color) {
            cout << text << "\n";
            continue;
        }

        /* per-column state: -1 default, -2 dim, else (int)AnnoKind color. */
        std::vector<int> attr(n, -1);

        for (const auto &d : info.dead) {
            if (line < d.l1 || line > d.l2)
                continue;
            const int from = (line == d.l1) ? d.c1 : 1;
            const int to   = (line == d.l2) ? d.c2 : n;
            for (int c = from; c <= to && c <= n; c++)
                if (c >= 1)
                    attr[c - 1] = -2;
        }

        /* color spans override dim (an annotated identifier still shows) */
        auto lo = info.spans.lower_bound({line, 0});
        auto hi = info.spans.lower_bound({line + 1, 0});
        for (auto it = lo; it != hi; ++it) {
            const int col = it->first.second;
            const int len = it->second.len;
            for (int c = col; c < col + len && c <= n; c++)
                if (c >= 1)
                    attr[c - 1] = static_cast<int>(it->second.kind);
        }

        string out;
        int cur = -1;   /* currently-open attribute (-1 == default/none open) */

        for (int c = 0; c < n; c++) {
            if (attr[c] != cur) {
                if (cur != -1)
                    out += RESET;
                if (attr[c] == -2)
                    out += DIM;
                else if (attr[c] != -1)
                    out += anno_code(static_cast<AnnoKind>(attr[c]));
                cur = attr[c];
            }
            out += text[c];
        }
        if (cur != -1)
            out += RESET;

        cout << out << "\n";
    }
}

int main(int argc, char **argv)
{
    try {

        parse_args(argc, argv);

        if (opt_repl)
            return run_repl();

        ParseContext ctx(TokenStream(tokens), !opt_no_const_eval);
        unique_ptr<Construct> root;

        /* -a: the parser records parse-time folds/DCE it would otherwise erase
         * (magenta folded calls, dim dead branches) into this collector; the
         * later passes add to it. Set before pBlock so the parser records. */
        AnalysisInfo analyze_info;
        if (opt_analyze)
            ctx.analysis = &analyze_info;

        if (opt_show_tokens) {
            cout << "Tokens" << endl;
            cout << "--------------------------" << endl;

            for (const auto &tok : tokens) {
                cout << tok << endl;
            }

            cout << endl;
        }

        root = pBlock(ctx);

        if (opt_show_syntax_tree) {
            cout << "Syntax tree" << endl;
            cout << "--------------------------" << endl;
            cout << *root << endl;
            cout << "--------------------------" << endl;
        }

        if (!ctx.eoi())
            throw SyntaxErrorEx(
                Loc(ctx.get_tok().loc),
                "Unexpected token at the end",
                &ctx.get_tok()
            );

        /* --debug-ti: dump the inferred type of every identifier + its use
         * sites (machine-readable) and exit, without running. */
        if (opt_debug_ti) {
            dump_type_info(root.get(), cout);
            return 0;
        }

        /* -a/--analyze: collect optimization decisions and reprint the source
         * with colors, then exit. Array-storage colors come from inference (on
         * the clean tree); the resolver passes run next and record auto-const /
         * dead-code / inlined / specialized / folded as the tree mutates. */
        if (opt_analyze) {
            /* analyze_info already holds the parser's records; add the
             * inference (array storage) and resolver (auto-const/inline/etc.)
             * decisions, then reprint the source colored. */
            collect_array_analysis(root.get(), analyze_info);
            resolve_names(root.get(), !opt_no_inline, opt_inline_threshold,
                          &analyze_info);
            collect_resolver_analysis(root.get(), analyze_info);
            render_analysis(lines, analyze_info, !opt_no_color);
            return 0;
        }

        /* Static type inference + checking (compile-time). Runs before
         * resolve_names, on the clean source tree. A type violation throws a
         * compile-time exception here. Validation-only (-nr) still runs it. */
        infer_types(root.get(), !opt_no_type_infer);

        if (!opt_no_run) {
            /* Resolve names to slots, then run the script. The root block
             * builds its own "main" Frame for slotted top-level variables. */
            resolve_names(root.get(), !opt_no_inline, opt_inline_threshold);
            specialize_types(root.get(), !opt_no_type_infer);
            root->eval(nullptr);
        }

    } catch (const SyntaxErrorEx &caught) {

        /* An "unexpected EOF" syntax error carries the EOF sentinel; point it
         * just past the last real token so the caret lands at end-of-input.
         * (Copy first - loc adjusts, the other fields are const.) */
        SyntaxErrorEx e = caught;
        if (e.tok == &invalid_tok && !tokens.empty()) {
            e.loc_start = tokens.back().loc + 1;
            e.loc_end = tokens.back().loc + 2;
        }

        format_exception(cerr, e, lines);
        return 1;

    } catch (const Exception &e) {

        format_exception(cerr, e, lines);
        return 1;
    }

    return 0;
}
