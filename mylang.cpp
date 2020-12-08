/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"

#include <initializer_list>
#include <fstream>
#include <cstring>

/*
 * For small C++ projects, often using std everywhere is better because
 * it reduces the clutter (no "std::") and makes the code look cleaner.
 */
using namespace std;

static bool opt_show_tokens;
static bool opt_show_syntax_tree;
static bool opt_no_const_eval;
static bool opt_no_run;

static vector<string> lines;
static vector<Tok> tokens;

void run_tests();

void help()
{
    cout << "Syntax:" << endl;
    cout << "   mylang [-t] [-s] [-nc] FILE | -e EXPR" << endl;
    cout << endl;
    cout << "   -t      Show all tokens" << endl;
    cout << "   -s      Dump the syntax tree" << endl;
    cout << "  -nc      No const eval (debug)" << endl;
    cout << "  -nr      Don't run, just validate" << endl;

#ifdef TESTS
    cout << "  -rt      Run unit tests" << endl;
#endif
}

void
read_script(const char *filename)
{
    {
        string line;
        ifstream filestream(filename);

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
        lexer(lines[i], i+1, tokens);

    if (tokens.empty()) {
        help();
        exit(1);
    }
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
            inline_text += arg;
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {

            help(); exit(0);

        } else if (!strcmp(arg, "-rt")) {

            run_tests(); exit(0);

        } else if (!strcmp(arg, "-t")) {

            opt_show_tokens = true;

        } else if (!strcmp(arg, "-s")) {

            opt_show_syntax_tree = true;

        } else if (!strcmp(arg, "-nc")) {

            opt_no_const_eval = true;

        } else if (!strcmp(arg, "-nr")) {

            opt_no_run = true;

        } else if (!strcmp(arg, "-e")) {

            if (argc > 1) {
                in_tokens = true;
                continue;
            }

            help(); exit(1);

        } else {

            read_script(arg);
        }
    }

    if (in_tokens) {

        if (inline_text.empty()) {
            tokens.emplace_back();
        } else {
            lines.emplace_back(move(inline_text));
            lexer(lines[0], 1, tokens);
        }
    }
}

static void
dumpLocInError(const Exception &e)
{
    if (e.loc_start.col) {

        cout << " at line "
             << e.loc_start.line
             << ", col "
             << e.loc_start.col;

        if (e.loc_end.col && e.loc_end.line == e.loc_start.line)
            cout << ":" << e.loc_end.col - 1;

        cout << endl << endl;
        cout << "    " << lines[e.loc_start.line - 1] << endl;
        cout << "    " << string(e.loc_start.col - 1, ' ');

        if (e.loc_end.col && e.loc_end.line == e.loc_start.line)
            cout << string(e.loc_end.col - e.loc_start.col - 1, '^');
        else
            cout << "^";
    }

    cout << endl;
}

static void
handleSyntaxError(const SyntaxErrorEx &e)
{
    cout << "SyntaxError";
    dumpLocInError(e);
    cout << e.msg;

    if (e.op != Op::invalid) {

        cout << " '" << OpString[(int)e.op] << "'";

        if (e.tok)
            cout << ", got:";
    }

    if (e.tok) {

        cout << " '";

        if (e.tok->op != Op::invalid)
            cout << OpString[(int)e.tok->op];
        else if (e.tok->kw != Keyword::kw_invalid)
            cout << KwString[(int)e.tok->kw];
        else
            cout << e.tok->value;

        cout << "'";

    }

    cout << endl;
}

int main(int argc, char **argv)
{
    try {

        parse_args(argc, argv);

        ParseContext ctx(TokenStream(tokens), !opt_no_const_eval);

        if (opt_show_tokens) {
            cout << "Tokens" << endl;
            cout << "--------------------------" << endl;

            for (const auto &tok : tokens) {
                cout << tok << endl;
            }

            cout << endl;
        }

        unique_ptr<Construct> root(pBlock(ctx));

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

        if (!opt_no_run) {
            /* Run the script */
            EvalContext evalCtx;
            root->eval(&evalCtx);
        }

    } catch (const InvalidTokenEx &e) {

        cout << "Invalid token: " << e.val << endl;
        return 1;

    } catch (const SyntaxErrorEx &e) {

        handleSyntaxError(e);
        return 1;

    } catch (const UndefinedVariableEx &e) {

        cout << "Undefined variable '" << e.name << "'";
        dumpLocInError(e);
        return 1;

    } catch (const Exception &e) {

        cout << e.name;
        dumpLocInError(e);
        return 1;
    }

    return 0;
}
