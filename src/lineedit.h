/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <vector>
#include <functional>

/*
 * A hand-rolled line editor (the project forbids third-party deps, so no
 * readline/reline). It splits cleanly in two:
 *
 *  - LineEditor: a PURE core - a (possibly multi-line) edit buffer + a 2-D
 *    cursor + history navigation + an escape-sequence decoder, driven one input
 *    byte at a time via feed(). It touches no file descriptor and emits no
 *    output, so it is fully headless-testable: feed a byte script, assert
 *    buffer()/cursor()/cursor_row()/cursor_col().
 *
 *  - read_line(): the thin TTY shell - raw mode, byte-at-a-time read, and the
 *    2-D repaint (paint each logical line under its prompt, position the cursor
 *    in two dimensions, clear the old block). Tiny + not unit-tested.
 *
 * 8-bit / no-Unicode (like the language): one byte == one column. Each logical
 * line is assumed to fit the terminal width (no soft-wrap handling).
 */
class LineEditor {

public:

    enum class Action {
        none,       /* keep editing (caller repaints)            */
        submit,     /* Enter - the line is ready                 */
        cancel,     /* Ctrl-C - discard the line, fresh prompt   */
        eof,        /* Ctrl-D on an empty line - end of input    */
        clear,      /* Ctrl-L - caller should clear the screen   */
    };

    /* Feed one raw input byte; returns an Action (usually `none`). */
    Action feed(unsigned char c);

    const std::string &buffer() const { return buf; }
    size_t cursor() const { return pos; }

    /* The cursor's position as (logical line, column) - the buffer may hold
     * embedded newlines (a multi-line block). Used by the 2-D renderer. */
    size_t cursor_row() const;
    size_t cursor_col() const;

    /* Start a fresh line (keeps the history pointer). */
    void reset() { buf.clear(); pos = 0; esc = Esc::none; hist_idx = -1; }

    /* Preload the buffer (e.g. to re-edit a multi-line block). */
    void set_buffer(const std::string &s) { buf = s; pos = s.size(); }

    /* The history vector is owned by the caller; the editor navigates it. */
    void set_history(const std::vector<std::string> *h) { hist = h; }

    /*
     * Completeness test for multi-line editing: given the whole buffer, return
     * true if it is a complete input (Enter SUBMITS) or false if it needs more
     * (Enter inserts a newline and keeps editing). When unset, Enter always
     * submits - the single-physical-line behavior.
     */
    using Submitter = std::function<bool(const std::string &)>;
    void set_submitter(Submitter s) { submitter = std::move(s); }

    /*
     * Tab completion. The callback is given (buffer, cursor) and returns the
     * candidate full words that could replace the identifier ending at the
     * cursor (each already matching that prefix). On Tab the editor completes a
     * lone candidate / the longest common prefix in place; when several remain
     * it stashes them for the caller to display via take_completions().
     */
    using Completer =
        std::function<std::vector<std::string>(const std::string &, size_t)>;
    void set_completer(Completer c) { completer = std::move(c); }

    std::vector<std::string> take_completions()
    {
        std::vector<std::string> v;
        v.swap(comp_list);
        return v;
    }

    /*
     * Inline autosuggestion (PowerShell-style "ghost text"). The callback is
     * given the whole buffer and returns a full suggested line (or "" for
     * none). The editor shows the un-typed remainder in dim gray after the
     * cursor and accepts it on Right-arrow / Ctrl-F when the cursor is at the
     * end of the line. Distinct from Tab (which completes the identifier under
     * the cursor): the suggestion completes the WHOLE line, from history.
     */
    using Suggester = std::function<std::string(const std::string &)>;
    void set_suggester(Suggester s) { suggester = std::move(s); }

    /*
     * The un-typed remainder of the current suggestion (the gray ghost text),
     * or "" when there is none. Shown only when the cursor is at the end of a
     * single-line buffer (so the 2-D renderer stays simple) and the suggestion
     * strictly extends what is typed - mirroring PowerShell, where the
     * prediction hides once the cursor leaves the end of the line. Public so
     * the renderer and the headless tests can read it.
     */
    std::string suggestion() const;

private:

    enum class Esc { none, esc, csi };

    std::string buf;
    size_t pos = 0;
    Esc esc = Esc::none;
    std::string esc_params;               /* CSI digits/';' accumulator */
    const std::vector<std::string> *hist = nullptr;
    int hist_idx = -1;                    /* -1 == editing the live line */
    std::string saved_live;               /* live line, saved entering hist */
    Completer completer;
    Submitter submitter;
    Suggester suggester;
    std::vector<std::string> comp_list;   /* candidates pending display */

    void complete();
    bool accept_suggestion();             /* Right/Ctrl-F: take ghost text */
    void insert(char c);
    void newline();                       /* Enter on an incomplete buffer */
    void backspace();
    void del_forward();
    void kill_to_end();
    void kill_to_start();
    void kill_word();
    void word_left();
    void word_right();
    void hist_prev();
    void hist_next();
    void move_up();                       /* within buffer, else history */
    void move_down();
    size_t line_start(size_t off) const;  /* start of the line containing off */
    size_t line_end(size_t off) const;    /* the '\n' / buf end after off */
    void csi_final(unsigned char c);
};

/* Number of logical lines in `s` (1 + the count of '\n'). */
size_t line_count(const std::string &s);

/* Result of one interactive line read. */
struct ReadLineResult {
    bool eof = false;        /* Ctrl-D on empty / stream closed */
    bool cancelled = false;  /* Ctrl-C */
    std::string line;
};

/*
 * Read one (possibly multi-line) input interactively in raw mode, restoring the
 * terminal on return. `prompt` is shown on the first row, `cont_prompt` on
 * continuation rows. `submitter` decides when Enter submits vs. inserts a
 * newline (so a multi-line block stays open until it parses complete); UP/DOWN
 * move within the block and fall through to `history` at its edges. `highlight`
 * recolors each line. Falls back to getline accumulation when stdin is not a
 * TTY. The returned `line` may contain embedded newlines.
 */
ReadLineResult
read_line(const std::string &prompt, const std::string &cont_prompt,
          std::vector<std::string> &history,
          std::string (*highlight)(const std::string &) = nullptr,
          LineEditor::Completer completer = {},
          LineEditor::Submitter submitter = {});
