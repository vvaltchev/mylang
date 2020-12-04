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

void help()
{
    cout << "Syntax:" << endl;
    cout << "   mylang [-t] [-s] FILE" << endl;
    cout << "   mylang [-t] [-s] -e EXPRESSION" << endl;
    cout << endl;
    cout << "   -t      Show all tokens" << endl;
    cout << "   -s      Dump the syntax tree" << endl;
}

void
read_script(const char *filename, vector<string>& lines, vector<Tok> &tokens)
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
parse_args(int argc,
           char **argv,
           vector<string> &lines,
           vector<Tok> &tokens)
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

        } else if (!strcmp(arg, "-t")) {

            opt_show_tokens = true;

        } else if (!strcmp(arg, "-s")) {

            opt_show_syntax_tree = true;

        } else if (!strcmp(arg, "-e")) {

            if (argc > 1) {
                in_tokens = true;
                continue;
            }

            help(); exit(1);

        } else {

            read_script(arg, lines, tokens);
        }
    }

    if (in_tokens) {

        if (inline_text.empty()) {
            tokens.emplace_back();
        } else {
            lines.emplace_back(move(inline_text));
            lexer(lines[0], 0, tokens);
        }
    }
}

static void
handleSyntaxError(const SyntaxErrorEx &e)
{
    cout << "SyntaxError";

    if (e.loc.line) {

        cout << " at line "
             << e.loc.line << ", col " << e.loc.col
             << ": ";

    } else if (e.loc.col) {

        cout << " at col " << e.loc.col << ": ";

    } else {

        cout << ": ";
    }

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
    vector<string> lines;
    vector<Tok> tokens;

    try {

        parse_args(argc, argv, lines, tokens);

        ParseContext ctx{TokenStream(tokens)};

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

        /* Run the script */
        EvalContext evalCtx;
        root->eval(&evalCtx);

    } catch (const InvalidTokenEx &e) {

        cout << "Invalid token: " << e.val << endl;
        return 1;

    } catch (const SyntaxErrorEx &e) {

        handleSyntaxError(e);
        return 1;

    } catch (const DivisionByZeroEx &e) {

        cout << "DivisionByZeroEx" << endl;
        return 1;

    } catch (const TypeErrorEx &e) {

        cout << "TypeErrorEx" << endl;
        return 1;

    } catch (const NotLValueEx &e) {

        cout << "Expected lvalue, got: ";
        cout << *e.expr << endl;
        return 1;

    } catch (const UndefinedVariableEx &e) {

        cout << "Undefined variable '" << e.name << "'" << endl;
        return 1;

    } catch (const InternalErrorEx &e) {

        cout << "InternalErrorEx" << endl;
        return 1;
    }

    return 0;
}
