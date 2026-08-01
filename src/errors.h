/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"
#include "poolalloc.h"
#include "uniqueid.h"

#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include "operators.h"

/*
 * Intern a dynamically-built error message so the `const char *` a thrown
 * Exception stores outlives the throw (the std::string that built it is
 * usually a temporary). `inline` => one shared pool across all TUs. Used by the
 * inferencer's compile-time errors and the named-argument desugaring.
 */
inline const char *intern_msg(const std::string &s)
{
    static std::deque<std::string> pool;
    pool.push_back(s);
    return pool.back().c_str();
}

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
struct FuncDescriptor;

struct BacktraceFrame {
    /*
     * The LAZY (hot-path) form: `desc` set means name/params render at
     * FORMAT time from the program-lifetime FuncDescriptor - capture
     * allocates NOTHING, so a CAUGHT exception (whose backtrace never
     * renders) pays no string churn (2026-07-18 profile #3: 13% of the
     * exception benches was this malloc/free). desc == null is the eager
     * string form, kept for the INLINE (virtual) frames - their source
     * strings are AST-owned and must be copied at capture.
     */
    const FuncDescriptor *desc = nullptr;
    std::string name;
    std::vector<std::string> params;
    Loc call_site;

    BacktraceFrame() = default;
    BacktraceFrame(const FuncDescriptor *d, Loc cs)
        : desc(d), call_site(cs) {}
    BacktraceFrame(std::string n, std::vector<std::string> p, Loc cs)
        : name(std::move(n)), params(std::move(p)), call_site(cs) {}
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

    /*
     * The INLINED-AT chain to flush, baked by the code that conveyed this
     * exception; -1 = "look it up by pc" (the ordinary path).
     *
     * WHY (#56): `vm_flush_inline` normally resolves the chain from the
     * raising op's pc via `Chunk::inline_frame_at`. That breaks once a native
     * run's interpreted ORIGINALS are deleted: every pc in the run collapses
     * onto the head EnterNative, so a run holding ops from two DIFFERENT
     * inlined bodies could no longer say which chain a raise belonged to -
     * which is exactly what kept such runs from being deleted. A conveying
     * fragment therefore stamps its op's chain index HERE, the same way it
     * already stamps the op's caret into loc_start/loc_end (emit_exc_stamp,
     * jit.cpp), and the flush prefers it. First conveyor wins, like the caret.
     *
     * `int32_t` (not the pool's index type) because the fragment writes it
     * with one `mov dword [rax+off], imm32`.
     */
    int32_t jit_inline_frame = -1;

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

    /*
     * 2026-07-18 profile #4: pool the exception OBJECTS. Inherited by every
     * subclass (the DECL_RUNTIME_EX classes, ExceptionObjectTempl), so the
     * VM's signal-path make_unique/clone() allocations come from the
     * program-lifetime pool instead of malloc (13% of the exception benches
     * was this churn). Sized delete through the virtual dtor passes the
     * most-derived size, so the pool's size classes stay exact; an
     * oversized class falls back to plain new automatically. (A C++
     * `throw X` value still uses the EH runtime's own allocation - only
     * explicit new/clone routes through this.)
     */
    ML_POOL_NEW_DELETE

    RuntimeException(const char *name,
                     const char *msg,
                     Loc start = Loc(),
                     Loc end = Loc())
        : Exception(name, msg, start, end)
    { }

    virtual RuntimeException *clone() const = 0;
    [[ noreturn ]] virtual void rethrow() const = 0;

    /* The catch-matching NAME (a user struct exception's type name, else
     * the built-in `name`) and the ExceptionObject discriminator - cheap
     * VIRTUALS, not dynamic_cast: the catch matcher runs these once per
     * caught exception, and the RTTI cast measured ~218 Ir per call on the
     * multiple-inheritance ExceptionObjectTempl graph (70_exc profile,
     * task #74) vs a ~3 Ir virtual dispatch. `is_exception_object()`
     * true guarantees the object IS an ExceptionObjectTempl (only that
     * class overrides it), so callers may static_cast. */
    virtual std::string_view match_name() const { return name; }
    virtual bool is_exception_object() const { return false; }

    /* The catch-matching name as an INTERNED pointer (#74 inc 3): the
     * matcher compares pointers instead of strings (the per-match
     * string_view(name) paid a strlen + memcmp, ~13% of the catch
     * bench). The DECL_RUNTIME_EX macro overrides this with a per-class
     * lazy static; ExceptionObjectTempl carries the thrown struct's
     * already-interned type name. nullptr = "match by string" - the
     * safe fallback for any subclass without the override. */
    virtual const UniqueId *match_uid() const { return nullptr; }
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
        const UniqueId *match_uid() const override {      \
            /* hand-rolled lazy init: a plain zero-init   \
             * pointer, no __cxa_guard acquire (the       \
             * interpreter is single-threaded; the guard  \
             * measured 18 Ir per catch on 70_exc) */     \
            static const UniqueId *u;                     \
            if (!u)                                       \
                u = UniqueId::get(#name);                 \
            return u;                                     \
        }                                                 \
                                                          \
        [[ noreturn ]] void rethrow() const override {    \
            throw *this;                                  \
        }                                                 \
    };

struct InvalidTokenEx : public Exception {

