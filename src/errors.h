/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"

#include <string>
#include <string_view>
#include <vector>
#include "operators.h"

struct Loc {

    int line;
    int col;

    Loc() : line(0), col(0) { }

    template <class T, class U>
    Loc(T line, U col)
        : line(static_cast<int>(line))
        , col(static_cast<int>(col))
    { }

    operator bool() const {
        return col != 0;
    }

    Loc operator+(size_t n) const {

        if (!col)
            return Loc();

        return Loc(line, col + static_cast<int>(n));
    }

    Loc operator+(int n) const {

        if (!col)
            return Loc();

        return Loc(line, col + n);
    }
};

class Tok;
class Construct;

/*
 * One frame of a runtime backtrace, captured (as self-contained strings, since
 * the AST is torn down while the exception unwinds) when an exception passes
 * through do_func_call (see eval.cpp), innermost first. `name`/`params` are the
 * function's name and parameter names; `call_site` is where it called the
 * next, deeper one.
 */
struct BacktraceFrame {
    std::string name;
    std::vector<std::string> params;
    Loc call_site;
};

/*
 * A virtual ("inlined-at") backtrace frame. When a function is inlined, every
 * node spliced from its body points to one of these; the chain (innermost
 * callee first, outer callers via `parent`) lets the backtrace reconstruct the
 * call frames that have no physical do_func_call frame. Each element maps 1:1
 * to a BacktraceFrame. See flush_inline_frames() in backtrace.cpp and the
 * plans/function-inlining.md design. (No inliner emits these yet.)
 */
struct InlineCtx {
    std::string callee_name;
    std::vector<std::string> params;
    Loc call_site;
    const InlineCtx *parent = nullptr;
};

struct Exception {

    const char *const name;
    const char *const msg;
    Loc loc_start;
    Loc loc_end;
    std::vector<BacktraceFrame> backtrace;

    /*
     * Set once the inlined-at frames for the error's innermost inlined node
     * have been emitted. The flush is keyed off this rather than the loc-stamp
     * once-guard, because many errors arrive with a loc already set (a builtin
     * call, a not-an-lvalue assignment) and would otherwise lose their frames.
     * `do_func_call` also sets it after its call-site flush so the enclosing
     * CallExpr doesn't re-emit. See Construct::eval / do_func_call (eval.cpp).
     */
    bool inline_origin_emitted = false;

    Exception(const char *name,
              const char *msg,
              Loc start = Loc(),
              Loc end = Loc())
        : name(name)
        , msg(msg)
        , loc_start(start)
        , loc_end(end)
    { }

    virtual ~Exception() = default;
};

struct RuntimeException : public Exception {

    RuntimeException(const char *name,
                     const char *msg,
                     Loc start = Loc(),
                     Loc end = Loc())
        : Exception(name, msg, start, end)
    { }

    virtual RuntimeException *clone() const = 0;
    [[ noreturn ]] virtual void rethrow() const = 0;
};

#define DECL_SIMPLE_EX(name, msg)                  \
                                                   \
    struct name : public Exception {               \
                                                   \
        name(Loc start = Loc(), Loc end = Loc())   \
            : Exception(#name, msg, start, end)    \
        { }                                        \
    };

#define DECL_RUNTIME_EX(name, msg)                        \
                                                          \
    struct name : public RuntimeException {               \
                                                          \
        name(Loc start = Loc(), Loc end = Loc())          \
            : RuntimeException(#name, msg, start, end)    \
        { }                                               \
                                                          \
        name(const char *custom_msg,                      \
             Loc s = Loc(), Loc e = Loc())                \
            : RuntimeException(#name, custom_msg, s, e)   \
        { }                                               \
                                                          \
        name *clone() const override {                    \
            return new name(*this);                       \
        }                                                 \
                                                          \
        [[ noreturn ]] void rethrow() const override {    \
            throw *this;                                  \
        }                                                 \
    };

struct InvalidTokenEx : public Exception {

    const std::string_view val;

    InvalidTokenEx(const std::string_view &val)
        : Exception("InvalidTokenEx", "Invalid token error")
        , val(val)
    { }
};

DECL_SIMPLE_EX(InternalErrorEx, "Internal error")
DECL_SIMPLE_EX(CannotRebindConstEx, "Cannot rebind const")
DECL_SIMPLE_EX(CannotRebindBuiltinEx, "Cannot rebind builtin")
DECL_SIMPLE_EX(ExpressionIsNotConstEx, "The expression is not const")
DECL_SIMPLE_EX(AlreadyDefinedEx, "Already defined error")
DECL_SIMPLE_EX(InvalidArgumentEx, "Invalid argument error")
DECL_SIMPLE_EX(InvalidNumberOfArgsEx, "Invalid number of arguments error")
DECL_SIMPLE_EX(CannotChangeConstEx, "Cannot change constant")

/*
 * Compile-time type-inference errors (see plans/type-inference.md). They are
 * plain Exceptions (NOT RuntimeExceptions), so script `try/catch` cannot catch
 * them: a type violation fails the build, like a SyntaxError. Each takes a
 * custom message (the inferencer interns it so it outlives the throw) and the
 * offending node's Loc for the caret.
 */
struct TypeMismatchEx : public Exception {
    TypeMismatchEx(const char *m = "Type mismatch",
                   Loc start = Loc(), Loc end = Loc())
        : Exception("TypeMismatchEx", m, start, end) { }
};

struct NullabilityEx : public Exception {
    NullabilityEx(const char *m = "Nullability error",
                  Loc start = Loc(), Loc end = Loc())
        : Exception("NullabilityEx", m, start, end) { }
};

struct WrongArgCountEx : public Exception {
    WrongArgCountEx(const char *m = "Wrong number of arguments",
                    Loc start = Loc(), Loc end = Loc())
        : Exception("WrongArgCountEx", m, start, end) { }
};

/* Runtime errors */
DECL_RUNTIME_EX(DivisionByZeroEx, "Division by zero")
DECL_RUNTIME_EX(AssertionFailureEx, "Assertion failure")
DECL_RUNTIME_EX(NotLValueEx, "Not an lvalue error")
DECL_RUNTIME_EX(TypeErrorEx, "Type error")
DECL_RUNTIME_EX(InvalidValueEx, "Invalid value error")
DECL_RUNTIME_EX(NotCallableEx, "Not a callable object")
DECL_RUNTIME_EX(OutOfBoundsEx, "Out of bounds error")
DECL_RUNTIME_EX(CannotOpenFileEx, "Cannot open file error")

struct UndefinedVariableEx : public Exception {

    const std::string_view name;
    bool in_pure_func;

    UndefinedVariableEx(const std::string_view &name, Loc start = Loc(), Loc end = Loc())
        : Exception("UndefinedVariable", nullptr, start, end)
        , name(name)
        , in_pure_func(false)
    { }
};

struct SyntaxErrorEx : public Exception {

    const char *const msg;
    const Tok *const tok;
    const Op op;

    SyntaxErrorEx(Loc loc_start,
                  const char *msg,
                  const Tok *tok = nullptr,
                  Op op = Op::invalid)
        : Exception("SyntaxError", nullptr, loc_start)
        , msg(msg)
        , tok(tok)
        , op(op)
    { }
};
