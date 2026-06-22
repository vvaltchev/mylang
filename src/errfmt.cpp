/* SPDX-License-Identifier: BSD-2-Clause */

#include "errfmt.h"
#include "errors.h"
#include "lexer.h"
#include "backtrace.h"
#include "evalvalue.h"
#include "exceptionobj.h"

#include <ostream>
#include <cctype>
#include <algorithm>

using std::string;
using std::ostream;

void
dump_line_with_caret(ostream &o, const string &ln, int from, int to)
{
    if (from < 1)
        from = 1;
    if (to == 0 || to > static_cast<int>(ln.length()))
        to = static_cast<int>(ln.length());

    o << "    " << ln << "\n    ";

    for (int i = 1; i < from; i++)
        o << (i - 1 < static_cast<int>(ln.length()) &&
                      isspace(static_cast<unsigned char>(ln[i - 1]))
                  ? ln[i - 1] : ' ');

    o << string(std::max(1, to - from + 1), '^') << "\n";
}

void
dump_loc_in_error(ostream &o, const Exception &e,
                  const std::vector<string> &lines)
{
    if (!e.loc_start.col) {
        o << "\n";
        return;
    }

    o << " at line " << e.loc_start.line << ", col " << e.loc_start.col;

    /* loc_end.col is one past the last char + 1, so the last char is at
     * loc_end.col - 2 and the printed end column is loc_end.col - 1. */
    const bool have_end =
        e.loc_end.col != 0 && e.loc_end.line >= e.loc_start.line;
    const int end_line = have_end ? e.loc_end.line : e.loc_start.line;
    const int end_col = have_end ? e.loc_end.col - 2 : 0;

    if (have_end) {
        if (e.loc_end.line == e.loc_start.line)
            o << ":" << e.loc_end.col - 1;
        else
            o << " to line " << e.loc_end.line
              << ", col " << e.loc_end.col - 1;
    }

    o << "\n\n";

    for (int ln = e.loc_start.line;
         ln <= end_line && ln <= static_cast<int>(lines.size());
         ln++) {

        const int from = (ln == e.loc_start.line) ? e.loc_start.col : 1;
        const int to = (ln == end_line) ? end_col : 0;   /* 0 == end of line */
        dump_line_with_caret(o, lines[ln - 1], from, to);
    }
}

void
format_exception(ostream &o, const Exception &e,
                 const std::vector<string> &lines)
{
    if (auto *se = dynamic_cast<const SyntaxErrorEx *>(&e)) {

        o << "SyntaxError";
        dump_loc_in_error(o, e, lines);
        o << se->msg;

        if (se->op != Op::invalid) {
            o << " '" << OpString[static_cast<int>(se->op)] << "'";
            if (se->tok)
                o << ", got:";
        }

        if (se->tok) {
            o << " '";
            if (se->tok->op != Op::invalid)
                o << OpString[static_cast<int>(se->tok->op)];
            else if (se->tok->kw != Keyword::kw_invalid)
                o << KwString[static_cast<int>(se->tok->kw)];
            else
                o << se->tok->value;
            o << "'";
        }

        o << "\n";
        return;
    }

    if (auto *ue = dynamic_cast<const UndefinedVariableEx *>(&e)) {

        o << "Undefined variable '" << ue->name << "'";
        if (ue->in_pure_func)
            o << " while evaluating a PURE function";
        dump_loc_in_error(o, e, lines);
        o << format_backtrace(e);
        return;
    }

    if (auto *xo = dynamic_cast<const ExceptionObject *>(&e)) {

        o << "Uncaught exception '" << xo->get_name() << "'";
        const EvalValue &data = xo->get_data();
        if (!data.is<NoneVal>())
            o << ", data: " << data.get_type()->to_string(data);
        dump_loc_in_error(o, e, lines);
        o << format_backtrace(e);
        return;
    }

    if (auto *it = dynamic_cast<const InvalidTokenEx *>(&e)) {
        o << "Invalid token: " << it->val << "\n";
        return;
    }

    o << e.name << ": " << (e.msg ? e.msg : "");
    dump_loc_in_error(o, e, lines);
    o << format_backtrace(e);
}
