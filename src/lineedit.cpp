/* SPDX-License-Identifier: BSD-2-Clause */

#include "lineedit.h"

#include <cctype>
#include <cstring>
#include <climits>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <utility>
#include <iostream>

/* The raw-mode interactive editor is Unix-only (termios). On Windows the REPL
 * is not interactive (read_line falls back to a cooked getline) - see
 * plans/repl.md; the pure LineEditor core compiles everywhere for the tests. */
#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#endif

using std::string;

/* ---------------------------- the pure core ----------------------------- */

size_t line_count(const string &s)
{
    return 1 + static_cast<size_t>(std::count(s.begin(), s.end(), '\n'));
}

size_t LineEditor::line_start(size_t off) const
{
    while (off > 0 && buf[off - 1] != '\n')
        off--;
    return off;
}

size_t LineEditor::line_end(size_t off) const
{
    while (off < buf.size() && buf[off] != '\n')
        off++;
    return off;
}

size_t LineEditor::cursor_row() const
{
    return static_cast<size_t>(
        std::count(buf.begin(), buf.begin() + pos, '\n'));
}

size_t LineEditor::cursor_col() const
{
    return pos - line_start(pos);
}

void LineEditor::insert(char c)
{
    buf.insert(buf.begin() + pos, c);
    pos++;
}

/* Net bracket depth of buf[0..upto), string-aware (a naive scan, tolerant of
 * mid-edit input). Drives auto-indent for both Enter and a pasted block. */
int LineEditor::indent_depth(size_t upto) const
{
    if (upto > buf.size())
        upto = buf.size();

    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; i < upto; i++) {
        const char c = buf[i];
        if (in_str) {
            if (c == '"')
                in_str = false;
        } else if (c == '"') {
            in_str = true;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            depth--;
        }
    }
    return depth < 0 ? 0 : depth;
}

/* Enter on an incomplete buffer: split the line here and auto-indent the new
 * line by the bracket depth up to the cursor. */
void LineEditor::newline()
{
    buf.insert(buf.begin() + pos, '\n');
    pos++;

    const int depth = indent_depth(pos);
    for (int i = 0; i < depth * 2; i++) {
        buf.insert(buf.begin() + pos, ' ');
        pos++;
    }
}

/*
 * Insert a bracketed paste as inert text: never as editor commands (no submit
 * on a newline, no Tab/Ctrl interpretation). The block is RE-INDENTED to the
 * editor's own style - each line's original leading whitespace is dropped and
 * replaced by bracket-depth indentation (2 spaces per level; a line that opens
 * with a closing bracket dedents one level). Safe because MyLang whitespace is
 * purely cosmetic. The first line is inserted verbatim when the cursor is
 * mid-line (it continues what is already there); otherwise it is re-indented
 * like the rest. Line endings are normalized and trailing blank lines dropped;
 * tabs become a space and other control bytes are dropped.
 */
void LineEditor::apply_paste(const std::string &text)
{
    std::string norm;
    norm.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                i++;                          /* CRLF -> one '\n' */
            norm += '\n';
        } else {
            norm += c;
        }
    }
    while (!norm.empty() && norm.back() == '\n')
        norm.pop_back();

    const bool first_inline = (cursor_col() != 0);   /* continuing a line? */

    size_t start = 0, li = 0;
    for (size_t i = 0; i <= norm.size(); i++) {

        if (i != norm.size() && norm[i] != '\n')
            continue;

        const std::string line = norm.substr(start, i - start);
        start = i + 1;

        /* Clean the line: drop its own leading whitespace (unless this is the
         * first line continuing existing text), turn tabs into a space, drop
         * other control bytes. */
        const bool keep_leading = (li == 0 && first_inline);
        size_t b = 0;
        if (!keep_leading)
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t'))
                b++;
        std::string content;
        for (size_t k = b; k < line.size(); k++) {
            const char ch = line[k];
            if (ch == '\t')
                content += ' ';
            else if (static_cast<unsigned char>(ch) >= 32 &&
                     static_cast<unsigned char>(ch) < 127)
                content += ch;
        }

        if (li > 0) {                          /* a fresh line in the block */
            buf.insert(buf.begin() + pos, '\n');
            pos++;
        }
        if (li > 0 || !first_inline) {         /* re-indent this line */
            int depth = indent_depth(pos);
            if (!content.empty()) {
                const char f = content[0];
                if (f == ')' || f == ']' || f == '}')
                    depth = depth > 0 ? depth - 1 : 0;
            }
            for (int s = 0; s < depth * 2; s++) {
                buf.insert(buf.begin() + pos, ' ');
                pos++;
            }
        }

        for (char ch : content)
            insert(ch);
        li++;
    }
}

