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
#include "trace.h"

#include <initializer_list>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#ifndef _WIN32
#include <unistd.h>   /* isatty (REPL launch; Unix-only) */
#endif

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
    cout << "  -T CATS  Trace the compiler's reasoning to stderr (--trace);"
         << endl;
    cout << "           CATS is comma-separated: infer,inline,specialize,"
         << endl;
    cout << "           template,autoconst,autopure,arrays,fold, or all"
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
         * like); otherwise print help. The REPL is Unix-only (see plans/
         * repl.md), so on Windows the no-args case just prints help. */
#ifndef _WIN32
        if (isatty(STDIN_FILENO)) {
            opt_repl = true;
            return;
        }
#endif

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

        } else if (!strcmp(arg, "-T") || !strcmp(arg, "--trace")) {

            /* Enable diagnostic trace categories (comma-separated, or "all")
             * BEFORE compilation, so a script run narrates the optimizer
             * reasoning to stderr. See trace.h / plans/repl-introspection. */
            if (argc < 2) {
                cout << "error: --trace requires a category list "
                        "(e.g. infer,inline or all)" << endl;
                exit(1);
            }
            const string cats = argv[1];
            argc--; argv++;   /* consume the value */
            size_t pos = 0;
            while (pos <= cats.size()) {
                const size_t comma = cats.find(',', pos);
                const size_t end =
                    (comma == string::npos) ? cats.size() : comma;
                const string c = cats.substr(pos, end - pos);
                if (!c.empty() && !trace_set(c, true))
                    cout << "warning: unknown trace category '" << c << "'"
                         << endl;
                if (comma == string::npos)
                    break;
                pos = comma + 1;
            }

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

/* anno_code + render_analysis now live in analyzer.cpp (shared with the REPL's
 * :analyze meta-command). */

int main(int argc, char **argv)
{
    /*
     * The AST owns every StructTypeDef, and an uncaught struct exception's
     * payload references its def (to print field names). So `root` must outlive
     * the catch handlers below: declared here, not inside the try, or unwinding
     * would free the def before format_exception() renders the value.
     */
    unique_ptr<Construct> root;

    try {

        parse_args(argc, argv);

        /* Color the trace tags on a stderr TTY (the trace sink is stderr for a
         * script), unless --no-color. Harmless when no category is enabled. */
#ifndef _WIN32
        trace_set_color(!opt_no_color && isatty(STDERR_FILENO));
#endif

        if (opt_repl)
            return run_repl();

        ParseContext ctx(TokenStream(tokens), !opt_no_const_eval);

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
            /* analyze_info already holds the parser's records; the shared
             * pipeline adds the inference (array storage) and resolver
             * (auto-const/inline/etc.) decisions, then reprints colored. */
            analyze_and_render(cout, root.get(), analyze_info, lines,
                               !opt_no_color, /*repl_mode=*/false,
                               !opt_no_inline, opt_inline_threshold);
            return 0;
        }

        /* Static type inference + checking (compile-time). Runs before
         * resolve_names, on the clean source tree. A type violation throws a
         * compile-time exception here. Validation-only (-nr) still runs it. */
        infer_types(root.get(), !opt_no_type_infer);

        if (!opt_no_run) {
            /* Run the optimizer pipeline (resolve_names + specialize_types -
             * the SAME helper the REPL uses), then run the script. The root
             * block builds its own "main" Frame for slotted top-level vars. */
            run_optimizers(root.get(), !opt_no_inline, opt_inline_threshold,
                           !opt_no_type_infer);
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
