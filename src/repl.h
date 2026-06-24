/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"
#include <string>
#include <vector>
#include <memory>

class EvalContext;
class Construct;

/*
 * The REPL evaluation engine. Holds the persistent interpreter state - the
 * const-eval context, the runtime global scope, the retained input ASTs and the
 * accumulated source - and evaluates one input at a time, faithfully running
 * real pipeline so each input behaves like a script augmented with the existing
 * globals (see plans/repl.md). The TTY / line-editor layer is separate; this
 * core is headless and is exactly what the `repl:` tests drive (a sequence of
 * input -> output pairs, to exercise the persisted global context).
 */
class ReplEngine {

public:

    ReplEngine();
    ~ReplEngine();

    /*
     * Evaluate one complete input (one or more statements / a multi-line block)
     * and return everything the user would see: any print() output, then the
     * `=> <value>` echo, or a formatted compile/runtime error. State (globals,
     * consts, funcs, structs) persists for the next call.
     */
    std::string eval_input(const std::string &src);

    /*
     * True when `src` is not yet a complete unit (an unbalanced (){}/[] or a
     * trailing continuation operator) - the loop/editor use it to decide if
     * to keep reading. Side-effect-free (no parsing, no state change).
     */
    static bool is_incomplete(const std::string &src);

private:

    struct Impl;
    std::unique_ptr<Impl> impl;
};

/*
 * Run the interactive REPL on stdin/stdout (cooked mode for now) and return the
 * process exit code. Launched by mylang.cpp when there is no FILE/-e argument.
 */
int run_repl();