/* UP: move to the same column on the previous line; on the first line, recall
 * the previous history entry instead. DOWN is the mirror. */
void LineEditor::move_up()
{
    const size_t ls = line_start(pos);
    if (ls == 0) {
        hist_prev();
        return;
    }
    const size_t col = pos - ls;
    const size_t prev_start = line_start(ls - 1);   /* ls-1 is the '\n' */
    const size_t prev_len = (ls - 1) - prev_start;
    pos = prev_start + std::min(col, prev_len);
}

void LineEditor::move_down()
{
    const size_t le = line_end(pos);
    if (le == buf.size()) {
        hist_next();
        return;
    }
    const size_t col = pos - line_start(pos);
    const size_t next_start = le + 1;
    const size_t next_len = line_end(next_start) - next_start;
    pos = next_start + std::min(col, next_len);
}

void LineEditor::backspace()
{
    if (pos) {
        buf.erase(buf.begin() + (pos - 1));
        pos--;
    }
}

void LineEditor::del_forward()
{
    if (pos < buf.size())
        buf.erase(buf.begin() + pos);
}

void LineEditor::kill_to_end()        /* to end of the current LINE */
{
    buf.erase(pos, line_end(pos) - pos);
}

void LineEditor::kill_to_start()      /* to start of the current LINE */
{
    const size_t ls = line_start(pos);
    buf.erase(ls, pos - ls);
    pos = ls;
}

void LineEditor::kill_word()
{
    const size_t end = pos;
    while (pos && isspace(static_cast<unsigned char>(buf[pos - 1])))
        pos--;
    while (pos && !isspace(static_cast<unsigned char>(buf[pos - 1])))
        pos--;
    buf.erase(pos, end - pos);
}

void LineEditor::word_left()
{
    while (pos && isspace(static_cast<unsigned char>(buf[pos - 1])))
        pos--;
    while (pos && !isspace(static_cast<unsigned char>(buf[pos - 1])))
        pos--;
}

void LineEditor::word_right()
{
    while (pos < buf.size() && isspace(static_cast<unsigned char>(buf[pos])))
        pos++;
    while (pos < buf.size() && !isspace(static_cast<unsigned char>(buf[pos])))
        pos++;
}

void LineEditor::hist_prev()
{
    if (!hist || hist->empty())
        return;

    if (hist_idx == -1) {                 /* entering history from live */
        saved_live = buf;
        hist_idx = static_cast<int>(hist->size());
    }
    if (hist_idx > 0) {
        hist_idx--;
        buf = (*hist)[hist_idx];
        pos = buf.size();
    }
}

void LineEditor::hist_next()
{
    if (!hist || hist_idx == -1)
        return;

    hist_idx++;
    if (hist_idx >= static_cast<int>(hist->size())) {
        hist_idx = -1;
        buf = saved_live;                 /* back to the live line */
    } else {
        buf = (*hist)[hist_idx];
    }
    pos = buf.size();
}

void LineEditor::complete()
{
    if (!completer)
        return;

    /* the identifier prefix ending at the cursor */
    size_t start = pos;
    while (start > 0 &&
           (isalnum(static_cast<unsigned char>(buf[start - 1])) ||
            buf[start - 1] == '_'))
        start--;
    const string word = buf.substr(start, pos - start);

    const std::vector<string> cands = completer(buf, pos);
    if (cands.empty())
        return;

    if (cands.size() == 1) {                       /* unique: complete it */
        buf.replace(start, pos - start, cands[0]);
        pos = start + cands[0].size();
        return;
    }

    /* several: extend to the longest common prefix, then offer the list */
    string lcp = cands[0];
    for (size_t i = 1; i < cands.size(); i++) {
        size_t j = 0;
        while (j < lcp.size() && j < cands[i].size() && lcp[j] == cands[i][j])
            j++;
        lcp.resize(j);
    }
    if (lcp.size() > word.size()) {
        buf.replace(start, pos - start, lcp);
        pos = start + lcp.size();
    }
    comp_list = cands;
}