    const std::string_view val;
    /* True when the lexer reached end-of-input still inside a string or block
     * comment (vs. a genuinely malformed token like `2_`). The REPL uses it to
     * keep the input open for more lines instead of reporting an error. */
    const bool unterminated;

    InvalidTokenEx(const std::string_view &val,
                   Loc start = Loc(), Loc end = Loc(),
                   bool unterminated = false)
        : Exception("InvalidTokenEx", "Invalid token error", start, end)
        , val(val)
        , unterminated(unterminated)
    { }
};

DECL_SIMPLE_EX(InternalErrorEx, "Internal error")
DECL_SIMPLE_EX(CannotRebindConstEx, "Cannot rebind const")
DECL_SIMPLE_EX(CannotRebindBuiltinEx, "Cannot rebind builtin")
DECL_SIMPLE_EX(ExpressionIsNotConstEx, "The expression is not const")
DECL_SIMPLE_EX(AlreadyDefinedEx, "Already defined error")
/* All three are RUNTIME (script-catchable) exceptions - builtins throw them at
 * runtime, and the JIT's exception conveyance (g_vm_jit_exc) is
 * RuntimeException-shaped, so a native builtin call (CallBuiltinV /
 * CallBuiltinLV / AppendV) that throws one must carry it without a
 * std::terminate. InvalidArgumentEx is inherently DYNAMIC (a bad VALUE into
 * max/min/round). CannotChangeConstEx (mutating a const/readonly container via a
 * LVALUE builtin - append/push/insert/erase/sort) is inherently DYNAMIC too (the
 * const-ness is a runtime property of the value, e.g. an aliased const passed as
 * a param), so both belong here permanently. InvalidNumberOfArgsEx is here FOR
 * NOW; the deeper fix is FIXED builtin arities (e.g. write(str) +
 * fwrite(file_or_handle, str)) so arity becomes a COMPILE-time check and the
 * runtime throw disappears - see plans/callbuiltinv-nativization.md #1. */
DECL_RUNTIME_EX(InvalidArgumentEx, "Invalid argument error")
DECL_RUNTIME_EX(InvalidNumberOfArgsEx, "Invalid number of arguments error")
DECL_RUNTIME_EX(CannotChangeConstEx, "Cannot change constant")

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

/*
 * A plain `var`/`const` whose type the inferencer could only conclude is `dyn`.
 * Under the mandatory-dyn rule a plain declaration must have a concrete static
 * type; a genuinely dynamic one must be declared `dyn` (see
 * plans/type-driven-specialization.md). Compile-time, uncatchable.
 */
struct DynRequiredEx : public Exception {
    DynRequiredEx(const char *m = "Declaration requires an explicit 'dyn'",
                  Loc start = Loc(), Loc end = Loc())
        : Exception("DynRequiredEx", m, start, end) { }
};

/*
 * A parameter that can receive `none` must be declared `opt` (the param
 * analogue of the mandatory-`dyn` rule; see the inferencer's
 * enforce_nonnull_params). Compile-time, uncatchable.
 */
struct OptRequiredEx : public Exception {
    OptRequiredEx(const char *m = "Parameter requires an explicit 'opt'",
                  Loc start = Loc(), Loc end = Loc())
        : Exception("OptRequiredEx", m, start, end) { }
};

/* A `foreach (x in ...)` (no `var`) whose loop var would shadow a variable
 * visible outside the loop - omitting `var` may not silently shadow. */
struct ShadowingEx : public Exception {
    ShadowingEx(const char *m = "Loop variable shadows an outer variable",
                Loc start = Loc(), Loc end = Loc())
        : Exception("ShadowingEx", m, start, end) { }
};

/* Runtime errors */
DECL_RUNTIME_EX(DivisionByZeroEx, "Division by zero")
DECL_RUNTIME_EX(StackOverflowEx, "Maximum call depth exceeded")
DECL_RUNTIME_EX(AssertionFailureEx, "Assertion failure")
DECL_RUNTIME_EX(NotLValueEx, "Not an lvalue error")
DECL_RUNTIME_EX(TypeErrorEx, "Type error")
DECL_RUNTIME_EX(InvalidValueEx, "Invalid value error")
DECL_RUNTIME_EX(NotCallableEx, "Not a callable object")
DECL_RUNTIME_EX(OutOfBoundsEx, "Out of bounds error")
DECL_RUNTIME_EX(KeyNotFoundEx, "Key not found in dict")
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
