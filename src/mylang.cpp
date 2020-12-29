/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"

#include <initializer_list>
#include <fstream>
#include <cstring>
#include <cctype>

using std::string;

static bool opt_show_tokens;
static bool opt_show_syntax_tree;
static bool opt_no_const_eval;
static bool opt_no_run;

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

            FlatSharedArray::vec_type vec;

            for (int i = 1; i < argc; i++)
                vec.emplace_back(FlatSharedStr(string(argv[i])), false);

            EvalContext::builtins.emplace(

                make_pair(
                    UniqueId::get("argv"),
                    LValue(
                        EvalValue(FlatSharedArray(move(vec))),
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

static void
dumpLocInError(const Exception &e)
{
    if (e.loc_start.col) {

        cerr << " at line "
             << e.loc_start.line
             << ", col "
             << e.loc_start.col;

        if (e.loc_end.col && e.loc_end.line == e.loc_start.line)
            cerr << ":" << e.loc_end.col - 1;

        const string &ln = lines[e.loc_start.line - 1];

        cerr << endl << endl;
        cerr << "    " << ln << endl;
        cerr << "    ";

        for (int i = 0; i < e.loc_start.col - 1; i++) {
            cerr << (isspace(ln[i]) ? ln[i] : ' ');
        }

        if (e.loc_end.col && e.loc_end.line == e.loc_start.line)
            cerr << string(std::max(1, e.loc_end.col - e.loc_start.col - 1), '^');
        else
            cerr << "^";
    }

    cerr << endl;
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

        if (!opt_no_run) {
            /* Run the script */
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
        return 1;

    } catch (const ExceptionObject &e) {

        cerr << "Uncaught dynamic exception: '" << e.get_name() << "'" << endl;
        return 1;

    } catch (const CannotBindPureFuncToConstEx &e) {

        cerr << e.name << ": " << e.msg;
        dumpLocInError(e);

        cerr << endl;
        cerr << "Solution: just declare a *named* pure function instead." << endl;

    } catch (const Exception &e) {

        cerr << e.name << ": " << e.msg;
        dumpLocInError(e);
        return 1;
    }

    return 0;
}