/*
 * The un-typed remainder of the inline suggestion (the gray ghost text). Shown
 * only at the end of a single-line buffer (PowerShell hides the prediction once
 * the cursor leaves the line end), and only when the suggested full line
 * strictly extends what is typed.
 */
std::string LineEditor::suggestion() const
{
    if (!suggester || buf.empty() || pos != buf.size() ||
        buf.find('\n') != string::npos)
        return string();

    const string full = suggester(buf);
    if (full.size() > buf.size() && full.find('\n') == string::npos &&
        full.compare(0, buf.size(), buf) == 0)
        return full.substr(buf.size());

    return string();
}

/* Right-arrow / Ctrl-F when a ghost suggestion is showing: take it (append the
 * remainder, cursor to end). Returns false when there is nothing to accept, so
 * the caller falls back to moving the cursor right. */
bool LineEditor::accept_suggestion()
{
    const string s = suggestion();
    if (s.empty())
        return false;

    buf += s;
    pos = buf.size();
    return true;
}

void LineEditor::csi_final(unsigned char c)
{
    switch (c) {
        case 'A': move_up(); break;                            /* up    */
        case 'B': move_down(); break;                          /* down  */
        case 'C':                                              /* right */
            if (!accept_suggestion() && pos < buf.size()) pos++;
            break;
        case 'D': if (pos) pos--; break;                       /* left  */
        case 'H': pos = line_start(pos); break;                /* home  */
        case 'F': pos = line_end(pos); break;                  /* end   */
        case '~':
            if (esc_params == "1" || esc_params == "7")
                pos = line_start(pos);                         /* home  */
            else if (esc_params == "4" || esc_params == "8")
                pos = line_end(pos);                           /* end   */
            else if (esc_params == "3")
                del_forward();                                 /* delete */
            else if (esc_params == "200") {            /* bracketed paste on */
                is_pasting = true;
                paste_buf.clear();
            }
            break;
        default:
            break;
    }
}

LineEditor::Action LineEditor::feed(unsigned char c)
{
    /* Bracketed paste in flight: swallow bytes verbatim (never as keystrokes -
     * a newline must not submit, a Tab must not complete) until the end marker
     * ESC[201~, then re-indent and insert the captured block. */
    if (is_pasting) {
        paste_buf += static_cast<char>(c);
        static const std::string END = "\033[201~";
        const size_t n = paste_buf.size();
        if (n >= END.size() &&
            paste_buf.compare(n - END.size(), END.size(), END) == 0) {
            paste_buf.erase(n - END.size());
            apply_paste(paste_buf);
            paste_buf.clear();
            is_pasting = false;
        }
        return Action::none;
    }

    /* Multi-byte escape sequence in progress (arrows, Home/End, Delete). */
    if (esc == Esc::esc) {
        esc = (c == '[' || c == 'O') ? Esc::csi : Esc::none;
        esc_params.clear();
        return Action::none;
    }
    if (esc == Esc::csi) {
        if ((c >= '0' && c <= '9') || c == ';') {
            esc_params += static_cast<char>(c);
            return Action::none;
        }
        csi_final(c);
        esc = Esc::none;
        return Action::none;
    }

    switch (c) {
        case 27:  esc = Esc::esc;          return Action::none;  /* ESC */
        case 9:   complete();              return Action::none;  /* Tab */
        case 13:
        case 10:                                                 /* Enter */
            if (!submitter || submitter(buf))
                return Action::submit;
            newline();                     return Action::none;
        case 3:                            return Action::cancel; /* Ctrl-C */
        case 4:                                                   /* Ctrl-D */
            if (buf.empty())               return Action::eof;
            del_forward();                 return Action::none;
        case 12:                           return Action::clear;  /* Ctrl-L */
        case 1:   pos = line_start(pos);   return Action::none;   /* Ctrl-A */
        case 5:   pos = line_end(pos);     return Action::none;   /* Ctrl-E */
        case 2:   if (pos) pos--;          return Action::none;   /* Ctrl-B */
        case 6:                                                    /* Ctrl-F */
            if (!accept_suggestion() && pos < buf.size()) pos++;
            return Action::none;
        case 8:
        case 127: backspace();             return Action::none;   /* Bksp */
        case 21:  kill_to_start();         return Action::none;   /* Ctrl-U */
        case 11:  kill_to_end();           return Action::none;   /* Ctrl-K */
        case 23:  kill_word();             return Action::none;   /* Ctrl-W */
        case 16:  move_up();               return Action::none;   /* Ctrl-P */
        case 14:  move_down();             return Action::none;   /* Ctrl-N */
        default:
            if (c >= 32 && c < 127)
                insert(static_cast<char>(c));
            return Action::none;
    }
}

