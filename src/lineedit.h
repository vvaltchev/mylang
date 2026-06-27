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

/*
 * Score how well `query_lc` (already lowercased) fuzzy-matches `cand`: a
 * case-insensitive SUBSEQUENCE match scored higher for contiguous runs and
 * word-boundary / early hits, with a mild length penalty. Returns INT_MIN when
 * `cand` does not contain the query as a subsequence; an empty query matches
 * everything with score 0 (the caller orders those by recency). Exposed for the
 * headless tests.
 */
int fuzzy_score(const std::string &query_lc, const std::string &cand);

/*
 * The interactive reverse history search (Ctrl-R): a PURE state machine over a
 * history list, the search analogue of LineEditor. As the query changes it
 * recomputes the matching (de-duplicated) entries ranked best-first
 * (fuzzy_score, most-recent as the tie-break) and tracks a selected index (0 ==
 * the best match, the default). feed() drives it one byte at a time - Up/Down
 * (or Ctrl-P/N) move the selection, Ctrl-R cycles to the next match, Enter
 * accepts, Ctrl-G/Ctrl-C cancel, Backspace edits the query, printable bytes
 * extend it - so it is headless-testable; read_line renders it as a pane.
 */
class HistorySearch {

public:

    enum class Action { searching, accept, cancel };

    struct Match {
        std::string value;     /* the original history entry (for accept) */
        std::string display;   /* flattened to one line, for the result row */
        int score;
    };

    void set_history(const std::vector<std::string> *h)
    {
        hist = h;
        recompute();
    }

    Action feed(unsigned char c);

    const std::string &query() const { return q; }
    const std::vector<Match> &matches() const { return res; }
    size_t selected() const { return sel; }
    std::string selected_value() const
    {
        return sel < res.size() ? res[sel].value : std::string();
    }

private:

    enum class Esc { none, esc, csi };

    const std::vector<std::string> *hist = nullptr;
    std::string q;
    std::vector<Match> res;
    size_t sel = 0;
    Esc esc = Esc::none;
    std::string esc_params;

    void recompute();                     /* rebuild res from q, reset sel=0 */
    void csi_final(unsigned char c);
};

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
