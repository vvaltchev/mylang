/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"
#include "resolver.h"
#include "backtrace.h"
#include "inferencer.h"

#include <initializer_list>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cctype>

using std::string;

static bool opt_show_tokens;
static bool opt_show_syntax_tree;
static bool opt_no_const_eval;
static bool opt_no_inline;
static int opt_inline_threshold = 24;  /* max inlined body size (nodes) */
static bool opt_no_run;
static bool opt_no_type_infer;

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

/*
 * Print one source line with a caret row underneath marking columns [from, to]
 * (1-based, inclusive). Leading whitespace is reproduced verbatim so tabs line
 * up. `to == 0` means "to the end of the line".
 */
static void
dumpLineWithCaret(const string &ln, int from, int to)
{
    if (from < 1)
        from = 1;
    if (to == 0 || to > static_cast<int>(ln.length()))
        to = static_cast<int>(ln.length());

    cerr << "    " << ln << endl << "    ";

    for (int i = 1; i < from; i++)
        cerr << (i - 1 < static_cast<int>(ln.length()) && isspace(ln[i - 1])
                     ? ln[i - 1] : ' ');

    cerr << string(std::max(1, to - from + 1), '^') << endl;
}

static void
dumpLocInError(const Exception &e)
{
    if (!e.loc_start.col) {
        cerr << endl;
        return;
    }

    cerr << " at line " << e.loc_start.line << ", col " << e.loc_start.col;

    /* loc_end.col is one past the last char + 1, so the last char is at
     * loc_end.col - 2 and the printed end column is loc_end.col - 1. */
    const bool have_end =
        e.loc_end.col != 0 && e.loc_end.line >= e.loc_start.line;
    const int end_line = have_end ? e.loc_end.line : e.loc_start.line;
    const int end_col = have_end ? e.loc_end.col - 2 : 0;

    if (have_end) {
        if (e.loc_end.line == e.loc_start.line)
            cerr << ":" << e.loc_end.col - 1;
        else
            cerr << " to line " << e.loc_end.line
                 << ", col " << e.loc_end.col - 1;
    }

    cerr << endl << endl;

    for (int ln = e.loc_start.line;
         ln <= end_line && ln <= static_cast<int>(lines.size());
         ln++) {

        const int from = (ln == e.loc_start.line) ? e.loc_start.col : 1;
        const int to = (ln == end_line) ? end_col : 0;   /* 0 == end of line */
        dumpLineWithCaret(lines[ln - 1], from, to);
    }
}

static void
handleSyntaxError(SyntaxErrorEx e)
{
    if (e.tok == &invalid_tok) {
        e.loc_start = tokens.back().loc + 1;
        e.loc_end = tokens.back().loc + 2;
    }

    cerr << "SyntaxError";
    dumpLocInError(e);
    cerr << e.msg;

    if (e.op != Op::invalid) {

        cerr << " '" << OpString[(int)e.op] << "'";

        if (e.tok)
            cerr << ", got:";
    }

    if (e.tok) {

        cerr << " '";

        if (e.tok->op != Op::invalid)
            cerr << OpString[(int)e.tok->op];
        else if (e.tok->kw != Keyword::kw_invalid)
            cerr << KwString[(int)e.tok->kw];
        else
            cerr << e.tok->value;

        cerr << "'";

    }

    cerr << endl;
}

int main(int argc, char **argv)
{
    try {

        parse_args(argc, argv);

        ParseContext ctx(TokenStream(tokens), !opt_no_const_eval);
        unique_ptr<Construct> root;

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

        /* Static type inference + checking (compile-time). Runs before
         * resolve_names, on the clean source tree. A type violation throws a
         * compile-time exception here. Validation-only (-nr) still runs it. */
        infer_types(root.get(), !opt_no_type_infer);

        if (!opt_no_run) {
            /* Resolve names to slots, then run the script. The root block
             * builds its own "main" Frame for slotted top-level variables. */
            resolve_names(root.get(), !opt_no_inline, opt_inline_threshold);
            root->eval(nullptr);
        }

    } catch (const InvalidTokenEx &e) {

        cerr << "Invalid token: " << e.val << endl;
        return 1;

    } catch (const SyntaxErrorEx &e) {

        handleSyntaxError(e);
        return 1;

    } catch (const UndefinedVariableEx &e) {

        cerr << "Undefined variable '" << e.name << "'";

        if (e.in_pure_func)
            cerr << " while evaluating a PURE function";

        dumpLocInError(e);
        cerr << format_backtrace(e);
        return 1;

    } catch (const ExceptionObject &e) {

        cerr << "Uncaught exception '" << e.get_name() << "'";

        const EvalValue &data = e.get_data();

        if (!data.is<NoneVal>())
            cerr << ", data: " << data.get_type()->to_string(data);

        dumpLocInError(e);
        cerr << format_backtrace(e);
        return 1;

    } catch (const Exception &e) {

        cerr << e.name << ": " << e.msg;
        dumpLocInError(e);
        cerr << format_backtrace(e);
        return 1;
    }

    return 0;
}