/* ------------------------ reverse history search ------------------------ */

int fuzzy_score(const string &ql, const string &cand)
{
    if (ql.empty())
        return 0;                          /* match all; caller uses recency */

    int score = 0, prev = -1;
    size_t qi = 0;

    for (size_t j = 0; j < cand.size() && qi < ql.size(); j++) {

        if (static_cast<char>(tolower(static_cast<unsigned char>(cand[j])))
                != ql[qi])
            continue;

        const int gap = static_cast<int>(j) - prev - 1;   /* chars skipped */

        /* Contiguity is the strongest signal (a gap-0 match continues the run,
         * including the very first char at index 0); a gap is penalized so a
         * scattered match never beats a tight one even when every hit lands on
         * a word boundary. */
        if (gap == 0)
            score += 15;
        else
            score -= gap;

        /* A match at a word boundary (start, after a non-alnum, or a camelCase
         * hump) scores extra - that's what floats a prefix to the top. */
        if (j == 0 ||
            !isalnum(static_cast<unsigned char>(cand[j - 1])) ||
            (isupper(static_cast<unsigned char>(cand[j])) &&
             islower(static_cast<unsigned char>(cand[j - 1]))))
            score += 10;

        prev = static_cast<int>(j);
        qi++;
    }

    if (qi < ql.size())
        return INT_MIN;                    /* not a subsequence */

    score -= static_cast<int>(cand.size()) / 8;   /* mild length tie-break */
    return score;
}

std::vector<int> fuzzy_match_positions(const string &ql, const string &cand)
{
    std::vector<int> pos;
    if (ql.empty())
        return pos;

    size_t qi = 0;
    for (size_t j = 0; j < cand.size() && qi < ql.size(); j++)
        if (static_cast<char>(tolower(static_cast<unsigned char>(cand[j])))
                == ql[qi]) {
            pos.push_back(static_cast<int>(j));
            qi++;
        }

    if (qi < ql.size())
        pos.clear();                       /* not a full match */
    return pos;
}

void HistorySearch::recompute()
{
    res.clear();
    sel = 0;
    if (!hist)
        return;

    string ql = q;
    for (char &c : ql)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    /* De-duplicate, newest-first: stable_sort below keeps this order for equal
     * scores, so among ties the most recent command ranks first. */
    std::unordered_set<string> seen;
    for (auto it = hist->rbegin(); it != hist->rend(); ++it) {

        if (!seen.insert(*it).second)
            continue;

        const int sc = fuzzy_score(ql, *it);
        if (sc == INT_MIN)
            continue;

        Match m;
        m.value = *it;
        m.display = *it;
        for (char &c : m.display)
            if (c == '\n' || c == '\t' || c == '\r')
                c = ' ';                   /* flatten for the one-line row */
        m.score = sc;
        res.push_back(std::move(m));
    }

    std::stable_sort(res.begin(), res.end(),
        [](const Match &a, const Match &b) { return a.score > b.score; });
}

void HistorySearch::csi_final(unsigned char c)
{
    if (res.empty())
        return;

    if (c == 'A') {                        /* up: toward the best match (top) */
        if (sel > 0)
            sel--;
    } else if (c == 'B') {                 /* down: toward worse matches */
        if (sel + 1 < res.size())
            sel++;
    }
}

