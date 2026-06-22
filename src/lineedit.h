/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>
#include <vector>
#include <functional>

/*
 * A hand-rolled line editor (the project forbids third-party deps, so no
 * readline/reline). It splits cleanly in two:
 *
 *  - LineEditor: a PURE core - an edit buffer + cursor + history navigation +
 *    an escape-sequence decoder, driven one input byte at a time via feed().
 *    It touches no file descriptor and emits no output, so it is fully
 *    headless-testable: feed a byte script, assert buffer()/cursor(). The
 *    rendering is a separate pure function (render_line).
 *
 *  - read_line(): the thin TTY shell - puts the terminal in raw mode, reads
 *    bytes, feeds them to a LineEditor, repaints via render_line, and returns
 *    the finished line. This part is deliberately tiny and not unit-tested.
 *
 * 8-bit / no-Unicode (like the language): one byte == one column.
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

    /* Start a fresh line (keeps the history pointer). */
    void reset() { buf.clear(); pos = 0; esc = Esc::none; hist_idx = -1; }

    /* Preload the buffer (e.g. to re-edit a multi-line block). */
    void set_buffer(const std::string &s) { buf = s; pos = s.size(); }

    /* The history vector is owned by the caller; the editor navigates it. */
    void set_history(const std::vector<std::string> *h) { hist = h; }

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
    std::vector<std::string> comp_list;   /* candidates pending display */

    void complete();
    void insert(char c);
    void backspace();
    void del_forward();
    void kill_to_end();
    void kill_to_start();
    void kill_word();
    void word_left();
    void word_right();
    void hist_prev();
    void hist_next();
    void csi_final(unsigned char c);
};

/*
 * Render the editor's line for a single-physical-line redraw: returns the ANSI
 * bytes that repaint `prompt + buf` and leave the cursor at the right column.
 * `highlight` (optional) recolors the buffer text (Phase 2); when null the text
 * is printed as-is.
 */
std::string render_line(const std::string &prompt, const std::string &buf,
                        size_t cursor,
                        std::string (*hl)(const std::string &) = nullptr);

/* Result of one interactive line read. */
struct ReadLineResult {
    bool eof = false;        /* Ctrl-D on empty / stream closed */
    bool cancelled = false;  /* Ctrl-C */
    std::string line;
};

/*
 * Read one line interactively in raw mode (restoring the terminal on return).
 * `history` is read for Up/Down navigation. `highlight`, if given, colors the
 * line as you type. `initial` preloads the buffer (used to auto-indent a
 * continuation line). Falls back to a plain getline when stdin is not a TTY.
 */
ReadLineResult
read_line(const std::string &prompt, std::vector<std::string> &history,
          std::string (*highlight)(const std::string &) = nullptr,
          const std::string &initial = "",
          LineEditor::Completer completer = {});
