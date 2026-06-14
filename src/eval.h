/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "uniqueid.h"

#include <map>
#include <string_view>

class Identifier;

/*
 * Non-local control flow (return/break/continue) is signaled through this
 * struct instead of C++ exceptions. Throwing is ~1.6us on this toolchain
 * (heap-allocated exception object + DWARF table-driven stack unwinding,
 * neither reducible by build flags), which dominated recursion-heavy code
 * because `return` was an exception. Statements set the FlowState, and
 * Block / loops / do_func_call check it and unwind via ordinary C++ returns.
 * Genuinely exceptional paths (runtime errors, user `throw`, `rethrow`) still
 * use C++ exceptions, where the zero-cost-when-not-thrown model is the right fit.
 */
struct FlowState {

    enum Type : unsigned char {
        none,   /* normal execution                                  */
        brk,    /* a `break` in flight, up to the nearest loop       */
        cont,   /* a `continue` in flight, up to the nearest loop    */
        ret,    /* a `return` in flight, up to the function boundary */
    };

    Type type = none;
    EvalValue value;    /* the return value, meaningful when type == ret */
};

class EvalContext {

    typedef std::map<const UniqueId *, LValue> SymbolsType;
    SymbolsType symbols;
    FlowState flow_state;   /* used only when this context is a flow root */

public:

    EvalContext *const parent;
    const bool const_ctx;
    const bool func_ctx;

    /*
     * Points at the FlowState shared by every context within the current
     * function invocation. Function-boundary contexts (func_ctx) and the root
     * own their flow_state; nested blocks/loops inherit the parent's pointer.
     * Each call gets a fresh one, so recursion never shares flow state.
     */
    FlowState *const flow;

    EvalContext(const EvalContext &rhs) = delete;
    EvalContext(EvalContext &&rhs) = delete;

    EvalContext(EvalContext *parent = nullptr,
                bool const_ctx = false,
                bool func_ctx = false);

    LValue *lookup(const Identifier *id);
    bool erase(const Identifier *id);

    void emplace(const Identifier *id, const EvalValue &val, bool is_const);
    void emplace(const Identifier *id, EvalValue &&val, bool is_const);
    void emplace(const std::string_view &id, EvalValue &&val, bool is_const);

    bool empty() const { return symbols.empty(); }
    void copy_symbols_from(const EvalContext &ctx) { symbols = ctx.symbols; }

    static SymbolsType builtins;
    static const SymbolsType const_builtins;
};


inline EvalContext *
get_root_ctx(EvalContext *ctx)
{
    while (ctx->parent)
        ctx = ctx->parent;

    return ctx;
}

class FuncObject {

public:

    const FuncDeclStmt *const func;
    EvalContext capture_ctx;

    FuncObject(const FuncDeclStmt *func, EvalContext *ctx);
    FuncObject(const FuncObject &rhs);
};
