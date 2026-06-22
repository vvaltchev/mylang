/* SPDX-License-Identifier: BSD-2-Clause */

#include "lineedit.h"

#include <cctype>
#include <cstring>
#include <string>
#include <iostream>

#include <unistd.h>
#include <termios.h>

using std::string;

/* ---------------------------- the pure core ----------------------------- */

void LineEditor::insert(char c)
{
    buf.insert(buf.begin() + pos, c);
    pos++;
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

void LineEditor::kill_to_end()
{
    buf.erase(pos);
}

void LineEditor::kill_to_start()
{
    buf.erase(0, pos);
    pos = 0;
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

void LineEditor::csi_final(unsigned char c)
{
    switch (c) {
        case 'A': hist_prev(); break;                          /* up    */
        case 'B': hist_next(); break;                          /* down  */
        case 'C': if (pos < buf.size()) pos++; break;          /* right */
        case 'D': if (pos) pos--; break;                       /* left  */
        case 'H': pos = 0; break;                              /* home  */
        case 'F': pos = buf.size(); break;                     /* end   */
        case '~':
            if (esc_params == "1" || esc_params == "7")
                pos = 0;                                       /* home  */
            else if (esc_params == "4" || esc_params == "8")
                pos = buf.size();                              /* end   */
            else if (esc_params == "3")
                del_forward();                                 /* delete */
            break;
        default:
            break;
    }
}

LineEditor::Action LineEditor::feed(unsigned char c)
{
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
        case 13:
        case 10:                           return Action::submit;
        case 3:                            return Action::cancel; /* Ctrl-C */
        case 4:                                                   /* Ctrl-D */
            if (buf.empty())               return Action::eof;
            del_forward();                 return Action::none;
        case 12:                           return Action::clear;  /* Ctrl-L */
        case 1:   pos = 0;                 return Action::none;   /* Ctrl-A */
        case 5:   pos = buf.size();        return Action::none;   /* Ctrl-E */
        case 2:   if (pos) pos--;          return Action::none;   /* Ctrl-B */
        case 6:   if (pos < buf.size()) pos++; return Action::none; /* C-F */
        case 8:
        case 127: backspace();             return Action::none;   /* Bksp */
        case 21:  kill_to_start();         return Action::none;   /* Ctrl-U */
        case 11:  kill_to_end();           return Action::none;   /* Ctrl-K */
        case 23:  kill_word();             return Action::none;   /* Ctrl-W */
        case 16:  hist_prev();             return Action::none;   /* Ctrl-P */
        case 14:  hist_next();             return Action::none;   /* Ctrl-N */
        default:
            if (c >= 32 && c < 127)
                insert(static_cast<char>(c));
            return Action::none;
    }
}

/* --------------------------- the renderer ------------------------------- */

string
render_line(const string &prompt, const string &buf, size_t cursor,
            string (*highlight)(const string &))
{
    string out;
    out += "\r\033[K";                          /* CR + clear to end of line */
    out += prompt;
    out += highlight ? highlight(buf) : buf;

    /* Put the cursor at column (prompt + cursor): CR, then move right. */
    out += "\r";
    const size_t target = prompt.size() + cursor;
    if (target > 0)
        out += "\033[" + std::to_string(target) + "C";

    return out;
}

/* --------------------------- the TTY shell ------------------------------ */

namespace {

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
    }

    ~RawMode()
    {
        if (ok)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    }
};

void wr(const string &s)
{
    ssize_t n = write(STDOUT_FILENO, s.data(), s.size());
    (void) n;
}

}  /* namespace */

ReadLineResult
read_line(const string &prompt, std::vector<string> &history,
          string (*highlight)(const string &))
{
    ReadLineResult res;

    /* Not a terminal (piped input / a test harness): plain line read. */
    if (!isatty(STDIN_FILENO)) {
        std::cout << prompt << std::flush;
        string line;
        if (!std::getline(std::cin, line)) {
            res.eof = true;
            return res;
        }
        res.line = line;
        return res;
    }

    RawMode raw;
    LineEditor ed;
    ed.set_history(&history);

    wr(render_line(prompt, "", 0, highlight));

    for (;;) {
        unsigned char c;
        const ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            wr("\n");
            res.eof = true;
            return res;
        }

        const LineEditor::Action a = ed.feed(c);

        if (a == LineEditor::Action::submit) {
            wr("\n");
            res.line = ed.buffer();
            return res;
        }
        if (a == LineEditor::Action::cancel) {
            wr("^C\n");
            res.cancelled = true;
            return res;
        }
        if (a == LineEditor::Action::eof) {
            wr("\n");
            res.eof = true;
            return res;
        }
        if (a == LineEditor::Action::clear)
            wr("\033[2J\033[H");

        wr(render_line(prompt, ed.buffer(), ed.cursor(), highlight));
    }
}