HistorySearch::Action HistorySearch::feed(unsigned char c)
{
    if (esc == Esc::esc) {
        esc = (c == '[' || c == 'O') ? Esc::csi : Esc::none;
        esc_params.clear();
        return Action::searching;
    }
    if (esc == Esc::csi) {
        if ((c >= '0' && c <= '9') || c == ';') {
            esc_params += static_cast<char>(c);
            return Action::searching;
        }
        csi_final(c);
        esc = Esc::none;
        return Action::searching;
    }

    switch (c) {
        case 27:  esc = Esc::esc;       return Action::searching;  /* ESC seq */
        case 13:
        case 10:                        return Action::accept;     /* Enter */
        case 3:                                                    /* Ctrl-C */
        case 7:                         return Action::cancel;     /* Ctrl-G */
        case 18:                                                   /* Ctrl-R */
            if (!res.empty())
                sel = (sel + 1) % res.size();   /* cycle to the next match */
            return Action::searching;
        case 8:
        case 127:                                                /* Backspace */
            if (!q.empty()) {
                q.pop_back();
                recompute();
            }
            return Action::searching;
        case 16:  csi_final('A');       return Action::searching;  /* Ctrl-P */
        case 14:  csi_final('B');       return Action::searching;  /* Ctrl-N */
        default:
            if (c >= 32 && c < 127) {
                q.push_back(static_cast<char>(c));
                recompute();
            }
            return Action::searching;
    }
}

/* --------------------------- the TTY shell ------------------------------ */

namespace {

/* Split `s` into its logical lines (by '\n'). */
std::vector<string> split_lines(const string &s)
{
    std::vector<string> rows;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++)
        if (i == s.size() || s[i] == '\n') {
            rows.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return rows;
}

#ifndef _WIN32
/* Put the terminal in raw mode for the lifetime of the object, restoring the
 * previous attributes on destruction (so the shell is never left broken). */
struct RawMode {
    termios orig;
    bool ok = false;

    RawMode()
    {
        if (tcgetattr(STDIN_FILENO, &orig) != 0)
            return;

        termios raw = orig;
        /* No canonical mode / echo / signal chars (we handle Ctrl-C/D/Z as
         * bytes); no XON/XOFF flow control or CR->NL input translation. Output
         * processing (OPOST) is left on so a written '\n' still does CR+LF. */
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
            ok = true;

        if (ok) {
            /* Enable bracketed paste so the editor receives a paste as data
             * (wrapped in ESC[200~ / ESC[201~) rather than as keystrokes. */
            const char *e = "\033[?2004h";
            ssize_t n = write(STDOUT_FILENO, e, strlen(e));
            (void) n;
        }
    }

    ~RawMode()
    {
        if (ok) {
            const char *d = "\033[?2004l";       /* disable bracketed paste */
            ssize_t n = write(STDOUT_FILENO, d, strlen(d));
            (void) n;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
        }
    }
};

void wr(const string &s)
{
    ssize_t n = write(STDOUT_FILENO, s.data(), s.size());
    (void) n;
}

/* The terminal size (rows, cols), with a sane fallback if the ioctl fails. */
void term_size(int &rows, int &cols)
{
    rows = 24;
    cols = 80;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) rows = ws.ws_row;
        if (ws.ws_col > 0) cols = ws.ws_col;
    }
}

/* Is another input byte available within `ms` milliseconds? Used to tell a lone
 * ESC (cancel) from the ESC that starts an arrow sequence (which arrives as a
 * burst), without consuming the byte. */
bool byte_ready(int ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}

/* Does the locale look like UTF-8? Decides whether the search pane draws its
 * border with rounded box-drawing glyphs or a pure-ASCII fallback (+-|). */
bool unicode_ok()
{
    for (const char *v : { "LC_ALL", "LC_CTYPE", "LANG" }) {
        const char *s = getenv(v);
        if (s && *s) {
            string val(s);
            for (char &c : val)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return val.find("utf-8") != string::npos ||
                   val.find("utf8") != string::npos;
        }
    }
    return false;
}
#endif /* !_WIN32 */

/*
 * Cooked (non-raw) read: accumulate physical lines until the input parses
 * complete (or EOF), so a multi-line block read off a pipe behaves like one
 * typed at the prompt. Used for piped input / a test harness, and as the whole
 * read path on Windows (no raw-mode editor there).
 */
