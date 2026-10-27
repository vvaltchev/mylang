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
                  const std::vector<string> &lines, const string &src_name)
{
    if (!e.loc_start.col) {
        o << "\n";
        return;
    }

    o << " at ";
    if (!src_name.empty())
        o << src_name << ", ";
    o << "line " << e.loc_start.line << ", col " << e.loc_start.col;

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

    /* No source text (a .myv whose source is absent, changed, or was never
     * referenced): print the located header only - the caret block needs the
     * LINE, which a Loc cannot supply. The file name above plus the
     * backtrace's per-frame "func(params) at line N" still locate the error
     * exactly; only the quoted text is missing. */
    if (lines.empty()) {
        o << "\n";
        return;
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

static void
format_exception_body(ostream &o, const Exception &e,
                      const std::vector<string> &lines,
                      const string &src_name, bool uncaught)
{
    if (auto *se = dynamic_cast<const SyntaxErrorEx *>(&e)) {

        o << "SyntaxError";
        dump_loc_in_error(o, e, lines, src_name);
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
        dump_loc_in_error(o, e, lines, src_name);
        o << format_backtrace(e);
        return;
    }

    if (auto *xo = dynamic_cast<const ExceptionObject *>(&e)) {

        o << (uncaught ? "Uncaught exception '" : "Exception '")
          << xo->get_name() << "'";
        const EvalValue &data = xo->get_data();
        if (!data.is<NoneVal>())
            o << ", data: " << data.get_type()->to_string(data);
        dump_loc_in_error(o, e, lines, src_name);
        o << format_backtrace(e);
        return;
    }

    if (auto *it = dynamic_cast<const InvalidTokenEx *>(&e)) {
        o << "Invalid token: " << it->val;
        /* a caret only when a loc is set */
        dump_loc_in_error(o, e, lines, src_name);
        return;
    }

    o << e.name << ": " << (e.msg ? e.msg : "");
    dump_loc_in_error(o, e, lines, src_name);
    o << format_backtrace(e);
}

/*
 * Question 12: the exceptions `e` was raised while handling, OLDEST first,
 * each followed by Python's separator - so the report ends with `e` itself.
 * A chain node is a clone without a backtrace (exc_note_context), so each
 * prints its header and caret only. At most EXC_CONTEXT_MAX, the youngest;
 * a note says when older ones were left out, here or when they escaped.
 */
static void
format_exception_context(ostream &o, const Exception &e,
                         const std::vector<string> &lines,
                         const string &src_name)
{
    const auto *re = dynamic_cast<const RuntimeException *>(&e);
    if (!re || !re->context)
        return;
    std::vector<const RuntimeException *> chain;      /* youngest first */
    bool cut = re->context_cut;
    for (const RuntimeException *c = re->context.get(); c;
         c = c->context.get()) {
        if (chain.size() == static_cast<size_t>(EXC_CONTEXT_MAX)) {
            cut = true;
            break;
        }
        chain.push_back(c);
        cut = cut || c->context_cut;
    }
    if (cut)
        o << "(older exceptions are not shown)\n\n";
    for (size_t i = chain.size(); i-- > 0; ) {
        format_exception_body(o, *chain[i], lines, src_name, false);
        o << "\nDuring handling of the above exception, another exception "
             "occurred:\n\n";
    }
}

void
format_exception(ostream &o, const Exception &e,
                 const std::vector<string> &lines, const string &src_name,
                 bool compiling)
{
    format_exception_context(o, e, lines, src_name);
    format_exception_body(o, e, lines, src_name, true);
    format_const_eval_note(o, e, compiling);
}

void format_const_eval_note(ostream &o, const Exception &e, bool compiling)
{
    if (!compiling || !e.is_catchable())
        return;
    o << "note: a constant expression is evaluated while compiling, so its "
         "error is a\n"
         "compile error, which no try/catch handles; to raise it when the "
         "program runs\n"
         "instead, pass one of its values through runtime(), as in "
         "10 / runtime(0)\n";
}