ReadLineResult
read_line_cooked(const string &prompt, const string &cont_prompt,
                 const LineEditor::Submitter &submitter)
{
    ReadLineResult res;
    std::cout << prompt << std::flush;
    string acc, line;
    while (std::getline(std::cin, line)) {
        if (!acc.empty())
            acc += "\n";
        acc += line;
        if (!submitter || submitter(acc)) {
            res.line = acc;
            return res;
        }
        std::cout << cont_prompt << std::flush;
    }
    if (!acc.empty()) {
        res.line = acc;     /* trailing incomplete block at EOF */
        return res;
    }
    res.eof = true;
    return res;
}

/* Join a Tab-completion candidate list (two spaces apart) + a trailing NL. */
string completions_str(const std::vector<string> &cands)
{
    string out;
    for (size_t i = 0; i < cands.size(); i++) {
        out += cands[i];
        if (i + 1 < cands.size())
            out += "  ";
    }
    out += "\n";
    return out;
}

}  /* namespace */

ReadLineResult
read_line(const string &prompt, const string &cont_prompt,
          std::vector<string> &history,
          string (*highlight)(const string &, int &),
          LineEditor::Completer completer, LineEditor::Submitter submitter)
{
    ReadLineResult res;

    /*
     * On Windows, or when stdin is not a terminal (piped input / a test
     * harness), there is no raw-mode editor - read in cooked mode.
     */
#ifdef _WIN32
    return read_line_cooked(prompt, cont_prompt, submitter);
#else
    if (!isatty(STDIN_FILENO))
        return read_line_cooked(prompt, cont_prompt, submitter);

    RawMode raw;
    LineEditor ed;
    ed.set_history(&history);
    if (completer)
        ed.set_completer(std::move(completer));
    if (submitter)
        ed.set_submitter(std::move(submitter));

    /*
     * PowerShell-style inline autosuggestion from history: the gray ghost text
     * is the most recent single-line history entry that the current buffer is a
     * strict prefix of. Enabled only with color on (the ghost must be visually
     * distinct from typed text), so NO_COLOR / no-TTY get no suggestion.
     */
    if (highlight) {
        ed.set_suggester([&history](const string &b) -> string {
            if (b.empty() || b.find('\n') != string::npos)
                return string();
            for (auto it = history.rbegin(); it != history.rend(); ++it) {
                const string &h = *it;
                if (h.size() > b.size() && h.find('\n') == string::npos &&
                    h.compare(0, b.size(), b) == 0)
                    return h;
            }
            return string();
        });
    }

    /* The row (0-based, within the block) the cursor was on after the last
     * paint - so the next paint can move back to the top of the block. */
    int prev_cursor_row = 0;

    auto repaint = [&]() {
        string o;
        if (prev_cursor_row > 0)
            o += "\033[" + std::to_string(prev_cursor_row) + "A";
        o += "\r\033[J";                       /* col 0, clear downward */

        const std::vector<string> rows = split_lines(ed.buffer());
        int hlstate = 0;           /* thread string/comment color across rows */
        for (size_t i = 0; i < rows.size(); i++) {
            o += (i == 0 ? prompt : cont_prompt);
            o += highlight ? highlight(rows[i], hlstate) : rows[i];
            if (i + 1 < rows.size())
                o += "\r\n";
        }

        /* The inline suggestion's remainder in dim gray, just past the cursor.
         * suggestion() is non-empty only for a single-line buffer with the
         * cursor at the end, so it always belongs on this last row; the cursor
         * is repositioned to its start (before the ghost) below. */
        const string ghost = ed.suggestion();
        if (!ghost.empty())
            o += "\033[90m" + ghost + "\033[0m";

        const int crow = static_cast<int>(ed.cursor_row());
        const int last = static_cast<int>(rows.size()) - 1;
        if (last > crow)
            o += "\033[" + std::to_string(last - crow) + "A";
        o += "\r";
        const size_t target =
            (crow == 0 ? prompt.size() : cont_prompt.size()) + ed.cursor_col();
        if (target > 0)
            o += "\033[" + std::to_string(target) + "C";

        prev_cursor_row = crow;
        wr(o);
    };

    /*
     * Ctrl-R: a reverse history search pane (~1/3 of the screen) below the
     * input. Results rank best-first and update live as you type; Up/Down (or
     * Ctrl-P/N) select, the default being the top (best) row, highlighted;
     * Ctrl-R cycles to the next match; Enter LOADS the selection into the
     * editor (it is not auto-run); Esc / Ctrl-G / Ctrl-C cancel. On return the
     * pane is erased and the cursor is back at the input's first row, so the
     * caller just repaints. Returns {accepted, value}.
     */
    auto reverse_search = [&]() -> std::pair<bool, string> {
        HistorySearch hs;
        hs.set_history(&history);

        int trows, tcols;
        term_size(trows, tcols);
        const int input_rows = static_cast<int>(line_count(ed.buffer()));
        int pane_rows = trows / 3;
        if (pane_rows < 3) pane_rows = 3;             /* top + 1 row + bottom */
        const int room = trows - input_rows - 1;      /* keep input visible */
        if (room >= 3 && pane_rows > room) pane_rows = room;
        if (pane_rows < 3) pane_rows = 3;

        /* Move below the input and reserve pane_rows lines. Printing newlines
         * then moving back up is scroll-safe: if the terminal scrolls, the
         * relative move lands at the same content. */
        {
            string o;
            const int down = (input_rows - 1) - prev_cursor_row;
            if (down > 0) o += "\033[" + std::to_string(down) + "B";
            o += "\r\n";
            o += string(pane_rows - 1, '\n') +
                 "\033[" + std::to_string(pane_rows - 1) + "A";
            wr(o);
        }

        /* Box-drawing glyphs (rounded), as explicit UTF-8 bytes so the source
         * stays pure ASCII; an ASCII fallback (+ - |) when the locale is not
         * UTF-8. Each glyph is one display column. */
        const bool color = highlight != nullptr;
        const bool uni = unicode_ok();
        const char *const TL = uni ? "\xe2\x95\xad" : "+"; /* U+256D */
        const char *const TR = uni ? "\xe2\x95\xae" : "+"; /* U+256E */
        const char *const BL = uni ? "\xe2\x95\xb0" : "+"; /* U+2570 */
        const char *const BR = uni ? "\xe2\x95\xaf" : "+"; /* U+256F */
        const char *const HZ = uni ? "\xe2\x94\x80" : "-"; /* U+2500 */
        const char *const VT = uni ? "\xe2\x94\x82" : "|"; /* U+2502 */
        const char *const MARK = uni ? "\xe2\x9d\xaf " : "> "; /* U+276F */

        auto hrule = [&](int n) {
            string s;
            for (int i = 0; i < n; i++) s += HZ;
            return s;
        };

        int win_top = 0;
        int cursor_col = 0;                  /* set by draw; the cursor slot */

        auto draw = [&]() {
            const std::vector<HistorySearch::Match> &ms = hs.matches();
            const int sel = static_cast<int>(hs.selected());
            const int visible = pane_rows - 2;        /* minus 2 borders */
            int inner = tcols - 4;                    /* "VT _ <cell> _ VT" */
            if (inner < 1) inner = 1;

            if (sel < win_top) win_top = sel;
            if (sel >= win_top + visible) win_top = sel - visible + 1;
            if (win_top < 0) win_top = 0;

            string ql = hs.query();
            for (char &c : ql)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

            string o = "\r";

            /* top border: corner + " search: <q> " fill " <n matches> " end */
            {
                string count = std::to_string(ms.size()) +
                               (ms.size() == 1 ? " match" : " matches");
                int qlen = static_cast<int>(hs.query().size());
                const int clen = static_cast<int>(count.size());
                bool show_count = true;
                int fill = tcols - 16 - qlen - clen;
                if (fill < 1) { show_count = false; fill = tcols - 14 - qlen; }
                if (fill < 1) {                       /* truncate the query */
                    qlen = std::max(0, qlen + fill - 1);
                    fill = 1;
                }
                o += string(TL) + HZ + " search: " +
                     hs.query().substr(0, qlen) + " " + hrule(fill);
                if (show_count) { o += " "; o += count; o += " "; }
                o += HZ; o += TR;
                cursor_col = 11 + qlen;               /* just after the query */
                if (cursor_col >= tcols) cursor_col = tcols - 1;
            }

            /* result rows, best-first, the selected one a reverse-video bar
             * with the matched query letters bolded */
            for (int k = 0; k < visible; k++) {
                o += "\r\n";
                string cell;

                const int idx = win_top + k;
                if (ms.empty() && k == 0) {

                    string t = "(no matches)";
                    if (static_cast<int>(t.size()) > inner)
                        t = t.substr(0, inner);
                    if (color) cell += "\033[90m";
                    cell += t;
                    if (color) cell += "\033[0m";
                    cell += string(inner - static_cast<int>(t.size()), ' ');

                } else if (!ms.empty() && idx < static_cast<int>(ms.size())) {

                    const bool is_sel = (idx == sel);
                    string body = ms[idx].display;
                    const int bodyw = inner - 2;      /* minus the 2-col mark */
                    if (static_cast<int>(body.size()) > bodyw && bodyw >= 0)
                        body = body.substr(0, bodyw);

                    const std::vector<int> hl = fuzzy_match_positions(ql, body);

                    if (is_sel && color) cell += "\033[7m";
                    cell += is_sel ? MARK : "  ";
                    size_t hp = 0;
                    for (int i = 0; i < static_cast<int>(body.size()); i++) {
                        const bool m = hp < hl.size() && hl[hp] == i;
                        if (m) { hp++; if (color) cell += "\033[1m"; }
                        cell += body[i];
                        if (m && color) cell += "\033[22m";
                    }
                    const int used = 2 + static_cast<int>(body.size());
                    if (used < inner) cell += string(inner - used, ' ');
                    if (is_sel && color) cell += "\033[0m";

                } else {
                    cell = string(inner, ' ');        /* an empty row */
                }

                o += string(VT) + " " + cell + " " + VT;
            }

            /* bottom border */
            o += "\r\n";
            o += string(BL) + hrule(tcols - 2) + BR;

            /* back to the top row, cursor into the query slot */
            o += "\033[" + std::to_string(pane_rows - 1) + "A\r";
            if (cursor_col > 0) o += "\033[" + std::to_string(cursor_col) + "C";
            wr(o);
        };

        draw();

        std::pair<bool, string> result{false, string()};
        for (;;) {
            unsigned char ch;
            const ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0)
                break;                          /* EOF -> cancel */
            if (ch == 27 && !byte_ready(40))
                break;                          /* a lone ESC -> cancel */
            const HistorySearch::Action a = hs.feed(ch);
            if (a == HistorySearch::Action::accept) {
                result = {true, hs.selected_value()};
                break;
            }
            if (a == HistorySearch::Action::cancel)
                break;
            draw();
        }

        string o = "\r\033[J";                  /* erase the pane */
        o += "\033[" + std::to_string(input_rows) + "A\r";   /* to input top */
        wr(o);
        prev_cursor_row = 0;
        return result;
    };

    /* Move the terminal cursor below the whole block (for submit / a list). */
    auto move_below = [&]() {
        const int rows = static_cast<int>(line_count(ed.buffer()));
        const int down = (rows - 1) - prev_cursor_row;
        string o;
        if (down > 0)
            o += "\033[" + std::to_string(down) + "B";
        o += "\r\n";
        wr(o);
        prev_cursor_row = 0;
    };

    repaint();

    for (;;) {
        unsigned char c;
        const ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            move_below();
            res.eof = true;
            return res;
        }

        if (!ed.pasting() && c == 18) {     /* Ctrl-R: reverse history search */
            const std::pair<bool, string> chosen = reverse_search();
            if (chosen.first && !chosen.second.empty())
                ed.set_buffer(chosen.second);
            repaint();
            continue;
        }

        const LineEditor::Action a = ed.feed(c);

        if (ed.pasting())                   /* mid-paste: accumulate quietly */
            continue;

        if (a == LineEditor::Action::submit) {
            move_below();
            res.line = ed.buffer();
            return res;
        }
        if (a == LineEditor::Action::cancel) {
            move_below();
            res.cancelled = true;
            return res;
        }
        if (a == LineEditor::Action::eof) {
            move_below();
            res.eof = true;
            return res;
        }
        if (a == LineEditor::Action::clear) {
            wr("\033[2J\033[H");
            prev_cursor_row = 0;
        }

        /* A Tab with several candidates: list them, then repaint fresh. */
        const std::vector<string> cands = ed.take_completions();
        if (!cands.empty()) {
            move_below();
            wr(completions_str(cands));
        }

        repaint();
    }
#endif /* !_WIN32 */
}
